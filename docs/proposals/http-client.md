# HTTP Client Proposal

Status: Draft, not implemented

## Decision

The official HTTP client should be a uvpp-native implementation, not a libcurl
wrapper.

libcurl may still be useful later as an optional, batteries-included adapter for
applications that want broad client behavior with minimal tuning. It must not
define the canonical `uvp::http` API, own the shared HTTP vocabulary, or become
the required path for HTTPS, WebSocket, HTTP/2, redirects, proxying, or
streaming.

The native client keeps the project coherent:

- uvpp owns the reactor integration, sockets, timers, and lifetime;
- DNS, TCP, TLS, ALPN, HTTP/1.1, HTTP/2, WebSocket, and future layers remain
  composable;
- public types stay independent from third-party client backends;
- server-side and client-side HTTP share vocabulary without sharing accidental
  session assumptions.

## Context

The project already has an HTTP/1.1 server, shared request/response vocabulary,
transport abstractions, and a planned TLS module. Client-side HTTP is the next
major program after TLS because it needs the same foundations in the opposite
direction:

```text
URL -> DNS -> TCP -> optional TLS/ALPN -> HTTP client session
```

For HTTPS and later HTTP/2, the client must be able to compose with
`uvp::tls::connect()` and inspect ALPN selection. For WebSocket client support,
the HTTP client must also expose an upgrade path that can hand the live
transport/session to the WebSocket module.

## Current State

- Implemented: HTTP/1.1 server, shared HTTP method/status/header vocabulary,
  request/response objects, `uvp::io::byte_stream`, and listener composition.
- Drafted separately: TLS stream/listener support, shared URL module, HTTP/2
  support, and WebSocket client support.
- Not implemented: public HTTP client API, DNS resolution, outbound TCP connect
  helper, client TLS integration, connection pooling, request writer, response
  parser, redirects, proxying, or streaming upload/download API.

## Goals

- Provide a simple one-shot API for common requests.
- Provide a streaming API for large uploads, large downloads, and incremental
  protocols.
- Keep all operations asynchronous and loop-bound.
- Reuse shared HTTP vocabulary where it is version-neutral.
- Keep the transport stack explicit internally while allowing URL-based
  convenience at the edge.
- Preserve a path toward HTTP/2 multiplexing without exposing HTTP/1-only
  assumptions as permanent API.
- Make cancellation, timeout, and error ownership visible.

## Public API Levels

The client should expose two levels.

### Simple Fetch-Like API

The high-level API owns the complete transfer and returns a buffered response:

```cpp
uvp::http::client client(loop);

client.get("https://api.example.com/users/42",
  [](uvp::result<uvp::http::response> r) {
    if (!r) {
      return;
    }

    auto body = r->body();
  });
```

Expected convenience methods:

- `get(url, callback)`;
- `head(url, callback)`;
- `post(url, body, callback)`;
- `put(url, body, callback)`;
- `delete_(url, callback)` or another spelling that avoids the C++ keyword;
- `fetch(request_options, callback)` as the generic entry point.

The buffered API should have explicit limits:

- maximum response body size;
- maximum header section size;
- maximum redirect count;
- per-request and phase-specific timeouts.

If a response exceeds the configured buffered limit, the request fails with a
typed HTTP client error instead of silently growing memory.

### Streaming API

The streaming API exposes request body writes and response body reads:

```cpp
auto req = client.request(
  uvp::http::method::post,
  "https://example.com/upload");

req.header("content-type", "application/octet-stream");

req.on_response_headers([](uvp::http::response_head const& h) {
  // status, headers
});

req.on_data([](std::span<std::byte const> chunk) {
  // response body stream
});

req.on_complete([](uvp::result<void> done) {
  // finished or failed
});

auto body = req.start();

body.write(chunk1);
body.write(chunk2);
body.end();
```

The exact owning types still need naming, but the lifecycle should be explicit:

- request metadata is configured before `start()`;
- `start()` begins DNS/connect/TLS/write orchestration;
- the returned upload body owns asynchronous request body writes;
- response headers arrive once;
- response data arrives as borrowed chunks with callback-scoped lifetime;
- completion fires exactly once with success, cancellation, timeout, protocol
  failure, or transport failure.

## Core Components

### URL and Request Target

The client should depend on the shared URL module for `http` and `https` URLs.
The request writer should derive:

- scheme;
- host;
- port;
- path and query request target;
- userinfo rejection or explicit policy;
- default port selection;
- TLS server name.

