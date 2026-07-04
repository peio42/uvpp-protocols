#include <uvpp/protocols/http/static_files.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/status.hpp>

namespace uvp::http {

namespace {

constexpr auto not_found_body = "Not Found\n";
constexpr auto internal_error_body = "Internal Server Error\n";

bool contains_path_separator(std::string_view value) noexcept {
  return value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos;
}

void require_path_param(std::string_view value) {
  if (value.empty() || contains_path_separator(value) || value.front() == ':' || value.front() == '*') {
    throw std::invalid_argument("static file path_param must be a route parameter name");
  }
}

void require_index_file(std::string_view value) {
  if (value.empty() || value == "." || value == ".." || contains_path_separator(value) ||
      value.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("static file index_file must be a file name");
  }
}

std::string to_lower(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const auto ch : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string_view content_type_for_extension(std::string_view extension) noexcept {
  struct mapping {
    std::string_view extension;
    std::string_view content_type;
  };

  static constexpr auto mappings = std::array{
    mapping{".html", "text/html; charset=utf-8"},
    mapping{".htm", "text/html; charset=utf-8"},
    mapping{".css", "text/css; charset=utf-8"},
    mapping{".js", "text/javascript; charset=utf-8"},
    mapping{".mjs", "text/javascript; charset=utf-8"},
    mapping{".json", "application/json; charset=utf-8"},
    mapping{".map", "application/json; charset=utf-8"},
    mapping{".txt", "text/plain; charset=utf-8"},
    mapping{".svg", "image/svg+xml"},
    mapping{".png", "image/png"},
    mapping{".jpg", "image/jpeg"},
    mapping{".jpeg", "image/jpeg"},
    mapping{".gif", "image/gif"},
    mapping{".webp", "image/webp"},
    mapping{".ico", "image/x-icon"},
    mapping{".wasm", "application/wasm"},
    mapping{".pdf", "application/pdf"},
    mapping{".xml", "application/xml; charset=utf-8"},
  };

  for (const auto& item : mappings) {
    if (extension == item.extension) {
      return item.content_type;
    }
  }
  return "application/octet-stream";
}

bool is_relative_to(const std::filesystem::path& path, const std::filesystem::path& root) {
  auto path_it = path.begin();
  auto root_it = root.begin();
  for (; root_it != root.end(); ++root_it, ++path_it) {
    if (path_it == path.end() || *path_it != *root_it) {
      return false;
    }
  }
  return true;
}

bool is_hidden_component(std::string_view value) noexcept {
  return !value.empty() && value.front() == '.';
}

bool hidden_allowed(std::span<const std::string> components, hidden_file_policy policy) {
  if (policy == hidden_file_policy::allow) {
    return true;
  }

  for (std::size_t index = 0; index < components.size(); ++index) {
    if (!is_hidden_component(components[index])) {
      continue;
    }
    if (policy == hidden_file_policy::allow_well_known && index == 0 && components[index] == ".well-known") {
      continue;
    }
    return false;
  }
  return true;
}

bool is_safe_component(std::string_view value) noexcept {
  if (value.empty() || value == "." || value == ".." || contains_path_separator(value) ||
      value.find('\0') != std::string_view::npos) {
    return false;
  }

#ifdef _WIN32
  if (value.find(':') != std::string_view::npos) {
    return false;
  }
#endif

  return true;
}

std::vector<std::string> pattern_segments(std::string_view pattern) {
  std::vector<std::string> segments;
  std::size_t offset = 0;
  while (offset < pattern.size()) {
    const auto next = pattern.find('/', offset);
    const auto end = next == std::string_view::npos ? pattern.size() : next;
    if (end > offset) {
      segments.emplace_back(pattern.substr(offset, end - offset));
    }
    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }
  return segments;
}

std::optional<std::vector<std::string>> request_tail_components(
  const request& req,
  const static_file_options& options) {
  const auto pattern = pattern_segments(req.matched_pattern());
  const auto wildcard = std::string{"*"} + std::string(options.path_param());

  auto wildcard_it = std::find(pattern.begin(), pattern.end(), wildcard);
  if (wildcard_it == pattern.end()) {
    return std::nullopt;
  }

  const auto prefix_size = static_cast<std::size_t>(std::distance(pattern.begin(), wildcard_it));
  const auto request_segments = req.decoded_path_segments();
  if (prefix_size > request_segments.size()) {
    return std::nullopt;
  }

  std::vector<std::string> components;
  components.reserve(request_segments.size() - prefix_size);
  for (std::size_t index = prefix_size; index < request_segments.size(); ++index) {
    if (!is_safe_component(request_segments[index])) {
      return std::nullopt;
    }
    components.push_back(request_segments[index]);
  }

  if (!hidden_allowed(components, options.hidden_files())) {
    return std::nullopt;
  }

  return components;
}

std::optional<std::filesystem::path> resolve_existing_target(
  const std::filesystem::path& root,
  std::span<const std::string> components,
  symlink_policy symlinks,
  std::error_code& ec) {
  const auto canonical_root = std::filesystem::canonical(root, ec);
  if (ec) {
    return std::nullopt;
  }

  auto candidate = canonical_root;
  for (const auto& component : components) {
    candidate /= component;
    if (symlinks == symlink_policy::reject) {
      const auto link_status = std::filesystem::symlink_status(candidate, ec);
      if (ec) {
        return std::nullopt;
      }
      if (std::filesystem::is_symlink(link_status)) {
        return std::nullopt;
      }
    }
  }

  const auto canonical_candidate = std::filesystem::canonical(candidate, ec);
  if (ec) {
    return std::nullopt;
  }
  if (!is_relative_to(canonical_candidate, canonical_root)) {
    return std::nullopt;
  }
  return canonical_candidate;
}

std::string format_http_date(std::time_t value) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &value);
#else
  gmtime_r(&value, &tm);
#endif

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
  return out.str();
}

std::time_t file_time_to_time_t(std::filesystem::file_time_type value) {
  const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
    value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  return std::chrono::system_clock::to_time_t(system_time);
}

std::string make_etag(std::uintmax_t size, std::time_t modified) {
  std::ostringstream out;
  out << "W/\"" << std::hex << size << '-' << static_cast<long long>(modified) << '"';
  return out.str();
}

std::string trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

bool if_none_match_matches(std::string_view header, std::string_view etag) {
  std::size_t offset = 0;
  while (offset <= header.size()) {
    const auto next = header.find(',', offset);
    const auto end = next == std::string_view::npos ? header.size() : next;
    const auto item = trim(header.substr(offset, end - offset));
    if (item == "*" || item == etag) {
      return true;
    }
    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }
  return false;
}

std::optional<int> month_index(std::string_view value) noexcept {
  static constexpr auto months = std::array{
    std::string_view{"Jan"},
    std::string_view{"Feb"},
    std::string_view{"Mar"},
    std::string_view{"Apr"},
    std::string_view{"May"},
    std::string_view{"Jun"},
    std::string_view{"Jul"},
    std::string_view{"Aug"},
    std::string_view{"Sep"},
    std::string_view{"Oct"},
    std::string_view{"Nov"},
    std::string_view{"Dec"},
  };
  for (std::size_t index = 0; index < months.size(); ++index) {
    if (value == months[index]) {
      return static_cast<int>(index);
    }
  }
  return std::nullopt;
}

bool parse_2digits(std::string_view value, int& out) noexcept {
  if (value.size() != 2 || !std::isdigit(static_cast<unsigned char>(value[0])) ||
      !std::isdigit(static_cast<unsigned char>(value[1]))) {
    return false;
  }
  out = (value[0] - '0') * 10 + (value[1] - '0');
  return true;
}

bool parse_4digits(std::string_view value, int& out) noexcept {
  if (value.size() != 4) {
    return false;
  }
  out = 0;
  for (const auto ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    out = out * 10 + (ch - '0');
  }
  return true;
}

std::optional<std::tm> parse_http_date_tm(std::string_view value) {
  // IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
  if (value.size() != 29 || value.substr(3, 2) != ", " || value[7] != ' ' || value[11] != ' ' ||
      value[16] != ' ' || value[19] != ':' || value[22] != ':' || value.substr(25) != " GMT") {
    return std::nullopt;
  }

  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_2digits(value.substr(5, 2), day) || !parse_4digits(value.substr(12, 4), year) ||
      !parse_2digits(value.substr(17, 2), hour) || !parse_2digits(value.substr(20, 2), minute) ||
      !parse_2digits(value.substr(23, 2), second)) {
    return std::nullopt;
  }

