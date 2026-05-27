#include <uvpp/protocols/http/method.hpp>

namespace uv::http {

std::string_view to_string(method value) noexcept {
  switch (value) {
  case method::get:
    return "GET";
  case method::head:
    return "HEAD";
  case method::post:
    return "POST";
  case method::put:
    return "PUT";
  case method::delete_:
    return "DELETE";
  case method::connect:
    return "CONNECT";
  case method::options:
    return "OPTIONS";
  case method::trace:
    return "TRACE";
  case method::patch:
    return "PATCH";
  case method::unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

} // namespace uv::http