Typed endpoint overloads may be added later for preconnected streams or unusual
transports, but URL input is the primary convenience path.

### DNS Resolution

DNS must be a separate reusable module, tracked in
[DNS resolution](dns-resolution.md).

The HTTP client should not bury resolver behavior in HTTP-only code. It needs a
resolver interface so tests, custom policies, and future Happy Eyeballs support
can be plugged in without changing request APIs.

Initial HTTP client needs:

- resolve host and service/port asynchronously on the client loop;
- preserve address ordering or apply a documented selection policy;
- surface DNS failures as DNS or HTTP client error categories;
- allow request timeout and cancellation to stop pending resolution work.

### Sockets and Connection

The client needs an outbound connection helper over uvpp TCP:

```text
resolve host
  -> connect TCP
    -> optional TLS connect
      -> HTTP session
```

The connection layer should own:

- TCP connect attempts;
- remote/local endpoint metadata;
- connect timeout;
- cancellation before or during connect;
- conversion to `uvp::io::byte_stream`.

Connection establishment should be factored below HTTP so SMTP, Redis, MQTT, or
database adapters can reuse the same direction later.

### TLS and ALPN

For HTTPS, the client composes over `uvp::tls::connect()`:

```text
TCP byte_stream
  -> uvp::tls::connect(client_context)
    -> clear byte_stream
      -> HTTP client session
```

The HTTP client should configure:

- SNI from the URL host;
- hostname verification by default;
- ALPN list, initially `{"http/1.1"}` and later `{"h2", "http/1.1"}`;
- per-request or per-client TLS policy hooks.

ALPN selection decides whether the connection enters the HTTP/1.1 or HTTP/2
session path. If only HTTP/1.1 is implemented, selecting an unsupported ALPN
value should fail clearly.

### HTTP/1.1 Parser and Writer

The first native milestone should implement HTTP/1.1.

Parser direction:

- reuse `llhttp` behind a private client response parser adapter if practical;
- parse status line, headers, chunked body, content-length body, and EOF body;
- enforce header/body limits;
- detect malformed responses and conflicting framing.

Writer direction:

- serialize request line, headers, and body framing;
- default `Host`, `User-Agent` policy, `Connection`, and `Accept` behavior;
- support fixed `Content-Length`, chunked upload, and no-body requests;
- reject illegal body combinations for the selected method or framing policy.

The HTTP/1.1 session owns parser/writer state and one in-flight request at a
time per connection.

### Connection Pool

Connection pooling should be designed in the first client proposal even if it
lands after the simplest request path.

Pool key should include at least:

- scheme;
- host;
- port;
- proxy identity, once proxying exists;
- TLS verification and ALPN-relevant policy.

HTTP/1.1 pool behavior:

- one in-flight request per connection;
- reuse only after the previous response is fully consumed or discarded safely;
- honor `Connection: close`;
- close idle connections after an idle timeout;
- bound total connections and per-origin connections.

HTTP/2 pool behavior, later:

- one connection can carry multiple concurrent streams;
- pool checkout returns stream capacity rather than exclusive connection
  ownership;
- GOAWAY and max-concurrent-streams affect reuse decisions.

### Keep-Alive

Keep-alive is part of pooling but has its own protocol rules:

- HTTP/1.1 defaults to persistent connections unless `Connection: close` or
  framing prevents reuse;
- HTTP/1.0 reuse requires explicit `Connection: keep-alive`;
- unread response bodies usually make the connection unreusable;
- parser errors, TLS errors, and timeout failures close the connection.

The public client should not promise reuse for any individual request, only
allow the pool to reuse when protocol state is clean.

### HTTP/2 Multiplexing

HTTP/2 is not part of the first HTTP client implementation, but the API must
leave room for it.

Design constraints:

- callbacks describe a request/response exchange, not exclusive connection
  ownership;
- cancellation is per request stream where the protocol supports it;
- connection-level errors fail all active streams;
- response body delivery does not assume socket read ordering maps to one
  request;
- connection pool logic can represent stream capacity.

When HTTP/2 is implemented, use `libnghttp2` behind a private adapter as
described in [HTTP/2 support](http2-support.md).

### Redirects

Redirect handling belongs in the high-level client layer, not the low-level
HTTP session.

Initial policy:

- disabled or conservative by default unless the API opts into automatic
  redirects;
- maximum redirect count;
- preserve method only where HTTP semantics require it;
- convert `POST` to `GET` for 301/302/303 only if the chosen policy allows the
  browser-compatible behavior;
