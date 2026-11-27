// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "ies_profile.h"

#include <pxr/imaging/plugin/hdEmbree/pxrIES/ies.h>
#include <pxr/imaging/plugin/hdEmbree/pxrIES/pxrIES.h>
#include <pxr/pxr.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolvedPath.h>
#include <pxr/usd/ar/resolver.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "host_copy.h"
#include "ies_tilt.h"

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

namespace {

using Failure = std::unexpected<std::string>;

// How far outside its range an angle may be, in radians: pxrIES's tolerance.
constexpr float kAngleTolerance = 1e-4F;

// The text of the asset at `resolved`, or none when it cannot be opened.
std::optional<std::string> ReadAsset(const std::string& resolved) {
  const std::shared_ptr<pxr::ArAsset> asset =
      pxr::ArGetResolver().OpenAsset(pxr::ArResolvedPath(resolved));
  if (asset == nullptr) {
    return std::nullopt;
  }
  std::string text(asset->GetSize(), '\0');
  if (asset->Read(text.data(), text.size(), 0) != text.size()) {
    return std::nullopt;
  }
  return text;
}

// Whether `angles`, in radians, are finite, do not descend, and are within
// 0 and `limit`.
bool IsAscendingWithin(std::span<const float> angles, float limit) {
  return !angles.empty() && std::ranges::all_of(angles, [&](float angle) {
    return std::isfinite(angle) && angle >= -kAngleTolerance &&
           angle <= limit + kAngleTolerance;
  }) && std::ranges::is_sorted(angles);
}

// Whether every one of `candelas` is finite and not negative.
bool IsIntensityTable(std::span<const float> candelas) {
  return std::ranges::all_of(candelas, [](float candela) {
    return std::isfinite(candela) && candela >= 0.0F;
  });
}

// A profile as pxrIES reads it. It loads as PxrIESFile::load does, except
// for type A photometry, which pxrIES's conversion (process_type_a) reads
// past the end of its angles for.
class Profile final : public pxr::PxrIESFile {
 public:
  // Parses `text` and converts it to type C, its symmetries unfolded, with
  // its power.
  std::expected<void, std::string> Load(const std::string& text) {
    clear();
    if (!parse(text)) {
      return Failure("it is not an IES (LM-63) profile");
    }
    if (type == TYPE_A) {
      return Failure("its photometry is of type A, which umbra does not read");
    }
    if (!process()) {
      return Failure("it has no angles");
    }
    pxr_extra_process();
    return {};
  }

  // Its table, once loaded, checked.
  [[nodiscard]] std::expected<host::IesProfile, std::string> Table() const {
    if (!IsAscendingWithin(v_angles, std::numbers::pi_v<float>)) {
      return Failure("its vertical angles do not ascend within 0 and 180");
    }
    if (!IsAscendingWithin(h_angles, 2.0F * std::numbers::pi_v<float>)) {
      return Failure("its horizontal angles do not ascend within 0 and 360");
    }
    host::IesProfile profile{
        .vertical_angles = v_angles,
        .horizontal_angles = h_angles,
        .candelas = {},
        .power = power(),
        .normalization = host::Normalization::kUnnormalized,
        .tilt = std::nullopt,
    };
    profile.candelas.reserve(v_angles.size() * h_angles.size());
    for (const std::vector<float>& row : intensity) {
      profile.candelas.insert(profile.candelas.end(), row.begin(), row.end());
    }
    if (profile.candelas.size() != v_angles.size() * h_angles.size() ||
        !IsIntensityTable(profile.candelas)) {
      return Failure("its intensities are missing, negative or not finite");
    }
    if (!std::isfinite(profile.power) || profile.power <= 0.0F) {
      return Failure("it emits nothing");
    }
    return profile;
  }
};

// The TILT data of the TILT file `name`, which Ar resolves next to the
// profile at `resolved`.
std::expected<IesTilt, std::string> ReadTiltFile(const std::string& name,
                                                 const std::string& resolved) {
  const pxr::ArResolver& resolver = pxr::ArGetResolver();
  const pxr::ArResolvedPath file = resolver.Resolve(
      resolver.CreateIdentifier(name, pxr::ArResolvedPath(resolved)));
  if (file.empty()) {
    return Failure(std::format("its TILT file {} does not resolve", name));
  }
  const std::optional<std::string> text = ReadAsset(file.GetPathString());
  if (!text.has_value()) {
    return Failure(std::format("its TILT file {} cannot be opened", name));
  }
  auto tilt = ParseIesTiltFile(*text);
  if (!tilt.has_value()) {
    return Failure(std::format("its TILT file {}: {}", name, tilt.error()));
  }
  return tilt;
}

// The TILT data of the profile `text`, which Ar resolved to `resolved`:
// none for TILT=NONE.
std::expected<std::optional<IesTilt>, std::string> ReadTilt(
    const std::string& text, const std::string& resolved) {
  auto line = ParseIesTiltLine(text);
  if (!line.has_value()) {
    return Failure(std::move(line.error()));
  }
  if (auto* included = std::get_if<IesTilt>(&*line)) {
    return std::move(*included);
  }
  if (const auto* file = std::get_if<IesTiltFile>(&*line)) {
    return ReadTiltFile(file->name, resolved);
  }
  return std::nullopt;
}

// Reads the TILT data of the profile `text`, which Ar resolved to
// `resolved`, into `tilt`; why it is not read, or empty.
std::string ReadTiltInto(const std::string& text, const std::string& resolved,
                         std::optional<IesTilt>& tilt) {
  try {
    auto read = ReadTilt(text, resolved);
    if (!read.has_value()) {
      return std::move(read.error());
    }
    tilt = std::move(*read);
    return {};
  } catch (const std::exception& error) {
    return std::format("its TILT data cannot be read ({})", error.what());
  }
}

}  // namespace

std::expected<ReadProfile, std::string> ReadIesProfile(
    const std::string& resolved) {
  try {
    const std::optional<std::string> text = ReadAsset(resolved);
    if (!text.has_value()) {
      return Failure("it cannot be opened");
    }
    Profile profile;
    if (auto loaded = profile.Load(*text); !loaded.has_value()) {
      return Failure(std::move(loaded.error()));
    }
    auto table = profile.Table();
    if (!table.has_value()) {
      return Failure(std::move(table.error()));
    }
    ReadProfile read{.profile = std::move(*table), .tilt_not_read = {}};
    read.tilt_not_read = ReadTiltInto(*text, resolved, read.profile.tilt);
    return read;
  } catch (const std::exception& error) {
    // pxrIES sizes its arrays by the counts the file gives, which may be
    // negative or too large.
    return Failure(std::format("pxrIES cannot read it ({})", error.what()));
  }
}

}  // namespace umbra::delegate::internal
