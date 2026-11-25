// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/usd/usd.h"

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/callContext.h>
#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/diagnosticLite.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/work/threadLimits.h>
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
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "close_scene.h"
#include "hydra.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd {
namespace {

namespace pxr = PXR_NS;

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// OpenUSD's diagnostics, routed into umbra's log. Tf prints nothing of its
// own once a delegate is registered, except for fatal errors, which it still
// reports and then aborts on. It may call the delegate from any thread.
class LogDelegate final : public pxr::TfDiagnosticMgr::Delegate {
 public:
  void IssueError(const pxr::TfError& error) override {
    if (error.GetQuiet()) {
      return;
    }
    if (error.GetDiagnosticCode() == pxr::TF_DIAGNOSTIC_CODING_ERROR_TYPE) {
      core::Log(
          LogLevel::kError, LogComponent::kUsd,
          std::format("coding error in {}: {}",
                      error.GetContext().GetFunction(), error.GetCommentary()));
      return;
    }
    core::Log(LogLevel::kError, LogComponent::kUsd, error.GetCommentary());
  }

  void IssueFatalError(const pxr::TfCallContext& context,
                       const std::string& message) override {
    core::Log(LogLevel::kFatal, LogComponent::kUsd,
              std::format("in {}: {}", context.GetFunction(), message));
  }

  void IssueStatus(const pxr::TfStatus& status) override {
    if (!status.GetQuiet()) {
      core::Log(LogLevel::kDebug, LogComponent::kUsd, status.GetCommentary());
    }
  }

  void IssueWarning(const pxr::TfWarning& warning) override {
    if (!warning.GetQuiet()) {
      core::Log(LogLevel::kWarning, LogComponent::kUsd,
                warning.GetCommentary());
    }
  }
};

// Registers the process's delegate, once. It is never removed: Tf may report
// until the process exits.
void RouteDiagnostics() {
  static LogDelegate delegate;
  static const bool kRegistered = [] {
    pxr::TfDiagnosticMgr::GetInstance().AddDelegate(&delegate);
    return true;
  }();
  static_cast<void>(kRegistered);
}

// Loads the plugin registry and checks that the file formats are among the
// plugins: without them no stage opens.
std::expected<void, Error> LoadPlugins() {
  const pxr::PlugRegistry& registry = pxr::PlugRegistry::GetInstance();
  if (!pxr::SdfFileFormat::FindByExtension("usda")) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        "OpenUSD: no plugin reads .usda files; its plugins (lib/usd) are not "
        "next to its libraries"));
  }
  UMBRA_LOG_DEBUG(kUsd, "{} plugins", registry.GetAllPlugins().size());
  return {};
}

// The messages of the errors posted since `mark`, joined: what OpenUSD
// says when a stage does not open.
std::string ErrorMessages(const pxr::TfErrorMark& mark) {
  std::string joined;
  for (const pxr::TfError& error : mark) {
    if (!joined.empty()) {
      joined += "; ";
    }
    joined += error.GetCommentary();
  }
  return joined.empty() ? std::string("unknown error") : joined;
}

// "cannot open '<path>': <reason>".
Error CannotOpen(ErrorCode code, std::string_view path,
                 std::string_view reason) {
  return {code, std::format("cannot open '{}': {}", path, reason)};
}

// Opens the stage of the file at `path`, with every payload loaded and no
// session layer. The errors of a stage that does not open become the
// returned error rather than log lines; those of a stage that opens are
// logged.
std::expected<pxr::UsdStageRefPtr, Error> OpenStage(const std::string& path) {
  // OpenUSD gives no reason for a file it cannot read; the system does.
  if (access(path.c_str(), R_OK) != 0) {
    const int reason = errno;
    return std::unexpected(CannotOpen(
        reason == ENOENT ? ErrorCode::kNotFound : ErrorCode::kInvalidArgument,
        path, std::generic_category().message(reason)));
  }
  const pxr::TfErrorMark mark;
  pxr::UsdStageRefPtr stage;
  try {
    // The layer stays open while `root` holds it; its handle comes from the
    // layer registry, as the stage takes it.
    if (const auto root = pxr::SdfLayer::FindOrOpen(path)) {
      stage =
          pxr::UsdStage::Open(pxr::SdfLayer::Find(path), pxr::SdfLayerHandle(),
                              pxr::UsdStage::LoadAll);
    }
  } catch (const std::exception& error) {
    mark.Clear();
    return std::unexpected(
        CannotOpen(ErrorCode::kInvalidArgument, path, error.what()));
  }
  if (!stage) {
    const std::string reason = ErrorMessages(mark);
    mark.Clear();
    return std::unexpected(
        CannotOpen(ErrorCode::kInvalidArgument, path, reason));
  }
  return stage;
}

