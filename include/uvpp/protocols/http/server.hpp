#pragma once

#include <string_view>
#include <utility>

#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/router.hpp>
#include <uvpp/protocols/http/server_options.hpp>

namespace uv {
class loop;
} // namespace uv

namespace uvp::http {

class server {
public:
  explicit server(uv::loop& loop);
  server(uv::loop& loop, server_options options);

  template<class Handler>
  server& get(std::string_view pattern, Handler&& handler) {
    router_.get(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& post(std::string_view pattern, Handler&& handler) {
    router_.post(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& put(std::string_view pattern, Handler&& handler) {
    router_.put(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& patch(std::string_view pattern, Handler&& handler) {
    router_.patch(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& delete_(std::string_view pattern, Handler&& handler) {
    router_.delete_(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& head(std::string_view pattern, Handler&& handler) {
    router_.head(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& options(std::string_view pattern, Handler&& handler) {
    router_.options(pattern, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  server& route(method method_value, std::string_view pattern, Handler&& handler) {
    router_.route(method_value, pattern, std::forward<Handler>(handler));
    return *this;
  }

  void listen(std::string_view host, unsigned int port);
  void close() noexcept;

  [[nodiscard]] uv::loop& loop() noexcept { return *loop_; }
  [[nodiscard]] const server_options& options() const noexcept { return options_; }
  [[nodiscard]] const router& routes() const noexcept { return router_; }

private:
  uv::loop* loop_;
  server_options options_;
  router router_;
  bool listening_ = false;
};

} // namespace uvp::http
