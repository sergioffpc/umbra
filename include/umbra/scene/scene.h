// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The Scene: what umbra renders, in umbra's own types, with no OpenUSD type
// (docs/adr/0006-scenes-enter-through-hydra.md). The OpenUSD bridge builds it
// (umbra::ext::usd::LoadScene) and it is immutable once built. Lengths are in
// meters. A mesh is a prototype, held once, that instances place.

#ifndef UMBRA_SCENE_SCENE_H_
#define UMBRA_SCENE_SCENE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::scene {

// How a camera projects the scene onto its aperture.
enum class Projection : std::uint8_t {
  kPerspective,
  kOrthographic,
};

// A camera, as the stage's Camera prim describes it, in meters.
struct Camera {
  // The path of its prim ("/World/cam").
  std::string path;
  Projection projection = Projection::kPerspective;
  // The camera-to-world transform, row-major, for row vectors: the
  // translation is the last row. The camera looks down its -Z axis, +Y up.
  std::array<double, 16> camera_to_world{};
  double focal_length = 0.0;
  // The aperture, horizontal then vertical, and its offset from the axis.
  std::array<double, 2> aperture{};
  std::array<double, 2> aperture_offset{};
  // The near and far clipping distances.
  std::array<double, 2> clipping_range{};
  // The f-number of the lens; 0 for a pinhole, with no depth of field.
  double f_stop = 0.0;
  // The distance in focus, with depth of field.
  double focus_distance = 0.0;
  // The shutter: when it opens and closes, in frames relative to the frame.
  std::array<double, 2> shutter{};
};

// The quantity a render var holds per pixel.
enum class RenderVarSource : std::uint8_t {
  // The radiance, RGBA.
  kColor,
  // The distance from the camera to the first hit, in meters.
  kDepth,
  // The world-space shading normal of the first hit.
  kNormal,
  // The diffuse reflectance of the first hit.
  kAlbedo,
};

// The type of the channels of a render var in its file.
enum class ChannelType : std::uint8_t {
  kHalf,
  kFloat,
};

// One quantity per pixel a render product holds: a layer of its file, named
// after the var ("color" gives color.R, color.G, ...).
struct RenderVar {
  std::string name;
  RenderVarSource source = RenderVarSource::kColor;
  ChannelType type = ChannelType::kHalf;
};

// How the aspect ratio of the aperture is conformed to that of the image
// when they differ, as UsdRender defines it.
enum class AspectRatioConformPolicy : std::uint8_t {
  kExpandAperture,
  kCropAperture,
  kAdjustApertureWidth,
  kAdjustApertureHeight,
  kAdjustPixelAspectRatio,
};

// Whether a render product integrates the shutter, or the frame's instant.
enum class MotionBlur : std::uint8_t {
  kEnabled,
  kDisabled,
};

// Whether a render product renders the lens's depth of field, or a pinhole.
enum class DepthOfField : std::uint8_t {
  kEnabled,
  kDisabled,
};

// An image file the render writes, an EXR whose layers are its vars.
struct RenderProduct {
  // The file, relative to the current directory unless absolute, with its
  // frame expanded ("render/beauty.0012.exr").
  std::string file;
  // The camera it sees through: an index into Scene::cameras().
  std::size_t camera = 0;
  // Its size in pixels, width then height.
  std::array<std::int32_t, 2> resolution{};
  float pixel_aspect_ratio = 1.0F;
  AspectRatioConformPolicy aspect_ratio_conform_policy =
      AspectRatioConformPolicy::kExpandAperture;
  // The pixels it holds, in NDC of the aperture: xmin, ymin, xmax, ymax, with
  // (0, 0) at the bottom left and (1, 1) at the top right.
  std::array<float, 4> data_window_ndc{};
  MotionBlur motion_blur = MotionBlur::kEnabled;
  DepthOfField depth_of_field = DepthOfField::kEnabled;
  // At least one var.
  std::vector<RenderVar> vars;
};

// The render settings the render uses.
struct RenderSettings {
  // The path of its prim ("/Render/main").
  std::string path;
  // Its camera: an index into Scene::cameras().
  std::size_t camera = 0;
  // At least one product.
  std::vector<RenderProduct> products;
};

