#pragma once

#include <functional>
#include <memory>

#include <uvpp/protocols/io/byte_stream.hpp>
#include <uvpp/protocols/io/endpoint.hpp>
#include <uvpp/protocols/io/error.hpp>

namespace uv {
class loop;
} // namespace uv

namespace uvp::tls {
class listener;
} // namespace uvp::tls

namespace uvp::io {

class accept_result {
public:
  accept_result(byte_stream stream);
  accept_result(stream_error error);

  [[nodiscard]] bool ok() const noexcept;
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] byte_stream& stream() &;
  [[nodiscard]] byte_stream&& stream() &&;
  [[nodiscard]] const stream_error& error() const noexcept;

private:
  byte_stream stream_;
  stream_error error_;
  bool ok_ = false;
};

using accept_callback = std::function<void(accept_result)>;

class stream_listener {
public:
  struct concept_ {
    virtual ~concept_() = default;

    virtual uv::loop& loop() noexcept = 0;
    virtual void listen(accept_callback on_accept) = 0;
    virtual void close() = 0;
    virtual endpoint local_endpoint() const = 0;
  };

  stream_listener() = default;
  ~stream_listener();

  stream_listener(stream_listener&&) noexcept;
  stream_listener& operator=(stream_listener&&) noexcept;

  stream_listener(const stream_listener&) = delete;
  stream_listener& operator=(const stream_listener&) = delete;

  [[nodiscard]] uv::loop& loop() noexcept;

  void listen(accept_callback on_accept);
  void close();

  [[nodiscard]] endpoint local_endpoint() const;

  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend class tcp_listener;
  friend class pipe_listener;
  friend class uvp::tls::listener;

  explicit stream_listener(std::unique_ptr<concept_> self);

  std::unique_ptr<concept_> self_;
};

} // namespace uvp::io
