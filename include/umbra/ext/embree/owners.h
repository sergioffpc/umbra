// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of Embree's handles: the process's device, which Initialize
// creates, and the scenes and geometries built with it.

#ifndef UMBRA_EXT_EMBREE_OWNERS_H_
#define UMBRA_EXT_EMBREE_OWNERS_H_

#include <embree4/rtcore_device.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_scene.h>

#include <utility>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// The device Initialize created; null before it.
[[nodiscard]] RTCDevice UMBRA_NULLABLE ProcessDevice();

// An Embree scene, released when it is destroyed.
class UMBRA_OWNER Scene {
 public:
  Scene() = default;
  explicit Scene(RTCScene UMBRA_NULLABLE scene) : scene_(scene) {}
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;
  Scene(Scene&& other) noexcept
      : scene_(std::exchange(other.scene_, nullptr)) {}
  Scene& operator=(Scene&& other) noexcept {
    std::swap(scene_, other.scene_);
    return *this;
  }
  ~Scene();

  [[nodiscard]] RTCScene UMBRA_NULLABLE get() const { return scene_; }

 private:
  RTCScene UMBRA_NULLABLE scene_ = nullptr;
};

// An Embree geometry, released when it is destroyed. A scene it is attached
// to holds a reference of its own.
class UMBRA_OWNER Geometry {
 public:
  explicit Geometry(RTCGeometry UMBRA_NULLABLE geometry)
      : geometry_(geometry) {}
  Geometry(const Geometry&) = delete;
  Geometry& operator=(const Geometry&) = delete;
  Geometry(Geometry&&) = delete;
  Geometry& operator=(Geometry&&) = delete;
  ~Geometry();

  [[nodiscard]] RTCGeometry UMBRA_NULLABLE get() const { return geometry_; }

 private:
  RTCGeometry UMBRA_NULLABLE geometry_;
};

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_OWNERS_H_
