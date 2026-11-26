// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a device builds its render data from, as the render delegate hands it
// over at a commit (docs/adr/0013-devices-render-from-their-own-memory.md):
// views of plain data, spans and string views into the host copies of the
// prims, valid only during the commit. They own nothing and hold no OpenUSD
// type. Lengths are in meters, colors in ACEScg
// (docs/adr/0007-spectral-rendering-from-acescg.md), and materials are
// neutral networks (docs/adr/0008-materials-cross-as-neutral-networks.md).
//
// Each attribute is one array: points are Float3 elements at a 16-byte
// stride, one array per motion sample, and triangles three uint32 indices
// each. Every numeric array starts at a multiple of kBufferAlignment, so
// that Embree and OptiX read it in place.

#ifndef UMBRA_DEVICE_RENDER_DATA_H_
#define UMBRA_DEVICE_RENDER_DATA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// The alignment of every numeric array a view points to, in bytes.
inline constexpr std::size_t kBufferAlignment = 16;

// A prim's id: a dense integer per kind, which the render delegate assigns
// and which is the same on every device. Render data refers to prims by it.
enum class PrototypeId : std::uint32_t {};
enum class LightId : std::uint32_t {};
enum class MaterialId : std::uint32_t {};
enum class CameraId : std::uint32_t {};
enum class TextureId : std::uint32_t {};

// A point or a vector, at a 16-byte stride: the fourth float is padding.
struct alignas(16) Float3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float padding = 0.0F;
};
static_assert(sizeof(Float3) == 16);

// A triangle, as the indices of its three points.
using Triangle = std::array<std::uint32_t, 3>;

// A transform, row-major, for row vectors: the translation, in meters, is
// the last row.
using Matrix = std::array<double, 16>;

// How a camera projects onto its aperture.
enum class Projection : std::uint8_t {
  kPerspective,
  kOrthographic,
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
// mesh's normals, one of its primvars, or one of an instance's.
struct Primvar {
  std::string_view name;
  PrimvarRate rate = PrimvarRate::kConstant;
  // The components of each value: 1, 2, 3, 4, or 16 for a 4x4 matrix,
  // row-major.
  std::uint32_t components = 1;
  // The values, `components` numbers each, one after another: floats, or
  // integers for a primvar of integers. The other is empty.
  std::span<const float> floats;
  std::span<const std::int32_t> integers;
  // kPerCorner only: three per triangle, in the triangle's order, each the
  // index of a value.
  std::span<const std::uint32_t> indices;

