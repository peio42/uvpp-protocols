#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>

#include <uvpp/protocols/io/endpoint.hpp>
#include <uvpp/protocols/io/error.hpp>

namespace uv {
class loop;
class pipe;
class tcp;
} // namespace uv

namespace uvp::io {

class read_result {
public:
  read_result() = default;
  read_result(std::span<const std::byte> bytes, stream_error error = {}, bool eof = false);

  [[nodiscard]] bool ok() const noexcept { return error_.ok() && !eof_; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] bool eof() const noexcept { return eof_; }
  [[nodiscard]] const stream_error& error() const noexcept { return error_; }
  // Valid only while the read callback is running.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
  std::span<const std::byte> bytes_;
  stream_error error_;
  bool eof_ = false;
};

using read_callback = std::function<void(read_result)>;
using write_callback = std::function<void(stream_error)>;
using close_callback = std::function<void()>;

class byte_stream {
public:
  struct concept_;

  byte_stream() = default;
  ~byte_stream();

  byte_stream(byte_stream&&) noexcept;
  byte_stream& operator=(byte_stream&&) noexcept;

  byte_stream(const byte_stream&) = delete;
  byte_stream& operator=(const byte_stream&) = delete;

  [[nodiscard]] uv::loop& loop() noexcept;

  void read_start(read_callback on_read);
  void read_stop();
  void write(std::span<const std::byte> bytes, write_callback on_write);
  void close(close_callback on_close = {});

  [[nodiscard]] endpoint local_endpoint() const;
  [[nodiscard]] endpoint remote_endpoint() const;

  [[nodiscard]] explicit operator bool() const noexcept;

  [[nodiscard]] uv::tcp* tcp() noexcept;
  [[nodiscard]] uv::pipe* pipe() noexcept;

  explicit byte_stream(std::unique_ptr<concept_> self);

private:
  friend class stream_listener;
  friend class tcp_listener;
  friend class pipe_listener;

  std::unique_ptr<concept_> self_;
};

} // namespace uvp::io
