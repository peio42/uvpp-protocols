#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http {

route_params& route_params::set(std::string_view name, std::string_view value) {
  for (auto& [existing_name, existing_value] : entries_) {
    if (existing_name == name) {
      existing_value.assign(value);
      return *this;
    }
  }

  entries_.emplace_back(std::string(name), std::string(value));
  return *this;
}

std::string_view route_params::get(std::string_view name) const noexcept {
  for (const auto& [existing_name, existing_value] : entries_) {
    if (existing_name == name) {
      return existing_value;
    }
  }
  return {};
}

} // namespace uvp::http

