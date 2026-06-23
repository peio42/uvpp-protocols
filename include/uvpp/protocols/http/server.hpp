#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/router.hpp>
#include <uvpp/protocols/http/server_options.hpp>
#include <uvpp/protocols/http/upgrade.hpp>
#include <uvpp/protocols/io/stream_listener.hpp>

namespace uv {
class loop;
} // namespace uv

namespace uvp::http {

class server {
public:
  using error_handler_type = std::function<void(request&, response&, std::exception_ptr)>;
  using upgrade_handler_type = std::function<void(upgrade_request&)>;

  explicit server(uv::loop& loop);
  server(uv::loop& loop, server_options options);
  ~server();

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

#define UVP_HTTP_SERVER_ROUTE_METHOD(name) \
  template<class BodyPolicy, class Handler> \
  server& name(std::string_view pattern, BodyPolicy policy, Handler&& handler) { \
    router_.name(pattern, policy, std::forward<Handler>(handler)); \
    return *this; \
  } \
  template<class Handler> \
  server& name(std::string_view pattern, Handler&& handler) { \
    router_.name(pattern, std::forward<Handler>(handler)); \
    return *this; \
  }

  UVP_HTTP_SERVER_ROUTE_METHOD(get)
  UVP_HTTP_SERVER_ROUTE_METHOD(post)
  UVP_HTTP_SERVER_ROUTE_METHOD(put)
  UVP_HTTP_SERVER_ROUTE_METHOD(patch)
  UVP_HTTP_SERVER_ROUTE_METHOD(delete_)
  UVP_HTTP_SERVER_ROUTE_METHOD(head)
  UVP_HTTP_SERVER_ROUTE_METHOD(options)

#undef UVP_HTTP_SERVER_ROUTE_METHOD

  template<class BodyPolicy, class Handler>
  server& route(method method_value, std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_.route(method_value, pattern, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& route(method method_value, std::string_view pattern, Handler&& handler) {
    router_.route(method_value, pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& not_found(Handler&& handler) {
    not_found_handler_ = uvp::http::detail::wrap_none_handler(std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& on_error(Handler&& handler) {
    error_handler_ = error_handler_type(std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& upgrade(std::string_view pattern, Handler&& handler) {
    upgrade_routes_.push_back(upgrade_route{
      std::string(pattern),
      [handler = std::forward<Handler>(handler)](upgrade_request& req) mutable {
        (void)handler(req);
      },
    });
    return *this;
  }

  void listen(std::string_view host, unsigned int port);
  void listen(uvp::io::stream_listener listener);
  void close() noexcept;

  [[nodiscard]] uv::loop& loop() noexcept { return *loop_; }
  [[nodiscard]] const server_options& options() const noexcept { return options_; }
  [[nodiscard]] const router& routes() const noexcept { return router_; }

private:
  uv::loop* loop_;
  server_options options_;
  router router_;
  router::handler_type not_found_handler_;
  error_handler_type error_handler_;
  struct upgrade_route {
    std::string pattern;
    upgrade_handler_type handler;
  };
  std::vector<upgrade_route> upgrade_routes_;
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace uvp::http
