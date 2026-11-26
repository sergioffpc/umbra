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
#include <pxr/imaging/pxOsd/tokens.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "umbra/core/log.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd::internal {
namespace {

using Numbers = std::variant<std::vector<float>, std::vector<std::int32_t>>;
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

// The numbers the Scene holds an element of type T in: integers for
// integers, floats otherwise.
template <typename T>
using NumbersOf =
    std::conditional_t<std::is_same_v<typename ScalarOf<T>::Type, int>,
                       std::vector<std::int32_t>, std::vector<float>>;

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
std::vector<Number> GatherNumbers(std::span<const Number> numbers,
                                  std::size_t components,
                                  std::span<const std::uint32_t> indices) {
  std::vector<Number> gathered;
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
  std::vector<std::uint32_t> face_vertex_counts;
  std::vector<std::array<std::uint32_t, 3>> triangles;
  std::vector<std::uint32_t> source_faces;
  // The face-vertex of each triangle corner, three per triangle.
  std::vector<std::uint32_t> corners;
  std::size_t face_vertices = 0;
  std::size_t points = 0;
};

// The face-vertex counts, checked: three or more each, adding up to
// `index_count`.
std::expected<std::vector<std::uint32_t>, std::string> CheckedCounts(
    const pxr::VtIntArray& counts, std::size_t index_count) {
  std::vector<std::uint32_t> checked;
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

// Checks the topology of a mesh of `point_count` points and fans it.
std::expected<Triangulation, std::string> Triangulate(
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
  const auto indices =
      CheckedIndices(face_vertex_indices, point_count, "faceVertexIndices");
  if (!indices.has_value()) {
    return std::unexpected(indices.error());
  }
  Triangulation triangulation;
  triangulation.face_vertex_counts = std::move(*counts);
  triangulation.face_vertices = face_vertex_indices.size();
  triangulation.points = point_count;
  Fan(topology, *indices, triangulation);
  return triangulation;
}

// The rate of an interpolation, or none for one a mesh does not take.
std::optional<scene::PrimvarRate> RateOf(pxr::HdInterpolation interpolation) {
  switch (interpolation) {
    case pxr::HdInterpolationConstant:
      return scene::PrimvarRate::kConstant;
    case pxr::HdInterpolationUniform:
      return scene::PrimvarRate::kPerFace;
    case pxr::HdInterpolationVertex:
    case pxr::HdInterpolationVarying:
      return scene::PrimvarRate::kPerVertex;
    case pxr::HdInterpolationFaceVarying:
      return scene::PrimvarRate::kPerCorner;
    default:
      return std::nullopt;
  }
}

// The elements a primvar at `rate` needs on a mesh.
std::size_t Needed(scene::PrimvarRate rate, const Triangulation& shape) {
  switch (rate) {
    case scene::PrimvarRate::kConstant:
      return 1;
    case scene::PrimvarRate::kPerFace:
      return shape.face_vertex_counts.size();
    case scene::PrimvarRate::kPerVertex:
      return shape.points;
    case scene::PrimvarRate::kPerCorner:
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
                                            scene::PrimvarRate rate,
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
  const bool matches = rate == scene::PrimvarRate::kConstant
                           ? values.count >= needed
                           : values.count == needed;
  if (!matches) {
    return std::unexpected(WrongCount(source, values.count, needed));
  }
  return values;
}

// The index of the value of each triangle corner of a face-varying primvar.
std::expected<std::vector<std::uint32_t>, std::string> CornerIndices(
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
  std::vector<std::uint32_t> indices;
  indices.reserve(shape.corners.size());
  for (const std::uint32_t corner : shape.corners) {
    indices.push_back(
        face_vertex_values.empty() ? corner : face_vertex_values.at(corner));
  }
  return indices;
}

// A primvar at the Scene's rates; none for one it drops.
std::expected<std::optional<scene::Primvar>, std::string> ConvertPrimvar(
    const SourcePrimvar& source, const Triangulation& shape,
    std::string_view path) {
  const auto rate = RateOf(source.interpolation);
  auto values = NumericValues(source.value);
  if (!rate.has_value() || !values.has_value()) {
    UMBRA_LOG_DEBUG(kUsd, "{}: primvar {} of type {} not supported; dropped",
                    path, source.name.GetString(), source.value.GetTypeName());
    return std::nullopt;
  }
  scene::Primvar primvar;
  primvar.name = source.name.GetString();
  primvar.rate = *rate;
  if (*rate == scene::PrimvarRate::kPerCorner) {
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
std::vector<Point> InMeters(const pxr::VtVec3fArray& source,
                            double meters_per_unit) {
  const auto scale = static_cast<float>(meters_per_unit);
  std::vector<Point> points;
  points.reserve(source.size());
  for (const pxr::GfVec3f& point : source) {
    points.push_back({point.x * scale, point.y * scale, point.z * scale});
  }
  return points;
}

// The points of a mesh at the frame, in meters.
std::expected<std::vector<Point>, std::string> PointsInMeters(
    const pxr::VtValue& value, double meters_per_unit) {
  if (!value.IsHolding<pxr::VtVec3fArray>()) {
    return std::unexpected(
        std::format("points of type {}, not point3f[]", value.GetTypeName()));
  }
  return InMeters(value.UncheckedGet<pxr::VtVec3fArray>(), meters_per_unit);
}

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
scene::Primvar SmoothNormals(
    const std::vector<Point>& points,
    const std::vector<std::array<std::uint32_t, 3>>& triangles) {
  std::vector<Point> sums(points.size(), Point{});
  for (const auto& [first, second, third] : triangles) {
    const Point& origin = points.at(first);
    const Point normal =
        Cross(Sub(points.at(second), origin), Sub(points.at(third), origin));
    for (const std::uint32_t vertex : {first, second, third}) {
      sums.at(vertex) = Add(sums.at(vertex), normal);
    }
  }
  std::vector<float> numbers;
  numbers.reserve(sums.size() * 3);
  for (const Point& sum : sums) {
    const Point normal = Normalized(sum);
    numbers.insert(numbers.end(), normal.begin(), normal.end());
  }
  return {
      .name = "normals",
      .rate = scene::PrimvarRate::kPerVertex,
      .components = 3,
      .values = std::move(numbers),
      .indices = {},
  };
}

// Whether `primvar` can be a mesh's normals: floats, three components, not
// constant.
bool IsNormals(const scene::Primvar& primvar) {
  return primvar.rate != scene::PrimvarRate::kConstant &&
         primvar.components == 3 &&
         std::holds_alternative<std::vector<float>>(primvar.values);
}

// Takes the authored normals, unless they cannot be normals; computes
// smooth ones from `points` otherwise, or when there are none.
void SetNormals(std::optional<scene::Primvar> authored,
                const std::vector<Point>& points, scene::Mesh& mesh) {
  if (authored.has_value() && IsNormals(*authored)) {
    mesh.normals = std::move(*authored);
    return;
  }
  if (authored.has_value()) {
    UMBRA_LOG_DEBUG(kUsd,
                    "{}: normals not float3 per point, face or corner; "
                    "smooth ones computed",
                    mesh.path);
  }
  mesh.normals = SmoothNormals(points, mesh.triangles);
}

// Converts the primvars of `source` into `mesh`, its normals apart: none
// when the mesh asks for subdivision, which ignores them.
std::expected<std::optional<scene::Primvar>, std::string> ConvertPrimvars(
    const SourceMesh& source, const Triangulation& shape, scene::Mesh& mesh) {
  std::optional<scene::Primvar> normals;
  const bool ignores_normals = IsSubdivided(source.topology);
  for (const SourcePrimvar& primvar : source.primvars) {
    if (primvar.name == pxr::HdTokens->points ||
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
std::array<double, 16> TransformInMeters(const pxr::GfMatrix4d& transform,
                                         double meters_per_unit) {
  std::array<double, 16> converted = RowMajor(transform);
  // The translation, in the last row, is a length; the rest is not.
  for (std::size_t column = 0; column < 3; ++column) {
    converted.at(12 + column) *= meters_per_unit;
  }
  return converted;
}

}  // namespace

bool IsSubdivided(const pxr::HdMeshTopology& topology) {
  const pxr::TfToken scheme = topology.GetScheme();
  return !scheme.IsEmpty() && scheme != pxr::PxOsdOpenSubdivTokens->none;
}

std::expected<scene::Mesh, std::string> ConvertMesh(const SourceMesh& source,
                                                    double meters_per_unit) {
  auto points = PointsInMeters(source.points, meters_per_unit);
  if (!points.has_value()) {
    return std::unexpected(points.error());
  }
  auto triangulation = Triangulate(source.topology, points->size());
  if (!triangulation.has_value()) {
    return std::unexpected(triangulation.error());
  }
  scene::Mesh mesh;
  mesh.path = source.path;
  mesh.sidedness = source.sidedness;
  auto normals = ConvertPrimvars(source, *triangulation, mesh);
  if (!normals.has_value()) {
    return std::unexpected(normals.error());
  }
  mesh.triangles = std::move(triangulation->triangles);
  mesh.source_faces = std::move(triangulation->source_faces);
  mesh.face_vertex_counts = std::move(triangulation->face_vertex_counts);
  SetNormals(std::move(*normals), *points, mesh);
  if (source.point_motion.empty()) {
    mesh.points.push_back(std::move(*points));
    return mesh;
  }
  mesh.points.reserve(source.point_motion.size());
  for (const pxr::VtVec3fArray& sample : source.point_motion) {
    mesh.points.push_back(InMeters(sample, meters_per_unit));
  }
  return mesh;
}

std::optional<scene::Primvar> ConstantPrimvar(std::string name,
                                              const pxr::VtValue& value) {
  auto values = NumericValues(value);
  if (!values.has_value()) {
    return std::nullopt;
  }
  return scene::Primvar{
      .name = std::move(name),
      .rate = scene::PrimvarRate::kConstant,
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

std::vector<std::array<double, 16>> TransformsInMeters(
    std::span<const pxr::GfMatrix4d> transforms, double meters_per_unit) {
  std::vector<std::array<double, 16>> converted;
  converted.reserve(transforms.size());
  for (const pxr::GfMatrix4d& transform : transforms) {
    converted.push_back(TransformInMeters(transform, meters_per_unit));
  }
  return converted;
}

}  // namespace umbra::ext::usd::internal
