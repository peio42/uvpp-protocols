# Authoring A Cancellable Protocol Operation

This guide is for an author adding a client-side protocol module to
`uvpp-protocols`, or implementing a protocol on top of its IO, DNS, and TLS
layers. It explains how to structure one user-visible operation such as
"fetch one reply", "publish one message", or "authenticate and open a
channel".

It does not describe a long-lived protocol session. A session can read many
messages and invoke many callbacks; an operation has one terminal callback.

## The problem this solves

A client operation commonly crosses several asynchronous stages:

```text
resolve -> connect -> optional TLS -> protocol handshake -> write -> read reply
```

At any point, one of these events may happen first:

- the user calls `cancel()`;
- a phase or overall deadline expires;
- a child operation fails;
- the peer rejects the request;
- the expected reply arrives.

The callbacks of work that lost this race can still arrive afterwards. For
example, cancelling a DNS lookup can cause its cancellation callback to run
immediately; a TCP connect callback may arrive after a timeout closed its
socket. The operation must report exactly one result to its caller, cancel
remaining work, and not leak or reuse a transport in an unknown protocol
state.

Use `uvp::detail::operation_lifetime<Result>` for this shape:

```cpp
#include <uvpp/protocols/detail/operation_lifetime.hpp>
```

It is an installed integration helper, but it lives in `uvp::detail`: it is
for protocol implementations, not an application-level Promise or future API.
It is intentionally callback-based, loop-affine, and has no hidden executor,
thread, or continuation chain.

## Recognising when to use it

Use one `operation_lifetime` in the state object behind a public operation
handle when all of these are true:

- the caller supplies one completion callback accepting a `uvp::result<T>` (or
  another single result type);
- the work owns, or temporarily borrows, a transport while it progresses;
- at least two asynchronous outcomes can race;
- cancellation must be accepted while any phase is active.

Typical examples are a Redis command, an SMTP submission, a WebSocket client
upgrade, or one MQTT publish acknowledgement.

Do not use it as the lifetime of a server, connection pool, WebSocket session,
or subscription. Those are long-lived owners that produce many events. They
can, however, create a separate operation lifetime for a finite sub-operation
such as a client handshake or graceful close.

## Structure the operation state around one terminal gate

Keep the state in a `shared_ptr`, as existing cancellable operation handles
do. Move the user callback into the `operation_lifetime` at construction and
do not retain a second callback or a separate `completed_` flag. Every
terminal path must go through that one instance.

When the public API configures its completion callback after it creates the
state (for example with an `on_complete(...)` builder method), construct the
lifetime with an empty callback and call `set_callback(...)` before `start()`.
The callback may be replaced while the operation is active; setting it after a
terminal result has no effect.

The following is a protocol-neutral skeleton. `reply`, `client_error`, and
the operation handles are owned by the new module; replace them with its real
types and error category.

