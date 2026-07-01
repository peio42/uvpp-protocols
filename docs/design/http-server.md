# HTTP Server Design

## Target

The first implementation target is an HTTP/1.1 server for small services,
embedded admin endpoints, local agents, and protocol gateways.

The API should make simple cases terse:

```cpp
uvp::http::server srv(loop);

srv.get("/health", uvp::http::body::none{}, [](uvp::http::request& req, uvp::http::response& res) {
  res.json(uvp::json{{"status", "ok"}});
});

srv.listen("127.0.0.1", 8080);
```

It should keep common routes lightweight while making request body handling
explicit at route declaration time. That body policy is how the server decides
whether to dispatch after headers, buffer bytes, parse a typed body, stream
chunks, or reject a body entirely.

## Public Types

Current public types:

- `uvp::http::server`
- `uvp::http::server_options`
- `uvp::http::request`
- `uvp::http::response`
- `uvp::http::deferred_response`
- `uvp::http::streaming_response`
- `uvp::http::stream_write_result`
- `uvp::http::connection_info`
- `uvp::http::upgrade_request`
- `uvp::http::router`
- `uvp::http::headers`
- `uvp::http::method`
- `uvp::http::status`
- `uvp::http::route_params`
- `uvp::http::query_params`
- `uvp::http::body::none`
- `uvp::http::body::bytes`
- `uvp::http::body::text`
- `uvp::http::body::stream`
- `uvp::http::request_body_stream`

The aggregate include should be:

```cpp
#include <uvpp/protocols/http.hpp>
```

## Server

`server` owns:

- one or more `uvp::io::stream_listener` objects, possibly created by
  convenience TCP `listen()` overloads;
- a route table;
- accepted HTTP connection sessions;
- server-level hooks;
- server options.

Sketch:

```cpp
namespace uvp::http {

class server {
public:
  explicit server(uv::loop& loop);
  server(uv::loop& loop, server_options options);

  template<class BodyPolicy, class Handler>
  server& get(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& post(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& put(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& patch(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& delete_(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& head(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& options(std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class BodyPolicy, class Handler>
  server& route(method method, std::string_view pattern, BodyPolicy body, Handler&& handler);

  template<class Handler>
  server& not_found(Handler&& handler);

  template<class Handler>
  server& on_exception(Handler&& handler);

  void listen(std::string_view host, unsigned int port);
  void listen(uvp::io::stream_listener listener);
  void close();
};

}
```

`listen()` should bind and start listening. Immediate libuv failures throw
`uv::error`.

The host/port overload is a convenience wrapper around
`uvp::io::tcp_listener`. Unix socket support should use the generic
listener overload:

```cpp
srv.listen(
  uvp::io::pipe_listener{loop}
    .bind("/run/my-app.sock"));
```

`close()` should stop accepting new connections and close active sessions
according to the configured close policy.

## Routing

The route model supports:

- exact paths: `/health`;
- named parameters: `/users/:id`;
- wildcard tail segments: `/static/*path`;
- method-specific handlers;
- global and scoped fallback `not_found` handlers;
- global and scoped application exception handlers.

Route matching should operate on the decoded path component, not the raw target.
The raw target remains available on `request`.

The router is implemented as a segment trie rather than a linear scan. The trie
matches in time proportional to the number of path segments, with route targets
stored at terminal nodes and method-specific handlers attached to each target.
Matching priority is:

1. static segment;
2. named parameter segment;
3. wildcard tail segment.

Static branches are tried first, but matching still falls back to a parameter or
wildcard branch if the static branch does not produce a complete match. This
preserves intuitive behavior for combinations such as `/users/me/details` and
`/users/:id`.

HTTP route registration validates patterns early:

- named parameters must have a non-empty name;
- wildcard tails must have a non-empty name and be the final segment;
- percent escapes in route patterns must be valid;
- duplicate method + pattern registrations are rejected;
- conflicting parameter names at the same tree position, such as `/users/:id`
  and `/users/:name`, are rejected.

