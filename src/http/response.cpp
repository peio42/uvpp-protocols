#include <uvpp/protocols/http/response.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

namespace uvp::http {

namespace detail {

struct response_state {
  unsigned int status_code = static_cast<unsigned int>(http::status::ok);
  http::headers headers;
  std::string body;
  bool ended = false;
  bool deferred = false;
  bool cancelled = false;
  std::function<void()> on_complete;
  std::function<void()> on_cancel;

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

namespace {

std::string escape_json_string(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);

  for (char ch : value) {
    switch (ch) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }

  return escaped;
}

} // namespace

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
  state().status_code = code;
  return *this;
}

response& response::status(http::status value) {
  return status(static_cast<unsigned int>(value));
}

response& response::header(std::string_view name, std::string_view value) {
  state().headers.set(name, value);
  return *this;
}

response& response::type(std::string_view content_type) {
  return header("content-type", content_type);
}

void response::text(std::string_view body) {
  type("text/plain; charset=utf-8");
  state().body.assign(body);
  end();
}

void response::json(std::string_view serialized_json) {
  type("application/json");
  state().body.assign(serialized_json);
  end();
}

void response::json(std::initializer_list<std::pair<std::string_view, std::string_view>> object) {
  type("application/json");

  auto& storage = state().body;
  storage = "{";
  bool first = true;
  for (const auto& [key, value] : object) {
    if (!first) {
      storage += ",";
    }
    first = false;
    storage += '"';
    storage += escape_json_string(key);
    storage += "\":\"";
    storage += escape_json_string(value);
    storage += '"';
  }
  storage += "}";

  end();
}

void response::bytes(std::span<const std::byte> body) {
  if (body.empty()) {
    state().body.clear();
    end();
    return;
  }

  state().body.assign(reinterpret_cast<const char*>(body.data()), body.size());
  end();
}

void response::end() {
  state().complete();
}

deferred_response response::defer() {
  auto& current = state();
  if (current.active()) {
    current.deferred = true;
  }
  return deferred_response{state_};
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

void response::on_complete(std::function<void()> callback) {
  state().on_complete = std::move(callback);
}

void response::reset() {
  auto& current = state();
  current.status_code = static_cast<unsigned int>(http::status::ok);
  current.headers = {};
  current.body.clear();
  current.ended = false;
  current.deferred = false;
  current.cancelled = false;
}

void response::cancel() noexcept {
  if (state_) {
    state_->cancel();
  }
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

void deferred_response::json(std::string_view serialized_json) {
  if (auto state = lock_active()) {
    response{std::move(state)}.json(serialized_json);
  }
}

void deferred_response::json(std::initializer_list<std::pair<std::string_view, std::string_view>> object) {
  if (auto state = lock_active()) {
    response{std::move(state)}.json(object);
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

} // namespace uvp::http
