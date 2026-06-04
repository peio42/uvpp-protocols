#include <uvpp/protocols/http/request.hpp>

#include <utility>

namespace uvp::http {

namespace detail {

struct request_body_stream_state {
  bool active = true;
  bool paused = false;
  std::function<void(std::span<const std::byte>)> on_data;
  std::function<void()> on_end;
  std::function<void(std::error_code)> on_error;
  std::function<void()> on_pause;
  std::function<void()> on_resume;
};

} // namespace detail

request_body_stream::request_body_stream()
    : state_(std::make_shared<detail::request_body_stream_state>()) {}

request_body_stream::request_body_stream(std::shared_ptr<detail::request_body_stream_state> state) noexcept
    : state_(std::move(state)) {}

request_body_stream& request_body_stream::on_data(std::function<void(std::span<const std::byte>)> callback) {
  if (state_ && state_->active) {
    state_->on_data = std::move(callback);
  }
  return *this;
}

request_body_stream& request_body_stream::on_end(std::function<void()> callback) {
  if (state_ && state_->active) {
    state_->on_end = std::move(callback);
  }
  return *this;
}

request_body_stream& request_body_stream::on_error(std::function<void(std::error_code)> callback) {
  if (state_ && state_->active) {
    state_->on_error = std::move(callback);
  }
  return *this;
}

void request_body_stream::pause() {
  if (!state_ || !state_->active || state_->paused) {
    return;
  }
  state_->paused = true;
  if (state_->on_pause) {
    state_->on_pause();
  }
}

void request_body_stream::resume() {
  if (!state_ || !state_->active || !state_->paused) {
    return;
  }
  state_->paused = false;
  if (state_->on_resume) {
    state_->on_resume();
  }
}

bool request_body_stream::active() const noexcept {
  return state_ && state_->active;
}

bool request_body_stream::paused() const noexcept {
  return state_ && state_->paused;
}

void request_body_stream::emit_data(std::span<const std::byte> chunk) {
  if (!state_ || !state_->active || !state_->on_data) {
    return;
  }
  try {
    state_->on_data(chunk);
  } catch (...) {
  }
}

void request_body_stream::emit_end() {
  if (!state_ || !state_->active) {
    return;
  }
  state_->active = false;
  auto callback = std::move(state_->on_end);
  if (callback) {
    try {
      callback();
    } catch (...) {
    }
  }
}

void request_body_stream::emit_error(std::error_code error) {
  if (!state_ || !state_->active) {
    return;
  }
  state_->active = false;
  auto callback = std::move(state_->on_error);
  if (callback) {
    try {
      callback(std::move(error));
    } catch (...) {
    }
  }
}

void request_body_stream::cancel() noexcept {
  if (state_) {
    state_->active = false;
  }
}

void request_body_stream::on_pause_resume(std::function<void()> on_pause, std::function<void()> on_resume) {
  if (state_) {
    state_->on_pause = std::move(on_pause);
    state_->on_resume = std::move(on_resume);
  }
}

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
