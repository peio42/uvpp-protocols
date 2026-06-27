# TLS Support Proposal

Status: Draft, not implemented

## Current State

- Implemented: shared `uvp::io::byte_stream` and `uvp::io::stream_listener`
  abstractions.
- Not implemented: `uvp::tls`, TLS contexts, OpenSSL integration, TLS stream
  adapter, TLS listener, verification policy, ALPN, SNI, and TLS errors.
- Related proposals: [HTTP TLS listener integration](http-tls-listener-integration.md).

## Purpose

TLS should be a reusable transport layer, not an HTTP feature.

The canonical shape is:

```text
uvp::io::byte_stream
  -> uvp::tls
    -> uvp::io::byte_stream
      -> HTTP, SMTP, IMAP, MQTT, or another byte-oriented protocol
```

`uvp::tls` owns TLS handshake, encryption, certificate verification, ALPN, SNI,
and close-notify handling. The application protocol above TLS should only see a
normal `uvp::io::byte_stream` carrying decrypted bytes.

This keeps these stacks equivalent from the upper protocol's point of view:

```text
TCP -> TLS -> HTTP
TCP -> TLS -> SMTP
TCP -> SMTP -> STARTTLS -> SMTP
TCP -> TLS -> MQTT
Unix socket -> TLS -> custom protocol
```

HTTP may offer convenience helpers later, but the core TLS module must not
depend on HTTP and HTTP must not depend on TLS.

## Public Shape

The lowest-level TLS API should adapt an existing stream:

```cpp
uvp::tls::accept(
  std::move(lower_stream),
  server_context,
  [](uvp::tls::handshake_result result) {
    if (!result) {
      return;
    }

    uvp::io::byte_stream clear = std::move(result).stream();
    uvp::smtp::server_session::start(std::move(clear));
  });
```

```cpp
uvp::tls::connect(
  std::move(lower_stream),
  client_context,
  [](uvp::tls::handshake_result result) {
    if (!result) {
      return;
    }

    auto clear = std::move(result).stream();
  });
```

Server listener support should be a generic adapter:

```cpp
auto plain = uvp::io::tcp_listener{loop}.bind("0.0.0.0", 443);

uvp::io::stream_listener secure =
  uvp::tls::listener{std::move(plain), server_context};

uvp::http::server http(loop);
http.listen(std::move(secure));
```

The listener is a convenience wrapper around the same `accept(byte_stream, ...)`
path. It accepts a lower stream, completes the TLS server handshake, and then
emits a clear `uvp::io::byte_stream` to its accept callback.

## STARTTLS

TLS must support protocols that begin in clear text and upgrade in place.
SMTP, IMAP, POP3, XMPP, and similar protocols need to release the current
transport after their own `STARTTLS` command has been negotiated:

```cpp
session.write_ready_to_start_tls();

uvp::tls::accept(
  session.release_transport(),
  server_context,
  [session = std::move(session)](uvp::tls::handshake_result result) mutable {
    if (!result) {
      session.fail(result.error());
      return;
    }

    session.resume_after_starttls(std::move(result).stream());
  });
```

This implies that application protocols which support STARTTLS need an explicit
transport handoff API. They must stop reading from the clear stream before
transferring ownership to TLS, and they must resume with the clear stream
returned by TLS after the handshake.

No TLS API should assume that TLS starts at byte zero of a TCP connection.

## Contexts

The public configuration surface should be backend-neutral:

```cpp
auto server_context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .alpn({"http/1.1"});
```

```cpp
auto client_context = uvp::tls::client_context{}
  .server_name("smtp.example.com")
  .default_verify_paths()
  .alpn({"smtp"});
```

Suggested initial context features:

- certificate chain and private key loading for servers;
- default trust paths and explicit CA file/directory loading for clients;
- minimum TLS version, with TLS 1.2 as the lowest acceptable default;
- ALPN protocol list;
- SNI server name for clients;
- peer verification policy;
- optional client certificate verification for servers.

Client contexts should verify peers by default. Hostname verification should be
enabled when a server name is configured. Disabling verification should require
an intentionally noisy API name such as `insecure_no_verify_peer()`.

## OpenSSL Backend

The first backend should be OpenSSL 3.x. OpenSSL should remain private to the
TLS module:

- public headers should not include OpenSSL headers;
- public types should not expose `SSL_CTX*`, `SSL*`, `BIO*`, `X509*`, or
  `EVP_PKEY*`;
- native-handle escape hatches, if added later, should live under an explicit
  backend namespace such as `uvp::tls::openssl_backend`;
- OpenSSL should not own the socket, event loop, timers, read buffers, write
  queue, or public session lifetime.

`server_context` and `client_context` should use PIMPL or another private RAII
wrapper around `SSL_CTX`. Individual TLS streams should own one `SSL` object
and the BIOs needed to connect OpenSSL to the lower `byte_stream`.

OpenSSL has process and thread level behavior, an error queue, and detailed
ownership rules. The wrapper should centralize those details in `src/tls` so
that other modules only observe typed results and `std::error_code` values.

## BIO Strategy

The initial implementation should use memory BIOs rather than OpenSSL socket
BIOs or `SSL_set_fd()`.

