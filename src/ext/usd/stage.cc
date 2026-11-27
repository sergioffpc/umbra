// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/usd/stage.h"

#include <pxr/base/tf/error.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/stage.h>
#include <unistd.h>

#include <cerrno>
#include <exception>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include "umbra/core/error.h"

namespace umbra::ext::usd {

namespace pxr = PXR_NS;

namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

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

}  // namespace

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

}  // namespace umbra::ext::usd
