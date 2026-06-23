#include <uvpp/protocols/http/router.hpp>

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

router::match_result router::match(method method_value, std::string_view path) const {
  for (const auto& route_entry : routes_) {
    if (route_entry.method_value != method_value) {
      continue;
    }

    route_params params;
    if (detail::route_pattern_matches(route_entry.pattern, path, params)) {
      return match_result{
        &route_entry.handler,
        route_entry.body,
        route_entry.max_body_bytes,
        std::move(params),
      };
    }
  }
  return {};
}

const router::handler_type* router::find(method method_value, std::string_view path) const {
  return match(method_value, path).handler;
}

} // namespace uvp::http
