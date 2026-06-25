#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uvp::test {

using test_function = void (*)();

struct test_case {
  std::string_view name;
  test_function function = nullptr;
};

[[nodiscard]] inline std::vector<test_case>& registry() {
  static auto tests = std::vector<test_case>{};
  return tests;
}

struct registrar {
  registrar(std::string_view name, test_function function) {
    registry().push_back(test_case{name, function});
  }
};

class assertion_failure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

template<class Value>
concept streamable = requires(std::ostream& stream, const Value& value) {
  stream << value;
};

template<class Value>
std::string format_value(const Value& value) {
  if constexpr (streamable<Value>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<unprintable>";
  }
}

[[noreturn]] inline void fail(const char* file, int line, std::string message) {
  std::ostringstream stream;
  stream << file << ':' << line << ": " << message;
  throw assertion_failure(stream.str());
}

inline void check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    fail(file, line, std::string{"check failed: "} + expression);
  }
}

template<class Actual, class Expected>
void check_equal(
  const Actual& actual,
  const Expected& expected,
  const char* actual_expression,
  const char* expected_expression,
  const char* file,
  int line) {
  if (!(actual == expected)) {
    fail(
      file,
      line,
      std::string{"check failed: "} + actual_expression + " == " + expected_expression + " (actual: " +
        format_value(actual) + ", expected: " + format_value(expected) + ')');
  }
}

inline int run_all_tests(int argc, char** argv) {
  std::string_view filter;
  if (argc > 1) {
    filter = argv[1];
  }

  auto failed = 0;
  auto skipped = 0;
  for (const auto& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string_view::npos) {
      ++skipped;
      continue;
    }

    try {
      test.function();
      std::cout << "[ OK ] " << test.name << '\n';
    } catch (const assertion_failure& error) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << '\n' << error.what() << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << "\nunexpected exception: " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << "\nunexpected non-standard exception\n";
    }
  }

  const auto ran = static_cast<int>(registry().size()) - skipped;
  std::cout << ran << " test(s) run";
  if (skipped != 0) {
    std::cout << ", " << skipped << " skipped by filter";
  }
  std::cout << '\n';

  if (failed != 0) {
    std::cerr << failed << " test(s) failed\n";
    return 1;
  }
  return 0;
}

} // namespace uvp::test

#define UVP_TEST_CONCAT_INNER(lhs, rhs) lhs##rhs
#define UVP_TEST_CONCAT(lhs, rhs) UVP_TEST_CONCAT_INNER(lhs, rhs)

#define UVP_TEST_CASE(name) \
  static void UVP_TEST_CONCAT(uvp_test_case_, __LINE__)(); \
  static ::uvp::test::registrar UVP_TEST_CONCAT(uvp_test_registrar_, __LINE__){ \
    name, \
    &UVP_TEST_CONCAT(uvp_test_case_, __LINE__)}; \
  static void UVP_TEST_CONCAT(uvp_test_case_, __LINE__)()

#define UVP_CHECK(expression) \
  do { \
    ::uvp::test::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__); \
  } while (false)

#define UVP_REQUIRE(expression) UVP_CHECK(expression)

#define UVP_CHECK_EQ(actual, expected) \
  do { \
    const auto& uvp_test_actual = (actual); \
    const auto& uvp_test_expected = (expected); \
    ::uvp::test::check_equal( \
      uvp_test_actual, uvp_test_expected, #actual, #expected, __FILE__, __LINE__); \
  } while (false)

#define UVP_CHECK_THROWS(expression, exception_type) \
  do { \
    bool uvp_test_thrown = false; \
    try { \
      expression; \
    } catch (const exception_type&) { \
      uvp_test_thrown = true; \
    } catch (...) { \
      ::uvp::test::fail(__FILE__, __LINE__, "unexpected exception type from " #expression); \
    } \
    if (!uvp_test_thrown) { \
      ::uvp::test::fail(__FILE__, __LINE__, "expected exception " #exception_type " from " #expression); \
    } \
  } while (false)

