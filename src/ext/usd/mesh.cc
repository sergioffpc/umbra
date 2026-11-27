// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "mesh.h"

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/traits.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2h.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4h.h>
#include <pxr/base/gf/vec4i.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/imaging/pxOsd/tokens.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "dicing.h"
#include "host_copy.h"
#include "subdivision.h"
#include "umbra/core/log.h"

namespace umbra::ext::usd::internal {
namespace {

using host::HostBuffer;
using host::Numbers;
using Point = std::array<float, 3>;

// The numbers of a primvar's value, `components` per element.
struct Values {
  Numbers numbers;
  std::uint32_t components = 1;
  std::size_t count = 0;
};

// The scalar type of a primvar element: that of its components.
template <typename T>
struct ScalarOf {
  using Type = T;
};
template <typename T>
  requires pxr::GfIsGfVec<T>::value
struct ScalarOf<T> {
  using Type = T::ScalarType;
};
template <>
struct ScalarOf<pxr::GfMatrix4d> {
  using Type = double;
};

// The components of a primvar element.
template <typename T>
constexpr std::uint32_t ComponentsOf() {
  if constexpr (pxr::GfIsGfVec<T>::value) {
    return static_cast<std::uint32_t>(T::dimension);
  } else if constexpr (std::is_same_v<T, pxr::GfMatrix4d>) {
    return 16;
  } else {
    return 1;
  }
}

// The numbers a host copy holds an element of type T in: integers for
// integers, floats otherwise.
template <typename T>
using NumbersOf =
    std::conditional_t<std::is_same_v<typename ScalarOf<T>::Type, int>,
                       HostBuffer<std::int32_t>, HostBuffer<float>>;

// Appends the components of `element` to `out`.
template <typename T>
void Append(const T& element, NumbersOf<T>& out) {
  if constexpr (pxr::GfIsGfVec<T>::value) {
    Append(element.x, out);
    Append(element.y, out);
    if constexpr (T::dimension > 2) {
      Append(element.z, out);
    }
    if constexpr (T::dimension > 3) {
      Append(element.w, out);
    }
  } else if constexpr (std::is_same_v<T, pxr::GfMatrix4d>) {
    for (int row = 0; row < 4; ++row) {
      Append(element.GetRow(row), out);
    }
  } else if constexpr (std::is_same_v<T, int>) {
    out.push_back(element);
  } else {
    out.push_back(static_cast<float>(element));
  }
}

// The values of `value` when it holds a T or an array of T.
template <typename T>
std::optional<Values> ValuesAs(const pxr::VtValue& value) {
  NumbersOf<T> numbers;
  std::size_t count = 0;
  if (value.IsHolding<pxr::VtArray<T>>()) {
    const auto& array = value.UncheckedGet<pxr::VtArray<T>>();
    numbers.reserve(array.size() * ComponentsOf<T>());
    for (const T& element : array) {
      Append(element, numbers);
    }
    count = array.size();
  } else if (value.IsHolding<T>()) {
    Append(value.UncheckedGet<T>(), numbers);
    count = 1;
  } else {
    return std::nullopt;
  }
  return Values{
      .numbers = std::move(numbers),
      .components = ComponentsOf<T>(),
      .count = count,
  };
}

// The values of the first of Ts that `value` holds, or of an array of.
template <typename... Ts>
std::optional<Values> ValuesAsAny(const pxr::VtValue& value) {
  std::optional<Values> values;
  static_cast<void>(
      ((values = ValuesAs<Ts>(value), values.has_value()) || ...));
  return values;
}

// The values of a numeric primvar, or none for another type.
std::optional<Values> NumericValues(const pxr::VtValue& value) {
  return ValuesAsAny<float, double, pxr::GfHalf, int, pxr::GfVec2f,
                     pxr::GfVec3f, pxr::GfVec4f, pxr::GfVec2d, pxr::GfVec3d,
                     pxr::GfVec4d, pxr::GfVec2h, pxr::GfVec3h, pxr::GfVec4h,
                     pxr::GfVec2i, pxr::GfVec3i, pxr::GfVec4i, pxr::GfMatrix4d>(
      value);
}

// The numbers of the elements of `numbers`, `components` each, at
// `indices`, in their order.
template <typename Number>
HostBuffer<Number> GatherNumbers(std::span<const Number> numbers,
                                 std::size_t components,
                                 std::span<const std::uint32_t> indices) {
  HostBuffer<Number> gathered;
  gathered.reserve(indices.size() * components);
  for (const std::uint32_t index : indices) {
    const std::span<const Number> element = numbers.subspan(
        static_cast<std::size_t>(index) * components, components);
    gathered.insert(gathered.end(), element.begin(), element.end());
  }
  return gathered;
}

// The numbers of the elements at `indices`, in their order.
Numbers Gather(const Values& values, std::span<const std::uint32_t> indices) {
  return std::visit(
      [&](const auto& numbers) -> Numbers {
        return GatherNumbers(std::span(numbers), values.components, indices);
      },
      values.numbers);
}

// `indices` as unsigned, each checked below `count`.
std::expected<std::vector<std::uint32_t>, std::string> CheckedIndices(
    const pxr::VtIntArray& indices, std::size_t count, std::string_view what) {
  std::vector<std::uint32_t> checked;
  checked.reserve(indices.size());
  for (const int index : indices) {
    if (std::cmp_less(index, 0) || std::cmp_greater_equal(index, count)) {
      return std::unexpected(
          std::format("{}: index {} out of range, of {}", what, index, count));
    }
    checked.push_back(static_cast<std::uint32_t>(index));
  }
  return checked;
}

// A polygon mesh's topology, checked, and fanned into triangles.
struct Triangulation {
  HostBuffer<std::uint32_t> face_vertex_counts;
  HostBuffer<host::Triangle> triangles;
  HostBuffer<std::uint32_t> source_faces;
  // The face-vertex of each triangle corner, three per triangle.
  std::vector<std::uint32_t> corners;
  std::size_t face_vertices = 0;
  std::size_t points = 0;
};

// The face-vertex counts, checked: three or more each, adding up to
// `index_count`.
std::expected<HostBuffer<std::uint32_t>, std::string> CheckedCounts(
    const pxr::VtIntArray& counts, std::size_t index_count) {
  HostBuffer<std::uint32_t> checked;
  checked.reserve(counts.size());
  std::size_t total = 0;
  for (const int count : counts) {
    if (count < 3) {
      return std::unexpected(
          std::format("a face of {} vertices, fewer than 3", count));
    }
    checked.push_back(static_cast<std::uint32_t>(count));
    total += static_cast<std::size_t>(count);
  }
  if (total != index_count) {
    return std::unexpected(
        std::format("faceVertexCounts add up to {}, for {} faceVertexIndices",
                    total, index_count));
  }
  return checked;
}

// Whether each face is a hole: 1 for a hole, 0 otherwise. Hole indices
// out of range name no face.
std::vector<std::uint8_t> HoleMarks(const pxr::VtIntArray& holes,
                                    std::size_t face_count) {
  std::vector<std::uint8_t> marks(face_count, 0);
  for (const int hole : holes) {
    if (std::cmp_greater_equal(hole, 0) && std::cmp_less(hole, face_count)) {
      marks.at(static_cast<std::size_t>(hole)) = 1;
    }
  }
  return marks;
}

// Fans the faces into triangles, counterclockwise: those of a leftHanded
// mesh flipped. Holes give none.
void Fan(const pxr::HdMeshTopology& topology,
         const std::vector<std::uint32_t>& indices, Triangulation& out) {
  const bool flip = topology.GetOrientation() == pxr::HdTokens->leftHanded;
  const std::vector<std::uint8_t> holes =
      HoleMarks(topology.GetHoleIndices(), out.face_vertex_counts.size());
  std::uint32_t first = 0;
  for (std::uint32_t face = 0; face < out.face_vertex_counts.size(); ++face) {
    const std::uint32_t count = out.face_vertex_counts.at(face);
    for (std::uint32_t i = 1; holes.at(face) == 0 && i + 1 < count; ++i) {
      const std::uint32_t second = first + (flip ? i + 1 : i);
      const std::uint32_t third = first + (flip ? i : i + 1);
      out.corners.insert(out.corners.end(), {first, second, third});
      out.triangles.push_back(
          {indices.at(first), indices.at(second), indices.at(third)});
      out.source_faces.push_back(face);
    }
    first += count;
  }
}

// A polygon mesh's topology, checked.
struct Polygons {
  HostBuffer<std::uint32_t> face_vertex_counts;
  // The point of each face-vertex.
  std::vector<std::uint32_t> indices;
};

// Checks the topology of a mesh of `point_count` points.
std::expected<Polygons, std::string> CheckTopology(
    const pxr::HdMeshTopology& topology, std::size_t point_count) {
  const pxr::VtIntArray& face_vertex_indices = topology.GetFaceVertexIndices();
  auto counts =
      CheckedCounts(topology.GetFaceVertexCounts(), face_vertex_indices.size());
  if (!counts.has_value()) {
    return std::unexpected(counts.error());
  }
  if (face_vertex_indices.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(std::string("more than 2^32 face vertices"));
  }
  auto indices =
      CheckedIndices(face_vertex_indices, point_count, "faceVertexIndices");
  if (!indices.has_value()) {
    return std::unexpected(indices.error());
  }
  return Polygons{
      .face_vertex_counts = std::move(*counts),
      .indices = std::move(*indices),
  };
}

// Checks the topology of a mesh of `point_count` points and fans it.
std::expected<Triangulation, std::string> Triangulate(
    const pxr::HdMeshTopology& topology, std::size_t point_count) {
  auto polygons = CheckTopology(topology, point_count);
  if (!polygons.has_value()) {
    return std::unexpected(polygons.error());
  }
  Triangulation triangulation;
  triangulation.face_vertex_counts = std::move(polygons->face_vertex_counts);
  triangulation.face_vertices = polygons->indices.size();
  triangulation.points = point_count;
  Fan(topology, polygons->indices, triangulation);
  return triangulation;
}

// The rate of an interpolation, or none for one a mesh does not take.
std::optional<host::PrimvarRate> RateOf(pxr::HdInterpolation interpolation) {
  switch (interpolation) {
    case pxr::HdInterpolationConstant:
      return host::PrimvarRate::kConstant;
    case pxr::HdInterpolationUniform:
      return host::PrimvarRate::kPerFace;
    case pxr::HdInterpolationVertex:
    case pxr::HdInterpolationVarying:
      return host::PrimvarRate::kPerVertex;
    case pxr::HdInterpolationFaceVarying:
      return host::PrimvarRate::kPerCorner;
    default:
      return std::nullopt;
  }
}

// The elements a primvar at `rate` needs on a mesh.
std::size_t Needed(host::PrimvarRate rate, const Triangulation& shape) {
  switch (rate) {
    case host::PrimvarRate::kConstant:
      return 1;
    case host::PrimvarRate::kPerFace:
      return shape.face_vertex_counts.size();
    case host::PrimvarRate::kPerVertex:
      return shape.points;
    case host::PrimvarRate::kPerCorner:
      return shape.face_vertices;
  }
  return 0;
}

// "primvar st has 3 values, 8 needed".
std::string WrongCount(const SourcePrimvar& source, std::size_t given,
                       std::size_t needed) {
  return std::format("primvar {} has {} values, {} needed",
                     source.name.GetString(), given, needed);
}

// The values of a primvar at a rate other than per corner, with its
// indices applied: exactly the elements it needs, or at least one for a
// constant one.
std::expected<Values, std::string> Elements(Values values,
                                            const SourcePrimvar& source,
                                            host::PrimvarRate rate,
                                            std::size_t needed) {
  const std::string what = std::format("primvar {}", source.name.GetString());
  if (!source.indices.empty()) {
    const auto indices = CheckedIndices(source.indices, values.count, what);
    if (!indices.has_value()) {
      return std::unexpected(indices.error());
    }
    values.numbers = Gather(values, *indices);
    values.count = indices->size();
  }
  const bool matches = rate == host::PrimvarRate::kConstant
                           ? values.count >= needed
                           : values.count == needed;
  if (!matches) {
    return std::unexpected(WrongCount(source, values.count, needed));
  }
  return values;
}

// The index of the value of each triangle corner of a face-varying primvar.
std::expected<HostBuffer<std::uint32_t>, std::string> CornerIndices(
    const Values& values, const SourcePrimvar& source,
    const Triangulation& shape) {
  const std::string what = std::format("primvar {}", source.name.GetString());
  const std::size_t needed = shape.face_vertices;
  const std::size_t given =
      source.indices.empty() ? values.count : source.indices.size();
  if (given != needed) {
    return std::unexpected(WrongCount(source, given, needed));
  }
  std::vector<std::uint32_t> face_vertex_values;
  if (!source.indices.empty()) {
    auto checked = CheckedIndices(source.indices, values.count, what);
    if (!checked.has_value()) {
      return std::unexpected(checked.error());
    }
    face_vertex_values = std::move(*checked);
  }
  HostBuffer<std::uint32_t> indices;
  indices.reserve(shape.corners.size());
  for (const std::uint32_t corner : shape.corners) {
    indices.push_back(
        face_vertex_values.empty() ? corner : face_vertex_values.at(corner));
  }
  return indices;
}

// A primvar at the rates devices take; none for one it drops.
std::expected<std::optional<host::Primvar>, std::string> ConvertPrimvar(
    const SourcePrimvar& source, const Triangulation& shape,
    std::string_view path) {
  if (source.element_size > 1) {
    UMBRA_LOG_DEBUG(kUsd,
                    "{}: primvar {} of element size {} not supported; "
                    "dropped",
                    path, source.name.GetString(), source.element_size);
    return std::nullopt;
  }
  const auto rate = RateOf(source.interpolation);
  auto values = NumericValues(source.value);
  if (!rate.has_value() || !values.has_value()) {
    UMBRA_LOG_DEBUG(kUsd, "{}: primvar {} of type {} not supported; dropped",
                    path, source.name.GetString(), source.value.GetTypeName());
    return std::nullopt;
  }
  host::Primvar primvar;
  primvar.name = source.name.GetString();
  primvar.rate = *rate;
  if (*rate == host::PrimvarRate::kPerCorner) {
    auto indices = CornerIndices(*values, source, shape);
    if (!indices.has_value()) {
      return std::unexpected(indices.error());
    }
    primvar.indices = std::move(*indices);
  } else {
    auto elements =
        Elements(std::move(*values), source, *rate, Needed(*rate, shape));
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    values = std::move(*elements);
  }
  primvar.components = values->components;
  primvar.values = std::move(values->numbers);
  return primvar;
}

// `source`, points in scene units, in meters.
HostBuffer<host::Float3> InMeters(const pxr::VtVec3fArray& source,
                                  double meters_per_unit) {
  const auto scale = static_cast<float>(meters_per_unit);
  HostBuffer<host::Float3> points;
  points.reserve(source.size());
  for (const pxr::GfVec3f& point : source) {
    points.push_back({
        .x = point.x * scale,
        .y = point.y * scale,
        .z = point.z * scale,
        .padding = 0.0F,
    });
  }
  return points;
}

// The points of a mesh at the frame, in meters.
std::expected<HostBuffer<host::Float3>, std::string> PointsInMeters(
    const pxr::VtValue& value, double meters_per_unit) {
  if (!value.IsHolding<pxr::VtVec3fArray>()) {
    return std::unexpected(
        std::format("points of type {}, not point3f[]", value.GetTypeName()));
  }
  return InMeters(value.UncheckedGet<pxr::VtVec3fArray>(), meters_per_unit);
}

// The point of `point`, without its padding.
Point PointOf(const host::Float3& point) { return {point.x, point.y, point.z}; }

Point Add(const Point& lhs, const Point& rhs) {
  return {
      std::get<0>(lhs) + std::get<0>(rhs),
      std::get<1>(lhs) + std::get<1>(rhs),
      std::get<2>(lhs) + std::get<2>(rhs),
  };
}

Point Sub(const Point& lhs, const Point& rhs) {
  return {
      std::get<0>(lhs) - std::get<0>(rhs),
      std::get<1>(lhs) - std::get<1>(rhs),
      std::get<2>(lhs) - std::get<2>(rhs),
  };
}

Point Cross(const Point& lhs, const Point& rhs) {
  const auto [lhs_x, lhs_y, lhs_z] = lhs;
  const auto [rhs_x, rhs_y, rhs_z] = rhs;
  return {
      (lhs_y * rhs_z) - (lhs_z * rhs_y),
      (lhs_z * rhs_x) - (lhs_x * rhs_z),
      (lhs_x * rhs_y) - (lhs_y * rhs_x),
  };
}

// `vector` scaled to unit length; zero when it has none.
Point Normalized(const Point& vector) {
  const auto [vec_x, vec_y, vec_z] = vector;
  const float length =
      std::sqrt((vec_x * vec_x) + (vec_y * vec_y) + (vec_z * vec_z));
  const float inverse = length > 0.0F ? 1.0F / length : 0.0F;
  return {vec_x * inverse, vec_y * inverse, vec_z * inverse};
}

// Smooth per-vertex normals: the unit sum of the area-weighted normals of
// the triangles around each point; zero for a point on none.
host::Primvar SmoothNormals(const HostBuffer<host::Float3>& points,
                            const HostBuffer<host::Triangle>& triangles) {
  std::vector<Point> sums(points.size(), Point{});
  for (const auto& [first, second, third] : triangles) {
    const Point origin = PointOf(points.at(first));
    const Point normal = Cross(Sub(PointOf(points.at(second)), origin),
                               Sub(PointOf(points.at(third)), origin));
    for (const std::uint32_t vertex : {first, second, third}) {
      sums.at(vertex) = Add(sums.at(vertex), normal);
    }
  }
  HostBuffer<float> numbers;
  numbers.reserve(sums.size() * 3);
  for (const Point& sum : sums) {
    const Point normal = Normalized(sum);
    numbers.insert(numbers.end(), normal.begin(), normal.end());
  }
  return {
      .name = "normals",
      .rate = host::PrimvarRate::kPerVertex,
      .components = 3,
      .values = std::move(numbers),
      .indices = {},
  };
}

// Whether `primvar` can be a mesh's normals: floats, three components, not
// constant.
bool IsNormals(const host::Primvar& primvar) {
  return primvar.rate != host::PrimvarRate::kConstant &&
         primvar.components == 3 &&
         std::holds_alternative<HostBuffer<float>>(primvar.values);
}

// Takes the authored normals, unless they cannot be normals; computes
// smooth ones from `points` otherwise, or when there are none.
NormalsSource SetNormals(std::optional<host::Primvar> authored,
                         const HostBuffer<host::Float3>& points,
                         host::Mesh& mesh) {
  if (authored.has_value() && IsNormals(*authored)) {
    mesh.normals = std::move(*authored);
    return NormalsSource::kAuthored;
  }
  if (authored.has_value()) {
    UMBRA_LOG_DEBUG(kUsd,
                    "{}: normals not float3 per point, face or corner; "
                    "smooth ones computed",
                    mesh.path);
  }
  mesh.normals = SmoothNormals(points, mesh.triangles);
  return NormalsSource::kComputed;
}

// Whether a primvar is in the skel: namespace: an input to skinning the
// points, not data for the materials.
bool IsSkinningInput(const SourcePrimvar& primvar) {
  return primvar.name.GetString().starts_with("skel:");
}

// Converts the primvars of `source` into `mesh`, its normals apart: none
// when the mesh asks for subdivision, which ignores them. Its skinning
// inputs are left out.
std::expected<std::optional<host::Primvar>, std::string> ConvertPrimvars(
    const SourceMesh& source, const Triangulation& shape, host::Mesh& mesh) {
  std::optional<host::Primvar> normals;
  const bool ignores_normals = IsSubdivided(source.topology);
  for (const SourcePrimvar& primvar : source.primvars) {
    if (primvar.name == pxr::HdTokens->points || IsSkinningInput(primvar) ||
        (ignores_normals && primvar.name == pxr::HdTokens->normals)) {
      continue;
    }
    auto converted = ConvertPrimvar(primvar, shape, source.path);
    if (!converted.has_value()) {
      return std::unexpected(converted.error());
    }
    if (!converted->has_value()) {
      continue;
    }
    if (primvar.name == pxr::HdTokens->normals) {
      normals = std::move(*converted);
    } else {
      mesh.primvars.push_back(std::move(**converted));
    }
  }
  return normals;
}

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

// A primvar of a subdivided mesh that OpenSubdiv refines: per point, or
// per face-vertex.
struct RefinedPrimvar {
  std::string name;
  host::PrimvarRate rate = host::PrimvarRate::kPerVertex;
  subdivision::Smoothness smoothness = subdivision::Smoothness::kSmooth;
  std::uint32_t components = 1;
  HostBuffer<float> numbers;
  // kPerCorner only: the value of each face-vertex.
  std::vector<std::uint32_t> indices;
};

// The primvars of a subdivided mesh: those kept as they are, constant and
// uniform, and those refined.
struct SubdividedPrimvars {
  std::vector<host::Primvar> kept;
  std::vector<RefinedPrimvar> refined;
};

// The value of each face-vertex of a face-varying primvar of `values`:
// its indices, checked, or each face-vertex its own.
std::expected<std::vector<std::uint32_t>, std::string> FaceVertexValues(
    const Values& values, const SourcePrimvar& source,
    std::size_t face_vertices) {
  const std::size_t given =
      source.indices.empty() ? values.count : source.indices.size();
  if (given != face_vertices) {
    return std::unexpected(WrongCount(source, given, face_vertices));
  }
  if (!source.indices.empty()) {
    return CheckedIndices(source.indices, values.count,
                          std::format("primvar {}", source.name.GetString()));
  }
  std::vector<std::uint32_t> indices(face_vertices);
  std::ranges::iota(indices, 0U);
  return indices;
}

// A primvar of a subdivided mesh that OpenSubdiv refines, from its values
// at `rate`, kPerVertex or kPerCorner.
std::expected<RefinedPrimvar, std::string> Refined(Values values,
                                                   const SourcePrimvar& source,
                                                   host::PrimvarRate rate,
                                                   const Polygons& shape,
                                                   std::size_t points) {
  RefinedPrimvar refined{
      .name = source.name.GetString(),
      .rate = rate,
      .smoothness = source.interpolation == pxr::HdInterpolationVarying
                        ? subdivision::Smoothness::kLinear
                        : subdivision::Smoothness::kSmooth,
      .components = values.components,
      .numbers = {},
      .indices = {},
  };
  if (rate == host::PrimvarRate::kPerCorner) {
    auto indices = FaceVertexValues(values, source, shape.indices.size());
    if (!indices.has_value()) {
      return std::unexpected(indices.error());
    }
    refined.indices = std::move(*indices);
  } else {
    auto elements = Elements(std::move(values), source, rate, points);
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    values = std::move(*elements);
  }
  refined.numbers = std::get<HostBuffer<float>>(std::move(values.numbers));
  return refined;
}

// Sorts a primvar of a subdivided mesh of `points` points into `out`: kept,
// refined, or dropped with a debug message.
std::expected<void, std::string> SortPrimvar(const SourcePrimvar& source,
                                             const Polygons& shape,
                                             std::size_t points,
                                             std::string_view path,
                                             SubdividedPrimvars& out) {
  const auto rate = RateOf(source.interpolation);
  auto values = NumericValues(source.value);
  if (source.element_size > 1 || !rate.has_value() || !values.has_value()) {
    UMBRA_LOG_DEBUG(kUsd, "{}: primvar {} of type {} not supported; dropped",
                    path, source.name.GetString(), source.value.GetTypeName());
    return {};
  }
  if (*rate == host::PrimvarRate::kConstant ||
      *rate == host::PrimvarRate::kPerFace) {
    const std::size_t needed = *rate == host::PrimvarRate::kConstant
                                   ? 1
                                   : shape.face_vertex_counts.size();
    auto elements = Elements(std::move(*values), source, *rate, needed);
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    out.kept.push_back({
        .name = source.name.GetString(),
        .rate = *rate,
        .components = elements->components,
        .values = std::move(elements->numbers),
        .indices = {},
    });
    return {};
  }
  if (!std::holds_alternative<HostBuffer<float>>(values->numbers)) {
    UMBRA_LOG_DEBUG(kUsd, "{}: primvar {} of integers not refined; dropped",
                    path, source.name.GetString());
    return {};
  }
  auto refined = Refined(std::move(*values), source, *rate, shape, points);
  if (!refined.has_value()) {
    return std::unexpected(refined.error());
  }
  out.refined.push_back(std::move(*refined));
  return {};
}

// The primvars of `source`, a subdivided mesh of `points` points, but its
// points, its normals and its skinning inputs.
std::expected<SubdividedPrimvars, std::string> SortPrimvars(
    const SourceMesh& source, const Polygons& shape, std::size_t points) {
  SubdividedPrimvars sorted;
  for (const SourcePrimvar& primvar : source.primvars) {
    if (primvar.name == pxr::HdTokens->points ||
        primvar.name == pxr::HdTokens->normals || IsSkinningInput(primvar)) {
      continue;
    }
    if (auto done = SortPrimvar(primvar, shape, points, source.path, sorted);
        !done.has_value()) {
      return std::unexpected(done.error());
    }
  }
  return sorted;
}

subdivision::Scheme SchemeOf(const pxr::TfToken& scheme) {
  if (scheme == pxr::PxOsdOpenSubdivTokens->loop) {
    return subdivision::Scheme::kLoop;
  }
  if (scheme == pxr::PxOsdOpenSubdivTokens->bilinear) {
    return subdivision::Scheme::kBilinear;
  }
  return subdivision::Scheme::kCatmullClark;
}

subdivision::BoundaryInterpolation BoundaryOf(const pxr::TfToken& rule) {
  if (rule == pxr::PxOsdOpenSubdivTokens->none) {
    return subdivision::BoundaryInterpolation::kNone;
  }
  if (rule == pxr::PxOsdOpenSubdivTokens->edgeOnly) {
    return subdivision::BoundaryInterpolation::kEdgeOnly;
  }
  return subdivision::BoundaryInterpolation::kEdgeAndCorner;
}

subdivision::FaceVaryingInterpolation FaceVaryingOf(const pxr::TfToken& rule) {
  using subdivision::FaceVaryingInterpolation;
  const auto& tokens = *pxr::PxOsdOpenSubdivTokens;
  const std::array<std::pair<pxr::TfToken, FaceVaryingInterpolation>, 5> rules{
      {
          {tokens.none, FaceVaryingInterpolation::kNone},
          {tokens.cornersOnly, FaceVaryingInterpolation::kCornersOnly},
          {tokens.cornersPlus2, FaceVaryingInterpolation::kCornersPlus2},
          {tokens.boundaries, FaceVaryingInterpolation::kBoundaries},
          {tokens.all, FaceVaryingInterpolation::kAll},
      },
  };
  const auto* const found = std::ranges::find(
      rules, rule, &std::pair<pxr::TfToken, FaceVaryingInterpolation>::first);
  return found != rules.end() ? found->second
                              : FaceVaryingInterpolation::kCornersPlus1;
}

// The values of `array`, which may be indexed with bounds checks.
template <typename T>
std::vector<T> CopyOf(const pxr::VtArray<T>& array) {
  return {array.begin(), array.end()};
}

// Whether `point` is the index of one of `points` points.
bool IsPoint(int point, std::size_t points) {
  return std::cmp_greater_equal(point, 0) && std::cmp_less(point, points);
}

// The creases of `tags`, as the scene delegate gives them.
struct CreaseTags {
  std::vector<int> indices;
  std::vector<int> lengths;
  std::vector<float> weights;
};

// The sharp edges, between points below `points`, of crease number
// `crease` of `tags`, whose points start at `first` in `tags.indices`;
// `made` sharp edges precede them.
std::vector<subdivision::Crease> CreaseEdges(const CreaseTags& tags,
                                             std::size_t crease,
                                             std::size_t first,
                                             std::size_t made,
                                             std::size_t points) {
  const bool per_crease = tags.weights.size() == tags.lengths.size();
  const auto length =
      static_cast<std::size_t>(std::max(tags.lengths.at(crease), 0));
  std::vector<subdivision::Crease> edges;
  for (std::size_t edge = first + 1; edge < first + length; ++edge) {
    const std::size_t weight = per_crease ? crease : made + edges.size();
    if (edge >= tags.indices.size() || weight >= tags.weights.size() ||
        !IsPoint(tags.indices.at(edge - 1), points) ||
        !IsPoint(tags.indices.at(edge), points)) {
      continue;
    }
    edges.push_back({
        .from = static_cast<std::uint32_t>(tags.indices.at(edge - 1)),
        .to = static_cast<std::uint32_t>(tags.indices.at(edge)),
        .sharpness = tags.weights.at(weight),
    });
  }
  return edges;
}

// The sharp edges of the creases of `tags`, between points below `points`:
// each crease a chain of points, sharp as a whole or edge by edge.
std::vector<subdivision::Crease> CreasesOf(const pxr::PxOsdSubdivTags& tags,
                                           std::size_t points) {
  const CreaseTags crease_tags{
      .indices = CopyOf(tags.GetCreaseIndices()),
      .lengths = CopyOf(tags.GetCreaseLengths()),
      .weights = CopyOf(tags.GetCreaseWeights()),
  };
  std::vector<subdivision::Crease> creases;
  std::size_t first = 0;
  for (std::size_t crease = 0; crease < crease_tags.lengths.size(); ++crease) {
    const std::vector<subdivision::Crease> edges =
        CreaseEdges(crease_tags, crease, first, creases.size(), points);
    creases.insert(creases.end(), edges.begin(), edges.end());
    first +=
        static_cast<std::size_t>(std::max(crease_tags.lengths.at(crease), 0));
  }
  return creases;
}

// The sharp corners of `tags`, at points below `points`.
std::vector<subdivision::Corner> CornersOf(const pxr::PxOsdSubdivTags& tags,
                                           std::size_t points) {
  const std::vector<int> indices = CopyOf(tags.GetCornerIndices());
  const std::vector<float> weights = CopyOf(tags.GetCornerWeights());
  std::vector<subdivision::Corner> corners;
  for (std::size_t corner = 0;
       corner < std::min(indices.size(), weights.size()); ++corner) {
    const int point = indices.at(corner);
    if (IsPoint(point, points)) {
      corners.push_back({
          .point = static_cast<std::uint32_t>(point),
          .sharpness = weights.at(corner),
      });
    }
  }
  return corners;
}

// The faces of a mesh of `face_count` faces that are holes: those of
// `holes` in range.
std::vector<std::uint32_t> HolesOf(const pxr::VtIntArray& holes,
                                   std::size_t face_count) {
  std::vector<std::uint32_t> faces;
  for (const int hole : holes) {
    if (std::cmp_greater_equal(hole, 0) && std::cmp_less(hole, face_count)) {
      faces.push_back(static_cast<std::uint32_t>(hole));
    }
  }
  return faces;
}

// What a cage points into, besides the source mesh.
struct CageArrays {
  Polygons shape;
  std::vector<std::uint32_t> holes;
  std::vector<subdivision::Crease> creases;
  std::vector<subdivision::Corner> corners;
  HostBuffer<host::Float3> frame;
  std::vector<HostBuffer<host::Float3>> motion;
  std::vector<std::span<const host::Float3>> motion_spans;
};

// The control cage of `source`, from `arrays`.
subdivision::Cage CageOf(const SourceMesh& source, const CageArrays& arrays) {
  const pxr::PxOsdSubdivTags& tags = source.subdiv_tags;
  return {
      .scheme = SchemeOf(source.topology.GetScheme()),
      .boundary = BoundaryOf(tags.GetVertexInterpolationRule()),
      .face_varying = FaceVaryingOf(tags.GetFaceVaryingInterpolationRule()),
      .triangles =
          tags.GetTriangleSubdivision() == pxr::PxOsdOpenSubdivTokens->smooth
              ? subdivision::TriangleSubdivision::kSmooth
              : subdivision::TriangleSubdivision::kCatmullClark,
      .creasing = tags.GetCreaseMethod() == pxr::PxOsdOpenSubdivTokens->chaikin
                      ? subdivision::Creasing::kChaikin
                      : subdivision::Creasing::kUniform,
      .orientation =
          source.topology.GetOrientation() == pxr::HdTokens->leftHanded
              ? subdivision::Orientation::kLeftHanded
              : subdivision::Orientation::kRightHanded,
      .face_vertex_counts = arrays.shape.face_vertex_counts,
      .face_vertex_indices = arrays.shape.indices,
      .holes = arrays.holes,
      .creases = arrays.creases,
      .corners = arrays.corners,
      .points = arrays.frame,
      .point_motion = arrays.motion_spans,
  };
}

// Reads the cage of `source` into `arrays`, with its lengths scaled by
// `meters_per_unit`.
std::expected<void, std::string> ReadCage(const SourceMesh& source,
                                          double meters_per_unit,
                                          CageArrays& arrays) {
  auto frame = PointsInMeters(source.points, meters_per_unit);
  if (!frame.has_value()) {
    return std::unexpected(frame.error());
  }
  auto shape = CheckTopology(source.topology, frame->size());
  if (!shape.has_value()) {
    return std::unexpected(shape.error());
  }
  arrays.holes = HolesOf(source.topology.GetHoleIndices(),
                         shape->face_vertex_counts.size());
  arrays.creases = CreasesOf(source.subdiv_tags, frame->size());
  arrays.corners = CornersOf(source.subdiv_tags, frame->size());
  arrays.shape = std::move(*shape);
  arrays.frame = std::move(*frame);
  for (const pxr::VtVec3fArray& sample : source.point_motion) {
    arrays.motion.push_back(InMeters(sample, meters_per_unit));
  }
  for (const HostBuffer<host::Float3>& sample : arrays.motion) {
    arrays.motion_spans.emplace_back(sample);
  }
  return {};
}

// What `view` dices a surface whose points at the frame are `frame` for:
// its closest instance of `instances`.
subdivision::Dicing DicingOf(const DicingView& view,
                             std::span<const host::Float3> frame,
                             std::span<const host::Instance> instances) {
  std::vector<std::span<const host::Matrix>> transforms;
  transforms.reserve(instances.size());
  for (const host::Instance& instance : instances) {
    transforms.emplace_back(instance.object_to_world);
  }
  return {
      .scale = view.scale,
      .viewpoint = subdivision::ClosestViewpoint(frame, transforms, view.eyes),
      .rate = view.rate,
  };
}

// The primvars of a refined mesh: `sorted`, those refined at the rates
// `refinement` gives them.
std::vector<host::Primvar> RefinedPrimvars(
    SubdividedPrimvars sorted, subdivision::Refinement& refinement) {
  std::vector<host::Primvar> primvars = std::move(sorted.kept);
  std::size_t point_values = 0;
  std::size_t face_varying = 0;
  for (RefinedPrimvar& refined : sorted.refined) {
    const bool per_corner = refined.rate == host::PrimvarRate::kPerCorner;
    std::vector<float>& values =
        per_corner ? refinement.face_varying.at(face_varying++)
                   : refinement.point_values.at(point_values++);
    primvars.push_back({
        .name = std::move(refined.name),
        .rate = refined.rate,
        .components = refined.components,
        .values = std::move(values),
        .indices =
            per_corner ? refinement.corners : HostBuffer<std::uint32_t>(),
    });
  }
  return primvars;
}

// The host copy of `source`, refined into `refinement` from its cage
// `shape`, its primvars `sorted`.
ConvertedMesh Assemble(const SourceMesh& source, Polygons shape,
                       SubdividedPrimvars sorted,
                       subdivision::Refinement refinement) {
  ConvertedMesh converted{
      .mesh = {},
      .normals = NormalsSource::kComputed,
      .longest_edge = refinement.longest_edge,
  };
  host::Mesh& mesh = converted.mesh;
  mesh.path = source.path;
  mesh.sidedness = source.sidedness;
  mesh.primvars = RefinedPrimvars(std::move(sorted), refinement);
  mesh.points = std::move(refinement.points);
  mesh.triangles = std::move(refinement.triangles);
  mesh.source_faces = std::move(refinement.source_faces);
  mesh.face_vertex_counts = std::move(shape.face_vertex_counts);
  mesh.normals = {
      .name = "normals",
      .rate = host::PrimvarRate::kPerCorner,
      .components = 3,
      .values = std::move(refinement.normals),
      .indices = std::move(refinement.corners),
  };
  return converted;
}

}  // namespace

bool IsSubdivided(const pxr::HdMeshTopology& topology) {
  const pxr::TfToken scheme = topology.GetScheme();
  return !scheme.IsEmpty() && scheme != pxr::PxOsdOpenSubdivTokens->none;
}

std::expected<ConvertedMesh, std::string> ConvertMesh(const SourceMesh& source,
                                                      double meters_per_unit) {
  auto points = PointsInMeters(source.points, meters_per_unit);
  if (!points.has_value()) {
    return std::unexpected(points.error());
  }
  auto triangulation = Triangulate(source.topology, points->size());
  if (!triangulation.has_value()) {
    return std::unexpected(triangulation.error());
  }
  ConvertedMesh converted;
  host::Mesh& mesh = converted.mesh;
  mesh.path = source.path;
  mesh.sidedness = source.sidedness;
  auto normals = ConvertPrimvars(source, *triangulation, mesh);
  if (!normals.has_value()) {
    return std::unexpected(normals.error());
  }
  mesh.triangles = std::move(triangulation->triangles);
  mesh.source_faces = std::move(triangulation->source_faces);
  mesh.face_vertex_counts = std::move(triangulation->face_vertex_counts);
  converted.normals = SetNormals(std::move(*normals), *points, mesh);
  if (source.point_motion.empty()) {
    mesh.points.push_back(std::move(*points));
    return converted;
  }
  mesh.points.reserve(source.point_motion.size());
  for (const pxr::VtVec3fArray& sample : source.point_motion) {
    mesh.points.push_back(InMeters(sample, meters_per_unit));
  }
  return converted;
}

std::expected<ConvertedMesh, std::string> RefineMesh(
    const SourceMesh& source, double meters_per_unit, const DicingView& view,
    std::span<const host::Instance> instances) {
  CageArrays arrays;
  if (auto read = ReadCage(source, meters_per_unit, arrays);
      !read.has_value()) {
    return std::unexpected(read.error());
  }
  auto sorted = SortPrimvars(source, arrays.shape, arrays.frame.size());
  if (!sorted.has_value()) {
    return std::unexpected(sorted.error());
  }
  std::vector<subdivision::PointValues> point_values;
  std::vector<subdivision::FaceVaryingValues> face_varying;
  for (const RefinedPrimvar& refined : sorted->refined) {
    if (refined.rate == host::PrimvarRate::kPerCorner) {
      face_varying.push_back({
          .values = refined.numbers,
          .components = refined.components,
          .indices = refined.indices,
      });
    } else {
      point_values.push_back({
          .values = refined.numbers,
          .components = refined.components,
          .smoothness = refined.smoothness,
      });
    }
  }
  auto refinement = subdivision::Refine({
      .cage = CageOf(source, arrays),
      .point_values = point_values,
      .face_varying = face_varying,
      .dicing = DicingOf(view, arrays.frame, instances),
  });
  if (!refinement.has_value()) {
    return std::unexpected(std::move(refinement.error()));
  }
  return Assemble(source, std::move(arrays.shape), *std::move(sorted),
                  *std::move(refinement));
}

std::expected<std::vector<HostBuffer<host::Float3>>, std::string> ConvertPoints(
    const pxr::VtValue& points, std::span<const pxr::VtVec3fArray> motion,
    double meters_per_unit) {
  std::vector<HostBuffer<host::Float3>> samples;
  if (motion.empty()) {
    auto frame = PointsInMeters(points, meters_per_unit);
    if (!frame.has_value()) {
      return std::unexpected(frame.error());
    }
    samples.push_back(std::move(*frame));
    return samples;
  }
  samples.reserve(motion.size());
  for (const pxr::VtVec3fArray& sample : motion) {
    samples.push_back(InMeters(sample, meters_per_unit));
  }
  return samples;
}

std::optional<host::Primvar> ConstantPrimvar(std::string name,
                                             const pxr::VtValue& value) {
  auto values = NumericValues(value);
  if (!values.has_value()) {
    return std::nullopt;
  }
  return host::Primvar{
      .name = std::move(name),
      .rate = host::PrimvarRate::kConstant,
      .components = values->components,
      .values = std::move(values->numbers),
      .indices = {},
  };
}

std::array<double, 16> RowMajor(const pxr::GfMatrix4d& transform) {
  std::array<double, 16> converted{};
  for (int row = 0; row < 4; ++row) {
    const pxr::GfVec4d values = transform.GetRow(row);
    const auto first = static_cast<std::size_t>(row) * 4;
    converted.at(first) = values.x;
    converted.at(first + 1) = values.y;
    converted.at(first + 2) = values.z;
    converted.at(first + 3) = values.w;
  }
  return converted;
}

HostBuffer<host::Matrix> TransformsInMeters(
    std::span<const pxr::GfMatrix4d> transforms, double meters_per_unit) {
  HostBuffer<host::Matrix> converted;
  converted.reserve(transforms.size());
  for (const pxr::GfMatrix4d& transform : transforms) {
    converted.push_back(TransformInMeters(transform, meters_per_unit));
  }
  return converted;
}

}  // namespace umbra::ext::usd::internal
