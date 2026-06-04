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
      res.json({{"status", "ok"}});
    });

  srv.listen("127.0.0.1", 8080);
  loop.run();
}
```

## Route Shape

Routes declare how the request body should be handled:

```cpp
srv.get("/health", uvp::http::body::none{}, handler);
srv.post("/echo", uvp::http::body::bytes{.max_size = 64 * 1024}, handler);
srv.post("/events", uvp::http::body::stream{}, handler);
```

The body policy controls when the handler is called and what body argument it
receives.

| Policy | Handler timing | Handler body argument |
| --- | --- | --- |
| `body::none{}` | After headers | None |
| `body::bytes{}` | After full body is buffered | `std::span<const std::byte>` |
| `body::stream{}` | After headers | `request_body_stream&` |

Short route overloads may exist for obvious cases, such as `get(path, handler)`
as shorthand for `get(path, body::none{}, handler)`. The explicit policy form is
the canonical style in this documentation because it makes body handling visible
at the route declaration.

## No Request Body

Use `body::none{}` for routes that do not accept a request body:

```cpp
srv.get("/health",
  uvp::http::body::none{},
  [](uvp::http::request& req, uvp::http::response& res) {
    res.text("ok\n");
  });
```

If a client sends a body to a `body::none{}` route, the server should reject the
request according to the configured error policy.

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

`on_end` runs after the whole body is received. `on_error` reports parser
errors, body limit violations, disconnects, and read failures.

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
    res.json({{"id", id}});
  });
```

## Errors

Route handlers may throw. Uncaught exceptions are handled by the server error
policy:

```cpp
srv.on_error([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
  res.status(500).json({{"error", "internal server error"}});
});
```

Use `not_found()` to customize missing routes:

```cpp
srv.not_found([](uvp::http::request&, uvp::http::response& res) {
  res.status(404).json({{"error", "not found"}});
});
```
