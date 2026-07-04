#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/http/response.hpp>

namespace uvp::http {

enum class hidden_file_policy {
  reject,
  allow,
  allow_well_known,
};

enum class symlink_policy {
  follow_within_root,
  reject,
};

class static_file_options {
public:
  static_file_options& path_param(std::string value) &;
  static_file_options&& path_param(std::string value) &&;
  [[nodiscard]] std::string_view path_param() const noexcept { return path_param_; }

  static_file_options& index_file(std::string value) &;
  static_file_options&& index_file(std::string value) &&;
  static_file_options& no_index_file() & noexcept;
  static_file_options&& no_index_file() && noexcept;
  [[nodiscard]] std::optional<std::string_view> index_file() const noexcept;

  static_file_options& hidden_files(hidden_file_policy value) & noexcept;
  static_file_options&& hidden_files(hidden_file_policy value) && noexcept;
  [[nodiscard]] hidden_file_policy hidden_files() const noexcept { return hidden_files_; }

  static_file_options& symlinks(symlink_policy value) & noexcept;
  static_file_options&& symlinks(symlink_policy value) && noexcept;
  [[nodiscard]] symlink_policy symlinks() const noexcept { return symlinks_; }

  static_file_options& cache_control(std::string value) &;
  static_file_options&& cache_control(std::string value) &&;
  static_file_options& no_cache_control() & noexcept;
  static_file_options&& no_cache_control() && noexcept;
  [[nodiscard]] std::optional<std::string_view> cache_control() const noexcept;

  static_file_options& etag(bool value) & noexcept;
  static_file_options&& etag(bool value) && noexcept;
  [[nodiscard]] bool etag() const noexcept { return etag_; }

  static_file_options& last_modified(bool value) & noexcept;
  static_file_options&& last_modified(bool value) && noexcept;
  [[nodiscard]] bool last_modified() const noexcept { return last_modified_; }

  static_file_options& nosniff(bool value) & noexcept;
  static_file_options&& nosniff(bool value) && noexcept;
  [[nodiscard]] bool nosniff() const noexcept { return nosniff_; }

  static_file_options& chunk_size(std::size_t value) &;
  static_file_options&& chunk_size(std::size_t value) &&;
  [[nodiscard]] std::size_t chunk_size() const noexcept { return chunk_size_; }

private:
  std::string path_param_ = "path";
  std::optional<std::string> index_file_ = std::string{"index.html"};
  hidden_file_policy hidden_files_ = hidden_file_policy::reject;
  symlink_policy symlinks_ = symlink_policy::follow_within_root;
  std::optional<std::string> cache_control_ = std::string{"no-cache"};
  bool etag_ = true;
  bool last_modified_ = true;
  bool nosniff_ = true;
  std::size_t chunk_size_ = 64 * 1024;
};

class static_file_handler {
public:
  static_file_handler(std::filesystem::path root, static_file_options options = {});

  static_file_handler& options(static_file_options value) &;
  static_file_handler&& options(static_file_options value) &&;

  static_file_handler& path_param(std::string value) &;
  static_file_handler&& path_param(std::string value) &&;
  static_file_handler& index_file(std::string value) &;
  static_file_handler&& index_file(std::string value) &&;
  static_file_handler& no_index_file() & noexcept;
  static_file_handler&& no_index_file() && noexcept;
  static_file_handler& hidden_files(hidden_file_policy value) & noexcept;
  static_file_handler&& hidden_files(hidden_file_policy value) && noexcept;
  static_file_handler& symlinks(symlink_policy value) & noexcept;
  static_file_handler&& symlinks(symlink_policy value) && noexcept;
  static_file_handler& cache_control(std::string value) &;
  static_file_handler&& cache_control(std::string value) &&;
  static_file_handler& no_cache_control() & noexcept;
  static_file_handler&& no_cache_control() && noexcept;
  static_file_handler& etag(bool value) & noexcept;
  static_file_handler&& etag(bool value) && noexcept;
  static_file_handler& last_modified(bool value) & noexcept;
  static_file_handler&& last_modified(bool value) && noexcept;
  static_file_handler& nosniff(bool value) & noexcept;
  static_file_handler&& nosniff(bool value) && noexcept;
  static_file_handler& chunk_size(std::size_t value) &;
  static_file_handler&& chunk_size(std::size_t value) &&;

  void operator()(request& req, response& res) const;

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] const static_file_options& options() const noexcept { return options_; }

private:
  std::filesystem::path root_;
  static_file_options options_;
};

[[nodiscard]] static_file_handler static_files(std::filesystem::path root, static_file_options options = {});

} // namespace uvp::http
