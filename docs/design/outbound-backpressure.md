# Outbound Backpressure

`uvp::detail::outbound_write_budget` is the shared, loop-affine accounting
helper for queued and in-flight outbound bytes. Protocol implementations can
include it through:

```cpp
#include <uvpp/protocols/detail/outbound_write_budget.hpp>
```

It is deliberately a budget, not a transport or a queue. A protocol still owns
its queued item type, serialization, write ordering, transport callbacks, and
the public result or error type it exposes.

## Contract

Construct the budget with a high watermark. `0` disables the limit; otherwise
the low watermark is half the high watermark.

Call `try_acquire(bytes)` before retaining an item or starting a write. A
reservation is either rejected without changing the accounting, or accepted.
An accepted reservation that reaches the high watermark has
`should_continue == false`: the producer must pause. While paused, no further
reservation is accepted. On each completed write, call `release(bytes)` with
the exact number previously reserved. It returns `true` once, when the pending
amount reaches the low watermark and the producer may be notified to drain.

This is a strict bound: an item that would exceed the high watermark is not
accepted. A producer must split it into smaller protocol messages where that is
valid, or report its own `operation_would_block`-style result. The budget
counts the encoded bytes actually retained by the protocol, including framing
such as an HTTP chunk header or a reserved terminal chunk.

On cancellation, failure, or session close, call `clear()` after dropping the
corresponding queue. It deliberately does not report a drain callback.

## Typical composition

```cpp
uvp::detail::outbound_write_budget budget_{options.max_pending_write_bytes};

stream_write_result write_payload(std::vector<std::byte> encoded) {
  const auto admission = budget_.try_acquire(encoded.size());
  if (!admission.accepted) {
    return stream_write_result::rejected(
      std::make_error_code(std::errc::operation_would_block));
  }

  writes_.push_back(std::move(encoded));
  flush_next_write();
  return admission.should_continue
    ? stream_write_result::ready()
    : stream_write_result::backpressure();
}

void on_write_complete() {
  const auto bytes = writes_.front().size();
  writes_.pop_front();
  if (budget_.release(bytes)) {
    notify_drain();
  }
  flush_next_write();
}
```

Inbound pause/resume is intentionally separate. It depends on parser state,
application delivery, and the protocol's definition of meaningful progress;
the outbound byte budget does not call `read_stop()` or `read_start()`.