  // The number of values.
  [[nodiscard]] std::size_t Count() const;
};

// A placement of a prototype or of a light. Every light lights it, and it
// casts shadows from every light (docs/adr/0009-lights-light-everything.md).
struct Instance {
  // The object-to-world transform, one per motion sample: more than one only
  // when it moves.
  std::span<const Matrix> object_to_world;
  // Its own primvars, constant, one value each: they take the place of the
  // prototype's of the same name.
  std::span<const Primvar> primvars;
};

// Whether the back of a mesh's triangles is seen.
enum class Sidedness : std::uint8_t {
  kSingleSided,
  kDoubleSided,
};

// A triangle mesh, held once, in its object space, with the instances that
// place it: one for a mesh that is not instanced, and one for each instance
// of every instancer above it, nested instancers flattened. Its triangles
// are all wound counterclockwise as seen from their front, the side their
// geometric normal points to by the right-hand rule.
struct Prototype {
  PrototypeId id{};
  // The path of its prim ("/World/Car/Body").
  std::string_view path;
  // The positions of its points, one array per motion sample, each as long
  // as the others: more than one only when they move.
  std::span<const std::span<const Float3>> points;
  std::span<const Triangle> triangles;
  // The source face of each triangle: an index into face_vertex_counts.
  std::span<const std::uint32_t> source_faces;
  // The number of vertices of each of the mesh's original polygons, in
  // order, holes included.
  std::span<const std::uint32_t> face_vertex_counts;
  // Unit normals, three components, at any rate but kConstant.
  Primvar normals;
  // Its other primvars, by name.
  std::span<const Primvar> primvars;
  Sidedness sidedness = Sidedness::kSingleSided;
  // Its materials: its own first, then those of its GeomSubsets bound to
  // one.
  std::span<const MaterialId> materials;
  // The material of each triangle, an index into `materials`; empty when
  // every triangle has materials[0].
  std::span<const std::uint32_t> triangle_materials;
  // At least one.
  std::span<const Instance> instances;
};

// A texture, read by materials and lights, which the renderer converts to
// ACEScg as it reads its texels.
struct Texture {
  TextureId id{};
  // The absolute path of its file; a UDIM set keeps its "<UDIM>" token
  // ("/show/tex/body.<UDIM>.exr").
  std::string_view file;
  // The color space of its texels, by its name in the OpenColorIO config
  // ("sRGB Encoded Rec.709 (sRGB)", "Raw").
  std::string_view color_space;
};

// A color, in ACEScg, the reference color space. It has no spectral kind:
// what reads it gives it one
// (docs/adr/0007-spectral-rendering-from-acescg.md).
struct Color {
  std::array<float, 3> rgb{};
};

// A color with an alpha, which is not converted.
struct ColorAlpha {
  std::array<float, 3> rgb{};
  float alpha = 1.0F;
};

// The value of a parameter: a vector or a point of N components is an
// std::array of N, a 3x3 or 4x4 matrix one of 9 or 16, row-major.
using ParameterValue =
    std::variant<bool, std::int32_t, float, std::array<float, 2>,
                 std::array<float, 3>, std::array<float, 4>,
                 std::array<float, 9>, std::array<float, 16>, Color, ColorAlpha,
                 std::string_view, TextureId, std::span<const std::int32_t>,
                 std::span<const float>, std::span<const std::string_view>>;

// The value a node gives one of its inputs.
struct Parameter {
  std::string_view name;
  ParameterValue value;
};

// An input of a node connected to the output of another node.
struct Connection {
  // The input it feeds.
  std::string_view input;
  // The node it comes from: an index into MaterialNetwork::nodes.
  std::uint32_t node = 0;
  // The output of that node ("out", "rgb").
  std::string_view output;
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
  std::string_view name;
  ShaderKind kind = ShaderKind::kMaterialX;
  // The MaterialX nodedef id ("ND_standard_surface_surfaceshader"), or the
  // absolute path of the .oso shader.
  std::string_view shader;
  // The values of its inputs that are not connected; the inputs it does not
  // give take their default.
  std::span<const Parameter> parameters;
  // Its connected inputs.
  std::span<const Connection> connections;
};

// The output of a node.
struct NodeOutput {
  // An index into MaterialNetwork::nodes.
  std::uint32_t node = 0;
  // Its name ("out", "surface").
  std::string_view name;
};

// A material, as a network of nodes, with no shader code
// (docs/adr/0008-materials-cross-as-neutral-networks.md).
struct MaterialNetwork {
  MaterialId id{};
  // The path of its prim ("/World/Looks/Car"), or the name of a material of
  // umbra's ("fallback").
  std::string_view path;
  std::span<const MaterialNode> nodes;
  // The output its surface is.
  NodeOutput surface;
};

// Whether a light's emission is divided by its size, so that its power does
// not change with it: by the world-space area of an area light, by the
// solid angle of a distant light.
enum class Normalization : std::uint8_t {
  kUnnormalized,
  kNormalized,
};

// How the texture of a dome light maps onto the directions around it.
enum class DomeTextureFormat : std::uint8_t {
  // As the file tells, latlong when it tells nothing.
  kAutomatic,
  // Longitude across, latitude down, as OpenEXR defines it.
  kLatlong,
  // An image of the environment reflected in a sphere, seen orthographically.
  kMirroredBall,
  // As kMirroredBall, with the radius proportional to the angle.
  kAngular,
  // The six faces of a cube, as a vertical cross.
  kCubeMapVerticalCross,
};

// A light, an emitter of one of six shapes, placed by its instances. Every
// light lights every instance and is seen by camera rays
// (docs/adr/0009-lights-light-everything.md); it has no artistic control
// without a physical counterpart (docs/adr/0010-lights-are-physical.md).
struct Light {
  // A sphere, around its origin.
  struct Sphere {
    double radius = 0.0;
  };
  // A rectangle in its XY plane, centered on its origin, that emits towards
  // its -Z axis, with the texture that colors it, if any.
  struct Rect {
    double width = 0.0;
    double height = 0.0;
    std::optional<TextureId> texture;
  };
  // A disk in its XY plane, centered on its origin, that emits towards its
  // -Z axis.
  struct Disk {
    double radius = 0.0;
  };
  // A cylinder along its X axis, centered on its origin, without caps.
  struct Cylinder {
    double length = 0.0;
    double radius = 0.0;
  };
  // A light infinitely far away, towards its +Z axis, that shines towards
  // -Z: the angle, in degrees, is the diameter it subtends; 0 for parallel
  // light.
  struct Distant {
    double angle = 0.0;
  };
  // The environment, infinitely far away, colored by its texture if any.
  struct Dome {
    std::optional<TextureId> texture;
    DomeTextureFormat format = DomeTextureFormat::kAutomatic;
    // The rotation from the dome's own frame, where its top pole is +Y and
    // its texture maps as its format says, to the light's space: the
    // alignment of its pole with the stage's up axis, when the stage asks
    // for it.
    Matrix orientation{
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
    };
  };
  using Shape = std::variant<Sphere, Rect, Disk, Cylinder, Distant, Dome>;

  LightId id{};
  // The path of its prim ("/World/key").
  std::string_view path;
  Shape shape;
  // Its luminance, in nits, before its color: intensity times 2 to the
  // power of exposure.
  float intensity = 1.0F;
  float exposure = 0.0F;
  Normalization normalization = Normalization::kUnnormalized;
  // The color it emits, of the illuminant kind
  // (docs/adr/0007-spectral-rendering-from-acescg.md).
  Color color{.rgb = {1.0F, 1.0F, 1.0F}};
  // The temperature, in kelvin, of the blackbody whose spectrum, normalized
  // so that 6500 K is white, multiplies its color; none for its color alone.
  // It is never turned into RGB.
  std::optional<float> color_temperature;
  // The half-angle, in degrees, of the cone around its -Z axis outside which
  // it emits nothing, from 0 to 180; 180 for no cone.
  float cone_angle = 180.0F;
  // Its placements, light-to-world, without primvars: at least one.
  std::span<const Instance> instances;
};

// The name of each shape of a light, in the order of Light::Shape.
inline constexpr std::array<std::string_view, 6> kLightShapeNames = {
    "sphere", "rect", "disk", "cylinder", "distant", "dome",
};
static_assert(kLightShapeNames.size() == std::variant_size_v<Light::Shape>);

// A camera, as the stage's Camera prim describes it.
struct Camera {
  CameraId id{};
  // The path of its prim ("/World/cam").
  std::string_view path;
  Projection projection = Projection::kPerspective;
  // The camera-to-world transform, one per motion sample. The camera looks
  // down its -Z axis, +Y up.
  std::span<const Matrix> camera_to_world;
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

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_RENDER_DATA_H_
