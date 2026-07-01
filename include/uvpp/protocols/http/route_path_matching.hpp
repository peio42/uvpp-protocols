#pragma once

namespace uvp::http {

enum class route_path_matching {
  raw,
  percent_decoded_segments,
};

} // namespace uvp::http
