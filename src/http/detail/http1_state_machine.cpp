#include "http1_state_machine.hpp"

#include <utility>

#include <llhttp.h>

namespace uvp::http::detail {

namespace {

http::method map_method(llhttp_method_t value) noexcept {
  switch (value) {
  case HTTP_DELETE:
    return http::method::delete_;
  case HTTP_GET:
    return http::method::get;
  case HTTP_HEAD:
    return http::method::head;
  case HTTP_POST:
    return http::method::post;
  case HTTP_PUT:
    return http::method::put;
  case HTTP_CONNECT:
    return http::method::connect;
  case HTTP_OPTIONS:
    return http::method::options;
  case HTTP_TRACE:
    return http::method::trace;
  case HTTP_PATCH:
    return http::method::patch;
  default:
    return http::method::unknown;
  }
}

} // namespace

class http1_state_machine::impl {
public:
  impl() {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &impl::on_message_begin;
    settings_.on_url = &impl::on_url;
    settings_.on_header_field = &impl::on_header_field;
    settings_.on_header_value = &impl::on_header_value;
    settings_.on_headers_complete = &impl::on_headers_complete;
    settings_.on_body = &impl::on_body;
    settings_.on_message_complete = &impl::on_message_complete;
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
  }

  void reset() {
    current_ = http1_message{};
    completed_messages_.clear();
    events_.clear();
    pending_header_field_.clear();
    pending_header_value_.clear();
    last_header_part_ = header_part::none;

    llhttp_reset(&parser_);
    parser_.data = this;
  }

  http1_parse_result parse(std::string_view bytes) {
    const auto err = llhttp_execute(&parser_, bytes.data(), bytes.size());
    if (err == HPE_OK) {
      return {http1_parse_result::status::ok, {}, bytes.size()};
    }

    if (err == HPE_PAUSED_UPGRADE) {
      if (!completed_messages_.empty()) {
        completed_messages_.back().upgrade = true;
      }
      const char* error_pos = llhttp_get_error_pos(&parser_);
      std::size_t parsed_bytes = bytes.size();
      if (error_pos >= bytes.data() && error_pos <= bytes.data() + bytes.size()) {
        parsed_bytes = static_cast<std::size_t>(error_pos - bytes.data());
      }
      return {http1_parse_result::status::upgrade, {}, parsed_bytes};
    }

    return {http1_parse_result::status::error, error_message(err), 0};
  }

  [[nodiscard]] const std::vector<http1_message>& completed_messages() const noexcept {
    return completed_messages_;
  }

  [[nodiscard]] const std::vector<http1_event>& events() const noexcept {
    return events_;
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
    self.current_ = http1_message{};
    self.pending_header_field_.clear();
    self.pending_header_value_.clear();
    self.last_header_part_ = header_part::none;
    return HPE_OK;
  }

  static int on_url(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    self.current_.target.append(at, length);
    return HPE_OK;
  }

  static int on_header_field(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    if (self.last_header_part_ == header_part::value) {
      self.commit_pending_header();
    }
    self.pending_header_field_.append(at, length);
    self.last_header_part_ = header_part::field;
    return HPE_OK;
  }

  static int on_header_value(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    self.pending_header_value_.append(at, length);
    self.last_header_part_ = header_part::value;
    return HPE_OK;
  }

  static int on_headers_complete(llhttp_t* parser) {
    auto& self = impl::self(parser);
    self.commit_pending_header();
    self.current_.method = map_method(static_cast<llhttp_method_t>(llhttp_get_method(parser)));
    self.current_.http_major = static_cast<unsigned int>(llhttp_get_http_major(parser));
    self.current_.http_minor = static_cast<unsigned int>(llhttp_get_http_minor(parser));
    self.events_.push_back(http1_event{
      http1_event::type::headers,
      self.current_,
      {},
    });
    if (!self.current_.headers.get("upgrade").empty() || self.current_.method == http::method::connect) {
      // llhttp uses callback return value 2 to pause with HPE_PAUSED_UPGRADE.
      return 2;
    }
    return HPE_OK;
  }

  static int on_body(llhttp_t* parser, const char* at, std::size_t length) {
    auto& self = impl::self(parser);
    self.current_.body.append(at, length);
    self.events_.push_back(http1_event{
      http1_event::type::body,
      {},
      std::string{at, length},
    });
    return HPE_OK;
  }

  static int on_message_complete(llhttp_t* parser) {
    auto& self = impl::self(parser);
    self.commit_pending_header();
    self.current_.keep_alive = llhttp_should_keep_alive(parser) != 0;
    self.events_.push_back(http1_event{
      http1_event::type::complete,
      self.current_,
      {},
    });
    self.completed_messages_.push_back(std::move(self.current_));
    self.current_ = http1_message{};
    return HPE_OK;
  }

  static std::string error_message(llhttp_errno_t err) {
    return std::string{llhttp_errno_name(err)};
  }

  llhttp_t parser_{};
  llhttp_settings_t settings_{};

  void commit_pending_header() {
    if (!pending_header_field_.empty()) {
      current_.headers.add(pending_header_field_, pending_header_value_);
      pending_header_field_.clear();
      pending_header_value_.clear();
    }
    last_header_part_ = header_part::none;
  }

  http1_message current_;
  std::vector<http1_message> completed_messages_;
  std::vector<http1_event> events_;
  std::string pending_header_field_;
  std::string pending_header_value_;
  header_part last_header_part_ = header_part::none;
};

http1_state_machine::http1_state_machine()
    : impl_(std::make_unique<impl>()) {}

http1_state_machine::~http1_state_machine() = default;

http1_state_machine::http1_state_machine(http1_state_machine&&) noexcept = default;

http1_state_machine& http1_state_machine::operator=(http1_state_machine&&) noexcept = default;

void http1_state_machine::reset() {
  impl_->reset();
}

http1_parse_result http1_state_machine::parse(std::string_view bytes) {
  return impl_->parse(bytes);
}

const std::vector<http1_message>& http1_state_machine::completed_messages() const noexcept {
  return impl_->completed_messages();
}

const std::vector<http1_event>& http1_state_machine::events() const noexcept {
  return impl_->events();
}

} // namespace uvp::http::detail
