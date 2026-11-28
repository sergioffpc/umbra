// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The vectors and matrices of Gf, OpenUSD's math library, as the numbers
// umbra holds them in: their components, and transforms row-major, in
// meters.

#ifndef UMBRA_DELEGATE_GF_H_
#define UMBRA_DELEGATE_GF_H_

#include <pxr/base/gf/matrix4d.h>
#include <pxr/pxr.h>

#include <array>
#include <cstddef>
#include <span>

#include "host_copy.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The components of a Gf vector of 2, 3 or 4 (GfVec2i, GfVec3f, GfVec4d,
// ...), as T, read from its named fields.
template <typename T, std::size_t N, typename Vector>
  requires(Vector::dimension == N)
std::array<T, N> Components(const Vector& vector) {
  if constexpr (N == 2) {
    return {static_cast<T>(vector.x), static_cast<T>(vector.y)};
  } else if constexpr (N == 3) {
    return {
        static_cast<T>(vector.x),
        static_cast<T>(vector.y),
        static_cast<T>(vector.z),
    };
  } else {
    static_assert(N == 4);
    return {
        static_cast<T>(vector.x),
        static_cast<T>(vector.y),
        static_cast<T>(vector.z),
        static_cast<T>(vector.w),
    };
  }
}

// The components of a square Gf matrix (GfMatrix3f, GfMatrix4d, ...), as
// T, row-major.
template <typename T, std::size_t N, typename Matrix>
  requires(Matrix::numRows * Matrix::numColumns == N)
std::array<T, N> Components(const Matrix& matrix) {
  constexpr std::size_t kRows = Matrix::numRows;
  std::array<T, N> components{};
  for (std::size_t row = 0; row < kRows; ++row) {
    const std::array<T, kRows> values =
        Components<T, kRows>(matrix.GetRow(static_cast<int>(row)));
    for (std::size_t column = 0; column < kRows; ++column) {
      components.at((row * kRows) + column) = values.at(column);
    }
  }
  return components;
}

// A transform with no unit, row-major, for row vectors.
[[nodiscard]] std::array<double, 16> RowMajor(const pxr::GfMatrix4d& transform);

// `matrix`, row-major, as Gf holds it: RowMajor undone.
[[nodiscard]] pxr::GfMatrix4d GfMatrixOf(const host::Matrix& matrix);

// The motion samples of a transform in scene units, each in meters:
// row-major, for row vectors, its translation (the last row) scaled by
// `meters_per_unit`.
[[nodiscard]] host::HostBuffer<host::Matrix> TransformsInMeters(
    std::span<const pxr::GfMatrix4d> transforms, double meters_per_unit);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_GF_H_
