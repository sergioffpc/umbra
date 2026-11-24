// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The command-line parser of the umbra executable, with GNU conventions:
// short flags bundle and repeat (-qv, -vvv), long options take their value
// after '=' or as the next argument, and "--" ends the options. Global
// options come before a command; each command parses its own options and
// arguments:
//
//   umbra [<global options>] [<command> [<options>] [<arguments>]]
//
// Commands and options are declared in tables, from which the help text is
// generated too.

#ifndef UMBRA_APPS_UMBRA_CLI_PARSER_H_
#define UMBRA_APPS_UMBRA_CLI_PARSER_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::cli {

// What an option takes.
enum class OptionKind : std::uint8_t {
  // Nothing; it may repeat, and its count is how often it was given (-vvv).
  kFlag,
  // A value, after '=' or as the next argument (--color=auto, --color auto).
  kValue,
};

// An option of a command, or a global one.
struct OptionSpec {
  // The short name ('q' for -q), or '\0' for none.
  char short_name = '\0';
  // The long name, without dashes ("quiet" for --quiet).
  std::string_view long_name;
  OptionKind kind = OptionKind::kFlag;
  // For kValue: the placeholder of its value in the help ("when").
  std::string_view value_name;
  // For kValue: the values it accepts, or empty for any.
  std::span<const std::string_view> choices;
  // One line of help.
  std::string_view help;
};

// A flag: -<short_name>, --<long_name>; '\0' for no short name.
constexpr OptionSpec FlagOption(char short_name, std::string_view long_name,
                                std::string_view help) {
  return {
      .short_name = short_name,
      .long_name = long_name,
      .kind = OptionKind::kFlag,
      .value_name = {},
      .choices = {},
      .help = help,
  };
}

// An option with a value, --<long_name>=<value_name>, restricted to `choices`
// unless they are empty.
constexpr OptionSpec ValueOption(char short_name, std::string_view long_name,
                                 std::string_view value_name,
                                 std::span<const std::string_view> choices,
                                 std::string_view help) {
  return {
      .short_name = short_name,
      .long_name = long_name,
      .kind = OptionKind::kValue,
      .value_name = value_name,
      .choices = choices,
      .help = help,
  };
}

// A command, or the program itself for its global options.
struct CommandSpec {
  std::string_view name;
  // One line of help.
  std::string_view summary;
  std::span<const OptionSpec> options;
  // The placeholder of its arguments in the help ("<scene>"), or empty when
  // it takes none.
  std::string_view arguments;
};

// The options and arguments given to one command. Views into the arguments
// passed to Parse.
class UMBRA_POINTER ParsedCommand {
 public:
  // The command's name.
  [[nodiscard]] std::string_view name() const { return name_; }
  // How many times the option with `long_name` was given.
  [[nodiscard]] int Count(std::string_view long_name) const;
  // The last value given to the option with `long_name`, or empty when it was
  // not given.
  [[nodiscard]] std::string_view Value(std::string_view long_name) const;
  // The arguments that are not options, in order.
  [[nodiscard]] std::span<const std::string_view> arguments() const
      UMBRA_LIFETIMEBOUND {
    return arguments_;
  }

 private:
  friend class Parser;

  struct Occurrence {
    std::string_view long_name;
    std::string_view value;
  };

  std::string_view name_;
  std::vector<Occurrence> occurrences_;
  std::vector<std::string_view> arguments_;
};

// A parsed command line: the global options and, when one was given, the
// command.
struct ParsedCommandLine {
  ParsedCommand global;
  // The command, when one was given.
  std::optional<ParsedCommand> command;
};

// Parses `args`, the arguments after the program name, against the program's
// global options and its commands. Fails with kInvalidArgument, naming the
// argument at fault, on an unknown option or command, a missing or rejected
// value, a value given to a flag, or an argument where none is expected.
[[nodiscard]] std::expected<ParsedCommandLine, core::Error> Parse(
    const CommandSpec& program, std::span<const CommandSpec> commands,
    std::span<const char* const> args UMBRA_LIFETIMEBOUND);

// The error for `name` where a command of `program` is expected:
// "'foo' is not an umbra command; see 'umbra --help'".
[[nodiscard]] core::Error NotACommand(const CommandSpec& program,
                                      std::string_view name);

// The help of the program, as git writes its own: the usage line with the
// global options, then the commands with their summaries.
[[nodiscard]] std::string FormatHelp(const CommandSpec& program,
                                     std::span<const CommandSpec> commands);

// The help of one command, as git writes it for `git <command> -h`: the usage
// line, then its options, but for -h itself.
[[nodiscard]] std::string FormatCommandHelp(const CommandSpec& program,
                                            const CommandSpec& command);

}  // namespace umbra::cli

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_CLI_PARSER_H_
