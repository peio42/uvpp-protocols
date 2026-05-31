#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <uvpp/protocols/io/stream_listener.hpp>

namespace uv {
class loop;
} // namespace uv

namespace uvp::io {

class tcp_listener {
public:
  explicit tcp_listener(uv::loop& loop);
  ~tcp_listener();

  tcp_listener(tcp_listener&&) noexcept;
  tcp_listener& operator=(tcp_listener&&) noexcept;

  tcp_listener(const tcp_listener&) = delete;
  tcp_listener& operator=(const tcp_listener&) = delete;

  tcp_listener& bind(std::string_view host, unsigned int port);
  tcp_listener& backlog(unsigned int value) noexcept;

  operator stream_listener() &&;

private:
  struct impl;

  std::unique_ptr<impl> impl_;
};

} // namespace uvp::io