  const auto month = month_index(value.substr(8, 3));
  if (!month || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60 || year < 1970) {
    return std::nullopt;
  }

  std::tm tm{};
  tm.tm_sec = second;
  tm.tm_min = minute;
  tm.tm_hour = hour;
  tm.tm_mday = day;
  tm.tm_mon = *month;
  tm.tm_year = year - 1900;
  tm.tm_isdst = 0;
  return tm;
}

std::optional<std::time_t> parse_http_date(std::string_view value) {
  auto tm = parse_http_date_tm(value);
  if (!tm) {
    return std::nullopt;
  }
#ifdef _WIN32
  return _mkgmtime(&*tm);
#else
  return timegm(&*tm);
#endif
}

bool is_not_modified(const request& req, std::string_view etag, std::optional<std::time_t> modified) {
  const auto if_none_match = req.header("if-none-match");
  if (!if_none_match.empty() && !etag.empty()) {
    return if_none_match_matches(if_none_match, etag);
  }

  if (!modified) {
    return false;
  }

  const auto if_modified_since = req.header("if-modified-since");
  if (if_modified_since.empty()) {
    return false;
  }

  const auto since = parse_http_date(if_modified_since);
  return since && *since >= *modified;
}

void not_found(response& res) {
  res.status(status::not_found).type("text/plain; charset=utf-8").text(not_found_body);
}

