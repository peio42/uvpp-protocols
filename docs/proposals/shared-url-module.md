# Shared URL Module Proposal

Status: Initial public API implemented; `ada-url` backend integration remains
open

## Decision

Add a small public `uvp::url` module backed by `ada-url` for URL strings that
use web-style WHATWG semantics. The module should provide an owning parsed URL
value, stable accessors for normalized components, conversion helpers for HTTP
client request targets and DNS/connect inputs, and typed URL errors.

The URL module is a foundation for client-side protocols, not an HTTP-only
helper. HTTP will consume it, but it should live beside shared IO/DNS
vocabulary so WebSocket client, MQTT-over-WebSocket, configuration strings, and
future adapters can reuse the same parsing behavior.

Do not replace typed endpoint APIs with URL strings. URLs are a convenience at
protocol boundaries; lower-level APIs should still accept explicit endpoint and
transport values where ambiguity matters.

## Context

Dependency decisions accept `ada-url` for a shared URL module. Several future
features need URL parsing, including HTTP client helpers and configuration
strings.

Milestone 6 starts the outbound/client-side foundation:

```text
URL string
  -> uvp::url
    -> DNS query
      -> TCP connect
        -> optional TLS/SNI/ALPN
          -> HTTP client request target
```

Without one shared URL layer, each client protocol would need to decide its own
normalization, host/port handling, percent-encoding, and error vocabulary. That
would make redirects, proxying, WebSocket client upgrades, and future connection
pool keys harder to keep consistent.

## Current State

- Implemented: request target, path, and query parsing inside HTTP request
  handling.
- Implemented: initial public `uvp::url` API, typed URL error category,
  absolute URL parsing, simple base-relative parsing, scheme classification,
  default/effective ports, authority endpoint extraction, origin keys, and
  HTTP origin-form/absolute-form target helpers.
- Not implemented: `ada-url` backend adapter, full WHATWG normalization,
  URL-based endpoint helpers outside the URL module, and protocol-specific URL
  policy in HTTP/WebSocket clients.
- Accepted dependency: `ada-url` for the general URL parser.
- Existing HTTP server route/query parsing remains separate and server-focused.

## Goals

- Provide a reusable parsed URL value for client-side protocols.
- Keep WHATWG normalization behind a stable uvpp-protocols API.
- Expose enough structure for HTTP client, DNS resolution, TLS SNI, ALPN, and
  connection pooling.
- Preserve raw user input where diagnostics need it.
- Provide typed errors for invalid URLs and unsupported schemes.
- Avoid leaking `ada-url` types or headers through public uvpp-protocols
  headers.
- Keep allocation and lifetime behavior simple: parsed URL objects own their
  component strings or stable storage.

## Non-Goals

- Do not implement a router or route pattern DSL.
- Do not make every endpoint API URL-based.
- Do not implement full URI template support.
- Do not provide browser policy features such as HSTS, CSP, CORS, cookies, or
  referrer policy.
- Do not make `uvp::url` responsible for DNS, TCP connect, TLS handshake,
  proxying, redirects, or filesystem path safety.
- Do not promise RFC-specific behavior for every protocol. Protocol modules may
  reject URLs that are syntactically valid under WHATWG but invalid for that
  protocol.

## Public Module Shape

Suggested include:

```cpp
#include <uvpp/protocols/url.hpp>
```

Suggested namespace and types:

```cpp
namespace uvp {

class url;

enum class url_errc {
  invalid,
  missing_scheme,
  unsupported_scheme,
  missing_host,
  invalid_port,
};

std::error_category const& url_category() noexcept;
std::error_code make_error_code(url_errc) noexcept;

result<url> parse_url(std::string_view input);
result<url> parse_url(std::string_view input, std::string_view base);

} // namespace uvp
```

`url` should be an owning value type. Accessors should return `std::string_view`
into storage owned by the `url` object, not borrowed views into the input string
or an `ada-url` object with unclear lifetime.

Basic accessors:

```cpp
class url {
public:
  [[nodiscard]] std::string_view href() const noexcept;
  [[nodiscard]] std::string_view scheme() const noexcept;
  [[nodiscard]] std::string_view username() const noexcept;
  [[nodiscard]] std::string_view password() const noexcept;
  [[nodiscard]] std::string_view host() const noexcept;
  [[nodiscard]] std::string_view hostname() const noexcept;
  [[nodiscard]] std::string_view port() const noexcept;
  [[nodiscard]] std::string_view path() const noexcept;
  [[nodiscard]] std::string_view query() const noexcept;
  [[nodiscard]] std::string_view fragment() const noexcept;

  [[nodiscard]] bool has_authority() const noexcept;
  [[nodiscard]] bool has_credentials() const noexcept;
  [[nodiscard]] bool has_port() const noexcept;
};
```

The exact accessor names should follow the names `ada-url` and WHATWG users
expect, but the public contract should document whether delimiters are included:

