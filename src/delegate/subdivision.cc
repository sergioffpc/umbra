// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "subdivision.h"

#include <opensubdiv/bfr/parameterization.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/tessellation.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyLevel.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/far/types.h>
#include <opensubdiv/sdc/options.h>
#include <opensubdiv/sdc/types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dicing.h"
#include "umbra/core/safety.h"
#include "umbra/device/geometry.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal::subdivision {
namespace {

namespace bfr = ::OpenSubdiv::Bfr;
namespace far = ::OpenSubdiv::Far;
namespace sdc = ::OpenSubdiv::Sdc;

using far::ConstIndexArray;
using far::Index;
using FaceSurface = bfr::Surface<float>;
using SurfaceFactory = bfr::RefinerSurfaceFactory<>;
using Uv = std::array<float, 2>;

// Element `index` of `values`, checked against its bounds.
template <typename T>
const T& ElementOf(std::span<const T> values, std::size_t index) {
  return values.subspan(index, 1).front();
}
using device::Vector3;
// A value of a primvar: at most 16 numbers, those of a 4x4 matrix.
using Value = std::array<float, 16>;

// The segments an edge's length on screen is measured along.
constexpr int kEdgeSamples = 4;

// The most tessellations of a surface: the first, and those that cut the
// faces whose triangles are still too long.
constexpr int kMaxRounds = 4;

// No point: a vertex or an edge of the cage whose points no face has made
// yet.
constexpr std::uint32_t kNoPoint = std::numeric_limits<std::uint32_t>::max();

sdc::SchemeType SchemeOf(Scheme scheme) {
  switch (scheme) {
    case Scheme::kBilinear:
      return sdc::SCHEME_BILINEAR;
    case Scheme::kLoop:
      return sdc::SCHEME_LOOP;
    case Scheme::kCatmullClark:
      break;
  }
  return sdc::SCHEME_CATMARK;
}

sdc::Options::VtxBoundaryInterpolation BoundaryOf(
    BoundaryInterpolation boundary) {
  switch (boundary) {
    case BoundaryInterpolation::kNone:
      return sdc::Options::VTX_BOUNDARY_NONE;
    case BoundaryInterpolation::kEdgeOnly:
      return sdc::Options::VTX_BOUNDARY_EDGE_ONLY;
    case BoundaryInterpolation::kEdgeAndCorner:
      break;
  }
  return sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER;
}

sdc::Options::FVarLinearInterpolation FaceVaryingOf(
    FaceVaryingInterpolation interpolation) {
  switch (interpolation) {
    case FaceVaryingInterpolation::kNone:
      return sdc::Options::FVAR_LINEAR_NONE;
    case FaceVaryingInterpolation::kCornersOnly:
      return sdc::Options::FVAR_LINEAR_CORNERS_ONLY;
    case FaceVaryingInterpolation::kCornersPlus2:
      return sdc::Options::FVAR_LINEAR_CORNERS_PLUS2;
    case FaceVaryingInterpolation::kBoundaries:
      return sdc::Options::FVAR_LINEAR_BOUNDARIES;
    case FaceVaryingInterpolation::kAll:
      return sdc::Options::FVAR_LINEAR_ALL;
    case FaceVaryingInterpolation::kCornersPlus1:
      break;
  }
  return sdc::Options::FVAR_LINEAR_CORNERS_PLUS1;
}

sdc::Options OptionsOf(const Cage& cage) {
  sdc::Options options;
  options.SetVtxBoundaryInterpolation(BoundaryOf(cage.boundary));
  options.SetFVarLinearInterpolation(FaceVaryingOf(cage.face_varying));
  options.SetCreasingMethod(cage.creasing == Creasing::kChaikin
                                ? sdc::Options::CREASE_CHAIKIN
                                : sdc::Options::CREASE_UNIFORM);
  options.SetTriangleSubdivision(cage.triangles == TriangleSubdivision::kSmooth
                                     ? sdc::Options::TRI_SUB_SMOOTH
                                     : sdc::Options::TRI_SUB_CATMARK);
  return options;
}

std::vector<int> AsInts(std::span<const std::uint32_t> values) {
  std::vector<int> ints;
  ints.reserve(values.size());
  for (const std::uint32_t value : values) {
    ints.push_back(static_cast<int>(value));
  }
  return ints;
}

// The arrays a TopologyDescriptor points into, and the descriptor.
struct Descriptor {
  std::vector<int> counts;
  std::vector<int> indices;
  std::vector<int> holes;
  std::vector<int> crease_points;
  std::vector<float> crease_weights;
  std::vector<int> corner_points;
  std::vector<float> corner_weights;
  std::vector<std::vector<int>> value_indices;
  std::vector<far::TopologyDescriptor::FVarChannel> channels;
  far::TopologyDescriptor descriptor;
};

// Fills `arrays` with the creases and corners of `cage`.
void DescribeSharpness(const Cage& cage, Descriptor& arrays) {
  for (const Crease& crease : cage.creases) {
    arrays.crease_points.push_back(static_cast<int>(crease.from));
    arrays.crease_points.push_back(static_cast<int>(crease.to));
    arrays.crease_weights.push_back(crease.sharpness);
  }
  for (const Corner& corner : cage.corners) {
    arrays.corner_points.push_back(static_cast<int>(corner.point));
    arrays.corner_weights.push_back(corner.sharpness);
  }
  far::TopologyDescriptor& descriptor = arrays.descriptor;
  descriptor.numCreases = static_cast<int>(cage.creases.size());
  descriptor.creaseVertexIndexPairs = arrays.crease_points.data();
  descriptor.creaseWeights = arrays.crease_weights.data();
  descriptor.numCorners = static_cast<int>(cage.corners.size());
  descriptor.cornerVertexIndices = arrays.corner_points.data();
  descriptor.cornerWeights = arrays.corner_weights.data();
}

// Fills `arrays` with the face-varying channels of `surface`, one per
// FaceVaryingValues.
void DescribeFaceVarying(const Surface& surface, Descriptor& arrays) {
  for (const FaceVaryingValues& values : surface.face_varying) {
    arrays.value_indices.push_back(AsInts(values.indices));
  }
  for (std::size_t channel = 0; channel < arrays.value_indices.size();
       ++channel) {
    const FaceVaryingValues& values = ElementOf(surface.face_varying, channel);
    far::TopologyDescriptor::FVarChannel described;
    described.numValues = static_cast<int>(values.values.size() /
                                           std::max(values.components, 1U));
    described.valueIndices = arrays.value_indices.at(channel).data();
    arrays.channels.push_back(described);
  }
  arrays.descriptor.numFVarChannels = static_cast<int>(arrays.channels.size());
  arrays.descriptor.fvarChannels = arrays.channels.data();
}

// Fills `arrays` with what OpenSubdiv refines `surface` from.
void Describe(const Surface& surface, Descriptor& arrays) {
  const Cage& cage = surface.cage;
  arrays.counts = AsInts(cage.face_vertex_counts);
  arrays.indices = AsInts(cage.face_vertex_indices);
  arrays.holes = AsInts(cage.holes);
  far::TopologyDescriptor& descriptor = arrays.descriptor;
  descriptor.numVertices = static_cast<int>(cage.points.size());
  descriptor.numFaces = static_cast<int>(arrays.counts.size());
  descriptor.numVertsPerFace = arrays.counts.data();
  descriptor.vertIndicesPerFace = arrays.indices.data();
  descriptor.numHoles = static_cast<int>(arrays.holes.size());
  descriptor.holeIndices = arrays.holes.data();
  descriptor.isLeftHanded = cage.orientation == Orientation::kLeftHanded;
  DescribeSharpness(cage, arrays);
  DescribeFaceVarying(surface, arrays);
}

// The topology of `surface`, for OpenSubdiv to refine.
std::expected<std::unique_ptr<far::TopologyRefiner>, std::string> CreateRefiner(
    const Surface& surface) {
  const Cage& cage = surface.cage;
  if (cage.scheme == Scheme::kLoop) {
    const auto polygon =
        std::ranges::find_if(cage.face_vertex_counts,
                             [](std::uint32_t count) { return count != 3; });
    if (polygon != cage.face_vertex_counts.end()) {
      return std::unexpected(std::format(
          "loop subdivision of a face of {} vertices, not a triangle",
          *polygon));
    }
  }
  if (cage.points.size() > std::numeric_limits<int>::max() ||
      cage.face_vertex_indices.size() > std::numeric_limits<int>::max()) {
    return std::unexpected(std::string("too large to subdivide"));
  }
  Descriptor arrays;
  Describe(surface, arrays);
  using Factory = far::TopologyRefinerFactory<far::TopologyDescriptor>;
  std::unique_ptr<far::TopologyRefiner> refiner(Factory::Create(
      arrays.descriptor,
      Factory::Options(SchemeOf(cage.scheme), OptionsOf(cage))));
  if (refiner == nullptr) {
    return std::unexpected(std::string("OpenSubdiv rejects its topology"));
  }
  return refiner;
}

// Appends the first `components` numbers of `value` to `out`.
void Append(const Value& value, std::uint32_t components,
            std::vector<float>& out) {
  out.insert(out.end(), value.begin(),
             std::next(value.begin(), static_cast<std::ptrdiff_t>(components)));
}

// The limit stencil of a point of a face's surface: the weight of each of
// its control points, for its position and its derivatives.
class Stencil {
 public:
  // Takes the control points of `surface`.
  void Bind(const FaceSurface& surface) {
    const auto count = static_cast<std::size_t>(surface.GetNumControlPoints());
    controls_.resize(count);
    position_.resize(count);
    du_.resize(count);
    dv_.resize(count);
    static_cast<void>(surface.GetControlPointIndices(controls_.data()));
  }

