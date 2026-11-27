// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "ies_tilt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {
namespace {

using Failure = std::unexpected<std::string>;

// The characters that separate the values of a profile.
constexpr std::string_view kSeparators = " \t\r\n\f\v,";
// The white space around the value of a profile's TILT line.
constexpr std::string_view kWhiteSpace = " \t\r\n\f\v";

// The most values reserved ahead for a count, whatever the count.
constexpr std::size_t kMaxReserved = 4096;

// `text` without the white space around it.
std::string_view Trimmed(std::string_view text) {
  const std::size_t first = text.find_first_not_of(kWhiteSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(kWhiteSpace);
  return text.substr(first, last - first + 1);
}

// Reads the values of TILT data one after another.
class Values {
 public:
  explicit Values(std::string_view text) : rest_(text) {}

  // The next value, of type T, none when there is none left, or why it is
  // not a number of that type, which names it as `what`.
  template <typename T>
  std::expected<std::optional<T>, std::string> Next(
      std::string_view what = "a TILT value") {
    const std::string_view token = NextToken();
    if (token.empty()) {
      return std::nullopt;
    }
    // from_chars takes no plus sign.
    const std::string_view digits =
        token.starts_with('+') ? token.substr(1) : token;
    T value{};
    const char* last = std::to_address(digits.end());
    const auto [end, error] =
        std::from_chars(std::to_address(digits.begin()), last, value);
    if (error != std::errc() || end != last) {
      return Failure(
          std::format("{}, '{}', is not {}", what, token,
                      std::is_integral_v<T> ? "an integer" : "a number"));
    }
    return value;
  }

  // Whether no value is left.
  [[nodiscard]] bool IsAtEnd() const {
    return rest_.find_first_not_of(kSeparators) == std::string_view::npos;
  }

 private:
  std::string_view NextToken() {
    const std::size_t first = rest_.find_first_not_of(kSeparators);
    if (first == std::string_view::npos) {
      rest_ = {};
      return {};
    }
    rest_.remove_prefix(first);
    const std::size_t last =
        std::min(rest_.find_first_of(kSeparators), rest_.size());
    const std::string_view token = rest_.substr(0, last);
    rest_.remove_prefix(last);
    return token;
  }

  std::string_view rest_;
};

// Whether `value` is an integer within `low` and `high`.
bool IsIntegerWithin(double value, double low, double high) {
  return value >= low && value <= high && std::trunc(value) == value;
}

// The next `count` values of `reader`.
std::expected<std::vector<float>, std::string> ReadValues(Values& reader,
                                                          std::size_t count) {
  std::vector<float> values;
  values.reserve(std::min(count, kMaxReserved));
  for (std::size_t index = 0; index < count; ++index) {
    auto value = reader.Next<double>();
    if (!value.has_value()) {
      return Failure(std::move(value.error()));
    }
    if (!value->has_value()) {
      return Failure(std::format(
          "the TILT count, {}, is more than the values that follow it", count));
    }
    values.push_back(static_cast<float>(**value));
  }
  return values;
}

// Checks the angles and factors of `tilt`.
std::expected<void, std::string> CheckTable(const IesTilt& tilt) {
  const bool within = std::ranges::all_of(tilt.angles, [](float angle) {
    return std::isfinite(angle) && angle >= 0.0F && angle <= 180.0F;
  });
  if (!within ||
      std::ranges::adjacent_find(tilt.angles, std::ranges::greater_equal{}) !=
          tilt.angles.end()) {
    return Failure("the tilt angles do not ascend within 0 and 180");
  }
  if (!std::ranges::all_of(tilt.factors, [](float factor) {
        return std::isfinite(factor) && factor >= 0.0F;
      })) {
    return Failure("the tilt factors are negative or not finite");
  }
  return {};
}

// Whether values may follow the TILT data: those of the profile that
// includes it.
enum class Rest : std::uint8_t { kPhotometry, kNothing };

// The lamp-to-luminaire geometry and the count of tilt angles that start
// TILT data.
struct Header {
  std::uint8_t geometry = 1;
  std::size_t count = 0;
};

// The geometry and the count that `reader` starts with, checked.
std::expected<Header, std::string> ReadHeader(Values& reader) {
  auto geometry = reader.Next<double>();
  if (!geometry.has_value()) {
    return Failure(std::move(geometry.error()));
  }
  if (!geometry->has_value()) {
    return Failure("the TILT data is empty");
  }
  if (!IsIntegerWithin(**geometry, 1.0, 3.0)) {
    return Failure(std::format(
        "the lamp-to-luminaire geometry, {}, is not 1, 2 or 3", **geometry));
  }
  // pxrIES reads the count as an integer. It is read unsigned, so that a
  // sign makes it fail.
  auto count = reader.Next<std::uint64_t>("the TILT count");
  if (!count.has_value()) {
    return Failure(std::move(count.error()));
  }
  if (!count->has_value() || **count == 0) {
    return Failure("the TILT count is missing or 0");
  }
  return Header{
      .geometry = static_cast<std::uint8_t>(**geometry),
      .count = static_cast<std::size_t>(**count),
  };
}

// The TILT data at the start of `text`, checked.
std::expected<IesTilt, std::string> ParseData(std::string_view text,
                                              Rest rest) {
  Values reader(text);
  const auto header = ReadHeader(reader);
  if (!header.has_value()) {
    return Failure(header.error());
  }
  auto angles = ReadValues(reader, header->count);
  if (!angles.has_value()) {
    return Failure(std::move(angles.error()));
  }
  auto factors = ReadValues(reader, header->count);
  if (!factors.has_value()) {
    return Failure(std::move(factors.error()));
  }
  if (rest == Rest::kNothing && !reader.IsAtEnd()) {
    return Failure(std::format(
        "the TILT count, {}, is less than the values that follow it",
        header->count));
  }
  IesTilt tilt{
      .geometry = header->geometry,
      .angles = std::move(*angles),
      .factors = std::move(*factors),
  };
  if (auto checked = CheckTable(tilt); !checked.has_value()) {
    return Failure(std::move(checked.error()));
  }
  return tilt;
}

}  // namespace

std::expected<IesTiltLine, std::string> ParseIesTiltLine(
    std::string_view text) {
  constexpr std::string_view kTilt = "\nTILT=";
  const std::size_t found = text.find(kTilt);
  if (found == std::string_view::npos) {
    return Failure("it has no TILT line");
  }
  const std::string_view rest = text.substr(found + kTilt.size());
  // pxrIES reads the photometry after included data only when INCLUDE
  // follows the equals sign at once, and right after it.
  constexpr std::string_view kInclude = "INCLUDE";
  if (rest.starts_with(kInclude)) {
    return ParseData(rest.substr(kInclude.size()), Rest::kPhotometry);
  }
  const std::string_view value =
      Trimmed(rest.substr(0, std::min(rest.find('\n'), rest.size())));
  if (value == "NONE") {
    return IesTiltNone{};
  }
  if (value.empty()) {
    return Failure("the TILT line names no file");
  }
  return IesTiltFile{.name = std::string(value)};
}

std::expected<IesTilt, std::string> ParseIesTiltFile(std::string_view text) {
  return ParseData(text, Rest::kNothing);
}

double IesInclination(const device::Matrix& light_to_world, UpAxis up_axis) {
  // For row vectors, -Z turns to the opposite of the matrix's third row.
  const std::array<double, 3> nadir = {
      -light_to_world.at(8),
      -light_to_world.at(9),
      -light_to_world.at(10),
  };
  const double length = std::hypot(nadir.at(0), nadir.at(1), nadir.at(2));
  if (!(length > 0.0)) {
    return 0.0;
  }
  // The nadir's component along the down direction.
  const double down = -(up_axis == UpAxis::kY ? nadir.at(1) : nadir.at(2));
  const double cosine = std::clamp(down / length, -1.0, 1.0);
  return std::acos(cosine) * 180.0 / std::numbers::pi;
}

float IesTiltFactor(const IesTilt& tilt, double inclination) {
  const auto after = std::ranges::upper_bound(
      tilt.angles, inclination, std::ranges::less{},
      [](float angle) { return static_cast<double>(angle); });
  const auto next =
      static_cast<std::size_t>(std::distance(tilt.angles.begin(), after));
  if (next == 0) {
    return tilt.factors.front();
  }
  if (next == tilt.angles.size()) {
    return tilt.factors.back();
  }
  const auto start = static_cast<double>(tilt.angles.at(next - 1));
  const auto end = static_cast<double>(tilt.angles.at(next));
  const double weight = (inclination - start) / (end - start);
  return static_cast<float>(
      std::lerp(static_cast<double>(tilt.factors.at(next - 1)),
                static_cast<double>(tilt.factors.at(next)), weight));
}

}  // namespace umbra::delegate::internal