// How the values of a primvar map onto the triangles of a mesh.
enum class PrimvarRate : std::uint8_t {
  // One value, or one array, for the whole mesh.
  kConstant,
  // One value per source face, which each of its triangles takes.
  kPerFace,
  // One value per point, interpolated across each triangle.
  kPerVertex,
  // One value per triangle corner, through Primvar::indices, interpolated
  // across each triangle; corners that share a value share its index.
  kPerCorner,
};

// Values given per point, per face, per corner or for the whole mesh: a
// mesh's normals, or one of its primvars.
struct Primvar {
  std::string name;
  PrimvarRate rate = PrimvarRate::kConstant;
  // The components of each value: 1, 2, 3, 4, or 16 for a 4x4 matrix,
  // row-major.
  std::uint32_t components = 1;
  // The values, `components` numbers each, one after another: floats, or
  // integers for a primvar of integers.
  std::variant<std::vector<float>, std::vector<std::int32_t>> values;
  // kPerCorner only: three per triangle, in the triangle's order, each the
  // index of a value.
  std::vector<std::uint32_t> indices;

  // The number of values.
  [[nodiscard]] std::size_t Count() const;
};

// Whether the back of a mesh's triangles is seen.
enum class Sidedness : std::uint8_t {
  kSingleSided,
  kDoubleSided,
};

// A triangle mesh, in meters, in its object space. Its triangles are all
// wound counterclockwise as seen from their front, the side their geometric
// normal points to by the right-hand rule.
struct Mesh {
  // The path of its prim ("/World/Car/Body").
  std::string path;
  // The positions of its points, one array per motion sample, each as long
  // as the others.
  std::vector<std::vector<std::array<float, 3>>> points;
  // Its triangles, as indices of points.
  std::vector<std::array<std::uint32_t, 3>> triangles;
  // The source face of each triangle: an index into face_vertex_counts.
  std::vector<std::uint32_t> source_faces;
  // The number of vertices of each of the mesh's original polygons, in
  // order, holes included.
  std::vector<std::uint32_t> face_vertex_counts;
  // Unit normals, three components, at any rate but kConstant.
  Primvar normals;
  // Its other primvars, by name.
  std::vector<Primvar> primvars;
  Sidedness sidedness = Sidedness::kSingleSided;
};

// A mask of light-link or shadow-link categories, one bit per category.
using LightMask = std::uint64_t;
// Every category: all the lights.
inline constexpr LightMask kAllLights = ~LightMask{0};

// A placement of a prototype.
struct Instance {
  // Its prototype: an index into Scene::prototypes().
  std::size_t prototype = 0;
  // The object-to-world transform, one per motion sample, row-major, for
  // row vectors: the translation, in meters, is the last row.
  std::vector<std::array<double, 16>> object_to_world;
  // The lights that illuminate it, and those it casts shadows from.
  LightMask light_link = kAllLights;
  LightMask shadow_link = kAllLights;
};

// The scene of one frame.
class Scene {
 public:
  // The cameras are those the settings and their products see through.
  Scene(RenderSettings settings, std::vector<Camera> cameras,
        std::vector<Mesh> prototypes, std::vector<Instance> instances);

  [[nodiscard]] const RenderSettings& settings() const UMBRA_LIFETIMEBOUND {
    return settings_;
  }
  [[nodiscard]] std::span<const Camera> cameras() const UMBRA_LIFETIMEBOUND {
    return cameras_;
  }
  // The geometry held once, for every instance of it.
  [[nodiscard]] std::span<const Mesh> prototypes() const UMBRA_LIFETIMEBOUND {
    return prototypes_;
  }
  [[nodiscard]] std::span<const Instance> instances() const
      UMBRA_LIFETIMEBOUND {
    return instances_;
  }

 private:
  RenderSettings settings_;
  std::vector<Camera> cameras_;
  std::vector<Mesh> prototypes_;
  std::vector<Instance> instances_;
};

}  // namespace umbra::scene

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_SCENE_SCENE_H_
