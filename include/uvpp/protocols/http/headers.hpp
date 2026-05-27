#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uv::http {

class headers {
public:
  using entry = std::pair<std::string, std::string>;
  using container_type = std::vector<entry>;
  using const_iterator = container_type::const_iterator;

  headers() = default;

  headers& set(std::string_view name, std::string_view value);
  headers& add(std::string_view name, std::string_view value);

  [[nodiscard]] std::string_view get(std::string_view name) const noexcept;
  [[nodiscard]] bool contains(std::string_view name) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }

private:
  static bool names_equal(std::string_view lhs, std::string_view rhs) noexcept;

  container_type entries_;
};

} // namespace uv::http

