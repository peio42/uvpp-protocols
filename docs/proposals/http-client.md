# HTTP Client Proposal

Status: Initial HTTP/HTTPS one-shot client plus request and response streaming
implemented; pooling, redirects, proxying, and broader timeout coverage remain
open

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
- Implemented: initial `uvp::http::client`, `client_options`, cancellable
  request operation, URL + DNS + TCP orchestration, HTTP/1.1 request writer,
  buffered response parser, response header/body limits, malformed response
  handling, chunked/content-length/EOF body handling, HEAD/204/304 no-body
  semantics, and one-shot `GET` over `http://` URLs.
- Implemented: reusable `uvp::io::tcp_connector` below HTTP, with sequential
  address attempts, typed connect errors, byte-stream conversion, and
  cancellation.
- Implemented: HTTPS one-shot requests through `uvp::tls::connect()`, SNI from
  the URL host, hostname verification by default, configurable CA file/path,
  and ALPN `http/1.1`.
- Implemented: response-body streaming with `on_response_headers`, `on_data`,
  and `on_complete`, covering content-length, chunked, EOF-delimited, and
  bodyless responses without buffering the full body first.
- Implemented: request-body streaming for fixed `Content-Length` and
  HTTP/1.1 chunked uploads, upload queue backpressure, request headers, and
  cancellation while the upload is still open.
- Implemented: opt-in HTTP/1.1 keep-alive pool keyed by origin, reuse after
  response consumption, `Connection: close` handling, idle timeout, explicit
  idle close, and per-origin idle connection limit.
- Implemented: initial client phase timeouts for DNS resolution, TCP connect,
  response headers, and response body transfer.
- Drafted separately or still open: byte-stream lifetime controls for cleaner
  idle pool liveness, TLS stream/listener support, HTTP/2 support, WebSocket
  client support, redirects, proxying, broader phase timeouts, response
  pause/resume controls, and more advanced upload/response concurrency.

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

The initial implemented subset exposes request and response streaming:

```cpp
auto req = client.request(
  uvp::http::method::post,
  "https://example.com/upload");

req.content_length(total_size)
  .header("content-type", "application/octet-stream")
  .on_response_headers([](uvp::http::response_head const& h) {
    // status, headers
  })
  .on_data([](std::span<std::byte const> chunk) {
    // borrowed response body chunk
  })
  .on_complete([](uvp::result<void> done) {
    // complete, cancelled, timed out, or failed
  });

auto body = req.start();
body.write(chunk1);
body.write(chunk2);
body.end();
```

This is enough for large downloads, SSE-style clients, and fixed or chunked
uploads. It also gives the future WebSocket client a path to observe response
headers before an upgrade handoff.

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

The client uses an outbound connection helper over uvpp TCP:

```text
resolve host
  -> connect TCP
    -> optional TLS connect
      -> HTTP session
```

The reusable `uvp::io::tcp_connector` owns:

- TCP connect attempts;
- remote/local endpoint metadata;
- cancellation before or during connect;
- conversion to `uvp::io::byte_stream`.

Connection establishment should be factored below HTTP so SMTP, Redis, MQTT, or
database adapters can reuse the same direction later.

Open hardening:

- Happy Eyeballs / IPv6-IPv4 racing;
- richer diagnostics for the last failed candidate.

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

- implemented first as a buffered parser for the one-shot client;
- parses status line, headers, chunked body, content-length body, and EOF body;
- enforces header/body limits;
- treats HEAD, 1xx, 204, and 304 responses as bodyless;
- detects malformed status lines, malformed headers, invalid chunk sizes,
  unterminated chunks, incomplete content-length bodies, and incomplete
  headers.

The response-streaming path currently reuses a small incremental HTTP/1.1 client
parser for status, headers, content-length, chunked, EOF-delimited, and
bodyless responses. Future hardening may replace the internal parser with an
adapter backed by `llhttp`, without changing the high-level response
vocabulary.

Writer direction:

- serialize request line, headers, and body framing;
- default `Host`, `User-Agent` policy, `Connection`, and `Accept` behavior;
- support fixed `Content-Length`, chunked upload, and no-body requests;
- reject illegal body combinations for the selected method or framing policy.

The HTTP/1.1 session owns parser/writer state and one in-flight request at a
time per connection.

### Connection Pool

Connection pooling is implemented for the initial HTTP/1.1 client as an opt-in
pool. It is disabled by default so existing code that runs the loop until idle
does not keep idle sockets alive unexpectedly.

Clean idle-pool liveness depends on
[byte stream lifetime controls](byte-stream-lifetime-controls.md). Once
`uvp::io::byte_stream` forwards `ref()`, `unref()`, and `has_ref()`, the pool can
mark idle transports as unreferenced without closing them, then reference them
again when checked out for reuse.

Pool key should include at least:

- implemented now: scheme;
- implemented now: host;
- implemented now: port;
- proxy identity, once proxying exists;
- TLS verification and ALPN-relevant policy.

HTTP/1.1 pool behavior:

- implemented: one in-flight request per connection;
- implemented: reuse only after the previous response is fully consumed;
- implemented: honor `Connection: close`;
- implemented: close idle connections after an idle timeout;
- implemented: bound idle connections per origin.

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

- implemented for fixed-length and chunked request bodies;
- implemented with bounded queue backpressure through `stream_write_result` and
  `on_drain`;
- implemented upload cancellation before the response path begins;
- surface write-side failures once.

Download streaming:

- implemented for response bodies with content-length, chunked, EOF-delimited,
  and bodyless framing;
- deliver body chunks incrementally as callback-scoped borrowed spans;
- allow the user to stop the transfer by cancelling the request operation;
- define whether unread bodies are drained, discarded, or make the connection
  unreusable;
- support buffered collection as a wrapper over streaming.

### Timeouts

Timeouts should be phase-aware:

- implemented for the one-shot client: DNS resolution timeout, TCP connect
  timeout, response header timeout, and buffered response body timeout;
- implemented for response streaming: DNS resolution timeout, TCP connect
  timeout, response header timeout, and response body transfer timeout;
- still open: overall request deadline;
- still open: TLS handshake timeout integration for HTTPS;
- still open: request header/body write timeout or idle timeout;
- still open: streaming response body idle timeout and pause-aware timeout
  semantics;
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

1. [x] shared URL dependency usable by HTTP client;
2. [x] DNS resolution API;
3. [x] HTTP/1.1 one-shot `GET` over plain HTTP with buffered body limit;
4. [x] outbound TCP connect helper extraction;
5. [x] HTTPS via TLS connect and hostname verification;
6. [x] streaming response body;
7. [x] streaming request body;
8. [x] basic keep-alive and pool reuse for HTTP/1.1;
9. byte-stream lifetime controls for idle pool liveness;
10. cancellation and phase-specific timeout coverage;
11. redirects with conservative policy;
12. proxy design spike or minimal HTTP CONNECT support.

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
- [Byte stream lifetime controls](byte-stream-lifetime-controls.md)
- [Shared URL module](shared-url-module.md)
- [TLS support](../archive/tls-support.md)
- [HTTP/2 support](http2-support.md)
- [Protocol composition](../design/protocol-composition.md)
- [Dependency decisions](../design/dependency-decisions.md)
- [Project scope](../design/project-scope.md)
