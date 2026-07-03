#include <uvpp/protocols/http/multipart.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace uvp::http {

namespace {

constexpr std::size_t default_part_header_bytes = 16 * 1024;

uvp::error make_error(errc code, std::string detail = {}) {
  auto ec = make_error_code(code);
  if (detail.empty()) {
    detail = ec.message();
  }
  return uvp::error{ec, std::move(detail)};
}

std::string_view trim_ows(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto left = static_cast<unsigned char>(lhs[index]);
    const auto right = static_cast<unsigned char>(rhs[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

bool contains_ctl(std::string_view value) noexcept {
  for (char ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte == 0 || byte == '\r' || byte == '\n' || byte < 0x20 || byte == 0x7f) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> parse_quoted_value(std::string_view value) {
  if (value.size() < 2 || value.front() != '"') {
    return std::nullopt;
  }

  std::string out;
  bool escaped = false;
  for (std::size_t index = 1; index < value.size(); ++index) {
    const auto ch = value[index];
    if (escaped) {
      if (ch == '\r' || ch == '\n' || ch == '\0') {
        return std::nullopt;
      }
      out.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      if (!trim_ows(value.substr(index + 1)).empty()) {
        return std::nullopt;
      }
      return out;
    }
    if (ch == '\r' || ch == '\n' || ch == '\0') {
      return std::nullopt;
    }
    out.push_back(ch);
  }

  return std::nullopt;
}

std::optional<std::string> parse_parameter_value(std::string_view value) {
  value = trim_ows(value);
  if (value.empty()) {
    return std::nullopt;
  }
  if (value.front() == '"') {
    return parse_quoted_value(value);
  }
  if (contains_ctl(value) || value.find('"') != std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(value);
}

bool valid_boundary(std::string_view boundary) noexcept {
  return !boundary.empty() && boundary.size() <= 70 && !contains_ctl(boundary);
}

uvp::result<std::string> boundary_from_content_type(std::string_view content_type) {
  if (content_type.empty()) {
    return make_error(errc::unsupported_media_type, "missing content-type");
  }

  const auto separator = content_type.find(';');
  const auto media_type = trim_ows(content_type.substr(0, separator));
  if (!ascii_iequals(media_type, "multipart/form-data")) {
    return make_error(errc::unsupported_media_type, "expected multipart/form-data");
  }

  bool saw_boundary = false;
  std::string boundary;
  std::size_t offset = separator == std::string_view::npos ? content_type.size() : separator + 1;
  while (offset < content_type.size()) {
    const auto next = content_type.find(';', offset);
    const auto raw_param = content_type.substr(
      offset,
      next == std::string_view::npos ? std::string_view::npos : next - offset);
    const auto equals = raw_param.find('=');
    if (equals == std::string_view::npos) {
      if (!trim_ows(raw_param).empty()) {
        return make_error(errc::malformed_content_type);
      }
    } else {
      const auto name = trim_ows(raw_param.substr(0, equals));
      const auto value = parse_parameter_value(raw_param.substr(equals + 1));
      if (!value) {
        return make_error(errc::malformed_content_type);
      }
      if (ascii_iequals(name, "boundary")) {
        if (saw_boundary) {
          return make_error(errc::malformed_content_type, "duplicate boundary parameter");
        }
        saw_boundary = true;
        boundary = *value;
      }
    }
    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }

  if (!saw_boundary) {
    return make_error(errc::multipart_missing_boundary);
  }
  if (!valid_boundary(boundary)) {
    return make_error(errc::multipart_invalid_boundary);
  }
  return boundary;
}

std::span<const std::byte> as_bytes(std::string_view value) noexcept {
  return std::as_bytes(std::span{value.data(), value.size()});
}

struct disposition_info {
  std::string name;
  std::optional<std::string> filename;
};

uvp::result<disposition_info> parse_content_disposition(std::string_view value) {
  const auto first_separator = value.find(';');
  const auto disposition = trim_ows(value.substr(0, first_separator));
  if (!ascii_iequals(disposition, "form-data")) {
    return make_error(errc::multipart_malformed_part_header, "content-disposition must be form-data");
  }

  bool saw_name = false;
  bool saw_filename = false;
  disposition_info info;
  std::size_t offset = first_separator == std::string_view::npos ? value.size() : first_separator + 1;
  while (offset < value.size()) {
    const auto next = value.find(';', offset);
    const auto raw_param = value.substr(
      offset,
      next == std::string_view::npos ? std::string_view::npos : next - offset);
    const auto equals = raw_param.find('=');
    if (equals == std::string_view::npos) {
      if (!trim_ows(raw_param).empty()) {
        return make_error(errc::multipart_malformed_part_header);
      }
    } else {
      const auto name = trim_ows(raw_param.substr(0, equals));
      const auto parsed_value = parse_parameter_value(raw_param.substr(equals + 1));
      if (!parsed_value) {
        return make_error(errc::multipart_malformed_part_header);
      }
      if (ascii_iequals(name, "name")) {
        if (saw_name) {
          return make_error(errc::multipart_malformed_part_header, "duplicate name parameter");
        }
        saw_name = true;
        info.name = *parsed_value;
      } else if (ascii_iequals(name, "filename")) {
        if (saw_filename) {
          return make_error(errc::multipart_malformed_part_header, "duplicate filename parameter");
        }
        saw_filename = true;
        info.filename = *parsed_value;
      }
    }
    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }

  if (!saw_name || info.name.empty() || contains_ctl(info.name)) {
    return make_error(errc::multipart_malformed_part_header, "missing or invalid name parameter");
  }
  if (info.filename && contains_ctl(*info.filename)) {
    return make_error(errc::multipart_malformed_part_header, "invalid filename parameter");
  }
  return info;
}

} // namespace

namespace detail {

enum class multipart_part_mode {
  none,
  stream,
  text,
  discard,
};

struct multipart_part_state {
  std::string name;
  std::optional<std::string> filename;
  http::headers headers;
  multipart_part_mode mode = multipart_part_mode::none;
  bool ended = false;
  bool paused = false;
  std::function<void(std::span<const std::byte>)> on_data;
  std::function<void()> on_end;
  std::function<void(uvp::error)> on_error;
  std::function<void(uvp::result<std::string>)> on_text;
  std::string text_buffer;
  std::size_t max_text_bytes = 0;
};

enum class multipart_parser_phase {
  first_boundary,
  headers,
  body,
  done,
  failed,
};

struct multipart_stream_state {
  request_body_stream* body = nullptr;
  std::string boundary;
  std::string first_delimiter;
  std::string next_delimiter;
  std::string buffer;
  multipart_parser_phase phase = multipart_parser_phase::first_boundary;
  std::shared_ptr<multipart_part_state> current_part;
  std::function<void(multipart_part&)> on_part;
  std::function<void()> on_end;
  std::function<void(uvp::error)> on_error;
  uvp::error construction_error;
  bool valid = false;
  bool failed = false;

  void fail(uvp::error error) {
    if (failed || phase == multipart_parser_phase::done) {
      return;
    }
    failed = true;
    phase = multipart_parser_phase::failed;
    if (current_part) {
      multipart_part{current_part}.emit_error(error);
      current_part.reset();
    }
    if (on_error) {
      on_error(std::move(error));
    }
  }

  void emit_current_data(std::string_view value) {
    if (!value.empty() && current_part) {
      multipart_part{current_part}.emit_data(as_bytes(value));
    }
  }

  void end_current_part() {
    if (current_part) {
      multipart_part{current_part}.emit_end();
      current_part.reset();
    }
  }

  void parse_part_headers(std::string block) {
    http::headers parsed_headers;
    std::optional<std::string> content_disposition;
    bool saw_content_type = false;

    std::size_t offset = 0;
    while (offset < block.size()) {
      const auto line_end = block.find("\r\n", offset);
      const auto line = std::string_view(block).substr(
        offset,
        line_end == std::string::npos ? std::string_view::npos : line_end - offset);
      if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        fail(make_error(errc::multipart_malformed_part_header, "folded part headers are not supported"));
        return;
      }

      const auto colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        fail(make_error(errc::multipart_malformed_part_header));
        return;
      }
      const auto name = trim_ows(line.substr(0, colon));
      const auto value = trim_ows(line.substr(colon + 1));
      if (name.empty() || contains_ctl(name)) {
        fail(make_error(errc::multipart_malformed_part_header));
        return;
      }
      if (ascii_iequals(name, "content-disposition")) {
        if (content_disposition) {
          fail(make_error(errc::multipart_malformed_part_header, "duplicate content-disposition"));
          return;
        }
        content_disposition = std::string(value);
      } else if (ascii_iequals(name, "content-type")) {
        if (saw_content_type) {
          fail(make_error(errc::multipart_malformed_part_header, "duplicate content-type"));
          return;
        }
        saw_content_type = true;
      } else if (ascii_iequals(name, "content-transfer-encoding") &&
                 !ascii_iequals(value, "7bit") &&
                 !ascii_iequals(value, "8bit") &&
                 !ascii_iequals(value, "binary")) {
        fail(make_error(errc::multipart_malformed_part_header, "unsupported content-transfer-encoding"));
        return;
      }
      parsed_headers.add(name, value);

      if (line_end == std::string::npos) {
        break;
      }
      offset = line_end + 2;
    }

    if (!content_disposition) {
      fail(make_error(errc::multipart_malformed_part_header, "missing content-disposition"));
      return;
    }

    auto disposition = parse_content_disposition(*content_disposition);
    if (!disposition) {
      fail(disposition.error());
      return;
    }

    current_part = std::make_shared<multipart_part_state>();
    current_part->name = std::move(disposition.value().name);
    current_part->filename = std::move(disposition.value().filename);
    current_part->headers = std::move(parsed_headers);

    if (!on_part) {
      fail(make_error(errc::multipart_malformed_body, "multipart part has no handler"));
      return;
    }

    multipart_part part{current_part};
    on_part(part);
    if (!part.consumed()) {
      fail(make_error(errc::multipart_malformed_body, "multipart part was not consumed"));
      return;
    }
    phase = multipart_parser_phase::body;
  }

  void parse() {
    while (!failed) {
      if (phase == multipart_parser_phase::first_boundary) {
        if (buffer.size() < first_delimiter.size() + 2) {
          return;
        }
        if (buffer.rfind(first_delimiter, 0) != 0) {
          fail(make_error(errc::multipart_malformed_body, "multipart body must start with boundary"));
          return;
        }
        const auto after = first_delimiter.size();
        if (buffer.compare(after, 2, "--") == 0) {
          buffer.erase(0, after + 2);
          phase = multipart_parser_phase::done;
          if (on_end) {
            on_end();
          }
          return;
        }
        if (buffer.compare(after, 2, "\r\n") != 0) {
          fail(make_error(errc::multipart_malformed_body, "invalid multipart boundary delimiter"));
          return;
        }
        buffer.erase(0, after + 2);
        phase = multipart_parser_phase::headers;
        continue;
      }

      if (phase == multipart_parser_phase::headers) {
        const auto header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
          if (buffer.size() > default_part_header_bytes) {
            fail(make_error(errc::multipart_limit_exceeded, "multipart part headers are too large"));
          }
          return;
        }
        auto block = buffer.substr(0, header_end);
        buffer.erase(0, header_end + 4);
        parse_part_headers(std::move(block));
        continue;
      }

      if (phase == multipart_parser_phase::body) {
        const auto delimiter = buffer.find(next_delimiter);
        if (delimiter == std::string::npos) {
          const auto keep = next_delimiter.size();
          if (buffer.size() > keep) {
            const auto emit_size = buffer.size() - keep;
            emit_current_data(std::string_view(buffer.data(), emit_size));
            buffer.erase(0, emit_size);
          }
          return;
        }

        const auto after = delimiter + next_delimiter.size();
        if (buffer.size() < after + 2) {
          return;
        }
        const auto suffix = buffer.substr(after, 2);
        if (suffix != "\r\n" && suffix != "--") {
          fail(make_error(errc::multipart_malformed_body, "invalid multipart delimiter suffix"));
          return;
        }

        emit_current_data(std::string_view(buffer.data(), delimiter));
        end_current_part();
        buffer.erase(0, after + 2);
        if (suffix == "--") {
          if (buffer.rfind("\r\n", 0) == 0) {
            buffer.erase(0, 2);
          }
          phase = multipart_parser_phase::done;
          if (on_end) {
            on_end();
          }
          return;
        }

        phase = multipart_parser_phase::headers;
        continue;
      }

      return;
    }
  }

  void finish() {
    if (failed) {
      return;
    }
    parse();
    if (failed) {
      return;
    }
    if (phase != multipart_parser_phase::done) {
      fail(make_error(errc::multipart_unexpected_end));
    }
  }
};

} // namespace detail

multipart_part_stream::multipart_part_stream(std::shared_ptr<detail::multipart_part_state> state) noexcept
    : state_(std::move(state)) {}

multipart_part_stream& multipart_part_stream::on_data(std::function<void(std::span<const std::byte>)> callback) {
  if (state_) {
    state_->on_data = std::move(callback);
  }
  return *this;
}

multipart_part_stream& multipart_part_stream::on_end(std::function<void()> callback) {
  if (state_) {
    state_->on_end = std::move(callback);
  }
  return *this;
}

multipart_part_stream& multipart_part_stream::on_error(std::function<void(uvp::error)> callback) {
  if (state_) {
    state_->on_error = std::move(callback);
  }
  return *this;
}

void multipart_part_stream::pause() {
  if (state_) {
    state_->paused = true;
  }
}

void multipart_part_stream::resume() {
  if (state_) {
    state_->paused = false;
  }
}

void multipart_part_stream::emit_data(std::span<const std::byte> chunk) {
  if (state_ && state_->on_data) {
    state_->on_data(chunk);
  }
}

void multipart_part_stream::emit_end() {
  if (state_ && state_->on_end) {
    state_->on_end();
  }
}

void multipart_part_stream::emit_error(uvp::error error) {
  if (state_ && state_->on_error) {
    state_->on_error(std::move(error));
  }
}

multipart_part::multipart_part(std::shared_ptr<detail::multipart_part_state> state) noexcept
    : state_(std::move(state)), stream_(state_) {}

std::string_view multipart_part::name() const noexcept {
  return state_ ? std::string_view(state_->name) : std::string_view{};
}

std::optional<std::string_view> multipart_part::filename() const noexcept {
  if (!state_ || !state_->filename) {
    return std::nullopt;
  }
  return std::string_view(*state_->filename);
}

std::string multipart_part::safe_filename() const {
  if (!state_ || !state_->filename) {
    return {};
  }
  std::string out;
  for (const auto ch : *state_->filename) {
    const auto byte = static_cast<unsigned char>(ch);
    if (ch == '/' || ch == '\\' || ch == ':' || byte < 0x20 || byte == 0x7f) {
      continue;
    }
    out.push_back(ch);
  }
  return out.empty() ? std::string{"upload"} : out;
}

const http::headers& multipart_part::headers() const noexcept {
  static const http::headers empty;
  return state_ ? state_->headers : empty;
}

multipart_part_stream& multipart_part::stream() {
  if (state_ && state_->mode == detail::multipart_part_mode::none) {
    state_->mode = detail::multipart_part_mode::stream;
  }
  return stream_;
}

void multipart_part::text(std::size_t max_bytes, std::function<void(uvp::result<std::string>)> callback) {
  if (!state_ || state_->mode != detail::multipart_part_mode::none) {
    return;
  }
  state_->mode = detail::multipart_part_mode::text;
  state_->max_text_bytes = max_bytes;
  state_->on_text = std::move(callback);
}

void multipart_part::discard() {
  if (state_ && state_->mode == detail::multipart_part_mode::none) {
    state_->mode = detail::multipart_part_mode::discard;
  }
}

void multipart_part::pause() {
  stream_.pause();
}

void multipart_part::resume() {
  stream_.resume();
}

bool multipart_part::consumed() const noexcept {
  return state_ && state_->mode != detail::multipart_part_mode::none;
}

void multipart_part::emit_data(std::span<const std::byte> chunk) {
  if (!state_ || chunk.empty()) {
    return;
  }
  if (state_->mode == detail::multipart_part_mode::stream) {
    stream_.emit_data(chunk);
    return;
  }
  if (state_->mode == detail::multipart_part_mode::text) {
    if (state_->text_buffer.size() + chunk.size() > state_->max_text_bytes) {
      emit_error(make_error(errc::multipart_limit_exceeded, "multipart field is too large"));
      return;
    }
    state_->text_buffer.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
  }
}

void multipart_part::emit_end() {
  if (!state_ || state_->ended) {
    return;
  }
  state_->ended = true;
  if (state_->mode == detail::multipart_part_mode::stream) {
    stream_.emit_end();
  } else if (state_->mode == detail::multipart_part_mode::text && state_->on_text) {
    state_->on_text(std::move(state_->text_buffer));
  }
}

void multipart_part::emit_error(uvp::error error) {
  if (!state_ || state_->ended) {
    return;
  }
  state_->ended = true;
  if (state_->mode == detail::multipart_part_mode::stream) {
    stream_.emit_error(error);
  } else if (state_->mode == detail::multipart_part_mode::text && state_->on_text) {
    state_->on_text(std::move(error));
  }
}

multipart_stream::multipart_stream()
    : state_(std::make_shared<detail::multipart_stream_state>()) {}

multipart_stream::multipart_stream(request_body_stream& body, std::string_view content_type)
    : state_(std::make_shared<detail::multipart_stream_state>()) {
  auto boundary = boundary_from_content_type(content_type);
  if (!boundary) {
    state_->construction_error = boundary.error();
    return;
  }

  state_->body = &body;
  state_->boundary = std::move(boundary.value());
  state_->first_delimiter = "--" + state_->boundary;
  state_->next_delimiter = "\r\n--" + state_->boundary;
  state_->valid = true;

  auto state = state_;
  body.on_data([state](std::span<const std::byte> chunk) {
    state->buffer.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
    state->parse();
  });
  body.on_end([state] {
    state->finish();
  });
  body.on_error([state](std::error_code code) {
    state->fail(uvp::error{std::move(code), "request body stream failed"});
  });
}

multipart_stream::~multipart_stream() = default;

multipart_stream& multipart_stream::on_part(std::function<void(multipart_part&)> callback) {
  if (state_) {
    state_->on_part = std::move(callback);
  }
  return *this;
}

multipart_stream& multipart_stream::on_end(std::function<void()> callback) {
  if (state_) {
    state_->on_end = std::move(callback);
  }
  return *this;
}

multipart_stream& multipart_stream::on_error(std::function<void(uvp::error)> callback) {
  if (state_) {
    state_->on_error = std::move(callback);
  }
  return *this;
}

bool multipart_stream::valid() const noexcept {
  return state_ && state_->valid;
}

const uvp::error& multipart_stream::error() const noexcept {
  static const auto empty = uvp::error{};
  return state_ ? state_->construction_error : empty;
}

bool multipart_stream::has_error_handler() const noexcept {
  return state_ && static_cast<bool>(state_->on_error);
}

} // namespace uvp::http
