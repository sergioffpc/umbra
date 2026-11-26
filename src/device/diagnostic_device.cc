// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/diagnostic_device.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::device {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

using Checked = std::expected<void, Error>;
using Digest = std::uint64_t;

// A 64-bit FNV-1a hash of what it is given, in order.
class Hasher {
 public:
  // Hashes the number of `values`, then their bytes: those of a type with
  // no padding, or Float3's, which is always zero.
  template <typename T>
  void Span(std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>);
    Value(values.size());
    Raw(std::as_bytes(values));
  }

  template <typename T>
  void Value(const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    Raw(std::as_bytes(std::span<const T, 1>(&value, 1)));
  }

  void Text(std::string_view text) {
    Span(std::span<const char>(text.data(), text.size()));
  }

  [[nodiscard]] Digest digest() const { return digest_; }

 private:
  static constexpr Digest kOffset = 14695981039346656037ULL;
  static constexpr Digest kPrime = 1099511628211ULL;

  void Raw(std::span<const std::byte> bytes) {
    for (const std::byte byte : bytes) {
      digest_ = (digest_ ^ static_cast<Digest>(byte)) * kPrime;
    }
  }

  Digest digest_ = kOffset;
};

// What the device keeps of a prototype: its path, the sizes its updates are
// checked against, its materials, and a digest of each part an update
// replaces.
struct PrototypeRecord {
  std::string path;
  std::size_t points = 0;
  std::size_t faces = 0;
  std::size_t triangles = 0;
  std::vector<MaterialId> materials;
  Digest placement = 0;
  Digest point_values = 0;
  Digest topology = 0;
  Digest primvars = 0;
  Digest triangle_materials = 0;
};

// What the device keeps of a light: its path, the index of its shape, its
// texture, a dome's portals, and a digest of the rest.
struct LightRecord {
  std::string path;
  std::size_t shape = 0;
  std::optional<TextureId> texture;
  std::vector<LightId> portals;
  Digest digest = 0;
};

// The index of a portal in Light::Shape.
constexpr std::size_t kPortalShape =
    Light::Shape(std::in_place_type<Light::Portal>).index();

// What the device keeps of a material: its path, the textures its
// parameters read in order, and a digest of the rest.
struct MaterialRecord {
  std::string path;
  std::vector<TextureId> textures;
  Digest digest = 0;
};

struct CameraRecord {
  std::string path;
  Digest digest = 0;
};

struct TextureRecord {
  std::string file;
  std::string color_space;
};

// The prims of one kind the device holds, by id: none for an id it does not
// hold.
template <typename Record>
using Table = std::vector<std::optional<Record>>;

// The error of an inconsistency of the change set.
std::unexpected<Error> Inconsistent(std::string message) {
  return std::unexpected(Error(ErrorCode::kInternal, std::move(message)));
}

// Whether `numbers` starts at a multiple of kBufferAlignment; an empty array
// starts nowhere.
template <typename T>
bool IsAligned(std::span<const T> numbers) {
  return numbers.empty() ||
         std::bit_cast<std::uintptr_t>(numbers.data()) % kBufferAlignment == 0;
}

// Fails, naming `owner` and `what`, when `numbers` is not aligned.
template <typename T>
Checked CheckAligned(std::span<const T> numbers, std::string_view owner,
                     std::string_view what) {
  if (!IsAligned(numbers)) {
    return Inconsistent(std::format("{}: its {} are not aligned to {} bytes",
                                    owner, what, kBufferAlignment));
  }
  return {};
}

// Whether `table` holds the id `index`.
template <typename Record>
bool IsHeld(const Table<Record>& table, std::uint32_t index) {
  return index < table.size() && table.at(index).has_value();
}

// The record at `index` in `table`, or null when none is held there.
template <typename Record>
Record* UMBRA_NULLABLE Find(Table<Record>& table, std::uint32_t index) {
  if (index >= table.size()) {
    return nullptr;
  }
  std::optional<Record>& held = table.at(index);
  return held.has_value() ? &*held : nullptr;
}

template <typename Record>
const Record* UMBRA_NULLABLE Find(const Table<Record>& table,
                                  std::uint32_t index) {
  if (index >= table.size()) {
    return nullptr;
  }
  const std::optional<Record>& held = table.at(index);
  return held.has_value() ? &*held : nullptr;
}

// The ids a change set gives one kind.
struct KindIds {
  std::vector<std::uint32_t> added;
  std::vector<std::uint32_t> updated;
  std::vector<std::uint32_t> removed;
};

// The ids of `changes`.
template <typename View, typename Id>
KindIds IdsOf(const Changes<View, Id>& changes) {
  KindIds ids;
  for (const View& view : changes.added) {
    ids.added.push_back(std::to_underlying(view.id));
  }
  for (const View& view : changes.updated) {
    ids.updated.push_back(std::to_underlying(view.id));
  }
  for (const Id removed : changes.removed) {
    ids.removed.push_back(std::to_underlying(removed));
  }
  return ids;
}

KindIds IdsOf(const PrototypeChanges& changes) {
  KindIds ids;
  for (const Prototype& prototype : changes.added) {
    ids.added.push_back(std::to_underlying(prototype.id));
  }
  for (const UpdatedPrototype& updated : changes.updated) {
    ids.updated.push_back(std::to_underlying(updated.prototype.id));
  }
  for (const PrototypeId removed : changes.removed) {
    ids.removed.push_back(std::to_underlying(removed));
  }
  return ids;
}

// The error of the id `index` of `kind`, not below `count`.
std::unexpected<Error> OutOfRange(std::string_view kind, std::uint32_t index,
                                  std::uint32_t count) {
  return Inconsistent(std::format("{} {}: its id is not below their count, {}",
                                  kind, index, count));
}

// Drops the ids `removed` from `table`, which must hold them.
template <typename Record>
Checked Remove(std::string_view kind, std::uint32_t count,
               std::span<const std::uint32_t> removed, Table<Record>& table) {
  for (const std::uint32_t index : removed) {
    if (index >= count) {
      return OutOfRange(kind, index, count);
    }
    if (!IsHeld(table, index)) {
      return Inconsistent(
          std::format("{} {}: removed, and not held", kind, index));
    }
    table.at(index).reset();
  }
  return {};
}

// Checks that `table` holds the ids `updated`, once.
template <typename Record>
Checked CheckUpdated(std::string_view kind, std::uint32_t count,
                     std::span<const std::uint32_t> updated,
                     const Table<Record>& table) {
  std::vector<std::uint8_t> seen(count, 0);
  for (const std::uint32_t index : updated) {
    if (index >= count) {
      return OutOfRange(kind, index, count);
    }
    if (!IsHeld(table, index)) {
      return Inconsistent(
          std::format("{} {}: updated, and not held or removed", kind, index));
    }
    if (seen.at(index) != 0) {
      return Inconsistent(std::format("{} {}: updated twice", kind, index));
    }
    seen.at(index) = 1;
  }
  return {};
}

