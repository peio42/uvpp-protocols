#pragma once

#include <system_error>

namespace uvp::tls {

enum class errc {
  ok = 0,
  openssl_error,
  handshake_failed,
  verification_failed,
  protocol_error,
  closed,
};

const std::error_category& error_category() noexcept;

std::error_code make_error_code(errc value) noexcept;

} // namespace uvp::tls

namespace std {

template<>
struct is_error_code_enum<uvp::tls::errc> : true_type {};

} // namespace std
