#pragma once

#include <string_view>

namespace uvp::http {

enum class status : unsigned int {
  ok = 200,
  created = 201,
  no_content = 204,
  bad_request = 400,
  not_found = 404,
  method_not_allowed = 405,
  payload_too_large = 413,
  internal_server_error = 500,
  not_implemented = 501,
};

std::string_view reason_phrase(status value) noexcept;

} // namespace uvp::http

