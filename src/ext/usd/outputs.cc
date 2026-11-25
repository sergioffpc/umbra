// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "outputs.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "umbra/core/error.h"

namespace umbra::ext::usd::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The whole part of the frame, toward zero: 12 for 12.5.
std::int64_t WholeFrame(double frame) {
  return static_cast<std::int64_t>(std::trunc(frame));
}

// The frame as given, in its shortest form: "12", "12.5".
std::string FrameText(double frame) { return std::format("{}", frame); }

// "cannot write '<file>': <reason>".
Error CannotWrite(std::string_view file, std::string_view reason) {
  return {ErrorCode::kUnavailable,
          std::format("cannot write '{}': {}", file, reason)};
}

// Whether `file` ends in .exr, in any case.
bool HasExrExtension(const std::filesystem::path& file) {
  std::string extension = file.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char letter) {
                           return static_cast<char>(std::tolower(letter));
                         });
  return extension == ".exr";
}

// Whether the process may write `path` with `mode` (access(2)), or the
// reason it may not.
std::expected<void, std::string> Accessible(const std::filesystem::path& path,
                                            int mode) {
  if (access(path.c_str(), mode) != 0) {
    return std::unexpected(std::generic_category().message(errno));
  }
  return {};
}

}  // namespace

std::string ExpandFrame(std::string_view product_name,
                        std::optional<double> frame) {
  const double value = frame.value_or(0.0);
  std::string expanded;
  for (std::size_t index = 0; index < product_name.size(); ++index) {
    const std::string_view rest = product_name.substr(index);
    if (rest.starts_with("$FF")) {
      expanded += FrameText(value);
      index += 2;
    } else if (rest.starts_with("$F4")) {
      expanded += std::format("{:04}", WholeFrame(value));
      index += 2;
    } else if (rest.starts_with("$F")) {
      expanded += std::format("{}", WholeFrame(value));
      index += 1;
    } else {
      expanded += rest.front();
    }
  }
  return expanded;
}

std::string ImplicitProductFile(std::string_view scene_path,
                                std::optional<double> frame) {
  const std::string stem = std::filesystem::path(scene_path).stem().string();
  if (!frame.has_value()) {
    return stem + ".exr";
  }
  const std::string text = FrameText(*frame);
  const std::size_t point = text.find('.');
  const std::string fraction =
      point == std::string::npos ? std::string() : text.substr(point);
  return std::format("{}.{:04}{}.exr", stem, WholeFrame(*frame), fraction);
}

std::expected<void, Error> PrepareOutput(std::string_view file) {
  const std::filesystem::path path(file);
  if (!HasExrExtension(path)) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("cannot write '{}': umbra writes EXR files only "
                          "(.exr)",
                          file)));
  }
  const std::filesystem::path directory =
      path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error != std::error_code()) {
    return std::unexpected(CannotWrite(file, error.message()));
  }
  if (std::filesystem::is_directory(path, error)) {
    return std::unexpected(CannotWrite(file, "it is a directory"));
  }
  const bool exists = std::filesystem::exists(path, error);
  const auto writable =
      exists ? Accessible(path, W_OK) : Accessible(directory, W_OK | X_OK);
  if (!writable.has_value()) {
    return std::unexpected(CannotWrite(file, writable.error()));
  }
  return {};
}

}  // namespace umbra::ext::usd::internal
