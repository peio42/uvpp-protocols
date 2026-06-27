# Server-Sent Events Support Proposal

Status: Draft, not implemented

## Current State

- Implemented: HTTP streaming responses with drain, cancel, and error hooks.
- Not implemented: `response::sse()`, `sse_stream`, SSE formatting, headers,
  retry frames, comments, and heartbeat support.

## Scope

Server-Sent Events support should be a small HTTP response helper for long-lived
`text/event-stream` responses. It should build on the existing
`streaming_response` implementation rather than introducing a new protocol
session owner.

The first implementation should cover:

- `response::sse()` as an ergonomic wrapper over `response::stream()`;
- automatic SSE response headers;
- event, retry, and comment formatting;
- optional heartbeat comments;
- close, drain, and error callbacks;
- the same backpressure behavior as HTTP streaming responses.

Client-side EventSource behavior, application replay logs, and generic event
bus abstractions are outside the initial scope.

## Public API Shape

The intended route shape is:

```cpp
srv.get("/events", [](uvp::http::request& req, uvp::http::response& res) {
  auto sse = res.sse(uvp::http::sse_options{
    .heartbeat = std::chrono::seconds{15},
  });

  auto last_id = req.header("Last-Event-ID");

  sse.on_close([] {
    // Unsubscribe, stop timers, release resources.
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
  std::chrono::milliseconds heartbeat = std::chrono::milliseconds{0};
  std::string_view heartbeat_comment = "ping";
  bool no_cache = true;
  bool x_accel_buffering_no = true;
};

class sse_stream {
public:
  bool active() const noexcept;

  sse_stream& on_close(std::function<void()> callback);
  sse_stream& on_drain(std::function<void()> callback);
  sse_stream& on_error(std::function<void(std::error_code)> callback);

  stream_write_result retry(std::chrono::milliseconds value);
  stream_write_result send(const sse_event& event);
  stream_write_result comment(std::string_view value);
  void close();
};
```

`on_close()` is the application cleanup hook. It should map to the underlying
stream cancellation and normal close behavior in a way that runs user cleanup at
most once. `on_error()` reports write failures and connection errors. `on_drain`
follows `streaming_response::on_drain()` and lets applications resume queued
event writes after backpressure clears.

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

`sse_stream::send()` formats one event according to the EventSource wire format:

```text
event: ready
id: 1
data: {"ok":true}

```

Rules:

- omit `event:` when `event` is empty;
- omit `id:` when `id` is empty;
- omit `data:` only when the application intentionally sends an empty data
  event;
- split multi-line data into one `data:` line per input line;
- terminate every event with a blank line;
- validate or reject raw CR/LF in `event` and `id`;
- encode `retry` as milliseconds and require a positive duration.

`comment(value)` writes a comment frame:

```text
: ping

```

Comments do not dispatch browser events, so they are the right primitive for
manual heartbeats.

## Heartbeats

Applications may send manual heartbeats:

```cpp
sse.comment("ping");
```

They may also request automatic heartbeat comments:

```cpp
auto sse = res.sse(uvp::http::sse_options{
  .heartbeat = std::chrono::seconds{15},
  .heartbeat_comment = "ping",
});
```

Automatic heartbeats should use a uvpp timer owned by the SSE stream state or
by the HTTP response slot. The timer must stop when the stream closes, when the
connection is cancelled, or when the user calls `close()`.

Heartbeat writes must respect backpressure. If a heartbeat cannot be accepted
because the stream is closed, the timer should stop. If a heartbeat is accepted
but `should_continue()` is false, later heartbeat ticks should not pile up an
unbounded queue.

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

Connection close, server shutdown, write failure, or response cancellation
should make `active() == false` and run `on_close()` at most once. Write errors
should also call `on_error()` with the underlying `std::error_code`.

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
separate `uvp::sse` namespace is only needed if the project later adds reusable
SSE client support, event codecs, or non-HTTP composition points.
