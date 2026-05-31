#pragma once

namespace uv {
class tcp;
} // namespace uv

namespace uvp::http {

class connection {
public:
  connection() = default;

  [[nodiscard]] uv::tcp* tcp() noexcept { return tcp_; }
  [[nodiscard]] const uv::tcp* tcp() const noexcept { return tcp_; }

private:
  friend class request;

  explicit connection(uv::tcp* tcp) noexcept : tcp_(tcp) {}

  uv::tcp* tcp_ = nullptr;
};

} // namespace uvp::http