// Holds the ids `added` in `table`, which must not hold them yet, with an
// empty record each.
template <typename Record>
Checked Add(std::string_view kind, std::uint32_t count,
            std::span<const std::uint32_t> added, Table<Record>& table) {
  for (const std::uint32_t index : added) {
    if (index >= count) {
      return OutOfRange(kind, index, count);
    }
    if (IsHeld(table, index)) {
      return Inconsistent(
          std::format("{} {}: added, and already held", kind, index));
    }
    table.at(index).emplace();
  }
  return {};
}

// Checks that `table` holds no id not below `count`.
template <typename Record>
Checked CheckBelow(std::string_view kind, std::uint32_t count,
                   const Table<Record>& table) {
  for (std::size_t index = count; index < table.size(); ++index) {
    if (table.at(index).has_value()) {
      return Inconsistent(std::format(
          "{} {}: still held, and its id is not below their count, {}", kind,
          index, count));
    }
  }
  return {};
}

// Applies `ids` to `table`: its removals, then its additions. Fails when an
// id is not below `count`, an id is removed or updated that `table` does not
// hold, or added that it does, or an id not below `count` is still held once
// they are applied.
template <typename Record>
std::expected<Table<Record>, Error> ApplyIds(std::string_view kind,
                                             std::uint32_t count,
                                             const KindIds& ids,
                                             Table<Record> table) {
  if (table.size() < count) {
    table.resize(count);
  }
  if (auto applied =
          Remove(kind, count, ids.removed, table)
              .and_then(
                  [&] { return CheckUpdated(kind, count, ids.updated, table); })
              .and_then([&] { return Add(kind, count, ids.added, table); })
              .and_then([&] { return CheckBelow(kind, count, table); });
      !applied.has_value()) {
    return std::unexpected(std::move(applied.error()));
  }
  table.resize(count);
  return table;
}

// Applies `ids` to `table`, which keeps what it held when they do not
// apply.
template <typename Record>
Checked Apply(std::string_view kind, std::uint32_t count, const KindIds& ids,
              Table<Record>& table) {
  auto applied = ApplyIds(kind, count, ids, table);
  if (!applied.has_value()) {
    return std::unexpected(std::move(applied.error()));
  }
  table = std::move(*applied);
  return {};
}

// What the primvars of a mesh are counted against.
struct MeshShape {
  std::size_t points = 0;
  std::size_t faces = 0;
  std::size_t triangles = 0;
};

// The values a primvar at `rate` needs on `shape`.
std::size_t Needed(PrimvarRate rate, const MeshShape& shape) {
  switch (rate) {
    case PrimvarRate::kConstant:
      return 1;
    case PrimvarRate::kPerFace:
      return shape.faces;
    case PrimvarRate::kPerVertex:
      return shape.points;
    case PrimvarRate::kPerCorner:
      return 0;
  }
  return 0;
}

// Checks the indices of a per-corner primvar of `count` values.
Checked CheckCorners(const Primvar& primvar, std::size_t count,
                     const MeshShape& shape, std::string_view owner) {
  if (primvar.indices.size() != shape.triangles * 3) {
    return Inconsistent(std::format(
        "{}: primvar {} has {} corner indices, for {} triangles", owner,
        primvar.name, primvar.indices.size(), shape.triangles));
  }
  const auto bad = std::ranges::find_if(
      primvar.indices, [count](std::uint32_t index) { return index >= count; });
  if (bad != primvar.indices.end()) {
    return Inconsistent(std::format("{}: primvar {} indexes value {}, of {}",
                                    owner, primvar.name, *bad, count));
  }
  return {};
}

// Checks the numbers of a primvar, named `what`: floats or integers, a
// whole number of values, aligned.
Checked CheckNumbers(const Primvar& primvar, std::string_view owner,
                     std::string_view what) {
  if (primvar.components == 0) {
    return Inconsistent(std::format("{}: {} has no components", owner, what));
  }
  if (!primvar.floats.empty() && !primvar.integers.empty()) {
    return Inconsistent(
        std::format("{}: {} has both floats and integers", owner, what));
  }
  const std::size_t numbers = primvar.floats.size() + primvar.integers.size();
  if (numbers % primvar.components != 0) {
    return Inconsistent(
        std::format("{}: {} has {} numbers, not a multiple of its {} "
                    "components",
                    owner, what, numbers, primvar.components));
  }
  return CheckAligned(primvar.floats, owner, std::format("{} floats", what))
      .and_then([&] {
        return CheckAligned(primvar.integers, owner,
                            std::format("{} integers", what));
      })
      .and_then([&] {
        return CheckAligned(primvar.indices, owner,
                            std::format("{} indices", what));
      });
}

// Checks that a primvar, named `what`, has as many values as its rate needs
// on `shape`.
Checked CheckCount(const Primvar& primvar, const MeshShape& shape,
                   std::string_view owner, std::string_view what) {
  const std::size_t count = primvar.Count();
  if (primvar.rate == PrimvarRate::kPerCorner) {
    return CheckCorners(primvar, count, shape, owner);
  }
  if (!primvar.indices.empty()) {
    return Inconsistent(
        std::format("{}: {} has indices, and is not per corner", owner, what));
  }
  const std::size_t needed = Needed(primvar.rate, shape);
  const bool matches = primvar.rate == PrimvarRate::kConstant ? count >= needed
                                                              : count == needed;
  if (!matches) {
    return Inconsistent(std::format("{}: {} has {} values, {} needed", owner,
                                    what, count, needed));
  }
  return {};
}

// Checks a primvar of a mesh of `shape`, or of an instance, which is
// constant.
Checked CheckPrimvar(const Primvar& primvar, const MeshShape& shape,
                     std::string_view owner) {
  const std::string what = std::format("primvar {}", primvar.name);
  return CheckNumbers(primvar, owner, what).and_then([&] {
    return CheckCount(primvar, shape, owner, what);
  });
}

