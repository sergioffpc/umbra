// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The host copies: what the prims of the render delegate convert their sync
// into, in umbra's types, laid out for the devices
// (docs/adr/0013-devices-render-from-their-own-memory.md). Each attribute
// is one array, every numeric array a HostBuffer, aligned to
// device::kBufferAlignment. A prim owns its host copy from its sync to the
// end of the commit, which hands devices views of it.

#ifndef UMBRA_DELEGATE_HOST_COPY_H_
#define UMBRA_DELEGATE_HOST_COPY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ies_tilt.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal::host {

// A numeric array of a host copy. operator new aligns its storage to
// __STDCPP_DEFAULT_NEW_ALIGNMENT__, and its elements' own alignment when it
// is larger.
template <typename T>
using HostBuffer = std::vector<T>;
static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= device::kBufferAlignment);

using device::Color;
using device::ColorAlpha;
using device::DomeTextureFormat;
using device::Emission;
using device::Float3;
using device::Matrix;
using device::Normalization;
using device::PrimvarRate;
using device::Projection;
using device::ShaderKind;
using device::Shutter;
using device::Sidedness;
using device::Triangle;

// The numbers of a primvar: floats, or integers for a primvar of integers.
using Numbers = std::variant<HostBuffer<float>, HostBuffer<std::int32_t>>;

// Values given per point, per face, per corner or for the whole mesh.
struct Primvar {
  std::string name;
  PrimvarRate rate = PrimvarRate::kConstant;
  // The components of each value: 1, 2, 3, 4, or 16 for a 4x4 matrix,
  // row-major.
  std::uint32_t components = 1;
  // The values, `components` numbers each, one after another.
  Numbers values;
  // kPerCorner only: three per triangle, in the triangle's order, each the
  // index of a value.
  HostBuffer<std::uint32_t> indices;

  // The number of values.
  [[nodiscard]] std::size_t Count() const;
};

// A texture a node or a light reads.
struct Texture {
  // The absolute path of its file; a UDIM set keeps its "<UDIM>" token.
  std::string file;
  // The color space of its texels, by its name in the OpenColorIO config.
  std::string color_space;

  bool operator==(const Texture&) const = default;
  auto operator<=>(const Texture&) const = default;
};

// The value of a parameter: a vector or a point of N components is an
// std::array of N, a 3x3 or 4x4 matrix one of 9 or 16, row-major.
using ParameterValue =
    std::variant<bool, std::int32_t, float, std::array<float, 2>,
                 std::array<float, 3>, std::array<float, 4>,
                 std::array<float, 9>, std::array<float, 16>, Color, ColorAlpha,
                 std::string, Texture, HostBuffer<std::int32_t>,
                 HostBuffer<float>, std::vector<std::string>>;

// The value a node gives one of its inputs.
struct Parameter {
  std::string name;
  ParameterValue value;
};

// An input of a node connected to the output of another node.
struct Connection {
  std::string input;
  // An index into MaterialNetwork::nodes.
  std::size_t node = 0;
  std::string output;
};

// A node of a material network.
struct MaterialNode {
  std::string name;
  ShaderKind kind = ShaderKind::kMaterialX;
  // The MaterialX nodedef id, or the absolute path of the .oso shader.
  std::string shader;
  std::vector<Parameter> parameters;
  std::vector<Connection> connections;
};

// The output of a node.
struct NodeOutput {
  // An index into MaterialNetwork::nodes.
  std::size_t node = 0;
  std::string name;
};

// A material, as a network of nodes, with no shader code.
struct MaterialNetwork {
  // The path of its prim, or the name of a material of umbra's.
  std::string path;
  std::vector<MaterialNode> nodes;
  NodeOutput surface;
};

// A placement of a mesh or of a light.
struct Instance {
  // The object-to-world transform, in meters, one per motion sample.
  HostBuffer<Matrix> object_to_world;
  // Its own primvars, constant, one value each.
  std::vector<Primvar> primvars;
};

// A triangle mesh, in meters, in its object space, wound counterclockwise.
struct Mesh {
  std::string path;
  // The positions of its points, one array per motion sample.
  std::vector<HostBuffer<Float3>> points;
  HostBuffer<Triangle> triangles;
  // The source face of each triangle: an index into face_vertex_counts.
  HostBuffer<std::uint32_t> source_faces;
  // The number of vertices of each of its original polygons, holes included.
  HostBuffer<std::uint32_t> face_vertex_counts;
  // Unit normals, three components, at any rate but kConstant.
  Primvar normals;
  std::vector<Primvar> primvars;
  Sidedness sidedness = Sidedness::kSingleSided;
  // Its materials, by the ids the commit assigns: its own first, then those
  // of its GeomSubsets bound to one.
  HostBuffer<device::MaterialId> materials;
  // The material of each triangle, an index into `materials`; empty when
  // every triangle has materials[0].
  HostBuffer<std::uint32_t> triangle_materials;
  // The light its surface emits (UsdLux's MeshLightAPI), its color in
  // ACEScg; none for a mesh that is not a mesh light.
  std::optional<Emission> emission;
};

// An IES profile, in type C photometry, its angles in radians.
struct IesProfile {
  HostBuffer<float> vertical_angles;
  HostBuffer<float> horizontal_angles;
  // As many as vertical angles for each horizontal angle.
  HostBuffer<float> candelas;
  float power = 0.0F;
  Normalization normalization = Normalization::kUnnormalized;
  // Its TILT data, which gives each instance of its light a tilt factor;
  // none for TILT=NONE.
  std::optional<IesTilt> tilt;
};

// A light, an emitter of one of six shapes or a portal, in meters.
struct Light {
  struct Sphere {
    double radius = 0.0;
  };
  struct Rect {
    double width = 0.0;
    double height = 0.0;
    std::optional<Texture> texture;
  };
  struct Disk {
    double radius = 0.0;
  };
  struct Cylinder {
    double length = 0.0;
    double radius = 0.0;
  };
  struct Distant {
    double angle = 0.0;
  };
  struct Dome {
    std::optional<Texture> texture;
    DomeTextureFormat format = DomeTextureFormat::kAutomatic;
    // The rotation from the dome's own frame to the light's space.
    Matrix orientation{
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
    };
    // Its portals, by the ids the commit assigns.
    HostBuffer<device::LightId> portals;
  };
  using Portal = device::Light::Portal;
  using Shape =
      std::variant<Sphere, Rect, Disk, Cylinder, Distant, Dome, Portal>;

  std::string path;
  Shape shape;
  float intensity = 1.0F;
  float exposure = 0.0F;
  Normalization normalization = Normalization::kUnnormalized;
  Color color{.rgb = {1.0F, 1.0F, 1.0F}};
  std::optional<float> color_temperature;
  float cone_angle = 180.0F;
  std::optional<IesProfile> ies_profile;
  // Its placements, light-to-world, without primvars.
  std::vector<Instance> instances;
  // The tilt factor of each instance, which scales its emission: 1 when its
  // IES profile has no TILT data.
  HostBuffer<float> tilt_factors;
};

// A camera, in meters.
struct Camera {
  std::string path;
  Projection projection = Projection::kPerspective;
  // The camera-to-world transform, one per motion sample.
  HostBuffer<Matrix> camera_to_world;
  double focal_length = 0.0;
  std::array<double, 2> aperture{};
  std::array<double, 2> aperture_offset{};
  std::array<double, 2> clipping_range{};
  double f_stop = 0.0;
  double focus_distance = 0.0;
  float exposure_scale = 1.0F;
};

}  // namespace umbra::delegate::internal::host

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_HOST_COPY_H_
