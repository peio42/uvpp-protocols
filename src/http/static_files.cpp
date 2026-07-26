#include <uvpp/protocols/http/static_files.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
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

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http/status.hpp>
#include <uvpp/protocols/http/headers.hpp>

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

std::time_t file_time_to_time_t(uv::fs::file_time value) {
  const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(value);
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

bool is_not_modified(
  std::string_view if_none_match,
  std::string_view if_modified_since,
  std::string_view etag,
  std::optional<std::time_t> modified) {
  if (!if_none_match.empty() && !etag.empty()) {
    return if_none_match_matches(if_none_match, etag);
  }

  if (!modified) {
    return false;
  }

  if (if_modified_since.empty()) {
    return false;
  }

  const auto since = parse_http_date(if_modified_since);
  return since && *since >= *modified;
}

bool permission_denied(std::error_code error) noexcept {
  return error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
    error == uv::make_error_code(UV_EACCES) || error == uv::make_error_code(UV_EPERM);
}

std::filesystem::path path_from_argument(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const auto byte : value) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return std::filesystem::path{utf8};
}

class static_file_operation : public std::enable_shared_from_this<static_file_operation> {
public:
  static_file_operation(
    uv::loop& loop,
    std::string root_argument,
    static_file_options options,
    std::vector<std::string> components,
    std::string if_none_match,
    std::string if_modified_since,
    streaming_response stream,
    bool content_type_set,
    bool cache_control_set,
    bool nosniff_set,
    bool suppress_body)
      : loop_(loop),
        root_argument_(std::move(root_argument)),
        options_(std::move(options)),
        components_(std::move(components)),
        if_none_match_(std::move(if_none_match)),
        if_modified_since_(std::move(if_modified_since)),
        stream_(std::move(stream)),
        content_type_set_(content_type_set),
        cache_control_set_(cache_control_set),
        nosniff_set_(nosniff_set),
        suppress_body_(suppress_body) {}

  void start() {
    auto self = shared_from_this();
    stream_
      .on_drain([self] {
        self->read_next();
      })
      .on_cancel([self] {
        self->cancel();
      })
      .on_error([self](std::error_code) {
        self->cancel();
      });

    resolve_root();
  }

private:
  void resolve_root() {
    auto self = shared_from_this();
    uv::fs::realpath(loop_, root_argument_, [self](uv::fs::path_result result) {
      if (self->cancelled_ || self->completed_) {
        return;
      }
      if (!result) {
        self->respond_internal_error();
        return;
      }
      self->canonical_root_ = path_from_argument(result.path());
      self->stat_root();
    });
  }

  void stat_root() {
    auto self = shared_from_this();
    uv::fs::stat(loop_, uv::fs::path_argument(canonical_root_), [self](uv::fs::stat_result result) {
      if (self->cancelled_ || self->completed_) {
        return;
      }
      if (!result || !result.file_status().is_directory()) {
        self->respond_internal_error();
        return;
      }
      self->resolve_components();
    });
  }

  void resolve_components() {
    if (cancelled_ || completed_) {
      return;
    }

    candidate_ = canonical_root_;
    for (const auto& component : components_) {
      candidate_ /= component;
    }
    checked_component_count_ = 0;
    if (options_.symlinks() == symlink_policy::reject) {
      reject_symlinks();
    } else {
      resolve_candidate();
    }
  }

  void reject_symlinks() {
    if (cancelled_ || completed_) {
      return;
    }
    if (checked_component_count_ == components_.size()) {
      resolve_candidate();
      return;
    }

    auto component = canonical_root_;
    for (std::size_t index = 0; index <= checked_component_count_; ++index) {
      component /= components_[index];
    }
    auto self = shared_from_this();
    uv::fs::lstat(loop_, uv::fs::path_argument(component), [self](uv::fs::stat_result result) {
      if (self->cancelled_ || self->completed_) {
        return;
      }
      if (!result) {
        self->resolve_failure(result.error_code());
        return;
      }
      if (result.file_status().is_symlink()) {
        self->respond_not_found();
        return;
      }
      ++self->checked_component_count_;
      self->reject_symlinks();
    });
  }

