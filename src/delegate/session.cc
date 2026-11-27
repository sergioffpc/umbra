// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/delegate/session.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/imaging/hdsi/sceneGlobalsSceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "commit.h"
#include "denoiser.h"
#include "hydra.h"
#include "ies_tilt.h"
#include "ledger.h"
#include "render_task.h"
#include "sync_counter.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/device.h"
#include "umbra/ext/usd/stage.h"

namespace umbra::delegate {

namespace pxr = PXR_NS;

struct Session::State {
  pxr::UsdStageRefPtr stage;
  // The scene indices from the stage to the render index, and those of
  // them that take the time and the frame.
  pxr::HdSceneIndexBaseRefPtr chain;
  pxr::UsdImagingStageSceneIndexRefPtr stage_index;
  pxr::HdsiSceneGlobalsSceneIndexRefPtr globals;
  internal::CommitInputs inputs;
  std::span<device::Device* const> devices;
  internal::DenoiseDevice denoise_device;
  // What shows the products while they render, if anything.
  Display* UMBRA_NULLABLE display = nullptr;
  // What the prims count their syncs into: the phase of the frame syncing.
  internal::SyncCounter counter;
  internal::Ledger ledger;
  // The frames synced.
  std::size_t frames = 0;
  std::unique_ptr<internal::RenderDelegate> delegate;
  // Destroyed before the delegate, whose prims it destroys.
  std::unique_ptr<pxr::HdRenderIndex> index;
  pxr::HdEngine engine;
  // The task, the only one Execute runs.
  std::shared_ptr<internal::RenderTask> task;
  pxr::HdTaskSharedPtrVector tasks;
  RenderSettings settings;
  device::FrameParameters frame;
};

Session::Session(std::unique_ptr<State> state) : state_(std::move(state)) {}

Session::~Session() = default;

Display::~Display() = default;

const RenderSettings& Session::settings() const { return state_->settings; }

const device::FrameParameters& Session::frame() const { return state_->frame; }

namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The time of `frame`: the default time for none.
pxr::UsdTimeCode TimeOf(std::optional<double> frame) {
  return frame.has_value() ? pxr::UsdTimeCode(*frame)
                           : pxr::UsdTimeCode::Default();
}

// "frame 12", "frame 12.5" or "default time".
std::string TimeName(pxr::UsdTimeCode time) {
  if (time.IsDefault()) {
    return "default time";
  }
  return std::format("frame {}", time.GetValue());
}

// The prims of the stage a render traverses: active, loaded, defined and
// not abstract.
std::size_t CountPrims(const pxr::UsdStageRefPtr& stage) {
  const pxr::UsdPrimRange prims = stage->Traverse();
  return static_cast<std::size_t>(std::distance(prims.begin(), prims.end()));
}

// Opens the stage of the file at `path`, as the render's first phase,
// "Loading stage".
std::expected<pxr::UsdStageRefPtr, Error> LoadStage(std::string_view path) {
  const auto start = std::chrono::steady_clock::now();
  // The number of layers is known once the stage is open: OpenUSD composes
  // it in one call.
  core::ProgressPhase phase("Loading stage", "layers");
  auto stage = ext::usd::OpenStage(std::string(path));
  if (!stage.has_value()) {
    return std::unexpected(stage.error());
  }
  const pxr::SdfLayerHandleVector layers = (*stage)->GetUsedLayers();
  phase.SetTotal(layers.size());
  phase.Add(layers.size());
  const std::size_t prims = CountPrims(*stage);
  phase.Finish();
  for (const pxr::SdfLayerHandle& layer : layers) {
    UMBRA_LOG_TRACE(kUsd, "layer {}", layer->GetIdentifier());
  }
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start;
  UMBRA_LOG_DEBUG(kUsd, "stage opened in {} ({} layers, {} prims)",
                  core::FormatDuration(elapsed),
                  core::FormatCount(layers.size()), core::FormatCount(prims));
  return stage;
}

// The RenderSettings prim the render uses: `given` (--settings), else the
// stage's renderSettingsPrimPath. Fails with kInvalidArgument when there is
// neither, or the path is not that of a prim.
std::expected<pxr::SdfPath, Error> SettingsPath(const pxr::UsdStage& stage,
                                                std::string_view given) {
  std::string text(given);
  if (text.empty() &&
      !stage.GetMetadata(pxr::UsdRenderTokens->renderSettingsPrimPath, &text)) {
    text.clear();
  }
  if (text.empty()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              "no render settings: give --settings, or author the stage's "
              "renderSettingsPrimPath"));
  }
  pxr::SdfPath path = pxr::SdfPath::IsValidPathString(text) ? pxr::SdfPath(text)
                                                            : pxr::SdfPath();
  if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
    return std::unexpected(Error(
        ErrorCode::kInvalidArgument,
        std::format("invalid render settings path '{}': the absolute path "
                    "of a prim is expected",
                    text)));
  }
  return path;
}

