#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/connection.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http {

namespace detail {
struct request_body_stream_state;
} // namespace detail

namespace body {

struct none {};

struct bytes {
  std::size_t max_size = 0;
};

struct text {
  std::size_t max_size = 0;
};

struct stream {
  std::size_t max_size = 0;
};

} // namespace body

class request_body_stream {
public:
  request_body_stream();
  ~request_body_stream() = default;

  request_body_stream(request_body_stream&&) noexcept = default;
  request_body_stream& operator=(request_body_stream&&) noexcept = default;

  request_body_stream(const request_body_stream&) = delete;
  request_body_stream& operator=(const request_body_stream&) = delete;

  request_body_stream& on_data(std::function<void(std::span<const std::byte>)> callback);
  request_body_stream& on_end(std::function<void()> callback);
  request_body_stream& on_error(std::function<void(std::error_code)> callback);

  void pause();
  void resume();

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool paused() const noexcept;

private:
  friend class server;

  explicit request_body_stream(std::shared_ptr<detail::request_body_stream_state> state) noexcept;

  void emit_data(std::span<const std::byte> chunk);
  void emit_end();
  void emit_error(std::error_code error);
  void cancel() noexcept;
  void on_pause_resume(std::function<void()> on_pause, std::function<void()> on_resume);

  std::shared_ptr<detail::request_body_stream_state> state_;
};

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