// The time to load: the frame given, else the stage's startTimeCode when it
// authors one, else the default time.
pxr::UsdTimeCode LoadTime(const pxr::UsdStage& stage,
                          std::optional<double> frame) {
  if (frame.has_value()) {
    return {*frame};
  }
  if (stage.HasAuthoredMetadata(pxr::SdfFieldKeys->StartTimeCode)) {
    return {stage.GetStartTimeCode()};
  }
  return pxr::UsdTimeCode::Default();
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
std::expected<pxr::UsdStageRefPtr, Error> LoadStage(
    std::string_view path, std::optional<double> frame) {
  const auto start = std::chrono::steady_clock::now();
  // The number of layers is known once the stage is open: OpenUSD composes
  // it in one call.
  core::ProgressPhase phase("Loading stage", "layers");
  auto stage = OpenStage(std::string(path));
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
  UMBRA_LOG_DEBUG(kUsd, "stage opened in {} ({} layers, {} prims, {})",
                  core::FormatDuration(elapsed),
                  core::FormatCount(layers.size()), core::FormatCount(prims),
                  TimeName(LoadTime(**stage, frame)));
  return stage;
}

// The frame of `time`, or none for the default time.
std::optional<double> FrameOf(pxr::UsdTimeCode time) {
  return time.IsDefault() ? std::nullopt
                          : std::optional<double>(time.GetValue());
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
// surfaces, as meshes.
pxr::HdSceneIndexBaseRefPtr ChainSceneIndices(const pxr::UsdStageRefPtr& stage,
                                              pxr::UsdTimeCode time,
                                              const pxr::SdfPath& settings) {
  pxr::UsdImagingCreateSceneIndicesInfo info;
  info.stage = stage;
  const pxr::UsdImagingSceneIndices indices =
      pxr::UsdImagingCreateSceneIndices(info);
  indices.stageSceneIndex->SetTime(time);
  const pxr::HdsiSceneGlobalsSceneIndexRefPtr globals =
      pxr::HdsiSceneGlobalsSceneIndex::New(indices.finalSceneIndex);
  globals->SetActiveRenderSettingsPrimPath(settings);
  if (!time.IsDefault()) {
    globals->SetCurrentFrame(time.GetValue());
  }
  pxr::HdSceneIndexBaseRefPtr chain = globals;
  chain = pxr::HdsiRenderSettingsFilteringSceneIndex::New(chain, nullptr);
  chain = pxr::HdsiLightLinkingSceneIndex::New(chain, nullptr);
  chain = ImplicitSurfacesToMeshes(chain);
  return chain;
}

// What SyncScene syncs.
struct SyncRequest {
  std::string scene_path;
  std::string settings;
  pxr::UsdTimeCode time;
};

// Builds the render index on umbra's delegate, populated from the chain,
// syncs it in HdEngine::Execute and takes the Scene the delegate closed.
std::expected<scene::Scene, Error> ExecuteSync(
    const pxr::UsdStageRefPtr& stage, pxr::UsdTimeCode time,
    const internal::CloseInputs& inputs, core::ProgressPhase& phase) {
  internal::RenderDelegate delegate(
      phase, inputs.meters_per_unit,
      [&inputs, &stage](const internal::SyncedPrims& prims) {
        return internal::CloseScene(inputs, *stage, prims);
      });
  // Destroyed before the delegate, whose prims it destroys.
  const std::unique_ptr<pxr::HdRenderIndex> index(
      pxr::HdRenderIndex::New(&delegate, {}));
  if (index == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Hydra: no render index"));
  }
  index->InsertSceneIndex(ChainSceneIndices(stage, time, inputs.settings),
                          pxr::SdfPath::AbsoluteRootPath(),
                          /*needsPrefixing=*/false);
  phase.SetTotal(delegate.PrimCount());
  // Hydra syncs the rprims of the collections queued, in their reprs: one
  // collection of every rprim, whose repr umbra never draws.
  index->EnqueueCollectionToSync(pxr::HdRprimCollection(
      pxr::HdTokens->geometry, pxr::HdReprSelector(pxr::HdReprTokens->hull)));
  pxr::HdTaskSharedPtrVector tasks = {
      std::make_shared<internal::SyncTask>(delegate, inputs.settings),
  };
  pxr::HdEngine engine;
  engine.Execute(index.get(), &tasks);
  auto scene = delegate.TakeScene();
  if (!scene.has_value()) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Hydra did not commit the scene"));
  }
  return std::move(*scene);
}

// Resolves the settings and syncs the stage into the Scene.
std::expected<scene::Scene, Error> SyncStage(const pxr::UsdStageRefPtr& stage,
                                             const SyncRequest& request,
                                             core::ProgressPhase& phase) {
  const auto settings = SettingsPath(*stage, request.settings);
  if (!settings.has_value()) {
    return std::unexpected(settings.error());
  }
  const internal::CloseInputs inputs{
      .settings = *settings,
      .scene_path = request.scene_path,
      .frame = FrameOf(request.time),
      .meters_per_unit = pxr::UsdGeomGetStageMetersPerUnit(stage),
  };
  return ExecuteSync(stage, request.time, inputs, phase);
}

// Syncs the stage into the Scene through Hydra, as the render's second
// phase, "Syncing scene".
std::expected<scene::Scene, Error> SyncScene(const pxr::UsdStageRefPtr& stage,
                                             const SyncRequest& request) {
  core::ProgressPhase phase("Syncing scene", "prims");
  try {
    auto scene = SyncStage(stage, request, phase);
    if (scene.has_value()) {
      phase.Finish();
    }
    return scene;
  } catch (const std::exception& error) {
    return std::unexpected(
        Error(ErrorCode::kInternal, std::format("Hydra: {}", error.what())));
  } catch (...) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Hydra: an unknown exception"));
  }
}