void internal_error(response& res) {
  res.status(status::internal_server_error).type("text/plain; charset=utf-8").text(internal_error_body);
}

void set_header_unless_present(response& res, std::string_view name, std::string_view value) {
  if (!res.headers().contains(name)) {
    res.header(name, value);
  }
}

class file_sender : public std::enable_shared_from_this<file_sender> {
public:
  file_sender(std::ifstream file, streaming_response stream, std::size_t chunk_size)
      : file_(std::move(file)),
        stream_(std::move(stream)),
        buffer_(chunk_size) {}

  void start() {
    auto self = shared_from_this();
    stream_
      .on_drain([self] {
        self->send_more();
      })
      .on_cancel([self] {
        self->close();
      })
      .on_error([self](std::error_code) {
        self->close();
      });

    send_more();
  }

private:
  void send_more() {
    if (!file_.is_open() || !stream_.active()) {
      close();
      return;
    }

    while (file_) {
      file_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
      const auto count = file_.gcount();
      if (count < 0) {
        close();
        return;
      }
      if (count == 0) {
        if (file_.eof()) {
          close();
          stream_.end();
        }
        return;
      }

      auto result = stream_.write(std::string_view{buffer_.data(), static_cast<std::size_t>(count)});
      if (!result.accepted()) {
        close();
        return;
      }
      if (!result.should_continue()) {
        return;
      }
    }

    if (file_.eof()) {
      close();
      stream_.end();
    }
  }

  void close() {
    if (file_.is_open()) {
      file_.close();
    }
  }

  std::ifstream file_;
  streaming_response stream_;
  std::vector<char> buffer_;
};

} // namespace

static_file_options& static_file_options::path_param(std::string value) & {
  require_path_param(value);
  path_param_ = std::move(value);
  return *this;
}

static_file_options&& static_file_options::path_param(std::string value) && {
  path_param(std::move(value));
  return std::move(*this);
}

static_file_options& static_file_options::index_file(std::string value) & {
  require_index_file(value);
  index_file_ = std::move(value);
  return *this;
}

static_file_options&& static_file_options::index_file(std::string value) && {
  index_file(std::move(value));
  return std::move(*this);
}

static_file_options& static_file_options::no_index_file() & noexcept {
  index_file_.reset();
  return *this;
}

static_file_options&& static_file_options::no_index_file() && noexcept {
  no_index_file();
  return std::move(*this);
}

std::optional<std::string_view> static_file_options::index_file() const noexcept {
  if (!index_file_) {
    return std::nullopt;
  }
  return std::string_view{*index_file_};
}

static_file_options& static_file_options::hidden_files(hidden_file_policy value) & noexcept {
  hidden_files_ = value;
  return *this;
}

static_file_options&& static_file_options::hidden_files(hidden_file_policy value) && noexcept {
  hidden_files(value);
  return std::move(*this);
}

static_file_options& static_file_options::symlinks(symlink_policy value) & noexcept {
  symlinks_ = value;
  return *this;
}

