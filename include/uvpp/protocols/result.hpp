#pragma once

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace uvp {

struct error {
  std::error_code code;
  std::string detail;
};

template<class T>
class result {
public:
  result(T value) : value_(std::move(value)) {}
  result(uvp::error value) : value_(std::move(value)) {}

  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(value_); }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & {
    if (!has_value()) {
      throw std::logic_error("uvp::result has no value");
    }
    return std::get<T>(value_);
  }

  [[nodiscard]] const T& value() const& {
    if (!has_value()) {
      throw std::logic_error("uvp::result has no value");
    }
    return std::get<T>(value_);
  }

  [[nodiscard]] T&& value() && {
    if (!has_value()) {
      throw std::logic_error("uvp::result has no value");
    }
    return std::move(std::get<T>(value_));
  }

  [[nodiscard]] uvp::error& error() & {
    if (has_value()) {
      throw std::logic_error("uvp::result has no error");
    }
    return std::get<uvp::error>(value_);
  }

  [[nodiscard]] const uvp::error& error() const& {
    if (has_value()) {
      throw std::logic_error("uvp::result has no error");
    }
    return std::get<uvp::error>(value_);
  }

private:
  std::variant<T, uvp::error> value_;
};

} // namespace uvp
