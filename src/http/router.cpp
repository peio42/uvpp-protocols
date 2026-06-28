#include <uvpp/protocols/http/router.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "detail/route_matching.hpp"

namespace uvp::http {

namespace detail {

namespace {

constexpr std::array routed_methods{
  method::get,
  method::head,
  method::post,
  method::put,
  method::delete_,
  method::connect,
  method::options,
  method::trace,
  method::patch,
};

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

std::string normalize_group_prefix(std::string_view prefix) {
  if (prefix.empty() || prefix == "/") {
    return {};
  }

  std::string normalized;
  if (prefix.front() != '/') {
    normalized.push_back('/');
  }
  normalized.append(prefix);
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

std::string join_route_pattern(std::string_view prefix, std::string_view pattern) {
  const auto normalized_prefix = normalize_group_prefix(prefix);
  if (normalized_prefix.empty()) {
    if (pattern.empty()) {
      return "/";
    }
    if (pattern.front() == '/') {
      return std::string(pattern);
    }
    return "/" + std::string(pattern);
  }

  if (pattern.empty() || pattern == "/") {
    return normalized_prefix;
  }

  std::string joined = normalized_prefix;
  if (pattern.front() != '/') {
    joined.push_back('/');
  }
  joined.append(pattern);
  return joined;
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

router& router::add_on_request_hook(std::string_view prefix, hook_type hook) {
  nodes_[ensure_prefix_node(prefix)].on_request_hooks.push_back(std::move(hook));
  return *this;
}

router& router::add_pre_handler_hook(std::string_view prefix, hook_type hook) {
  nodes_[ensure_prefix_node(prefix)].pre_handler_hooks.push_back(std::move(hook));
  return *this;
}

router& router::add_on_response_hook(std::string_view prefix, response_hook_type hook) {
  nodes_[ensure_prefix_node(prefix)].on_response_hooks.push_back(std::move(hook));
  return *this;
}

std::size_t router::ensure_prefix_node(std::string_view prefix) {
  std::size_t node_index = 0;
  std::string_view rest = prefix;
  std::string_view segment;
  while (detail::next_route_segment(rest, segment)) {
    if (!segment.empty() && segment.front() == '*') {
      throw std::invalid_argument("HTTP route group wildcard prefixes are not supported");
    }

    if (!segment.empty() && segment.front() == ':') {
      const auto name = segment.substr(1);
      if (name.empty()) {
        throw std::invalid_argument("HTTP route group parameter must be named");
      }

      if (nodes_[node_index].parameter_child && nodes_[node_index].parameter_child->name != name) {
        throw std::invalid_argument("HTTP route group parameter conflicts with an existing parameter route");
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

  return node_index;
}

const router::route_target* router::match_node(
  std::size_t node_index,
  std::size_t method_index,
  std::string_view path,
  route_params& params,
  std::vector<const hook_type*>& on_request_hooks,
  std::vector<const hook_type*>& pre_handler_hooks,
  std::vector<const response_hook_type*>& on_response_hooks) const {
  const auto& node = nodes_[node_index];
  for (const auto& hook : node.on_request_hooks) {
    on_request_hooks.push_back(&hook);
  }
  for (const auto& hook : node.pre_handler_hooks) {
    pre_handler_hooks.push_back(&hook);
  }
  for (const auto& hook : node.on_response_hooks) {
    on_response_hooks.push_back(&hook);
  }

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
      auto wildcard_on_request_hooks = on_request_hooks;
      auto wildcard_pre_handler_hooks = pre_handler_hooks;
      auto wildcard_on_response_hooks = on_response_hooks;
      const auto& wildcard_node = nodes_[node.wildcard_child->node];
      for (const auto& hook : wildcard_node.on_request_hooks) {
        wildcard_on_request_hooks.push_back(&hook);
      }
      for (const auto& hook : wildcard_node.pre_handler_hooks) {
        wildcard_pre_handler_hooks.push_back(&hook);
      }
      for (const auto& hook : wildcard_node.on_response_hooks) {
        wildcard_on_response_hooks.push_back(&hook);
      }
      if (const auto& target = nodes_[node.wildcard_child->node].targets[method_index]) {
        params = std::move(wildcard_params);
        on_request_hooks = std::move(wildcard_on_request_hooks);
        pre_handler_hooks = std::move(wildcard_pre_handler_hooks);
        on_response_hooks = std::move(wildcard_on_response_hooks);
        return &*target;
      }
    }
    return nullptr;
  }

  if (auto child = node.static_children.find(std::string(segment)); child != node.static_children.end()) {
    auto child_params = params;
    auto child_on_request_hooks = on_request_hooks;
    auto child_pre_handler_hooks = pre_handler_hooks;
    auto child_on_response_hooks = on_response_hooks;
    if (auto target = match_node(
          child->second,
          method_index,
          rest,
          child_params,
          child_on_request_hooks,
          child_pre_handler_hooks,
          child_on_response_hooks)) {
      params = std::move(child_params);
      on_request_hooks = std::move(child_on_request_hooks);
      pre_handler_hooks = std::move(child_pre_handler_hooks);
      on_response_hooks = std::move(child_on_response_hooks);
      return target;
    }
  }

  if (node.parameter_child) {
    auto child_params = params;
    child_params.set(node.parameter_child->name, segment);
    auto child_on_request_hooks = on_request_hooks;
    auto child_pre_handler_hooks = pre_handler_hooks;
    auto child_on_response_hooks = on_response_hooks;
    if (auto target = match_node(
          node.parameter_child->node,
          method_index,
          rest,
          child_params,
          child_on_request_hooks,
          child_pre_handler_hooks,
          child_on_response_hooks)) {
      params = std::move(child_params);
      on_request_hooks = std::move(child_on_request_hooks);
      pre_handler_hooks = std::move(child_pre_handler_hooks);
      on_response_hooks = std::move(child_on_response_hooks);
      return target;
    }
  }

  if (node.wildcard_child) {
    auto wildcard_params = params;
    if (!node.wildcard_child->name.empty()) {
      wildcard_params.set(node.wildcard_child->name, detail::capture_wildcard(path));
    }
    auto wildcard_on_request_hooks = on_request_hooks;
    auto wildcard_pre_handler_hooks = pre_handler_hooks;
    auto wildcard_on_response_hooks = on_response_hooks;
    const auto& wildcard_node = nodes_[node.wildcard_child->node];
    for (const auto& hook : wildcard_node.on_request_hooks) {
      wildcard_on_request_hooks.push_back(&hook);
    }
    for (const auto& hook : wildcard_node.pre_handler_hooks) {
      wildcard_pre_handler_hooks.push_back(&hook);
    }
    for (const auto& hook : wildcard_node.on_response_hooks) {
      wildcard_on_response_hooks.push_back(&hook);
    }
    if (const auto& target = nodes_[node.wildcard_child->node].targets[method_index]) {
      params = std::move(wildcard_params);
      on_request_hooks = std::move(wildcard_on_request_hooks);
      pre_handler_hooks = std::move(wildcard_pre_handler_hooks);
      on_response_hooks = std::move(wildcard_on_response_hooks);
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
  std::vector<const hook_type*> on_request_hooks;
  std::vector<const hook_type*> pre_handler_hooks;
  std::vector<const response_hook_type*> on_response_hooks;
  const auto* target = match_node(
    0,
    method_index,
    path,
    params,
    on_request_hooks,
    pre_handler_hooks,
    on_response_hooks);
  if (!target) {
    return {};
  }
  return match_result{
    &target->handler,
    target->body,
    target->max_body_bytes,
    std::move(params),
    std::move(on_request_hooks),
    std::move(pre_handler_hooks),
    std::move(on_response_hooks),
  };
}

const router::handler_type* router::find(method method_value, std::string_view path) const {
  return match(method_value, path).handler;
}

std::vector<method> router::allowed_methods(std::string_view path) const {
  std::vector<method> methods;
  for (auto method_value : detail::routed_methods) {
    auto params = route_params{};
    std::vector<const hook_type*> on_request_hooks;
    std::vector<const hook_type*> pre_handler_hooks;
    std::vector<const response_hook_type*> on_response_hooks;
    const auto method_index = static_cast<std::size_t>(method_value);
    if (method_index < method_count_ &&
        match_node(0, method_index, path, params, on_request_hooks, pre_handler_hooks, on_response_hooks)) {
      methods.push_back(method_value);
    }
  }
  return methods;
}

route_group router::group(std::string_view prefix) {
  return route_group{*this, detail::normalize_group_prefix(prefix)};
}

route_resource router::resource(std::string_view pattern) {
  return route_resource{*this, detail::join_route_pattern({}, pattern)};
}

route_group route_group::group(std::string_view prefix) const {
  return route_group{*router_, route_pattern(prefix)};
}

route_resource route_group::resource(std::string_view pattern) const {
  return route_resource{*router_, route_pattern(pattern)};
}

std::string route_group::route_pattern(std::string_view pattern) const {
  return detail::join_route_pattern(prefix_, pattern);
}

} // namespace uvp::http