static_file_options&& static_file_options::symlinks(symlink_policy value) && noexcept {
  symlinks(value);
  return std::move(*this);
}

static_file_options& static_file_options::cache_control(std::string value) & {
  if (value.empty()) {
    throw std::invalid_argument("static file cache_control must not be empty");
  }
  cache_control_ = std::move(value);
  return *this;
}

static_file_options&& static_file_options::cache_control(std::string value) && {
  cache_control(std::move(value));
  return std::move(*this);
}

static_file_options& static_file_options::no_cache_control() & noexcept {
  cache_control_.reset();
  return *this;
}

static_file_options&& static_file_options::no_cache_control() && noexcept {
  no_cache_control();
  return std::move(*this);
}

std::optional<std::string_view> static_file_options::cache_control() const noexcept {
  if (!cache_control_) {
    return std::nullopt;
  }
  return std::string_view{*cache_control_};
}

static_file_options& static_file_options::etag(bool value) & noexcept {
  etag_ = value;
  return *this;
}

static_file_options&& static_file_options::etag(bool value) && noexcept {
  etag(value);
  return std::move(*this);
}

static_file_options& static_file_options::last_modified(bool value) & noexcept {
  last_modified_ = value;
  return *this;
}

static_file_options&& static_file_options::last_modified(bool value) && noexcept {
  last_modified(value);
  return std::move(*this);
}

static_file_options& static_file_options::nosniff(bool value) & noexcept {
  nosniff_ = value;
  return *this;
}

static_file_options&& static_file_options::nosniff(bool value) && noexcept {
  nosniff(value);
  return std::move(*this);
}

static_file_options& static_file_options::chunk_size(std::size_t value) & {
  if (value == 0 || value > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::invalid_argument("static file chunk_size must be greater than zero");
  }
  chunk_size_ = value;
  return *this;
}

static_file_options&& static_file_options::chunk_size(std::size_t value) && {
  chunk_size(value);
  return std::move(*this);
}

static_file_handler::static_file_handler(std::filesystem::path root, static_file_options options)
    : root_(std::move(root)),
      options_(std::move(options)) {
  if (root_.empty()) {
    throw std::invalid_argument("static file root must not be empty");
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(root_, ec) || ec) {
    throw std::invalid_argument("static file root must be an existing directory");
  }
}

static_file_handler& static_file_handler::options(static_file_options value) & {
  options_ = std::move(value);
  return *this;
}

static_file_handler&& static_file_handler::options(static_file_options value) && {
  options(std::move(value));
  return std::move(*this);
}

static_file_handler& static_file_handler::path_param(std::string value) & {
  options_.path_param(std::move(value));
  return *this;
}

static_file_handler&& static_file_handler::path_param(std::string value) && {
  path_param(std::move(value));
  return std::move(*this);
}

static_file_handler& static_file_handler::index_file(std::string value) & {
  options_.index_file(std::move(value));
  return *this;
}

static_file_handler&& static_file_handler::index_file(std::string value) && {
  index_file(std::move(value));
  return std::move(*this);
}

static_file_handler& static_file_handler::no_index_file() & noexcept {
  options_.no_index_file();
  return *this;
}

static_file_handler&& static_file_handler::no_index_file() && noexcept {
  no_index_file();
  return std::move(*this);
}

static_file_handler& static_file_handler::hidden_files(hidden_file_policy value) & noexcept {
  options_.hidden_files(value);
  return *this;
}

static_file_handler&& static_file_handler::hidden_files(hidden_file_policy value) && noexcept {
  hidden_files(value);
  return std::move(*this);
}

static_file_handler& static_file_handler::symlinks(symlink_policy value) & noexcept {
  options_.symlinks(value);
  return *this;
}

static_file_handler&& static_file_handler::symlinks(symlink_policy value) && noexcept {
  symlinks(value);
  return std::move(*this);
}

static_file_handler& static_file_handler::cache_control(std::string value) & {
  options_.cache_control(std::move(value));
  return *this;
}

static_file_handler&& static_file_handler::cache_control(std::string value) && {
  cache_control(std::move(value));
  return std::move(*this);
}

static_file_handler& static_file_handler::no_cache_control() & noexcept {
  options_.no_cache_control();
  return *this;
}

static_file_handler&& static_file_handler::no_cache_control() && noexcept {
  no_cache_control();
  return std::move(*this);
}

