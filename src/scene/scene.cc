// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/scene/scene.h"

#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

namespace umbra::scene {

std::size_t Primvar::Count() const {
  const std::size_t numbers =
      std::visit([](const auto& numbers) { return numbers.size(); }, values);
  return components == 0 ? 0 : numbers / components;
}

Scene::Scene(RenderSettings settings, Shutter shutter,
             std::vector<Camera> cameras, std::vector<Mesh> prototypes,
             std::vector<Instance> instances)
    : settings_(std::move(settings)),
      shutter_(shutter),
      cameras_(std::move(cameras)),
      prototypes_(std::move(prototypes)),
      instances_(std::move(instances)) {}

}  // namespace umbra::scene
