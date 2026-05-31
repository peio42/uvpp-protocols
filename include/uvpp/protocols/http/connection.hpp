#pragma once

namespace uv {
class tcp;
} // namespace uv

namespace uvp::io {
class byte_stream;
} // namespace uvp::io

namespace uvp::http {

class connection {
public:
  connection() = default;

  [[nodiscard]] uv::tcp* tcp() noexcept { return tcp_; }
  [[nodiscard]] const uv::tcp* tcp() const noexcept { return tcp_; }
  [[nodiscard]] uvp::io::byte_stream* stream() noexcept { return stream_; }
  [[nodiscard]] const uvp::io::byte_stream* stream() const noexcept { return stream_; }

  explicit connection(uv::tcp* tcp) noexcept : tcp_(tcp) {}
  explicit connection(uvp::io::byte_stream* stream) noexcept : stream_(stream) {}

private:
  uv::tcp* tcp_ = nullptr;
  uvp::io::byte_stream* stream_ = nullptr;
};

} // namespace uvp::http
