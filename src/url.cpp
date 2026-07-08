#include <uvpp/protocols/url.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace uvp {

namespace {

class url_error_category_impl : public std::error_category {
public:
  [[nodiscard]] const char* name() const noexcept override { return "uvp.url"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<url_errc>(value)) {
    case url_errc::invalid:
      return "invalid URL";
    case url_errc::missing_scheme:
      return "URL scheme is missing";
    case url_errc::unsupported_scheme:
      return "URL scheme is unsupported";
    case url_errc::missing_host:
      return "URL host is missing";
    case url_errc::invalid_port:
      return "URL port is invalid";
    }
    return "unknown URL error";
  }
};

[[nodiscard]] uvp::error make_url_error(url_errc code, std::string detail = {}) {
  return uvp::error{make_error_code(code), std::move(detail)};
}

[[nodiscard]] bool is_ascii_alpha(unsigned char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

[[nodiscard]] bool is_ascii_digit(unsigned char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] bool is_scheme_char(unsigned char value) noexcept {
  return is_ascii_alpha(value) || is_ascii_digit(value) || value == '+' || value == '-' || value == '.';
}

[[nodiscard]] std::string lowercase(std::string_view value) {
  auto result = std::string{value};
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

[[nodiscard]] bool valid_scheme(std::string_view value) noexcept {
  if (value.empty() || !is_ascii_alpha(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  for (const auto ch : value.substr(1)) {
    if (!is_scheme_char(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool contains_ascii_whitespace(std::string_view value) noexcept {
  for (const auto ch : value) {
    switch (ch) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '\f':
      return true;
    default:
      break;
    }
  }
  return false;
}

[[nodiscard]] bool looks_like_ipv4_literal(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }

  auto parts = 0;
  std::size_t offset = 0;
  while (offset <= value.size()) {
    const auto next = value.find('.', offset);
    const auto end = next == std::string_view::npos ? value.size() : next;
    const auto segment = value.substr(offset, end - offset);
    if (segment.empty() || segment.size() > 3) {
      return false;
    }
    auto number = 0;
    const auto* first = segment.data();
    const auto* last = first + segment.size();
    const auto [ptr, ec] = std::from_chars(first, last, number);
    if (ec != std::errc{} || ptr != last || number > 255) {
      return false;
    }
    ++parts;
    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }

  return parts == 4;
}

[[nodiscard]] bool looks_like_ipv6_literal(std::string_view value) noexcept {
  return value.find(':') != std::string_view::npos;
}

[[nodiscard]] std::size_t first_of_or_end(std::string_view value, std::string_view chars) noexcept {
  const auto pos = value.find_first_of(chars);
  return pos == std::string_view::npos ? value.size() : pos;
}

[[nodiscard]] bool has_absolute_scheme(std::string_view value) noexcept {
  const auto scheme_end = value.find(':');
  if (scheme_end == std::string_view::npos) {
    return false;
  }

  const auto first_path_query_or_fragment = first_of_or_end(value, "/?#");
  return scheme_end < first_path_query_or_fragment && valid_scheme(value.substr(0, scheme_end));
}

} // namespace

class url_parser {
public:
  [[nodiscard]] static result<url> parse_absolute(std::string_view input);
  [[nodiscard]] static std::string serialize_authority(const url& value);
  [[nodiscard]] static bool is_host_ip_literal(const url& value) noexcept;

private:
  [[nodiscard]] static bool parse_authority(std::string_view authority, url& parsed);
};

namespace {

[[nodiscard]] result<std::uint16_t> parse_port(std::string_view value) {
  if (value.empty()) {
    return make_url_error(url_errc::invalid_port);
  }
  auto number = 0U;
  const auto* first = value.data();
  const auto* last = first + value.size();
  const auto [ptr, ec] = std::from_chars(first, last, number);
  if (ec != std::errc{} || ptr != last || number > std::numeric_limits<std::uint16_t>::max()) {
    return make_url_error(url_errc::invalid_port);
  }
  return static_cast<std::uint16_t>(number);
}

} // namespace

bool url_parser::parse_authority(std::string_view authority, url& parsed) {
  parsed.has_authority_ = true;

  const auto at = authority.rfind('@');
  auto host_port = authority;
  if (at != std::string_view::npos) {
    const auto userinfo = authority.substr(0, at);
    host_port = authority.substr(at + 1);

    const auto colon = userinfo.find(':');
    if (colon == std::string_view::npos) {
      parsed.username_ = std::string{userinfo};
    } else {
      parsed.username_ = std::string{userinfo.substr(0, colon)};
      parsed.password_ = std::string{userinfo.substr(colon + 1)};
    }
  }

  if (host_port.empty()) {
    return false;
  }

  if (host_port.front() == '[') {
    const auto close = host_port.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    parsed.hostname_ = std::string{host_port.substr(1, close - 1)};
    parsed.host_ = std::string{host_port.substr(0, close + 1)};
    parsed.host_is_ip_literal_ = looks_like_ipv6_literal(parsed.hostname_);

    const auto rest = host_port.substr(close + 1);
    if (!rest.empty()) {
      if (rest.front() != ':') {
        return false;
      }
      parsed.port_ = std::string{rest.substr(1)};
    }
    return !parsed.hostname_.empty();
  }

  const auto colon = host_port.rfind(':');
  if (colon != std::string_view::npos && host_port.find(':') == colon) {
    parsed.hostname_ = lowercase(host_port.substr(0, colon));
    parsed.port_ = std::string{host_port.substr(colon + 1)};
  } else {
    parsed.hostname_ = lowercase(host_port);
  }

  parsed.host_ = parsed.hostname_;
  parsed.host_is_ip_literal_ = looks_like_ipv4_literal(parsed.hostname_);
  return !parsed.hostname_.empty();
}

std::string url_parser::serialize_authority(const url& value) {
  auto authority = std::string{};
  if (value.has_credentials()) {
    authority += value.username();
    if (!value.password().empty()) {
      authority += ':';
      authority += value.password();
    }
    authority += '@';
  }
  authority += value.host();
  if (value.has_port()) {
    authority += ':';
    authority += value.port();
  }
  return authority;
}

result<url> url_parser::parse_absolute(std::string_view input) {
  if (input.empty() || contains_ascii_whitespace(input)) {
    return make_url_error(url_errc::invalid);
  }

  const auto scheme_end = input.find(':');
  if (scheme_end == std::string_view::npos) {
    return make_url_error(url_errc::missing_scheme);
  }

  auto parsed = url{};
  parsed.scheme_ = lowercase(input.substr(0, scheme_end));
  if (!valid_scheme(parsed.scheme_)) {
    return make_url_error(url_errc::invalid);
  }

  auto rest = input.substr(scheme_end + 1);
  if (rest.starts_with("//")) {
    rest.remove_prefix(2);
    const auto authority_end = first_of_or_end(rest, "/?#");
    if (!url_parser::parse_authority(rest.substr(0, authority_end), parsed)) {
      return make_url_error(url_errc::missing_host);
    }
    rest.remove_prefix(authority_end);
  }

  const auto fragment_start = rest.find('#');
  if (fragment_start != std::string_view::npos) {
    parsed.fragment_ = std::string{rest.substr(fragment_start + 1)};
    rest = rest.substr(0, fragment_start);
  }

  const auto query_start = rest.find('?');
  if (query_start != std::string_view::npos) {
    parsed.query_ = std::string{rest.substr(query_start + 1)};
    rest = rest.substr(0, query_start);
  }

  parsed.path_ = rest.empty() && parsed.has_authority_ ? "/" : std::string{rest};

  parsed.href_ = parsed.scheme_;
  parsed.href_ += ':';
  if (parsed.has_authority_) {
    parsed.href_ += "//";
    parsed.href_ += url_parser::serialize_authority(parsed);
  }
  parsed.href_ += parsed.path_;
  if (!parsed.query_.empty()) {
    parsed.href_ += '?';
    parsed.href_ += parsed.query_;
  }
  if (!parsed.fragment_.empty()) {
    parsed.href_ += '#';
    parsed.href_ += parsed.fragment_;
  }

  return parsed;
}

bool url_parser::is_host_ip_literal(const url& value) noexcept {
  return value.host_is_ip_literal_;
}

const std::error_category& url_category() noexcept {
  static const url_error_category_impl instance;
  return instance;
}

std::error_code make_error_code(url_errc value) noexcept {
  return {static_cast<int>(value), url_category()};
}

result<url> parse_url(std::string_view input) {
  return url_parser::parse_absolute(input);
}

result<url> parse_url(std::string_view input, std::string_view base) {
  if (has_absolute_scheme(input)) {
    return url_parser::parse_absolute(input);
  }

  auto base_url = url_parser::parse_absolute(base);
  if (!base_url) {
    return base_url.error();
  }

  auto base_value = std::move(base_url).value();
  if (input.empty()) {
    return base_value;
  }

  auto reference = std::string{};
  if (input.starts_with("//")) {
    reference += base_value.scheme();
    reference += ':';
    reference += input;
  } else if (input.starts_with('/')) {
    reference += base_value.scheme();
    reference += "://";
    reference += url_parser::serialize_authority(base_value);
    reference += input;
  } else {
    reference += base_value.scheme();
    reference += "://";
    reference += url_parser::serialize_authority(base_value);
    auto base_path = std::string{base_value.path()};
    const auto slash = base_path.rfind('/');
    if (slash == std::string::npos) {
      reference += '/';
    } else {
      reference += base_path.substr(0, slash + 1);
    }
    reference += input;
  }

  return url_parser::parse_absolute(reference);
}

url_scheme scheme_id(const url& value) noexcept {
  if (value.scheme() == "http") {
    return url_scheme::http;
  }
  if (value.scheme() == "https") {
    return url_scheme::https;
  }
  if (value.scheme() == "ws") {
    return url_scheme::ws;
  }
  if (value.scheme() == "wss") {
    return url_scheme::wss;
  }
  return url_scheme::other;
}

std::optional<std::uint16_t> explicit_port(const url& value) {
  if (!value.has_port()) {
    return std::nullopt;
  }
  auto parsed = parse_port(value.port());
  if (!parsed) {
    return std::nullopt;
  }
  return parsed.value();
}

std::optional<std::uint16_t> default_port(url_scheme scheme) noexcept {
  switch (scheme) {
  case url_scheme::http:
  case url_scheme::ws:
    return 80;
  case url_scheme::https:
  case url_scheme::wss:
    return 443;
  case url_scheme::other:
    return std::nullopt;
  }
  return std::nullopt;
}

result<std::uint16_t> effective_port(const url& value) {
  if (value.has_port()) {
    return parse_port(value.port());
  }

  auto fallback = default_port(scheme_id(value));
  if (!fallback) {
    return make_url_error(url_errc::unsupported_scheme);
  }
  return *fallback;
}

bool host_is_ip_literal(const url& value) noexcept {
  return url_parser::is_host_ip_literal(value);
}

std::string origin_form_target(const url& value) {
  auto target = std::string{value.path().empty() ? "/" : value.path()};
  if (!value.query().empty()) {
    target += '?';
    target += value.query();
  }
  return target;
}

std::string absolute_form_target(const url& value) {
  auto target = std::string{value.scheme()};
  target += ':';
  if (value.has_authority()) {
    target += "//";
    target += url_parser::serialize_authority(value);
  }
  target += origin_form_target(value);
  return target;
}

result<url_authority_endpoint> authority_endpoint(const url& value) {
  if (!value.has_authority() || value.hostname().empty()) {
    return make_url_error(url_errc::missing_host);
  }

  auto port = effective_port(value);
  if (!port) {
    return port.error();
  }

  return url_authority_endpoint{
    .host = std::string{value.hostname()},
    .port = port.value(),
    .host_is_ip_literal = host_is_ip_literal(value),
  };
}

result<origin> origin_from_url(const url& value) {
  if (!value.has_authority() || value.hostname().empty()) {
    return make_url_error(url_errc::missing_host);
  }

  auto port = effective_port(value);
  if (!port) {
    return port.error();
  }

  return origin{
    .scheme = scheme_id(value),
    .hostname = std::string{value.hostname()},
    .port = port.value(),
  };
}

} // namespace uvp
