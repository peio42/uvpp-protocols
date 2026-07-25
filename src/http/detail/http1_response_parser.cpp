#include "http1_response_parser.hpp"

#include <utility>

#include <llhttp.h>

namespace uvp::http::detail {

namespace {

[[nodiscard]] std::string trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return std::string{value};
}

} // namespace

class http1_response_parser::impl {
public:
  impl() {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &impl::on_message_begin;
    settings_.on_protocol = &impl::on_header_prefix;
    settings_.on_version = &impl::on_header_prefix;
    settings_.on_status = &impl::on_header_prefix;
    settings_.on_header_field = &impl::on_header_field;
    settings_.on_header_value = &impl::on_header_value;
    settings_.on_headers_complete = &impl::on_headers_complete;
    settings_.on_body = &impl::on_body;
    settings_.on_message_complete = &impl::on_message_complete;
    llhttp_init(&parser_, HTTP_RESPONSE, &settings_);
    parser_.data = this;
  }

  void reset(http::method request_method) {
    current_ = http1_response_message{};
    pending_header_field_.clear();
    pending_header_value_.clear();
    header_bytes_ = 0;
    header_count_ = 0;
    body_bytes_ = 0;
    limit_error_.clear();
    last_header_part_ = header_part::none;
    final_response_ = false;
    complete_ = false;
    unsupported_upgrade_ = false;
    request_method_ = request_method;
    event_handler_ = nullptr;

    llhttp_reset(&parser_);
    parser_.data = this;
  }

  void limits(http1_response_limits value) {
    limits_ = value;
  }

  http1_response_parse_result parse(std::string_view bytes, const http1_response_event_handler& on_event) {
    event_handler_ = &on_event;
    const auto err = llhttp_execute(&parser_, bytes.data(), bytes.size());
    event_handler_ = nullptr;
    return result(err, bytes);
  }

  http1_response_parse_result finish(const http1_response_event_handler& on_event) {
    event_handler_ = &on_event;
    const auto err = llhttp_finish(&parser_);
    event_handler_ = nullptr;
    return result(err, {});
  }

private:
  enum class header_part {
    none,
    field,
    value,
  };

  static impl& self(llhttp_t* parser) noexcept {
    return *static_cast<impl*>(parser->data);
  }

  static int on_message_begin(llhttp_t* parser) {
    auto& self = impl::self(parser);
    self.current_ = http1_response_message{};
    self.pending_header_field_.clear();
    self.pending_header_value_.clear();
    self.header_bytes_ = 0;
    self.header_count_ = 0;
    self.body_bytes_ = 0;
    self.limit_error_.clear();
    self.last_header_part_ = header_part::none;
    self.final_response_ = false;
    return HPE_OK;
  }

  static int on_header_prefix(llhttp_t* parser, const char*, std::size_t length) {
    auto& self = impl::self(parser);
    return self.reserve_header_bytes(length) ? HPE_OK : HPE_USER;
  }

  static int on_header_field(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    if (self.last_header_part_ == header_part::value && !self.commit_pending_header(parser)) {
      return HPE_USER;
    }
    if (!self.reserve_header_bytes(length)) {
      return HPE_USER;
    }
    self.pending_header_field_.append(at, length);
    self.last_header_part_ = header_part::field;
    return HPE_OK;
  }

  static int on_header_value(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    if (!self.reserve_header_bytes(length)) {
      return HPE_USER;
    }
    self.pending_header_value_.append(at, length);
    self.last_header_part_ = header_part::value;
    return HPE_OK;
  }

  static int on_headers_complete(llhttp_t* parser) {
    auto& self = impl::self(parser);
    if (!self.commit_pending_header(parser)) {
      return HPE_USER;
    }
    self.current_.head.status_code = static_cast<unsigned int>(llhttp_get_status_code(parser));
    self.current_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    self.final_response_ = self.current_.head.status_code < 100 || self.current_.head.status_code >= 200;
    if (!self.final_response_) {
      return HPE_OK;
    }
    if (self.request_method_ == http::method::connect &&
        self.current_.head.status_code >= 200 && self.current_.head.status_code < 300) {
      self.unsupported_upgrade_ = true;
      return HPE_PAUSED;
    }
    if (!self.emit(http1_response_event::headers(self.current_.head))) {
      return HPE_PAUSED;
    }
    // llhttp does not expose a setter for the request method used while
    // parsing a response. Enforce the HEAD rule explicitly at the framing
    // boundary, before it considers a response body.
    return self.request_method_ == http::method::head ? 1 : HPE_OK;
  }

  static int on_body(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    if (length > self.limits_.max_body_bytes || self.body_bytes_ > self.limits_.max_body_bytes - length) {
      self.limit_error_ = "response body is too large";
      self.limit_kind_ = http1_response_parse_result::error_kind::body_limit;
      return HPE_USER;
    }
    self.body_bytes_ += length;
    return self.emit(http1_response_event::body(std::string_view{at, length})) ? HPE_OK : HPE_PAUSED;
  }