  void resolve_candidate() {
    auto self = shared_from_this();
    uv::fs::realpath(loop_, uv::fs::path_argument(candidate_), [self](uv::fs::path_result result) {
      if (self->cancelled_ || self->completed_) {
        return;
      }
      if (!result) {
        self->resolve_failure(result.error_code());
        return;
      }
      self->target_ = path_from_argument(result.path());
      if (!is_relative_to(self->target_, self->canonical_root_)) {
        self->respond_not_found();
        return;
      }
      self->stat_target();
    });
  }

  void stat_target() {
    auto self = shared_from_this();
    uv::fs::stat(loop_, uv::fs::path_argument(target_), [self](uv::fs::stat_result result) {
      if (self->cancelled_ || self->completed_) {
        return;
      }
      if (!result) {
        self->resolve_failure(result.error_code());
        return;
      }
      if (result.file_status().is_directory()) {
        self->resolve_index();
        return;
      }
      if (!result.file_status().is_regular()) {
        self->respond_not_found();
        return;
      }
      self->open_file();
    });
  }

  void resolve_index() {
    const auto index = options_.index_file();
    if (!index) {
      respond_not_found();
      return;
    }

    auto indexed_components = components_;
    indexed_components.emplace_back(*index);
    if (!hidden_allowed(indexed_components, options_.hidden_files())) {
      respond_not_found();
      return;
    }
    components_ = std::move(indexed_components);
    resolve_components();
  }

  void open_file() {
    auto self = shared_from_this();
    uv::fs::open(loop_, uv::fs::path_argument(target_), O_RDONLY, 0, [self](uv::fs::open_result result) {
      if (!result) {
        if (!self->cancelled_ && !self->completed_) {
          self->resolve_failure(result.error_code());
        }
        return;
      }
      self->file_ = result.file();
      if (self->cancelled_ || self->completed_) {
        self->close_file();
        return;
      }
      self->stat_open_file();
    });
  }

  void stat_open_file() {
    file_operation_in_flight_ = true;
    auto self = shared_from_this();
    uv::fs::fstat(loop_, file_, [self](uv::fs::stat_result result) {
      self->file_operation_in_flight_ = false;
      if (self->cancelled_ || self->completed_) {
        self->close_file();
        return;
      }
      if (!result) {
        self->respond_internal_error();
        self->close_file();
        return;
      }
      if (!result.file_status().is_regular()) {
        self->respond_not_found();
        self->close_file();
        return;
      }
      self->prepare_response(result.file_status());
    });
  }

  void prepare_response(const uv::fs::file_status& metadata) {
    const auto modified = file_time_to_time_t(metadata.modification_time());
    const auto size = metadata.size();
    const auto etag = options_.etag() ? make_etag(size, modified) : std::string{};

    if ((!etag.empty() || options_.last_modified()) &&
        is_not_modified(if_none_match_, if_modified_since_, etag, modified)) {
      stream_.status(status::not_modified);
      set_cache_headers(etag, modified);
      completed_ = true;
      stream_.end();
      close_file();
      return;
    }

    const auto extension = to_lower(target_.extension().string());
    if (!content_type_set_) {
      stream_.type(content_type_for_extension(extension));
    }
    stream_.header("content-length", std::to_string(size));
    set_cache_headers(etag, modified);
    if (suppress_body_) {
      completed_ = true;
      stream_.end();
      close_file();
      return;
    }
    read_next();
  }

  void set_cache_headers(std::string_view etag, std::time_t modified) {
    if (options_.cache_control() && !cache_control_set_) {
      stream_.header("cache-control", *options_.cache_control());
    }
    if (!etag.empty()) {
      stream_.header("etag", etag);
    }
    if (options_.last_modified()) {
      stream_.header("last-modified", format_http_date(modified));
    }
    if (options_.nosniff() && !nosniff_set_) {
      stream_.header("x-content-type-options", "nosniff");
    }
  }

