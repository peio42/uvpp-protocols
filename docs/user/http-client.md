# HTTP Client

`uvp::http::client` provides the first uvpp-native client path: a one-shot
buffered HTTP/1.1 request over `http://` and `https://` URLs.

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
URL -> DNS -> TCP connect -> optional TLS/ALPN -> HTTP/1.1 request -> buffered response
```

Current limits:

- pooling, redirects, proxying, and streaming request or response bodies are
  follow-up work;
- responses are buffered and bounded by `client_options::max_header_bytes` and
  `client_options::max_body_bytes`;
- requests use `Connection: close`, so connections are not reused yet.

The buffered parser handles status lines, headers, content-length bodies,
chunked bodies, EOF-delimited bodies, and bodyless HEAD/204/304 responses.
Malformed responses fail with `uvp::http::errc::client_malformed_response`;
header and body limits have dedicated client errors.

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
