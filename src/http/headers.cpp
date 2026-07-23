#include <uvpp/protocols/http/headers.hpp>

#include <cctype>
#include <stdexcept>

namespace uvp::http {

headers& headers::set(std::string_view name, std::string_view value) {
  if (!is_valid_name(name)) {
    throw std::invalid_argument("HTTP header name must be a non-empty token");
  }
  if (!is_valid_value(value)) {
    throw std::invalid_argument("HTTP header value must not contain CR, LF, or NUL");
  }
  for (auto& [existing_name, existing_value] : entries_) {
    if (names_equal(existing_name, name)) {
      existing_value.assign(value);
      return *this;
    }
  }

  return add(name, value);
}

headers& headers::add(std::string_view name, std::string_view value) {
  if (!is_valid_name(name)) {
    throw std::invalid_argument("HTTP header name must be a non-empty token");
  }
  if (!is_valid_value(value)) {
    throw std::invalid_argument("HTTP header value must not contain CR, LF, or NUL");
  }
  entries_.emplace_back(std::string(name), std::string(value));
  return *this;
}

bool headers::is_valid_name(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  for (const auto value : name) {
    const auto ch = static_cast<unsigned char>(value);
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
      continue;
    }
    switch (ch) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      continue;
    default:
      return false;
    }
  }
  return true;
}

bool headers::is_valid_value(std::string_view value) noexcept {
  for (const auto ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\0') {
      return false;
    }
  }
  return true;
}

std::string_view headers::get(std::string_view name) const noexcept {
  for (const auto& [existing_name, existing_value] : entries_) {
    if (names_equal(existing_name, name)) {
      return existing_value;
    }
  }
  return {};
}

bool headers::contains(std::string_view name) const noexcept {
  for (const auto& [existing_name, existing_value] : entries_) {
    (void)existing_value;
    if (names_equal(existing_name, name)) {
      return true;
    }
  }
  return false;
}

bool headers::names_equal(std::string_view lhs, std::string_view rhs) noexcept {
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

} // namespace uvp::http