  void read_next() {
    if (cancelled_ || completed_ || file_operation_in_flight_) {
      return;
    }
    if (!stream_.active()) {
      cancel();
      return;
    }

    file_operation_in_flight_ = true;
    auto self = shared_from_this();
    uv::fs::read(loop_, file_, options_.chunk_size(), offset_, [self](uv::fs::read_result result) {
      self->file_operation_in_flight_ = false;
      if (self->cancelled_ || self->completed_) {
        self->close_file();
        return;
      }
      if (!result) {
        self->finish_stream_after_error();
        return;
      }
      if (result.count() == 0) {
        self->completed_ = true;
        self->stream_.end();
        self->close_file();
        return;
      }

      self->offset_ += static_cast<std::int64_t>(result.count());
      const auto write_result = self->stream_.write(result.bytes());
      if (!write_result.accepted()) {
        self->cancel();
        return;
      }
      if (write_result.should_continue()) {
        self->read_next();
      }
    });
  }

  void resolve_failure(std::error_code error) {
    if (permission_denied(error)) {
      respond_internal_error();
    } else {
      respond_not_found();
    }
  }

  void respond_not_found() {
    respond_with_text(status::not_found, not_found_body);
  }

  void respond_internal_error() {
    respond_with_text(status::internal_server_error, internal_error_body);
  }

  void respond_with_text(status code, std::string_view body) {
    if (cancelled_ || completed_) {
      return;
    }
    completed_ = true;
    stream_.status(code)
      .type("text/plain; charset=utf-8")
      .header("content-length", std::to_string(body.size()));
    (void)stream_.write(body);
    stream_.end();
    close_file();
  }

  void finish_stream_after_error() {
    if (cancelled_ || completed_) {
      return;
    }
    completed_ = true;
    stream_.end();
    close_file();
  }

  void cancel() {
    cancelled_ = true;
    close_file();
  }

  void close_file() {
    if (!file_ || file_operation_in_flight_ || close_in_flight_) {
      return;
    }
    close_in_flight_ = true;
    const auto file = file_;
    file_ = {};
    auto self = shared_from_this();
    uv::fs::close(loop_, file, [self](uv::fs::status_result) {
      self->close_in_flight_ = false;
    });
  }

  uv::loop& loop_;
  std::string root_argument_;
  static_file_options options_;
  std::vector<std::string> components_;
  std::string if_none_match_;
  std::string if_modified_since_;
  streaming_response stream_;
  std::filesystem::path canonical_root_;
  std::filesystem::path candidate_;
  std::filesystem::path target_;
  uv::file_descriptor file_;
  std::size_t checked_component_count_ = 0;
  std::int64_t offset_ = 0;
  bool content_type_set_ = false;
  bool cache_control_set_ = false;
  bool nosniff_set_ = false;
  bool suppress_body_ = false;
  bool file_operation_in_flight_ = false;
  bool close_in_flight_ = false;
  bool cancelled_ = false;
  bool completed_ = false;
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
  if (!headers::is_valid_value(value)) {
    throw std::invalid_argument("static file cache_control must not contain CR, LF, or NUL");
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
  if (value == 0 || value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
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

  root_argument_ = uv::fs::path_argument(root_);
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
  auto stream = res.stream();
  if (!components) {
    stream.status(status::not_found)
      .type("text/plain; charset=utf-8")
      .header("content-length", std::to_string(std::string_view{not_found_body}.size()));
    (void)stream.write(std::string_view{not_found_body});
    stream.end();
    return;
  }

  auto operation = std::make_shared<static_file_operation>(
    req.loop(),
    root_argument_,
    options_,
    *components,
    std::string(req.header("if-none-match")),
    std::string(req.header("if-modified-since")),
    std::move(stream),
    res.headers().contains("content-type"),
    res.headers().contains("cache-control"),
    res.headers().contains("x-content-type-options"),
    req.method() == method::head);
  operation->start();
}

static_file_handler static_files(std::filesystem::path root, static_file_options options) {
  return static_file_handler{std::move(root), std::move(options)};
}

} // namespace uvp::http
