# URL

`uvp::url` is an owning parsed URL value for client-side protocol code. It is
intended for absolute URLs such as HTTP, HTTPS, WS, and WSS endpoints.

```cpp
#include <uvpp/protocols/url.hpp>

auto parsed = uvp::parse_url("https://api.example.com:8443/users/42?q=full");
if (!parsed) {
  auto error = parsed.error().code;
  return;
}

auto const& url = parsed.value();
auto host = url.hostname();
auto port = uvp::effective_port(url);
auto target = uvp::origin_form_target(url);
```

The URL object owns its serialized components. Views returned by accessors
borrow from the `url` object:

```cpp
url.scheme();    // "https"
url.hostname();  // "api.example.com"
url.port();      // "8443"
url.path();      // "/users/42"
url.query();     // "q=full"
url.fragment();  // without '#'
```

Helpers provide the pieces needed by future client-side layers:

```cpp
uvp::scheme_id(url);           // http, https, ws, wss, or other
uvp::effective_port(url);      // explicit port or default for known schemes
uvp::authority_endpoint(url);  // host + port for DNS/connect
uvp::origin_from_url(url);     // scheme + host + port pool key
uvp::origin_form_target(url);  // path + query for direct HTTP/1.1 requests
```

Server-side HTTP request parsing is unchanged. `uvp::url` is not used to parse
inbound `request::target()`, route paths, or `http::query_params`.