  // Evaluates the stencil of the bound `surface` at `parameter`.
  void At(const FaceSurface& surface, const Uv& parameter) {
    static_cast<void>(surface.EvaluateStencil(
        parameter.data(), position_.data(), du_.data(), dv_.data()));
  }

  [[nodiscard]] device::Float3 Position(
      std::span<const device::Float3> points) const {
    const Vector3 sum = Weigh(position_, points);
    return {.x = sum.x, .y = sum.y, .z = sum.z, .padding = 0.0F};
  }

  // The unit normal, from the derivatives over `points`.
  [[nodiscard]] Vector3 Normal(std::span<const device::Float3> points) const {
    return device::Normalize(
        device::Cross(Weigh(du_, points), Weigh(dv_, points)));
  }

  // The weighted sum of `values`, `components` numbers per control point:
  // its first `components` numbers.
  [[nodiscard]] Value Weigh(std::span<const float> values,
                            std::uint32_t components) const {
    Value sum{};
    for (std::size_t component = 0; component < components; ++component) {
      for (std::size_t control = 0; control < controls_.size(); ++control) {
        const auto value =
            (static_cast<std::size_t>(controls_.at(control)) * components) +
            component;
        sum.at(component) += position_.at(control) * ElementOf(values, value);
      }
    }
    return sum;
  }

