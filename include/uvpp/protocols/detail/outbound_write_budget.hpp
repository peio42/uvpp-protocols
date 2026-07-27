#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace uvp::detail {

// Result of reserving bytes in an outbound queue. A rejected reservation owns
// no bytes; a caller maps it to its protocol-specific error result.
struct write_admission {
  bool accepted = false;
  bool should_continue = false;
};

// Tracks a strictly bounded amount of queued and in-flight outbound data.
//
// Reaching the high watermark accepts the write but asks the producer to
// pause. Once paused, no further reservation is accepted until completions
// lower the outstanding bytes to the low watermark. release() returns true
// exactly for that backpressured-to-drained transition.
class outbound_write_budget {
public:
  // A zero high watermark disables the limit. Otherwise the low watermark is
  // half of high, providing hysteresis between backpressure and drain.
  explicit outbound_write_budget(std::size_t high_watermark) noexcept
      : high_watermark_(high_watermark), low_watermark_(high_watermark / 2) {}

  [[nodiscard]] write_admission try_acquire(std::size_t bytes) noexcept {
    if (backpressured_ || bytes > available_capacity()) {
      return {};
    }

    pending_bytes_ += bytes;
    if (high_watermark_ != 0 && pending_bytes_ >= high_watermark_) {
      backpressured_ = true;
      return {true, false};
    }
    return {true, true};
  }

  // Returns true only when a producer that was told to pause may resume.
  [[nodiscard]] bool release(std::size_t bytes) noexcept {
    pending_bytes_ -= std::min(pending_bytes_, bytes);
    if (!backpressured_ || pending_bytes_ > low_watermark_) {
      return false;
    }

    backpressured_ = false;
    return true;
  }

  void clear() noexcept {
    pending_bytes_ = 0;
    backpressured_ = false;
  }

  [[nodiscard]] std::size_t pending_bytes() const noexcept { return pending_bytes_; }
  [[nodiscard]] bool backpressured() const noexcept { return backpressured_; }

private:
  [[nodiscard]] std::size_t available_capacity() const noexcept {
    if (high_watermark_ == 0) {
      return std::numeric_limits<std::size_t>::max() - pending_bytes_;
    }
    return high_watermark_ - pending_bytes_;
  }

  std::size_t high_watermark_ = 0;
  std::size_t low_watermark_ = 0;
  std::size_t pending_bytes_ = 0;
  bool backpressured_ = false;
};

} // namespace uvp::detail
