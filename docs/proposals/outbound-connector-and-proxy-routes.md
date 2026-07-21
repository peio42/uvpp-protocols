# Outbound Connector and Proxy Routes Proposal

Status: Proposed after Milestone 6 HTTP forward proxy support

## Context

Client-side protocol modules need a reusable way to open an outbound
`uvp::io::byte_stream` to a logical target. Today the HTTP client composes DNS,
TCP, optional TLS, and HTTP request writing directly. That is acceptable for the
first HTTP client slices, but proxy support makes the transport route more
interesting:

```text
target endpoint
  -> direct TCP
  -> HTTP CONNECT tunnel
  -> SOCKS tunnel
  -> future custom tunnel
  -> byte_stream to the logical target
```

For tunnel-style proxies, the result is naturally a `byte_stream` that upper
protocols can treat as connected to the logical target. HTTP, WebSocket, SMTP,
Redis, MQTT, or database adapters can then compose TLS or their own protocol on
top without knowing the tunnel details.

Forward HTTP proxying for clear `http://` requests is different. It does not
create a transparent byte stream to the origin server; it creates a connection
to the proxy and requires HTTP absolute-form request targets. That case belongs
in the HTTP client layer, while generic tunnel routes belong in a reusable IO
foundation.

## Decision

Create a separate design for outbound route construction and tunnel proxies.
Do not force clear HTTP forward proxying into that abstraction.

For Milestone 6, the HTTP client should handle the clear HTTP proxy slice
directly:

```text
http://target
  -> TCP connection to HTTP proxy
  -> HTTP request with absolute-form target
```

After that, introduce a reusable outbound connector for direct connections and
tunnel routes:

```text
logical target
  -> route policy
  -> direct TCP or proxy TCP
  -> optional tunnel handshake
  -> uvp::io::byte_stream
```

## Goals

- Provide a reusable outbound connector below HTTP.
- Return a normal `uvp::io::byte_stream` for direct and tunnel routes.
- Support HTTP CONNECT as the first tunnel route.
- Leave room for SOCKS4/SOCKS5 and custom tunnel adapters.
- Preserve uvpp-style cancellation, timeout, and exactly-once completion.
- Keep target identity distinct from proxy identity.
- Give upper protocols enough metadata to pool and diagnose routed streams.
- Avoid making HTTP depend on TLS, SOCKS, or proxy-specific protocol details.

## Non-Goals

- Replacing `uvp::io::tcp_connector` immediately.
- Browser proxy environment behavior by default.
- Full HTTP forward proxy handling for clear HTTP requests.
- SOCKS support in the first tunnel implementation.
- Proxy auto-discovery, PAC files, or platform proxy settings.
- Authentication frameworks beyond explicit proxy headers/options.

## Proposed Public Shape

Initial sketch:

```cpp
namespace uvp::io {

struct connect_target {
  std::string host;
  std::uint16_t port = 0;
};

enum class route_kind {
  direct,
  tunnel,
  http_forward_proxy,
};

struct proxy_endpoint {
  std::string scheme; // "http", later "socks5", ...
  std::string host;
  std::uint16_t port = 0;
};

struct route_result {
  byte_stream stream;
  connect_target target;
  route_kind kind = route_kind::direct;
  std::optional<proxy_endpoint> proxy;
};

class outbound_connector {
public:
  explicit outbound_connector(uv::loop& loop);

  connect_operation connect(
    connect_target target,
    outbound_options options,
    std::function<void(uvp::result<route_result>)> callback);
};

}
```

Exact names can change during implementation. The important separation is:

- tunnel routes return a stream that behaves like a connection to `target`;
- forward HTTP proxy routes expose `route_kind::http_forward_proxy` so HTTP can
  choose absolute-form request targets instead of origin-form targets.

## Route Types

### Direct TCP

Direct route:

```text
DNS target -> TCP target -> byte_stream
```

This is equivalent to the current `tcp_connector` behavior and should probably
reuse it internally.

### HTTP CONNECT Tunnel

Tunnel route:

```text
DNS proxy -> TCP proxy -> CONNECT target:port -> 2xx -> byte_stream
```

After a successful `CONNECT`, the connector returns the same underlying
`byte_stream`, now representing an opaque tunnel to the logical target.

For HTTPS, upper layers then compose TLS over that stream:

```text
CONNECT tunnel byte_stream
  -> uvp::tls::connect(SNI = target host)
  -> HTTP/1.1 or later HTTP/2
```

TLS verification, SNI, and ALPN must use the logical target, not the proxy.

### SOCKS Tunnel

Future route:

```text
DNS proxy -> TCP proxy -> SOCKS greeting -> CONNECT target -> byte_stream
```

SOCKS naturally fits the same connector shape because it produces a transparent
tunnel after the handshake.

### Clear HTTP Forward Proxy

Clear HTTP proxying is not a transparent tunnel:

```text
DNS proxy -> TCP proxy -> HTTP request absolute-form
```

The stream is connected to the proxy, not to the target. HTTP must write:

```http
GET http://example.com/path?q=1 HTTP/1.1
Host: example.com
```

This is why the route result needs `route_kind` metadata. A pure
`byte_stream`-only return type would hide a protocol-level decision that HTTP
must make.

## Pooling

Connection pools must distinguish:

- logical target scheme, host, and port;
- route kind;
- proxy identity;
- tunnel protocol;
- TLS verification policy and ALPN-relevant settings once TLS is applied.

Direct and tunnel routes can usually be pooled as target connections. Clear
HTTP forward proxy connections may be pooled by proxy plus HTTP client rules,
but request routing and absolute-form behavior remain HTTP-layer concerns.

## Timeouts and Cancellation

The outbound connector should expose phase-specific timeout detail:

- target DNS;
- proxy DNS;
- proxy TCP connect;
- tunnel handshake;
- cancellation during any phase.

Completion should be exactly once. If cancellation wins after the proxy socket
opens but before tunnel completion, the connector owns closing the lower stream.

## Errors

Suggested error cases:

- invalid target;
- invalid proxy configuration;
- proxy DNS failure;
- proxy TCP connect failure;
- tunnel handshake timeout;
- malformed proxy response;
- HTTP CONNECT rejected;
- proxy authentication required;
- unsupported proxy scheme;
- cancelled.

Errors should preserve source categories where useful while presenting a stable
IO/proxy connector category to callers.

## HTTP Client Integration

Milestone 6 should keep HTTP forward proxy work in the HTTP client:

- `http://` through `http://proxy` uses absolute-form request targets;
- pool keys include proxy identity;
- redirects re-run proxy selection when the target URL changes;
- HTTPS through `CONNECT` can later use the outbound connector tunnel result.

Once `outbound_connector` exists, the HTTP client can use it for:

- direct TCP;
- HTTPS via HTTP CONNECT;
- future SOCKS tunnels;
- future WebSocket client and HTTP/2 client transport setup.

## Tests

Add coverage for:

- direct route returns a usable `byte_stream`;
- HTTP CONNECT sends the correct authority-form target;
- CONNECT 2xx returns a tunneled `byte_stream`;
- CONNECT non-2xx fails with a proxy error;
- malformed CONNECT response fails;
- cancellation during proxy DNS/connect/CONNECT closes handles;
- tunnel timeout reports the tunnel phase;
- TLS over CONNECT uses target SNI and target hostname verification;
- HTTP client pool keys separate direct, proxy, and tunnel routes.

## Source Documents

- [HTTP client](../archive/http-client.md)
- [DNS resolution](../archive/dns-resolution.md)
- [Transport abstractions](../design/transport-abstractions.md)
- [Protocol composition](../design/protocol-composition.md)
