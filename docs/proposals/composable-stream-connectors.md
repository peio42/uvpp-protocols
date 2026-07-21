# Composable Stream Connectors Proposal

Status: Draft, companion to the outbound connector and proxy routes proposal

## Decision

Introduce a reusable `uvp::io::stream_connector` transport shape for opening
an outbound `uvp::io::byte_stream` to a logical TCP endpoint. Concrete
connectors compose by ownership: a proxy connector receives a lower
`stream_connector`, opens a stream to its next hop through that lower
connector, performs its handshake, then returns the resulting tunnel stream.

The public composition API should use concrete classes and rvalue conversions,
not `make_*` factory functions and not a fluent pipeline DSL in the first
iteration. This follows the established listener composition style:

```text
tcp_listener / pipe_listener  --rvalue conversion--> stream_listener
tls::listener(stream_listener, options) --rvalue conversion--> stream_listener

tcp_connector                 --rvalue conversion--> stream_connector
proxy::socks5_connector(stream_connector, options)
                              --rvalue conversion--> stream_connector
proxy::http_connect_connector(stream_connector, options)
                              --rvalue conversion--> stream_connector
```

`router` is reserved for a future policy component that selects a route. It is
not the name of a concrete transport hop, and it avoids ambiguity with
`uvp::http::router`.

## Context

The current IO layer has a type-erased, move-only `byte_stream` for an
established bidirectional byte transport and a `tcp_connector` for outbound
TCP establishment. TLS already composes over an established `byte_stream`; the
server-side TLS listener composes over a move-only `stream_listener`.

Tunnel proxies have the same useful boundary as TLS listeners, but on the
outbound path:

```text
logical target
  -> TCP connection to a proxy
  -> proxy handshake naming the logical target
  -> byte_stream to the logical target
```

SOCKS5 and HTTP CONNECT both yield a transparent byte tunnel after their
handshake. A layer above them can use the returned stream without knowing which
proxy, if any, was traversed.

The existing [outbound connector and proxy routes proposal](outbound-connector-and-proxy-routes.md)
correctly identifies this boundary. Its initial `outbound_connector` sketch is
intentionally centralised and models at most one optional proxy. That shape is
not enough for nested routes such as SOCKS5 to an HTTP CONNECT proxy, and would
grow a protocol switch each time a new tunnel type is added.

## Goals

- Make direct TCP, SOCKS5, and HTTP CONNECT composable without special cases
  between the layers.
- Return a normal `uvp::io::byte_stream` for every transparent tunnel route.
- Preserve `connect_operation`, cancellation, and exactly-once completion
  conventions already used by `tcp_connector`.
- Keep the logical target, the current proxy hop, and route identity separate.
- Allow a configured connector to start multiple independent operations.
- Keep proxy protocols below HTTP, WebSocket, SMTP, Redis, MQTT, and future
  client modules.
- Keep the common direct-TCP path simple and allocation-conscious.

## Non-Goals

- Replacing `uvp::io::tcp_connector`; it remains the concrete direct connector.
- Treating clear HTTP forward proxying as a transparent byte tunnel.
- Proxy auto-discovery, PAC files, environment-variable policy, or platform
  proxy settings.
- SOCKS UDP, HTTP CONNECT-UDP, QUIC, or any datagram transport. They need a
  separate datagram-oriented abstraction.
- Connection multiplexing or pooling inside proxy connectors in the first
  slice.
- A fluent `direct | socks5 | connect` DSL. It can be considered after the
  ownership and error semantics are proven by concrete use.

## Terminology

### Logical target

The authority requested by the protocol client, for example
`api.example.com:443`. It is the hostname used for TLS SNI and certificate
verification after routing succeeds.

### Next hop

The endpoint to which a connector asks its lower connector to open a stream.
For a SOCKS connector this is the SOCKS server; for an HTTP CONNECT connector
this is the HTTP proxy. It is not the logical target.

### Connector

A reusable object that turns a `tcp_endpoint` target into an asynchronous
`byte_stream` result. A connector is loop-affine. It may start several
operations, but each call owns separate operation state.

### Route and router

A route is a configured connector chain plus a stable, credential-free
identity. A future router or route selector chooses one route according to a
destination and policy. It is intentionally distinct from tunnel handshakes.

## Proposed Public Shape

The common type-erased handle belongs to `uvp::io` and deliberately reuses the
existing target, options, callback, and cancellation types:

```cpp
namespace uvp::proxy {
class socks5_connector;
class http_connect_connector;
}

namespace uvp::io {

class stream_connector {
public:
  stream_connector() = default;
  ~stream_connector();

  stream_connector(stream_connector&&) noexcept;
  stream_connector& operator=(stream_connector&&) noexcept;

  stream_connector(const stream_connector&) = delete;
  stream_connector& operator=(const stream_connector&) = delete;

  [[nodiscard]] connect_operation connect(
    tcp_endpoint target,
    connect_callback callback);

  [[nodiscard]] connect_operation connect(
    tcp_endpoint target,
    connect_options options,
    connect_callback callback);

  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend class tcp_connector;
  friend class uvp::proxy::socks5_connector;
  friend class uvp::proxy::http_connect_connector;

  struct concept_;
  explicit stream_connector(std::unique_ptr<concept_> self);

  std::unique_ptr<concept_> self_;
};

class tcp_connector {
public:
  explicit tcp_connector(uv::loop& loop) noexcept;

  // Existing direct connect overloads remain available.
  // ...

  operator stream_connector() &&;
};

} // namespace uvp::io
```

Like `stream_listener`, `stream_connector` is move-only and owns its concrete
implementation. The move-only conversion makes ownership of the lower layer
visible in route construction. It also prevents a proxy connector from
silently retaining a reference to a temporary lower connector.

The proxy protocols live in a new `uvp::proxy` namespace. They are concrete
adapter classes rather than factories:

```cpp
namespace uvp::proxy {

enum class socks5_target_resolution {
  proxy,
  local,
};

struct socks5_options {
  uvp::io::tcp_endpoint proxy;
  socks5_target_resolution target_resolution = socks5_target_resolution::proxy;
  std::chrono::milliseconds handshake_timeout{0};
  std::optional<socks5_credentials> credentials;
};

class socks5_connector {
public:
  socks5_connector(
    uvp::io::stream_connector lower,
    socks5_options options);
  ~socks5_connector();

  socks5_connector(socks5_connector&&) noexcept;
  socks5_connector& operator=(socks5_connector&&) noexcept;

  socks5_connector(const socks5_connector&) = delete;
  socks5_connector& operator=(const socks5_connector&) = delete;

  operator uvp::io::stream_connector() &&;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

struct http_connect_options {
  uvp::io::tcp_endpoint proxy;
  std::string authorization;
  std::chrono::milliseconds handshake_timeout{0};
  std::size_t max_response_header_bytes = 64 * 1024;
};

class http_connect_connector {
public:
  http_connect_connector(
    uvp::io::stream_connector lower,
    http_connect_options options);
  ~http_connect_connector();

  http_connect_connector(http_connect_connector&&) noexcept;
  http_connect_connector& operator=(http_connect_connector&&) noexcept;

  http_connect_connector(const http_connect_connector&) = delete;
  http_connect_connector& operator=(const http_connect_connector&) = delete;

  operator uvp::io::stream_connector() &&;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace uvp::proxy
```

`socks5_credentials` is intentionally only named here. Its final public shape
should follow the credential and redaction conventions defined by the shared
protocol foundation instead of normalising secret handling prematurely.

The private type-erasure model mirrors `byte_stream` and `stream_listener`:
its virtual `connect` operation is the only polymorphic boundary. No public
base class is introduced, and no protocol implementation inherits from another
protocol implementation.

## Composition Examples

Direct TCP remains unchanged for callers that need it directly:

```cpp
uvp::io::tcp_connector connector{loop};
connector.connect({"api.example.com", 443}, on_connected);
```

A SOCKS5 route is built by passing the direct connector as the lower layer:

```cpp
uvp::io::stream_connector connector = uvp::proxy::socks5_connector{
  uvp::io::tcp_connector{loop},
  {
    .proxy = {"socks.example.net", 1080},
    .target_resolution = uvp::proxy::socks5_target_resolution::proxy,
  },
};

connector.connect({"api.example.com", 443}, on_connected);
```

An HTTP CONNECT proxy reached through SOCKS5 needs no connector-specific
knowledge between the layers:

```cpp
auto socks = uvp::proxy::socks5_connector{
  uvp::io::tcp_connector{loop},
  {.proxy = {"socks.example.net", 1080}},
};

uvp::io::stream_connector connector = uvp::proxy::http_connect_connector{
  std::move(socks),
  {
    .proxy = {"connect-proxy.example.net", 3128},
    .authorization = proxy_authorization,
  },
};

connector.connect({"api.example.com", 443}, on_connected);
```

