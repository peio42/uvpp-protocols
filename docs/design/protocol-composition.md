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

The shared transport boundary should be small. A transport must be able to:

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
namespace uv::protocols {

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
uv::http::server srv(loop);
srv.listen("127.0.0.1", 8080);
```

That is shorthand for a plain TCP listener feeding HTTP sessions.

TLS should be presented as an explicit listener or acceptor layer:

```cpp
auto tls = uv::tls::server_context::builder()
  .certificate_file("server.crt")
  .private_key_file("server.key")
  .alpn({"http/1.1"})
  .build();

uv::http::server srv(loop);
srv.listen(
  uv::tls::listener::builder(loop)
    .bind("0.0.0.0", 443)
    .context(tls)
    .build());
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
srv.upgrade("/events", [](uv::http::upgrade_request& req) {
  return uv::websocket::accept(req, {
    .on_text = [](uv::websocket::session& ws, std::string_view msg) {
      ws.text(msg);
    },
  });
});
```

The exact callback shape may change, but the dependency direction should stay:

```text
uv::websocket -> uv::http upgrade API
uv::http      -> no dependency on uv::websocket
```

## Protocols Inside WebSocket

Some protocols can run directly over WebSocket frames. MQTT is the useful
example for this project.

The API should make the carrier explicit:

```cpp
srv.upgrade("/mqtt", [](uv::http::upgrade_request& req) {
  auto ws = uv::websocket::accept(req, uv::websocket::accept_options::builder()
    .subprotocol("mqtt")
    .build());

  return uv::mqtt::client_session::over_websocket(std::move(ws),
    uv::mqtt::client_options::builder()
      .client_id("agent-1")
      .keep_alive(30s)
      .build());
});
```

MQTT over raw TCP or TLS should use the same MQTT session type with a different
transport:

```cpp
auto mqtt = uv::mqtt::client_session::connect(
  loop,
  uv::tcp::endpoint{"broker.local", 1883},
  uv::mqtt::client_options::builder()
    .client_id("agent-1")
    .build());
```

```cpp
auto mqtt = uv::mqtt::client_session::connect(
  loop,
  uv::tls::endpoint{"broker.local", 8883, tls_context},
  uv::mqtt::client_options::builder()
    .client_id("agent-1")
    .build());
```

The important design point is that MQTT owns MQTT state, WebSocket owns
WebSocket framing, TLS owns encryption, and TCP owns the socket. No layer should
pretend to be the other.

## Client Stacks

Client APIs should follow the same pattern:

```cpp
auto http = uv::http::client::connect(
  loop,
  uv::tcp::endpoint{"127.0.0.1", 8080});
```

```cpp
auto http = uv::http::client::connect(
  loop,
  uv::tls::endpoint{"api.example.com", 443, tls_context});
```

```cpp
auto ws = uv::websocket::client::connect(
  loop,
  uv::http::url{"wss://api.example.com/events"},
  uv::websocket::client_options::builder()
    .subprotocol("mqtt")
    .build());
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
uv::tls        -> uvpp
uv::http       -> uvpp
uv::websocket  -> uv::http upgrade API, uvpp
uv::mqtt       -> shared byte stream API, optional uv::websocket adapter
```

Avoid hard dependencies such as:

```text
uv::http -> uv::tls
uv::http -> uv::websocket
uv::tls  -> uv::http
```

Convenience headers may compose modules:

```cpp
#include <uvpp/protocols/https.hpp>
#include <uvpp/protocols/mqtt_websocket.hpp>
```

Those headers should remain adapters. They should not move ownership or
protocol logic out of the canonical modules.

