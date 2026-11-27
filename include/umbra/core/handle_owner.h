// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The owner of a library's handle, which releases it with the library's
// function. Header-only: the bridges linked into the GPU module, which links
// no umbra module, own their handles with it too.

#ifndef UMBRA_CORE_HANDLE_OWNER_H_
#define UMBRA_CORE_HANDLE_OWNER_H_

#include <utility>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// Owns a `Handle`, a pointer or an integer whose value-initialized value
// (null, 0) means none, and calls `Release` on it when it is destroyed or
// replaced. `Release` cannot fail, or logs its failure: an owner whose
// release can fail and must say so has a Destroy of its own.
template <typename Handle, auto Release>
class UMBRA_OWNER HandleOwner {
 public:
  HandleOwner() = default;
  explicit HandleOwner(Handle handle) : handle_(handle) {}
  HandleOwner(const HandleOwner&) = delete;
  HandleOwner& operator=(const HandleOwner&) = delete;
  HandleOwner(HandleOwner&& other) noexcept
      : handle_(std::exchange(other.handle_, Handle{})) {}
  HandleOwner& operator=(HandleOwner&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, Handle{}));
    }
    return *this;
  }
  ~HandleOwner() { reset(); }

  // The handle; none when it owns none.
  [[nodiscard]] Handle get() const { return handle_; }

  // Releases the handle it owns, if any, and owns `handle` instead.
  void reset(Handle handle = Handle{}) {
    if (const Handle released = std::exchange(handle_, handle);
        released != Handle{}) {
      Release(released);
    }
  }

  // Gives up the handle, unreleased, and owns none.
  [[nodiscard]] Handle release() { return std::exchange(handle_, Handle{}); }

 private:
  Handle handle_{};
};

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_HANDLE_OWNER_H_
