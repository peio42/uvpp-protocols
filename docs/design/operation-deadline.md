# Operation Deadlines

`uvp::detail::operation_deadline` is the shared, loop-affine owner for the
timer mechanics of one finite protocol operation. Protocol code can include it
through:

```cpp
#include <uvpp/protocols/detail/operation_deadline.hpp>
```

It complements `operation_lifetime`; it does not replace it. The deadline
owner starts, replaces, and closes `uv::timer` handles. The operation state
still decides what an expired timer means in its own error category and how to
cancel child work or close a transport.

## Contract

`arm_phase(phase, duration)` replaces the prior phase timeout. A non-positive
duration disables that phase timer. `arm_deadline(duration)` manages a second,
independent timer measured from that call. Phase changes never restart the
overall deadline.

The timeout callback receives an `operation_phase`: the phase passed to
`arm_phase`, or `overall_deadline_phase` for the global budget. Before calling
the callback, the helper invalidates and closes the timer that fired. Timers
from a replaced arm are identified by generation and ignored if their callback
was already queued.

`disarm_phase()` stops only the phase timer. `stop()` stops both timer kinds;
install it as the `finish_action` of the paired `operation_lifetime` so every
terminal path releases timer handles.

Arm a phase before starting its child operation. This also handles a child
that reports a validation failure synchronously: its terminal path will stop
the just-armed timer instead of leaving a timer for a phase that has ended.

All calls must be made on the owning event-loop thread. The callback and the
finish action must not throw.

## Typical composition

```cpp
inline constexpr uvp::detail::operation_phase resolve_phase{"resolve"};

class request_state {
  uvp::detail::operation_lifetime<result_type> lifetime_;
  uvp::detail::operation_deadline deadlines_;

  request_state(uv::loop& loop, completion_callback done)
      : lifetime_(std::move(done)),
        deadlines_(loop, [this](uvp::detail::operation_phase phase) {
          if (lifetime_.active()) {
            (void)lifetime_.abort(make_timeout_error(phase.name()));
          }
        }) {
    lifetime_.set_finish_action([this]() noexcept {
      deadlines_.stop();
    });
  }

  void start() {
    deadlines_.arm_deadline(options_.overall_timeout);
    lifetime_.enter_phase(resolve_phase);
    deadlines_.arm_phase(resolve_phase, options_.resolve_timeout);
    // Start DNS resolution.
  }
};
```

An idle timeout is different: it represents lack of I/O progress, and protocol
code must explicitly re-arm its timer after the progress it considers
meaningful. The deadline helper intentionally does not infer that policy.
