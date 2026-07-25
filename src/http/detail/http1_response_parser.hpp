#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <uvpp/protocols/http/client.hpp>
#include <uvpp/protocols/http/method.hpp>

namespace uvp::http::detail {

struct http1_response_limits {
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_header_count = 128;
  std::size_t max_body_bytes = 4 * 1024 * 1024;
};

struct http1_response_message {
  http::response_head head;
  bool keep_alive = false;
};

struct http1_response_event {
  enum class type {
    headers,
    body,
    complete,
  };

  struct headers_payload {
    http::response_head head;
  };

  struct body_payload {
    std::string_view body;
  };

  struct complete_payload {
    http1_response_message message;
  };

  using payload_type = std::variant<headers_payload, body_payload, complete_payload>;

  explicit http1_response_event(headers_payload value)
      : payload(std::move(value)) {}

  explicit http1_response_event(body_payload value)
      : payload(value) {}

  explicit http1_response_event(complete_payload value)
      : payload(std::move(value)) {}

  [[nodiscard]] static http1_response_event headers(http::response_head head) {
    return http1_response_event{headers_payload{std::move(head)}};
  }

  [[nodiscard]] static http1_response_event body(std::string_view chunk) {
    return http1_response_event{body_payload{chunk}};
  }

  [[nodiscard]] static http1_response_event complete(http1_response_message message) {
    return http1_response_event{complete_payload{std::move(message)}};
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

  [[nodiscard]] const http::response_head& head() const {
    if (const auto* headers = std::get_if<headers_payload>(&payload)) {
      return headers->head;
    }
    return std::get<complete_payload>(payload).message.head;
  }

  [[nodiscard]] std::string_view body() const {
    return std::get<body_payload>(payload).body;
  }

  [[nodiscard]] const http1_response_message& message() const {
    return std::get<complete_payload>(payload).message;
  }

  payload_type payload;
};

// Events are borrowed for the duration of the callback. Returning false stops
// parsing at the event currently being delivered.
using http1_response_event_handler = std::function<bool(const http1_response_event&)>;

struct http1_response_parse_result {
  enum class status {
    ok,
    complete,
    error,
    upgrade,
    paused,
  };

  enum class error_kind {
    malformed,
    header_limit,
    body_limit,
  };

  status code = status::ok;
  error_kind kind = error_kind::malformed;
  std::string error;
  std::size_t parsed_bytes = 0;

  [[nodiscard]] bool ok() const noexcept { return code == status::ok; }
};

class http1_response_parser {
public:
  http1_response_parser();
  ~http1_response_parser();

  http1_response_parser(const http1_response_parser&) = delete;
  http1_response_parser& operator=(const http1_response_parser&) = delete;

  http1_response_parser(http1_response_parser&&) noexcept;
  http1_response_parser& operator=(http1_response_parser&&) noexcept;

  void reset(http::method request_method);
  void limits(http1_response_limits value);
  [[nodiscard]] http1_response_parse_result parse(
    std::string_view bytes,
    const http1_response_event_handler& on_event);
  [[nodiscard]] http1_response_parse_result finish(const http1_response_event_handler& on_event);

private:
  class impl;

  std::unique_ptr<impl> impl_;
};

} // namespace uvp::http::detail
