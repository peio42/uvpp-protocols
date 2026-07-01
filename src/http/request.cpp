#include <uvpp/protocols/http/request.hpp>

#include <utility>

namespace uvp::http {

namespace {

int hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

std::string decode_query_component(std::string_view component) {
  std::string decoded;
  decoded.reserve(component.size());

  for (std::size_t offset = 0; offset < component.size(); ++offset) {
    const auto value = component[offset];
    if (value == '+') {
      decoded.push_back(' ');
      continue;
    }

    if (value == '%' && offset + 2 < component.size()) {
      const auto high = hex_value(component[offset + 1]);
      const auto low = hex_value(component[offset + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        offset += 2;
        continue;
      }
    }

    decoded.push_back(value);
  }

  return decoded;
}

} // namespace

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

query_params::query_params(std::string_view raw_query) {
  std::size_t offset = 0;
  while (offset <= raw_query.size()) {
    const auto next = raw_query.find('&', offset);
    const auto segment_end = next == std::string_view::npos ? raw_query.size() : next;
    const auto segment = raw_query.substr(offset, segment_end - offset);

    if (!segment.empty()) {
      const auto separator = segment.find('=');
      const auto raw_name = separator == std::string_view::npos ? segment : segment.substr(0, separator);
      const auto raw_value =
        separator == std::string_view::npos ? std::string_view{} : segment.substr(separator + 1);

      auto name = decode_query_component(raw_name);
      auto value = decode_query_component(raw_value);
      bool appended = false;
      for (auto& entry : entries_) {
        if (entry.name == name) {
          entry.values.push_back(std::move(value));
          appended = true;
          break;
        }
      }
      if (!appended) {
        auto& entry = entries_.emplace_back();
        entry.name = std::move(name);
        entry.values.push_back(std::move(value));
      }
    }

    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }
}

bool query_params::contains(std::string_view name) const noexcept {
  return first(name).has_value();
}

std::optional<std::string_view> query_params::first(std::string_view name) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      return std::string_view(entry.values.front());
    }
  }
  return std::nullopt;
}

std::string_view query_params::get(std::string_view name, std::string_view fallback) const noexcept {
  if (const auto value = first(name)) {
    return *value;
  }
  return fallback;
}

std::span<const std::string> query_params::all(std::string_view name) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      return entry.values;
    }
  }
  return {};
}

request::request(
  http::method method,
  std::string target,
  std::string path,
  std::string query,
  http::headers headers,
  std::vector<std::byte> body,
  route_params params,
  http::connection_info connection,
  std::vector<std::string> decoded_path_segments)
    : method_(method),
      target_(std::move(target)),
      path_(std::move(path)),
      query_(std::move(query)),
      query_params_(query_),
      headers_(std::move(headers)),
      body_(std::move(body)),
      params_(std::move(params)),
      decoded_path_segments_(std::move(decoded_path_segments)),
      connection_(std::move(connection)) {}

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