Request paths are parsed into raw and percent-decoded segments once per
request. The default matching mode uses decoded segments. Percent-decoding is
segment-local: `%2F` decodes to `/` inside the segment value but never creates
another segment. `+` remains a literal plus sign in path segments. Malformed
percent escapes reject the request with `400 Bad Request`.

Upgrade route patterns follow the same segment parser and are pre-parsed when
registered, so upgrade matching does not re-parse every candidate pattern for
each upgrade request.

Servers can opt into raw route matching with
`server_options::route_path_matching(route_path_matching::raw)`. Raw mode keeps
captured route parameters raw, while still validating percent escapes and
keeping decoded segments available for application inspection.

The trie also drives method-aware HTTP behavior:

- if a path matches another method, the server returns `405 Method Not Allowed`
  with an `Allow` header;
- `HEAD` falls back to a matching `GET` route and suppresses the response body;
- `OPTIONS` is answered automatically for known paths when no explicit
  `OPTIONS` route is registered.

Route groups, request hooks, response observers, scoped fallbacks, and
mountable routers build on the same trie.

Response observers are retained by stable handles once a response slot is
created, so deferred and streaming responses can complete after later hook
registrations without holding invalidated pointers into router storage.

Handlers receive request and response references, plus a body argument when the
body policy produces one:

```cpp
using handler = std::function<void(request&, response&)>;
using bytes_handler = std::function<void(request&, response&, std::span<const std::byte>)>;
using text_handler = std::function<void(request&, response&, std::string_view)>;
using stream_handler = std::function<void(request&, response&, request_body_stream&)>;
using exception_handler = std::function<void(request&, response&, std::exception_ptr)>;
```

The implementation may use templates at registration time, but stored handlers
can be type-erased until a proven performance issue appears.

## Body Policies

Routes declare a body policy explicitly:

```cpp
srv.get("/health", body::none{}, handler);
srv.post("/echo", route_options{}.max_body_bytes(64 * 1024), body::bytes{}, handler);
srv.post("/message", route_options{}.max_body_bytes(64 * 1024), body::text{}, handler);
srv.post("/json", route_options{}.max_body_bytes(64 * 1024), body::json<my_type>{}, handler);
srv.post("/events", body::stream{}, handler);
```

The body policy is part of the route contract:

- `body::none{}` dispatches after request headers and rejects request bodies;
- `body::bytes{}` buffers the body, then dispatches with
  `std::span<const std::byte>`;
- `body::text{}` buffers the body, then dispatches with `std::string_view`;
- `body::json<T>{}` buffers the body, validates a JSON content type, parses
  through `uvp::json`, converts with nlohmann `from_json`, then dispatches
  with `const T&`;
- `body::stream{}` dispatches after request headers and provides a
  `request_body_stream&`.

`body::json<>` dispatches with `const uvp::json&`. JSON request policies are
not inferred from handler signatures; routes declare them explicitly because
parsing and typed conversion are part of the route contract. Future typed
policies such as multipart should use the same explicit route declaration
shape. Multipart work is tracked in
[multipart handling](../proposals/multipart-handling.md).

Route-level options extend the body contract with operational metadata:

```cpp
srv.post(
  "/upload",
  route_options{}
    .max_body_bytes(20 * 1024 * 1024)
    .body_timeout(std::chrono::seconds{30}),
  body::stream{},
  handler);
```

Use `route_options::max_body_bytes(...)` when a route needs its own request
body limit. Otherwise the server falls back to
`server_options::max_body_bytes()`. Use
`route_options::body_timeout(...)` when a route needs a body receive timeout
different from `server_options::body_timeout()`.

Configured body limits must be greater than zero. A route that does not accept a
request body should use `body::none{}` instead of a zero byte limit. The current
route-level implementation still uses an internal `0` value to mean "inherit the
server default"; replacing that sentinel with an explicit unset state is tracked
in [route body limit inheritance](../proposals/route-body-limit-inheritance.md).

Convenience overloads infer the body policy from the handler signature when the
mapping is unambiguous:

```cpp
srv.get("/health", handler); // equivalent to body::none{}
srv.post("/echo", bytes_handler); // equivalent to body::bytes{}
srv.post("/message", text_handler); // equivalent to body::text{}
srv.post("/events", stream_handler); // equivalent to body::stream{}
```

