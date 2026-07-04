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
srv.post(
  "/echo",
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::bytes{},
  handler);
srv.post(
  "/message",
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::text{},
  handler);
srv.post(
  "/json",
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::json<my_type>{},
  handler);
srv.post(
  "/upload-form",
  uvp::http::body::multipart_form{},
  handler);
srv.post(
  "/upload-stream",
  uvp::http::body::multipart_stream{}.max_file_bytes(2 * 1024 * 1024),
  handler);
srv.post("/events", uvp::http::body::stream{}, handler);
```

The body policy controls when the handler is called and what body argument it
receives.

| Policy | Handler timing | Handler body argument |
| --- | --- | --- |
| `body::none{}` | After headers | None |
| `body::bytes{}` | After full body is buffered | `std::span<const std::byte>` |
| `body::text{}` | After full body is buffered | `std::string_view` |
| `body::json<T>{}` | After full body is buffered and decoded | `const T&` |
| `body::multipart_form{}` | After full multipart body is parsed and bounded | `const multipart_form&` |
| `body::multipart_stream{}` | After multipart headers are validated | `multipart_stream&` |
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
JSON is not inferred from handler signatures. Use `body::json<T>{}` explicitly
when the route should parse and convert JSON. Multipart is not inferred either;
use `body::multipart_form{}` or `body::multipart_stream{}` explicitly so
buffering, parsing, file handling, and upload limits stay visible at the route.
Use `route_options` for route-level limits.

Route-level options can carry operational body settings next to the route
declaration:

```cpp
srv.post(
  "/upload",
  uvp::http::route_options{}
    .max_body_bytes(20 * 1024 * 1024)
    .body_timeout(std::chrono::seconds{30}),
  uvp::http::body::stream{},
  upload_file);
```

Use `route_options::max_body_bytes(...)` when a route needs its own request
body limit. Otherwise the server falls back to
`server_options::max_body_bytes()`. Use
`route_options::body_timeout(...)` when a route has a slower expected request
body, such as an upload. Routes without an override use
`server_options::body_timeout()`. Use
`route_options::inherit_body_timeout()` when a builder should explicitly reset
the route to that inherited default; passing `0ms` to `body_timeout(...)` has
the same effect.

Multipart policies are the bounded exception to the body-limit fallback:
when `route_options::max_body_bytes(...)` is not set,
`body::multipart_stream{}.max_total_bytes(...)` and
`body::multipart_form{}.max_total_bytes(...)` also become the route HTTP body
limit. A route-level `max_body_bytes(...)` always has priority.

Body limits must be greater than zero. Use `body::none{}` for routes that should
not receive a request body; do not use `max_body_bytes(0)` for that case.

`server_options::max_header_bytes(...)` and
`server_options::max_header_count(...)` bound untrusted request headers before
the request is dispatched. Header storage preserves insertion order and uses
linear scans, which keeps the common small-header path simple while relying on
those parser limits for adversarial inputs.

`server_options::header_timeout(...)` applies while waiting for complete request
headers. `server_options::idle_timeout(...)` applies while a keep-alive
connection is open without an active request. Those two settings are
connection-level policies, so they are not route options.

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
  // Observe status, response body size, outcome, and a copied request snapshot.
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

The applicable response hooks are captured when the response lifecycle starts.
Hooks registered later apply only to future responses; in-flight deferred and
streaming responses keep stable handles to the hooks they captured.

Groups can also define scoped fallbacks:

```cpp
auto api = srv.group("/api");

api.not_found([](uvp::http::request&, uvp::http::response& res) {
  res.status(uvp::http::status::not_found).json(uvp::json{{"error", "api route not found"}});
});

api.on_exception([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
  res.status(uvp::http::status::internal_server_error).json(uvp::json{{"error", "api failure"}});
});
```

Scoped fallbacks use the most specific matching group. If no group fallback is
available, the server-level `not_found()` or `on_exception()` handler is used.

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
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::bytes{},
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
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::text{},
  [](uvp::http::request&, uvp::http::response& res, std::string_view body) {
    res.text(body);
  });
```

