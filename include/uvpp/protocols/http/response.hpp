#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/status.hpp>
#include <uvpp/protocols/json.hpp>

namespace uvp::http {

namespace detail {
struct response_state;
} // namespace detail

class deferred_response;
class sse_stream;
class streaming_response;

struct sse_event {
  std::string_view event;
  std::string_view id;
  // Data bytes are split into SSE data lines as-is; callers own UTF-8 validity.
  std::string_view data;
};

struct sse_options {
  bool no_cache = true;
  bool x_accel_buffering_no = true;
};

class stream_write_result {
public:
  static stream_write_result ready() noexcept;
  static stream_write_result backpressure() noexcept;
  static stream_write_result rejected(std::error_code error = {}) noexcept;

  [[nodiscard]] bool accepted() const noexcept { return accepted_; }
  [[nodiscard]] bool should_continue() const noexcept { return accepted_ && should_continue_; }
  [[nodiscard]] const std::error_code& error() const noexcept { return error_; }

  explicit operator bool() const noexcept { return should_continue(); }

private:
  stream_write_result(bool accepted, bool should_continue, std::error_code error) noexcept;

  bool accepted_ = false;
  bool should_continue_ = false;
  std::error_code error_;
};

class response {
public:
  response() = default;

  response& status(unsigned int code);
  response& status(http::status value);
  response& header(std::string_view name, std::string_view value);
  response& type(std::string_view content_type);

  void text(std::string_view body);
  void json(const char* serialized_json);
  void json(const std::string& serialized_json);
  void json(std::string_view serialized_json);
  void json(const uvp::json& value);
  void bytes(std::span<const std::byte> body);
  void end();
  [[nodiscard]] deferred_response defer();
  [[nodiscard]] streaming_response stream();
  [[nodiscard]] sse_stream sse(sse_options options = {});

  [[nodiscard]] unsigned int status_code() const noexcept;
  [[nodiscard]] const http::headers& headers() const noexcept;
  [[nodiscard]] std::string_view body() const noexcept;
  [[nodiscard]] bool ended() const noexcept;
  [[nodiscard]] bool deferred() const noexcept;
  [[nodiscard]] bool streaming() const noexcept;

private:
  friend class deferred_response;
  friend class streaming_response;
  friend class server;

  explicit response(std::shared_ptr<detail::response_state> state);

  void on_complete(std::function<void()> callback);
  void on_stream_write(std::function<stream_write_result(std::string)> callback);
  void on_stream_end(std::function<void()> callback);
  void notify_stream_drain();
  void notify_stream_error(std::error_code error);
  void reset();
  void cancel() noexcept;
  void commit_headers();
  void complete_stream();

  [[nodiscard]] detail::response_state& state();
  [[nodiscard]] const detail::response_state& state() const;

  std::shared_ptr<detail::response_state> state_;
};

class deferred_response {
public:
  deferred_response() = default;
  ~deferred_response() = default;

  deferred_response(deferred_response&&) noexcept = default;
  deferred_response& operator=(deferred_response&&) noexcept = default;

  deferred_response(const deferred_response&) = delete;
  deferred_response& operator=(const deferred_response&) = delete;

  [[nodiscard]] bool active() const noexcept;

  deferred_response& on_cancel(std::function<void()> callback);

  deferred_response& status(unsigned int code);
  deferred_response& status(http::status value);
  deferred_response& header(std::string_view name, std::string_view value);
  deferred_response& type(std::string_view content_type);

  [[nodiscard]] bool try_status(unsigned int code);
  [[nodiscard]] bool try_status(http::status value);
  [[nodiscard]] bool try_header(std::string_view name, std::string_view value);
  [[nodiscard]] bool try_type(std::string_view content_type);

  void text(std::string_view body);
  void json(const char* serialized_json);
  void json(const std::string& serialized_json);
  void json(std::string_view serialized_json);
  void json(const uvp::json& value);
  void bytes(std::span<const std::byte> body);
  void end();

  [[nodiscard]] bool try_text(std::string_view body);
  [[nodiscard]] bool try_json(const char* serialized_json);
  [[nodiscard]] bool try_json(const std::string& serialized_json);
  [[nodiscard]] bool try_json(std::string_view serialized_json);
  [[nodiscard]] bool try_json(const uvp::json& value);
  [[nodiscard]] bool try_bytes(std::span<const std::byte> body);
  [[nodiscard]] bool try_end();

private:
  friend class response;

  explicit deferred_response(std::weak_ptr<detail::response_state> state) noexcept;

  [[nodiscard]] std::shared_ptr<detail::response_state> lock_active() const noexcept;

  std::weak_ptr<detail::response_state> state_;
};

class streaming_response {
public:
  streaming_response() = default;
  ~streaming_response() = default;

  streaming_response(streaming_response&&) noexcept = default;
  streaming_response& operator=(streaming_response&&) noexcept = default;

  streaming_response(const streaming_response&) = delete;
  streaming_response& operator=(const streaming_response&) = delete;

  [[nodiscard]] bool active() const noexcept;

  streaming_response& on_cancel(std::function<void()> callback);
  streaming_response& on_drain(std::function<void()> callback);
  streaming_response& on_error(std::function<void(std::error_code)> callback);

  streaming_response& status(unsigned int code);
  streaming_response& status(http::status value);
  streaming_response& header(std::string_view name, std::string_view value);
  streaming_response& type(std::string_view content_type);

  [[nodiscard]] stream_write_result write(std::string_view chunk);
  [[nodiscard]] stream_write_result write(std::span<const std::byte> chunk);
  [[nodiscard]] stream_write_result write(std::string chunk);
  void end();

private:
  friend class response;
  friend class server;

  explicit streaming_response(std::weak_ptr<detail::response_state> state) noexcept;

  [[nodiscard]] std::shared_ptr<detail::response_state> lock_active() const noexcept;

  std::weak_ptr<detail::response_state> state_;
};

class sse_stream {
public:
  sse_stream() = default;
  ~sse_stream() = default;

  sse_stream(sse_stream&&) noexcept = default;
  sse_stream& operator=(sse_stream&&) noexcept = default;

  sse_stream(const sse_stream&) = delete;
  sse_stream& operator=(const sse_stream&) = delete;

  [[nodiscard]] bool active() const noexcept;

  sse_stream& on_cancel(std::function<void()> callback);
  sse_stream& on_drain(std::function<void()> callback);
  sse_stream& on_error(std::function<void(std::error_code)> callback);

  [[nodiscard]] stream_write_result retry(std::chrono::milliseconds value);
  [[nodiscard]] stream_write_result send(const sse_event& event);
  [[nodiscard]] stream_write_result comment(std::string_view value);
  void close();

private:
  friend class response;

  explicit sse_stream(streaming_response stream) noexcept;

  streaming_response stream_;
};

} // namespace uvp::http
