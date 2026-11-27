// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// An rprim of a type umbra does not take, synced only to be counted.

#ifndef UMBRA_DELEGATE_UNSUPPORTED_PRIM_H_
#define UMBRA_DELEGATE_UNSUPPORTED_PRIM_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstdint>

#include "sync_counter.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// An rprim of a type umbra does not take (basisCurves, points, volume):
// synced only to be counted, so that it is reported.
class UnsupportedPrim final : public pxr::HdRprim {
 public:
  // Counts its sync into `counter`.
  UnsupportedPrim(pxr::TfToken type_id, const pxr::SdfPath& prim_id,
                  SyncCounter& counter);

  [[nodiscard]] pxr::HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(pxr::HdSceneDelegate* delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits, const pxr::TfToken& repr) override;
  [[nodiscard]] const pxr::TfTokenVector& GetBuiltinPrimvarNames()
      const override;

  [[nodiscard]] const pxr::TfToken& type() const UMBRA_LIFETIMEBOUND {
    return type_;
  }
  // Whether Hydra synced it: whether its purpose is rendered.
  [[nodiscard]] bool IsSynced() const { return synced_ == Synced::kSynced; }

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // names of the methods OpenUSD has rprims override.
  [[nodiscard]] pxr::HdDirtyBits _PropagateDirtyBits(
      pxr::HdDirtyBits bits) const override;
  void _InitRepr(const pxr::TfToken& repr,
                 pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  enum class Synced : std::uint8_t { kNotSynced, kSynced };

  pxr::TfToken type_;
  SyncCounter* counter_;
  Synced synced_ = Synced::kNotSynced;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_UNSUPPORTED_PRIM_H_
