#pragma once

#include <system_error>
#include <type_traits>

namespace uvp::http {

enum class errc {
  malformed_content_type = 1,
  unsupported_media_type,
  multipart_missing_boundary,
  multipart_invalid_boundary,
  multipart_malformed_body,
  multipart_malformed_part_header,
  multipart_unexpected_end,
  multipart_limit_exceeded,
  client_invalid_url,
  client_unsupported_scheme,
  client_dns_failed,
  client_connect_failed,
  client_tls_failed,
  client_malformed_response,
  client_header_limit_exceeded,
  client_body_limit_exceeded,
  client_cancelled,
  client_timeout,
};

std::error_code make_error_code(errc value) noexcept;

} // namespace uvp::http

namespace std {

template<>
struct is_error_code_enum<uvp::http::errc> : true_type {};

} // namespace std
