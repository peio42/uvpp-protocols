#include "test.hpp"

#include <cstddef>
#include <span>
#include <string_view>

#include <uvpp/protocols/http/detail/multipart_parser.hpp>
#include <uvpp/protocols/http/multipart.hpp>

namespace {

std::span<const std::byte> as_bytes(std::string_view value) noexcept {
  return std::as_bytes(std::span{value.data(), value.size()});
}

} // namespace

UVP_TEST_CASE("multipart content type parser accepts quoted boundaries") {
  auto boundary = uvp::http::detail::parse_multipart_boundary(
    "multipart/form-data; charset=utf-8; boundary=\"AaB03x\"");

  UVP_REQUIRE(boundary);
  UVP_CHECK_EQ(boundary.value(), "AaB03x");
}

UVP_TEST_CASE("multipart content disposition parser ignores semicolons inside quoted values") {
  auto disposition = uvp::http::detail::parse_multipart_content_disposition(
    "form-data; name=\"file\"; filename=\"a;b.txt\"");

  UVP_REQUIRE(disposition);
  UVP_CHECK_EQ(disposition.value().name, "file");
  UVP_REQUIRE(disposition.value().filename.has_value());
  UVP_CHECK_EQ(*disposition.value().filename, "a;b.txt");
}

UVP_TEST_CASE("multipart content disposition parser handles escaped quoted values") {
  auto disposition = uvp::http::detail::parse_multipart_content_disposition(
    R"(form-data; name="file"; filename="a\"b.txt")");

  UVP_REQUIRE(disposition);
  UVP_REQUIRE(disposition.value().filename.has_value());
  UVP_CHECK_EQ(*disposition.value().filename, "a\"b.txt");
}

UVP_TEST_CASE("multipart content disposition parser rejects duplicate parameters") {
  auto disposition = uvp::http::detail::parse_multipart_content_disposition(
    "form-data; name=\"a\"; name=\"b\"");

  UVP_CHECK(!disposition);
  UVP_CHECK(disposition.error().code == uvp::http::make_error_code(uvp::http::errc::multipart_malformed_part_header));
}

UVP_TEST_CASE("multipart content type parser rejects duplicate boundaries") {
  auto boundary = uvp::http::detail::parse_multipart_boundary(
    "multipart/form-data; boundary=a; boundary=b");

  UVP_CHECK(!boundary);
  UVP_CHECK(boundary.error().code == uvp::http::make_error_code(uvp::http::errc::malformed_content_type));
}

UVP_TEST_CASE("multipart form collects repeated fields without collapsing order") {
  const std::string_view body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"a\"\r\n"
    "\r\n"
    "one\r\n"
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"b\"\r\n"
    "\r\n"
    "two\r\n"
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"a\"\r\n"
    "\r\n"
    "three\r\n"
    "--AaB03x--\r\n";

  auto form = uvp::http::parse_multipart_form("multipart/form-data; boundary=AaB03x", as_bytes(body));

  UVP_REQUIRE(form);
  UVP_CHECK_EQ(form.value().size(), 3U);
  UVP_CHECK_EQ(form.value().parts()[0].name(), "a");
  UVP_CHECK_EQ(form.value().parts()[1].name(), "b");
  UVP_CHECK_EQ(form.value().parts()[2].name(), "a");

  const auto a = form.value().fields("a");
  UVP_REQUIRE(a.size() == 2U);
  UVP_CHECK_EQ(a[0].text(), "one");
  UVP_CHECK_EQ(a[1].text(), "three");
  UVP_CHECK(!form.value().single_field("a"));
}

UVP_TEST_CASE("multipart form rejects files by default") {
  const std::string_view body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"a;b.txt\"\r\n"
    "\r\n"
    "abc\r\n"
    "--AaB03x--\r\n";

  auto form = uvp::http::parse_multipart_form("multipart/form-data; boundary=AaB03x", as_bytes(body));

  UVP_CHECK(!form);
  UVP_CHECK(form.error().code == uvp::http::make_error_code(uvp::http::errc::multipart_limit_exceeded));
}

UVP_TEST_CASE("multipart form accepts explicitly allowed small files") {
  const std::string_view body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"../a;b.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "abc\r\n"
    "--AaB03x--\r\n";
  uvp::http::multipart_form_options options;
  options.limits.max_file_bytes = 8;

  auto form = uvp::http::parse_multipart_form("multipart/form-data; boundary=AaB03x", as_bytes(body), options);

  UVP_REQUIRE(form);
  const auto files = form.value().files("file");
  UVP_REQUIRE(files.size() == 1U);
  UVP_CHECK_EQ(files[0].filename(), "../a;b.txt");
  UVP_CHECK_EQ(files[0].safe_filename(), "..a;b.txt");
  UVP_CHECK_EQ(files[0].text(), "abc");
  UVP_REQUIRE(form.value().first_file("file").has_value());
  UVP_REQUIRE(form.value().single_file("file"));
}

UVP_TEST_CASE("multipart form rejects field memory limits") {
  const std::string_view body =
    "--AaB03x\r\n"
    "Content-Disposition: form-data; name=\"field\"\r\n"
    "\r\n"
    "abcd\r\n"
    "--AaB03x--\r\n";
  uvp::http::multipart_form_options options;
  options.limits.max_field_bytes = 3;

  auto form = uvp::http::parse_multipart_form("multipart/form-data; boundary=AaB03x", as_bytes(body), options);

  UVP_CHECK(!form);
  UVP_CHECK(form.error().code == uvp::http::make_error_code(uvp::http::errc::multipart_limit_exceeded));
}