// The implicit surfaces (capsules, cones, cubes, cylinders, planes and
// spheres), turned into meshes.
pxr::HdSceneIndexBaseRefPtr ImplicitSurfacesToMeshes(
    const pxr::HdSceneIndexBaseRefPtr& input) {
  const pxr::HdDataSourceBaseHandle to_mesh =
      pxr::HdRetainedTypedSampledDataSource<pxr::TfToken>::New(
          pxr::HdsiImplicitSurfaceSceneIndexTokens->toMesh);
  const pxr::HdContainerDataSourceHandle args =
      pxr::HdRetainedContainerDataSource::New(
          pxr::HdPrimTypeTokens->capsule, to_mesh, pxr::HdPrimTypeTokens->cone,
          to_mesh, pxr::HdPrimTypeTokens->cube, to_mesh,
          pxr::HdPrimTypeTokens->cylinder, to_mesh,
          pxr::HdPrimTypeTokens->plane, to_mesh, pxr::HdPrimTypeTokens->sphere,
          to_mesh);
  return pxr::HdsiImplicitSurfaceSceneIndex::New(input, args);
}

// The scene indices from the stage to the render index: usdImaging's, at
// `time`, then the scene globals, which name the settings and the frame,
// hdsi's render settings and light linking filters, and its implicit
// surfaces, as meshes. The stage is given once the chain is whole: the
// light linking filter learns the lights' collections only as their prims
// are added.
void ChainSceneIndices(Session::State& state, pxr::UsdTimeCode time) {
  const pxr::UsdImagingSceneIndices indices = pxr::UsdImagingCreateSceneIndices(
      pxr::UsdImagingCreateSceneIndicesInfo());
  state.stage_index = indices.stageSceneIndex;
  state.stage_index->SetTime(time);
  state.globals = pxr::HdsiSceneGlobalsSceneIndex::New(indices.finalSceneIndex);
  state.globals->SetActiveRenderSettingsPrimPath(state.inputs.settings);
  if (!time.IsDefault()) {
    state.globals->SetCurrentFrame(time.GetValue());
  }
  pxr::HdSceneIndexBaseRefPtr chain = state.globals;
  chain = pxr::HdsiRenderSettingsFilteringSceneIndex::New(chain, nullptr);
  chain = pxr::HdsiLightLinkingSceneIndex::New(chain, nullptr);
  state.chain = ImplicitSurfacesToMeshes(chain);
  state.stage_index->SetStage(state.stage);
}

