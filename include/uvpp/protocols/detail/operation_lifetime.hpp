#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace uvp::detail {

// Coordinates the terminal result of one loop-affine protocol operation.
//
// Result is normally uvp::result<T>, but the helper deliberately does not
// depend on a particular error category or result representation.
template<class Result>
class operation_lifetime {
public:
  using callback = std::function<void(Result)>;
  using finish_action = std::function<void()>;
  using abort_action = std::function<void()>;

  explicit operation_lifetime(callback done)
      : done_(std::move(done)) {}

  operation_lifetime(const operation_lifetime&) = delete;
  operation_lifetime& operator=(const operation_lifetime&) = delete;
  operation_lifetime(operation_lifetime&&) = delete;
  operation_lifetime& operator=(operation_lifetime&&) = delete;

  [[nodiscard]] bool active() const noexcept { return !completed_; }
  [[nodiscard]] bool completed() const noexcept { return completed_; }
  [[nodiscard]] bool has_callback() const noexcept { return static_cast<bool>(done_); }

  // Replaces the terminal callback while the operation is active. This lets a
  // configurable operation attach its callback after construction.
  void set_callback(callback done) {
    if (!completed_) {
      done_ = std::move(done);
    }
  }

  void enter_phase(std::string_view name) {
    if (!completed_) {
      phase_.assign(name);
    }
  }

  [[nodiscard]] std::string_view phase() const noexcept { return phase_; }

  // Replaces the work to perform before reporting any terminal result. The
  // action must not throw and is run at most once.
  void set_finish_action(finish_action action) {
    if (!completed_) {
      finish_ = std::move(action);
    }
  }

  // Replaces the work to perform before reporting a failed or cancelled
  // operation. The action must not throw and is run at most once.
  void set_abort_action(abort_action action) {
    if (!completed_) {
      abort_ = std::move(action);
    }
  }

  [[nodiscard]] bool complete(Result result) {
    return finish(std::move(result), false);
  }

  [[nodiscard]] bool abort(Result result) {
    return finish(std::move(result), true);
  }

  [[nodiscard]] bool cancel(Result result) {
    return finish(std::move(result), true);
  }

private:
  [[nodiscard]] bool finish(Result result, bool run_abort) {
    if (completed_) {
      return false;
    }

    // Establish terminal state before aborting child operations. A child can
    // complete synchronously as it is cancelled; its late result must lose.
    completed_ = true;

    auto done = std::move(done_);
    done_ = {};
    auto finish = std::move(finish_);
    finish_ = {};
    auto abort = run_abort ? std::move(abort_) : abort_action{};
    abort_ = {};

    if (finish) {
      finish();
    }
    if (abort) {
      abort();
    }
    if (done) {
      done(std::move(result));
    }
    return true;
  }

  callback done_;
  finish_action finish_;
  abort_action abort_;
  std::string phase_;
  bool completed_ = false;
};

} // namespace uvp::detail