- `scheme()` excludes the trailing `:`;
- `hostname()` excludes userinfo, brackets, and port;
- `port()` excludes the leading `:`;
- `path()` includes the leading `/` for special schemes when present;
- `query()` excludes the leading `?`;
- `fragment()` excludes the leading `#`;
- `href()` is the normalized serialized URL.

## Protocol Policies

The parser should answer "is this a syntactically valid URL?" Protocol modules
should answer "is this URL acceptable for this protocol?".

Suggested helper type:

```cpp
enum class url_scheme {
  http,
  https,
  ws,
  wss,
  other,
};

[[nodiscard]] url_scheme scheme_id(url const&) noexcept;
```

HTTP client policy should initially accept only:

- `http`;
- `https`.

WebSocket client policy should later accept:

- `ws`;
- `wss`.

Other protocol modules can add their own validators without expanding the core
URL type into a protocol registry.

## Host And Port Handling

The URL module should expose helpers that avoid every protocol reimplementing
default-port logic:

```cpp
[[nodiscard]] std::optional<std::uint16_t> explicit_port(url const&);
[[nodiscard]] std::optional<std::uint16_t> default_port(url_scheme) noexcept;
[[nodiscard]] result<std::uint16_t> effective_port(url const&);
```

For Milestone 6:

- `http` defaults to port `80`;
- `https` defaults to port `443`;
- `ws` defaults to port `80`;
- `wss` defaults to port `443`;
- unknown schemes have no default port.

`hostname()` should be the value passed to DNS for ordinary host names and to
TLS as SNI for HTTPS when the host is not an IP literal. The module should
provide a helper or documented predicate for IP literals so TLS code can avoid
sending invalid SNI:

```cpp
[[nodiscard]] bool host_is_ip_literal(url const&) noexcept;
```

IPv6 literals should be accepted and exposed without brackets through
`hostname()`, while `host()` may keep the serialized host form if that is useful
for diagnostics. The exact behavior should be covered by tests.

## HTTP Request Target Helpers

HTTP/1.1 origin-form request targets require path and query, not scheme,
authority, or fragment:

```cpp
[[nodiscard]] std::string origin_form_target(url const&);
```

Examples:

```text
https://example.com            -> /
https://example.com/users?q=1  -> /users?q=1
https://example.com/a#frag     -> /a
```

Proxy support may later need absolute-form targets. Keep that as a separate
helper so ordinary direct HTTP requests cannot accidentally send the wrong
form:

```cpp
[[nodiscard]] std::string absolute_form_target(url const&);
```

The first HTTP client milestone should require `origin_form_target()`.
`absolute_form_target()` can be added with proxying if it is not needed earlier.

## Query Handling

The initial URL module should expose the raw normalized query string. It should
not immediately merge with `uvp::http::query_params`, because server-side query
handling currently follows HTML form conventions such as `+` as space, while
WHATWG URL behavior has its own rules and client code often needs exact
serialization for signatures.

Optional follow-up:

- add a shared query-params utility only after deciding whether it should model
  URL search params, form encoding, or both.

## Impact On HTTP Server

The shared URL module should not change HTTP server behavior in Milestone 6.
The server already handles inbound HTTP request targets with HTTP-specific
rules:

- `request::target()` exposes the raw request target;
- `request::path()` exposes the raw path component;
- route matching uses segment-local percent-decoding by default;
- `request::decoded_path_segments()` exposes decoded path segments without
  losing segment boundaries;
- `uvp::http::query_params` decodes query parameters using server/form-style
  conventions, including `+` as space.

Those choices should remain stable. A WHATWG URL parser is appropriate for
client-side absolute URLs such as `https://example.com/users`, but inbound
HTTP/1.1 servers most often receive origin-form request targets such as
`/users?q=1`, not full URLs. Feeding every inbound request target through
`uvp::url` would introduce different normalization rules and risk changing
routing, query decoding, signature-sensitive handlers, proxy routes, and
diagnostic logging.

The server-side boundary should therefore be:

```text
HTTP server inbound target
  -> existing HTTP request-target parser
    -> raw target/path/query + route path segments

HTTP client outbound URL
  -> uvp::url
    -> origin-form target + DNS/TLS/connect metadata
```

Potential future server uses should be explicit helpers, not a silent parser
replacement:

- validating absolute-form request targets for forward-proxy support;
- parsing `Location` header values or redirect targets generated by server
  applications;
- parsing configuration URLs used to mount proxy/upstream routes;
- validating authority-like values if an HTTP gateway needs stricter checks.

Even in those cases, `uvp::url` should be opt-in. Existing route registration,
route path matching, `request::path()`, `request::query()`, and
`uvp::http::query_params` should not change as part of the shared URL module.

## Build Integration

The first implementation exposes the public `uvp::url` boundary and keeps URL
state owned by uvpp-protocols. The full `ada-url` adapter should be wired behind
that same boundary. `ada-url` should stay private to the URL implementation:

- public headers do not include `ada.h` or expose `ada::url`;
- CMake may use `find_package(ada CONFIG)` or fetch it behind an option;
- if fetched, the dependency should be compiled or linked privately;
- tests cover behavior through `uvp::url`, not `ada-url` directly.