The string view is borrowed and valid only while the handler is running. The
initial text policy does not validate or transcode charsets; it exposes the
buffered bytes as a `std::string_view`.

## JSON Bodies

Use `body::json<T>{}` when a route expects a JSON request body:

```cpp
struct create_item {
  std::string title;
};

void from_json(const uvp::json& value, create_item& out) {
  out.title = value.at("title").get<std::string>();
}

srv.post("/items",
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::json<create_item>{},
  [](uvp::http::request&, uvp::http::response& res, const create_item& body) {
    res.status(uvp::http::status::created).json(uvp::json{{"title", body.title}});
  });
```

`body::json<>` parses and passes `const uvp::json&` directly. Typed policies
use nlohmann `from_json` customization. The decoded value is borrowed and valid
only while the handler is running.

JSON routes accept `application/json`, structured suffixes such as
`application/problem+json`, and media type parameters such as
`application/json; charset=utf-8`. Missing or non-JSON `Content-Type` returns
`415 Unsupported Media Type`; malformed JSON returns `400 Bad Request`; typed
conversion failures, including standard exceptions from `from_json`, return
`422 Unprocessable Content`.

## Multipart Forms

Use `body::multipart_form{}` for small `multipart/form-data` forms where every
accepted part should be collected before the handler runs:

```cpp
srv.post("/profile",
  uvp::http::body::multipart_form{},
  [](uvp::http::request&, uvp::http::response& res, const uvp::http::multipart_form& form) {
    auto display_name = form.single_field("display_name");
    if (!display_name) {
      res.status(422).text("missing display_name\n");
      return;
    }

    auto tags = form.fields("tag");
    save_profile(display_name.value().text(), tags);
    res.status(204).end();
  });
```

The default form policy rejects files, limits each field to 1 MiB, caps memory
at 8 MiB, and caps the total multipart body at 16 MiB. Set a non-zero
`max_file_bytes()` only for deliberately small file parts. For
`body::multipart_form{}`, `max_file_bytes(0)` means file parts are rejected:

```cpp
srv.post("/avatar",
  uvp::http::body::multipart_form{}.max_file_bytes(256 * 1024),
  [](uvp::http::request&, uvp::http::response& res, const uvp::http::multipart_form& form) {
    auto avatar = form.single_file("avatar");
    if (!avatar) {
      res.status(422).text("missing avatar\n");
      return;
    }
    store_avatar(avatar.value().safe_filename(), avatar.value().bytes());
    res.status(204).end();
  });
```

`multipart_form` preserves original part order through `parts()` and keeps
repeated names explicit through `fields(name)` / `files(name)`. Convenience
accessors `first_field()`, `single_field()`, `first_file()`, and
`single_file()` never collapse repeated values into a map entry. Field and file
views are borrowed from the owning form and remain valid only while the form
object remains alive and unmoved.

Malformed multipart input returns `400 Bad Request`, body or part limits return
`413 Payload Too Large`, and non-multipart content types return
`415 Unsupported Media Type` before the handler is called.

## Multipart Streaming

Use `body::multipart_stream{}` for `multipart/form-data` uploads that should
stream files or large fields without buffering the whole request:

```cpp
srv.post("/upload",
  uvp::http::body::multipart_stream{},
  [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& multipart) {
    multipart
      .on_part([](uvp::http::multipart_part& part) {
        if (part.filename()) {
          auto sink = open_upload_sink(part.safe_filename());
          part.stream()
            .on_data([sink](std::span<const std::byte> chunk) mutable {
              sink.write(chunk);
            })
            .on_end([sink]() mutable {
              sink.close();
            })
            .on_error([sink](uvp::error) mutable {
              sink.abort();
            });
          return;
        }

        if (part.name() == "title") {
          part.text(1024 * 1024, [](uvp::result<std::string> value) {
            if (value) {
              store_title(value.value());
            }
          });
          return;
        }

        part.discard();
      })
      .on_end([&res] {
        res.status(201).text("uploaded\n");
      })
      .on_error([&res](uvp::error error) {
        res.status(400).text(error.detail + "\n");
      });
  });
```

