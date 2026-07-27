#include "test.hpp"

#include <uvpp/protocols/detail/operation_deadline.hpp>
#include <uvpp/protocols/detail/operation_lifetime.hpp>
#include <uvpp/protocols/detail/outbound_write_budget.hpp>
#include <uvpp/uv.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {

inline constexpr uvp::detail::operation_phase connect_phase{"connect"};
inline constexpr uvp::detail::operation_phase resolve_phase{"resolve"};
inline constexpr uvp::detail::operation_phase handshake_phase{"handshake"};
inline constexpr uvp::detail::operation_phase read_phase{"read"};
inline constexpr uvp::detail::operation_phase write_phase{"write"};

UVP_TEST_CASE("operation lifetime reports only its first terminal result") {
  auto results = std::vector<int>{};
  uvp::detail::operation_lifetime<int> lifetime([&](int result) {
    results.push_back(result);
  });

  lifetime.enter_phase(connect_phase);

  UVP_CHECK(lifetime.complete(200));
  UVP_CHECK(!lifetime.cancel(499));
  UVP_CHECK(!lifetime.abort(500));
  UVP_CHECK(lifetime.completed());
  UVP_CHECK_EQ(lifetime.phase(), "connect");
  UVP_CHECK_EQ(results.size(), 1U);
  UVP_CHECK_EQ(results[0], 200);
}

UVP_TEST_CASE("operation lifetime finishes and aborts before reporting failure") {
  auto events = std::vector<std::string>{};
  uvp::detail::operation_lifetime<int> lifetime([&](int result) {
    events.push_back("callback:" + std::to_string(result));
  });
  lifetime.set_finish_action([&] {
    events.push_back("finish");
  });
  lifetime.set_abort_action([&] {
    events.push_back("abort");
  });

  UVP_CHECK(lifetime.abort(500));
  UVP_CHECK_EQ(events.size(), 3U);
  UVP_CHECK_EQ(events[0], "finish");
  UVP_CHECK_EQ(events[1], "abort");
  UVP_CHECK_EQ(events[2], "callback:500");
}

UVP_TEST_CASE("operation lifetime finishes successful operations without aborting") {
  auto events = std::vector<std::string>{};
  uvp::detail::operation_lifetime<int> lifetime([&](int result) {
    events.push_back("callback:" + std::to_string(result));
  });
  lifetime.set_finish_action([&] {
    events.push_back("finish");
  });
  lifetime.set_abort_action([&] {
    events.push_back("abort");
  });

  UVP_CHECK(lifetime.complete(200));
  UVP_CHECK_EQ(events.size(), 2U);
  UVP_CHECK_EQ(events[0], "finish");
  UVP_CHECK_EQ(events[1], "callback:200");
}

UVP_TEST_CASE("operation lifetime accepts a callback attached before completion") {
  auto result = 0;
  uvp::detail::operation_lifetime<int> lifetime({});

  lifetime.set_callback([&](int value) {
    result = value;
  });

  UVP_CHECK(lifetime.complete(200));
  UVP_CHECK_EQ(result, 200);

  lifetime.set_callback([&](int value) {
    result = value + 1;
  });
  UVP_CHECK(!lifetime.has_callback());
  UVP_CHECK_EQ(result, 200);
}

UVP_TEST_CASE("operation lifetime makes cancellation win over a reentrant child completion") {
  auto results = std::vector<int>{};
  uvp::detail::operation_lifetime<int> lifetime([&](int result) {
    results.push_back(result);
  });
  lifetime.set_abort_action([&] {
    UVP_CHECK(!lifetime.complete(200));
  });

  UVP_CHECK(lifetime.cancel(499));
  UVP_CHECK_EQ(results.size(), 1U);
  UVP_CHECK_EQ(results[0], 499);
}

UVP_TEST_CASE("operation lifetime tracks phases and ignores phase changes after completion") {
  uvp::detail::operation_lifetime<int> lifetime([](int) {});

  lifetime.enter_phase(resolve_phase);
  UVP_CHECK_EQ(lifetime.phase(), "resolve");
  lifetime.enter_phase(handshake_phase);
  UVP_CHECK_EQ(lifetime.phase(), "handshake");

  UVP_CHECK(lifetime.complete(0));
  lifetime.enter_phase(read_phase);
  UVP_CHECK_EQ(lifetime.phase(), "handshake");
}

UVP_TEST_CASE("operation lifetime does not abort a successful operation") {
  auto aborts = 0;
  uvp::detail::operation_lifetime<int> lifetime([](int) {});
  lifetime.set_abort_action([&] {
    ++aborts;
  });

  UVP_CHECK(lifetime.complete(0));
  UVP_CHECK_EQ(aborts, 0);
}

UVP_TEST_CASE("operation deadline replaces an earlier phase timeout") {
  uv::loop loop;
  auto expired = std::vector<std::string_view>{};

  {
    uvp::detail::operation_deadline deadlines(loop, [&](uvp::detail::operation_phase phase) {
      expired.push_back(phase.name());
    });
    deadlines.arm_phase(connect_phase, std::chrono::milliseconds{20});
    deadlines.arm_phase(write_phase, std::chrono::milliseconds{1});
    loop.run();
  }

  loop.run();
  loop.close();

  UVP_CHECK_EQ(expired.size(), 1U);
  UVP_CHECK_EQ(expired[0], "write");
}

UVP_TEST_CASE("operation deadline makes an overall deadline independent from its phase") {
  uv::loop loop;
  auto result = 0;
  auto expired = std::string_view{};
  uvp::detail::operation_lifetime<int> lifetime([&](int value) {
    result = value;
  });

  {
    uvp::detail::operation_deadline deadlines(loop, [&](uvp::detail::operation_phase phase) {
      expired = phase.name();
      (void)lifetime.abort(408);
    });
    lifetime.set_finish_action([&]() noexcept {
      deadlines.stop();
    });
    deadlines.arm_phase(connect_phase, std::chrono::milliseconds{20});
    deadlines.arm_deadline(std::chrono::milliseconds{1});
    loop.run();
  }

  loop.run();
  loop.close();

  UVP_CHECK_EQ(expired, "overall-deadline");
  UVP_CHECK_EQ(result, 408);
}

UVP_TEST_CASE("outbound write budget enforces its high and low watermarks") {
  auto budget = uvp::detail::outbound_write_budget{10};

  const auto first = budget.try_acquire(10);
  UVP_CHECK(first.accepted);
  UVP_CHECK(!first.should_continue);
  UVP_CHECK(budget.backpressured());
  UVP_CHECK_EQ(budget.pending_bytes(), 10U);

  UVP_CHECK(!budget.try_acquire(1).accepted);
  UVP_CHECK(!budget.release(4));
  UVP_CHECK(budget.backpressured());
  UVP_CHECK(budget.release(1));
  UVP_CHECK(!budget.backpressured());
  UVP_CHECK_EQ(budget.pending_bytes(), 5U);
}

UVP_TEST_CASE("outbound write budget rejects an oversized item without retaining it") {
  auto budget = uvp::detail::outbound_write_budget{10};

  UVP_CHECK(!budget.try_acquire(11).accepted);
  UVP_CHECK_EQ(budget.pending_bytes(), 0U);
  UVP_CHECK(!budget.backpressured());
}

} // namespace
