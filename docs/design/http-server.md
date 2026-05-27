# HTTP Server Design

## Target

The first implementation target is an HTTP/1.1 server for small services,
embedded admin endpoints, local agents, and protocol gateways.

The API should make simple cases terse:

```cpp
uv::http::server srv(loop);

srv.get("/health", [](uv::http::request& req, uv::http::response& res) {
  res.json({{"status", "ok"}});
});

srv.listen("127.0.0.1", 8080);
```

It should still leave room for streaming bodies, custom error handling, route
parameters, middleware-like hooks, and protocol upgrades.

## Public Types

Initial public types:

- `uv::http::server`
- `uv::http::server_options`
- `uv::http::request`
- `uv::http::response`
- `uv::http::connection`
- `uv::http::router`
- `uv::http::headers`
- `uv::http::method`
- `uv::http::status`
- `uv::http::route_params`

The aggregate include should be:

```cpp
#include <uvpp/protocols/http.hpp>
```

## Server

`server` owns:

- a listening `uv::tcp`;
- a route table;
- accepted HTTP connection sessions;
- server-level hooks;
- server options.

Sketch:

```cpp
namespace uv::http {

class server {
public:
  explicit server(uv::loop& loop);
  server(uv::loop& loop, server_options options);

  template<class Handler>
  server& get(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& post(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& put(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& patch(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& delete_(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& head(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& options(std::string_view pattern, Handler&& handler);

  template<class Handler>
  server& route(method method, std::string_view pattern, Handler&& handler);

  void listen(std::string_view host, unsigned int port);
  void close();

  uv::tcp& tcp() noexcept;
};

}
```

`listen()` should bind and start listening. Immediate libuv failures throw
`uv::error`.

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

Handlers receive request and response references:

```cpp
using handler = std::function<void(request&, response&)>;
```

The implementation may use templates at registration time, but stored handlers
can be type-erased until a proven performance issue appears.

## Request

`request` is owned by the active connection while a handler is running. The
initial implementation may buffer the full body up to `max_body_bytes`.

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

  bool ended() const noexcept;
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
srv.get("/health", [](request& req, response& res) {
  res.text("ok");
});
```

For asynchronous application work, add an explicit deferral model rather than
letting users keep arbitrary references without a signal:

```cpp
srv.get("/data", [](request& req, response& res) {
  auto reply = res.defer();
  start_async_load([reply = std::move(reply)](auto result) mutable {
    reply.text(render(result));
  });
});
```

`deferred_response` should own or reference response state safely until it is
completed or cancelled by connection close. This can be added after the initial
synchronous handler path, but the core response design should leave room for it.

## Parser

HTTP parsing is security-sensitive and full of edge cases. The preferred design
is to use a mature HTTP/1 parser behind a private adapter instead of writing one
from scratch.

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
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 1024 * 1024;
  std::size_t max_pending_write_bytes = 1024 * 1024;

  std::chrono::milliseconds header_timeout = std::chrono::seconds{10};
  std::chrono::milliseconds body_timeout = std::chrono::seconds{30};
  std::chrono::milliseconds idle_timeout = std::chrono::seconds{60};

  bool keep_alive = true;
  bool server_header = true;
};
```

Defaults should favor safe local services and small APIs. Larger uploads and
long-lived streaming should be explicit.

User-facing configuration should normally use the builder style defined in the
API principles:

```cpp
uv::http::server srv(
  loop,
  uv::http::server_options::builder()
    .max_header_bytes(32 * 1024)
    .max_body_bytes(10 * 1024 * 1024)
    .idle_timeout(2min)
    .server_header(false)
    .build());
```

## Future HTTP Features

After the first route-based server works:

- streaming request bodies;
- streaming/chunked responses;
- middleware or hooks;
- custom error handlers;
- static file helper;
- HTTP upgrade hooks for WebSocket;
- HTTP client;
- proxy and CONNECT support;
- optional TLS listener integration;
- HTTP/2 only if a suitable backend and scope are chosen.
