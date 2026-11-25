// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "stderr_output.h"

#include <asm-generic/ioctls.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

#include "umbra/core/safety.h"

namespace umbra::core::internal {
namespace {

// Moves to the start of the line, and clears from the cursor to its end.
constexpr std::string_view kReturn = "\r";
constexpr std::string_view kClearToEnd = "\x1b[K";

// `line` cut to the terminal's width, less one column, so that the terminal
// never wraps it and a carriage return goes back to its start.
std::string_view FitTerminal(std::string_view line) {
  winsize size{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): ioctl is variadic.
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0) {
    return line;
  }
  const std::size_t columns = size.ws_col - std::size_t{1};
  return line.substr(0, columns);
}

// The progress line shown on the terminal, empty when none is, and the
// writes that keep it on the last line.
class ProgressLine {
 public:
  void Write(std::string_view text) UMBRA_EXCLUDES(mutex_) {
    const std::scoped_lock lock(mutex_);
    if (line_.empty()) {
      WriteAll(STDERR_FILENO, text);
      return;
    }
    std::string output;
    output.append(kReturn).append(kClearToEnd).append(text).append(line_);
    WriteAll(STDERR_FILENO, output);
  }

  void Draw(std::string_view line) UMBRA_EXCLUDES(mutex_) {
    const std::scoped_lock lock(mutex_);
    line_ = FitTerminal(line);
    std::string output;
    output.append(kReturn).append(line_).append(kClearToEnd);
    WriteAll(STDERR_FILENO, output);
  }

  void End(std::string_view line) UMBRA_EXCLUDES(mutex_) {
    const std::scoped_lock lock(mutex_);
    line_.clear();
    std::string output;
    output.append(kReturn).append(line).append(kClearToEnd).append("\n");
    WriteAll(STDERR_FILENO, output);
  }

 private:
  std::mutex mutex_;
  std::string line_ UMBRA_GUARDED_BY(mutex_);
};

ProgressLine& Shown() {
  static ProgressLine shown;
  return shown;
}

}  // namespace

void WriteAll(int output, std::string_view text) {
  while (!text.empty()) {
    const auto written = write(output, text.data(), text.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    text.remove_prefix(static_cast<std::size_t>(written));
  }
}

void WriteStderr(std::string_view text) { Shown().Write(text); }

void DrawProgressLine(std::string_view line) { Shown().Draw(line); }

void EndProgressLine(std::string_view line) { Shown().End(line); }

}  // namespace umbra::core::internal
