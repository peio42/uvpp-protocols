#pragma once

#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/response.hpp>
#include <uvpp/protocols/result.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace uv {
class loop;
}

namespace uvp::http {

namespace detail {
struct request_operation_state;
class streaming_request_state;
} // namespace detail

struct client_options {
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 4 * 1024 * 1024;
  std::chrono::milliseconds dns_timeout{0};
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds response_header_timeout{0};
  std::chrono::milliseconds response_body_timeout{0};
  bool tls_default_verify_paths = true;
  std::string tls_ca_file;
  std::string tls_ca_path;
};

using client_callback = std::function<void(uvp::result<http::response>)>;

struct response_head {
  unsigned int status_code = 0;
  http::headers headers;
};

using response_headers_callback = std::function<void(const response_head&)>;
using response_data_callback = std::function<void(std::span<const std::byte>)>;
using response_complete_callback = std::function<void(uvp::result<void>)>;

class request_operation {
public:
  request_operation() = default;
  explicit request_operation(std::shared_ptr<detail::request_operation_state> state);

  void cancel() noexcept;
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  std::shared_ptr<detail::request_operation_state> state_;
};

class streaming_request {
public:
  streaming_request() = default;

  streaming_request& on_response_headers(response_headers_callback callback) &;
  streaming_request&& on_response_headers(response_headers_callback callback) &&;
  streaming_request& on_data(response_data_callback callback) &;
  streaming_request&& on_data(response_data_callback callback) &&;
  streaming_request& on_complete(response_complete_callback callback) &;
  streaming_request&& on_complete(response_complete_callback callback) &&;

  [[nodiscard]] request_operation start();
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  friend class client;

  explicit streaming_request(std::shared_ptr<detail::streaming_request_state> state);

  std::shared_ptr<detail::streaming_request_state> state_;
};

class client {
public:
  explicit client(uv::loop& loop);
  client(uv::loop& loop, client_options options);

  [[nodiscard]] request_operation get(std::string_view url, client_callback callback);
  [[nodiscard]] request_operation fetch(http::method method, std::string_view url, client_callback callback);
  [[nodiscard]] streaming_request stream(http::method method, std::string_view url);
  [[nodiscard]] streaming_request stream_get(std::string_view url);

private:
  uv::loop* loop_;
  client_options options_;
};

} // namespace uvp::http
