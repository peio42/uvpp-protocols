#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/method.hpp>

namespace uv::http {

class request;
class response;

class router {
public:
  using handler_type = std::function<void(request&, response&)>;

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

  [[nodiscard]] const handler_type* find(method method_value, std::string_view path) const noexcept;
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

inline const router::handler_type* router::find(method method_value, std::string_view path) const noexcept {
  for (const auto& route_entry : routes_) {
    if (route_entry.method_value == method_value && route_entry.pattern == path) {
      return &route_entry.handler;
    }
  }
  return nullptr;
}

} // namespace uv::http
