# WebSocket

`uvp::websocket` builds server-side WebSocket sessions on top of
`uvp::http::server` upgrade routes.

```cpp
#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/websocket.hpp>

uv::loop loop;
uvp::http::server srv(loop);

srv.upgrade("/echo", [](uvp::http::upgrade_request& req) {
  uvp::websocket::accept_detached(req)
    .on_text([](uvp::websocket::session& ws, std::string_view message) {
      ws.text(message);
    })
    .on_binary([](uvp::websocket::session& ws, std::span<const std::byte> message) {
      ws.binary(message);
    });
});

srv.listen("127.0.0.1", 8084);
loop.run();
```

## Upgrade Routes

Upgrade routes are separate from normal HTTP routes:

```cpp
std::vector<uvp::websocket::session> sessions;

srv.upgrade("/events", [&sessions](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, options);
  sessions.push_back(std::move(ws));
});
```

If the path matches, the WebSocket module validates the RFC 6455 handshake,
sends `101 Switching Protocols`, and takes ownership of the connection. Invalid
handshakes are rejected with a normal HTTP response and the connection is
closed.

## Session API

`uvp::websocket::accept()` returns a `[[nodiscard]]` move-only
`uvp::websocket::session` handle. The handle owns the accepted WebSocket
session: keep it for as long as the connection should remain open, or move it
to a higher-level protocol object. Destroying the owning handle closes the
session.

The session owns WebSocket framing, message assembly, control frames, close
state, and queued writes.

When selected, `accept_options::subprotocol(...)` must be one non-empty HTTP
token (for example `chat` or `myproto-v1`); it throws `std::invalid_argument`
for spaces, commas, or header-control characters.

```cpp
auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
  .max_message_bytes(1024 * 1024)
  .max_pending_write_bytes(1024 * 1024)
  .close_timeout(std::chrono::seconds{5})
  .subprotocol("chat"));

ws
  .on_text([](uvp::websocket::session& ws, std::string_view message) {
    ws.text(message);
  })
  .on_ping([](uvp::websocket::session&, std::span<const std::byte>) {})
  .on_pong([](uvp::websocket::session&, std::span<const std::byte>) {})
  .on_close([](uvp::websocket::session&, uvp::websocket::close_code, std::string_view) {})
  .on_error([](uvp::websocket::session&, std::error_code) {});
```

A session can send:

```cpp
ws.text("hello");
ws.binary(bytes);
ws.ping();
ws.pong(payload);
ws.close(uvp::websocket::close_code::normal, "bye");
```

After a close frame is sent, the session waits for the peer's close frame or
transport close until `accept_options::close_timeout(...)` expires, then closes
the transport. Invalid peer close codes are treated as protocol errors and do
not call `on_close`.

`close_code` names the standard close codes the session may send or report:
`normal`, `going_away`, `protocol_error`, `unsupported_data`,
`invalid_payload`, `policy_violation`, `message_too_large`,
`mandatory_extension`, and `internal_error`. Peer private-use codes from
`3000` through `4999` are accepted and can still be observed through the enum's
underlying value.

For callback-only endpoints that intentionally do not keep an owning session
handle, use `uvp::websocket::accept_detached()`. The detached form returns a
non-owning handle for callback registration and keeps the internal session
alive until the WebSocket closes:

```cpp
srv.upgrade("/echo", [](uvp::http::upgrade_request& req) {
  uvp::websocket::accept_detached(req)
    .on_text([](uvp::websocket::session& ws, std::string_view message) {
      ws.text(message);
    });
});
```

Inbound client frames must be masked. Server frames are never masked. Ping
frames are automatically answered with pong frames by default. In that mode,
`on_ping` is an observation hook and should not send the protocol pong itself.

Applications that need full control can opt out and assume responsibility for
the required pong response:

```cpp
auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
  .auto_pong(false));

ws
  .on_ping([](uvp::websocket::session& ws, std::span<const std::byte> payload) {
    ws.pong(payload);
  });
```

`pong()` remains available for manual ping handling and for unsolicited pong
heartbeats. `on_pong()` observes inbound pong frames, which is useful for
heartbeat or latency tracking after sending application pings.

The session exposes endpoint metadata from the underlying transport:

```cpp
auto local = ws.local_endpoint();
auto remote = ws.remote_endpoint();
```

Those endpoints may be TCP or pipe endpoints depending on how the HTTP server
accepted the connection.

## Protocols Above WebSocket

Message-oriented protocols should consume `uvp::websocket::session` directly:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req,
    uvp::websocket::accept_options{}.subprotocol("myproto"));

  uvp::myproto::websocket_session::accept(
    std::move(ws),
    uvp::myproto::server_options{});
});
```

Byte-stream-oriented protocols can explicitly request a `uvp::io::byte_stream`
adapter. `session::into_byte_stream()` consumes the WebSocket session handle:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req,
    uvp::websocket::accept_options{}.subprotocol("myproto"));

  uvp::myproto::session::accept(
    std::move(ws).into_byte_stream(),
    uvp::myproto::server_options{});
});
```

For the common case, `accept_byte_stream()` combines acceptance and conversion:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto stream = uvp::websocket::accept_byte_stream(req,
    uvp::websocket::accept_options{}.subprotocol("myproto"));

  uvp::myproto::session::accept(std::move(stream), uvp::myproto::server_options{});
});
```

The byte-stream adapter maps outbound writes to binary WebSocket messages and
delivers inbound binary messages as ordered bytes. Text messages are outside
that contract and close the WebSocket with a protocol error.