- strip sensitive headers when origin changes;
- re-run DNS, proxy, TLS, and pool selection for each redirect target;
- fail on unsupported schemes.

Streaming uploads should not be automatically replayed unless the body source
is explicitly replayable.

### Proxying

Proxy support should be planned but not required for the first milestone.

Needed shapes:

- HTTP proxy for clear HTTP requests using absolute-form request targets;
- HTTPS through HTTP proxy via `CONNECT`;
- proxy authentication hooks later;
- pool key includes proxy identity;
- `NO_PROXY` or environment behavior only if explicitly adopted.

SOCKS proxying can remain out of scope until there is a concrete need.

### Upload and Download Streaming

Upload streaming:

- support fixed-length and chunked request bodies;
- provide backpressure through asynchronous write callbacks or a bounded queue;
- allow upload cancellation while response headers may or may not have arrived;
- surface write-side failures once.

Download streaming:

- deliver body chunks incrementally;
- allow the user to stop reading and cancel the request;
- define whether unread bodies are drained, discarded, or make the connection
  unreusable;
- support buffered collection as a wrapper over streaming.

### Timeouts

Timeouts should be phase-aware:

- overall request deadline;
- DNS resolution timeout;
- TCP connect timeout;
- TLS handshake timeout;
- request header/body write timeout or idle timeout;
- response header timeout;
- response body idle timeout;
- pool checkout timeout.

Timeout errors should include the phase where practical.

### Errors and Categories

The client should surface typed errors without exposing implementation
dependencies.

Likely categories:

- URL parse or unsupported scheme;
- DNS resolution failure;
- TCP connect failure;
- TLS handshake or verification failure;
- HTTP protocol parse/framing failure;
- unsupported protocol version or ALPN;
- timeout by phase;
- cancellation;
- redirect policy failure;
- body limit exceeded;
- proxy failure;
- pool closed or client closed.

Errors that originate in `uvp::io`, `uvp::tls`, or DNS should preserve enough
source information for diagnostics while presenting a coherent HTTP client
result.

### Cancellation

Cancellation must be explicit and race-safe.

Expected behavior:

- request handle can cancel before DNS, during connect, during TLS handshake,
  while uploading, while downloading, or while queued for a pool connection;
- `on_complete` fires exactly once with cancellation unless completion already
  won the race;
- cancellation closes or returns the underlying connection according to whether
  protocol state is reusable;
- destroying an owning request handle either cancels or detaches by a documented
  policy, with preference for visible cancellation.

## Optional libcurl Adapter

A future adapter may provide broad libcurl-backed behavior under a separate
module or explicit factory. It should be treated like an integration backend,
not as the native client:

```cpp
auto client = uvp::http::curl_client(loop, options);
```

Constraints:

- no libcurl headers in canonical `uvp::http` public headers;
- no libcurl error codes as the primary public error model;
- no requirement that native HTTP features be expressible through libcurl;
- no public backend selector until there are at least two supported and tested
  client implementations.

## Initial Milestone Slice

Suggested first implementation slice:

1. shared URL dependency usable by HTTP client;
2. DNS resolution API and outbound TCP connect helper;
3. HTTP/1.1 one-shot `GET` over plain HTTP with buffered body limit;
4. HTTPS via TLS connect and hostname verification;
5. streaming response body;
6. streaming request body;
7. basic keep-alive and pool reuse for HTTP/1.1;
8. cancellation and phase-specific timeout coverage;
9. redirects with conservative policy;
10. proxy design spike or minimal HTTP CONNECT support.

HTTP/2 should follow only after ALPN, pooling, and stream ownership are settled.

## Out Of Scope For First Implementation

- HTTP/2 and HTTP/3 transfer support.
- Browser cache, cookie jar, HSTS, CSP, CORS, or full browser fetch semantics.
- Transparent content decompression unless explicitly added as a small optional
  helper.
- Authentication frameworks beyond raw header configuration.
- Automatic replay of non-replayable streaming uploads.
- SOCKS proxying.
- Public backend selection.

## Source Documents

- [DNS resolution](dns-resolution.md)
- [Shared URL module](shared-url-module.md)
- [TLS support](../archive/tls-support.md)
- [HTTP/2 support](http2-support.md)
- [Protocol composition](../design/protocol-composition.md)
- [Dependency decisions](../design/dependency-decisions.md)
- [Project scope](../design/project-scope.md)
