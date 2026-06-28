# HTTP Server

The HTTP server is built around explicit routes, explicit request body policies,
and response objects owned by the active connection.

```cpp
#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

int main() {
  uv::loop loop;
  uvp::http::server srv(loop);

  srv.get("/health",
    uvp::http::body::none{},
    [](uvp::http::request&, uvp::http::response& res) {
      res.json(uvp::json{{"status", "ok"}});
    });

  srv.listen("127.0.0.1", 8080);
  loop.run();
}
```

## Listening

`server::listen(host, port)` is the TCP convenience form:

```cpp
srv.listen("127.0.0.1", 8080);
```

For transport composition, HTTP can also listen on any
`uvp::io::stream_listener`:

```cpp
auto listener = uvp::io::tcp_listener{loop}
  .bind("127.0.0.1", 8080)
  .backlog(128);

srv.listen(std::move(listener));
```

The built-in listener adapters are:

- `uvp::io::tcp_listener`: accepts TCP connections;
- `uvp::io::pipe_listener`: accepts Unix socket or named pipe connections;
- `uvp::io::stream_listener`: type-erased listener consumed by
  `server::listen(...)`.

Unix socket listeners use the same HTTP server:

```cpp
auto listener = uvp::io::pipe_listener{loop}
  .bind("/run/my-service.sock");

srv.listen(std::move(listener));
```

`stream_listener` yields `uvp::io::byte_stream` objects to HTTP. This is the
composition point used by TCP, Unix sockets, and future transports such as TLS.
At this stage, user code obtains stream listeners through the built-in adapters
or future library adapters; implementing a third-party listener adapter is not
part of the public API yet.

## Route Shape

Routes declare how the request body should be handled:

```cpp
srv.get("/health", uvp::http::body::none{}, handler);
srv.post("/echo", uvp::http::body::bytes{.max_size = 64 * 1024}, handler);
srv.post("/message", uvp::http::body::text{.max_size = 64 * 1024}, handler);
srv.post("/events", uvp::http::body::stream{}, handler);
```

The body policy controls when the handler is called and what body argument it
receives.

| Policy | Handler timing | Handler body argument |
| --- | --- | --- |
| `body::none{}` | After headers | None |
| `body::bytes{}` | After full body is buffered | `std::span<const std::byte>` |
| `body::text{}` | After full body is buffered | `std::string_view` |
| `body::stream{}` | After headers | `request_body_stream&` |

Short route overloads infer the body policy from the handler signature:

```cpp
srv.get("/health", [](uvp::http::request&, uvp::http::response&) {});
srv.post("/echo", [](uvp::http::request&, uvp::http::response&, std::span<const std::byte>) {});
srv.post("/message", [](uvp::http::request&, uvp::http::response&, std::string_view) {});
srv.post("/upload", [](uvp::http::request&, uvp::http::response&, uvp::http::request_body_stream&) {});
```

Those infer `body::none{}`, `body::bytes{}`, `body::text{}`, and
`body::stream{}` respectively.
Use the explicit policy form when route limits or future typed policies such as
JSON or multipart matter at the declaration site.

## Method Handling

Routes are method-specific. If a path exists for another method, the server
returns `405 Method Not Allowed` with an `Allow` header. `HEAD` requests fall
back to a matching `GET` route and omit the response body. `OPTIONS` requests
for a known path are answered automatically with `204 No Content` and `Allow`
unless an explicit `OPTIONS` route is registered.

## Route Groups and Hooks

Use route groups to share a path prefix and hooks across related routes:

```cpp
auto api = srv.group("/api/v1");

api.on_request([](uvp::http::request& req, uvp::http::response& res) {
  if (req.header("authorization").empty()) {
    res.status(uvp::http::status::unauthorized).text("unauthorized\n");
    return uvp::http::hook_result::stop;
  }
  return uvp::http::hook_result::next;
});

api.pre_handler([](uvp::http::request&, uvp::http::response& res) {
  res.header("x-api", "v1");
});

api.on_response([](const uvp::http::response_info& info) {
  // Observe status, response size, outcome, and a copied request snapshot.
  record_request(info.request.path, info.status, info.response_body_size);
});

api.get("/health", [](uvp::http::request&, uvp::http::response& res) {
  res.text("ok\n");
});
```

