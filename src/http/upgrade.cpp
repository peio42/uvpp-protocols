#include <uvpp/protocols/http/upgrade.hpp>

#include <utility>

namespace uvp::http {

upgrade_request::upgrade_request(
  http::method method,
  std::string target,
  std::string path,
  std::string query,
  http::headers headers,
  route_params params,
  std::span<const std::byte> extra_bytes,
  accept_operation accept)
    : method_(method),
      target_(std::move(target)),
      path_(std::move(path)),
      query_(std::move(query)),
      headers_(std::move(headers)),
      params_(std::move(params)),
      extra_bytes_(extra_bytes.begin(), extra_bytes.end()),
      accept_(std::move(accept)) {}

std::string_view upgrade_request::header(std::string_view name) const noexcept {
  return headers_.get(name);
}

void upgrade_request::accept(std::string response, accept_callback on_accept) {
  if (accept_) {
    accept_(std::move(response), std::move(on_accept));
  }
}

void upgrade_request::reject(std::string response) {
  accept(std::move(response), [](uvp::io::byte_stream stream) mutable {
    stream.close();
  });
}

} // namespace uvp::http
