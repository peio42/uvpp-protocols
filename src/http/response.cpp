#include <uvpp/protocols/http/response.hpp>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace uvp::http {

namespace detail {

struct response_state {
  unsigned int status_code = static_cast<unsigned int>(http::status::ok);
  http::headers headers;
  std::string body;
  bool ended = false;
  bool deferred = false;
  bool streaming = false;
  bool headers_committed = false;
  bool cancelled = false;
  std::function<void()> on_complete;
  std::function<void()> on_cancel;
  std::function<void()> on_drain;
  std::function<void(std::error_code)> on_error;
  std::function<stream_write_result(std::string)> on_stream_write;
  std::function<void()> on_stream_end;

  [[nodiscard]] bool active() const noexcept {
    return !ended && !cancelled;
  }

  void complete() {
    if (!active()) {
      return;
    }

    ended = true;
    deferred = false;

    auto callback = on_complete;
    if (callback) {
      callback();
    }
  }

  void cancel() noexcept {
    if (!active()) {
      return;
    }

    cancelled = true;
    deferred = false;

    auto callback = std::move(on_cancel);
    on_cancel = {};
    if (callback) {
      try {
        callback();
      } catch (...) {
      }
    }
  }
};

} // namespace detail

stream_write_result::stream_write_result(bool accepted, bool should_continue, std::error_code error) noexcept
    : accepted_(accepted), should_continue_(should_continue), error_(std::move(error)) {}

stream_write_result stream_write_result::ready() noexcept {
  return stream_write_result{true, true, {}};
}

stream_write_result stream_write_result::backpressure() noexcept {
  return stream_write_result{true, false, {}};
}

stream_write_result stream_write_result::rejected(std::error_code error) noexcept {
  return stream_write_result{false, false, std::move(error)};
}

response::response(std::shared_ptr<detail::response_state> state)
    : state_(std::move(state)) {}

detail::response_state& response::state() {
  if (!state_) {
    state_ = std::make_shared<detail::response_state>();
  }
  return *state_;
}

const detail::response_state& response::state() const {
  static const auto empty = detail::response_state{};
  return state_ ? *state_ : empty;
}

response& response::status(unsigned int code) {
  if (code < 100 || code > 999) {
    throw std::invalid_argument("HTTP status code must be between 100 and 999");
  }
  if (state().headers_committed) {
    throw std::logic_error("HTTP response headers are already committed");
  }
  state().status_code = code;
  return *this;
}

response& response::status(http::status value) {
  return status(static_cast<unsigned int>(value));
}

response& response::header(std::string_view name, std::string_view value) {
  if (state().headers_committed) {
    throw std::logic_error("HTTP response headers are already committed");
  }
  state().headers.set(name, value);
  return *this;
}

response& response::type(std::string_view content_type) {
  return header("content-type", content_type);
}

void response::text(std::string_view body) {
  if (state().streaming) {
    throw std::logic_error("HTTP response is streaming");
  }
  type("text/plain; charset=utf-8");
  state().body.assign(body);
  end();
}

void response::json(const char* serialized_json) {
  json(std::string_view{serialized_json});
}

void response::json(const std::string& serialized_json) {
  json(std::string_view{serialized_json});
}

void response::json(std::string_view serialized_json) {
  if (state().streaming) {
    throw std::logic_error("HTTP response is streaming");
  }
  type("application/json");
  state().body.assign(serialized_json);
  end();
}

void response::json(const uvp::json& value) {
  const auto serialized = value.dump();
  json(serialized);
}

void response::bytes(std::span<const std::byte> body) {
  if (state().streaming) {
    throw std::logic_error("HTTP response is streaming");
  }
  if (body.empty()) {
    state().body.clear();
    end();
    return;
  }

  state().body.assign(reinterpret_cast<const char*>(body.data()), body.size());
  end();
}

void response::end() {
  if (state().streaming) {
    throw std::logic_error("HTTP response is streaming");
  }
  state().complete();
}

deferred_response response::defer() {
  auto& current = state();
  if (current.streaming) {
    throw std::logic_error("HTTP response is streaming");
  }
  if (current.active()) {
    current.deferred = true;
  }
  return deferred_response{state_};
}

streaming_response response::stream() {
  auto& current = state();
  if (!current.active()) {
    throw std::logic_error("HTTP response is not active");
  }
  if (current.deferred || current.streaming) {
    throw std::logic_error("HTTP response has already been claimed");
  }
  current.streaming = true;
  return streaming_response{state_};
}

unsigned int response::status_code() const noexcept {
  return state().status_code;
}

const http::headers& response::headers() const noexcept {
  return state().headers;
}

std::string_view response::body() const noexcept {
  return state().body;
}

bool response::ended() const noexcept {
  return state().ended;
}

bool response::deferred() const noexcept {
  return state().deferred;
}

bool response::streaming() const noexcept {
  return state().streaming;
}

void response::on_complete(std::function<void()> callback) {
  state().on_complete = std::move(callback);
}

void response::on_stream_write(std::function<stream_write_result(std::string)> callback) {
  state().on_stream_write = std::move(callback);
}

void response::on_stream_end(std::function<void()> callback) {
  state().on_stream_end = std::move(callback);
}

void response::notify_stream_drain() {
  auto callback = state().on_drain;
  if (callback && state().active() && state().streaming) {
    try {
      callback();
    } catch (...) {
    }
  }
}

