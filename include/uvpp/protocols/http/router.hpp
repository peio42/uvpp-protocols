#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http {

class request;
class response;

class router {
public:
  using handler_type = std::function<void(request&, response&)>;

  struct match_result {
    const handler_type* handler = nullptr;
    route_params params;

    [[nodiscard]] bool ok() const noexcept { return handler != nullptr; }
    explicit operator bool() const noexcept { return ok(); }
  };

  router() = default;

  template<class Handler>
  router& route(method method_value, std::string_view pattern, Handler&& handler) {
    routes_.push_back(route_entry{
      method_value,
      std::string(pattern),
      handler_type(std::forward<Handler>(handler)),
    });
    return *this;
  }

  template<class Handler>
  router& get(std::string_view pattern, Handler&& handler) {
    return route(method::get, pattern, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& post(std::string_view pattern, Handler&& handler) {
    return route(method::post, pattern, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& put(std::string_view pattern, Handler&& handler) {
    return route(method::put, pattern, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& patch(std::string_view pattern, Handler&& handler) {
    return route(method::patch, pattern, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& delete_(std::string_view pattern, Handler&& handler) {
    return route(method::delete_, pattern, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& head(std::string_view pattern, Handler&& handler) {
    return route(method::head, pattern, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& options(std::string_view pattern, Handler&& handler) {
    return route(method::options, pattern, std::forward<Handler>(handler));
  }

  [[nodiscard]] match_result match(method method_value, std::string_view path) const;
  [[nodiscard]] const handler_type* find(method method_value, std::string_view path) const;
  [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return routes_.size(); }

private:
  struct route_entry {
    method method_value;
    std::string pattern;
    handler_type handler;
  };

  std::vector<route_entry> routes_;
};

namespace detail {

inline bool next_route_segment(std::string_view& input, std::string_view& segment) noexcept {
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

inline bool route_pattern_matches(std::string_view pattern, std::string_view path, route_params& params) {
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

inline router::match_result router::match(method method_value, std::string_view path) const {
  for (const auto& route_entry : routes_) {
    if (route_entry.method_value != method_value) {
      continue;
    }

    route_params params;
    if (detail::route_pattern_matches(route_entry.pattern, path, params)) {
      return match_result{&route_entry.handler, std::move(params)};
    }
  }
  return {};
}

inline const router::handler_type* router::find(method method_value, std::string_view path) const {
  return match(method_value, path).handler;
}

} // namespace uvp::http
