# HTTP Server Design

## Target

The first implementation target is an HTTP/1.1 server for small services,
embedded admin endpoints, local agents, and protocol gateways.

The API should make simple cases terse:

```cpp
uvp::http::server srv(loop);

srv.get("/health", uvp::http::body::none{}, [](uvp::http::request& req, uvp::http::response& res) {
  res.json({{"status", "ok"}});
});

srv.listen("127.0.0.1", 8080);
```

It should keep common routes lightweight while making request body handling
explicit at route declaration time. That body policy is how the server decides
whether to dispatch after headers, buffer bytes, parse a typed body, stream
chunks, or reject a body entirely.

## Public Types

Initial public types:

- `uvp::http::server`
- `uvp::http::server_options`
- `uvp::http::request`
- `uvp::http::response`
- `uvp::http::connection`
- `uvp::http::router`
- `uvp::http::headers`
- `uvp::http::method`
- `uvp::http::status`
- `uvp::http::route_params`
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
  server& on_error(Handler&& handler);

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

The MVP router may start with exact method/path matches. The intended route
model should support:

- exact paths: `/health`;
- named parameters: `/users/:id`;
- wildcard tail segments: `/static/*path`;
- method-specific handlers;
- fallback `not_found` handler.

Route matching should operate on the decoded path component, not the raw target.
The raw target remains available on `request`.

Handlers receive request and response references, plus a body argument when the
body policy produces one:

```cpp
using handler = std::function<void(request&, response&)>;
using bytes_handler = std::function<void(request&, response&, std::span<const std::byte>)>;
using text_handler = std::function<void(request&, response&, std::string_view)>;
using stream_handler = std::function<void(request&, response&, request_body_stream&)>;
using error_handler = std::function<void(request&, response&, std::exception_ptr)>;
```

The implementation may use templates at registration time, but stored handlers
can be type-erased until a proven performance issue appears.

## Body Policies

Routes declare a body policy explicitly:

```cpp
srv.get("/health", body::none{}, handler);
srv.post("/echo", body::bytes{.max_size = 64 * 1024}, handler);
srv.post("/message", body::text{.max_size = 64 * 1024}, handler);
srv.post("/events", body::stream{}, handler);
srv.post("/users", body::json<User>{}, handler);
```

The body policy is part of the route contract:

- `body::none{}` dispatches after request headers and rejects request bodies;
- `body::bytes{}` buffers the body up to a configured limit, then dispatches
  with `std::span<const std::byte>`;
- `body::text{}` buffers the body up to a configured limit, then dispatches
  with `std::string_view`;
- `body::stream{}` dispatches after request headers and provides a
  `request_body_stream&`;
- `body::json<T>{}` is the intended typed JSON policy once JSON integration is
  selected;
- future policies such as multipart, form data, or protocol-specific decoders
  should use the same route declaration shape.

Convenience overloads infer the body policy from the handler signature when the
mapping is unambiguous:

```cpp
srv.get("/health", handler); // equivalent to body::none{}
srv.post("/echo", bytes_handler); // equivalent to body::bytes{}
srv.post("/message", text_handler); // equivalent to body::text{}
srv.post("/events", stream_handler); // equivalent to body::stream{}
```

Inference should stop at `body::none{}`, `body::bytes{}`, `body::text{}`, and
`body::stream{}`. Typed policies such as `body::json<T>` and future multipart
decoders should be declared explicitly.

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

  const headers& headers() const noexcept;
  std::string_view header(std::string_view name) const noexcept;

  std::span<const std::byte> body_bytes() const noexcept;
  std::string_view body() const noexcept;

  const route_params& params() const noexcept;
  connection& connection() noexcept;
};
```

Borrowed string views remain valid only for the lifetime of the request object.
Applications should copy values they need after the handler returns.

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
connection. `on_error()` covers parser errors, body limit violations, client
disconnects, and stream read failures.

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
  void json(/* JSON-compatible value, exact type TBD */);
  void bytes(std::span<const std::byte> body);
  void end();
  deferred_response defer();
  streaming_response stream();

  bool ended() const noexcept;
  bool deferred() const noexcept;
  bool streaming() const noexcept;
};
```

The exact JSON integration should stay undecided until the build and dependency
policy is chosen. Options include:

- no built-in JSON dependency, only `json(std::string_view serialized_json)`;
- optional adapter for a popular C++ JSON library;
- tiny object builder for simple values only.

The user-facing example can keep `res.json({{"status", "ok"}})` as the desired
ergonomic target, but the implementation should avoid forcing a heavy JSON
dependency into users who only need HTTP.

## Handler Completion

The first version should support synchronous handler completion:

```cpp
srv.get("/health", body::none{}, [](request& req, response& res) {
  res.text("ok");
});
```

For asynchronous application work, add an explicit deferral model rather than
letting users keep arbitrary references without a signal:

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

`deferred_response` should own or reference response state safely until it is
completed or cancelled by connection close. This can be added after the initial
synchronous handler path, but the core response design should leave room for it.

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

Initial options:

```cpp
struct server_options {
  server_options& max_header_bytes(std::size_t value) &;
  server_options&& max_header_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_header_bytes() const noexcept;

  server_options& max_body_bytes(std::size_t value) &;
  server_options&& max_body_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_body_bytes() const noexcept;

  server_options& keep_alive(bool value) & noexcept;
  server_options&& keep_alive(bool value) && noexcept;
  [[nodiscard]] bool keep_alive() const noexcept;

  server_options& server_header(bool value) & noexcept;
  server_options&& server_header(bool value) && noexcept;
  [[nodiscard]] bool server_header() const noexcept;

private:
  // Defaults are safe for small services.
};
```

Defaults should favor safe local services and small APIs. Larger uploads and
long-lived streaming should be explicit.

User-facing configuration should normally use the builder style defined in the
API principles:

```cpp
uvp::http::server srv(
  loop,
  uvp::http::server_options{}
    .max_header_bytes(32 * 1024)
    .max_body_bytes(10 * 1024 * 1024)
    .idle_timeout(2min)
    .server_header(false));
```

## Future HTTP Features

After the first route-based server works:

- streaming request bodies;
- middleware or hooks;
- static file helper;
- HTTP upgrade hooks for WebSocket;
- HTTP client;
- proxy and CONNECT support;
- optional TLS listener integration;
- HTTP/2 only if a suitable backend and scope are chosen.
