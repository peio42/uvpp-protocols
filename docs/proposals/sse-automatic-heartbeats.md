# SSE Automatic Heartbeat Scheduling Proposal

Status: Draft, follow-up to [Server-Sent Events support](../archive/sse-support.md)

## Context

The initial SSE helper should support manual heartbeat comments through
`sse_stream::comment()`. Automatic heartbeat scheduling is useful for long-lived
browser `EventSource` connections behind proxies, but it requires a timer whose
lifetime is tied to a live HTTP response slot.

The current HTTP streaming API exposes `streaming_response` as a move-only
application handle over a response slot. It does not expose the loop or own
per-stream timers in `response_state`. Adding automatic SSE heartbeats directly
to `response::sse()` would therefore force a broader ownership decision than
the formatting helper needs.

## Proposed API

If implemented, automatic heartbeats should be opt-in through `sse_options`:

```cpp
auto sse = res.sse(uvp::http::sse_options{
  .heartbeat = std::chrono::seconds{15},
  .heartbeat_comment = "ping",
});
```

A zero heartbeat interval should disable automatic scheduling.

## Design Requirements

- The timer must stop when the stream completes normally, the response slot is
  cancelled, the connection closes, the server shuts down, or a heartbeat write
  is rejected.
- Heartbeat writes must use `sse_stream::comment()` formatting.
- Heartbeats must obey the same backpressure contract as `streaming_response`.
  If a heartbeat is accepted with backpressure, subsequent timer ticks must not
  enqueue unbounded heartbeat frames.
- The implementation must not make destruction of the public `sse_stream`
  handle close the response, because that would diverge from
  `streaming_response`.
- Callback exceptions must not escape through uvpp/libuv callbacks.

## Open Design Point

Choose where timer ownership lives:

- HTTP response slot ownership: the server/session owns the timer and can stop
  it with the rest of the slot lifecycle.
- SSE shared state ownership: `response::sse()` creates a small internal state
  object that owns the underlying `streaming_response`, timer, backpressure
  flag, and cancellation wiring.

The first option keeps lifetime close to existing HTTP response ordering. The
second option avoids growing generic HTTP response state for an SSE-specific
feature, but it needs a clean way to access the uvpp loop.
