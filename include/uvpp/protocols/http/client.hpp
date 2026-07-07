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
class connection_pool;
struct request_operation_state;
class streaming_request_state;
} // namespace detail

struct proxy_options {
  std::string url;
  std::string authorization;

  proxy_options& basic_auth(std::string_view username, std::string_view password) &;
  proxy_options&& basic_auth(std::string_view username, std::string_view password) &&;
};

struct client_options {
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 4 * 1024 * 1024;
  std::size_t max_pending_request_body_bytes = 2 * 1024 * 1024;
  std::size_t max_idle_connections_per_origin = 0;
  std::size_t max_redirects = 5;
  std::chrono::milliseconds idle_connection_timeout = std::chrono::seconds{30};
  std::chrono::milliseconds dns_timeout{0};
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds tls_handshake_timeout{0};
  std::chrono::milliseconds request_body_timeout{0};
  std::chrono::milliseconds response_header_timeout{0};
  std::chrono::milliseconds response_body_timeout{0};
  bool follow_redirects = false;
  bool tls_default_verify_paths = true;
  proxy_options proxy;
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
using request_body_drain_callback = std::function<void()>;

class request_operation {
public:
  request_operation() = default;
  explicit request_operation(std::shared_ptr<detail::request_operation_state> state);

  void cancel() noexcept;
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  std::shared_ptr<detail::request_operation_state> state_;
};

class request_body_writer {
public:
  request_body_writer() = default;
  explicit request_body_writer(std::shared_ptr<detail::streaming_request_state> state);

  request_body_writer& on_drain(request_body_drain_callback callback) &;
  request_body_writer&& on_drain(request_body_drain_callback callback) &&;

  [[nodiscard]] stream_write_result write(const char* chunk);
  [[nodiscard]] stream_write_result write(std::string_view chunk);
  [[nodiscard]] stream_write_result write(std::span<const std::byte> chunk);
  [[nodiscard]] stream_write_result write(std::string chunk);
  void end();
  void cancel() noexcept;

  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  std::shared_ptr<detail::streaming_request_state> state_;
};

class streaming_request {
public:
  streaming_request() = default;

  streaming_request& header(std::string_view name, std::string_view value) &;
  streaming_request&& header(std::string_view name, std::string_view value) &&;
  streaming_request& content_length(std::size_t bytes) &;
  streaming_request&& content_length(std::size_t bytes) &&;
  streaming_request& chunked() &;
  streaming_request&& chunked() &&;
  streaming_request& on_response_headers(response_headers_callback callback) &;
  streaming_request&& on_response_headers(response_headers_callback callback) &&;
  streaming_request& on_data(response_data_callback callback) &;
  streaming_request&& on_data(response_data_callback callback) &&;
  streaming_request& on_complete(response_complete_callback callback) &;
  streaming_request&& on_complete(response_complete_callback callback) &&;

  [[nodiscard]] request_body_writer start();
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
  [[nodiscard]] streaming_request request(http::method method, std::string_view url);
  [[nodiscard]] streaming_request stream(http::method method, std::string_view url);
  [[nodiscard]] streaming_request stream_get(std::string_view url);
  void close_idle_connections() noexcept;

private:
  uv::loop* loop_;
  client_options options_;
  std::shared_ptr<detail::connection_pool> pool_;
};

} // namespace uvp::http
