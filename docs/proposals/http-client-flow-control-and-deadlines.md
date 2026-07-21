# HTTP Client Flow Control and Deadlines Proposal

Status: Proposed after Milestone 6 client-side foundations

## Context

Milestone 6 delivers a native HTTP/1.1 client with URL, DNS, TCP, TLS,
streaming upload/download, keep-alive pooling, conservative redirects,
phase-specific timeouts, and clear HTTP forward proxy support.

That is enough to close the client-side foundations milestone. The remaining
client ergonomics are about operational control:

- pausing or resuming streamed response delivery;
- distinguishing transfer idle timeouts from total request deadlines;
- bounding pool checkout or queued work;
- reporting richer completion metadata without destabilizing the simple
  `result<http::response>` API.

These concerns should be designed together because they all affect ownership,
timer semantics, connection reuse, and exactly-once completion.

## Goals

- Add explicit response-stream flow control for downloads, SSE clients, and
  future WebSocket upgrade paths.
- Define an overall request deadline in addition to existing phase timeouts.
- Make body idle timeout semantics pause-aware and predictable.
- Add pool checkout timeout or queued-acquire timeout once pooling can queue.
- Preserve exactly-once completion for cancellation, timeout, parser failure,
  and user-driven abort.
- Keep the simple one-shot API simple while allowing advanced users to opt into
  finer control.
- Avoid making HTTP/1.1-only assumptions that would block HTTP/2 streams later.

## Non-Goals

- Replacing the Milestone 6 timeout model.
- Browser fetch compatibility.
- Full scheduler or rate-limiter APIs.
- Automatic replay of streaming request bodies.
- HTTP/2-specific flow-control windows. This proposal should leave room for
  HTTP/2 but not define its wire-level window management.

## Proposed API Direction

Streaming response requests should expose a small flow-control handle. Exact
names can change, but the shape should be explicit:

```cpp
auto req = client.stream_get("https://example.com/events");

req.on_data([&](std::span<const std::byte> chunk) {
  if (buffer_is_full()) {
    req.pause_response();
  }
});

drain_buffer([&] {
  req.resume_response();
});
```

Alternative handle-oriented shape:

```cpp
req.on_response_body([&](uvp::http::response_stream& body) {
  body.on_data(...);
  body.pause();
  body.resume();
});
```

The chosen API should make it clear whether pause applies only to user
callbacks, to lower stream reads, or to both. The preferred behavior is to stop
lower reads when practical so backpressure can propagate to the transport.

## Deadline Model

Existing phase timeouts remain useful:

- DNS resolution timeout;
- TCP connect timeout;
- TLS handshake timeout;
- request write/upload timeout;
- response header timeout;
- response body timeout.

Add a separate overall deadline:

```cpp
uvp::http::client_options options{
  .request_deadline = std::chrono::seconds{30},
};
```

The deadline covers the whole exchange from `start()` until final completion,
including redirects. If a per-phase timeout and the overall deadline could both
fire, completion should happen exactly once and report the timeout that wins the
race. Diagnostics should still include enough detail to identify the phase when
possible.

## Pause-Aware Body Timeouts

The current response body timeout is transfer-oriented. Once response delivery
can pause, the policy must be explicit:

- active transfer idle timeout: counts only while reads are enabled;
- wall-clock body deadline: counts even while paused;
- optional maximum pause duration: bounds user-side backpressure.

Suggested initial split:

```cpp
uvp::http::client_options options{
  .response_body_idle_timeout = std::chrono::seconds{15},
  .response_pause_timeout = std::chrono::minutes{2},
};
```

This prevents a paused stream from either timing out unexpectedly or staying
open forever without an intentional bound.

## Pool Checkout Timeout

Milestone 6 pooling is opt-in and reuses idle connections immediately. Future
pool behavior may queue requests when per-origin limits are reached. At that
point, add:

- timeout while waiting for an available pooled connection or stream capacity;
- cancellation while queued;
- diagnostics that distinguish queued checkout timeout from DNS/connect
  timeout.

HTTP/2 should reuse the same concept, but checkout may mean waiting for stream
capacity rather than exclusive connection ownership.

## Completion and Metadata

The client should continue to guarantee exactly-once completion.

For richer diagnostics, consider a future wrapper:

```cpp
struct client_result_metadata {
  std::chrono::milliseconds elapsed;
  std::size_t redirects_followed = 0;
  std::string final_url;
  timeout_phase timed_out_phase;
};
```

This metadata can be exposed through a future advanced result type or request
inspection API. It should not be forced into the simple
`result<http::response>` shape until the larger client result model is clear.

## Connection Reuse Rules

Pause, cancellation, and timeout decisions affect whether a connection can be
returned to the pool:

- completed response body: reusable if framing and headers allow it;
- user cancellation before body completion: close unless the body was fully
  drained by policy;
- parser error: close;
- timeout: close;
- pause timeout: close;
- protocol upgrade handoff: remove from HTTP pool ownership.

These rules should be documented and tested because they are visible through
pool reuse and resource lifetime.

## Tests

Add coverage for:

- pausing response delivery stops `on_data` callbacks until resume;
- resume continues delivery without duplicating chunks;
- cancellation while paused completes exactly once;
- response body idle timeout does not fire while intentionally paused if the
  selected policy says so;
- pause timeout closes and completes with a typed timeout;
- overall request deadline during DNS, connect, TLS, upload, headers, body, and
  redirects;
- pool checkout timeout once queued checkout exists;
- connection reuse after completed streamed responses;
- connection close after cancellation, timeout, parser failure, and pause
  timeout.

## Source Documents

- [HTTP client](../archive/http-client.md)
- [HTTP redirect policy extensions](http-redirect-policy-extensions.md)
- [Outbound connector and proxy routes](outbound-connector-and-proxy-routes.md)
- [HTTP/2 support](http2-support.md)
