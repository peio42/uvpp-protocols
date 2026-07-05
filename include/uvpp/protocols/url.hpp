#pragma once

#include <uvpp/protocols/result.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace uvp {

class url_parser;

enum class url_errc {
  invalid = 1,
  missing_scheme,
  unsupported_scheme,
  missing_host,
  invalid_port,
};

std::error_code make_error_code(url_errc value) noexcept;
const std::error_category& url_category() noexcept;

enum class url_scheme {
  http,
  https,
  ws,
  wss,
  other,
};

class url {
public:
  url() = default;

  [[nodiscard]] std::string_view href() const noexcept { return href_; }
  [[nodiscard]] std::string_view scheme() const noexcept { return scheme_; }
  [[nodiscard]] std::string_view username() const noexcept { return username_; }
  [[nodiscard]] std::string_view password() const noexcept { return password_; }
  [[nodiscard]] std::string_view host() const noexcept { return host_; }
  [[nodiscard]] std::string_view hostname() const noexcept { return hostname_; }
  [[nodiscard]] std::string_view port() const noexcept { return port_; }
  [[nodiscard]] std::string_view path() const noexcept { return path_; }
  [[nodiscard]] std::string_view query() const noexcept { return query_; }
  [[nodiscard]] std::string_view fragment() const noexcept { return fragment_; }

  [[nodiscard]] bool has_authority() const noexcept { return has_authority_; }
  [[nodiscard]] bool has_credentials() const noexcept { return !username_.empty() || !password_.empty(); }
  [[nodiscard]] bool has_port() const noexcept { return !port_.empty(); }

private:
  friend result<url> parse_url(std::string_view input);
  friend result<url> parse_url(std::string_view input, std::string_view base);
  friend class url_parser;

  std::string href_;
  std::string scheme_;
  std::string username_;
  std::string password_;
  std::string host_;
  std::string hostname_;
  std::string port_;
  std::string path_;
  std::string query_;
  std::string fragment_;
  bool has_authority_ = false;
  bool host_is_ip_literal_ = false;
};

struct url_authority_endpoint {
  std::string host;
  std::uint16_t port = 0;
  bool host_is_ip_literal = false;

  friend bool operator==(const url_authority_endpoint&, const url_authority_endpoint&) = default;
};

struct origin {
  url_scheme scheme = url_scheme::other;
  std::string hostname;
  std::uint16_t port = 0;

  friend bool operator==(const origin&, const origin&) = default;
};

[[nodiscard]] result<url> parse_url(std::string_view input);
[[nodiscard]] result<url> parse_url(std::string_view input, std::string_view base);

[[nodiscard]] url_scheme scheme_id(const url& value) noexcept;
[[nodiscard]] std::optional<std::uint16_t> explicit_port(const url& value);
[[nodiscard]] std::optional<std::uint16_t> default_port(url_scheme scheme) noexcept;
[[nodiscard]] result<std::uint16_t> effective_port(const url& value);
[[nodiscard]] bool host_is_ip_literal(const url& value) noexcept;
[[nodiscard]] std::string origin_form_target(const url& value);
[[nodiscard]] std::string absolute_form_target(const url& value);
[[nodiscard]] result<url_authority_endpoint> authority_endpoint(const url& value);
[[nodiscard]] result<origin> origin_from_url(const url& value);

} // namespace uvp

namespace std {

template<>
struct is_error_code_enum<uvp::url_errc> : true_type {};

} // namespace std
