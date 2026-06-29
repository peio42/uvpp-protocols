# HTTP Timeout Enforcement Proposal

Status: Implemented

## Context

`uvp::http::server_options` exposes `header_timeout`, `body_timeout`, and
`idle_timeout`. Those values are now stored, validated, and enforced in the
HTTP session lifecycle.

## Current State

- Implemented: public timeout setters and getters on `server_options`.
- Implemented: `route_options::body_timeout(...)` for route-level request body
  timeout overrides.
- Implemented: validation that configured timeout values are greater than zero.
- Implemented: session close paths for parser errors, write failures, explicit
  close, and connection errors.
- Implemented: timers that enforce header, body, and idle timeouts.
- Implemented: tests that verify header timeout, route body timeout, and
  keep-alive idle timeout behavior.

## Scope

- Enforce `header_timeout` while waiting for complete request headers.
- Enforce `body_timeout` while receiving a request body, with route-level
  overrides through `route_options`.
- Enforce `idle_timeout` while a keep-alive connection is open but no request
  is in progress.
- Define how timeout closure interacts with deferred and streaming responses.
- Notify cancellation/error callbacks that are already part of the response and
  request body stream contracts.
- Add tests for timeout-driven connection closure and cleanup.

## Proposed Semantics

`header_timeout` starts when a session begins reading a new request. It stops
when request headers are complete. If it fires before any request has been
accepted, the server sends `408 Request Timeout` and then closes the
connection. It remains a server/session option because the route is not known
until headers have been parsed.

`body_timeout` applies after headers complete and until the full request body is
received or rejected. The timer is reset by body progress. If the application
pauses a `request_body_stream`, the timer is stopped while backpressure is in
effect and restarted when the stream resumes. If it fires, active request body
streams receive a timeout error, outstanding responses are cancelled with an
error outcome, and the connection closes.

`body_timeout` can be overridden per route:

```cpp
srv.post(
  "/upload",
  uvp::http::route_options{}
    .max_body_bytes(20 * 1024 * 1024)
    .body_timeout(std::chrono::seconds{30}),
  uvp::http::body::stream{},
  upload_handler);
```

Routes that do not set a route-level timeout use
`server_options::body_timeout()`.

`idle_timeout` applies after the last response for a keep-alive connection is
fully written and before the next request begins. If it fires, the session
closes quietly. It remains a server/session option because no route is active
while the connection is idle.

Timeouts should cancel outstanding deferred and streaming responses through the
same cancellation paths used by connection close.

## Implementation Notes

- Add uvpp timer ownership to the HTTP session implementation.
- Keep timers private to the HTTP server implementation.
- Avoid exposing timer handles or libuv timer details in public HTTP APIs.
- Use one session timer with explicit phases (`header`, `body`, `idle`) because
  those phases are mutually exclusive for one HTTP/1 session.
- Store route body timeout metadata next to route body limit metadata.
- Reset timers at clear lifecycle boundaries:
  - session start;
  - headers complete;
  - body chunk received;
  - message complete;
  - response queue drained;
  - connection close.
- Make timeout close idempotent with normal close, parser-error close, and
  write-error close.
- Keep `server_options` defaults unchanged.

## Tests

Covered by focused integration or session-level tests:

- incomplete headers returning `408 Request Timeout` and closing after
  `header_timeout`;
- slow request body closing after route-level `body_timeout`;
- keep-alive idle connection closing after `idle_timeout`;

Additional hardening tests can still be added for:

- body progress resetting `body_timeout`;
- deferred response cancellation on timeout;
- streaming response `on_error` or `on_cancel` on timeout;
- no timeout firing after explicit close.

## Out Of Scope

- Public timeout hooks.
- Per-route `header_timeout` and `idle_timeout`.
- Adaptive timeout logic.
- Request deadline propagation to user handlers.
- TLS handshake timeouts.
- HTTP/2 stream-level timeouts.

## Documentation Updates

When implemented, update:

- [HTTP server design](../design/http-server.md)
- [HTTP server user documentation](../user/http-server.md)

The design documentation should then move the timeout options from "exposed but
not yet enforced" to the stable enforced option list.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [Code quality audit](../audits/code-quality.md)