Inference stops at `body::none{}`, `body::bytes{}`, `body::text{}`, and
`body::stream{}`. Typed policies should be declared explicitly.

## Request

`request` is owned by the active connection while a handler is running.
Buffered request bodies are available only on routes declared with
`body::bytes{}` or another policy that explicitly produces buffered bytes.
Streaming bodies are consumed through `request_body_stream`.

Public shape:

```cpp
class request {
public:
  method method() const noexcept;
  std::string_view target() const noexcept;
  std::string_view path() const noexcept;
  std::string_view query() const noexcept;
  std::string_view matched_pattern() const noexcept;
  std::optional<std::string_view> query(std::string_view name) const noexcept;
  std::string_view query_or(std::string_view name, std::string_view fallback = {}) const noexcept;
  std::span<const std::string> query_all(std::string_view name) const noexcept;
  const query_params& query_params() const noexcept;

  const headers& headers() const noexcept;
  std::string_view header(std::string_view name) const noexcept;

  std::span<const std::byte> body_bytes() const noexcept;
  std::string_view body() const noexcept;

  const route_params& params() const noexcept;
  std::span<const std::string> decoded_path_segments() const noexcept;
  const connection_info& connection() const noexcept;
};
```

Borrowed string views remain valid only for the lifetime of the request object.
Applications should copy values they need after the handler returns.
Connection metadata is a snapshot of local and remote endpoints. Handlers that
need to take over the transport should use the explicit upgrade path rather
than `request`.

`matched_pattern()` returns the canonical route pattern that matched the
request, such as `/items/:id`. It is empty for fallback responses and other
requests that did not match an application route.

`query()` without arguments returns the raw query string exactly as received in
the request target. Structured query access is provided by an immutable
`query_params` view owned by the request:

```cpp
class query_params {
public:
  bool contains(std::string_view name) const noexcept;
  std::optional<std::string_view> first(std::string_view name) const noexcept;
  std::string_view get(std::string_view name, std::string_view fallback = {}) const noexcept;
  std::span<const std::string> all(std::string_view name) const noexcept;
};
```

Parsing keeps duplicate keys, treats a key without `=` as present with an empty
value, decodes `%XX` escapes, and maps `+` to a space. Invalid percent escapes
are left literal instead of rejecting the request. The initial API intentionally
stays string-based; typed extraction and validation can be added later as a
separate body/query binding layer.

## Request Body Streaming

`body::stream{}` dispatches the route after headers are complete. Body chunks
arrive through a move-only `request_body_stream` object:

```cpp
class request_body_stream {
public:
  request_body_stream& on_data(std::function<void(std::span<const std::byte>)> callback);
  request_body_stream& on_end(std::function<void()> callback);
  request_body_stream& on_error(std::function<void(std::error_code)> callback);

  void pause();
  void resume();
  bool active() const noexcept;
};
```

The chunk span passed to `on_data` is borrowed and valid only while the callback
is running. Applications must copy bytes they need to keep.

`pause()` stops consuming additional body data after the current callback.
`resume()` restarts reads. `on_end()` means the full request body has been
received and the HTTP parser can continue to the next request on a keep-alive
connection. `on_error()` covers body limit violations, client disconnects, and
stream read failures. Malformed HTTP is rejected by the protocol layer with
`400 Bad Request` before application error hooks are involved.

## Response

`response` represents one HTTP response. It is owned by the active connection
until the response has ended and all queued writes have completed.

Public shape:

```cpp
class response {
public:
  response& status(unsigned int code);
  response& header(std::string_view name, std::string_view value);
  response& type(std::string_view content_type);

  void text(std::string_view body);
  void json(std::string_view serialized_json);
  void json(const uvp::json& value);
  void bytes(std::span<const std::byte> body);
  void end();
  deferred_response defer();
  streaming_response stream();

  bool ended() const noexcept;
  bool deferred() const noexcept;
  bool streaming() const noexcept;
};
```