uvpp already owns the socket and event loop. The TLS stream should therefore
drive OpenSSL like this:

```text
lower read ciphertext
  -> BIO_write(rbio)
  -> SSL_do_handshake / SSL_read_ex
  -> clear read callback

upper write plaintext
  -> SSL_write_ex
  -> BIO_read(wbio)
  -> lower write ciphertext
```

This design keeps libuv readiness, buffering, and backpressure in uvpp while
OpenSSL owns only TLS protocol state.

OpenSSL operations are stateful and may need to be retried. `SSL_do_handshake`,
`SSL_read_ex`, `SSL_write_ex`, and `SSL_shutdown` can report that progress
requires more encrypted input or more encrypted output flushing. The TLS stream
should treat those outcomes as normal state transitions, not fatal errors.

## TLS Stream State

The TLS stream implementation should act as a small state machine:

```text
handshaking -> open -> shutting_down -> closed
            \-> failed
```

Responsibilities:

- start lower reads while the handshake or clear reads are active;
- feed encrypted bytes into the read BIO;
- drain encrypted bytes from the write BIO into the lower stream;
- serialize lower writes so ciphertext order is preserved;
- queue upper writes until the handshake is complete;
- invoke each upper write callback once the corresponding encrypted output has
  been accepted by the lower transport;
- deliver decrypted read bytes with the same callback-scoped lifetime contract
  as `uvp::io::byte_stream`;
- convert received TLS `close_notify` into a clean EOF;
- deliver protocol, verification, and transport failures exactly once.

Backpressure should be explicit. If lower writes are pending, the TLS stream can
continue accepting upper writes up to a configured limit. Beyond that limit,
`write()` should fail asynchronously with a TLS or stream error rather than
growing memory without bound.

## Shutdown

TLS close is not the same as closing a TCP stream.

`uvp::tls::stream::close()` should attempt a graceful TLS shutdown:

1. stop accepting new upper writes;
2. call `SSL_shutdown()`;
3. flush any encrypted `close_notify` bytes to the lower stream;
4. wait for peer `close_notify` when practical;
5. close the lower stream.

The first milestone may use a simpler policy: send `close_notify` when possible,
flush pending encrypted bytes, then close the lower stream. A close-handshake
timeout can be added after timers are introduced for TLS sessions.

## ALPN and SNI

ALPN and SNI are TLS concepts, not HTTP concepts.

ALPN should use generic protocol names supplied by the caller:

```cpp
server_context.alpn({"h2", "http/1.1"});
client_context.alpn({"smtp"});
```

After a successful handshake, the result or stream should expose the selected
ALPN value:

```cpp
auto protocol = result.selected_alpn();
```

Server-side ALPN selection should prefer the server's configured order among the
client-proposed protocols. No match should fail the handshake only when the
context is configured to require ALPN; otherwise it can complete with no
selected protocol.

Client SNI should come from `client_context::server_name()`. Server-side SNI can
start as an observable callback and later grow into context selection for
multi-tenant servers.

## Errors

TLS needs its own error category:

```cpp
namespace uvp::tls {
const std::error_category& error_category() noexcept;
}
```

The public result types should still use `std::error_code` and compose with
`uvp::io::stream_error`.

Error sources to distinguish:

- lower transport errors;
- malformed TLS records or protocol alerts;
- certificate verification failure;
- hostname verification failure;
- unsupported protocol version or cipher configuration;
- ALPN failure when ALPN is required;
- local misuse such as writing after close.

OpenSSL's thread-local error queue should be drained at the boundary where an
OpenSSL operation fails and mapped into stable uvpp-protocols errors.

## Build Integration

TLS support should be optional at the CMake level until the project decides that
OpenSSL is a mandatory dependency for the whole package:

```cmake
option(UVPP_PROTOCOLS_WITH_OPENSSL "Build TLS support with OpenSSL" ON)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
target_link_libraries(uvpp_protocols PRIVATE OpenSSL::SSL OpenSSL::Crypto)
```

If TLS is disabled, TLS headers can either be omitted from installation or
provide a clear compile-time diagnostic. The rest of the project should remain
buildable without OpenSSL.

OpenSSL is the initial backend, but the public design should leave room for a
future backend boundary. That does not require a runtime pluggable backend in
the first milestone. It only requires that backend details stay out of public
API and non-TLS modules.

## Milestone Scope

The first TLS milestone should include:

- backend-private OpenSSL RAII wrappers;
- `server_context` and `client_context`;
- `accept(byte_stream, server_context, callback)`;
- `connect(byte_stream, client_context, callback)`;
- a generic `tls::listener` adapter over `io::stream_listener`;
- ALPN configuration and selected-ALPN access;
- SNI client configuration;
- peer verification defaults for clients;
- TLS error category and result types;
- examples for HTTPS-style HTTP over TLS and one non-HTTP or STARTTLS-shaped
  usage sketch;
- tests for handshake success, verification failure, fragmented records, echo,
  close, and listener adaptation.

Follow-up work:

- server-side SNI context selection;
- client certificate authentication;
- configurable close-handshake timeout;
- session resumption;
- key logging for debugging when explicitly enabled;
- native OpenSSL escape hatches if real users need them.
