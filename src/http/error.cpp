#include <uvpp/protocols/http/error.hpp>

#include <string>

namespace uvp::http {

namespace {

class http_error_category : public std::error_category {
public:
  [[nodiscard]] const char* name() const noexcept override { return "uvp.http"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<errc>(value)) {
    case errc::malformed_content_type:
      return "malformed content type";
    case errc::unsupported_media_type:
      return "unsupported media type";
    case errc::multipart_missing_boundary:
      return "multipart boundary is missing";
    case errc::multipart_invalid_boundary:
      return "multipart boundary is invalid";
    case errc::multipart_malformed_body:
      return "multipart body is malformed";
    case errc::multipart_malformed_part_header:
      return "multipart part header is malformed";
    case errc::multipart_unexpected_end:
      return "multipart body ended unexpectedly";
    case errc::multipart_limit_exceeded:
      return "multipart limit exceeded";
    case errc::client_invalid_url:
      return "HTTP client URL is invalid";
    case errc::client_unsupported_scheme:
      return "HTTP client URL scheme is unsupported";
    case errc::client_dns_failed:
      return "HTTP client DNS resolution failed";
    case errc::client_connect_failed:
      return "HTTP client connection failed";
    case errc::client_malformed_response:
      return "HTTP client response is malformed";
    case errc::client_body_limit_exceeded:
      return "HTTP client response body limit exceeded";
    case errc::client_cancelled:
      return "HTTP client request was cancelled";
    }
    return "unknown HTTP protocol error";
  }
};

const std::error_category& category() noexcept {
  static const http_error_category instance;
  return instance;
}

} // namespace

std::error_code make_error_code(errc value) noexcept {
  return {static_cast<int>(value), category()};
}

} // namespace uvp::http
