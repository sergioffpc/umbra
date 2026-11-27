// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/gpu/module.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "umbra/device/change_set.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/device.h"
#include "umbra/device/gpu/module_abi.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/embree/embree.h"

namespace umbra::device::gpu {
namespace {

using device::Float3;
using device::Matrix;
using device::Triangle;

// A translation by (x, y, z), for row vectors.
constexpr Matrix Translation(double x_offset, double y_offset,
                             double z_offset) {
  return {
      1.0,      0.0,      0.0,      0.0,  //
      0.0,      1.0,      0.0,      0.0,  //
      0.0,      0.0,      1.0,      0.0,  //
      x_offset, y_offset, z_offset, 1.0,
  };
}

// A rotation by `angle` radians about X, then a translation, for row
// vectors.
Matrix RotationX(double angle, double x_offset, double y_offset,
                 double z_offset) {
  const double cosine = std::cos(angle);
  const double sine = std::sin(angle);
  return {
      1.0,      0.0,      0.0,      0.0,  //
      0.0,      cosine,   sine,     0.0,  //
      0.0,      -sine,    cosine,   0.0,  //
      x_offset, y_offset, z_offset, 1.0,
  };
}

// A unit cube around the origin.
constexpr std::array<Float3, 8> kCubePoints = {
    Float3{.x = -0.5F, .y = -0.5F, .z = -0.5F},
    Float3{.x = 0.5F, .y = -0.5F, .z = -0.5F},
    Float3{.x = 0.5F, .y = 0.5F, .z = -0.5F},
    Float3{.x = -0.5F, .y = 0.5F, .z = -0.5F},
    Float3{.x = -0.5F, .y = -0.5F, .z = 0.5F},
    Float3{.x = 0.5F, .y = -0.5F, .z = 0.5F},
    Float3{.x = 0.5F, .y = 0.5F, .z = 0.5F},
    Float3{.x = -0.5F, .y = 0.5F, .z = 0.5F},
};
constexpr std::array<Triangle, 12> kCubeTriangles = {
    Triangle{0, 2, 1}, Triangle{0, 3, 2}, Triangle{4, 5, 6}, Triangle{4, 6, 7},
    Triangle{0, 1, 5}, Triangle{0, 5, 4}, Triangle{3, 7, 6}, Triangle{3, 6, 2},
    Triangle{0, 4, 7}, Triangle{0, 7, 3}, Triangle{1, 2, 6}, Triangle{1, 6, 5},
};

// A ground quad, 20 meters wide, at y = 0.
constexpr std::array<Float3, 4> kGroundPoints = {
    Float3{.x = -10.0F, .y = 0.0F, .z = -10.0F},
    Float3{.x = -10.0F, .y = 0.0F, .z = 10.0F},
    Float3{.x = 10.0F, .y = 0.0F, .z = 10.0F},
    Float3{.x = 10.0F, .y = 0.0F, .z = -10.0F},
};
constexpr std::array<Triangle, 2> kGroundTriangles = {
    Triangle{0, 1, 2},
    Triangle{0, 2, 3},
};

// A cube whose points move up within the shutter.
constexpr std::array<Float3, 8> Raised(float height) {
  std::array<Float3, 8> points = kCubePoints;
  for (Float3& point : points) {
    point.y += height;
  }
  return points;
}
constexpr std::array<Float3, 8> kRaisedPoints = Raised(0.3F);

// The scene both devices render: a ground; a cube placed three times, one
// of which moves within the shutter; a cube whose points deform within it;
// a sphere light and a tilted rect light.
class Scene {
 public:
  Scene() {
    still_ = {Translation(1.0, 0.5, 1.0)};
    other_ = {Translation(2.5, 0.5, -1.0)};
    moving_ = {Translation(-2.0, 0.5, 0.0), Translation(-1.0, 0.8, 0.0)};
    origin_ = {Translation(0.0, 0.0, 0.0)};
    deforming_at_ = {Translation(0.0, 0.5, -2.0)};
    sphere_at_ = {Translation(0.0, 3.0, 0.0)};
    rect_at_ = {RotationX(-0.5, 3.0, 2.5, -2.0)};
    camera_at_ = {Translation(0.0, 1.0, 8.0)};
    cube_instances_ = {
        device::Instance{.object_to_world = still_, .primvars = {}},
        device::Instance{.object_to_world = other_, .primvars = {}},
        device::Instance{.object_to_world = moving_, .primvars = {}},
    };
    ground_instances_ = {
        device::Instance{.object_to_world = origin_, .primvars = {}},
    };
    deforming_instances_ = {
        device::Instance{.object_to_world = deforming_at_, .primvars = {}},
    };
    sphere_instances_ = {
        device::Instance{.object_to_world = sphere_at_, .primvars = {}},
    };
    rect_instances_ = {
        device::Instance{.object_to_world = rect_at_, .primvars = {}},
    };
    cube_keys_ = {kCubePoints};
    ground_keys_ = {kGroundPoints};
    deforming_keys_ = {kCubePoints, kRaisedPoints};
  }

