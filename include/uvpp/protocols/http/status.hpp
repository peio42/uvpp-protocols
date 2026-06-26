#pragma once

#include <string_view>

namespace uvp::http {

enum class status : unsigned int {
  ok = 200,
  created = 201,
  no_content = 204,
  moved_permanently = 301,
  found = 302,
  not_modified = 304,
  bad_request = 400,
  unauthorized = 401,
  forbidden = 403,
  not_found = 404,
  method_not_allowed = 405,
  request_timeout = 408,
  conflict = 409,
  payload_too_large = 413,
  unprocessable_content = 422,
  unprocessable_entity = unprocessable_content,
  too_many_requests = 429,
  bad_gateway = 502,
  service_unavailable = 503,
  internal_server_error = 500,
  not_implemented = 501,
};

std::string_view reason_phrase(status value) noexcept;

} // namespace uvp::http
