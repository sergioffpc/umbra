// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "gf.h"

#include <pxr/base/gf/matrix4d.h>

#include <array>
#include <cstddef>
#include <span>

#include "host_copy.h"

namespace umbra::delegate::internal {
namespace {

// A transform in scene units, in meters: row-major, for row vectors, its
// translation (the last row) scaled by `meters_per_unit`.
host::Matrix TransformInMeters(const pxr::GfMatrix4d& transform,
                               double meters_per_unit) {
  host::Matrix converted = RowMajor(transform);
  // The translation, in the last row, is a length; the rest is not.
  for (std::size_t column = 0; column < 3; ++column) {
    converted.at(12 + column) *= meters_per_unit;
  }
  return converted;
}

}  // namespace

std::array<double, 16> RowMajor(const pxr::GfMatrix4d& transform) {
  return Components<double, 16>(transform);
}

pxr::GfMatrix4d GfMatrixOf(const host::Matrix& matrix) {
  return {
      matrix.at(0),  matrix.at(1),  matrix.at(2),  matrix.at(3),
      matrix.at(4),  matrix.at(5),  matrix.at(6),  matrix.at(7),
      matrix.at(8),  matrix.at(9),  matrix.at(10), matrix.at(11),
      matrix.at(12), matrix.at(13), matrix.at(14), matrix.at(15),
  };
}

host::HostBuffer<host::Matrix> TransformsInMeters(
    std::span<const pxr::GfMatrix4d> transforms, double meters_per_unit) {
  host::HostBuffer<host::Matrix> converted;
  converted.reserve(transforms.size());
  for (const pxr::GfMatrix4d& transform : transforms) {
    converted.push_back(TransformInMeters(transform, meters_per_unit));
  }
  return converted;
}

}  // namespace umbra::delegate::internal
