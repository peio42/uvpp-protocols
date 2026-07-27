#include "test.hpp"

#include <uvpp/protocols/detail/operation_lifetime.hpp>

#include <string>
#include <vector>

namespace {

UVP_TEST_CASE("operation lifetime reports only its first terminal result") {
  auto results = std::vector<int>{};
  uvp::detail::operation_lifetime<int> lifetime([&](int result) {
    results.push_back(result);
  });

  lifetime.enter_phase("connect");

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

  lifetime.enter_phase("resolve");
  UVP_CHECK_EQ(lifetime.phase(), "resolve");
  lifetime.enter_phase("handshake");
  UVP_CHECK_EQ(lifetime.phase(), "handshake");

  UVP_CHECK(lifetime.complete(0));
  lifetime.enter_phase("read");
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

} // namespace
