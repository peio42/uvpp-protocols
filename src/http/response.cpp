#include <uvpp/protocols/http/response.hpp>

#include <stdexcept>

namespace uv::http {

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

response& response::status(unsigned int code) {
  if (code < 100 || code > 999) {
    throw std::invalid_argument("HTTP status code must be between 100 and 999");
  }
  status_code_ = code;
  return *this;
}

response& response::status(http::status value) {
  return status(static_cast<unsigned int>(value));
}

response& response::header(std::string_view name, std::string_view value) {
  headers_.set(name, value);
  return *this;
}

response& response::type(std::string_view content_type) {
  return header("content-type", content_type);
}

void response::text(std::string_view body) {
  type("text/plain; charset=utf-8");
  body_.assign(body);
  end();
}

void response::json(std::string_view serialized_json) {
  type("application/json");
  body_.assign(serialized_json);
  end();
}

void response::json(std::initializer_list<std::pair<std::string_view, std::string_view>> object) {
  type("application/json");

  body_ = "{";
  bool first = true;
  for (const auto& [key, value] : object) {
    if (!first) {
      body_ += ",";
    }
    first = false;
    body_ += '"';
    body_ += escape_json_string(key);
    body_ += "\":\"";
    body_ += escape_json_string(value);
    body_ += '"';
  }
  body_ += "}";

  end();
}

void response::bytes(std::span<const std::byte> body) {
  if (body.empty()) {
    body_.clear();
    end();
    return;
  }

  body_.assign(reinterpret_cast<const char*>(body.data()), body.size());
  end();
}

void response::end() {
  ended_ = true;
}

} // namespace uv::http
