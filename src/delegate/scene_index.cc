// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "scene_index.h"

#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/usd/sdf/path.h>

namespace umbra::delegate::internal {

pxr::HdContainerDataSourceHandle PrimDataSource(pxr::HdSceneDelegate& delegate,
                                                const pxr::SdfPath& prim_id) {
  const pxr::HdSceneIndexBaseRefPtr terminal =
      delegate.GetRenderIndex().GetTerminalSceneIndex();
  return terminal == nullptr ? nullptr : terminal->GetPrim(prim_id).dataSource;
}

}  // namespace umbra::delegate::internal