For this last example, a call for `api.example.com:443` executes as follows:

```text
http_connect_connector.connect(api.example.com:443)
  -> lower.connect(connect-proxy.example.net:3128)
     [the lower is socks5_connector]
       -> lower.connect(socks.example.net:1080)
          [the lower is tcp_connector]
       -> SOCKS5 CONNECT connect-proxy.example.net:3128
  -> HTTP CONNECT api.example.com:443
  -> complete with the tunneled byte_stream
```

The final stream is still owned by the callback recipient. Until successful
completion, each connector owns the stream it acquired from its lower layer.

## Connector Semantics

### Reuse and concurrency

A configured `stream_connector` is a reusable route object, not an operation.
Each `connect` call creates isolated state for its lower operation, timer,
handshake buffers, and candidate stream. Sharing one configured connector among
sequential or concurrent client operations is therefore valid on its bound
event loop.

### Direct TCP adapter

The first `stream_connector` implementation wraps the existing
`tcp_connector`; it does not duplicate DNS or TCP connection logic. Direct
users keep the current `tcp_connector` API. Higher-level modules that accept a
route receive a `stream_connector` instead.

### SOCKS5 connector

The SOCKS connector asks the lower connector for a stream to its configured
proxy endpoint. It writes the greeting and optional authentication exchange,
then sends a CONNECT request for the logical target. A successful reply hands
the same stream to the caller.

With `target_resolution::proxy`, a domain target remains a domain in the SOCKS
request. This avoids local DNS lookup and lets the SOCKS server resolve it. With
`local`, the connector resolves the target before sending an IP address; that
mode needs an explicit DNS dependency and must not be an accidental fallback.

SOCKS BIND and UDP ASSOCIATE are out of scope because they do not have the
outbound byte-stream contract described here.

### HTTP CONNECT connector

The HTTP CONNECT connector asks the lower connector for a stream to its proxy
endpoint, sends an authority-form CONNECT request for the logical target, and
parses a bounded HTTP response. A 2xx response returns the lower stream
unchanged as a tunnel. Non-2xx responses, malformed responses, oversized
headers, or premature EOF fail the operation and close the candidate stream.

The response parser belongs to the connector implementation. It may reuse a
small HTTP parsing utility, but it must not require an HTTP client object or
make proxy routing depend on HTTP request pooling.

## TLS Composition

For a normal HTTPS origin, routing completes before TLS:

```text
connector.connect(api.example.com:443)
  -> byte_stream tunnel to api.example.com
  -> tls::connect(stream, client context for api.example.com)
```

SNI, certificate hostname verification, and origin pooling use
`api.example.com`, never the proxy name.

The current `uvp::tls::connect(byte_stream, ...)` API is sufficient for this
first integration. A future `uvp::tls::connector` can follow the same adapter
pattern when TLS-to-proxy routes are needed:

```text
tcp_connector -> tls::connector(proxy identity) -> http_connect_connector
```

That adapter should be proposed separately because it needs an explicit rule
for choosing a client TLS context from the target supplied by its outer layer.

## HTTP Forward Proxies Remain HTTP-Level

Clear HTTP proxying is not a transparent tunnel:

```text
TCP proxy -> GET http://origin/path HTTP/1.1
```

The stream reaches the proxy, while the HTTP request must use absolute-form.
It must therefore remain in the HTTP client route logic. It is not represented
by a `stream_connector` that claims to connect to the origin.

The existing HTTP client already has this distinction: it connects to the
configured proxy and chooses absolute-form request targets. HTTPS through
HTTP CONNECT should migrate to the connector path once the CONNECT connector
exists.

## Target Identity, Route Identity, and Pooling

An endpoint alone is not enough to pool routed connections safely. Pool keys
must include at least:

- logical origin scheme, host, and port;
- a stable route identifier, including the ordered proxy protocols and proxy
  endpoints but never credentials;
- TLS verification policy and ALPN-relevant settings.

The first connector slice does not need to prescribe a public route-description
type. A protocol client can receive the route identifier from its configuration
alongside its `stream_connector`. When a route selector is added, it should
return both the selected connector and an immutable route identity.

There must be no implicit fallback from a proxy route to direct TCP. Such a
fallback can expose traffic or DNS queries outside an intended network policy.

## Timeouts, Cancellation, and Errors

The existing `connect_options::timeout` is the outward-facing time budget for
the complete route, not a fresh full timeout for every hop. The outer
connector computes a deadline and each lower operation receives only the
remaining time. Per-protocol options may impose a shorter handshake limit.

