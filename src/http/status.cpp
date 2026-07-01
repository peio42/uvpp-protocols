#include <uvpp/protocols/http/status.hpp>

namespace uvp::http {

std::string_view reason_phrase(status value) noexcept {
  switch (value) {
  case status::ok:
    return "OK";
  case status::created:
    return "Created";
  case status::no_content:
    return "No Content";
  case status::moved_permanently:
    return "Moved Permanently";
  case status::found:
    return "Found";
  case status::not_modified:
    return "Not Modified";
  case status::bad_request:
    return "Bad Request";
  case status::unauthorized:
    return "Unauthorized";
  case status::forbidden:
    return "Forbidden";
  case status::not_found:
    return "Not Found";
  case status::method_not_allowed:
    return "Method Not Allowed";
  case status::request_timeout:
    return "Request Timeout";
  case status::conflict:
    return "Conflict";
  case status::payload_too_large:
    return "Payload Too Large";
  case status::unsupported_media_type:
    return "Unsupported Media Type";
  case status::unprocessable_content:
    return "Unprocessable Content";
  case status::too_many_requests:
    return "Too Many Requests";
  case status::bad_gateway:
    return "Bad Gateway";
  case status::service_unavailable:
    return "Service Unavailable";
  case status::internal_server_error:
    return "Internal Server Error";
  case status::not_implemented:
    return "Not Implemented";
  }
  return "";
}

} // namespace uvp::http