Suggested CMake options matching existing dependency style:

```sh
cmake -S . -B build -DUVPP_PROTOCOLS_FETCH_ADA=ON
cmake -S . -B build -DUVPP_PROTOCOLS_ADA_SOURCE_DIR=/path/to/ada
```

Exact option names can be adjusted during implementation, but they should be
documented in the root README once added.

## Error Model

URL parsing failures should map to `uvp::url` errors, not exceptions from
`ada-url`.

Suggested behavior:

- `parse_url(...)` returns `result<url>`;
- invalid input maps to `url_errc::invalid`;
- a protocol-specific validator maps missing scheme, unsupported scheme, or
  missing host to more specific `url_errc` values;
- helpers such as `effective_port()` fail with `invalid_port` when a parsed URL
  contains a port outside `uint16_t`.

If constructors are added for ergonomic code, they should either be explicit and
throw `std::invalid_argument` consistently, or be omitted until the project has
a settled exception/result split for value parsing. For Milestone 6, prefer
`parse_url(...)` returning `result<url>`.

## Integration With DNS

DNS should not parse URLs. It should consume already validated host/service
inputs.

The HTTP client flow should be:

```text
parse URL
  -> validate HTTP scheme and authority
    -> hostname + effective_port
      -> uvp::dns::resolver
```

The shared URL module may provide a small value object for outbound endpoint
intent if it helps avoid duplicated conversion:

```cpp
struct url_authority_endpoint {
  std::string host;
  std::uint16_t port;
  bool host_is_ip_literal = false;
};

result<url_authority_endpoint> authority_endpoint(url const&);
```

This helper should still not perform DNS or create sockets.

## Integration With TLS

For HTTPS and WSS, URL parsing feeds TLS configuration:

- `scheme` decides whether TLS is required;
- `hostname` feeds peer-name verification;
- non-IP host names feed SNI;
- ALPN remains configured by the HTTP/WebSocket client layer, not the URL
  module.

The URL module should not depend on `uvp::tls`.

## Connection Pool Keys

The HTTP client will need stable origin keys:

```cpp
struct origin {
  url_scheme scheme;
  std::string hostname;
  std::uint16_t port;

  friend bool operator==(origin const&, origin const&) = default;
};

result<origin> origin_from_url(url const&);
```

For Milestone 6, an `origin` helper is useful because DNS, TLS, and connection
pooling should agree about what "same origin connection" means. It should ignore
path, query, fragment, and userinfo.

## Milestone 6 Scope

- Add a shared public URL wrapper where WHATWG URL semantics are appropriate.
- Integrate `ada-url` behind private implementation files.
- Add typed URL error category and `parse_url(...)`.
- Expose stable component accessors.
- Add HTTP-relevant helpers: scheme classification, effective port,
  origin-form request target, host/IP-literal predicate, and origin key.
- Add tests for valid URLs, invalid URLs, normalization, default ports, IPv6
  literals, credentials, fragments, query preservation, and HTTP target
  construction.
- Update README and user documentation when the API lands.

## Implementation Steps

1. Add public header `include/uvpp/protocols/url.hpp`.
2. Add implementation file under `src/url/`.
3. Add CMake dependency discovery/fetch support for `ada-url`.
4. Define URL error category and result-returning parser.
5. Implement `url` as an owning wrapper around serialized component strings.
6. Add HTTP helpers needed by the future client proposal.
7. Add focused `url_test.cpp` coverage.
8. Update dependency and user documentation.

## Test Scope

Minimum tests:

- parse `http://example.com` and expose scheme, host, path, effective port, and
  origin target `/`;
- parse `https://example.com:8443/a/b?q=1#frag` and expose port `8443`,
  origin target `/a/b?q=1`, and no fragment in the target;
- reject invalid URLs;
- classify unsupported-but-valid schemes as `other`;
- default ports for `http`, `https`, `ws`, and `wss`;
- IPv6 literal host and effective port;
- userinfo is preserved for diagnostics but not included in origin keys;
- query serialization is preserved for HTTP request target generation;
- relative URL with base, if `parse_url(input, base)` is included in the first
  implementation;
- `ada-url` headers do not leak through the public header.
- HTTP server request-target parsing, route matching, and query parameter tests
  continue to pass unchanged.

## Out Of Scope

- Replacing typed endpoint overloads.
- URL routing DSL.
- Protocol-specific validation for every module.
- Proxy URL policy.
- URL search params or form encoding utilities.
- IDNA/punycode policy beyond what `ada-url` normalizes.
- File URLs and filesystem path conversion.
- Unix socket URL conventions.
- Public mutable URL builder API.
- Replacing HTTP server request-target, route path, or query parsing.

## Source Documents

- [Dependency decisions](../design/dependency-decisions.md)
- [Transport abstractions](../design/transport-abstractions.md)
- [DNS resolution](dns-resolution.md)
- [HTTP client](http-client.md)
- [Route path decoding](../archive/route-path-decoding.md)
