// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The warnings of the commits: each reports the prims of one kind at fault,
// by their count and the first, and is logged once.

#ifndef UMBRA_DELEGATE_COMMIT_WARNINGS_H_
#define UMBRA_DELEGATE_COMMIT_WARNINGS_H_

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <set>
#include <string>
#include <string_view>

#include "light.h"
#include "motion_samples.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// The warnings of the commits, each logged once: a commit that finds the
// same prims at fault as an earlier one does not log them again.
class Warnings {
 public:
  explicit Warnings(std::set<std::string>& logged UMBRA_LIFETIMEBOUND)
      : logged_(&logged) {}

  // Logs `message`, unless a commit logged it before.
  void Warn(std::string message);

 private:
  std::set<std::string>* logged_;
};

// The prims of one kind a warning reports: their count and the first, with
// a detail of the first.
class Tally {
 public:
  void Add(const pxr::SdfPath& path, std::string_view detail = {});

  [[nodiscard]] std::size_t count() const { return count_; }
  // "/World/Car/Body" for one, "first /World/Car/Body" for more.
  [[nodiscard]] std::string First() const;
  // The path of the first: "/World/Car/Body".
  [[nodiscard]] const std::string& first_path() const UMBRA_LIFETIMEBOUND {
    return first_;
  }
  [[nodiscard]] const std::string& detail() const UMBRA_LIFETIMEBOUND {
    return detail_;
  }

 private:
  std::size_t count_ = 0;
  std::string first_;
  std::string detail_;
};

// The prims whose motion sampling changed, for the warnings that report
// them.
class MotionTallies {
 public:
  void Add(const pxr::SdfPath& path, MotionOutcome outcome);

  // "warning: usd: 3 prims change topology within the shutter, rendered
  // without motion blur (first /World/fluid)", and "warning: usd: 2 prims
  // have more than 16 motion samples over the shutter, resampled down to
  // 16 (first /World/sim)".
  void Report(Warnings& warnings) const;

 private:
  Tally changing_;
  Tally resampled_;
};

// "warning: usd: 2 lights have light filters, which umbra does not take
// yet; ignored (first /World/key)", and so on; with the `prefix` "mesh ",
// of mesh lights.
void ReportLightIssue(LightIssue issue, const Tally& tally, Warnings& warnings,
                      std::string_view prefix = {});

// "warning: usd: 2 Volume prims not supported yet; left out (first
// /World/smoke)": the prims of `type`.
void ReportUnsupported(std::string_view type, const Tally& tally,
                       Warnings& warnings);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_WARNINGS_H_