Groups may be nested. Prefixes are normalized, so these declarations register
`/api/v1/items/:id`:

```cpp
auto api = srv.group("/api");
auto v1 = api.group("v1");
v1.get("items/:id", show_item);
```

`on_request` runs after the route is matched and before the request body is
buffered or handed to a streaming route. It can return `hook_result::stop` to
short-circuit the final handler. A hook that writes, defers, or starts a
streaming response also stops the chain.

`pre_handler` runs immediately before the final route handler. For buffered
body policies, the request body is already available. For streaming policies,
it runs before the handler receives `request_body_stream&`.

Hook execution order is root to leaf: server-level hooks, parent groups, child
groups, then the final route handler. Hooks that return `void` are treated as
`hook_result::next`.

`on_response` is observational. It runs once when the response completes or is
cancelled, and receives `response_info` with a copied request snapshot, status,
response headers, logical response body size, and a `response_outcome`.
Response hooks cannot mutate the response. They run leaf to root, so the most
specific group observes first.

Route groups are lightweight value handles. You can chain route declarations
directly:

```cpp
srv.group("/api/v1")
  .get("/items", list_items)
  .get("/items/:id", show_item);
```

When you want to keep using a group later, prefer naming it before chaining:

```cpp
auto api = srv.group("/api/v1");
api
  .get("/items", list_items)
  .get("/items/:id", show_item);
```

## Resources

Use `resource()` when several methods apply to the same exact endpoint:

```cpp
srv.resource("/items/:id")
  .get(show_item)
  .put(uvp::http::body::text{}, update_item)
  .delete_(delete_item);
```

Resources are declaration helpers. They do not create a subtree and do not own
hooks. Combine them with groups when a resource lives under a shared prefix or
shared hooks:

```cpp
auto api = srv.group("/api/v1");

api.resource("/items/:id")
  .get(show_item)
  .patch(uvp::http::body::text{}, patch_item);
```

## Mountable Routers

Use `mount()` to compose an independently declared router under a path prefix:

```cpp
uvp::http::router api;

api.on_request(authenticate_api_request);
api.get("/items", list_items);
api.resource("/items/:id")
  .get(show_item)
  .put(uvp::http::body::text{}, update_item);

srv.mount("/api/v1", std::move(api));
```

Mounted routers are moved into the destination router. This keeps route
handlers and hooks owned by one router after composition and avoids copying
application callables.

Mounting preserves the mounted router's internal routes, body policies, route
parameters, wildcard routes, and hooks. Hooks already registered on the
destination prefix run before hooks from the mounted router for
`on_request` and `pre_handler`; `on_response` keeps the usual leaf-to-root
order.

Groups can mount routers relative to their prefix:

```cpp
auto api = srv.group("/api");
api.mount("/v1", std::move(v1_router));
```

If a mounted route conflicts with an existing route for the same effective
method and path, or if parameter/wildcard names conflict at the same trie
position, `mount()` throws `std::invalid_argument`.

## No Request Body

Use `body::none{}` for routes that do not accept a request body:

```cpp
srv.get("/health",
  uvp::http::body::none{},
  [](uvp::http::request& req, uvp::http::response& res) {
    res.text("ok\n");
  });
```

If a client sends a body to a `body::none{}` route, the server rejects the
request with `400 Bad Request`.

## Buffered Bytes

Use `body::bytes{}` when the handler needs the complete request body:

```cpp
srv.post("/echo",
  uvp::http::body::bytes{.max_size = 64 * 1024},
  [](uvp::http::request&, uvp::http::response& res, std::span<const std::byte> body) {
    res.bytes(body);
  });
```