 private:
  // The weighted sum of the control points of `points`.
  [[nodiscard]] Vector3 Weigh(const std::vector<float>& weights,
                              std::span<const device::Float3> points) const {
    Vector3 sum;
    for (std::size_t control = 0; control < controls_.size(); ++control) {
      const float weight = weights.at(control);
      const device::Float3& point =
          ElementOf(points, static_cast<std::size_t>(controls_.at(control)));
      sum.x += weight * point.x;
      sum.y += weight * point.y;
      sum.z += weight * point.z;
    }
    return sum;
  }

  std::vector<Index> controls_;
  std::vector<float> position_;
  std::vector<float> du_;
  std::vector<float> dv_;
};

// The points an edge of the cage gives the faces that meet there: those a
// face made first, along it from the vertex `from`.
struct EdgePoints {
  std::uint32_t first = kNoPoint;
  Index from = -1;
};

// Whether a surface has values given per point that vary linearly.
enum class Varying : std::uint8_t {
  kAbsent,
  kPresent,
};

// The surfaces of a face, and their stencils.
struct FaceSurfaces {
  FaceSurface vertex;
  Stencil vertex_stencil;
  FaceSurface varying;
  Stencil varying_stencil;
  std::vector<FaceSurface> face_varying;
  std::vector<Stencil> face_varying_stencils;
};

// The tessellation of one face: its coordinates and triangles, and the point
// of each coordinate.
struct FaceTessellation {
  std::vector<float> coords;
  std::vector<int> facets;
  // The point of each coordinate.
  std::vector<std::uint32_t> points;
  // The coordinates that are new points, in the order of their indices.
  std::vector<std::size_t> made;
};

// Refines one surface.
class Refiner {
 public:
  Refiner(const Surface& surface UMBRA_LIFETIMEBOUND,
          const far::TopologyRefiner& topology UMBRA_LIFETIMEBOUND)
      : surface_(&surface),
        base_(&topology.GetLevel(0)),
        factory_(topology, SurfaceFactory::Options()),
        varying_(std::ranges::any_of(surface.point_values,
                                     [](const PointValues& values) {
                                       return values.smoothness ==
                                              Smoothness::kLinear;
                                     })
                     ? Varying::kPresent
                     : Varying::kAbsent),
        edge_rates_(static_cast<std::size_t>(base_->GetNumEdges()), 1U) {
    surfaces_.face_varying.resize(surface.face_varying.size());
    surfaces_.face_varying_stencils.resize(surface.face_varying.size());
  }

