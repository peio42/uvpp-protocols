#pragma once

#include <utility>

#include <uvpp/protocols/io/endpoint.hpp>

namespace uvp::http {

class connection_info {
public:
  connection_info() = default;

  connection_info(uvp::io::endpoint local, uvp::io::endpoint remote)
      : local_endpoint_(std::move(local)), remote_endpoint_(std::move(remote)) {}

  [[nodiscard]] const uvp::io::endpoint& local_endpoint() const noexcept { return local_endpoint_; }
  [[nodiscard]] const uvp::io::endpoint& remote_endpoint() const noexcept { return remote_endpoint_; }

private:
  uvp::io::endpoint local_endpoint_;
  uvp::io::endpoint remote_endpoint_;
};

} // namespace uvp::http
