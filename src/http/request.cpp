#include <uvpp/protocols/http/request.hpp>

namespace uvp::http {

request::request(
  http::method method,
  std::string target,
  std::string path,
  std::string query,
  http::headers headers,
  std::vector<std::byte> body,
  route_params params,
  http::connection connection)
    : method_(method),
      target_(std::move(target)),
      path_(std::move(path)),
      query_(std::move(query)),
      headers_(std::move(headers)),
      body_(std::move(body)),
      params_(std::move(params)),
      connection_(connection) {}

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
