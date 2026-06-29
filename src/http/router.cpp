#include <uvpp/protocols/http/router.hpp>

#include <iterator>
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

std::string_view route_segment_name(std::string_view segment) noexcept {
  return segment.substr(1);
}

std::span<const std::string> selected_segments(
  const detail::route_path& path,
  route_path_matching matching) noexcept {
  return path.segments(matching);
}

} // namespace

bool route_pattern_matches(
  std::string_view pattern,
  const route_path& path,
  route_path_matching matching,
  route_params& params) {
  const auto parsed_pattern = parse_route_path(pattern);
  if (!parsed_pattern.valid || !path.valid) {
    return false;
  }

  const auto pattern_segments = selected_segments(parsed_pattern, matching);
  const auto path_segments = selected_segments(path, matching);
  const auto& raw_pattern_segments = parsed_pattern.raw_segments;

  std::size_t path_index = 0;
  for (std::size_t pattern_index = 0; pattern_index < raw_pattern_segments.size(); ++pattern_index) {
    const auto& raw_pattern_segment = raw_pattern_segments[pattern_index];
    if (!raw_pattern_segment.empty() && raw_pattern_segment.front() == '*') {
      const auto name = route_segment_name(raw_pattern_segment);
      if (!name.empty()) {
        params.set(name, join_route_segments(path_segments, path_index));
      }
      return pattern_index + 1 == raw_pattern_segments.size();
    }

    if (path_index >= path_segments.size()) {
      return false;
    }

    if (!raw_pattern_segment.empty() && raw_pattern_segment.front() == ':') {
      const auto name = route_segment_name(raw_pattern_segment);
      if (!name.empty()) {
        params.set(name, path_segments[path_index]);
      }
      ++path_index;
      continue;
    }

    if (pattern_segments[pattern_index] != path_segments[path_index]) {
      return false;
    }
    ++path_index;
  }

  return path_index == path_segments.size();
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

  auto normalized_pattern = detail::join_route_pattern({}, pattern);
  auto parsed_pattern = detail::parse_route_path(normalized_pattern);
  if (!parsed_pattern.valid) {
    throw std::invalid_argument("HTTP route pattern contains invalid percent encoding");
  }

  const auto route_segments = detail::selected_segments(parsed_pattern, matching_);
  std::size_t node_index = 0;
  for (std::size_t segment_index = 0; segment_index < parsed_pattern.raw_segments.size(); ++segment_index) {
    const auto& segment = parsed_pattern.raw_segments[segment_index];
    if (!segment.empty() && segment.front() == '*') {
      const auto name = detail::route_segment_name(segment);
      if (name.empty()) {
        throw std::invalid_argument("HTTP route wildcard must be named");
      }
      if (segment_index + 1 < parsed_pattern.raw_segments.size()) {
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
      const auto name = detail::route_segment_name(segment);
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

    const auto& route_segment = route_segments[segment_index];
    const auto child = nodes_[node_index].static_children.find(route_segment);
    if (child != nodes_[node_index].static_children.end()) {
      node_index = child->second;
    } else {
      const auto child_index = nodes_.size();
      nodes_.push_back(trie_node{});
      nodes_[node_index].static_children.emplace(route_segment, child_index);
      node_index = child_index;
    }
  }

  auto& target = nodes_[node_index].targets[method_index];
  if (target) {
    throw std::invalid_argument("HTTP route already registered for this method and pattern");
  }

  target = route_target{body, max_body_bytes, std::move(handler), std::move(normalized_pattern)};
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

router& router::add_not_found_handler(std::string_view prefix, handler_type handler) {
  nodes_[ensure_prefix_node(prefix)].not_found_handler = std::move(handler);
  return *this;
}

router& router::add_exception_handler(std::string_view prefix, exception_handler_type handler) {
  nodes_[ensure_prefix_node(prefix)].exception_handler = std::move(handler);
  return *this;
}

std::size_t router::ensure_prefix_node(std::string_view prefix) {
  const auto parsed_prefix = detail::parse_route_path(prefix);
  if (!parsed_prefix.valid) {
    throw std::invalid_argument("HTTP route group prefix contains invalid percent encoding");
  }

  const auto prefix_segments = detail::selected_segments(parsed_prefix, matching_);
  std::size_t node_index = 0;
  for (std::size_t segment_index = 0; segment_index < parsed_prefix.raw_segments.size(); ++segment_index) {
    const auto& segment = parsed_prefix.raw_segments[segment_index];
    if (!segment.empty() && segment.front() == '*') {
      throw std::invalid_argument("HTTP route group wildcard prefixes are not supported");
    }

    if (!segment.empty() && segment.front() == ':') {
      const auto name = detail::route_segment_name(segment);
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

    const auto& prefix_segment = prefix_segments[segment_index];
    const auto child = nodes_[node_index].static_children.find(prefix_segment);
    if (child != nodes_[node_index].static_children.end()) {
      node_index = child->second;
    } else {
      const auto child_index = nodes_.size();
      nodes_.push_back(trie_node{});
      nodes_[node_index].static_children.emplace(prefix_segment, child_index);
      node_index = child_index;
    }
  }

  return node_index;
}

router& router::mount(std::string_view prefix, router&& mounted) {
  if (&mounted == this) {
    throw std::invalid_argument("HTTP router cannot mount itself");
  }
  if (mounted.matching_ != matching_) {
    throw std::invalid_argument("HTTP mounted router path matching mode conflicts with destination router");
  }

  const auto destination_index = ensure_prefix_node(prefix);
  validate_mount_node(mounted, destination_index, 0);
  mounted.prefix_route_patterns(detail::normalize_group_prefix(prefix));
  merge_mount_node(mounted, destination_index, 0);
  route_count_ += mounted.route_count_;

  mounted.nodes_.clear();
  mounted.nodes_.push_back(trie_node{});
  mounted.route_count_ = 0;
  return *this;
}

void router::prefix_route_patterns(std::string_view prefix) {
  if (prefix.empty()) {
    return;
  }

  for (auto& node : nodes_) {
    for (auto& target : node.targets) {
      if (target) {
        target->pattern = detail::join_route_pattern(prefix, target->pattern);
      }
    }
  }
}

void router::validate_mount_node(
  const router& mounted,
  std::size_t destination_index,
  std::size_t source_index) const {
  const auto& destination = nodes_[destination_index];
  const auto& source = mounted.nodes_[source_index];

  for (std::size_t method_index = 0; method_index < method_count_; ++method_index) {
    if (destination.targets[method_index] && source.targets[method_index]) {
      throw std::invalid_argument("HTTP mounted router conflicts with an existing route");
    }
  }
  if (destination.not_found_handler && source.not_found_handler) {
    throw std::invalid_argument("HTTP mounted router not_found fallback conflicts with an existing fallback");
  }
  if (destination.exception_handler && source.exception_handler) {
    throw std::invalid_argument("HTTP mounted router exception fallback conflicts with an existing fallback");
  }

  for (const auto& [segment, source_child_index] : source.static_children) {
    if (const auto destination_child = destination.static_children.find(segment);
        destination_child != destination.static_children.end()) {
      validate_mount_node(mounted, destination_child->second, source_child_index);
    }
  }

  if (source.parameter_child) {
    if (destination.parameter_child) {
      if (destination.parameter_child->name != source.parameter_child->name) {
        throw std::invalid_argument("HTTP mounted router parameter conflicts with an existing parameter route");
      }
      validate_mount_node(mounted, destination.parameter_child->node, source.parameter_child->node);
    }
  }

  if (source.wildcard_child) {
    if (destination.wildcard_child) {
      if (destination.wildcard_child->name != source.wildcard_child->name) {
        throw std::invalid_argument("HTTP mounted router wildcard conflicts with an existing wildcard route");
      }
      validate_mount_node(mounted, destination.wildcard_child->node, source.wildcard_child->node);
    }
  }
}

void router::merge_mount_node(router& mounted, std::size_t destination_index, std::size_t source_index) {
  auto& destination = nodes_[destination_index];
  auto& source = mounted.nodes_[source_index];

  destination.on_request_hooks.insert(
    destination.on_request_hooks.end(),
    std::make_move_iterator(source.on_request_hooks.begin()),
    std::make_move_iterator(source.on_request_hooks.end()));
  destination.pre_handler_hooks.insert(
    destination.pre_handler_hooks.end(),
    std::make_move_iterator(source.pre_handler_hooks.begin()),
    std::make_move_iterator(source.pre_handler_hooks.end()));
  destination.on_response_hooks.insert(
    destination.on_response_hooks.end(),
    std::make_move_iterator(source.on_response_hooks.begin()),
    std::make_move_iterator(source.on_response_hooks.end()));

  for (std::size_t method_index = 0; method_index < method_count_; ++method_index) {
    if (source.targets[method_index]) {
      destination.targets[method_index] = std::move(source.targets[method_index]);
    }
  }
  if (source.not_found_handler) {
    destination.not_found_handler = std::move(source.not_found_handler);
  }
  if (source.exception_handler) {
    destination.exception_handler = std::move(source.exception_handler);
  }

  for (const auto& [segment, source_child_index] : source.static_children) {
    if (const auto destination_child = nodes_[destination_index].static_children.find(segment);
        destination_child != nodes_[destination_index].static_children.end()) {
      merge_mount_node(mounted, destination_child->second, source_child_index);
    } else {
      const auto destination_child_index = move_mount_subtree(mounted, source_child_index);
      nodes_[destination_index].static_children.emplace(segment, destination_child_index);
    }
  }

  if (source.parameter_child) {
    if (nodes_[destination_index].parameter_child) {
      merge_mount_node(mounted, nodes_[destination_index].parameter_child->node, source.parameter_child->node);
    } else {
      const auto destination_child_index = move_mount_subtree(mounted, source.parameter_child->node);
      nodes_[destination_index].parameter_child = named_child{source.parameter_child->name, destination_child_index};
    }
  }

  if (source.wildcard_child) {
    if (nodes_[destination_index].wildcard_child) {
      merge_mount_node(mounted, nodes_[destination_index].wildcard_child->node, source.wildcard_child->node);
    } else {
      const auto destination_child_index = move_mount_subtree(mounted, source.wildcard_child->node);
      nodes_[destination_index].wildcard_child = named_child{source.wildcard_child->name, destination_child_index};
    }
  }
}

std::size_t router::move_mount_subtree(router& mounted, std::size_t source_index) {
  const auto destination_index = nodes_.size();
  nodes_.push_back(trie_node{});
  merge_mount_node(mounted, destination_index, source_index);
  return destination_index;
}

const router::route_target* router::match_node(
  std::size_t node_index,
  std::size_t method_index,
  std::span<const std::string> segments,
  std::size_t segment_index,
  route_params& params,
  std::vector<const hook_type*>& on_request_hooks,
  std::vector<const hook_type*>& pre_handler_hooks,
  std::vector<const response_hook_type*>& on_response_hooks,
  const exception_handler_type*& exception_handler) const {
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
  if (node.exception_handler) {
    exception_handler = &*node.exception_handler;
  }

  if (segment_index >= segments.size()) {
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
      auto* wildcard_exception_handler = exception_handler;
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
      if (wildcard_node.exception_handler) {
        wildcard_exception_handler = &*wildcard_node.exception_handler;
      }
      if (const auto& target = nodes_[node.wildcard_child->node].targets[method_index]) {
        params = std::move(wildcard_params);
        on_request_hooks = std::move(wildcard_on_request_hooks);
        pre_handler_hooks = std::move(wildcard_pre_handler_hooks);
        on_response_hooks = std::move(wildcard_on_response_hooks);
        exception_handler = wildcard_exception_handler;
        return &*target;
      }
    }
    return nullptr;
  }

  const auto& segment = segments[segment_index];
  const auto next_segment_index = segment_index + 1;

  if (auto child = node.static_children.find(segment); child != node.static_children.end()) {
    auto child_params = params;
    auto child_on_request_hooks = on_request_hooks;
    auto child_pre_handler_hooks = pre_handler_hooks;
    auto child_on_response_hooks = on_response_hooks;
    auto* child_exception_handler = exception_handler;
    if (auto target = match_node(
          child->second,
          method_index,
          segments,
          next_segment_index,
          child_params,
          child_on_request_hooks,
          child_pre_handler_hooks,
          child_on_response_hooks,
          child_exception_handler)) {
      params = std::move(child_params);
      on_request_hooks = std::move(child_on_request_hooks);
      pre_handler_hooks = std::move(child_pre_handler_hooks);
      on_response_hooks = std::move(child_on_response_hooks);
      exception_handler = child_exception_handler;
      return target;
    }
  }

  if (node.parameter_child) {
    auto child_params = params;
    child_params.set(node.parameter_child->name, segment);
    auto child_on_request_hooks = on_request_hooks;
    auto child_pre_handler_hooks = pre_handler_hooks;
    auto child_on_response_hooks = on_response_hooks;
    auto* child_exception_handler = exception_handler;
    if (auto target = match_node(
          node.parameter_child->node,
          method_index,
          segments,
          next_segment_index,
          child_params,
          child_on_request_hooks,
          child_pre_handler_hooks,
          child_on_response_hooks,
          child_exception_handler)) {
      params = std::move(child_params);
      on_request_hooks = std::move(child_on_request_hooks);
      pre_handler_hooks = std::move(child_pre_handler_hooks);
      on_response_hooks = std::move(child_on_response_hooks);
      exception_handler = child_exception_handler;
      return target;
    }
  }

  if (node.wildcard_child) {
    auto wildcard_params = params;
    if (!node.wildcard_child->name.empty()) {
      wildcard_params.set(node.wildcard_child->name, detail::join_route_segments(segments, segment_index));
    }
    auto wildcard_on_request_hooks = on_request_hooks;
    auto wildcard_pre_handler_hooks = pre_handler_hooks;
    auto wildcard_on_response_hooks = on_response_hooks;
    auto* wildcard_exception_handler = exception_handler;
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
    if (wildcard_node.exception_handler) {
      wildcard_exception_handler = &*wildcard_node.exception_handler;
    }
    if (const auto& target = nodes_[node.wildcard_child->node].targets[method_index]) {
      params = std::move(wildcard_params);
      on_request_hooks = std::move(wildcard_on_request_hooks);
      pre_handler_hooks = std::move(wildcard_pre_handler_hooks);
      on_response_hooks = std::move(wildcard_on_response_hooks);
      exception_handler = wildcard_exception_handler;
      return &*target;
    }
  }

  return nullptr;
}

router::match_result router::match(method method_value, std::string_view path) const {
  const auto parsed_path = detail::parse_route_path(path);
  return match(method_value, parsed_path);
}

router::match_result router::match(method method_value, const detail::route_path& path) const {
  if (!path.valid) {
    match_result result;
    result.invalid_path = true;
    return result;
  }

  const auto method_index = static_cast<std::size_t>(method_value);
  if (method_index >= method_count_) {
    return {};
  }

  const auto path_segments = detail::selected_segments(path, matching_);
  route_params params;
  std::vector<const hook_type*> on_request_hooks;
  std::vector<const hook_type*> pre_handler_hooks;
  std::vector<const response_hook_type*> on_response_hooks;
  const exception_handler_type* exception_handler = nullptr;
  const auto* target = match_node(
    0,
    method_index,
    path_segments,
    0,
    params,
    on_request_hooks,
    pre_handler_hooks,
    on_response_hooks,
    exception_handler);
  if (!target) {
    return {};
  }
  return match_result{
    &target->handler,
    target->body,
    target->max_body_bytes,
    target->pattern,
    false,
    std::move(params),
    std::move(on_request_hooks),
    std::move(pre_handler_hooks),
    std::move(on_response_hooks),
    exception_handler,
  };
}

router::fallback_result router::fallback(std::string_view path) const {
  const auto parsed_path = detail::parse_route_path(path);
  if (!parsed_path.valid) {
    return {};
  }
  return fallback(parsed_path);
}

router::fallback_result router::fallback(const detail::route_path& path) const {
  fallback_result result;
  route_params params;
  const auto path_segments = detail::selected_segments(path, matching_);

  std::size_t node_index = 0;
  const auto remember_fallback = [&](const trie_node& node) {
    if (node.not_found_handler) {
      result.not_found_handler = &*node.not_found_handler;
      result.params = params;
    }
    if (node.exception_handler) {
      result.exception_handler = &*node.exception_handler;
    }
  };

  remember_fallback(nodes_[node_index]);

  for (std::size_t segment_index = 0; segment_index < path_segments.size(); ++segment_index) {
    const auto& segment = path_segments[segment_index];
    const auto& node = nodes_[node_index];
    if (auto child = node.static_children.find(segment); child != node.static_children.end()) {
      node_index = child->second;
      remember_fallback(nodes_[node_index]);
      continue;
    }

    if (node.parameter_child) {
      params.set(node.parameter_child->name, segment);
      node_index = node.parameter_child->node;
      remember_fallback(nodes_[node_index]);
      continue;
    }

    if (node.wildcard_child) {
      if (!node.wildcard_child->name.empty()) {
        params.set(node.wildcard_child->name, detail::join_route_segments(path_segments, segment_index));
      }
      node_index = node.wildcard_child->node;
      remember_fallback(nodes_[node_index]);
    }
    break;
  }

  return result;
}

const router::handler_type* router::find(method method_value, std::string_view path) const {
  return match(method_value, path).handler;
}

std::vector<method> router::allowed_methods(std::string_view path) const {
  const auto parsed_path = detail::parse_route_path(path);
  if (!parsed_path.valid) {
    return {};
  }
  return allowed_methods(parsed_path);
}

std::vector<method> router::allowed_methods(const detail::route_path& path) const {
  std::vector<method> methods;
  const auto path_segments = detail::selected_segments(path, matching_);
  for (auto method_value : detail::routed_methods) {
    auto params = route_params{};
    std::vector<const hook_type*> on_request_hooks;
    std::vector<const hook_type*> pre_handler_hooks;
    std::vector<const response_hook_type*> on_response_hooks;
    const exception_handler_type* exception_handler = nullptr;
    const auto method_index = static_cast<std::size_t>(method_value);
    if (method_index < method_count_ &&
        match_node(
          0,
          method_index,
          path_segments,
          0,
          params,
          on_request_hooks,
          pre_handler_hooks,
          on_response_hooks,
          exception_handler)) {
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
