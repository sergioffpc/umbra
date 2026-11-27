// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The refinement of a subdivision surface into triangles on its limit
// surface, through OpenSubdiv: each face of its control cage is tessellated,
// its edges cut until they look shorter than the dicing rate
// (docs/adr/0016-subdivision-surfaces-are-diced-in-the-sync.md).

#ifndef UMBRA_DELEGATE_SUBDIVISION_H_
#define UMBRA_DELEGATE_SUBDIVISION_H_

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "dicing.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal::subdivision {

// The subdivision scheme of a surface.
enum class Scheme : std::uint8_t {
  kBilinear,
  kCatmullClark,
  // Every face a triangle.
  kLoop,
};

// How the boundary of a surface is interpolated: UsdGeomMesh's
// interpolateBoundary.
enum class BoundaryInterpolation : std::uint8_t {
  kNone,
  kEdgeOnly,
  kEdgeAndCorner,
};

// Where face-varying values are interpolated linearly: UsdGeomMesh's
// faceVaryingLinearInterpolation.
enum class FaceVaryingInterpolation : std::uint8_t {
  kNone,
  kCornersOnly,
  kCornersPlus1,
  kCornersPlus2,
  kBoundaries,
  kAll,
};

// How the triangles of a Catmull-Clark surface are subdivided: UsdGeomMesh's
// triangleSubdivisionRule.
enum class TriangleSubdivision : std::uint8_t {
  kCatmullClark,
  kSmooth,
};

// How sharpness decreases across the subdivision levels of a crease:
// UsdGeomMesh's creaseMethod, as OpenSubdiv takes it.
enum class Creasing : std::uint8_t {
  kUniform,
  kChaikin,
};

// How the vertices of a face are ordered.
enum class Orientation : std::uint8_t {
  kRightHanded,
  kLeftHanded,
};

// A sharp edge of a cage, between two of its points.
struct Crease {
  std::uint32_t from = 0;
  std::uint32_t to = 0;
  float sharpness = 0.0F;
};

// A sharp point of a cage.
struct Corner {
  std::uint32_t point = 0;
  float sharpness = 0.0F;
};

// The control cage of a subdivision surface, in meters.
struct Cage {
  Scheme scheme = Scheme::kCatmullClark;
  BoundaryInterpolation boundary = BoundaryInterpolation::kEdgeAndCorner;
  FaceVaryingInterpolation face_varying =
      FaceVaryingInterpolation::kCornersPlus1;
  TriangleSubdivision triangles = TriangleSubdivision::kCatmullClark;
  Creasing creasing = Creasing::kUniform;
  Orientation orientation = Orientation::kRightHanded;
  // The number of points of each face, 3 or more.
  std::span<const std::uint32_t> face_vertex_counts;
  // The points of each face, in order: indices into `points`.
  std::span<const std::uint32_t> face_vertex_indices;
  // The faces that are holes.
  std::span<const std::uint32_t> holes;
  std::span<const Crease> creases;
  std::span<const Corner> corners;
  // Its points at the frame.
  std::span<const device::Float3> points;
  // Its points at each motion sample, each as many as `points`; none when
  // they do not move within the shutter.
  std::span<const std::span<const device::Float3>> point_motion;
};

// How values given per point vary between the points.
enum class Smoothness : std::uint8_t {
  // As the points do: UsdGeom's vertex interpolation.
  kSmooth,
  // Linearly: UsdGeom's varying interpolation.
  kLinear,
};

// Values given per point of the cage, `components` numbers each, 1 to 16.
struct PointValues {
  std::span<const float> values;
  std::uint32_t components = 1;
  Smoothness smoothness = Smoothness::kSmooth;
};

// Values given per face-vertex of the cage, `components` numbers each, 1 to
// 16, interpolated as the cage's face_varying says.
struct FaceVaryingValues {
  std::span<const float> values;
  std::uint32_t components = 1;
  // The value of each face-vertex: an index into `values`.
  std::span<const std::uint32_t> indices;
};

// What a surface is diced for: the image of the active camera, the
// viewpoint of its closest instance, and the dicing rate, in pixels.
struct Dicing {
  ImageScale scale;
  Viewpoint viewpoint;
  double rate = 1.0;
};

// What Refine refines, and for what.
struct Surface {
  Cage cage;
  std::span<const PointValues> point_values;
  std::span<const FaceVaryingValues> face_varying;
  Dicing dicing;
};

// A surface refined into triangles on its limit surface, wound
// counterclockwise.
//
// Its points are shared by the faces of the cage that meet there, so that
// it is watertight. Each face's tessellation also has corners of its own,
// which the values that may differ on either side of an edge are given at:
// its normals, across a crease, and its face-varying values.
struct Refinement {
  // The positions of its points, one array per motion sample: the frame's
  // alone without motion.
  std::vector<std::vector<device::Float3>> points;
  std::vector<device::Triangle> triangles;
  // The face of the cage each triangle comes from.
  std::vector<std::uint32_t> source_faces;
  // Three per triangle, in the triangle's order: the corner of each point,
  // an index into the values per corner.
  std::vector<std::uint32_t> corners;
  // The unit normal at each corner, three numbers each, at the frame.
  std::vector<float> normals;
  // The values of each PointValues, at each point.
  std::vector<std::vector<float>> point_values;
  // The values of each FaceVaryingValues, at each corner.
  std::vector<std::vector<float>> face_varying;
  // The longest edge of a triangle, in pixels, measured as the dicing rate
  // is: on the closest instance, at the frame.
  double longest_edge = 0.0;
};

// Refines `surface` into triangles: each face of its cage tessellated, but
// for its holes and the faces with no limit surface, each of its edges cut
// into the segments EdgeRate gives for its length in pixels along the limit
// surface, and cut further, for a face where any edge of a triangle is
// longer than the dicing rate, until none is or kMaxEdgeRate stops it.
// Fails with the reason when OpenSubdiv cannot refine the cage: a Loop
// cage with a face of more than three points, or a topology it rejects.
[[nodiscard]] std::expected<Refinement, std::string> Refine(
    const Surface& surface);

}  // namespace umbra::delegate::internal::subdivision

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_SUBDIVISION_H_
