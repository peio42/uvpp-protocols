#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/connection.hpp>
#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/multipart_options.hpp>
#include <uvpp/protocols/http/route_params.hpp>
#include <uvpp/protocols/json.hpp>

namespace uvp::http {

namespace detail {
struct request_body_stream_state;
} // namespace detail

namespace body {

struct none {};

struct bytes {};

struct text {};

struct stream {};

struct multipart_stream {
  multipart_stream() = default;
  explicit multipart_stream(http::multipart_stream_options options) noexcept
      : options_(std::move(options)) {}

  multipart_stream& options(http::multipart_stream_options value) & noexcept {
    options_ = std::move(value);
    return *this;
  }

  multipart_stream&& options(http::multipart_stream_options value) && noexcept {
    options(std::move(value));
    return std::move(*this);
  }

  multipart_stream& max_total_bytes(std::size_t value) & noexcept {
    options_.max_total_bytes(value);
    return *this;
  }

  multipart_stream&& max_total_bytes(std::size_t value) && noexcept {
    max_total_bytes(value);
    return std::move(*this);
  }

  multipart_stream& max_file_bytes(std::size_t value) & noexcept {
    options_.max_file_bytes(value);
    return *this;
  }

  multipart_stream&& max_file_bytes(std::size_t value) && noexcept {
    max_file_bytes(value);
    return std::move(*this);
  }

  multipart_stream& max_field_bytes(std::size_t value) & noexcept {
    options_.max_field_bytes(value);
    return *this;
  }

  multipart_stream&& max_field_bytes(std::size_t value) && noexcept {
    max_field_bytes(value);
    return std::move(*this);
  }

  [[nodiscard]] const http::multipart_stream_options& options() const noexcept { return options_; }

private:
  http::multipart_stream_options options_;
};

struct multipart_form {
  multipart_form() = default;
  explicit multipart_form(http::multipart_form_options options) noexcept
      : options_(std::move(options)) {}

  multipart_form& options(http::multipart_form_options value) & noexcept {
    options_ = std::move(value);
    return *this;
  }

  multipart_form&& options(http::multipart_form_options value) && noexcept {
    options(std::move(value));
    return std::move(*this);
  }

  multipart_form& max_total_bytes(std::size_t value) & noexcept {
    options_.limits.max_total_bytes = value;
    return *this;
  }

  multipart_form&& max_total_bytes(std::size_t value) && noexcept {
    max_total_bytes(value);
    return std::move(*this);
  }

  multipart_form& max_file_bytes(std::size_t value) & noexcept {
    options_.limits.max_file_bytes = value;
    return *this;
  }

  multipart_form&& max_file_bytes(std::size_t value) && noexcept {
    max_file_bytes(value);
    return std::move(*this);
  }

  multipart_form& max_field_bytes(std::size_t value) & noexcept {
    options_.limits.max_field_bytes = value;
    return *this;
  }

  multipart_form&& max_field_bytes(std::size_t value) && noexcept {
    max_field_bytes(value);
    return std::move(*this);
  }

  multipart_form& max_memory_bytes(std::size_t value) & noexcept {
    options_.max_memory_bytes = value;
    return *this;
  }

  multipart_form&& max_memory_bytes(std::size_t value) && noexcept {
    max_memory_bytes(value);
    return std::move(*this);
  }

  [[nodiscard]] const http::multipart_form_options& options() const noexcept { return options_; }

private:
  http::multipart_form_options options_;
};

template<class T = uvp::json>
struct json {};

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

class query_params {
public:
  struct entry {
    std::string name;
    std::vector<std::string> values;
  };

  using container_type = std::vector<entry>;
  using const_iterator = container_type::const_iterator;

  query_params() = default;
  explicit query_params(std::string_view raw_query);

  [[nodiscard]] bool contains(std::string_view name) const noexcept;
  [[nodiscard]] std::optional<std::string_view> first(std::string_view name) const noexcept;
  [[nodiscard]] std::string_view get(std::string_view name, std::string_view fallback = {}) const noexcept;
  [[nodiscard]] std::span<const std::string> all(std::string_view name) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }

private:
  container_type entries_;
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
    http::connection_info connection,
    std::vector<std::string> decoded_path_segments = {});

  [[nodiscard]] http::method method() const noexcept { return method_; }
  // Returned views borrow storage owned by this request object.
  [[nodiscard]] std::string_view target() const noexcept { return target_; }
  [[nodiscard]] std::string_view path() const noexcept { return path_; }
  [[nodiscard]] std::string_view query() const noexcept { return query_; }
  [[nodiscard]] std::string_view matched_pattern() const noexcept { return matched_pattern_; }
  [[nodiscard]] std::optional<std::string_view> query(std::string_view name) const noexcept {
    return query_params_.first(name);
  }
  [[nodiscard]] std::string_view query_or(std::string_view name, std::string_view fallback = {}) const noexcept {
    return query_params_.get(name, fallback);
  }
  [[nodiscard]] std::span<const std::string> query_all(std::string_view name) const noexcept {
    return query_params_.all(name);
  }
  [[nodiscard]] const http::query_params& query_params() const noexcept { return query_params_; }

  [[nodiscard]] const http::headers& headers() const noexcept { return headers_; }
  [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

  // Returned body views borrow storage owned by this request object.
  [[nodiscard]] std::span<const std::byte> body_bytes() const noexcept;
  [[nodiscard]] std::string_view body() const noexcept;

  [[nodiscard]] const route_params& params() const noexcept { return params_; }
  [[nodiscard]] std::span<const std::string> decoded_path_segments() const noexcept { return decoded_path_segments_; }
  [[nodiscard]] const http::connection_info& connection() const noexcept { return connection_; }

private:
  friend class router;
  friend class server;

  http::method method_ = http::method::unknown;
  std::string target_;
  std::string path_;
  std::string query_;
  std::string_view matched_pattern_;
  http::query_params query_params_;
  http::headers headers_;
  std::vector<std::byte> body_;
  route_params params_;
  std::vector<std::string> decoded_path_segments_;
  http::connection_info connection_;
};

} // namespace uvp::http
