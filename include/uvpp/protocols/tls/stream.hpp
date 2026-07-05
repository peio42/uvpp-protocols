#pragma once

#include <functional>
#include <string>

#include <uvpp/protocols/io/byte_stream.hpp>
#include <uvpp/protocols/result.hpp>
#include <uvpp/protocols/tls/context.hpp>
#include <uvpp/protocols/tls/error.hpp>

namespace uvp::tls {

class handshake_result {
public:
  handshake_result(uvp::io::byte_stream stream, std::string selected_alpn);
  handshake_result(uvp::error error);

  [[nodiscard]] bool ok() const noexcept;
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] uvp::io::byte_stream& stream() &;
  [[nodiscard]] uvp::io::byte_stream&& stream() &&;
  [[nodiscard]] std::string_view selected_alpn() const noexcept;
  [[nodiscard]] const uvp::error& error() const&;

private:
  uvp::io::byte_stream stream_;
  std::string selected_alpn_;
  uvp::error error_;
  bool ok_ = false;
};

using handshake_callback = std::function<void(handshake_result)>;

void accept(uvp::io::byte_stream lower, server_context context, handshake_callback callback);
void connect(uvp::io::byte_stream lower, client_context context, handshake_callback callback);

} // namespace uvp::tls
