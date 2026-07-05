#pragma once

#include <chrono>
#include <cstddef>
#include <memory>

#include <uvpp/protocols/io/stream_listener.hpp>
#include <uvpp/protocols/tls/context.hpp>

namespace uvp::tls {

class listener_options {
public:
  listener_options& handshake_timeout(std::chrono::milliseconds value) noexcept;
  listener_options& max_pending_handshakes(std::size_t value) noexcept;

  [[nodiscard]] std::chrono::milliseconds handshake_timeout() const noexcept;
  [[nodiscard]] std::size_t max_pending_handshakes() const noexcept;

private:
  std::chrono::milliseconds handshake_timeout_{std::chrono::seconds{10}};
  std::size_t max_pending_handshakes_ = 1024;
};

class listener {
public:
  listener(uvp::io::stream_listener lower, server_context context, listener_options options = {});
  ~listener();

  listener(listener&&) noexcept;
  listener& operator=(listener&&) noexcept;

  listener(const listener&) = delete;
  listener& operator=(const listener&) = delete;

  operator uvp::io::stream_listener() &&;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace uvp::tls
