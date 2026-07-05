# HTTP Client

`uvp::http::client` provides the first uvpp-native client path: a one-shot
buffered HTTP/1.1 request over plain `http://` URLs.

```cpp
#include <uvpp/protocols/http.hpp>

uv::loop loop;
uvp::http::client client(loop);

auto op = client.get("http://127.0.0.1:8080/health",
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
URL -> DNS -> TCP connect -> HTTP/1.1 request -> buffered response
```

Current limits:

- only `http://` is supported;
- HTTPS/TLS, pooling, redirects, proxying, and streaming request or response
  bodies are follow-up work;
- responses are buffered and bounded by `client_options::max_body_bytes`;
- requests use `Connection: close`, so connections are not reused yet.

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
  });
```

If a phase expires, the request completes with
`uvp::http::errc::client_timeout`. The error detail names the timed-out phase.

Requests are cancellable:

```cpp
op.cancel();
```