  // The change set that adds all of it.
  [[nodiscard]] device::ChangeSet Changes() {
    prototypes_ = {
        Prototype(0, "/ground", ground_keys_, kGroundTriangles,
                  ground_instances_),
        Prototype(1, "/cube", cube_keys_, kCubeTriangles, cube_instances_),
        Prototype(2, "/deforming", deforming_keys_, kCubeTriangles,
                  deforming_instances_),
    };
    lights_ = {
        Light(0, "/sphere", device::Light::Sphere{.radius = 0.4},
              sphere_instances_),
        Light(1, "/rect",
              device::Light::Rect{.width = 2.0, .height = 1.0, .texture = {}},
              rect_instances_),
    };
    device::Camera camera;
    camera.path = "/camera";
    camera.camera_to_world = camera_at_;
    camera.focal_length = 35.0;
    camera.aperture = {36.0, 24.0};
    camera.clipping_range = {0.1, 1000.0};
    cameras_ = {camera};
    device::ChangeSet changes;
    changes.ids = {.prototypes = 3, .lights = 2, .cameras = 1};
    changes.prototypes.added = prototypes_;
    changes.lights.added = lights_;
    changes.cameras.added = cameras_;
    return changes;
  }

 private:
  static device::Prototype Prototype(
      std::uint32_t number, std::string_view path,
      std::span<const std::span<const Float3>> points,
      std::span<const Triangle> triangles,
      std::span<const device::Instance> instances) {
    device::Prototype prototype;
    prototype.id = device::PrototypeId{number};
    prototype.path = path;
    prototype.points = points;
    prototype.triangles = triangles;
    prototype.instances = instances;
    return prototype;
  }

  static device::Light Light(std::uint32_t number, std::string_view path,
                             device::Light::Shape shape,
                             std::span<const device::Instance> instances) {
    device::Light light;
    light.id = device::LightId{number};
    light.path = path;
    light.shape = shape;
    light.instances = instances;
    return light;
  }

