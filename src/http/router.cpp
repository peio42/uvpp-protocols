#include <uvpp/protocols/http/router.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "detail/route_matching.hpp"

namespace uvp::http {

namespace detail {

namespace {

bool next_route_segment(std::string_view& input, std::string_view& segment) noexcept {
  if (input.empty()) {
    return false;
  }

  if (input.front() == '/') {
    input.remove_prefix(1);
  }

  if (input.empty()) {
    return false;
  }

  const auto offset = input.find('/');
  if (offset == std::string_view::npos) {
    segment = input;
    input = {};
    return true;
  }

  segment = input.substr(0, offset);
  input.remove_prefix(offset + 1);
  return true;
}

std::string capture_wildcard(std::string_view rest) {
  if (!rest.empty() && rest.front() == '/') {
    rest.remove_prefix(1);
  }
  return std::string(rest);
}

} // namespace

bool route_pattern_matches(std::string_view pattern, std::string_view path, route_params& params) {
  if (pattern == path) {
    return true;
  }

  std::string_view pattern_rest = pattern;
  std::string_view path_rest = path;

  std::string_view pattern_segment;
  while (next_route_segment(pattern_rest, pattern_segment)) {
    if (!pattern_segment.empty() && pattern_segment.front() == '*') {
      const auto name = pattern_segment.substr(1);
      if (!name.empty()) {
        if (!path_rest.empty() && path_rest.front() == '/') {
          path_rest.remove_prefix(1);
        }
        params.set(name, path_rest);
      }
      return pattern_rest.empty();
    }

    std::string_view path_segment;
    if (!next_route_segment(path_rest, path_segment)) {
      return false;
    }

    if (!pattern_segment.empty() && pattern_segment.front() == ':') {
      const auto name = pattern_segment.substr(1);
      if (!name.empty()) {
        params.set(name, path_segment);
      }
      continue;
    }

    if (pattern_segment != path_segment) {
      return false;
    }
  }

  std::string_view trailing;
  return !next_route_segment(path_rest, trailing);
}

} // namespace detail

router& router::add_route(
  method method_value,
  std::string_view pattern,
  detail::body_mode body,
  std::size_t max_body_bytes,
  handler_type handler) {
  auto method_index = static_cast<std::size_t>(method_value);
  if (method_index >= method_count_) {
    throw std::invalid_argument("HTTP route method is out of range");
  }

  std::size_t node_index = 0;
  std::string_view rest = pattern;
  std::string_view segment;
  while (detail::next_route_segment(rest, segment)) {
    if (!segment.empty() && segment.front() == '*') {
      const auto name = segment.substr(1);
      if (name.empty()) {
        throw std::invalid_argument("HTTP route wildcard must be named");
      }
      std::string_view trailing;
      if (detail::next_route_segment(rest, trailing)) {
        throw std::invalid_argument("HTTP route wildcard must be the final segment");
      }

      if (nodes_[node_index].wildcard_child && nodes_[node_index].wildcard_child->name != name) {
        throw std::invalid_argument("HTTP route wildcard conflicts with an existing wildcard route");
      }
      if (!nodes_[node_index].wildcard_child) {
        const auto child_index = nodes_.size();
        nodes_.push_back(trie_node{});
        nodes_[node_index].wildcard_child = named_child{std::string(name), child_index};
      }
      node_index = nodes_[node_index].wildcard_child->node;
      break;
    }

    if (!segment.empty() && segment.front() == ':') {
      const auto name = segment.substr(1);
      if (name.empty()) {
        throw std::invalid_argument("HTTP route parameter must be named");
      }

      if (nodes_[node_index].parameter_child && nodes_[node_index].parameter_child->name != name) {
        throw std::invalid_argument("HTTP route parameter conflicts with an existing parameter route");
      }
      if (!nodes_[node_index].parameter_child) {
        const auto child_index = nodes_.size();
        nodes_.push_back(trie_node{});
        nodes_[node_index].parameter_child = named_child{std::string(name), child_index};
      }
      node_index = nodes_[node_index].parameter_child->node;
      continue;
    }

    const auto child = nodes_[node_index].static_children.find(std::string(segment));
    if (child != nodes_[node_index].static_children.end()) {
      node_index = child->second;
    } else {
      const auto child_index = nodes_.size();
      nodes_.push_back(trie_node{});
      nodes_[node_index].static_children.emplace(std::string(segment), child_index);
      node_index = child_index;
    }
  }

  auto& target = nodes_[node_index].targets[method_index];
  if (target) {
    throw std::invalid_argument("HTTP route already registered for this method and pattern");
  }

  target = route_target{body, max_body_bytes, std::move(handler)};
  ++route_count_;
  return *this;
}

const router::route_target* router::match_node(
  std::size_t node_index,
  std::size_t method_index,
  std::string_view path,
  route_params& params) const {
  const auto& node = nodes_[node_index];

  std::string_view rest = path;
  std::string_view segment;
  if (!detail::next_route_segment(rest, segment)) {
    if (const auto& target = node.targets[method_index]) {
      return &*target;
    }
    if (node.wildcard_child) {
      auto wildcard_params = params;
      if (!node.wildcard_child->name.empty()) {
        wildcard_params.set(node.wildcard_child->name, {});
      }
      if (const auto& target = nodes_[node.wildcard_child->node].targets[method_index]) {
        params = std::move(wildcard_params);
        return &*target;
      }
    }
    return nullptr;
  }

  if (auto child = node.static_children.find(std::string(segment)); child != node.static_children.end()) {
    auto child_params = params;
    if (auto target = match_node(child->second, method_index, rest, child_params)) {
      params = std::move(child_params);
      return target;
    }
  }

  if (node.parameter_child) {
    auto child_params = params;
    child_params.set(node.parameter_child->name, segment);
    if (auto target = match_node(node.parameter_child->node, method_index, rest, child_params)) {
      params = std::move(child_params);
      return target;
    }
  }

  if (node.wildcard_child) {
    auto wildcard_params = params;
    if (!node.wildcard_child->name.empty()) {
      wildcard_params.set(node.wildcard_child->name, detail::capture_wildcard(path));
    }
    if (const auto& target = nodes_[node.wildcard_child->node].targets[method_index]) {
      params = std::move(wildcard_params);
      return &*target;
    }
  }

  return nullptr;
}

router::match_result router::match(method method_value, std::string_view path) const {
  const auto method_index = static_cast<std::size_t>(method_value);
  if (method_index >= method_count_) {
    return {};
  }

  route_params params;
  const auto* target = match_node(0, method_index, path, params);
  if (!target) {
    return {};
  }
  return match_result{
    &target->handler,
    target->body,
    target->max_body_bytes,
    std::move(params),
  };
}

const router::handler_type* router::find(method method_value, std::string_view path) const {
  return match(method_value, path).handler;
}

} // namespace uvp::http