The span is borrowed. It is valid only while the handler is running. Copy the
bytes if application code needs to keep them after the handler returns.

## Buffered Text

Use `body::text{}` when the handler needs the complete request body as textual
data:

```cpp
srv.post("/message",
  uvp::http::body::text{.max_size = 64 * 1024},
  [](uvp::http::request&, uvp::http::response& res, std::string_view body) {
    res.text(body);
  });
```

The string view is borrowed and valid only while the handler is running. The
initial text policy does not validate or transcode charsets; it exposes the
buffered bytes as a `std::string_view`.

## Streaming Request Bodies

Use `body::stream{}` for uploads or protocol endpoints that should process data
as it arrives:

```cpp
srv.post("/upload",
  uvp::http::body::stream{},
  [](uvp::http::request&, uvp::http::response& res, uvp::http::request_body_stream& body) {
    body.on_data([](std::span<const std::byte> chunk) {
      // Consume chunk before the callback returns, or copy it.
    });

    body.on_end([&res] {
      res.status(201).text("ok\n");
    });

    body.on_error([](std::error_code) {
      // The connection may already be gone, so a response is not guaranteed.
    });
  });
```

`on_data` receives a borrowed span that is valid only during the callback.
`pause()` and `resume()` let the application apply read backpressure:

```cpp
body.on_data([&body](std::span<const std::byte> chunk) {
  if (!sink_accepts(chunk)) {
    body.pause();
    wait_for_sink([&body] {
      body.resume();
    });
  }
});
```

`on_end` runs after the whole body is received. `on_error` reports body limit
violations, disconnects, and read failures. Malformed HTTP is rejected by the
server with `400 Bad Request`.

## Query Parameters

`request::query()` returns the raw query string, which is useful for logging,
signature checks, proxies, and diagnostics. For application logic, use the
parsed query parameter helpers:

```cpp
srv.get("/search", [](uvp::http::request& req, uvp::http::response& res) {
  const auto q = req.query_or("q", "");
  const auto tags = req.query_all("tag");

  if (req.query("debug")) {
    // The parameter is present, even if its value is empty.
  }

  res.json(uvp::json{
    {"q", std::string(q)},
    {"tag_count", tags.size()},
  });
});
```

Repeated keys are preserved:

```text
/search?q=uvpp&tag=cxx&tag=networking
```

`req.query("tag")` returns the first value, while `req.query_all("tag")`
returns all values in request order. Names and values are decoded with `%XX`
escapes and `+` as a space. Invalid percent escapes are kept literally.

## Connection Info

`request::connection()` exposes the local and remote endpoints associated with
the accepted connection:

```cpp
srv.get("/whoami", [](uvp::http::request& req, uvp::http::response& res) {
  const auto& connection = req.connection();
  const auto& local = connection.local_endpoint();
  const auto& remote = connection.remote_endpoint();

  (void)local;
  (void)remote;
  res.text("ok\n");
});
```

The endpoint value can represent TCP or pipe endpoints depending on the
listener that accepted the connection.

## Responses

Simple responses end immediately:

```cpp
res.status(201).text("created\n");
res.type("application/octet-stream").bytes(payload);
res.status(204).end();
```

Use `defer()` when application work completes later:

```cpp
srv.get("/report",
  uvp::http::body::none{},
  [](uvp::http::request&, uvp::http::response& res) {
    auto reply = res.defer();
    reply.on_cancel([] {
      // Cancel application-owned work.
    });

    start_report([reply = std::move(reply)](std::string report) mutable {
      if (reply.active()) {
        reply.type("text/plain").text(report);
      }
    });
  });
```

## Chunked Response Streaming

Use `stream()` for HTTP/1.1 chunked responses:

```cpp
class log_feed : public std::enable_shared_from_this<log_feed> {
public:
  explicit log_feed(uvp::http::streaming_response stream)
      : stream_(std::move(stream)) {}

  void start() {
    stream_.type("application/x-ndjson");

    auto self = weak_from_this();
    stream_.on_drain([self] {
      if (auto feed = self.lock()) {
        feed->produce_more();
      }
    });
    stream_.on_error([self](std::error_code) {
      if (auto feed = self.lock()) {
        feed->closed_ = true;
      }
    });

    produce_more();
  }

private:
  void produce_more() {
    while (!closed_ && has_more()) {
      auto line = next_line();
      auto result = stream_.write(std::move(line));

      if (!result.accepted()) {
        closed_ = true;
        return;
      }
      if (!result) {
        return; // Accepted, but wait for on_drain before producing more.
      }
    }

    if (!closed_) {
      stream_.end();
      closed_ = true;
    }
  }

  uvp::http::streaming_response stream_;
  bool closed_ = false;
};

std::vector<std::shared_ptr<log_feed>> active_feeds;

srv.get("/logs",
  uvp::http::body::none{},
  [&active_feeds](uvp::http::request&, uvp::http::response& res) {
    auto feed = std::make_shared<log_feed>(res.stream());
    feed->start();
    active_feeds.push_back(feed);
  });
```

`write()` accepts `std::string_view`, `std::span<const std::byte>`, and owned
`std::string` payloads:

```cpp
void log_feed::produce_more() {
  while (has_more()) {
    auto line = next_line();
    auto result = stream_.write(std::move(line));

    if (!result.accepted()) {
      return;
    }
    if (!result) {
      return; // Accepted, but wait for on_drain before producing more.
    }
  }

  stream_.end();
}
```

The first `write()` or `end()` commits the response headers. After that,
changing status or headers is invalid. A `false` result means the chunk was
accepted but the pending write queue reached backpressure; resume from
`on_drain`.

## Route Parameters

Named parameters and wildcard tails are available through `req.params()`:

```cpp
srv.get("/users/:id",
  uvp::http::body::none{},
  [](uvp::http::request& req, uvp::http::response& res) {
    auto id = req.params().get("id");
    res.json(uvp::json{{"id", std::string(id)}});
  });
```

Routes are matched by path segment. Static segments have priority over named
parameters, and named parameters have priority over wildcard tails. If a static
branch does not produce a complete match, the router can still fall back to a
parameter or wildcard branch.

HTTP route registration rejects duplicate method/pattern pairs, unnamed
parameters, wildcards that are not the final segment, and conflicting parameter
names at the same tree position.

## Upgrade Routes

`server::upgrade(...)` registers upgrade routes separately from normal HTTP
routes. The handler receives an `upgrade_request` with the parsed request
metadata, route parameters, connection info, and any bytes already read after
the upgrade boundary:

```cpp
srv.upgrade("/raw/:name", [](uvp::http::upgrade_request& req) {
  auto name = req.params().get("name");
  auto protocol = req.header("upgrade");
  auto extra = req.extra_bytes();

  (void)name;
  (void)protocol;
  (void)extra;

  req.reject(
    "HTTP/1.1 400 Bad Request\r\n"
    "Connection: close\r\n"
    "Content-Length: 0\r\n"
    "\r\n");
});
```

Protocol helpers such as WebSocket normally call `req.accept(...)` or
`req.reject(...)` for you. Applications that implement their own upgrade
protocol can accept the connection manually:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  req.accept(
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: myproto\r\n"
    "\r\n",
    [](uvp::io::byte_stream stream) {
      uvp::myproto::session::accept(std::move(stream));
    });
});
```

After acceptance, HTTP transfers ownership of the underlying byte stream to the
upgrade protocol.

## Errors

Route handlers may throw. Uncaught exceptions are handled by the route error
policy:

```cpp
srv.on_error([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
  res.status(500).json(uvp::json{{"error", "internal server error"}});
});
```

Use `not_found()` to customize missing routes:

```cpp
srv.not_found([](uvp::http::request&, uvp::http::response& res) {
  res.status(404).json(uvp::json{{"error", "not found"}});
});
```
