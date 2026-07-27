# Operation Lifetime

`uvp::detail::operation_lifetime<Result>` is the shared, loop-affine helper
for a protocol operation that produces one terminal result. It is available to
protocol modules through:

```cpp
#include <uvpp/protocols/detail/operation_lifetime.hpp>
```

It is an implementation-detail foundation, rather than a general asynchronous
or Promise API. It does not schedule work, own a `uv::loop`, add threads,
convert callbacks into continuations, or define protocol errors. A protocol
state machine continues to own its transport, child operations, and error
category.

## Contract

All calls to an instance must be made on the operation's owning event-loop
thread. The helper is deliberately not thread-safe: libuv callbacks for one
operation are serialized by that loop, so atomics and locks would add cost
without making cross-thread cancellation safe.

`complete`, `abort`, and `cancel` each attempt to deliver the supplied
`Result`. The first attempt returns `true` and invokes the completion callback
once. Later attempts return `false` and do nothing. This includes completions
that arrive synchronously while an abort action cancels a child operation.

`complete` reports an already-final result without abort cleanup. Use it for,
for example, a DNS lookup that has returned either addresses or its own DNS
error.
`abort` and `cancel` run the configured abort action before dispatching their
result. They are for a failed composite operation or explicit cancellation,
when active children and a partially used transport must be stopped.

`set_finish_action` installs cleanup that runs for every terminal result. Use
it for work that must stop on both success and failure, such as a phase timer
or a registration with another owner. `set_abort_action` is reserved for work
that only applies to an aborted operation, such as closing a partially used
transport. Both actions must not throw.

The completion callback is invoked inline, on the owning loop thread, after
the helper has become terminal. It may therefore safely re-enter the owning
state, but it must not assume that the operation remains active. Validation
failures may consequently invoke a callback while a public `start` function is
still executing; this matches the existing callback convention.

`set_callback` replaces the completion callback while the lifetime remains
active. It supports configurable operations, such as a streaming HTTP request,
whose caller supplies its completion callback after constructing the operation
state. Setting it after completion has no effect.

## Using it in an operation state

The state installs an abort action that knows how to cancel its children and
make a partially used transport unavailable. That work remains
protocol-specific: a successful HTTP operation may return a stream to a pool,
while a failed WebSocket upgrade must close it.

```cpp
using result_type = uvp::result<uvp::http::response>;

class request_state {
  // ...
  uvp::detail::operation_lifetime<result_type> lifetime_;

  request_state(uvp::http::client_callback done)
      : lifetime_(std::move(done)) {
    lifetime_.set_finish_action([this]() noexcept {
      stop_phase_timer();
    });
    lifetime_.set_abort_action([this]() noexcept {
      dns_operation_.cancel();
      connect_operation_.cancel();
      tls_operation_.cancel();
      close_stream();
    });
  }

  void start_connect() {
    lifetime_.enter_phase("connect");
    // Start the protocol-specific connect operation.
  }

  void on_connected(result_type result) {
    if (!lifetime_.active()) {
      return; // A cancellation or timeout already won.
    }
    (void)lifetime_.complete(std::move(result));
  }

  void cancel() {
    (void)lifetime_.cancel(make_client_error(errc::client_cancelled));
  }
};
```

The finish and abort actions must not throw. Every terminal method first makes
the lifetime terminal, then runs the finish action, the abort action for
`abort` and `cancel`, and finally dispatches the user callback. Thus a child
operation which invokes its callback immediately from `cancel()` cannot
replace the cancellation or failure result.

Use `enter_phase` whenever the state moves through a diagnostic phase such as
`"resolve"`, `"connect"`, `"tls-handshake"`, `"protocol-handshake"`,
`"write"`, or `"read"`. The string is copied, so callers may pass either a
literal or a transient `std::string`. A future timeout/deadline owner can read
this phase when it constructs its protocol-specific timeout error.

## Boundaries

This helper intentionally does not decide whether to close or recycle a byte
stream, nor does it provide timeout timers or a cancellation-token tree.
Keeping those choices in each state machine preserves explicit transport
ownership and avoids imposing a generic protocol engine on unusual lifetimes.
