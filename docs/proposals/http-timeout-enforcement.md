# HTTP Timeout Enforcement Proposal

Status: Draft

## Context

`uvp::http::server_options` exposes `header_timeout`, `body_timeout`, and
`idle_timeout`. Those values are stored and validated, but the HTTP server does
not currently enforce them in the connection/session lifecycle.

Because these options are already public API, the implementation should either
enforce them or remove/defer them. Keeping them as inert options is misleading
for users and weakens the design documentation as a stable source of truth.

## Current State

- Implemented: public timeout setters and getters on `server_options`.
- Implemented: validation that timeout values are greater than zero.
- Implemented: session close paths for parser errors, write failures, explicit
  close, and connection errors.
- Not implemented: timers that enforce header, body, or idle timeouts.
- Not implemented: tests that verify timeout behavior.

## Draft Scope

- Enforce `header_timeout` while waiting for complete request headers.
- Enforce `body_timeout` while receiving a request body.
- Enforce `idle_timeout` while a keep-alive connection is open but no request
  is in progress.
- Define how timeout closure interacts with deferred and streaming responses.
- Notify cancellation/error callbacks that are already part of the response and
  request body stream contracts.
- Add tests for timeout-driven connection closure and cleanup.

## Proposed Semantics

`header_timeout` starts when a session begins reading a new request. It stops
when request headers are complete. If it fires, the session should close the
connection. A `408 Request Timeout` response can be considered only if it can be
sent without complicating parser state or opening response ordering edge cases.

`body_timeout` applies after headers complete and until the full request body is
received, rejected, or upgraded to streaming ownership. The timer should be
reset by body progress. If it fires, active request body streams should receive
an error notification and the connection should close.

`idle_timeout` applies after the last response for a keep-alive connection is
fully written and before the next request begins. If it fires, the session
should close quietly.

Timeouts should cancel outstanding deferred and streaming responses through the
same cancellation paths used by connection close.

## Implementation Notes

- Add uvpp timer ownership to the HTTP session implementation.
- Keep timers private to the HTTP server implementation.
- Avoid exposing timer handles or libuv timer details in public HTTP APIs.
- Reset timers at clear lifecycle boundaries:
  - session start;
  - headers complete;
  - body chunk received;
  - message complete;
  - response queue drained;
  - connection close.
- Make timeout close idempotent with normal close, parser-error close, and
  write-error close.
- Keep `server_options` defaults unchanged unless tests show they are too
  aggressive for examples.

## Tests

Add focused integration or session-level tests for:

- incomplete headers closing after `header_timeout`;
- slow request body closing after `body_timeout`;
- body progress resetting `body_timeout`;
- keep-alive idle connection closing after `idle_timeout`;
- deferred response cancellation on timeout;
- streaming response `on_error` or `on_cancel` on timeout;
- no timeout firing after explicit close.

## Out Of Scope

- Public timeout hooks.
- Per-route timeout policies.
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
