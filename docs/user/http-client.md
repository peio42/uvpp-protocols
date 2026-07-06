# HTTP Client

`uvp::http::client` provides the first uvpp-native client path: one-shot
buffered requests and response-body streaming over HTTP/1.1 `http://` and
`https://` URLs.

```cpp
#include <uvpp/protocols/http.hpp>

uv::loop loop;
uvp::http::client client(loop);

auto op = client.get("https://api.example.com/health",
  [](uvp::result<uvp::http::response> result) {
    if (!result) {
      auto error = result.error().code;
      return;
    }

    auto status = result.value().status_code();
    auto body = result.value().body();
  });

loop.run();
```

The first implementation composes the shared URL and DNS foundations:

```text
URL -> DNS -> TCP connect -> optional TLS/ALPN -> HTTP/1.1 request -> response
```

Use the streaming API for downloads, Server-Sent Events clients, and future
upgrade-oriented client protocols where the response body should not be
collected before user code sees it:

```cpp
auto request = client.stream_get("https://api.example.com/events");

request
  .on_response_headers([](const uvp::http::response_head& head) {
    auto status = head.status_code;
    auto type = head.headers.get("content-type");
  })
  .on_data([](std::span<const std::byte> chunk) {
    // The span is borrowed and valid only for this callback.
  })
  .on_complete([](uvp::result<void> done) {
    if (!done) {
      auto error = done.error().code;
    }
  });

auto op = request.start();
```

The parser handles status lines, headers, content-length bodies, chunked bodies,
EOF-delimited bodies, and bodyless HEAD/204/304 responses. Malformed responses
fail with `uvp::http::errc::client_malformed_response`; header and body limits
have dedicated client errors. For the one-shot API, response bodies are
buffered and bounded by `client_options::max_body_bytes`. For the streaming API,
body chunks are delivered incrementally and the same body limit acts as a
transfer cap.

Current limits:

- pooling, redirects, proxying, and streaming request bodies are follow-up
  work;
- requests use `Connection: close`, so connections are not reused yet;
- response streaming currently supports cancellation through the returned
  `request_operation`, but does not yet expose user-controlled pause/resume
  backpressure.

HTTPS uses `uvp::tls::connect()` internally. The client sets SNI and hostname
verification from the URL host, loads default verify paths by default, and offers
ALPN `http/1.1`. Additional trust roots can be configured on the client:

```cpp
uvp::http::client client(
  loop,
  uvp::http::client_options{
    .tls_ca_file = "local-test-ca.pem",
  });
```

Timeouts are phase-scoped and disabled by default. Configure the phases needed
by the application:

```cpp
uvp::http::client client(
  loop,
  uvp::http::client_options{
    .max_header_bytes = 64 * 1024,
    .max_body_bytes = 4 * 1024 * 1024,
    .dns_timeout = std::chrono::seconds{2},
    .connect_timeout = std::chrono::seconds{3},
    .response_header_timeout = std::chrono::seconds{5},
    .response_body_timeout = std::chrono::seconds{30},
    .tls_default_verify_paths = true,
  });
```

If a phase expires, the request completes with
`uvp::http::errc::client_timeout`. The error detail names the timed-out phase.

Requests are cancellable:

```cpp
op.cancel();
```