// "render/beauty.0012.exr [color, depth]".
std::string DescribeProduct(const scene::RenderProduct& product) {
  std::string vars;
  for (const scene::RenderVar& var : product.vars) {
    vars += vars.empty() ? var.name : ", " + var.name;
  }
  return std::format("{} [{}]", product.file, vars);
}

// "1.24M triangles in 3.4k meshes, 18 primvars": the primvars counted by
// name, the normals apart.
std::string DescribeGeometry(const scene::Scene& scene) {
  std::size_t triangles = 0;
  std::set<std::string_view> primvars;
  for (const scene::Mesh& mesh : scene.prototypes()) {
    triangles += mesh.triangles.size();
    for (const scene::Primvar& primvar : mesh.primvars) {
      primvars.insert(primvar.name);
    }
  }
  const std::size_t meshes = scene.prototypes().size();
  return std::format("{} triangle{} in {} mesh{}, {} primvar{}",
                     core::FormatCount(triangles), triangles == 1 ? "" : "s",
                     core::FormatCount(meshes), meshes == 1 ? "" : "es",
                     core::FormatCount(primvars.size()),
                     primvars.size() == 1 ? "" : "s");
}

// "settings /Render/main, camera /World/cam, 1 product: render/beauty.0012.exr
// [color, depth], 1.24M triangles in 3.4k meshes, 18 primvars".
std::string DescribeScene(const scene::Scene& scene) {
  const scene::RenderSettings& settings = scene.settings();
  std::string products;
  for (const scene::RenderProduct& product : settings.products) {
    products += (products.empty() ? "" : ", ") + DescribeProduct(product);
  }
  const std::size_t count = settings.products.size();
  return std::format(
      "settings {}, camera {}, {} product{}: {}, {}", settings.path,
      scene.cameras().subspan(settings.camera).front().path, count,
      count == 1 ? "" : "s", products, DescribeGeometry(scene));
}

}  // namespace

std::expected<void, Error> Initialize() {
  RouteDiagnostics();
  // TF_DEBUG output goes to stdout by default, which carries only results;
  // Tf writes it to a file, so it cannot go through the log.
  pxr::TfDebug::SetOutputFile(stderr);
  try {
    if (auto loaded = LoadPlugins(); !loaded.has_value()) {
      return loaded;
    }
  } catch (const std::exception& error) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        std::format("OpenUSD: cannot load its plugins: {}", error.what())));
  }
  // OpenUSD runs on the process's oneTBB pool. umbra sets no limit of its
  // own; PXR_WORK_THREAD_LIMIT still applies.
  UMBRA_LOG_DEBUG(kUsd, "{} threads", pxr::WorkGetConcurrencyLimit());
  UMBRA_LOG_DEBUG(kUsd, "OpenUSD {}.{:02}", PXR_MINOR_VERSION,
                  PXR_PATCH_VERSION);
  return {};
}

std::expected<scene::Scene, Error> LoadScene(std::string_view path,
                                             const LoadOptions& options) {
  const auto stage = LoadStage(path, options.frame);
  if (!stage.has_value()) {
    return std::unexpected(stage.error());
  }
  const pxr::UsdTimeCode time = LoadTime(**stage, options.frame);
  const auto start = std::chrono::steady_clock::now();
  auto scene = SyncScene(*stage, {
                                     .scene_path = std::string(path),
                                     .settings = options.settings,
                                     .time = time,
                                 });
  if (!scene.has_value()) {
    return std::unexpected(scene.error());
  }
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start;
  UMBRA_LOG_DEBUG(kUsd, "scene synced in {} ({})",
                  core::FormatDuration(elapsed), DescribeScene(*scene));
  return scene;
}

}  // namespace umbra::ext::usd