`multipart_stream` validates `multipart/form-data` and its boundary before the
handler is called. Once the handler receives the stream, parser errors are
reported to `on_error`; the application is responsible for completing the
response. Route and server body-limit errors are also delivered through
`on_error` after handler entry, rather than as automatic `413` responses. Each
part must choose exactly one consumption path: `stream()`,
`text(max_bytes, callback)`, or `discard()`. Selecting a second consumption path
is reported through `multipart.on_error(...)`; if the first path was `text()`,
its callback also receives an error result.

`multipart.on_error(...)` must be configured before the handler returns,
including handlers that call `res.defer()`. If a multipart streaming handler
returns without an error handler and the response is not ended or already
claimed for streaming, the framework fails the request with
`500 Internal Server Error` and the diagnostic
`multipart on_error handler required`.

Part names and filenames come from `Content-Disposition`. Repeated field names
are allowed. `safe_filename()` strips path separators and control bytes, but
applications should still apply their own storage policy.

Use `body::multipart_stream{}` builder methods or
`multipart_stream_options` / `multipart_limits` to enforce multipart-specific
limits such as total body bytes, file bytes, field bytes, part header bytes,
part header count, part count, field name length, and filename length. For
`body::multipart_stream{}`, `max_file_bytes(0)` means file bytes are unlimited
by the multipart parser.

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
  const auto local_known = connection.local_endpoint().index() != 0;
  const auto remote_known = connection.remote_endpoint().index() != 0;

  res.json(uvp::json{
    {"local_endpoint", local_known ? "known" : "unknown"},
    {"remote_endpoint", remote_known ? "known" : "unknown"},
  });
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
      if (reply.try_type("text/plain")) {
        reply.text(report);
      }
    });
  });
```

The fluent `deferred_response` methods are best-effort once the owning
connection can disappear. Use `active()` for a coarse check, or the `try_*`
methods when application code must know whether an operation was applied.

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

## Server-Sent Events

Use `sse()` for browser `EventSource` responses. It claims the response in the
same way as `stream()`, sets the SSE response headers, and writes formatted SSE
frames over the existing HTTP streaming path:

```cpp
srv.get("/events",
  uvp::http::body::none{},
  [](uvp::http::request& req, uvp::http::response& res) {
    auto sse = res.sse();

    auto last_id = req.header("Last-Event-ID");
    (void)last_id;

    sse.on_cancel([] {
      // Stop application-owned producers for this stream.
    });

    sse.retry(std::chrono::seconds{5});
    sse.comment("ping");
    sse.send(uvp::http::sse_event{
      .event = "ready",
      .id = "1",
      .data = R"({"ok":true})",
    });
  });
```

`sse_stream` is a move-only handle like `streaming_response`. Store it in
application-owned state when events will be published after the handler returns.
Destroying the handle does not close the response. Use `close()` to end the SSE
stream normally.

`send()` emits `event:`, `id:`, and one `data:` line per data line. `comment()`
writes comment frames for manual heartbeats. `retry()` writes the browser
reconnect delay in milliseconds. All writes return `stream_write_result`, so
applications should stop publishing when a frame is accepted with backpressure
and resume from `on_drain`.

## Static Files

Use `static_files()` to serve regular files below an explicit directory root:

```cpp
srv.get(
  "/assets/*path",
  uvp::http::static_files("./public")
    .cache_control("public, max-age=3600"));
