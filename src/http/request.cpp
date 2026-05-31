#include <uvpp/protocols/http/request.hpp>

namespace uvp::http {

std::string_view request::header(std::string_view name) const noexcept {
  return headers_.get(name);
}

std::span<const std::byte> request::body_bytes() const noexcept {
  return body_;
}

std::string_view request::body() const noexcept {
  if (body_.empty()) {
    return {};
  }

  const auto* data = reinterpret_cast<const char*>(body_.data());
  return std::string_view(data, body_.size());
}

} // namespace uvp::http
