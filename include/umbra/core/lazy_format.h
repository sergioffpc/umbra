// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A message formatted only when it is needed. Header-only: the bridges
// linked into the GPU module, which links no umbra module, take the message
// of a call's error this way, so that a call that succeeds formats nothing
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_CORE_LAZY_FORMAT_H_
#define UMBRA_CORE_LAZY_FORMAT_H_

#include <format>
#include <string>
#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// A format string and the arguments it formats, made into a message only
// when Format is called. It refers to the arguments, as std::format_args
// does, so it lives only as long as the expression that makes it: a function
// takes it as a parameter, as std::vformat takes its arguments, and formats
// it, or not, before it returns.
class LazyFormat {
 public:
  LazyFormat(std::string_view format, std::format_args args)
      : format_(format), args_(args) {}

  // The message: the format string with its arguments.
  [[nodiscard]] std::string Format() const {
    return std::vformat(format_, args_);
  }

 private:
  std::string_view format_;
  std::format_args args_;
};

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_LAZY_FORMAT_H_
