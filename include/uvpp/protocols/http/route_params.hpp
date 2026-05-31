#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uvp::http {

class route_params {
public:
  using entry = std::pair<std::string, std::string>;
  using container_type = std::vector<entry>;
  using const_iterator = container_type::const_iterator;

  route_params() = default;

  route_params& set(std::string_view name, std::string_view value);
  [[nodiscard]] std::string_view get(std::string_view name) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }

private:
  container_type entries_;
};

} // namespace uvp::http

