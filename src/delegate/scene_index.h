// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The prims' data in the terminal scene index of the render index: what
// Hydra's scene delegate API does not give, such as the color spaces of
// their parameters and primvars.

#ifndef UMBRA_DELEGATE_SCENE_INDEX_H_
#define UMBRA_DELEGATE_SCENE_INDEX_H_

#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The data source of the prim at `prim_id` in the terminal scene index of
// the render index of `delegate`, or none.
[[nodiscard]] pxr::HdContainerDataSourceHandle PrimDataSource(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_SCENE_INDEX_H_
