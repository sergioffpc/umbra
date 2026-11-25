// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/scene/scene.h"

#include <utility>
#include <vector>

namespace umbra::scene {

Scene::Scene(RenderSettings settings, std::vector<Camera> cameras)
    : settings_(std::move(settings)), cameras_(std::move(cameras)) {}

}  // namespace umbra::scene