  static int on_message_complete(llhttp_t* parser) {
    auto& self = impl::self(parser);
    if (!self.commit_pending_header(parser)) {
      return HPE_USER;
    }
    if (!self.final_response_) {
      return HPE_OK;
    }
    self.current_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    self.complete_ = true;
    if (!self.emit(http1_response_event::complete(self.current_))) {
      return HPE_PAUSED;
    }
    // The client accepts one final response at a time. Stopping here lets the
    // caller refuse pooling when the transport read contained extra bytes.
    return HPE_PAUSED;
  }

  [[nodiscard]] bool reserve_header_bytes(std::size_t length) {
    if (length > limits_.max_header_bytes || header_bytes_ > limits_.max_header_bytes - length) {
      limit_error_ = "response headers are too large";
      limit_kind_ = http1_response_parse_result::error_kind::header_limit;
      return false;
    }
    header_bytes_ += length;
    return true;
  }

  [[nodiscard]] bool commit_pending_header(llhttp_t* parser) {
    if (!pending_header_field_.empty()) {
      if (header_count_ >= limits_.max_header_count) {
        limit_error_ = "response has too many headers";
        limit_kind_ = http1_response_parse_result::error_kind::header_limit;
        return false;
      }
      if (!http::headers::is_valid_name(pending_header_field_) ||
          !http::headers::is_valid_value(pending_header_value_)) {
        limit_error_ = "response header is invalid";
        return false;
      }
      if ((parser->flags & F_TRAILING) == 0) {
        current_.head.headers.add(pending_header_field_, trim(pending_header_value_));
      }
      ++header_count_;
      pending_header_field_.clear();
      pending_header_value_.clear();
    }
    last_header_part_ = header_part::none;
    return true;
  }

  [[nodiscard]] bool emit(const http1_response_event& event) const {
    return event_handler_ && (*event_handler_)(event);
  }

  [[nodiscard]] std::size_t parsed_bytes(std::string_view bytes) const noexcept {
    const auto* error_pos = llhttp_get_error_pos(&parser_);
    if (error_pos >= bytes.data() && error_pos <= bytes.data() + bytes.size()) {
      return static_cast<std::size_t>(error_pos - bytes.data());
    }
    return bytes.size();
  }

  [[nodiscard]] http1_response_parse_result result(llhttp_errno_t err, std::string_view bytes) const {
    if (unsupported_upgrade_) {
      return {http1_response_parse_result::status::upgrade, {}, {}, parsed_bytes(bytes)};
    }
    if (complete_ && err == HPE_PAUSED) {
      return {http1_response_parse_result::status::complete, {}, {}, parsed_bytes(bytes)};
    }
    if (err == HPE_OK) {
      return {http1_response_parse_result::status::ok, {}, {}, bytes.size()};
    }
    if (err == HPE_PAUSED_UPGRADE) {
      return {http1_response_parse_result::status::upgrade, {}, {}, parsed_bytes(bytes)};
    }
    if (err == HPE_PAUSED) {
      return {http1_response_parse_result::status::paused, {}, {}, parsed_bytes(bytes)};
    }
    if (!limit_error_.empty()) {
      return {http1_response_parse_result::status::error, limit_kind_, limit_error_, parsed_bytes(bytes)};
    }
    return {
      http1_response_parse_result::status::error,
      http1_response_parse_result::error_kind::malformed,
      std::string{llhttp_errno_name(err)},
      parsed_bytes(bytes),
    };
  }

  llhttp_t parser_{};
  llhttp_settings_t settings_{};
  http1_response_limits limits_;
  http1_response_message current_;
  const http1_response_event_handler* event_handler_ = nullptr;
  std::string pending_header_field_;
  std::string pending_header_value_;
  std::size_t header_bytes_ = 0;
  std::size_t header_count_ = 0;
  std::size_t body_bytes_ = 0;
  std::string limit_error_;
  http1_response_parse_result::error_kind limit_kind_ = http1_response_parse_result::error_kind::malformed;
  header_part last_header_part_ = header_part::none;
  bool final_response_ = false;
  bool complete_ = false;
  bool unsupported_upgrade_ = false;
  http::method request_method_ = http::method::get;
};

http1_response_parser::http1_response_parser()
    : impl_(std::make_unique<impl>()) {}

http1_response_parser::~http1_response_parser() = default;

http1_response_parser::http1_response_parser(http1_response_parser&&) noexcept = default;

http1_response_parser& http1_response_parser::operator=(http1_response_parser&&) noexcept = default;

void http1_response_parser::reset(http::method request_method) {
  impl_->reset(request_method);
}

void http1_response_parser::limits(http1_response_limits value) {
  impl_->limits(value);
}

http1_response_parse_result http1_response_parser::parse(
  std::string_view bytes,
  const http1_response_event_handler& on_event) {
  return impl_->parse(bytes, on_event);
}

http1_response_parse_result http1_response_parser::finish(const http1_response_event_handler& on_event) {
  return impl_->finish(on_event);
}

} // namespace uvp::http::detail
