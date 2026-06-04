# Protocol Composition

## Purpose

Many real deployments are protocol stacks, not isolated protocols:

```text
TCP -> HTTP
TCP -> TLS -> HTTP
TCP -> TLS -> HTTP -> WebSocket
TCP -> TLS -> HTTP -> WebSocket -> MQTT
```

`uvpp-protocols` should make those stacks explicit and readable. A user should
see which layer owns the network socket, which layer performs encryption, which
layer parses HTTP, and which layer owns the application protocol.

## Core Rule

Higher-level protocols should depend on a stream-like transport abstraction, not
on one specific lower protocol implementation.

That keeps these combinations possible:

- HTTP over plain TCP;
- HTTP over TLS;
- WebSocket over plain HTTP;
- WebSocket over HTTPS;
- MQTT over TCP;
- MQTT over TLS;
- MQTT over WebSocket.

The public API should present protocol composition as wrapping or accepting a
transport, not as hidden global configuration.

## Transport Concept

The shared transport boundary should be small. The first concrete abstraction is
documented in [Transport abstractions](transport-abstractions.md). A transport
must be able to:

- start reads;
- write byte buffers asynchronously;
- close gracefully or forcefully;
- expose the owning loop;
- report peer/local endpoint metadata when available;
- surface asynchronous errors and close events.

The boundary can begin as concrete overloads for uvpp stream types and evolve
toward a named concept or type-erased transport when more modules need it.

Possible internal shape:

```cpp
namespace uvp::io {

class byte_stream {
public:
  uv::loop& loop() noexcept;
  void read_start(read_callback cb);
  void write(std::span<const std::byte> bytes, write_callback cb);
  void close(close_callback cb);
};

}
```

This type does not have to be the first implementation. It documents the
contract protocol modules are trying to share.

## Server Stacks

For common cases, the high-level module should keep the terse API:

```cpp
uvp::http::server srv(loop);
srv.listen("127.0.0.1", 8080);
```

That is shorthand for a plain TCP listener feeding HTTP sessions.

TLS should be presented as an explicit listener or acceptor layer:

```cpp
auto tls = uvp::tls::server_context{}
  .certificate_file("server.crt")
  .private_key_file("server.key")
  .alpn({"http/1.1"});

uvp::http::server srv(loop);
srv.listen(
  uvp::tls::listener{loop}
    .bind("0.0.0.0", 443)
    .context(tls));
```

In that model, HTTP does not know OpenSSL or mbedTLS. It only receives accepted
byte streams from the TLS listener.

An alternative convenience API may be added later:

```cpp
srv.listen_tls("0.0.0.0", 443, tls);
```

The convenience API should delegate to the same composition path and must not
make TLS a required HTTP dependency.

## Upgrade Stacks

Some stacks are created dynamically by protocol negotiation. WebSocket is the
main example: it starts as HTTP, then an accepted request upgrades the
connection.

The HTTP module should expose upgrade hooks without depending on the WebSocket
module:

```cpp
srv.upgrade("/events", [](uvp::http::upgrade_request& req) {
  return uvp::websocket::accept(req, {
    .on_text = [](uvp::websocket::session& ws, std::string_view msg) {
      ws.text(msg);
    },
  });
});
```

The exact callback shape may change, but the dependency direction should stay:

```text
uvp::websocket -> uvp::http upgrade API
uvp::http      -> no dependency on uvp::websocket
```

## Protocols Over WebSocket

Protocols can run above WebSocket either by consuming the WebSocket session
directly or by consuming an explicit byte-stream adapter derived from that
session.

Message-oriented protocols should accept `uvp::websocket::session` directly.
That keeps WebSocket message boundaries, text/binary distinction, close reasons,
and subprotocol details visible to the upper protocol:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  return uvp::myproto::session::accept(std::move(ws),
    uvp::myproto::server_options{});
});
```

Byte-stream-oriented protocols should request a named adapter from the
WebSocket session. `uvp::websocket::accept()` still returns a
`uvp::websocket::session`; conversion to `uvp::io::byte_stream` is explicit and
consumes the WebSocket session:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  return uvp::myproto::session::accept(
    std::move(ws).into_byte_stream(),
    uvp::myproto::server_options{});
});
```

The byte-stream adapter maps outbound writes to binary WebSocket frames and
delivers inbound binary message payloads as ordered bytes. Text frames are not
part of that byte-stream contract; protocols that need text frames or message
metadata should consume `uvp::websocket::session` directly.

MQTT is a useful example of a byte-oriented protocol over WebSocket:

```cpp
srv.upgrade("/mqtt", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("mqtt"));

  return uvp::mqtt::client_session::accept(std::move(ws).into_byte_stream(),
    uvp::mqtt::client_options{}
      .client_id("agent-1")
      .keep_alive(30s));
});
```

MQTT over raw TCP or TLS should use the same MQTT session type with a different
transport:

```cpp
auto mqtt = uvp::mqtt::client_session::connect(
  loop,
  uvp::io::tcp_endpoint{"broker.local", 1883},
  uvp::mqtt::client_options{}
    .client_id("agent-1"));
```

```cpp
auto mqtt = uvp::mqtt::client_session::connect(
  loop,
  uvp::tls::endpoint{"broker.local", 8883, tls_context},
  uvp::mqtt::client_options{}
    .client_id("agent-1"));
```

The important design point is that MQTT owns MQTT state, WebSocket owns
WebSocket framing, TLS owns encryption, and TCP owns the socket. No layer should
pretend to be the other.

## Client Stacks

Client APIs should follow the same pattern:

```cpp
auto http = uvp::http::client::connect(
  loop,
  uvp::io::tcp_endpoint{"127.0.0.1", 8080});
```

```cpp
auto http = uvp::http::client::connect(
  loop,
  uvp::tls::endpoint{"api.example.com", 443, tls_context});
```

```cpp
auto ws = uvp::websocket::client::connect(
  loop,
  uvp::url{"wss://api.example.com/events"},
  uvp::websocket::client_options{}
    .subprotocol("mqtt"));
```

High-level URL helpers are acceptable, but they should expand into explicit
transport choices internally.

## Ownership Across Layers

Layer ownership should be strict:

- the lowest active transport owns the native socket or handle;
- TLS owns handshake/encryption state and wraps the lower byte stream;
- HTTP owns HTTP parser state, routing, and request/response lifecycle;
- WebSocket owns frame parsing, masking, ping/pong, and message boundaries;
- MQTT owns MQTT packets, subscriptions, QoS state, and keep-alive.

Closing an upper layer should close or release the layers below according to a
documented policy. A close initiated by a lower layer must notify every upper
layer exactly once.

## Dependency Direction

Preferred dependency graph:

```text
uvp::tls        -> uvpp
uvp::http       -> uvpp
uvp::websocket  -> uvp::http upgrade API, uvpp
uvp::mqtt       -> shared byte stream API
```

Avoid hard dependencies such as:

```text
uvp::http -> uvp::tls
uvp::http -> uvp::websocket
uvp::tls  -> uvp::http
```

Convenience headers may compose modules:

```cpp
#include <uvpp/protocols/https.hpp>
#include <uvpp/protocols/mqtt_websocket.hpp>
```

Those headers should remain adapters. They should not move ownership or
protocol logic out of the canonical modules.