Every connector operation must maintain these invariants:

- the user callback runs exactly once;
- cancellation propagates to the current lower `connect_operation`;
- if a lower stream has already arrived, cancellation or failure closes it;
- completion after cancellation is ignored except for required cleanup;
- success transfers the stream exactly once to the callback.

Errors should retain the failed phase and hop while exposing a stable public
category. Useful phases include `proxy_connect`, `socks_greeting`,
`socks_authentication`, `socks_connect`, and `http_connect`. Diagnostics may
name endpoints and status codes, but must redact credentials and
`Proxy-Authorization` values.

## Performance and Resource Boundaries

The type-erased connector dispatch occurs once per connection phase, not once
per byte read or written after a tunnel is established. Its overhead is
negligible relative to DNS, TCP, and proxy round trips.

The implementation should nevertheless keep resource limits explicit:

- one operation state and one bounded handshake buffer per in-flight hop;
- configurable HTTP CONNECT response-header limit;
- no unbounded accumulation while parsing proxy responses;
- no background route activity after cancellation or completion.

The first implementation should favour clear state machines and bounded
buffers over pooling or micro-optimisations. Measurements can later justify
reusing proxy sessions or reducing allocation in hot client workloads.

## Alternatives Considered

### `make_socks5_connector(...)` functions

Factories obscure the owning layer and do not match the established
`tcp_listener` / `tls::listener` adapter style. Concrete constructors make the
composition, option ownership, and rvalue conversion visible in the type
system. Small helper functions could be added later only if they materially
improve a demonstrated configuration use case.

### One central `outbound_connector` with a route-kind switch

This works for direct TCP and one proxy but becomes a growing conditional tree
for SOCKS, CONNECT, TLS-to-proxy, custom tunnels, and observability wrappers.
It also makes a multi-hop route harder to model than the connection it creates.
Composition localises each protocol state machine to one class.

### A public inheritance hierarchy

The IO design intentionally uses type erasure rather than a common transport
base. The same decision applies here: concrete connectors should not expose
their implementation inheritance as public API.

### A pipe or fluent route DSL

An expression such as `direct | socks5(options) | connect(options)` could be
pleasant once the types are stable, but it adds operators, temporaries, and
error-prone ownership rules before the core protocol contract is proven. The
nested-constructor form is explicit and already familiar in this library.

## Implementation Plan

1. Add `io/stream_connector.hpp` and its type-erased implementation, reusing
   `tcp_endpoint`, `connect_options`, `connect_callback`, and
   `connect_operation`.
2. Add the rvalue conversion from `tcp_connector` and tests proving that the
   adapter preserves direct connection behaviour and cancellation.
3. Add `proxy/http_connect_connector` with a bounded response parser,
   handshaking timeout, redacted diagnostics, and unit/integration tests.
4. Route HTTPS HTTP-client establishment through a supplied
   `stream_connector`, then include route identity in its connection-pool key.
5. Add `proxy/socks5_connector`, starting with CONNECT and explicit remote DNS
   semantics, then optional username/password authentication.
6. Add configuration parsing and a route selector only after there are at
   least two real selection policies to support.

## Test Plan

- direct TCP through `stream_connector` reaches the same endpoint and has the
  same cancellation outcome as `tcp_connector`;
- a SOCKS connector asks its lower connector for the SOCKS endpoint, then
  sends the expected greeting and target representation;
- an HTTP CONNECT connector asks its lower connector for the proxy endpoint,
  sends authority-form `CONNECT`, and returns the same successful stream;
- SOCKS-to-CONNECT verifies the ordered lower targets and handshakes without
  any connector knowing the concrete type below it;
- cancellation at DNS, lower TCP, SOCKS handshake, and CONNECT parsing closes
  the candidate stream and completes once;
- route deadlines do not multiply across hops;
- proxy authentication and authorization are absent from error diagnostics;
- TLS over a CONNECT tunnel uses the logical target for SNI and hostname
  verification;
- HTTP connection pooling keeps direct and differently routed connections
  separate.

## Source Documents

- [Outbound connector and proxy routes](outbound-connector-and-proxy-routes.md)
- [Shared protocol foundation](shared-protocol-foundation.md)
- [Transport abstractions](../design/transport-abstractions.md)
- [Protocol composition](../design/protocol-composition.md)
- [TLS stream API](../../include/uvpp/protocols/tls/stream.hpp)
- [TCP connector API](../../include/uvpp/protocols/io/tcp_connector.hpp)
