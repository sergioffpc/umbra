// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_warnings.h"

#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <set>
#include <string>
#include <string_view>

#include "light.h"
#include "motion_samples.h"
#include "plural.h"
#include "umbra/core/log.h"

namespace umbra::delegate::internal {
namespace {

// The words of the warning about one LightIssue: "{count} {subject}
// {predicate} ({first}{: detail})", each for one light and for more.
struct IssueWords {
  LightIssue issue;
  std::array<std::string_view, 2> subject;
  std::array<std::string_view, 2> predicate;
};

constexpr std::array<IssueWords, 11> kIssueWords = {
    IssueWords{
        .issue = LightIssue::kFilters,
        .subject = {"light has", "lights have"},
        .predicate =
            {
                "light filters, which umbra does not take yet; ignored",
                "light filters, which umbra does not take yet; ignored",
            },
    },
    IssueWords{
        .issue = LightIssue::kIesProfileNotRead,
        .subject = {"light's IES profile is", "lights' IES profiles are"},
        .predicate =
            {
                "not read, rendered without it",
                "not read, rendered without them",
            },
    },
    IssueWords{
        .issue = LightIssue::kIesTiltNotRead,
        .subject =
            {
                "light's IES profile has TILT data that is",
                "lights' IES profiles have TILT data that is",
            },
        .predicate =
            {
                "not read, rendered without it",
                "not read, rendered without it",
            },
    },
    IssueWords{
        .issue = LightIssue::kIesAngleScale,
        .subject = {"light rescales", "lights rescale"},
        .predicate =
            {
                "the angles of its IES profile (shaping:ies:angleScale), "
                "which umbra does not: lights are physical",
                "the angles of their IES profiles (shaping:ies:angleScale), "
                "which umbra does not: lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kNonPositiveTemperature,
        .subject = {"light enables", "lights enable"},
        .predicate =
            {
                "a color temperature not above 0 K; its color is used alone",
                "a color temperature not above 0 K; their color is used alone",
            },
    },
    IssueWords{
        .issue = LightIssue::kDiffuseSpecular,
        .subject = {"light scales", "lights scale"},
        .predicate =
            {
                "its diffuse or specular contribution, which umbra does not: "
                "lights are physical",
                "their diffuse or specular contribution, which umbra does not: "
                "lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kFocus,
        .subject = {"light focuses", "lights focus"},
        .predicate =
            {
                "its emission (shaping:focus), which umbra does not: lights "
                "are physical",
                "their emission (shaping:focus), which umbra does not: lights "
                "are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kConeSoftness,
        .subject = {"light softens", "lights soften"},
        .predicate =
            {
                "the edge of its cone, which umbra does not: lights are "
                "physical",
                "the edge of their cone, which umbra does not: lights are "
                "physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kShadowControls,
        .subject = {"light authors", "lights author"},
        .predicate =
            {
                "shadow controls (ShadowAPI), which umbra does not take: "
                "lights are physical",
                "shadow controls (ShadowAPI), which umbra does not take: "
                "lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kColorNotConverted,
        .subject = {"light's color does", "lights' colors do"},
        .predicate =
            {
                "not convert into ACEScg, rendered white",
                "not convert into ACEScg, rendered white",
            },
    },
    IssueWords{
        .issue = LightIssue::kTextureNotRead,
        .subject = {"light texture is", "light textures are"},
        .predicate =
            {
                "not read, rendered with its light's color",
                "not read, rendered with their light's color",
            },
    },
};

}  // namespace

void Warnings::Warn(std::string message) {
  if (logged_->insert(message).second) {
    UMBRA_LOG_WARNING(kUsd, "{}", message);
  }
}

void Tally::Add(const pxr::SdfPath& path, std::string_view detail) {
  if (count_ == 0) {
    first_ = path.GetString();
    detail_ = detail;
  }
  ++count_;
}

std::string Tally::First() const {
  return count_ == 1 ? first_ : "first " + first_;
}

void MotionTallies::Add(const pxr::SdfPath& path, MotionOutcome outcome) {
  switch (outcome) {
    case MotionOutcome::kSampled:
      break;
    case MotionOutcome::kResampledDown:
      resampled_.Add(path);
      break;
    case MotionOutcome::kTopologyChanges:
      changing_.Add(path);
      break;
  }
}

void MotionTallies::Report(Warnings& warnings) const {
  if (changing_.count() > 0) {
    warnings.Warn(std::format(
        "{} {} topology within the shutter, rendered without motion blur "
        "({})",
        changing_.count(),
        Plural(changing_.count(), "prim changes", "prims change"),
        changing_.First()));
  }
  if (resampled_.count() > 0) {
    warnings.Warn(
        std::format("{} {} more than {} motion samples over the "
                    "shutter, resampled down to {} ({})",
                    resampled_.count(),
                    Plural(resampled_.count(), "prim has", "prims have"),
                    kMaxMotionSamples, kMaxMotionSamples, resampled_.First()));
  }
}

void ReportLightIssue(LightIssue issue, const Tally& tally, Warnings& warnings,
                      std::string_view prefix) {
  const auto* words = std::ranges::find(kIssueWords, issue, &IssueWords::issue);
  if (words == kIssueWords.end()) {
    return;
  }
  const std::size_t form = tally.count() == 1 ? 0 : 1;
  warnings.Warn(std::format("{} {}{} {} ({}{}{})", tally.count(), prefix,
                            words->subject.at(form), words->predicate.at(form),
                            tally.First(), tally.detail().empty() ? "" : ": ",
                            tally.detail()));
}

void ReportUnsupported(std::string_view type, const Tally& tally,
                       Warnings& warnings) {
  warnings.Warn(
      std::format("{} {} {} not supported yet; left out ({})", tally.count(),
                  type, Plural(tally.count(), "prim", "prims"), tally.First()));
}

}  // namespace umbra::delegate::internal
