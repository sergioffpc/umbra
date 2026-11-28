// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A mesh as Hydra gives it, converted into its host copy: triangulated,
// wound one way, in meters, with its primvars at the rates devices take
// (docs/adr/0012-umbra-is-a-hydra-renderer.md).

#ifndef UMBRA_DELEGATE_MESH_H_
#define UMBRA_DELEGATE_MESH_H_

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dicing.h"
#include "host_copy.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

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
  // Its creases, corners and interpolation rules, for subdivision.
  pxr::PxOsdSubdivTags subdiv_tags;
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
  // The longest edge of its triangles, in pixels, measured as the dicing
  // rate is, when it is subdivided; 0 otherwise.
  double longest_edge = 0.0;
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

// Converts `source`, a mesh that asks for subdivision, into its host copy,
// with its lengths scaled by `meters_per_unit`: its limit surface, refined
// through OpenSubdiv into triangles whose edges look shorter than the dicing
// rate of `view` on the closest of `instances`, with its creases, corners,
// holes and interpolation rules (subdivision::Refine). Its points are one
// array per motion sample; its normals, at each corner of a face's
// tessellation, are the limit surface's, at the frame; its vertex and
// varying primvars are refined as its points are, smoothly and linearly,
// its face-varying ones as faceVaryingLinearInterpolation says, and its
// uniform and constant ones kept, per source face and whole. Its authored
// normals are ignored, and its primvars of integers but uniform and
// constant ones dropped with a debug message. Fails as ConvertMesh does,
// and with the reason OpenSubdiv cannot refine it.
[[nodiscard]] std::expected<ConvertedMesh, std::string> RefineMesh(
    const SourceMesh& source, double meters_per_unit, const DicingView& view,
    std::span<const host::Instance> instances);

// The points of a mesh, with their lengths scaled by `meters_per_unit`, one
// array per motion sample: those of `motion`, each as long as `points`,
// when they move within the shutter, and `points`, at the frame, otherwise.
// Fails when `points` are not point3f[].
[[nodiscard]] std::expected<std::vector<host::HostBuffer<host::Float3>>,
                            std::string>
ConvertPoints(const pxr::VtValue& points,
              std::span<const pxr::VtVec3fArray> motion,
              double meters_per_unit);

// How ReadPrimvars gives an indexed primvar.
enum class PrimvarIndices : std::uint8_t {
  // Its values, and its indices.
  kKept,
  // Its values flattened through its indices, as Hydra's Get gives them.
  kFlattened,
};

// The primvars of the prim at `prim_id` of each of `interpolations`, in
// their order, indexed ones as `indices` says.
[[nodiscard]] std::vector<SourcePrimvar> ReadPrimvars(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id,
    std::span<const pxr::HdInterpolation> interpolations,
    PrimvarIndices indices);

// The elements of the value of `source`, a primvar of the prim at `path`
// whose value is a numeric value or array, as a constant primvar holding
// them all, one after another; none, with a debug message, for a value of
// another type.
[[nodiscard]] std::optional<host::Primvar> ConstantPrimvar(
    const SourcePrimvar& source, std::string_view path);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_MESH_H_
