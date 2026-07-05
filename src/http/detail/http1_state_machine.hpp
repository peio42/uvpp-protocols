#pragma once

#include <memory>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/method.hpp>

namespace uvp::http::detail {

struct http1_limits {
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_header_count = 128;
};

struct http1_message {
  http::method method = http::method::unknown;
  std::string target;
  http::headers headers;
  std::string body;
  unsigned int http_major = 1;
  unsigned int http_minor = 1;
  bool keep_alive = false;
  bool upgrade = false;
};

struct http1_event {
  enum class type {
    headers,
    body,
    complete,
  };

  struct headers_payload {
    http1_message message;
  };

  struct body_payload {
    std::string body;
  };

  struct complete_payload {
    http1_message message;
  };

  using payload_type = std::variant<headers_payload, body_payload, complete_payload>;

  explicit http1_event(headers_payload value)
      : payload(std::move(value)) {}

  explicit http1_event(body_payload value)
      : payload(std::move(value)) {}

  explicit http1_event(complete_payload value)
      : payload(std::move(value)) {}

  [[nodiscard]] static http1_event headers(http1_message message) {
    return http1_event{headers_payload{std::move(message)}};
  }

  [[nodiscard]] static http1_event body(std::string chunk) {
    return http1_event{body_payload{std::move(chunk)}};
  }

  [[nodiscard]] static http1_event complete(http1_message message) {
    return http1_event{complete_payload{std::move(message)}};
  }

  [[nodiscard]] type event_type() const noexcept {
    switch (payload.index()) {
    case 0:
      return type::headers;
    case 1:
      return type::body;
    default:
      return type::complete;
    }
  }

  [[nodiscard]] const http1_message& message() const {
    if (const auto* headers = std::get_if<headers_payload>(&payload)) {
      return headers->message;
    }
    return std::get<complete_payload>(payload).message;
  }

  [[nodiscard]] const std::string& body() const {
    return std::get<body_payload>(payload).body;
  }

  payload_type payload;
};

struct http1_parse_result {
  enum class status {
    ok,
    error,
    upgrade,
  };

  status code = status::ok;
  std::string error;
  std::size_t parsed_bytes = 0;

  [[nodiscard]] bool ok() const noexcept { return code == status::ok; }
};

class http1_state_machine {
public:
  http1_state_machine();
  ~http1_state_machine();

  http1_state_machine(const http1_state_machine&) = delete;
  http1_state_machine& operator=(const http1_state_machine&) = delete;

  http1_state_machine(http1_state_machine&&) noexcept;
  http1_state_machine& operator=(http1_state_machine&&) noexcept;

  void reset();
  void limits(http1_limits value);
  [[nodiscard]] http1_parse_result parse(std::string_view bytes);

  [[nodiscard]] const std::vector<http1_message>& completed_messages() const noexcept;
  [[nodiscard]] const std::vector<http1_event>& events() const noexcept;

private:
  class impl;

  std::unique_ptr<impl> impl_;
};

} // namespace uvp::http::detail