void response::notify_stream_error(std::error_code error) {
  auto callback = state().on_error;
  if (callback) {
    try {
      callback(std::move(error));
    } catch (...) {
    }
  }
}

void response::reset() {
  auto& current = state();
  current.status_code = static_cast<unsigned int>(http::status::ok);
  current.headers = {};
  current.body.clear();
  current.ended = false;
  current.deferred = false;
  current.streaming = false;
  current.headers_committed = false;
  current.cancelled = false;
}

void response::cancel() noexcept {
  if (state_) {
    state_->cancel();
  }
}

void response::commit_headers() {
  state().headers_committed = true;
}

void response::complete_stream() {
  auto& current = state();
  if (!current.streaming) {
    return;
  }
  current.streaming = false;
  current.complete();
}

deferred_response::deferred_response(std::weak_ptr<detail::response_state> state) noexcept
    : state_(std::move(state)) {}

std::shared_ptr<detail::response_state> deferred_response::lock_active() const noexcept {
  auto state = state_.lock();
  if (!state || !state->active()) {
    return {};
  }
  return state;
}

bool deferred_response::active() const noexcept {
  return static_cast<bool>(lock_active());
}

deferred_response& deferred_response::on_cancel(std::function<void()> callback) {
  if (auto state = lock_active()) {
    state->on_cancel = std::move(callback);
  }
  return *this;
}

deferred_response& deferred_response::status(unsigned int code) {
  if (auto state = lock_active()) {
    response{std::move(state)}.status(code);
  }
  return *this;
}

deferred_response& deferred_response::status(http::status value) {
  return status(static_cast<unsigned int>(value));
}

deferred_response& deferred_response::header(std::string_view name, std::string_view value) {
  if (auto state = lock_active()) {
    response{std::move(state)}.header(name, value);
  }
  return *this;
}

deferred_response& deferred_response::type(std::string_view content_type) {
  return header("content-type", content_type);
}

void deferred_response::text(std::string_view body) {
  if (auto state = lock_active()) {
    response{std::move(state)}.text(body);
  }
}

void deferred_response::json(const char* serialized_json) {
  if (auto state = lock_active()) {
    response{std::move(state)}.json(serialized_json);
  }
}

void deferred_response::json(const std::string& serialized_json) {
  if (auto state = lock_active()) {
    response{std::move(state)}.json(serialized_json);
  }
}

void deferred_response::json(std::string_view serialized_json) {
  if (auto state = lock_active()) {
    response{std::move(state)}.json(serialized_json);
  }
}

void deferred_response::json(const uvp::json& value) {
  if (auto state = lock_active()) {
    response{std::move(state)}.json(value);
  }
}

void deferred_response::bytes(std::span<const std::byte> body) {
  if (auto state = lock_active()) {
    response{std::move(state)}.bytes(body);
  }
}

void deferred_response::end() {
  if (auto state = lock_active()) {
    response{std::move(state)}.end();
  }
}

streaming_response::streaming_response(std::weak_ptr<detail::response_state> state) noexcept
    : state_(std::move(state)) {}

std::shared_ptr<detail::response_state> streaming_response::lock_active() const noexcept {
  auto state = state_.lock();
  if (!state || !state->active() || !state->streaming) {
    return {};
  }
  return state;
}

bool streaming_response::active() const noexcept {
  return static_cast<bool>(lock_active());
}

streaming_response& streaming_response::on_cancel(std::function<void()> callback) {
  if (auto state = lock_active()) {
    state->on_cancel = std::move(callback);
  }
  return *this;
}

streaming_response& streaming_response::on_drain(std::function<void()> callback) {
  if (auto state = lock_active()) {
    state->on_drain = std::move(callback);
  }
  return *this;
}

streaming_response& streaming_response::on_error(std::function<void(std::error_code)> callback) {
  if (auto state = lock_active()) {
    state->on_error = std::move(callback);
  }
  return *this;
}

streaming_response& streaming_response::status(unsigned int code) {
  if (auto state = lock_active()) {
    response{std::move(state)}.status(code);
  }
  return *this;
}

streaming_response& streaming_response::status(http::status value) {
  return status(static_cast<unsigned int>(value));
}

streaming_response& streaming_response::header(std::string_view name, std::string_view value) {
  if (auto state = lock_active()) {
    response{std::move(state)}.header(name, value);
  }
  return *this;
}

streaming_response& streaming_response::type(std::string_view content_type) {
  return header("content-type", content_type);
}

stream_write_result streaming_response::write(std::string_view chunk) {
  return write(std::string(chunk));
}

stream_write_result streaming_response::write(std::span<const std::byte> chunk) {
  std::string payload;
  payload.resize(chunk.size());
  if (!chunk.empty()) {
    std::memcpy(payload.data(), chunk.data(), chunk.size());
  }
  return write(std::move(payload));
}

stream_write_result streaming_response::write(std::string chunk) {
  auto state = lock_active();
  if (!state) {
    return stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
  }
  if (!state->on_stream_write) {
    return stream_write_result::rejected(std::make_error_code(std::errc::operation_not_supported));
  }
  return state->on_stream_write(std::move(chunk));
}

void streaming_response::end() {
  auto state = lock_active();
  if (!state) {
    return;
  }
  if (state->on_stream_end) {
    state->on_stream_end();
  }
}

} // namespace uvp::http