```cpp
using operation_result = uvp::result<reply>;
using completion_callback = std::function<void(operation_result)>;

class request_state : public std::enable_shared_from_this<request_state> {
public:
  explicit request_state(completion_callback done)
      : lifetime_(std::move(done)) {
    lifetime_.set_finish_action([this]() noexcept {
      stop_phase_timer();
    });
    lifetime_.set_abort_action([this]() noexcept {
      // Cancelling an inactive child must be harmless.
      resolve_.cancel();
      connect_.cancel();
      tls_.cancel();

      // A stream in an incomplete exchange is not poolable.
      if (stream_) {
        stream_.close();
      }
    });
  }

  void start() {
    lifetime_.enter_phase("resolve");
    auto self = shared_from_this();
    resolve_ = resolver_.resolve(query_, [self](auto addresses) {
      self->on_resolved(std::move(addresses));
    });
  }

  void cancel() noexcept {
    (void)lifetime_.cancel(make_client_error(errc::cancelled));
  }

private:
  void on_resolved(uvp::result<uvp::dns::address_list> addresses) {
    if (!lifetime_.active()) {
      return; // Cancellation, timeout, or another error already won.
    }
    if (!addresses) {
      (void)lifetime_.complete(wrap_dns_error(addresses.error()));
      return;
    }

    lifetime_.enter_phase("connect");
    auto self = shared_from_this();
    connect_ = connector_.connect(addresses.value(), [self](auto connected) {
      self->on_connected(std::move(connected));
    });
  }

  void on_connected(uvp::result<uvp::io::byte_stream> connected) {
    if (!lifetime_.active()) {
      return;
    }
    if (!connected) {
      (void)lifetime_.complete(wrap_connect_error(connected.error()));
      return;
    }

    stream_ = std::move(connected.value());
    lifetime_.enter_phase("protocol-handshake");
    // Continue with the module-specific handshake, write, and read steps.
  }

  void on_timeout() {
    if (!lifetime_.active()) {
      return;
    }
    (void)lifetime_.abort(make_client_error(errc::timeout, lifetime_.phase()));
  }

  void finish_with_reply(reply value, bool reusable) {
    if (!lifetime_.active()) {
      return;
    }

    if (reusable) {
      release_to_pool(std::move(stream_));
    } else if (stream_) {
      stream_.close();
    }
    (void)lifetime_.complete(std::move(value));
  }

  uvp::detail::operation_lifetime<operation_result> lifetime_;
  uvp::dns::resolve_operation resolve_;
  uvp::io::connect_operation connect_;
  uvp::tls::handshake_operation tls_;
  uvp::io::byte_stream stream_;
  // resolver_, connector_, query_, and module-specific state are omitted.
};
```

The members in the example are declared in an order that keeps the lifetime
alive while its callback and abort action run. The returned public operation
handle should retain `request_state`; every asynchronous callback captures a
`shared_ptr` (`self`) rather than a raw pointer.

## Choose the right terminal method

The three terminal methods all deliver one result. Their cleanup semantics are
different:

| Method | Use it when | Effect before the user callback |
| --- | --- | --- |
| `complete(result)` | The result is already final and no composite cleanup is required. | Runs the finish action, but not the abort action. |
| `abort(result)` | A timeout, protocol error, or failed later phase leaves active work or a partly used stream. | Runs the finish action, then cancels children through the abort action. |
| `cancel(result)` | The caller explicitly cancelled the operation. | Same cleanup as `abort`. |

For example, a DNS error before opening TCP can normally use `complete`; a
malformed reply after writing bytes should use `abort`, because that TCP stream
cannot be returned to a pool. A successful reply is completed only after the
protocol has either transferred the stream to its next owner or released it to
the pool.

All terminal methods set the terminal flag before invoking cleanup. They run a
configured finish action first, then `abort` and `cancel` run the abort action.
If cancelling a child synchronously calls back into `request_state`, its
callback observes `!lifetime_.active()` and cannot replace the chosen result.

## Phase names, timers, and callback policy

Call `enter_phase` immediately before starting each externally visible step:
`"resolve"`, `"connect"`, `"tls-handshake"`, `"protocol-handshake"`,
`"write"`, and `"read"` are useful conventional names. A timeout owner uses
`phase()` to construct the module's own error detail; the lifetime helper does
not create timers or impose an error category.

All calls must occur on the owning `uv::loop` thread. The helper is not
thread-safe and is deliberately free of locks and atomics. It invokes the user
callback inline, after it has become terminal and after an abort action has
run. Consequently, never invoke the public completion callback directly from
a child callback: finish through `operation_lifetime` instead.

The finish and abort actions must not throw. The finish action is the right
place to stop timers or unregister work shared by all terminal paths. Limit
the abort action to work that is safe and idempotent: cancelling child handles
and closing a non-reusable stream. It must not return a stream to a connection
pool; pooling is a successful protocol-state transition and belongs on the
explicit success path.

## A final checklist

Before exposing a new operation, check that:

- one state object owns the callback, child operations, timers, and temporary
  stream;
- the finish action stops timers and releases registrations shared by all
  terminal paths;
- every asynchronous entry starts with an `active()` check;
- every terminal path uses `complete`, `abort`, or `cancel` exactly once;
- abort cleanup cancels all active children and closes unsafe transports;
- success transfers, closes, or pools the transport before `complete`;
- timeout errors identify `lifetime_.phase()`;
- operation and protocol tests cover success, cancellation, timeout, and a
  late child callback after cancellation.
