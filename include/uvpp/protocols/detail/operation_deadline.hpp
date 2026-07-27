#pragma once

#include <uvpp/protocols/detail/operation_phase.hpp>

#include <uvpp/handles/timer.hpp>
#include <uvpp/uv.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace uvp::detail {

inline constexpr operation_phase overall_deadline_phase{"overall-deadline"};

// Owns the timer mechanics for one loop-affine protocol operation.
//
// A phase timeout is replaced when arm_phase is called again. An overall
// deadline is independent and is never restarted by phase transitions. The
// owner does not decide how a protocol reacts to expiry: its callback receives
// the static phase that expired and normally aborts an operation_lifetime.
class operation_deadline {
public:
  using timeout_callback = std::function<void(operation_phase)>;

  operation_deadline(uv::loop& loop, timeout_callback on_timeout)
      : state_(std::make_shared<state>(loop, std::move(on_timeout))) {}

  ~operation_deadline() {
    stop();
  }

  operation_deadline(const operation_deadline&) = delete;
  operation_deadline& operator=(const operation_deadline&) = delete;
  operation_deadline(operation_deadline&&) = delete;
  operation_deadline& operator=(operation_deadline&&) = delete;

  // Replaces the active phase timeout. A non-positive duration records the
  // phase but leaves its timer disabled.
  void arm_phase(operation_phase phase, std::chrono::milliseconds duration) {
    auto& state = *state_;
    stop_timer(state.phase_timer);
    ++state.phase_generation;
    state.phase = phase;
    if (duration <= std::chrono::milliseconds{0}) {
      return;
    }

    const auto generation = state.phase_generation;
    auto timer = std::make_shared<uv::timer>(*state.loop);
    state.phase_timer = timer;
    auto weak_state = std::weak_ptr<struct state>{state_};
    timer->start(duration, [weak_state, generation](uv::timer&) {
      const auto state = weak_state.lock();
      if (!state || generation != state->phase_generation || !state->phase) {
        return;
      }

      const auto phase = *state->phase;
      stop_timer(state->phase_timer);
      ++state->phase_generation;
      if (state->on_timeout) {
        state->on_timeout(phase);
      }
    });
  }

  // Arms a deadline measured from this call. It is deliberately unaffected by
  // arm_phase and therefore represents a total operation budget.
  void arm_deadline(std::chrono::milliseconds duration) {
    auto& state = *state_;
    stop_timer(state.deadline_timer);
    ++state.deadline_generation;
    if (duration <= std::chrono::milliseconds{0}) {
      return;
    }

    const auto generation = state.deadline_generation;
    auto timer = std::make_shared<uv::timer>(*state.loop);
    state.deadline_timer = timer;
    auto weak_state = std::weak_ptr<struct state>{state_};
    timer->start(duration, [weak_state, generation](uv::timer&) {
      const auto state = weak_state.lock();
      if (!state || generation != state->deadline_generation) {
        return;
      }

      stop_timer(state->deadline_timer);
      ++state->deadline_generation;
      if (state->on_timeout) {
        state->on_timeout(overall_deadline_phase);
      }
    });
  }

  // Stops the timer but preserves the last phase for diagnostics.
  void disarm_phase() noexcept {
    auto& state = *state_;
    stop_timer(state.phase_timer);
    ++state.phase_generation;
  }

  // Stops both timer kinds. This is normally an operation_lifetime finish
  // action, so success, failure, and cancellation all release the handles.
  void stop() noexcept {
    if (!state_) {
      return;
    }

    auto& state = *state_;
    stop_timer(state.phase_timer);
    stop_timer(state.deadline_timer);
    ++state.phase_generation;
    ++state.deadline_generation;
  }

private:
  struct state {
    state(uv::loop& loop_value, timeout_callback callback)
        : loop(&loop_value), on_timeout(std::move(callback)) {}

    uv::loop* loop;
    timeout_callback on_timeout;
    std::shared_ptr<uv::timer> phase_timer;
    std::shared_ptr<uv::timer> deadline_timer;
    std::optional<operation_phase> phase;
    std::size_t phase_generation = 0;
    std::size_t deadline_generation = 0;
  };

  static void stop_timer(std::shared_ptr<uv::timer>& slot) noexcept {
    if (!slot) {
      return;
    }

    auto timer = std::move(slot);
    if (timer->closing()) {
      return;
    }

    try {
      timer->stop();
    } catch (...) {
    }
    timer->close([timer](uv::timer&) {});
  }

  std::shared_ptr<state> state_;
};

} // namespace uvp::detail