```

The route should end with a named wildcard. The helper uses the wildcard tail as
path components, rejects traversal attempts and encoded path separators, rejects
hidden files by default, and serves `index.html` for directory requests by
default. Rejected paths return `404 Not Found`.

Successful responses include a detected `Content-Type`, `Content-Length`,
`Last-Modified`, weak `ETag`, `Cache-Control: no-cache` by default, and
`X-Content-Type-Options: nosniff`. `HEAD` requests fall back to the GET route
and return the same metadata without a body. Conditional `If-None-Match` and
`If-Modified-Since` requests can return `304 Not Modified`.

Options use the same fluent style as other HTTP helpers:

```cpp
srv.get(
  "/downloads/*file",
  uvp::http::static_files("./downloads")
    .path_param("file")
    .no_index_file()
    .hidden_files(uvp::http::hidden_file_policy::allow_well_known)
    .cache_control("private, max-age=0"));
```

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

## Route Path Decoding

Routes match percent-decoded path segments by default. The server first splits
the raw path on literal `/`, then decodes each segment independently:

```cpp
srv.get("/files/:name", [](uvp::http::request& req, uvp::http::response& res) {
  auto name = req.params().get("name"); // "a/b"
  res.text(std::string{name});
});
```

`GET /files/a%2Fb` matches this route and captures `a/b` as one parameter
value. The encoded slash does not create another route segment. `+` remains a
literal plus sign in path segments; it is not decoded as a space. Invalid
percent escapes such as `%`, `%0`, or `%zz` are rejected with
`400 Bad Request`.

Use `req.decoded_path_segments()` when application code needs the decoded path
without losing segment boundaries:

```cpp
auto segments = req.decoded_path_segments(); // ["files", "a/b"]
```

Applications that need raw matching can opt in at server construction time:

```cpp
uvp::http::server srv(
  loop,
  uvp::http::server_options{}
    .route_path_matching(uvp::http::route_path_matching::raw));
```

In raw mode, route matching and captured parameters use raw path segments, but
the server still validates percent escapes and still exposes decoded segments
for inspection. Routers mounted into a server or another router must use the
same route path matching mode as their destination.

## Matched Route Pattern

Use `req.matched_pattern()` when logging or exporting metrics for matched
routes:

```cpp
srv.get("/items/:id", [](uvp::http::request& req, uvp::http::response& res) {
  auto pattern = req.matched_pattern(); // "/items/:id"
  res.text(std::string{"route "} + std::string(pattern) + "\n");
});
```

The value is the canonical full pattern after group prefixes and mounted
router prefixes have been applied. For `not_found`, automatic 404/405
responses, and other unmatched requests, the value is empty. `on_response`
receives the same value through `info.request.matched_pattern`.

## Upgrade Routes

`server::upgrade(...)` registers upgrade routes separately from normal HTTP
routes. The handler receives an `upgrade_request` with the parsed request
metadata, route parameters, connection info, and any bytes already read after
the upgrade boundary:

Upgrade route patterns are validated and prepared when they are registered.
They use the same segment-local percent-decoding rules as normal routes.

```cpp
srv.upgrade("/raw/:name", [](uvp::http::upgrade_request& req) {
  auto name = req.params().get("name");
  auto protocol = req.header("upgrade");
  auto extra = req.extra_bytes();
  auto body = std::string{"unsupported upgrade for "} + std::string{name}
    + " using " + std::string{protocol}
    + " with " + std::to_string(extra.size()) + " extra bytes\n";

  req.reject(
    std::string{"HTTP/1.1 400 Bad Request\r\n"}
    "Connection: close\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: " + std::to_string(body.size()) + "\r\n"
    "\r\n" + body);
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

Route handlers may throw. Uncaught application exceptions are handled by the
route exception policy:

```cpp
srv.on_exception([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
  res.status(500).json(uvp::json{{"error", "internal server error"}});
});
```

Use `not_found()` to customize missing routes:

```cpp
srv.not_found([](uvp::http::request&, uvp::http::response& res) {
  res.status(404).json(uvp::json{{"error", "not found"}});
});
```

`on_exception()` handles uncaught C++ exceptions from route handlers and
request-side hooks. It does not handle malformed HTTP, socket failures,
request-body stream errors, streaming response errors, or exceptions thrown by
`on_response` observers.
