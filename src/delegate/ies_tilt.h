// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The TILT data of IES profiles (IESNA LM-63): how a lamp's output changes
// with the inclination at which its luminaire is mounted. A profile tells it
// on its TILT line: NONE when the output does not change, INCLUDE when the
// data follows that line, or the name of a file that holds it. The data is
// the lamp-to-luminaire geometry (1, 2 or 3), the count of tilt angles, the
// angles, in degrees, ascending within 0 and 180, and as many multiplying
// factors. Each instance of a light scales its emission by the tilt factor
// at its own inclination. It holds no OpenUSD type.

#ifndef UMBRA_DELEGATE_IES_TILT_H_
#define UMBRA_DELEGATE_IES_TILT_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// The TILT data of a profile, checked.
struct IesTilt {
  // The lamp-to-luminaire geometry: 1, 2 or 3. The tilt factor does not
  // depend on it.
  std::uint8_t geometry = 1;
  // The tilt angles, in degrees, strictly ascending within 0 and 180.
  std::vector<float> angles;
  // The multiplying factor at each angle, finite and not negative.
  std::vector<float> factors;

  friend bool operator==(const IesTilt&, const IesTilt&) = default;
};

// A profile whose lamp's output does not change with its inclination
// (TILT=NONE).
struct IesTiltNone {
  friend bool operator==(const IesTiltNone&, const IesTiltNone&) = default;
};

// A profile whose TILT data is in another file, by the name the profile
// gives it (TILT=<file>).
struct IesTiltFile {
  std::string name;

  friend bool operator==(const IesTiltFile&, const IesTiltFile&) = default;
};

// What a profile's TILT line says.
using IesTiltLine = std::variant<IesTiltNone, IesTilt, IesTiltFile>;

// What the TILT line of the profile `text` says, with its data when it
// includes it: TILT=INCLUDE, with nothing between the equals sign and
// INCLUDE. Values are separated by white space or commas, and the count is
// an integer. Fails with why when the profile has no TILT line or its
// included data is not valid.
[[nodiscard]] std::expected<IesTiltLine, std::string> ParseIesTiltLine(
    std::string_view text);

// The TILT data of the TILT file `text`, which holds that data alone.
// Fails with why when it is not valid.
[[nodiscard]] std::expected<IesTilt, std::string> ParseIesTiltFile(
    std::string_view text);

// The stage's up axis, the opposite of the direction of gravity.
enum class UpAxis : std::uint8_t { kY, kZ };

// The inclination, in degrees within 0 and 180, of a light placed by
// `light_to_world`: the angle between the nadir of its profile, its -Z axis,
// in world space, and the stage's down direction, the opposite of `up_axis`. 0
// when its -Z axis has no length.
[[nodiscard]] double IesInclination(const device::Matrix& light_to_world,
                                    UpAxis up_axis);

// The factor that `tilt` gives at `inclination`, in degrees: linear between
// the two tilt angles around it, and the factor of the first or the last
// angle before or beyond them. `tilt` holds one angle or more, and as many
// factors, as the parsers give it.
[[nodiscard]] float IesTiltFactor(const IesTilt& tilt, double inclination);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_IES_TILT_H_
