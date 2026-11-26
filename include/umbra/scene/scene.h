// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The Scene: what umbra renders, in umbra's own types, with no OpenUSD type
// (docs/adr/0006-scenes-enter-through-hydra.md). The OpenUSD bridge builds it
// (umbra::ext::usd::LoadScene) and it is immutable once built. Lengths are in
// meters. A mesh is a prototype, held once, that instances place. Materials
// are neutral networks (docs/adr/0008-materials-cross-as-neutral-networks.md)
// and colors are in ACEScg (docs/adr/0007-spectral-rendering-from-acescg.md).

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

// The interval a frame integrates, in frames relative to the frame: when the
// shutter opens and when it closes. Both are 0 when there is no motion blur.
// The motion samples of a prim that moves are equally spaced over it, the
// first when it opens and the last when it closes; a prim that does not move
// has one sample, at the frame.
struct Shutter {
  double open = 0.0;
  double close = 0.0;
};

// A camera, as the stage's Camera prim describes it, in meters.
struct Camera {
  // The path of its prim ("/World/cam").
  std::string path;
  Projection projection = Projection::kPerspective;
  // The camera-to-world transform, one per motion sample, row-major, for row
  // vectors: the translation is the last row. The camera looks down its -Z
  // axis, +Y up.
  std::vector<std::array<double, 16>> camera_to_world;
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

// A color of a material network, in ACEScg, the reference color space. It
// has no spectral kind: the closure it reaches gives it one
// (docs/adr/0007-spectral-rendering-from-acescg.md).
struct Color {
  std::array<float, 3> rgb{};
};

// A color with an alpha, which is not converted.
struct ColorAlpha {
  std::array<float, 3> rgb{};
  float alpha = 1.0F;
};

// A texture a node reads, which the renderer converts to ACEScg as it reads
// its texels.
struct Texture {
  // The absolute path of its file; a UDIM set keeps its "<UDIM>" token
  // ("/show/tex/body.<UDIM>.exr").
  std::string file;
  // The color space of its texels, by its name in the OpenColorIO config
  // ("sRGB Encoded Rec.709 (sRGB)", "Raw").
  std::string color_space;
};

// The value of a parameter: a vector or a point of N components is an
// std::array of N, a 3x3 or 4x4 matrix one of 9 or 16, row-major.
using ParameterValue =
    std::variant<bool, std::int32_t, float, std::array<float, 2>,
                 std::array<float, 3>, std::array<float, 4>,
                 std::array<float, 9>, std::array<float, 16>, Color, ColorAlpha,
                 std::string, Texture, std::vector<std::int32_t>,
                 std::vector<float>, std::vector<std::string>>;

// The value a node gives one of its inputs.
struct Parameter {
  std::string name;
  ParameterValue value;
};

// An input of a node connected to the output of another node.
struct Connection {
  // The input it feeds.
  std::string input;
  // The node it comes from: an index into MaterialNetwork::nodes.
  std::size_t node = 0;
  // The output of that node ("out", "rgb").
  std::string output;
};

// What a node of a material network runs.
enum class ShaderKind : std::uint8_t {
  // A MaterialX nodedef, UsdPreviewSurface's among them.
  kMaterialX,
  // A compiled OSL shader.
  kOsl,
};

// A node of a material network.
struct MaterialNode {
  // Its name within the network, unique ("Texture").
  std::string name;
  ShaderKind kind = ShaderKind::kMaterialX;
  // The MaterialX nodedef id ("ND_standard_surface_surfaceshader"), or the
  // absolute path of the .oso shader.
  std::string shader;
  // The values of its inputs that are not connected; the inputs it does not
  // give take their default.
  std::vector<Parameter> parameters;
  // Its connected inputs.
  std::vector<Connection> connections;
};

// The output of a node.
struct NodeOutput {
  // An index into MaterialNetwork::nodes.
  std::size_t node = 0;
  // Its name ("out", "surface").
  std::string name;
};

// A material, as a network of nodes, with no shader code: the renderer
// compiles it (docs/adr/0008-materials-cross-as-neutral-networks.md).
struct MaterialNetwork {
  // The path of its prim ("/World/Looks/Car"), or the name of a material of
  // umbra's ("fallback").
  std::string path;
  std::vector<MaterialNode> nodes;
  // The output its surface is.
  NodeOutput surface;
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
  // as the others: more than one only when they move.
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
  // Its materials, indices into Scene::materials(): its own first, then
  // those of its GeomSubsets bound to one.
  std::vector<std::size_t> materials;
  // The material of each triangle, an index into `materials`; empty when
  // every triangle has materials[0].
  std::vector<std::uint32_t> triangle_materials;
};

// A placement of a prototype. Every light lights it, and it casts shadows
// from every light (docs/adr/0009-lights-light-everything.md).
struct Instance {
  // Its prototype: an index into Scene::prototypes().
  std::size_t prototype = 0;
  // The object-to-world transform, one per motion sample, row-major, for
  // row vectors: the translation, in meters, is the last row. More than one
  // only when it moves.
  std::vector<std::array<double, 16>> object_to_world;
  // Its own primvars, constant, one value each: they take the place of
  // the prototype's of the same name.
  std::vector<Primvar> primvars;
};

// What a scene renders: its materials, its geometry, held once, and the
// instances that place it.
struct Content {
  std::vector<MaterialNetwork> materials;
  std::vector<Mesh> prototypes;
  std::vector<Instance> instances;
};

// The scene of one frame.
class Scene {
 public:
  // The cameras are those the settings and their products see through.
  Scene(RenderSettings settings, Shutter shutter, std::vector<Camera> cameras,
        Content content);

  [[nodiscard]] const RenderSettings& settings() const UMBRA_LIFETIMEBOUND {
    return settings_;
  }
  // The shutter the motion samples span.
  [[nodiscard]] const Shutter& shutter() const UMBRA_LIFETIMEBOUND {
    return shutter_;
  }
  [[nodiscard]] std::span<const Camera> cameras() const UMBRA_LIFETIMEBOUND {
    return cameras_;
  }
  // The materials of the prototypes.
  [[nodiscard]] std::span<const MaterialNetwork> materials() const
      UMBRA_LIFETIMEBOUND {
    return materials_;
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
  Shutter shutter_;
  std::vector<Camera> cameras_;
  std::vector<MaterialNetwork> materials_;
  std::vector<Mesh> prototypes_;
  std::vector<Instance> instances_;
};

}  // namespace umbra::scene

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_SCENE_SCENE_H_
