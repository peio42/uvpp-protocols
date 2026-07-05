#pragma once

#include <uvpp/protocols/io/endpoint.hpp>
#include <uvpp/protocols/result.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace uv {
class loop;
}

namespace uvp::dns {

enum class errc {
  invalid_query = 1,
  unsupported_address_family,
  name_not_found,
  temporary_failure,
  no_usable_addresses,
  cancelled,
  system_failure,
};

std::error_code make_error_code(errc value) noexcept;
const std::error_category& error_category() noexcept;

enum class address_family {
  any,
  ipv4,
  ipv6,
};

class query {
public:
  query& host(std::string value) &;
  query&& host(std::string value) &&;
  [[nodiscard]] std::string_view host() const noexcept { return host_; }

  query& service(std::string value) &;
  query&& service(std::string value) &&;
  query& port(unsigned int value) &;
  query&& port(unsigned int value) &&;
  [[nodiscard]] std::string_view service() const noexcept { return service_; }

  query& family(address_family value) noexcept;
  [[nodiscard]] address_family family() const noexcept { return family_; }

private:
  std::string host_;
  std::string service_;
  address_family family_ = address_family::any;
};

class address {
public:
  address() = default;
  address(address_family family, uvp::io::tcp_endpoint endpoint);

  [[nodiscard]] address_family family() const noexcept { return family_; }
  [[nodiscard]] const uvp::io::tcp_endpoint& endpoint() const noexcept { return endpoint_; }
  [[nodiscard]] std::string_view host() const noexcept { return endpoint_.host; }
  [[nodiscard]] unsigned int port() const noexcept { return endpoint_.port; }

private:
  address_family family_ = address_family::any;
  uvp::io::tcp_endpoint endpoint_;
};

class address_list {
public:
  using container_type = std::vector<address>;
  using const_iterator = container_type::const_iterator;

  address_list() = default;
  explicit address_list(container_type addresses);

  [[nodiscard]] bool empty() const noexcept { return addresses_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return addresses_.size(); }
  [[nodiscard]] const address& operator[](std::size_t index) const noexcept { return addresses_[index]; }
  [[nodiscard]] const_iterator begin() const noexcept { return addresses_.begin(); }
  [[nodiscard]] const_iterator end() const noexcept { return addresses_.end(); }

private:
  container_type addresses_;
};

using resolve_callback = std::function<void(uvp::result<address_list>)>;

class resolve_operation {
public:
  resolve_operation() = default;
  explicit resolve_operation(std::shared_ptr<void> state);

  void cancel() noexcept;
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  std::shared_ptr<void> state_;
};

class resolver {
public:
  explicit resolver(uv::loop& loop) noexcept;

  [[nodiscard]] resolve_operation resolve(query request, resolve_callback callback);

private:
  uv::loop* loop_;
};

} // namespace uvp::dns

namespace std {

template<>
struct is_error_code_enum<uvp::dns::errc> : true_type {};

} // namespace std
