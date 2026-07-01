#include <uvpp/protocols/http/detail/route_path.hpp>

#include <utility>

namespace uvp::http::detail {

namespace {

int hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool decode_path_segment(std::string_view segment, std::string& decoded) {
  decoded.clear();
  decoded.reserve(segment.size());

  for (std::size_t offset = 0; offset < segment.size(); ++offset) {
    const auto value = segment[offset];
    if (value != '%') {
      decoded.push_back(value);
      continue;
    }

    if (offset + 2 >= segment.size()) {
      return false;
    }

    const auto high = hex_value(segment[offset + 1]);
    const auto low = hex_value(segment[offset + 2]);
    if (high < 0 || low < 0) {
      return false;
    }

    decoded.push_back(static_cast<char>((high << 4) | low));
    offset += 2;
  }

  return true;
}

} // namespace

std::span<const std::string> route_path::segments(route_path_matching matching) const noexcept {
  const auto& selected = matching == route_path_matching::raw ? raw_segments : decoded_segments;
  return std::span<const std::string>{selected.data(), selected.size()};
}

route_path parse_route_path(std::string_view path) {
  route_path parsed;

  std::size_t offset = 0;
  while (offset <= path.size()) {
    const auto next = path.find('/', offset);
    const auto segment_end = next == std::string_view::npos ? path.size() : next;
    const auto segment = path.substr(offset, segment_end - offset);

    if (!segment.empty()) {
      std::string decoded;
      if (!decode_path_segment(segment, decoded)) {
        parsed.valid = false;
        return parsed;
      }
      parsed.raw_segments.emplace_back(segment);
      parsed.decoded_segments.push_back(std::move(decoded));
    }

    if (next == std::string_view::npos) {
      break;
    }
    offset = next + 1;
  }

  return parsed;
}

std::string join_route_segments(std::span<const std::string> segments, std::size_t first) {
  std::string joined;
  for (std::size_t index = first; index < segments.size(); ++index) {
    if (index > first) {
      joined.push_back('/');
    }
    joined.append(segments[index]);
  }
  return joined;
}

} // namespace uvp::http::detail