static_file_handler& static_file_handler::etag(bool value) & noexcept {
  options_.etag(value);
  return *this;
}

static_file_handler&& static_file_handler::etag(bool value) && noexcept {
  etag(value);
  return std::move(*this);
}

static_file_handler& static_file_handler::last_modified(bool value) & noexcept {
  options_.last_modified(value);
  return *this;
}

static_file_handler&& static_file_handler::last_modified(bool value) && noexcept {
  last_modified(value);
  return std::move(*this);
}

static_file_handler& static_file_handler::nosniff(bool value) & noexcept {
  options_.nosniff(value);
  return *this;
}

static_file_handler&& static_file_handler::nosniff(bool value) && noexcept {
  nosniff(value);
  return std::move(*this);
}

static_file_handler& static_file_handler::chunk_size(std::size_t value) & {
  options_.chunk_size(value);
  return *this;
}

static_file_handler&& static_file_handler::chunk_size(std::size_t value) && {
  chunk_size(value);
  return std::move(*this);
}

void static_file_handler::operator()(request& req, response& res) const {
  const auto components = request_tail_components(req, options_);
  if (!components) {
    not_found(res);
    return;
  }

  std::error_code ec;
  auto target = resolve_existing_target(root_, *components, options_.symlinks(), ec);
  if (!target) {
    if (ec == std::errc::permission_denied) {
      internal_error(res);
    } else {
      not_found(res);
    }
    return;
  }

  auto status_value = std::filesystem::status(*target, ec);
  if (ec) {
    internal_error(res);
    return;
  }

  if (std::filesystem::is_directory(status_value)) {
    const auto index = options_.index_file();
    if (!index) {
      not_found(res);
      return;
    }

    std::vector<std::string> indexed_components = *components;
    indexed_components.emplace_back(*index);
    if (!hidden_allowed(indexed_components, options_.hidden_files())) {
      not_found(res);
      return;
    }

    target = resolve_existing_target(root_, indexed_components, options_.symlinks(), ec);
    if (!target) {
      if (ec == std::errc::permission_denied) {
        internal_error(res);
      } else {
        not_found(res);
      }
      return;
    }
    status_value = std::filesystem::status(*target, ec);
    if (ec) {
      internal_error(res);
      return;
    }
  }

  if (!std::filesystem::is_regular_file(status_value)) {
    not_found(res);
    return;
  }

  const auto size = std::filesystem::file_size(*target, ec);
  if (ec) {
    internal_error(res);
    return;
  }

  const auto modified_time = std::filesystem::last_write_time(*target, ec);
  std::optional<std::time_t> modified;
  if (!ec) {
    modified = file_time_to_time_t(modified_time);
  }

  std::string etag;
  if (options_.etag() && modified) {
    etag = make_etag(size, *modified);
  }

  if ((!etag.empty() || modified) && is_not_modified(req, etag, modified)) {
    res.status(status::not_modified);
    if (options_.cache_control()) {
      set_header_unless_present(res, "cache-control", *options_.cache_control());
    }
    if (!etag.empty()) {
      res.header("etag", etag);
    }
    if (options_.last_modified() && modified) {
      res.header("last-modified", format_http_date(*modified));
    }
    if (options_.nosniff()) {
      set_header_unless_present(res, "x-content-type-options", "nosniff");
    }
    res.end();
    return;
  }

  std::ifstream file(*target, std::ios::binary);
  if (!file) {
    internal_error(res);
    return;
  }

  const auto extension = to_lower(target->extension().string());
  set_header_unless_present(res, "content-type", content_type_for_extension(extension));
  res.header("content-length", std::to_string(size));
  if (options_.cache_control()) {
    set_header_unless_present(res, "cache-control", *options_.cache_control());
  }
  if (!etag.empty()) {
    res.header("etag", etag);
  }
  if (options_.last_modified() && modified) {
    res.header("last-modified", format_http_date(*modified));
  }
  if (options_.nosniff()) {
    set_header_unless_present(res, "x-content-type-options", "nosniff");
  }

  auto stream = res.stream();
  auto sender = std::make_shared<file_sender>(std::move(file), std::move(stream), options_.chunk_size());
  sender->start();
}

static_file_handler static_files(std::filesystem::path root, static_file_options options) {
  return static_file_handler{std::move(root), std::move(options)};
}

} // namespace uvp::http
