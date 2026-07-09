# TLS Graceful Shutdown Proposal

Status: Draft, follow-up after initial TLS milestone

## Decision

The initial TLS stream closes by sending `close_notify` when possible, flushing
encrypted shutdown bytes, then closing the lower stream. That is acceptable for
the first milestone, but the full TLS close handshake should be tracked as a
separate lifecycle hardening proposal.

The follow-up should implement bounded graceful shutdown:

```text
local close()
  -> stop accepting upper writes
  -> send close_notify
  -> flush encrypted close_notify
  -> wait for peer close_notify until timeout
  -> close lower stream
  -> invoke close callback exactly once
```

## Goals

- Make `byte_stream::close(callback)` wait for a bounded TLS close handshake.
- Keep close completion exactly once.
- Keep read EOF/error delivery exactly once.
- Avoid unbounded waits when a peer never sends `close_notify`.
- Preserve current behavior for callers that do not care about graceful close
  timing.

## Public API

`uvp::io::byte_stream::close(...)` does not accept per-call options, so TLS
close policy should be configured on contexts:

```cpp
auto context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .close_timeout(std::chrono::seconds{5});
```

The same option should exist on `client_context`.

Suggested defaults:

- non-zero default close timeout, for example 5 seconds;
- `0ms` means do not wait for peer `close_notify` after flushing local
  shutdown bytes;
- timeout closes the lower stream and completes the close callback without
  reporting a second read error.

## State Machine

The TLS stream should distinguish:

- open;
- local close requested;
- local `close_notify` flushed;
- peer `close_notify` received;
- timed out waiting for peer close;
- closed;
- failed.

Important transitions:

- writes after local close fail with `errc::closed`;
- queued clear writes are failed when close starts;
- lower reads may need to remain active during shutdown even if the application
  previously called `read_stop()`;
- receiving peer `close_notify` before local close remains a clean read EOF;
- receiving transport EOF without `close_notify` before shutdown remains
  `errc::unexpected_eof`;
- receiving transport EOF after local close but before peer `close_notify`
  should close the stream according to the configured shutdown policy.

## Timer Ownership

TLS stream shutdown needs a timer owned by the stream state, similar in spirit
to listener handshake timeouts. The timer must:

- be closed before the loop closes;
- keep state alive until its close callback finishes;
- avoid invoking user callbacks after the stream is already terminal;
- not race with lower close callbacks.

## Error Policy

Recommended initial policy:

- close timeout is not delivered through the read callback;
- close timeout does not fail the close callback, because `byte_stream` close
  callbacks currently have no error argument;
- transport errors while flushing local `close_notify` should still fail
  pending write/handshake/read operations consistently and close the lower
  stream;
- read-side `unexpected_eof` before local close remains a read error.

If the project later introduces error-aware close callbacks, this policy can be
revisited.

## Tests

Core tests:

- peer sends `close_notify`; close callback runs after peer notification;
- peer never sends `close_notify`; close callback runs after timeout;
- `close_timeout(0ms)` preserves immediate-after-flush behavior;
- close called twice queues or completes callbacks exactly once each;
- writes after close fail with `errc::closed`;
- read EOF/error remains terminal and exactly once;
- closing while encrypted writes are in flight preserves callback ordering;
- lower transport error while flushing shutdown bytes does not leak state.

Integration tests:

- HTTP-over-TLS connection close still completes without hanging;
- listener close with pending handshakes is unaffected by stream close timeout.

## Out Of Scope

- Changing `uvp::io::byte_stream::close_callback` to carry errors.
- TLS session resumption.
- Half-close API for protocols that want asymmetric shutdown.
- Exposing OpenSSL shutdown internals.

## Related Documents

- [TLS support](../archive/tls-support.md)
- [TLS listener adapter](../archive/tls-listener-adapter.md)