JSON is represented by `uvp::json`, a public alias for `nlohmann::json`.
`json(std::string_view)` remains available for already serialized payloads.
`json(const uvp::json&)` serializes with `dump()` and supports strings,
numbers, booleans, arrays, objects, and null values. Typed JSON request bodies
use the same type and nlohmann's `from_json` customization point.

## Handler Completion

Handlers may complete responses synchronously:

```cpp
srv.get("/health", body::none{}, [](request& req, response& res) {
  res.text("ok");
});
```

For asynchronous application work, use the explicit deferral model rather than
keeping arbitrary references without a signal:

```cpp
srv.get("/data", body::none{}, [](request& req, response& res) {
  auto reply = res.defer();
  reply.on_cancel([] {
    // Cancel application-owned work, close cursors, stop timers, etc.
  });
  start_async_load([reply = std::move(reply)](auto result) mutable {
    if (reply.active()) {
      reply.text(render(result));
    }
  });
});
```

`deferred_response` owns or references response state safely until it is
completed or cancelled by connection close.

Public shape:

```cpp
class deferred_response {
public:
  deferred_response(deferred_response&&) noexcept;
  deferred_response& operator=(deferred_response&&) noexcept;

  deferred_response(const deferred_response&) = delete;
  deferred_response& operator=(const deferred_response&) = delete;

  bool active() const noexcept;
  deferred_response& on_cancel(std::function<void()> callback);

  deferred_response& status(unsigned int code);
  deferred_response& header(std::string_view name, std::string_view value);
  deferred_response& type(std::string_view content_type);

  void text(std::string_view body);
  void json(std::string_view serialized_json);
  void json(const uvp::json& value);
  void bytes(std::span<const std::byte> body);
  void end();
};
```

Internally, each HTTP request owns a response slot that preserves HTTP/1.1
response ordering even when pipelined requests complete out of order. A slot
starts open and may transition through these states:

```text
open -> completed
open -> deferred -> completed
open -> streaming -> completed
open -> upgraded
open/deferred/streaming -> cancelled
```

Non-terminal mutators such as `status()` and `header()` only configure the
response. Terminal operations such as `text()`, `json()`, `bytes()`, and
`end()` mark the slot completed. `defer()` marks the slot deferred and prevents
automatic completion at handler return. A `deferred_response` is move-only and
can complete the same slot later if the connection is still active.

Cancellation must be explicit and safe. `deferred_response::active()` reports
whether the slot is still connected to a live session. `on_cancel()` registers
application cleanup for cases where the connection closes, the server closes
the session, a timeout fires, or the slot is otherwise abandoned before normal
completion. Cancellation callbacks must not run after normal completion, and
exceptions from cancellation callbacks must not escape into libuv callbacks.

`server_options::max_pending_responses_per_connection()` limits the number of
response slots that may be open, deferred, streaming, or completed but not yet
fully written for a single connection. `max_pending_write_bytes()` remains the
separate memory limit for serialized payload/chunk bytes already queued for
writing.

Streaming responses build on the same slot model. A streaming response marks
its slot streaming, emits chunks in order, and completes with `end()`. Later
response slots may be prepared while an earlier slot is deferred or streaming,
but they must not write bytes before all earlier slots complete.

Public streaming shape:

```cpp
class stream_write_result {
public:
  bool accepted() const noexcept;
  bool should_continue() const noexcept;
  std::error_code error() const noexcept;

  explicit operator bool() const noexcept;
};

class streaming_response {
public:
  bool active() const noexcept;

  streaming_response& on_cancel(std::function<void()> callback);
  streaming_response& on_drain(std::function<void()> callback);
  streaming_response& on_error(std::function<void(std::error_code)> callback);

  streaming_response& status(unsigned int code);
  streaming_response& header(std::string_view name, std::string_view value);
  streaming_response& type(std::string_view content_type);

  stream_write_result write(std::string_view chunk);
  stream_write_result write(std::span<const std::byte> chunk);
  stream_write_result write(std::string chunk);
  void end();
};
```

`write()` copies borrowed input immediately and may move from an owned
`std::string`. A successful result with `operator bool() == false` means the
chunk was accepted, but the application should stop writing until `on_drain`
runs. `accepted() == false` is an immediate rejection, such as writing after the
stream has closed.

