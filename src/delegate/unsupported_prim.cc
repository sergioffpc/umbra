// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "unsupported_prim.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include <utility>

#include "sync_counter.h"
#include "umbra/core/safety.h"

namespace umbra::delegate::internal {

UnsupportedPrim::UnsupportedPrim(pxr::TfToken type_id,
                                 const pxr::SdfPath& prim_id,
                                 SyncCounter& counter)
    : pxr::HdRprim(prim_id), type_(std::move(type_id)), counter_(&counter) {}

pxr::HdDirtyBits UnsupportedPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdChangeTracker::AllSceneDirtyBits;
}

void UnsupportedPrim::Sync(pxr::HdSceneDelegate* /*delegate*/,
                           pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                           pxr::HdDirtyBits* dirty_bits,
                           const pxr::TfToken& /*repr*/) {
  *dirty_bits &= ~pxr::HdChangeTracker::AllSceneDirtyBits;
  synced_ = Synced::kSynced;
  CountSync(counter_, GetId());
}

const pxr::TfTokenVector& UnsupportedPrim::GetBuiltinPrimvarNames() const {
  static const pxr::TfTokenVector kNames;
  return kNames;
}

pxr::HdDirtyBits UnsupportedPrim::_PropagateDirtyBits(
    pxr::HdDirtyBits bits) const {
  return bits;
}

void UnsupportedPrim::_InitRepr(const pxr::TfToken& /*repr*/,
                                pxr::HdDirtyBits* /*dirty_bits*/) {
  // umbra draws no repr: it only counts.
}

}  // namespace umbra::delegate::internal
