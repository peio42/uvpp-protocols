#pragma once

#include <string>
#include <system_error>

namespace uvp::io {

class stream_error {
public:
  stream_error() = default;
  explicit stream_error(std::error_code code) noexcept : code_(code) {}

  [[nodiscard]] bool ok() const noexcept { return !code_; }
  explicit operator bool() const noexcept { return !ok(); }

  [[nodiscard]] const std::error_code& code() const noexcept { return code_; }
  [[nodiscard]] std::string message() const { return code_.message(); }

private:
  std::error_code code_;
};

} // namespace uvp::io
