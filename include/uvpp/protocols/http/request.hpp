#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/connection.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http {

class request {
public:
  request() = default;
  request(
    http::method method,
    std::string target,
    std::string path,
    std::string query,
    http::headers headers,
    std::vector<std::byte> body,
    route_params params,
    http::connection connection);

  [[nodiscard]] http::method method() const noexcept { return method_; }
  [[nodiscard]] std::string_view target() const noexcept { return target_; }
  [[nodiscard]] std::string_view path() const noexcept { return path_; }
  [[nodiscard]] std::string_view query() const noexcept { return query_; }

  [[nodiscard]] const http::headers& headers() const noexcept { return headers_; }
  [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

  [[nodiscard]] std::span<const std::byte> body_bytes() const noexcept;
  [[nodiscard]] std::string_view body() const noexcept;

  [[nodiscard]] const route_params& params() const noexcept { return params_; }
  [[nodiscard]] http::connection& connection() noexcept { return connection_; }
  [[nodiscard]] const http::connection& connection() const noexcept { return connection_; }

private:
  friend class router;
  friend class server;

  http::method method_ = http::method::unknown;
  std::string target_;
  std::string path_;
  std::string query_;
  http::headers headers_;
  std::vector<std::byte> body_;
  route_params params_;
  http::connection connection_;
};

} // namespace uvp::http
