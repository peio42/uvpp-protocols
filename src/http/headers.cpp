#include <uvpp/protocols/http/headers.hpp>

#include <cctype>

namespace uv::http {

headers& headers::set(std::string_view name, std::string_view value) {
  for (auto& [existing_name, existing_value] : entries_) {
    if (names_equal(existing_name, name)) {
      existing_value.assign(value);
      return *this;
    }
  }

  return add(name, value);
}

headers& headers::add(std::string_view name, std::string_view value) {
  entries_.emplace_back(std::string(name), std::string(value));
  return *this;
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

} // namespace uv::http

