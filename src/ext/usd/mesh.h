// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A mesh as Hydra gives it, converted into its host copy: triangulated,
// wound one way, in meters, with its primvars at the rates devices take
// (docs/adr/0012-umbra-is-a-hydra-renderer.md).

#ifndef UMBRA_EXT_USD_MESH_H_
#define UMBRA_EXT_USD_MESH_H_

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/pxr.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "host_copy.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

namespace pxr = PXR_NS;

// A primvar of a mesh, as Hydra gives it.
struct SourcePrimvar {
  pxr::TfToken name;
  pxr::HdInterpolation interpolation = pxr::HdInterpolationConstant;
  pxr::VtValue value;
  // The index of the value of each element, or none when it is not indexed.
  pxr::VtIntArray indices;
  // The number of values that make up each element.
  int element_size = 1;
};

// A mesh, as Hydra gives it: its topology, its points and its primvars, the
// normals among them when authored.
struct SourceMesh {
  std::string path;
  pxr::HdMeshTopology topology;
  // Its points at the frame.
  pxr::VtValue points;
  // The motion samples of its points, each as long as `points`, when they
  // move within the shutter; none otherwise.
  std::vector<pxr::VtVec3fArray> point_motion;
  std::vector<SourcePrimvar> primvars;
  host::Sidedness sidedness = host::Sidedness::kSingleSided;
};

// Whether a mesh's topology asks for subdivision: any subdivisionScheme but
// none.
[[nodiscard]] bool IsSubdivided(const pxr::HdMeshTopology& topology);

// Where the normals of a mesh's host copy come from.
enum class NormalsSource : std::uint8_t {
  // They are its authored normals.
  kAuthored,
  // They are computed from its points: they change with them.
  kComputed,
};

// A mesh's host copy, and where its normals come from.
struct ConvertedMesh {
  host::Mesh mesh;
  NormalsSource normals = NormalsSource::kAuthored;
};

// Converts `source` into its host copy, with its lengths scaled by
// `meters_per_unit`: its polygons fanned into triangles, but for its holes,
// wound counterclockwise (leftHanded ones flipped); its points one array per
// motion sample; its numeric primvars at the rates devices take, the others
// dropped with a debug message. Authored normals are kept unless the mesh
// asks for subdivision; smooth ones are computed otherwise, from its points
// at the frame. Fails with the reason the mesh is invalid: points
// that are not point3f[], a face of fewer than three vertices, counts that
// do not add up to its indices, an index out of range, or a primvar whose
// size does not match its interpolation.
[[nodiscard]] std::expected<ConvertedMesh, std::string> ConvertMesh(
    const SourceMesh& source, double meters_per_unit);

// The points of a mesh, with their lengths scaled by `meters_per_unit`, one
// array per motion sample: those of `motion`, each as long as `points`,
// when they move within the shutter, and `points`, at the frame, otherwise.
// Fails when `points` are not point3f[].
[[nodiscard]] std::expected<std::vector<host::HostBuffer<host::Float3>>,
                            std::string>
ConvertPoints(const pxr::VtValue& points,
              std::span<const pxr::VtVec3fArray> motion,
              double meters_per_unit);

// The elements of `value`, a numeric value or array, as a constant primvar
// named `name` holding them all, one after another; none for a value of
// another type.
[[nodiscard]] std::optional<host::Primvar> ConstantPrimvar(
    std::string name, const pxr::VtValue& value);

// A transform with no unit, row-major, for row vectors.
[[nodiscard]] std::array<double, 16> RowMajor(const pxr::GfMatrix4d& transform);

// The motion samples of a transform in scene units, each in meters:
// row-major, for row vectors, its translation (the last row) scaled by
// `meters_per_unit`.
[[nodiscard]] host::HostBuffer<host::Matrix> TransformsInMeters(
    std::span<const pxr::GfMatrix4d> transforms, double meters_per_unit);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_MESH_H_
