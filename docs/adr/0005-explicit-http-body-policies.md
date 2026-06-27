# ADR 0005: Explicit HTTP Body Policies

Status: Accepted

Date: 2026-06-27

## Context

HTTP route handlers need different request body behavior. Some routes reject
bodies, some need bounded buffered bytes or text, some need streaming upload
access, and future routes may need typed JSON, multipart, or form decoding.

If body handling is discovered late through methods such as
`request::multipart()` or `request::body()`, the server cannot make routing,
buffering, content-type validation, dispatch timing, and limit enforcement
obvious at the route declaration site.

## Decision

HTTP routes declare their request body policy explicitly:

```cpp
srv.get("/health", uvp::http::body::none{}, handler);
srv.post("/echo", uvp::http::body::bytes{.max_size = 64 * 1024}, handler);
srv.post("/message", uvp::http::body::text{.max_size = 64 * 1024}, handler);
srv.post("/upload", uvp::http::body::stream{}, handler);
```

The body policy is part of the route contract. It controls dispatch timing,
buffering, streaming, rejection, and the handler body argument.

Short overloads may infer simple policies from unambiguous handler signatures,
but typed or expensive policies such as JSON and multipart must be declared
explicitly.

## Consequences

- Body ownership and resource limits are visible before the handler runs.
- The server can reject unsupported content types or oversized bodies before
  application code accidentally starts consuming them.
- Multipart and future typed decoders fit the same route model instead of
  becoming late request-side APIs.
- Route declarations are a little more explicit, but the explicitness prevents
  hidden asynchronous reads and accidental buffering.

## References

- [HTTP server design](../design/http-server.md)
- [Multipart handling proposal](../proposals/multipart-handling.md)
- [User HTTP server documentation](../user/http-server.md)