// Builds the render index on umbra's delegate, populated from the chain at
// `time`, whose prims count their syncs into `phase`.
std::expected<void, Error> BuildRenderIndex(Session::State& state,
                                            pxr::UsdTimeCode time,
                                            core::ProgressPhase& phase) {
  ChainSceneIndices(state, time);
  state.delegate = std::make_unique<internal::RenderDelegate>(
      state.counter, pxr::UsdGeomGetStageMetersPerUnit(state.stage),
      pxr::UsdGeomGetStageUpAxis(state.stage) == pxr::UsdGeomTokens->z
          ? internal::UpAxis::kZ
          : internal::UpAxis::kY,
      [&state](internal::SyncedPrims& prims) {
        return internal::Commit(
            state.inputs, {.stage = &*state.stage, .scene_index = state.chain},
            prims, state.ledger, state.devices);
      },
      state.devices);
  state.index.reset(pxr::HdRenderIndex::New(state.delegate.get(), {}));
  if (state.index == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Hydra: no render index"));
  }
  state.index->InsertSceneIndex(state.chain, pxr::SdfPath::AbsoluteRootPath(),
                                /*needsPrefixing=*/false);
  phase.SetTotal(state.delegate->PrimCount());
  state.task = std::make_shared<internal::RenderTask>(
      *state.delegate, state.inputs.settings, state.denoise_device,
      state.display);
  state.tasks = {state.task};
  return {};
}

// Moves the chain to `time`: usdImaging dirties what varies over time.
void SetTime(const Session::State& state, pxr::UsdTimeCode time) {
  state.stage_index->SetTime(time);
  if (!time.IsDefault()) {
    state.globals->SetCurrentFrame(time.GetValue());
  }
}

// Runs HdEngine::Execute once: the prims that changed sync, the delegate
// commits them to the devices, and the task renders the products. Gives what
// the debug line says was committed.
std::expected<std::string, Error> Execute(Session::State& state) {
  // Hydra syncs the rprims of the collections queued, in their reprs, and
  // empties the queue: one collection of every rprim, whose repr umbra never
  // draws, queued again for each frame.
  state.index->EnqueueCollectionToSync(pxr::HdRprimCollection(
      pxr::HdTokens->geometry, pxr::HdReprSelector(pxr::HdReprTokens->hull)));
  state.engine.Execute(state.index.get(), &state.tasks);
  std::optional<std::expected<internal::Committed, Error>> committed =
      state.delegate->TakeCommitted();
  if (!committed.has_value()) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Hydra did not commit the prims"));
  }
  if (!committed->has_value()) {
    return std::unexpected(std::move(committed->error()));
  }
  std::optional<std::expected<void, Error>> rendered = state.task->TakeResult();
  if (!rendered.has_value()) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Hydra did not render the products"));
  }
  if (!rendered->has_value()) {
    return std::unexpected(std::move(rendered->error()));
  }
  state.settings = std::move((*committed)->settings);
  state.frame = (*committed)->frame;
  return std::move((*committed)->description);
}

// Syncs the stage at `frame` through Hydra, commits what changed to the
// devices, in `phase`, calls `on_committed`, and renders the products.
std::expected<std::string, Error> Sync(
    Session::State& state, std::optional<double> frame,
    core::ProgressPhase& phase, const std::function<void()>& on_committed) {
  const pxr::UsdTimeCode time = TimeOf(frame);
  state.inputs.frame = frame;
  if (state.index == nullptr) {
    if (auto built = BuildRenderIndex(state, time, phase); !built.has_value()) {
      return std::unexpected(built.error());
    }
  } else {
    SetTime(state, time);
  }
  state.task->SetOnCommitted(on_committed);
  auto executed = Execute(state);
  state.task->SetOnCommitted({});
  return executed;
}

// The name of the phase of the frame at `time`: "Syncing scene" for the
// first, "Syncing frame 12" for a later one.
std::string PhaseName(std::size_t synced, pxr::UsdTimeCode time) {
  return synced == 0 ? std::string("Syncing scene")
                     : std::format("Syncing {}", TimeName(time));
}

// Ends the counting of syncs into a phase when it goes out of scope.
class CountingInto {
 public:
  CountingInto(internal::SyncCounter& counter UMBRA_LIFETIMEBOUND,
               core::ProgressPhase& phase)
      : counter_(&counter) {
    counter_->Begin(phase);
  }
  CountingInto(const CountingInto&) = delete;
  CountingInto& operator=(const CountingInto&) = delete;
  CountingInto(CountingInto&&) = delete;
  CountingInto& operator=(CountingInto&&) = delete;
  ~CountingInto() { counter_->End(); }

