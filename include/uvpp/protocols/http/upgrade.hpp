#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/connection.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/route_params.hpp>
#include <uvpp/protocols/io/byte_stream.hpp>

namespace uvp::http {

class upgrade_request {
public:
  using accept_callback = std::function<void(uvp::io::byte_stream)>;
  using accept_operation = std::function<void(std::string, accept_callback)>;

  upgrade_request() = default;

  upgrade_request(
    http::method method,
    std::string target,
    std::string path,
    std::string query,
    http::headers headers,
    route_params params,
    http::connection_info connection,
    std::span<const std::byte> extra_bytes,
    accept_operation accept);

  [[nodiscard]] http::method method() const noexcept { return method_; }
  [[nodiscard]] std::string_view target() const noexcept { return target_; }
  [[nodiscard]] std::string_view path() const noexcept { return path_; }
  [[nodiscard]] std::string_view query() const noexcept { return query_; }

  [[nodiscard]] const http::headers& headers() const noexcept { return headers_; }
  [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
  [[nodiscard]] const route_params& params() const noexcept { return params_; }
  [[nodiscard]] const http::connection_info& connection() const noexcept { return connection_; }
  [[nodiscard]] std::span<const std::byte> extra_bytes() const noexcept { return extra_bytes_; }

  void accept(std::string response, accept_callback on_accept);
  void reject(std::string response);

private:
  http::method method_ = http::method::unknown;
  std::string target_;
  std::string path_;
  std::string query_;
  http::headers headers_;
  route_params params_;
  http::connection_info connection_;
  std::vector<std::byte> extra_bytes_;
  accept_operation accept_;
};

} // namespace uvp::http
