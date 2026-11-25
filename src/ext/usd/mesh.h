// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A mesh as Hydra gives it, converted into the Scene's: triangulated, wound
// one way, in meters, with its primvars at the Scene's rates
// (docs/adr/0006-scenes-enter-through-hydra.md).

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
#include <expected>
#include <string>
#include <vector>

#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

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
};

// A mesh, as Hydra gives it: its topology, its points and its primvars, the
// normals among them when authored.
struct SourceMesh {
  std::string path;
  pxr::HdMeshTopology topology;
  pxr::VtValue points;
  std::vector<SourcePrimvar> primvars;
  scene::Sidedness sidedness = scene::Sidedness::kSingleSided;
};

// Whether a mesh's topology asks for subdivision: any subdivisionScheme but
// none.
[[nodiscard]] bool IsSubdivided(const pxr::HdMeshTopology& topology);

// Converts `source` into the Scene's mesh, with its lengths scaled by
// `meters_per_unit`: its polygons fanned into triangles, but for its holes,
// wound counterclockwise (leftHanded ones flipped); its numeric primvars at
// the Scene's rates, the others dropped with a debug message. Authored
// normals are kept unless the mesh asks for subdivision; smooth ones are
// computed otherwise. Fails with the reason the mesh is invalid: points
// that are not point3f[], a face of fewer than three vertices, counts that
// do not add up to its indices, an index out of range, or a primvar whose
// size does not match its interpolation.
[[nodiscard]] std::expected<scene::Mesh, std::string> ConvertMesh(
    const SourceMesh& source, double meters_per_unit);

// A transform in scene units, in meters: row-major, for row vectors, its
// translation (the last row) scaled by `meters_per_unit`.
[[nodiscard]] std::array<double, 16> TransformInMeters(
    const pxr::GfMatrix4d& transform, double meters_per_unit);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_MESH_H_
