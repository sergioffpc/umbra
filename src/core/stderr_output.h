// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The writer the logger and the progress reporter share on stderr. On a
// terminal, the progress line of the current phase stays on the last line:
// a diagnostic clears it, is written, and the line is drawn again below it,
// all with one write, so that neither interleaves with the other.

#ifndef UMBRA_CORE_STDERR_OUTPUT_H_
#define UMBRA_CORE_STDERR_OUTPUT_H_

#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core::internal {

// Writes all of `text` to `output`. Text that cannot be written is dropped:
// there is nowhere left to report it.
void WriteAll(int output, std::string_view text);

// Writes lines to stderr with one write, above the progress line: clears
// it first and draws it again after, when one is shown.
void WriteAboveProgressLine(std::string_view text);

// Shows `line` as the progress line on the terminal, in place of the one
// shown, cut to the terminal's width.
void DrawProgressLine(std::string_view line);

// Replaces the progress line with `line`, which stays, and ends it: the
// next output starts on a new line.
void EndProgressLine(std::string_view line);

}  // namespace umbra::core::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_STDERR_OUTPUT_H_