The first `write()` or `end()` commits the response headers. After headers are
committed, mutating `status()`, `header()`, or `type()` is invalid. Streaming
responses use HTTP/1.1 chunked transfer encoding: each write becomes one chunk,
and `end()` emits the terminating chunk.

## Parser

HTTP parsing is security-sensitive and full of edge cases. The preferred design
is to use a mature HTTP/1 parser behind a private adapter instead of writing one
from scratch.

The HTTP/1 backend is `llhttp`. It must remain behind `detail/` and act only as
a synchronous state machine. It must not own the socket, uvpp loop, timers,
response write buffers, or public request/response objects.

HTTP/1 should not expose a backend selector while `llhttp` is the only supported
parser. Build configuration may decide how `llhttp` is supplied, such as
FetchContent or a vendored source directory, but not which HTTP/1 parser the
library uses.

The first adapter should collect parser events into an internal message model:
method, target, headers, body chunks, HTTP version, keep-alive, and upgrade
state. Later milestones can wire that internal model into connection sessions
and streaming dispatch without exposing `llhttp` types.

`libnghttp2` is reserved for a future HTTP/2 implementation behind the same
boundary. HTTP/2 support should not shape the HTTP/1 public API until a concrete
implementation milestone starts. Early milestones should not configure, link,
or wrap `libnghttp2`; they should only avoid choices that would block a later
HTTP/2 transport/session implementation.

The parser adapter should emit events into the session:

- message begin;
- URL/target bytes;
- header field/value bytes;
- headers complete;
- body bytes;
- message complete;
- upgrade requested.

The public API should not expose parser-specific types.

## Connection Lifecycle

Per accepted TCP connection:

1. Create a session owned by the server.
2. Start reading from the uvpp TCP stream.
3. Feed received bytes into the parser.
4. Build a request until headers and body policy allow dispatch.
5. Invoke the matched route handler.
6. Serialize and queue the response.
7. Keep the connection alive when HTTP semantics and options allow it.
8. Close the session on parser error, timeout, write failure, or explicit close.

The session must own all uvpp request objects and buffers needed for async
writes. Payload bytes must live until write completion.

## Options

`server_options` uses the fluent value-object style defined in the API
principles. Defaults should favor safe local services and small APIs. Larger
uploads, longer-lived streams, and larger write queues should be explicit.

Currently enforced options:

- `max_header_bytes`: maximum accepted request header bytes;
- `max_body_bytes`: default request body limit when a route does not override
  it with `route_options::max_body_bytes(...)`; values must be greater than
  zero;
- `header_timeout`: maximum time spent waiting for complete request headers;
- `body_timeout`: default request body receive timeout when a route does not
  override it with `route_options::body_timeout(...)`;
- `idle_timeout`: maximum keep-alive idle time after a response has been fully
  written;
- `max_pending_write_bytes`: maximum queued serialized response bytes per
  connection before write backpressure is reported;
- `max_pending_responses_per_connection`: maximum open, deferred, streaming, or
  completed-but-not-written response slots per connection;
- `keep_alive`: whether the server keeps HTTP/1.1 connections open when the
  request allows it;
- `server_header`: whether the default `Server` response header is added;
- `route_path_matching`: whether routing uses percent-decoded segments or raw
  path segments.

User-facing configuration should normally use the builder style defined in the
API principles:

```cpp
uvp::http::server srv(
  loop,
  uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(10 * 1024 * 1024)
    .max_pending_write_bytes(2 * 1024 * 1024)
    .server_header(false));
```

## Related Proposals

Future HTTP work is tracked outside stable design:

- [Multipart handling](../proposals/multipart-handling.md)
- [Server-Sent Events support](../proposals/sse-support.md)
- [Static file helper](../proposals/static-file-helper.md)
- [HTTP client](../proposals/http-client.md)
- [HTTP TLS listener integration](../proposals/http-tls-listener-integration.md)
- [Route body limit inheritance](../proposals/route-body-limit-inheritance.md)
- [HTTP/2 support](../proposals/http2-support.md)