  std::vector<Matrix> still_, other_, moving_, origin_, deforming_at_,
      sphere_at_, rect_at_, camera_at_;
  std::vector<device::Instance> cube_instances_, ground_instances_,
      deforming_instances_, sphere_instances_, rect_instances_;
  std::vector<std::span<const Float3>> cube_keys_, ground_keys_,
      deforming_keys_;
  std::vector<device::Prototype> prototypes_;
  std::vector<device::Light> lights_;
  std::vector<device::Camera> cameras_;
};

// The image: 200 × 130 pixels, so that its right and bottom tiles are
// partial.
constexpr std::array<std::int32_t, 2> kResolution = {200, 130};
constexpr std::size_t kPixels =
    static_cast<std::size_t>(kResolution.at(0)) * kResolution.at(1);

// The depth and normal render buffers of a render.
struct Buffers {
  std::vector<float> depth = std::vector<float>(kPixels);
  std::vector<float> normal = std::vector<float>(3 * kPixels);
};

// Renders the scene committed to `device` into new buffers, with the
// device alone taking every tile.
Buffers Render(device::Device& device) {
  Buffers buffers;
  const std::array vars = {
      device::FilmVar{
          .source = device::VarSource::kDepth,
          .pixels = buffers.depth,
          .channels = 1,
      },
      device::FilmVar{
          .source = device::VarSource::kNormal,
          .pixels = buffers.normal,
          .channels = 3,
      },
  };
  const device::FrameParameters frame{
      .shutter = {.open = -0.25, .close = 0.25},
      .camera = device::CameraId{0},
      .image =
          {
              .resolution = kResolution,
              .window =
                  {
                      .x_end = kResolution.at(0),
                      .y_end = kResolution.at(1),
                  },
          },
      .samples_per_pixel = 4,
      .vars = vars,
  };
  device::TileQueue tiles(frame.image.window);
  const auto rendered = device.Render(frame, tiles);
  EXPECT_TRUE(rendered.has_value()) << rendered.error().message();
  EXPECT_EQ(tiles.taken(), tiles.count());
  return buffers;
}

// The device on gpu0, with `module`, or none when no GPU is usable.
std::unique_ptr<device::Device> UsableGpu(const Module& module) {
  const auto gpus = module.DescribeDevices();
  if (!gpus.has_value() || gpus->gpus.empty() ||
      gpus->gpus.front().usability != GpuUsability::kUsable) {
    return nullptr;
  }
  auto created = module.CreateDevice(gpus->gpus.front().ordinal);
  return created.has_value() ? *std::move(created) : nullptr;
}

// The pixels where `cpu` and `gpu` differ by more than `tolerance`, relative
// to the CPU's value for those above 1: both or neither infinite.
std::size_t CountMismatches(std::span<const float> cpu,
                            std::span<const float> gpu, std::size_t channels,
                            float tolerance) {
  std::size_t mismatches = 0;
  for (std::size_t pixel = 0; pixel < cpu.size() / channels; ++pixel) {
    bool differs = false;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const float expected =
          cpu.subspan((pixel * channels) + channel, 1).front();
      const float actual = gpu.subspan((pixel * channels) + channel, 1).front();
      if (std::isinf(expected) || std::isinf(actual)) {
        differs = differs || expected != actual;
        continue;
      }
      const float scale = std::fmax(1.0F, std::fabs(expected));
      differs = differs || std::fabs(expected - actual) > tolerance * scale;
    }
    mismatches += differs ? 1 : 0;
  }
  return mismatches;
}

// The pixels that may differ: those that some samples see across an edge,
// where Embree and OptiX may split a sample between two triangles, or hit
// and miss a silhouette, differently.
constexpr std::size_t kMostMismatches = kPixels / 200;
constexpr float kTolerance = 1e-3F;

TEST(GpuRenderTest, RendersTheDepthAndNormalsOfTheCpuDevice) {
  auto module = Module::Load(UMBRA_GPU_MODULE_PATH);
  ASSERT_TRUE(module.has_value()) << module.error().message();
  const std::unique_ptr<device::Device> gpu = UsableGpu(*module);
  if (gpu == nullptr) {
    GTEST_SKIP() << "no usable GPU";
  }
  const auto embree = ext::embree::Initialize();
  ASSERT_TRUE(embree.has_value()) << embree.error().message();
  device::cpu::CpuDevice cpu(0);
  Scene scene;
  const device::ChangeSet changes = scene.Changes();
  const auto cpu_committed = cpu.Commit(changes);
  ASSERT_TRUE(cpu_committed.has_value()) << cpu_committed.error().message();
  const auto gpu_committed = gpu->Commit(changes);
  ASSERT_TRUE(gpu_committed.has_value()) << gpu_committed.error().message();

  const Buffers expected = Render(cpu);
  const Buffers actual = Render(*gpu);

  EXPECT_LE(CountMismatches(expected.depth, actual.depth, 1, kTolerance),
            kMostMismatches);
  EXPECT_LE(CountMismatches(expected.normal, actual.normal, 3, kTolerance),
            kMostMismatches);
  // Most rays hit something: the comparison is not of two empty images.
  EXPECT_GT(std::ranges::count_if(
                expected.depth, [](float depth) { return !std::isinf(depth); }),
            static_cast<std::ptrdiff_t>(kPixels / 4));
}

}  // namespace
}  // namespace umbra::device::gpu