  // Measures the edges of the cage on screen and gives each its rate.
  void RateEdges();
  // Tessellates every face at the edges' rates.
  void Tessellate();
  // Cuts further the edges of the faces whose triangles are longer than
  // the dicing rate; gives the number of edges it cut finer.
  std::size_t CutFurther();
  [[nodiscard]] Refinement Take() { return std::move(out_); }

 private:
  [[nodiscard]] double Target(const FaceSurface& surface) const;
  void RateFaceEdges(Index face);
  // The length in pixels of edge `edge` of the bound vertex surface.
  [[nodiscard]] double EdgePixels(int edge);
  void Reset();
  void TessellateFace(Index face);
  // The rates of the edges of `face`, and its inner rates.
  [[nodiscard]] std::vector<int> FaceRates(Index face,
                                           const FaceSurface& surface) const;
  // Gives each coordinate of `tessellation` its point: those of the
  // vertices and edges other faces made, or new ones.
  void AssignPoints(Index face, const bfr::Tessellation& pattern,
                    FaceTessellation& tessellation);
  void AssignEdgePoints(Index edge, Index from, std::size_t count,
                        FaceTessellation& tessellation);
  std::uint32_t NewPoint(std::size_t coord, FaceTessellation& tessellation);
  // Evaluates the new points and the corners of `tessellation`.
  void Evaluate(Index face, const FaceTessellation& tessellation);
  void EvaluatePoint(std::uint32_t point, const Uv& parameter);
  // Makes room for `count` more points.
  void GrowPoints(std::size_t count);
  void EvaluateCorner(const Uv& parameter);
  void InitSurfaces(Index face);
  void AddTriangles(Index face, const FaceTessellation& tessellation);