 private:
  internal::SyncCounter* counter_;
};

}  // namespace

std::optional<TimeCodes> Session::time_codes() const {
  const pxr::UsdStage& stage = *state_->stage;
  const bool has_start =
      stage.HasAuthoredMetadata(pxr::SdfFieldKeys->StartTimeCode);
  const bool has_end =
      stage.HasAuthoredMetadata(pxr::SdfFieldKeys->EndTimeCode);
  if (!has_start && !has_end) {
    return std::nullopt;
  }
  const double start =
      has_start ? stage.GetStartTimeCode() : stage.GetEndTimeCode();
  const double end = has_end ? stage.GetEndTimeCode() : start;
  return TimeCodes{.start = start, .end = end};
}

std::vector<RenderedProduct> Session::Rendered() const {
  const State& state = *state_;
  std::vector<RenderedProduct> rendered;
  if (state.task == nullptr) {
    return rendered;
  }
  const std::span<const internal::ProductBuffers> buffers =
      state.task->buffers();
  for (const auto& [product, product_buffers] :
       std::views::zip(state.settings.products, buffers)) {
    RenderedProduct& out = rendered.emplace_back(&product);
    for (const auto& [var, buffer] :
         std::views::zip(product.vars, product_buffers)) {
      out.vars.push_back({.var = &var, .pixels = buffer->pixels()});
    }
  }
  return rendered;
}

std::expected<void, Error> Session::RenderFrame(std::optional<double> frame) {
  State& state = *state_;
  const pxr::UsdTimeCode time = TimeOf(frame);
  const auto start = std::chrono::steady_clock::now();
  std::chrono::duration<double> synced_in{};
  std::expected<std::string, Error> committed;
  {
    // The phase ends, failed, before the error is reported; it ends once
    // the devices have committed, before the products render.
    core::ProgressPhase phase(PhaseName(state.frames, time), "prims");
    std::optional<CountingInto> counting(std::in_place, state.counter, phase);
    const std::function<void()> on_committed = [&] {
      counting.reset();
      phase.Finish();
      synced_in = std::chrono::steady_clock::now() - start;
    };
    try {
      committed = Sync(state, frame, phase, on_committed);
    } catch (const std::exception& error) {
      committed = std::unexpected(
          Error(ErrorCode::kInternal, std::format("Hydra: {}", error.what())));
    } catch (...) {
      committed = std::unexpected(
          Error(ErrorCode::kInternal, "Hydra: an unknown exception"));
    }
  }
  if (!committed.has_value()) {
    return std::unexpected(std::move(committed.error()));
  }
  UMBRA_LOG_DEBUG(kUsd, "{} synced in {} ({})",
                  state.frames == 0 ? std::string("scene") : TimeName(time),
                  core::FormatDuration(synced_in), *committed);
  ++state.frames;
  return {};
}

std::expected<std::unique_ptr<Session>, Error> OpenSession(
    std::string_view path, const LoadOptions& options,
    std::span<device::Device* const> devices, std::span<const Gpu> gpus,
    Display* UMBRA_NULLABLE display) {
  auto stage = LoadStage(path);
  if (!stage.has_value()) {
    return std::unexpected(stage.error());
  }
  const auto settings = SettingsPath(**stage, options.settings);
  if (!settings.has_value()) {
    return std::unexpected(settings.error());
  }
  auto state = std::make_unique<Session::State>();
  state->stage = std::move(*stage);
  state->devices = devices;
  state->denoise_device = internal::DenoiseDeviceOf(gpus);
  state->display = display;
  state->inputs = {
      .settings = *settings,
      .scene_path = std::string(path),
      .frame = std::nullopt,
  };
  return std::make_unique<Session>(std::move(state));
}

}  // namespace umbra::delegate
