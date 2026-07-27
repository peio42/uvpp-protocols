#pragma once

#include <cstddef>
#include <string_view>

namespace uvp::detail {

// A diagnostic name for a statically-defined operation phase.
//
// The consteval constructor deliberately accepts only string literals. Protocol
// modules define their own inline constexpr phases, which makes a phase cheap
// to copy and safe to retain without allocating or owning a string.
class operation_phase {
public:
  template<std::size_t Size>
  consteval explicit operation_phase(const char (&name)[Size])
      : name_(name, Size - 1) {}

  [[nodiscard]] constexpr std::string_view name() const noexcept { return name_; }

  friend constexpr bool operator==(operation_phase, operation_phase) = default;

private:
  std::string_view name_;
};

} // namespace uvp::detail