  const Surface* surface_;
  const far::TopologyLevel* base_;
  SurfaceFactory factory_;
  Varying varying_;
  FaceSurfaces surfaces_;
  std::vector<std::uint32_t> edge_rates_;
  std::vector<std::uint32_t> vertex_points_;
  std::vector<EdgePoints> edge_points_;
  // The positions of the points at the frame, in world space.
  std::vector<device::Float3> frame_;
  std::vector<Point3> world_;
  // The longest triangle edge of each face, in pixels.
  std::vector<double> face_longest_;
  Refinement out_;
};

double Refiner::Target(const FaceSurface& surface) const {
  // A triangle of a quad's tessellation has its diagonal for an edge.
  const double rate = surface_->dicing.rate;
  return surface.GetParameterization().GetType() == bfr::Parameterization::TRI
             ? rate
             : rate / std::numbers::sqrt2;
}

double Refiner::EdgePixels(int edge) {
  const Dicing& dicing = surface_->dicing;
  const bfr::Parameterization parameterization =
      surfaces_.vertex.GetParameterization();
  double pixels = 0.0;
  Point3 previous{};
  for (int sample = 0; sample <= kEdgeSamples; ++sample) {
    Uv parameter{};
    parameterization.GetEdgeCoord(
        edge, static_cast<float>(sample) / static_cast<float>(kEdgeSamples),
        parameter.data());
    surfaces_.vertex_stencil.At(surfaces_.vertex, parameter);
    const Point3 world =
        ToWorld(dicing.viewpoint.object_to_world,
                surfaces_.vertex_stencil.Position(surface_->cage.points));
    if (sample > 0) {
      pixels +=
          PixelLength(dicing.scale, dicing.viewpoint.eye, previous, world);
    }
    previous = world;
  }
  return pixels;
}

void Refiner::RateFaceEdges(Index face) {
  if (!factory_.InitVertexSurface(face, &surfaces_.vertex)) {
    return;
  }
  surfaces_.vertex_stencil.Bind(surfaces_.vertex);
  const double target = Target(surfaces_.vertex);
  const ConstIndexArray edges = base_->GetFaceEdges(face);
  int edge = 0;
  for (const Index index : edges) {
    std::uint32_t& rate = edge_rates_.at(static_cast<std::size_t>(index));
    rate = std::max(rate, EdgeRate(EdgePixels(edge), target));
    ++edge;
  }
}

void Refiner::RateEdges() {
  for (Index face = 0; face < base_->GetNumFaces(); ++face) {
    if (factory_.FaceHasLimitSurface(face)) {
      RateFaceEdges(face);
    }
  }
}

void Refiner::Reset() {
  const std::size_t samples =
      std::max<std::size_t>(surface_->cage.point_motion.size(), 1);
  out_ = {};
  out_.points.resize(samples);
  out_.point_values.resize(surface_->point_values.size());
  out_.face_varying.resize(surface_->face_varying.size());
  vertex_points_.assign(static_cast<std::size_t>(base_->GetNumVertices()),
                        kNoPoint);
  edge_points_.assign(static_cast<std::size_t>(base_->GetNumEdges()), {});
  frame_.clear();
  world_.clear();
  face_longest_.assign(static_cast<std::size_t>(base_->GetNumFaces()), 0.0);
}

void Refiner::Tessellate() {
  Reset();
  for (Index face = 0; face < base_->GetNumFaces(); ++face) {
    if (factory_.FaceHasLimitSurface(face)) {
      TessellateFace(face);
    }
  }
  out_.longest_edge = std::ranges::fold_left(
      face_longest_, 0.0,
      [](double longest, double length) { return std::max(longest, length); });
}

std::vector<int> Refiner::FaceRates(Index face,
                                    const FaceSurface& surface) const {
  const ConstIndexArray edges = base_->GetFaceEdges(face);
  std::vector<int> rates;
  rates.reserve(static_cast<std::size_t>(edges.size()) + 2);
  for (const Index edge : edges) {
    rates.push_back(
        static_cast<int>(edge_rates_.at(static_cast<std::size_t>(edge))));
  }
  // Inner rates as fine as the finest edge across: the average OpenSubdiv
  // infers would leave the inner triangles longer than the edges'.
  if (surface.GetParameterization().GetType() == bfr::Parameterization::QUAD) {
    const int across_u = std::max(rates.at(0), rates.at(2));
    const int across_v = std::max(rates.at(1), rates.at(3));
    rates.push_back(across_u);
    rates.push_back(across_v);
  } else {
    rates.push_back(std::ranges::max(rates));
  }
  return rates;
}

std::uint32_t Refiner::NewPoint(std::size_t coord,
                                FaceTessellation& tessellation) {
  const auto point =
      static_cast<std::uint32_t>(frame_.size() + tessellation.made.size());
  tessellation.made.push_back(coord);
  return point;
}

void Refiner::AssignEdgePoints(Index edge, Index from, std::size_t count,
                               FaceTessellation& tessellation) {
  EdgePoints& shared = edge_points_.at(static_cast<std::size_t>(edge));
  const std::size_t coord = tessellation.points.size();
  if (shared.first == kNoPoint) {
    shared.first =
        static_cast<std::uint32_t>(frame_.size() + tessellation.made.size());
    shared.from = from;
    for (std::size_t along = 0; along < count; ++along) {
      tessellation.points.push_back(NewPoint(coord + along, tessellation));
    }
    return;
  }
  // A face on the other side of the edge walks it the other way.
  const bool reversed = shared.from != from;
  for (std::size_t along = 0; along < count; ++along) {
    const std::size_t offset = reversed ? count - 1 - along : along;
    tessellation.points.push_back(shared.first +
                                  static_cast<std::uint32_t>(offset));
  }
}

void Refiner::AssignPoints(Index face, const bfr::Tessellation& pattern,
                           FaceTessellation& tessellation) {
  const ConstIndexArray vertices = base_->GetFaceVertices(face);
  const ConstIndexArray edges = base_->GetFaceEdges(face);
  tessellation.points.clear();
  tessellation.made.clear();
  int corner = 0;
  for (const auto [vertex, edge] : std::views::zip(vertices, edges)) {
    std::uint32_t& shared = vertex_points_.at(static_cast<std::size_t>(vertex));
    if (shared == kNoPoint) {
      shared = NewPoint(tessellation.points.size(), tessellation);
    }
    tessellation.points.push_back(shared);
    AssignEdgePoints(edge, vertex,
                     static_cast<std::size_t>(pattern.GetNumEdgeCoords(corner)),
                     tessellation);
    ++corner;
  }
  const auto coords = static_cast<std::size_t>(pattern.GetNumCoords());
  for (std::size_t coord = tessellation.points.size(); coord < coords;
       ++coord) {
    tessellation.points.push_back(NewPoint(coord, tessellation));
  }
}

void Refiner::InitSurfaces(Index face) {
  surfaces_.vertex_stencil.Bind(surfaces_.vertex);
  if (varying_ == Varying::kPresent &&
      factory_.InitVaryingSurface(face, &surfaces_.varying)) {
    surfaces_.varying_stencil.Bind(surfaces_.varying);
  }
  for (std::size_t channel = 0; channel < surfaces_.face_varying.size();
       ++channel) {
    FaceSurface& surface = surfaces_.face_varying.at(channel);
    if (factory_.InitFaceVaryingSurface(
            face, &surface, static_cast<SurfaceFactory::FVarID>(channel))) {
      surfaces_.face_varying_stencils.at(channel).Bind(surface);
    }
  }
}

void Refiner::EvaluatePoint(std::uint32_t point, const Uv& parameter) {
  if (surfaces_.varying.IsValid()) {
    surfaces_.varying_stencil.At(surfaces_.varying, parameter);
  }
  const Cage& cage = surface_->cage;
  const Stencil& stencil = surfaces_.vertex_stencil;
  const device::Float3 frame = stencil.Position(cage.points);
  frame_.at(point) = frame;
  world_.at(point) = ToWorld(surface_->dicing.viewpoint.object_to_world, frame);
  if (cage.point_motion.empty()) {
    out_.points.front().at(point) = frame;
  }
  for (std::size_t sample = 0; sample < cage.point_motion.size(); ++sample) {
    out_.points.at(sample).at(point) =
        stencil.Position(ElementOf(cage.point_motion, sample));
  }
  for (std::size_t index = 0; index < surface_->point_values.size(); ++index) {
    const PointValues& values = ElementOf(surface_->point_values, index);
    const Stencil& weights = values.smoothness == Smoothness::kLinear
                                 ? surfaces_.varying_stencil
                                 : stencil;
    Append(weights.Weigh(values.values, values.components), values.components,
           out_.point_values.at(index));
  }
}

void Refiner::EvaluateCorner(const Uv& parameter) {
  const Vector3 normal = surfaces_.vertex_stencil.Normal(surface_->cage.points);
  out_.normals.insert(out_.normals.end(), {normal.x, normal.y, normal.z});
  for (std::size_t channel = 0; channel < surfaces_.face_varying.size();
       ++channel) {
    const FaceVaryingValues& values =
        ElementOf(surface_->face_varying, channel);
    std::vector<float>& out = out_.face_varying.at(channel);
    const FaceSurface& surface = surfaces_.face_varying.at(channel);
    if (!surface.IsValid()) {
      out.resize(out.size() + values.components, 0.0F);
      continue;
    }
    Stencil& stencil = surfaces_.face_varying_stencils.at(channel);
    stencil.At(surface, parameter);
    Append(stencil.Weigh(values.values, values.components), values.components,
           out);
  }
}

void Refiner::GrowPoints(std::size_t count) {
  const std::size_t total = frame_.size() + count;
  frame_.resize(total);
  world_.resize(total);
  for (std::vector<device::Float3>& sample : out_.points) {
    sample.resize(total);
  }
}

void Refiner::Evaluate(Index face, const FaceTessellation& tessellation) {
  InitSurfaces(face);
  GrowPoints(tessellation.made.size());
  std::vector<std::uint8_t> made(tessellation.points.size(), 0);
  for (const std::size_t coord : tessellation.made) {
    made.at(coord) = 1;
  }
  for (std::size_t coord = 0; coord < tessellation.points.size(); ++coord) {
    const Uv parameter{
        tessellation.coords.at(coord * 2),
        tessellation.coords.at((coord * 2) + 1),
    };
    surfaces_.vertex_stencil.At(surfaces_.vertex, parameter);
    if (made.at(coord) != 0) {
      EvaluatePoint(tessellation.points.at(coord), parameter);
    }
    EvaluateCorner(parameter);
  }
}

void Refiner::AddTriangles(Index face, const FaceTessellation& tessellation) {
  const Dicing& dicing = surface_->dicing;
  const auto first_corner = static_cast<std::uint32_t>(
      (out_.normals.size() / 3) - tessellation.points.size());
  double& longest = face_longest_.at(static_cast<std::size_t>(face));
  for (std::size_t facet = 0; facet + 2 < tessellation.facets.size();
       facet += 3) {
    device::Triangle triangle{};
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
      const auto coord =
          static_cast<std::size_t>(tessellation.facets.at(facet + vertex));
      triangle.at(vertex) = tessellation.points.at(coord);
      out_.corners.push_back(first_corner + static_cast<std::uint32_t>(coord));
    }
    for (std::size_t edge = 0; edge < 3; ++edge) {
      longest = std::max(longest,
                         PixelLength(dicing.scale, dicing.viewpoint.eye,
                                     world_.at(triangle.at(edge)),
                                     world_.at(triangle.at((edge + 1) % 3))));
    }
    out_.triangles.push_back(triangle);
    out_.source_faces.push_back(static_cast<std::uint32_t>(face));
  }
}