// Checks each of `primvars`, of a mesh of `shape`.
Checked CheckPrimvars(std::span<const Primvar> primvars, const MeshShape& shape,
                      std::string_view owner) {
  for (const Primvar& primvar : primvars) {
    if (auto checked = CheckPrimvar(primvar, shape, owner);
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks an instance, named `name`: a transform per motion sample, aligned,
// and constant primvars.
Checked CheckInstance(const Instance& instance, std::string_view name) {
  if (instance.object_to_world.empty()) {
    return Inconsistent(std::format("{}: no transform", name));
  }
  if (auto aligned = CheckAligned(instance.object_to_world, name, "transforms");
      !aligned.has_value()) {
    return aligned;
  }
  for (const Primvar& primvar : instance.primvars) {
    if (primvar.rate != PrimvarRate::kConstant) {
      return Inconsistent(
          std::format("{}: primvar {} is not constant", name, primvar.name));
    }
  }
  return CheckPrimvars(instance.primvars, {}, name);
}

// Checks the placements of `owner`: at least one, each valid.
Checked CheckInstances(std::span<const Instance> instances,
                       std::string_view owner) {
  if (instances.empty()) {
    return Inconsistent(std::format("{}: no instance", owner));
  }
  std::size_t index = 0;
  for (const Instance& instance : instances) {
    if (auto checked = CheckInstance(
            instance, std::format("{}, instance {}", owner, index++));
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks the points of a prototype: one array or more, as long as each
// other, aligned.
Checked CheckPoints(const Prototype& prototype, std::string_view owner) {
  if (prototype.points.empty()) {
    return Inconsistent(std::format("{}: no points", owner));
  }
  const std::size_t count = prototype.points.front().size();
  for (const std::span<const Float3> sample : prototype.points) {
    if (sample.size() != count) {
      return Inconsistent(std::format("{}: motion samples of {} and {} points",
                                      owner, count, sample.size()));
    }
    if (auto aligned = CheckAligned(sample, owner, "points");
        !aligned.has_value()) {
      return aligned;
    }
  }
  return {};
}

// Checks the triangles of a prototype and their source faces.
Checked CheckTriangles(const Prototype& prototype, std::string_view owner) {
  const std::size_t points = prototype.points.front().size();
  for (const Triangle& triangle : prototype.triangles) {
    if (std::ranges::any_of(triangle, [points](std::uint32_t index) {
          return index >= points;
        })) {
      return Inconsistent(std::format(
          "{}: a triangle indexes a point not below {}", owner, points));
    }
  }
  if (prototype.source_faces.size() != prototype.triangles.size()) {
    return Inconsistent(std::format("{}: {} source faces, for {} triangles",
                                    owner, prototype.source_faces.size(),
                                    prototype.triangles.size()));
  }
  const std::size_t faces = prototype.face_vertex_counts.size();
  if (std::ranges::any_of(prototype.source_faces, [faces](std::uint32_t face) {
        return face >= faces;
      })) {
    return Inconsistent(
        std::format("{}: a source face not below {}", owner, faces));
  }
  return CheckAligned(prototype.triangles, owner, "triangles")
      .and_then([&] {
        return CheckAligned(prototype.source_faces, owner, "source faces");
      })
      .and_then([&] {
        return CheckAligned(prototype.face_vertex_counts, owner,
                            "face vertex counts");
      });
}

// Checks the materials of a prototype of `triangles` triangles: one or
// more, each held, and those of its triangles.
Checked CheckMaterials(const Prototype& prototype, std::size_t triangles,
                       const Table<MaterialRecord>& materials,
                       std::string_view owner) {
  if (prototype.materials.empty()) {
    return Inconsistent(std::format("{}: no material", owner));
  }
  for (const MaterialId material : prototype.materials) {
    if (!IsHeld(materials, std::to_underlying(material))) {
      return Inconsistent(std::format("{}: material {} is not held", owner,
                                      std::to_underlying(material)));
    }
  }
  const std::span<const std::uint32_t> per_triangle =
      prototype.triangle_materials;
  if (!per_triangle.empty() && per_triangle.size() != triangles) {
    return Inconsistent(
        std::format("{}: {} triangle materials, for {} triangles", owner,
                    per_triangle.size(), triangles));
  }
  const std::size_t count = prototype.materials.size();
  if (std::ranges::any_of(per_triangle, [count](std::uint32_t index) {
        return index >= count;
      })) {
    return Inconsistent(
        std::format("{}: a triangle's material not below {}", owner, count));
  }
  return CheckAligned(prototype.materials, owner, "materials").and_then([&] {
    return CheckAligned(per_triangle, owner, "triangle materials");
  });
}

// Checks the normals of a prototype of `shape`: three floats at a rate but
// constant.
Checked CheckNormals(const Primvar& normals, const MeshShape& shape,
                     std::string_view owner) {
  if (normals.rate == PrimvarRate::kConstant || normals.components != 3 ||
      !normals.integers.empty()) {
    return Inconsistent(std::format(
        "{}: its normals are not three floats at a rate but constant", owner));
  }
  return CheckPrimvar(normals, shape, owner);
}

// "prototype 3 (/World/Car/Body)".
std::string PrototypeName(const Prototype& prototype) {
  return std::format("prototype {} ({})", std::to_underlying(prototype.id),
                     prototype.path);
}

// The shape of a prototype whose points and triangles are given.
MeshShape ShapeOf(const Prototype& prototype) {
  return {
      .points = prototype.points.front().size(),
      .faces = prototype.face_vertex_counts.size(),
      .triangles = prototype.triangles.size(),
  };
}

// Checks a prototype added.
Checked CheckPrototype(const Prototype& prototype,
                       const Table<MaterialRecord>& materials) {
  const std::string owner = PrototypeName(prototype);
  if (auto points = CheckPoints(prototype, owner); !points.has_value()) {
    return points;
  }
  const MeshShape shape = ShapeOf(prototype);
  return CheckTriangles(prototype, owner)
      .and_then([&] { return CheckNormals(prototype.normals, shape, owner); })
      .and_then([&] { return CheckPrimvars(prototype.primvars, shape, owner); })
      .and_then([&] {
        return CheckMaterials(prototype, shape.triangles, materials, owner);
      })
      .and_then([&] { return CheckInstances(prototype.instances, owner); });
}

// Whether `primvar` gives nothing.
bool IsEmpty(const Primvar& primvar) {
  return primvar.floats.empty() && primvar.integers.empty() &&
         primvar.indices.empty();
}

// A member of an updated prototype, with the flags that name it.
struct Member {
  std::string_view name;
  ChangeFlags flags;
};

// The members of an updated prototype, in the order MemberSizes gives them.
constexpr std::array<Member, 9> kMembers = {
    Member{.name = "instances", .flags = ChangeFlags::kPlacement},
    Member{
        .name = "points",
        .flags = ChangeFlags::kPoints | ChangeFlags::kTopology,
    },
    Member{.name = "triangles", .flags = ChangeFlags::kTopology},
    Member{.name = "source faces", .flags = ChangeFlags::kTopology},
    Member{.name = "face vertex counts", .flags = ChangeFlags::kTopology},
    Member{.name = "normals", .flags = ChangeFlags::kPrimvars},
    Member{.name = "primvars", .flags = ChangeFlags::kPrimvars},
    Member{.name = "materials", .flags = ChangeFlags::kMaterials},
    Member{.name = "triangle materials", .flags = ChangeFlags::kMaterials},
};

// How many elements `prototype` gives of each of kMembers.
std::array<std::size_t, kMembers.size()> MemberSizes(
    const Prototype& prototype) {
  return {
      prototype.instances.size(),          prototype.points.size(),
      prototype.triangles.size(),          prototype.source_faces.size(),
      prototype.face_vertex_counts.size(), IsEmpty(prototype.normals) ? 0U : 1U,
      prototype.primvars.size(),           prototype.materials.size(),
      prototype.triangle_materials.size(),
  };
}

// Checks that an updated prototype gives only the members its flags name.
Checked CheckOnlyNamed(const UpdatedPrototype& updated,
                       std::string_view owner) {
  for (const auto& [member, given] :
       std::views::zip(kMembers, MemberSizes(updated.prototype))) {
    if (given > 0 && !HasAny(updated.changes, member.flags)) {
      return Inconsistent(
          std::format("{}: updated with its {}, which its flags do not name",
                      owner, member.name));
    }
  }
  return {};
}

// Checks the flags of an updated prototype: some, kTopology with kPrimvars
// and kMaterials, and not with kPoints.
Checked CheckFlags(ChangeFlags flags, std::string_view owner) {
  if (flags == ChangeFlags::kNone) {
    return Inconsistent(std::format("{}: updated with no change", owner));
  }
  if (!HasAny(flags, ChangeFlags::kTopology)) {
    return {};
  }
  if (HasAny(flags, ChangeFlags::kPoints) ||
      !HasAny(flags, ChangeFlags::kPrimvars) ||
      !HasAny(flags, ChangeFlags::kMaterials)) {
    return Inconsistent(
        std::format("{}: its topology changed, without its primvars and its "
                    "materials, or with its points alone",
                    owner));
  }
  return {};
}

// Checks the points and triangles of an updated prototype against `held`,
// and gives its shape once updated.
std::expected<MeshShape, Error> CheckShape(const UpdatedPrototype& updated,
                                           const PrototypeRecord& held,
                                           std::string_view owner) {
  const Prototype& prototype = updated.prototype;
  if (HasAny(updated.changes, ChangeFlags::kTopology)) {
    if (auto checked = CheckPoints(prototype, owner).and_then([&] {
          return CheckTriangles(prototype, owner);
        });
        !checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    return ShapeOf(prototype);
  }
  const MeshShape shape{
      .points = held.points,
      .faces = held.faces,
      .triangles = held.triangles,
  };
  if (HasAny(updated.changes, ChangeFlags::kPoints)) {
    if (auto points = CheckPoints(prototype, owner); !points.has_value()) {
      return std::unexpected(std::move(points.error()));
    }
    if (prototype.points.front().size() != held.points) {
      return std::unexpected(Error(
          ErrorCode::kInternal,
          std::format("{}: its points changed from {} to {}, and its "
                      "topology did not",
                      owner, held.points, prototype.points.front().size())));
    }
  }
  return shape;
}

// Checks the primvars, materials and instances of a prototype updated,
// `shape` once updated, against what the device holds of it.
Checked CheckUpdatedValues(const UpdatedPrototype& updated,
                           const MeshShape& shape, const PrototypeRecord& held,
                           const Table<MaterialRecord>& materials,
                           std::string_view owner) {
  const Prototype& prototype = updated.prototype;
  if (HasAny(updated.changes, ChangeFlags::kPrimvars)) {
    if (auto checked =
            CheckNormals(prototype.normals, shape, owner).and_then([&] {
              return CheckPrimvars(prototype.primvars, shape, owner);
            });
        !checked.has_value()) {
      return checked;
    }
  }
  if (HasAny(updated.changes, ChangeFlags::kMaterials)) {
    if (auto checked =
            CheckMaterials(prototype, shape.triangles, materials, owner);
        !checked.has_value()) {
      return checked;
    }
  } else if (shape.triangles != held.triangles) {
    return Inconsistent(std::format(
        "{}: its triangles changed, and its materials did not", owner));
  }
  if (HasAny(updated.changes, ChangeFlags::kPlacement)) {
    return CheckInstances(prototype.instances, owner);
  }
  return {};
}

// Checks a prototype updated, against what the device holds of it.
Checked CheckUpdate(const UpdatedPrototype& updated,
                    const PrototypeRecord& held,
                    const Table<MaterialRecord>& materials) {
  const Prototype& prototype = updated.prototype;
  const std::string owner = PrototypeName(prototype);
  if (prototype.path != held.path) {
    return Inconsistent(
        std::format("{}: updated, and held as {}", owner, held.path));
  }
  if (auto checked = CheckFlags(updated.changes, owner).and_then([&] {
        return CheckOnlyNamed(updated, owner);
      });
      !checked.has_value()) {
    return checked;
  }
  const auto shape = CheckShape(updated, held, owner);
  if (!shape.has_value()) {
    return std::unexpected(shape.error());
  }
  return CheckUpdatedValues(updated, *shape, held, materials, owner);
}

// Fails, naming `owner`, when `texture` is not held.
Checked CheckTextureHeld(TextureId texture,
                         const Table<TextureRecord>& textures,
                         std::string_view owner) {
  if (!IsHeld(textures, std::to_underlying(texture))) {
    return Inconsistent(std::format("{}: texture {} is not held", owner,
                                    std::to_underlying(texture)));
  }
  return {};
}

// Checks a parameter of a node.
Checked CheckParameter(const Parameter& parameter,
                       const Table<TextureRecord>& textures,
                       std::string_view owner) {
  const std::string what = std::format("parameter {}", parameter.name);
  return std::visit(
      [&]<typename T>(const T& value) -> Checked {
        if constexpr (std::is_same_v<T, TextureId>) {
          return CheckTextureHeld(value, textures, owner);
        } else if constexpr (std::is_same_v<T, std::span<const float>> ||
                             std::is_same_v<T, std::span<const std::int32_t>>) {
          return CheckAligned(value, owner, what);
        } else {
          return {};
        }
      },
      parameter.value);
}

// Checks a node, named `name`, of a material of `nodes` nodes: its inputs
// come from nodes of the material, and its textures are held.
Checked CheckNode(const MaterialNode& node, std::size_t nodes,
                  const Table<TextureRecord>& textures, std::string_view name) {
  for (const Connection& connection : node.connections) {
    if (connection.node >= nodes) {
      return Inconsistent(std::format("{}: input {} comes from node {}, of {}",
                                      name, connection.input, connection.node,
                                      nodes));
    }
  }
  for (const Parameter& parameter : node.parameters) {
    if (auto checked = CheckParameter(parameter, textures, name);
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks a material added or updated.
Checked CheckMaterial(const MaterialNetwork& material,
                      const Table<TextureRecord>& textures) {
  const std::string owner = std::format(
      "material {} ({})", std::to_underlying(material.id), material.path);
  const std::size_t nodes = material.nodes.size();
  if (material.surface.node >= nodes) {
    return Inconsistent(std::format("{}: its surface is node {}, of {}", owner,
                                    material.surface.node, nodes));
  }
  for (const MaterialNode& node : material.nodes) {
    if (auto checked = CheckNode(node, nodes, textures,
                                 std::format("{}, node {}", owner, node.name));
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// The texture of a light's shape, if any.
std::optional<TextureId> TextureOf(const Light::Shape& shape) {
  if (const auto* rect = std::get_if<Light::Rect>(&shape)) {
    return rect->texture;
  }
  if (const auto* dome = std::get_if<Light::Dome>(&shape)) {
    return dome->texture;
  }
  return std::nullopt;
}

// Checks a light added or updated.
Checked CheckLight(const Light& light, const Table<TextureRecord>& textures) {
  const std::string owner =
      std::format("light {} ({})", std::to_underlying(light.id), light.path);
  if (const std::optional<TextureId> texture = TextureOf(light.shape)) {
    if (auto held = CheckTextureHeld(*texture, textures, owner);
        !held.has_value()) {
      return held;
    }
  }
  for (const Instance& instance : light.instances) {
    if (!instance.primvars.empty()) {
      return Inconsistent(std::format("{}: an instance has primvars", owner));
    }
  }
  return CheckInstances(light.instances, owner);
}

// Checks a camera added or updated.
Checked CheckCamera(const Camera& camera) {
  const std::string owner =
      std::format("camera {} ({})", std::to_underlying(camera.id), camera.path);
  if (camera.camera_to_world.empty()) {
    return Inconsistent(std::format("{}: no transform", owner));
  }
  return CheckAligned(camera.camera_to_world, owner, "transforms");
}

// Checks a texture added: an absolute file, with a color space.
Checked CheckTexture(const Texture& texture) {
  const std::string owner = std::format(
      "texture {} ({})", std::to_underlying(texture.id), texture.file);
  if (!texture.file.starts_with('/')) {
    return Inconsistent(std::format("{}: its file is not absolute", owner));
  }
  if (texture.color_space.empty()) {
    return Inconsistent(std::format("{}: no color space", owner));
  }
  return {};
}

// Hashes a primvar into `hasher`.
void HashPrimvar(const Primvar& primvar, Hasher& hasher) {
  hasher.Text(primvar.name);
  hasher.Value(primvar.rate);
  hasher.Value(primvar.components);
  hasher.Span(primvar.floats);
  hasher.Span(primvar.integers);
  hasher.Span(primvar.indices);
}

Digest PlacementDigest(std::span<const Instance> instances) {
  Hasher hasher;
  hasher.Value(instances.size());
  for (const Instance& instance : instances) {
    hasher.Span(instance.object_to_world);
    hasher.Value(instance.primvars.size());
    for (const Primvar& primvar : instance.primvars) {
      HashPrimvar(primvar, hasher);
    }
  }
  return hasher.digest();
}

Digest PointsDigest(const Prototype& prototype) {
  Hasher hasher;
  hasher.Value(prototype.points.size());
  for (const std::span<const Float3> sample : prototype.points) {
    hasher.Span(sample);
  }
  return hasher.digest();
}

Digest TopologyDigest(const Prototype& prototype) {
  Hasher hasher;
  hasher.Span(prototype.triangles);
  hasher.Span(prototype.source_faces);
  hasher.Span(prototype.face_vertex_counts);
  hasher.Value(prototype.sidedness);
  return hasher.digest();
}

Digest PrimvarsDigest(const Prototype& prototype) {
  Hasher hasher;
  HashPrimvar(prototype.normals, hasher);
  hasher.Value(prototype.primvars.size());
  for (const Primvar& primvar : prototype.primvars) {
    HashPrimvar(primvar, hasher);
  }
  return hasher.digest();
}

Digest TriangleMaterialsDigest(const Prototype& prototype) {
  Hasher hasher;
  hasher.Span(prototype.triangle_materials);
  return hasher.digest();
}

// Applies to `record` the members of `prototype` that `changes` names.
void Record(const Prototype& prototype, ChangeFlags changes,
            PrototypeRecord& record) {
  record.path = prototype.path;
  if (HasAny(changes, ChangeFlags::kPlacement)) {
    record.placement = PlacementDigest(prototype.instances);
  }
  if (HasAny(changes, ChangeFlags::kPoints | ChangeFlags::kTopology)) {
    record.points = prototype.points.front().size();
    record.point_values = PointsDigest(prototype);
  }
  if (HasAny(changes, ChangeFlags::kTopology)) {
    record.faces = prototype.face_vertex_counts.size();
    record.triangles = prototype.triangles.size();
    record.topology = TopologyDigest(prototype);
  }
  if (HasAny(changes, ChangeFlags::kPrimvars)) {
    record.primvars = PrimvarsDigest(prototype);
  }
  if (HasAny(changes, ChangeFlags::kMaterials)) {
    record.materials.assign(prototype.materials.begin(),
                            prototype.materials.end());
    record.triangle_materials = TriangleMaterialsDigest(prototype);
  }
}

constexpr ChangeFlags kWhole = ChangeFlags::kPlacement |
                               ChangeFlags::kTopology | ChangeFlags::kPrimvars |
                               ChangeFlags::kMaterials;

// Hashes `texts`, their count first.
void HashTexts(std::span<const std::string_view> texts, Hasher& hasher) {
  hasher.Value(texts.size());
  for (const std::string_view text : texts) {
    hasher.Text(text);
  }
}

// Hashes a parameter's value, a texture as its place in `textures`, which it
// is appended to.
void HashValue(const ParameterValue& value, std::vector<TextureId>& textures,
               Hasher& hasher) {
  hasher.Value(value.index());
  std::visit(
      [&]<typename T>(const T& held) {
        if constexpr (std::is_same_v<T, TextureId>) {
          hasher.Value(textures.size());
          textures.push_back(held);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          hasher.Text(held);
        } else if constexpr (std::is_same_v<T, std::span<const std::int32_t>> ||
                             std::is_same_v<T, std::span<const float>>) {
          hasher.Span(held);
        } else if constexpr (std::is_same_v<
                                 T, std::span<const std::string_view>>) {
          HashTexts(held, hasher);
        } else {
          hasher.Value(held);
        }
      },
      value);
}

MaterialRecord RecordOf(const MaterialNetwork& material) {
  MaterialRecord record{
      .path = std::string(material.path),
      .textures = {},
      .digest = 0,
  };
  Hasher hasher;
  hasher.Value(material.nodes.size());
  for (const MaterialNode& node : material.nodes) {
    hasher.Text(node.name);
    hasher.Value(node.kind);
    hasher.Text(node.shader);
    hasher.Value(node.parameters.size());
    for (const Parameter& parameter : node.parameters) {
      hasher.Text(parameter.name);
      HashValue(parameter.value, record.textures, hasher);
    }
    hasher.Value(node.connections.size());
    for (const Connection& connection : node.connections) {
      hasher.Text(connection.input);
      hasher.Value(connection.node);
      hasher.Text(connection.output);
    }
  }
  hasher.Value(material.surface.node);
  hasher.Text(material.surface.name);
  record.digest = hasher.digest();
  return record;
}

// Hashes a light's shape into `hasher`, but its texture and its portals,
// which are references.
void HashShape(const Light::Shape& light_shape, Hasher& hasher) {
  hasher.Value(light_shape.index());
  std::visit(
      [&]<typename T>(const T& shape) {
        if constexpr (std::is_same_v<T, Light::Sphere> ||
                      std::is_same_v<T, Light::Disk>) {
          hasher.Value(shape.radius);
        } else if constexpr (std::is_same_v<T, Light::Rect> ||
                             std::is_same_v<T, Light::Portal>) {
          hasher.Value(shape.width);
          hasher.Value(shape.height);
        } else if constexpr (std::is_same_v<T, Light::Cylinder>) {
          hasher.Value(shape.length);
          hasher.Value(shape.radius);
        } else if constexpr (std::is_same_v<T, Light::Distant>) {
          hasher.Value(shape.angle);
        } else {
          static_assert(std::is_same_v<T, Light::Dome>);
          hasher.Value(shape.format);
          hasher.Value(shape.orientation);
        }
      },
      light_shape);
}

// The portals of a light's shape: a dome's.
std::vector<LightId> PortalsOf(const Light::Shape& shape) {
  const auto* dome = std::get_if<Light::Dome>(&shape);
  return dome == nullptr
             ? std::vector<LightId>()
             : std::vector<LightId>(dome->portals.begin(), dome->portals.end());
}

LightRecord RecordOf(const Light& light) {
  LightRecord record{
      .path = std::string(light.path),
      .shape = light.shape.index(),
      .texture = TextureOf(light.shape),
      .portals = PortalsOf(light.shape),
      .digest = 0,
  };
  Hasher hasher;
  HashShape(light.shape, hasher);
  hasher.Value(light.intensity);
  hasher.Value(light.exposure);
  hasher.Value(light.normalization);
  hasher.Value(light.color.rgb);
  hasher.Value(light.color_temperature.has_value());
  hasher.Value(light.color_temperature.value_or(0.0F));
  hasher.Value(light.cone_angle);
  record.digest = PlacementDigest(light.instances) ^ hasher.digest();
  return record;
}

CameraRecord RecordOf(const Camera& camera) {
  Hasher hasher;
  hasher.Value(camera.projection);
  hasher.Span(camera.camera_to_world);
  hasher.Value(camera.focal_length);
  hasher.Value(camera.aperture);
  hasher.Value(camera.aperture_offset);
  hasher.Value(camera.clipping_range);
  hasher.Value(camera.f_stop);
  hasher.Value(camera.focus_distance);
  return {.path = std::string(camera.path), .digest = hasher.digest()};
}

TextureRecord RecordOf(const Texture& texture) {
  return {
      .file = std::string(texture.file),
      .color_space = std::string(texture.color_space),
  };
}

// "prototype", "light", ... : the kind of `View`, as messages name it.
template <typename View>
constexpr std::string_view KindName() {
  if constexpr (std::is_same_v<View, Light>) {
    return "light";
  } else if constexpr (std::is_same_v<View, MaterialNetwork>) {
    return "material";
  } else if constexpr (std::is_same_v<View, Camera>) {
    return "camera";
  } else {
    static_assert(std::is_same_v<View, Texture>);
    return "texture";
  }
}

// The flags of `changes`, by name: "placement, points".
std::string FlagNames(ChangeFlags changes) {
  struct FlagName {
    ChangeFlags flag;
    std::string_view name;
  };
  static constexpr std::array<FlagName, 5> kNames = {
      FlagName{.flag = ChangeFlags::kPlacement, .name = "placement"},
      FlagName{.flag = ChangeFlags::kPoints, .name = "points"},
      FlagName{.flag = ChangeFlags::kTopology, .name = "topology"},
      FlagName{.flag = ChangeFlags::kPrimvars, .name = "primvars"},
      FlagName{.flag = ChangeFlags::kMaterials, .name = "materials"},
  };
  std::string names;
  for (const auto& [flag, name] : kNames) {
    if (HasAny(changes, flag)) {
      names += std::format("{}{}", names.empty() ? "" : ", ", name);
    }
  }
  return names;
}

// "1 camera", "12 prototypes".
std::string Counted(std::size_t count, std::string_view noun) {
  return std::format("{} {}{}", core::FormatCount(count), noun,
                     count == 1 ? "" : "s");
}

// The number of prims `table` holds.
template <typename Record>
std::size_t CountHeld(const Table<Record>& table) {
  return static_cast<std::size_t>(std::ranges::count_if(
      table,
      [](const std::optional<Record>& held) { return held.has_value(); }));
}

}  // namespace

// What the device holds of every kind.
struct DiagnosticDevice::State {
  Table<PrototypeRecord> prototypes;
  Table<LightRecord> lights;
  Table<MaterialRecord> materials;
  Table<CameraRecord> cameras;
  Table<TextureRecord> textures;
};

namespace {

using State = DiagnosticDevice::State;

// Applies the ids of `changes` to `state`.
Checked ApplyAll(const ChangeSet& changes, State& state) {
  return Apply("prototype", changes.ids.prototypes, IdsOf(changes.prototypes),
               state.prototypes)
      .and_then([&] {
        return Apply("light", changes.ids.lights, IdsOf(changes.lights),
                     state.lights);
      })
      .and_then([&] {
        return Apply("material", changes.ids.materials,
                     IdsOf(changes.materials), state.materials);
      })
      .and_then([&] {
        return Apply("camera", changes.ids.cameras, IdsOf(changes.cameras),
                     state.cameras);
      })
      .and_then([&] {
        return Apply("texture", changes.ids.textures, IdsOf(changes.textures),
                     state.textures);
      });
}

// Checks each of `views` with `check`, then records it in `table`.
template <typename View, typename Record, typename Check>
Checked CheckAndRecord(std::span<const View> views, const Check& check,
                       Table<Record>& table) {
  for (const View& view : views) {
    if (auto checked = check(view); !checked.has_value()) {
      return checked;
    }
    table.at(std::to_underlying(view.id)) = RecordOf(view);
  }
  return {};
}

// Checks the views of `changes`, added and updated, with `check`, and
// records them in `table`. A texture is never updated.
template <typename View, typename Id, typename Record, typename Check>
Checked CheckKind(const Changes<View, Id>& changes, const Check& check,
                  Table<Record>& table) {
  if constexpr (std::is_same_v<View, Texture>) {
    if (!changes.updated.empty()) {
      return Inconsistent(
          std::format("texture {}: updated, and a texture is never updated",
                      std::to_underlying(changes.updated.front().id)));
    }
  }
  return CheckAndRecord(changes.added, check, table).and_then([&] {
    return CheckAndRecord(changes.updated, check, table);
  });
}

// The record of the prototype `prototype` in `state`, which its ids,
// applied, hold.
std::expected<PrototypeRecord*, Error> RecordFor(const Prototype& prototype,
                                                 State& state) {
  PrototypeRecord* record =
      Find(state.prototypes, std::to_underlying(prototype.id));
  if (record == nullptr) {
    return std::unexpected(Error(
        ErrorCode::kInternal,
        std::format("{}: not held once applied", PrototypeName(prototype))));
  }
  return record;
}

// Checks the prototypes of `changes`, and records them in `state`.
Checked CheckPrototypes(const PrototypeChanges& changes, State& state) {
  for (const Prototype& prototype : changes.added) {
    auto record = CheckPrototype(prototype, state.materials).and_then([&] {
      return RecordFor(prototype, state);
    });
    if (!record.has_value()) {
      return std::unexpected(std::move(record.error()));
    }
    Record(prototype, kWhole, **record);
  }
  for (const UpdatedPrototype& updated : changes.updated) {
    auto held = RecordFor(updated.prototype, state);
    if (!held.has_value()) {
      return std::unexpected(std::move(held.error()));
    }
    if (auto checked = CheckUpdate(updated, **held, state.materials);
        !checked.has_value()) {
      return checked;
    }
    Record(updated.prototype, updated.changes, **held);
  }
  return {};
}

// Checks that `prototype`, at `index`, reads only materials of `materials`.
Checked CheckReadsHeld(const PrototypeRecord& prototype, std::size_t index,
                       const Table<MaterialRecord>& materials) {
  for (const MaterialId material : prototype.materials) {
    if (!IsHeld(materials, std::to_underlying(material))) {
      return Inconsistent(
          std::format("prototype {} ({}): reads material {}, "
                      "not held",
                      index, prototype.path, std::to_underlying(material)));
    }
  }
  return {};
}

// Checks that `light`, at `index`, is guided only by portals of `lights`.
Checked CheckPortalsHeld(const LightRecord& light, std::size_t index,
                         const Table<LightRecord>& lights) {
  for (const LightId portal : light.portals) {
    const LightRecord* held = Find(lights, std::to_underlying(portal));
    if (held == nullptr || held->shape != kPortalShape) {
      return Inconsistent(
          std::format("light {} ({}): its portal {} is not a portal held",
                      index, light.path, std::to_underlying(portal)));
    }
  }
  return {};
}

// Checks that the dome lights `state` holds are guided only by portals it
// holds: a portal removed must not guide a dome kept.
Checked CheckHeldPortals(const State& state) {
  for (std::size_t index = 0; index < state.lights.size(); ++index) {
    const std::optional<LightRecord>& light = state.lights.at(index);
    if (!light.has_value()) {
      continue;
    }
    if (auto checked = CheckPortalsHeld(*light, index, state.lights);
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks that the prototypes `state` holds read only materials it holds: a
// material removed must not be read by one kept.
Checked CheckHeldReferences(const State& state) {
  for (std::size_t index = 0; index < state.prototypes.size(); ++index) {
    const std::optional<PrototypeRecord>& prototype =
        state.prototypes.at(index);
    if (!prototype.has_value()) {
      continue;
    }
    if (auto checked = CheckReadsHeld(*prototype, index, state.materials);
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks the content of every view of `changes` against `state`, the ids
// held once they are applied, and records it.
Checked CheckAll(const ChangeSet& changes, State& state) {
  return CheckKind(changes.textures, CheckTexture, state.textures)
      .and_then([&] {
        return CheckKind(changes.cameras, CheckCamera, state.cameras);
      })
      .and_then([&] {
        return CheckKind(
            changes.materials,
            [&](const MaterialNetwork& material) {
              return CheckMaterial(material, state.textures);
            },
            state.materials);
      })
      .and_then([&] {
        return CheckKind(
            changes.lights,
            [&](const Light& light) {
              return CheckLight(light, state.textures);
            },
            state.lights);
      })
      .and_then([&] { return CheckPrototypes(changes.prototypes, state); })
      .and_then([&] { return CheckHeldReferences(state); })
      .and_then([&] { return CheckHeldPortals(state); });
}

// The key of a texture held, which names it whatever its id.
std::string TextureKey(const State& state, TextureId texture) {
  const TextureRecord* record =
      Find(state.textures, std::to_underlying(texture));
  return record == nullptr ? std::string()
                           : record->file + '\n' + record->color_space;
}

// The path of the material `material` in `state`.
std::string_view MaterialPath(const State& state, MaterialId material) {
  const MaterialRecord* record =
      Find(state.materials, std::to_underlying(material));
  return record == nullptr ? std::string_view() : record->path;
}

// Each prim of `table`, by `key`, with its digest, the references to
// other prims resolved by `resolve`, sorted by key.
template <typename Record, typename Resolve>
std::vector<std::pair<std::string, Digest>> Entries(const Table<Record>& table,
                                                    const Resolve& resolve) {
  std::vector<std::pair<std::string, Digest>> entries;
  for (const std::optional<Record>& record : table) {
    if (record.has_value()) {
      entries.push_back(resolve(*record));
    }
  }
  std::ranges::sort(entries);
  return entries;
}

// Hashes `entries`, their count first.
void HashEntries(std::span<const std::pair<std::string, Digest>> entries,
                 Hasher& hasher) {
  hasher.Value(entries.size());
  for (const auto& [key, digest] : entries) {
    hasher.Text(key);
    hasher.Value(digest);
  }
}

// The entry of `record`, its textures resolved by `state`.
std::pair<std::string, Digest> EntryOf(const MaterialRecord& record,
                                       const State& state) {
  Hasher resolved;
  resolved.Value(record.digest);
  for (const TextureId texture : record.textures) {
    resolved.Text(TextureKey(state, texture));
  }
  return {record.path, resolved.digest()};
}

std::pair<std::string, Digest> EntryOf(const LightRecord& record,
                                       const State& state) {
  Hasher resolved;
  resolved.Value(record.digest);
  if (record.texture.has_value()) {
    resolved.Text(TextureKey(state, *record.texture));
  }
  for (const LightId portal : record.portals) {
    const LightRecord* held = Find(state.lights, std::to_underlying(portal));
    resolved.Text(held == nullptr ? std::string_view() : held->path);
  }
  return {record.path, resolved.digest()};
}

std::pair<std::string, Digest> EntryOf(const PrototypeRecord& record,
                                       const State& state) {
  Hasher resolved;
  resolved.Value(record.placement);
  resolved.Value(record.point_values);
  resolved.Value(record.topology);
  resolved.Value(record.primvars);
  resolved.Value(record.triangle_materials);
  for (const MaterialId material : record.materials) {
    resolved.Text(MaterialPath(state, material));
  }
  return {record.path, resolved.digest()};
}

// A digest of the prims `state` holds, by their paths and texture files,
// whatever their ids.
Digest StateDigest(const State& state) {
  Hasher hasher;
  const auto resolve = [&state](const auto& record) {
    return EntryOf(record, state);
  };
  HashEntries(Entries(state.textures,
                      [](const TextureRecord& record) {
                        return std::pair{
                            record.file + '\n' + record.color_space,
                            Digest{0},
                        };
                      }),
              hasher);
  HashEntries(Entries(state.cameras,
                      [](const CameraRecord& record) {
                        return std::pair{record.path, record.digest};
                      }),
              hasher);
  HashEntries(Entries(state.materials, resolve), hasher);
  HashEntries(Entries(state.lights, resolve), hasher);
  HashEntries(Entries(state.prototypes, resolve), hasher);
  return hasher.digest();
}

// "12 prototypes (2.1M instances, 1.24M triangles), 14 lights, 57
// materials, 1 camera, 210 textures".
std::string DescribeAdded(const ChangeSet& changes) {
  std::size_t instances = 0;
  std::size_t triangles = 0;
  for (const Prototype& prototype : changes.prototypes.added) {
    instances += prototype.instances.size();
    triangles += prototype.triangles.size();
  }
  return std::format("{} ({}, {}), {}, {}, {}, {}",
                     Counted(changes.prototypes.added.size(), "prototype"),
                     Counted(instances, "instance"),
                     Counted(triangles, "triangle"),
                     Counted(changes.lights.added.size(), "light"),
                     Counted(changes.materials.added.size(), "material"),
                     Counted(changes.cameras.added.size(), "camera"),
                     Counted(changes.textures.added.size(), "texture"));
}

// The number of prims `changes` updates, or removes.
std::size_t CountUpdated(const ChangeSet& changes) {
  return changes.prototypes.updated.size() + changes.lights.updated.size() +
         changes.materials.updated.size() + changes.cameras.updated.size() +
         changes.textures.updated.size();
}

std::size_t CountRemoved(const ChangeSet& changes) {
  return changes.prototypes.removed.size() + changes.lights.removed.size() +
         changes.materials.removed.size() + changes.cameras.removed.size() +
         changes.textures.removed.size();
}

// Traces each prim `changes` adds.
void TraceAdded(const ChangeSet& changes) {
  for (const Prototype& prototype : changes.prototypes.added) {
    UMBRA_LOG_TRACE(kDevice, "added prototype {} ({})",
                    std::to_underlying(prototype.id), prototype.path);
  }
  for (const Light& light : changes.lights.added) {
    UMBRA_LOG_TRACE(kDevice, "added light {} ({})",
                    std::to_underlying(light.id), light.path);
  }
  for (const MaterialNetwork& material : changes.materials.added) {
    UMBRA_LOG_TRACE(kDevice, "added material {} ({})",
                    std::to_underlying(material.id), material.path);
  }
  for (const Camera& camera : changes.cameras.added) {
    UMBRA_LOG_TRACE(kDevice, "added camera {} ({})",
                    std::to_underlying(camera.id), camera.path);
  }
  for (const Texture& texture : changes.textures.added) {
    UMBRA_LOG_TRACE(kDevice, "added texture {} ({}, {})",
                    std::to_underlying(texture.id), texture.file,
                    texture.color_space);
  }
}

// Logs at debug each prim of one kind `changes` updates, with its path.
template <typename View, typename Id>
void LogUpdated(const Changes<View, Id>& changes) {
  for (const View& view : changes.updated) {
    UMBRA_LOG_DEBUG(kDevice, "diagnostic: updated {} {} ({})", KindName<View>(),
                    std::to_underlying(view.id), view.path);
  }
}

// The name of a record in the debug log: its file for a texture, its path
// otherwise.
std::string_view NameOf(const TextureRecord& record) { return record.file; }
template <typename Record>
std::string_view NameOf(const Record& record) {
  return record.path;
}

// Logs at debug each prim of one kind `changes` removes, with the path
// `held` has for it.
template <typename Id, typename Record>
void LogRemoved(std::string_view kind, std::span<const Id> removed,
                const Table<Record>& held) {
  for (const Id removed_id : removed) {
    const std::uint32_t index = std::to_underlying(removed_id);
    const Record* record = Find(held, index);
    UMBRA_LOG_DEBUG(kDevice, "diagnostic: removed {} {} ({})", kind, index,
                    record == nullptr ? std::string_view() : NameOf(*record));
  }
}

// Logs at debug each prim `changes` updates or removes; `held` is the state
// before the commit.
void LogChanges(const ChangeSet& changes, const State& held) {
  for (const UpdatedPrototype& updated : changes.prototypes.updated) {
    UMBRA_LOG_DEBUG(kDevice, "diagnostic: updated prototype {} ({}): {}",
                    std::to_underlying(updated.prototype.id),
                    updated.prototype.path, FlagNames(updated.changes));
  }
  LogUpdated(changes.lights);
  LogUpdated(changes.materials);
  LogUpdated(changes.cameras);
  LogRemoved("prototype", changes.prototypes.removed, held.prototypes);
  LogRemoved("light", changes.lights.removed, held.lights);
  LogRemoved("material", changes.materials.removed, held.materials);
  LogRemoved("camera", changes.cameras.removed, held.cameras);
  LogRemoved("texture", changes.textures.removed, held.textures);
}

}  // namespace

DiagnosticDevice::DiagnosticDevice() : state_(std::make_unique<State>()) {}

DiagnosticDevice::~DiagnosticDevice() = default;

std::string_view DiagnosticDevice::name() const { return "diagnostic"; }

std::expected<void, Error> DiagnosticDevice::Commit(const ChangeSet& changes) {
  ++commits_;
  UMBRA_LOG_DEBUG(kDevice,
                  "diagnostic: commit {}: {} added, {} updated, {} removed",
                  commits_, DescribeAdded(changes), CountUpdated(changes),
                  CountRemoved(changes));
  TraceAdded(changes);
  LogChanges(changes, *state_);
  auto state = std::make_unique<State>(*state_);
  if (auto checked = ApplyAll(changes, *state).and_then([&] {
        return CheckAll(changes, *state);
      });
      !checked.has_value()) {
    return checked;
  }
  state_ = std::move(state);
  UMBRA_LOG_DEBUG(
      kDevice,
      "diagnostic: commit {} is consistent; it holds {}, {}, {}, {}, {} "
      "(state {:016x})",
      commits_, Counted(CountHeld(state_->prototypes), "prototype"),
      Counted(CountHeld(state_->lights), "light"),
      Counted(CountHeld(state_->materials), "material"),
      Counted(CountHeld(state_->cameras), "camera"),
      Counted(CountHeld(state_->textures), "texture"), StateDigest(*state_));
  return {};
}

}  // namespace umbra::device
