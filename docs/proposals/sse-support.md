# Server-Sent Events Support Proposal

Status: Draft, not implemented

## Current State

- Implemented: HTTP streaming responses with drain, cancel, and error hooks.
- Not implemented: `response::sse()`, `sse_stream`, SSE formatting, headers,
  retry frames, comments, and automatic heartbeat support.

## Scope

Server-Sent Events support should be a small HTTP response helper for long-lived
`text/event-stream` responses. It should build on the existing
`streaming_response` implementation rather than introducing a new protocol
session owner.

The first implementation should cover:

- `response::sse()` as an ergonomic wrapper over `response::stream()`;
- automatic SSE response headers;
- event, retry, and comment formatting;
- manual heartbeat comments through `comment()`;
- cancel, drain, and error callbacks aligned with `streaming_response`;
- the same backpressure behavior as HTTP streaming responses.

Client-side EventSource behavior, application replay logs, and generic event
bus abstractions are outside the initial scope.

Automatic heartbeat scheduling is also outside this first implementation. It
needs an owned timer tied to the HTTP response slot or another long-lived SSE
state object, which is a broader lifetime decision than formatting SSE frames.
That follow-up is tracked in
[SSE automatic heartbeat scheduling](sse-automatic-heartbeats.md).

## Public API Shape

The intended route shape is:

```cpp
srv.get("/events", [](uvp::http::request& req, uvp::http::response& res) {
  auto sse = res.sse();

  auto last_id = req.header("Last-Event-ID");

  sse.on_cancel([] {
    // Unsubscribe application-owned work after connection cancellation.
  });

  sse.retry(std::chrono::seconds{5});

  sse.send({
    .event = "ready",
    .id = "1",
    .data = R"({"ok":true})",
  });
});
```

`response::sse()` should claim the response slot in the same way as
`response::stream()`. The returned `sse_stream` is move-only and remains active
until the response ends, the connection closes, the server cancels the slot, or
a write error occurs.

Sketch:

```cpp
struct sse_event {
  std::string_view event;
  std::string_view id;
  std::string_view data;
};

struct sse_options {
  bool no_cache = true;
  bool x_accel_buffering_no = true;
};

class sse_stream {
public:
  bool active() const noexcept;

  sse_stream& on_cancel(std::function<void()> callback);
  sse_stream& on_drain(std::function<void()> callback);
  sse_stream& on_error(std::function<void(std::error_code)> callback);

  stream_write_result retry(std::chrono::milliseconds value);
  stream_write_result send(const sse_event& event);
  stream_write_result comment(std::string_view value);
  void close();
};
```

`sse_stream` follows the same handle model as `streaming_response`: it is a
move-only application handle, not an owning detached protocol session. A handler
that wants to keep publishing events after it returns must store the
`sse_stream` in application-owned state. A local `sse_stream` is useful for
setting headers, registering callbacks, sending immediate frames, or closing
immediately, but destroying the handle must not implicitly close the HTTP
response.

`on_cancel()` maps directly to `streaming_response::on_cancel()` and is the
application cleanup hook for connection close, server shutdown, timeout, or any
other response-slot cancellation before normal completion. It should not run for
the normal application path where `close()` ends the underlying stream.
`on_error()` maps to `streaming_response::on_error()` and reports write failures
or connection errors. If an error also cancels the response slot, `on_error()`
may run before `on_cancel()`, matching the existing HTTP streaming behavior.
`on_drain()` maps to `streaming_response::on_drain()` and lets applications
resume queued domain events after backpressure clears.

## Response Headers

`response::sse()` should set:

```http
Content-Type: text/event-stream; charset=utf-8
Cache-Control: no-cache
X-Accel-Buffering: no
```

`Cache-Control: no-cache` avoids stale event streams and matches common SSE
practice. `X-Accel-Buffering: no` is useful for deployments behind nginx-like
proxies and is safe as an opt-out option.

HTTP transport details remain owned by the HTTP streaming layer:

- HTTP/1.1 streaming responses use chunked transfer encoding;
- keep-alive is governed by the HTTP connection policy;
- SSE should not force a transport header that is invalid or meaningless for
  future HTTP versions.

If the HTTP/1.1 serializer already emits `Connection: keep-alive` according to
server options, SSE can benefit from it. It should not need to set that header
itself.

## Event Formatting

`sse_stream::send()` formats one event according to the EventSource wire
format. Formatting is deterministic and does not perform JSON serialization,
escaping, compression, or replay bookkeeping:

```text
event: ready
id: 1
data: {"ok":true}

```

Rules:

- omit `event:` when `event` is empty;
- omit `id:` when `id` is empty;
- always emit at least one `data:` line, so an empty `data` value dispatches an
  empty browser event rather than becoming an id-only control frame;