void Refiner::TessellateFace(Index face) {
  if (!factory_.InitVertexSurface(face, &surfaces_.vertex)) {
    return;
  }
  const std::vector<int> rates = FaceRates(face, surfaces_.vertex);
  bfr::Tessellation::Options options;
  options.SetFacetSize(3);
  const bfr::Tessellation pattern(surfaces_.vertex.GetParameterization(),
                                  static_cast<int>(rates.size()), rates.data(),
                                  options);
  if (!pattern.IsValid()) {
    return;
  }
  FaceTessellation tessellation;
  tessellation.coords.resize(static_cast<std::size_t>(pattern.GetNumCoords()) *
                             2);
  static_cast<void>(pattern.GetCoords(tessellation.coords.data()));
  tessellation.facets.resize(static_cast<std::size_t>(pattern.GetNumFacets()) *
                             3);
  static_cast<void>(pattern.GetFacets(tessellation.facets.data()));
  AssignPoints(face, pattern, tessellation);
  Evaluate(face, tessellation);
  AddTriangles(face, tessellation);
}

std::size_t Refiner::CutFurther() {
  const double rate = surface_->dicing.rate;
  std::size_t cut = 0;
  for (Index face = 0; face < base_->GetNumFaces(); ++face) {
    const double longest = face_longest_.at(static_cast<std::size_t>(face));
    if (longest <= rate) {
      continue;
    }
    for (const Index edge : base_->GetFaceEdges(face)) {
      std::uint32_t& edge_rate = edge_rates_.at(static_cast<std::size_t>(edge));
      const std::uint32_t finer =
          EdgeRate(static_cast<double>(edge_rate) * longest, rate);
      cut += finer > edge_rate ? 1U : 0U;
      edge_rate = std::max(edge_rate, finer);
    }
  }
  return cut;
}

}  // namespace

std::expected<Refinement, std::string> Refine(const Surface& surface) {
  auto topology = CreateRefiner(surface);
  if (!topology.has_value()) {
    return std::unexpected(std::move(topology.error()));
  }
  Refiner refiner(surface, **topology);
  refiner.RateEdges();
  refiner.Tessellate();
  for (int rounds = 1; rounds < kMaxRounds && refiner.CutFurther() > 0;
       ++rounds) {
    refiner.Tessellate();
  }
  return refiner.Take();
}

}  // namespace umbra::delegate::internal::subdivision