- split multi-line data into one `data:` line per input line, treating `\n`,
  `\r\n`, and lone `\r` as line separators;
- preserve a trailing empty data line, so `data == "a\n"` is sent as
  `data: a`, then `data:`, and dispatches `a\n` in the browser;
- terminate every event with a blank line;
- validate or reject raw CR, LF, and NUL bytes in `event` and `id`;
- pass `data` bytes through unchanged other than line splitting; callers own
  UTF-8 correctness for the declared `text/event-stream; charset=utf-8` content
  type;
- return `stream_write_result::rejected(std::errc::invalid_argument)` for
  invalid event fields and perform no partial write.

Field separators should follow the common readable form:

```text
event: ready
id: 1
data: {"ok":true}
data:

```

For non-empty values, emit `field: value`. For an empty field value, emit
`field:` without a trailing space.

`retry(value)` writes a retry frame:

```text
retry: 5000

```

Rules:

- encode the duration as a decimal millisecond count;
- require a strictly positive duration;
- reject durations that cannot be represented as a non-negative millisecond
  count;
- return `stream_write_result::rejected(std::errc::invalid_argument)` for
  invalid durations and perform no partial write.

`comment(value)` writes a comment frame:

```text
: ping

```

Comments do not dispatch browser events, so they are the right primitive for
manual heartbeats.

Comment formatting rules:

- split `value` on `\n`, `\r\n`, and lone `\r`;
- emit one comment line per input line;
- preserve trailing empty comment lines;
- emit `:` for an empty comment value and `: value` for a non-empty line;
- terminate the comment frame with a blank line;
- return the underlying stream write result.

## Heartbeats

Applications may send manual heartbeats:

```cpp
sse.comment("ping");
```

Manual heartbeat writes use the same `stream_write_result` model as any other
SSE write. If `comment()` returns accepted backpressure, the application should
wait for `on_drain()` before sending more heartbeat or domain frames.

Automatic heartbeat comments can be added later behind an explicit user option,
but the first SSE helper should not add a timer-owning state machine just for
that feature. See
[SSE automatic heartbeat scheduling](sse-automatic-heartbeats.md).

## Last-Event-ID

The browser sends the last observed id as the `Last-Event-ID` request header
when reconnecting. The HTTP request API already exposes headers, so the SSE
helper does not need a special accessor:

```cpp
auto last_id = req.header("Last-Event-ID");
```

Header lookup should remain case-insensitive according to the HTTP header API.

Application code should own replay behavior. The library does not know whether
event ids are numeric, monotonic, global, per-user, partitioned, or persistent.
It also does not know which historical events are still available.

For that reason, SSE should not auto-generate event ids by default. An omitted
id should remain omitted.

An explicit auto-id option can be considered later for simple local streams:

```cpp
auto sse = res.sse(uvp::http::sse_options{
  .auto_id = uvp::http::sse_auto_id::incremental,
});
```

If such an option is added, it should be clearly documented as connection-local
unless the caller provides a durable id source. It must not imply replay
support.

## Backpressure

SSE writes should return the same `stream_write_result` model as
`streaming_response::write()`:

```cpp
auto result = sse.send(event);
if (!result.accepted()) {
  // Closed or rejected.
}
if (!result.should_continue()) {
  // Queue application events and resume on on_drain().
}
```

`sse_stream` should not hide backpressure behind an unbounded internal queue.
It may hold a small formatted frame only while passing it to the underlying
streaming response. If an application wants to buffer domain events, it should
do so explicitly and obey `on_drain()`.

This keeps SSE behavior aligned with existing chunked response examples and
with future WebSocket send-queue semantics.

## Errors And Close

`close()` should end the underlying streaming response. It is a normal
application close and should not call the error callback.

Connection close, server shutdown, timeout, write failure, or response
cancellation should make `active() == false` and run `on_cancel()` at most once
when the stream did not complete normally. Write errors should also call
`on_error()` with the underlying `std::error_code`, following the current HTTP
streaming callback order.

Callbacks must not throw through libuv callbacks. If the HTTP server already
has a policy for uncaught handler exceptions, SSE callback exceptions should
follow the same policy or be swallowed after reporting through the server error
hook.

## Relationship To HTTP Streaming

SSE is a formatting and lifecycle helper over HTTP streaming:

```text
response -> streaming_response -> sse_stream
```

It should not own the socket, parse HTTP, manage request routing, or know about
transport-level chunk serialization. The HTTP module owns response ordering,
write queues, pending-write limits, chunked transfer encoding, and connection
keep-alive behavior.

This means `uvp::http::sse_stream` can live in the HTTP module initially. A
separate `uvp::sse` namespace is not planned for the initial implementation. It
would only be worth revisiting if the project later adds reusable SSE client
support, event codecs, or credible non-HTTP composition points.
