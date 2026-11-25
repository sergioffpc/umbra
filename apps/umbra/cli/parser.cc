// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "cli/parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "umbra/core/error.h"

namespace umbra::cli {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

int ParsedCommand::Count(std::string_view long_name) const {
  return static_cast<int>(
      std::ranges::count(occurrences_, long_name, &Occurrence::long_name));
}

std::string_view ParsedCommand::Value(std::string_view long_name) const {
  const auto last = std::ranges::find(occurrences_ | std::views::reverse,
                                      long_name, &Occurrence::long_name);
  return last == std::ranges::rend(occurrences_) ? std::string_view()
                                                 : last->value;
}

// Parses the arguments of one command at a time. Kept as a class so that
// ParsedCommand can grant it access to its fields.
class Parser {
 public:
  // `command` names the command in messages; it is empty for the global
  // options.
  Parser(const CommandSpec& spec, std::vector<std::string_view> args,
         std::string_view command)
      : spec_(&spec), args_(std::move(args)), command_(command) {
    parsed_.name_ = spec.name;
  }

  // Parses options and arguments until the end, or, when `commands` is not
  // empty, until the first argument, which names a command and is left for
  // its own parser. Returns how many arguments it consumed.
  std::expected<std::size_t, Error> Run(std::span<const CommandSpec> commands);

  [[nodiscard]] ParsedCommand& parsed() { return parsed_; }

 private:
  // Whether `arg`, at the current position, is an option (or "--").
  [[nodiscard]] bool IsOption(std::string_view arg) const;
  // Consumes `arg`, the argument at the current position, as an option or as
  // an argument of the command.
  std::expected<void, Error> Step(std::string_view arg);
  std::expected<void, Error> ParseOption(std::string_view arg);
  std::expected<void, Error> AddArgument(std::string_view arg);
  std::expected<void, Error> ParseLong(std::string_view arg);
  std::expected<void, Error> ParseShorts(std::string_view arg);
  [[nodiscard]] std::expected<const OptionSpec*, Error> FindShort(
      char name) const;
  // Adds a kValue option, with its value from TakeValue.
  std::expected<void, Error> AddValue(
      const OptionSpec& option, std::optional<std::string_view> inline_value);
  // The value of a kValue option: `inline_value` when it was given after '='
  // or in the same short argument, otherwise the next argument.
  std::expected<std::string_view, Error> TakeValue(
      const OptionSpec& option, std::optional<std::string_view> inline_value);
  std::expected<void, Error> Add(const OptionSpec& option,
                                 std::string_view value);
  [[nodiscard]] Error Fail(std::string message) const;

  // Whether "--" has ended the options.
  enum class Options : std::uint8_t {
    kOpen,
    kEnded,
  };

  const CommandSpec* spec_;
  std::vector<std::string_view> args_;
  std::string_view command_;
  std::size_t next_ = 0;
  Options options_ = Options::kOpen;
  ParsedCommand parsed_;
};

namespace {

// How an option is written in messages: --long, or -s when it has no long
// name.
std::string OptionName(const OptionSpec& option) {
  return option.long_name.empty() ? std::format("-{}", option.short_name)
                                  : std::format("--{}", option.long_name);
}

// Joins choices as "a, b or c".
std::string JoinChoices(std::span<const std::string_view> choices) {
  std::string joined;
  std::size_t left = choices.size();
  for (const std::string_view choice : choices) {
    joined += choice;
    --left;
    if (left > 0) {
      joined += left == 1 ? " or " : ", ";
    }
  }
  return joined;
}

}  // namespace

Error Parser::Fail(std::string message) const {
  if (!command_.empty()) {
    message = std::format("{}: {}", command_, message);
  }
  return {ErrorCode::kInvalidArgument, std::move(message)};
}

std::expected<std::size_t, Error> Parser::Run(
    std::span<const CommandSpec> commands) {
  while (next_ < args_.size()) {
    const std::string_view arg = args_.at(next_);
    if (!IsOption(arg) && !commands.empty()) {
      return next_;
    }
    if (auto step = Step(arg); !step.has_value()) {
      return std::unexpected(std::move(step.error()));
    }
  }
  return next_;
}

std::expected<void, Error> Parser::Step(std::string_view arg) {
  if (!IsOption(arg)) {
    return AddArgument(arg);
  }
  ++next_;
  return ParseOption(arg);
}

bool Parser::IsOption(std::string_view arg) const {
  return options_ == Options::kOpen && arg.starts_with('-') && arg != "-";
}

std::expected<void, Error> Parser::ParseOption(std::string_view arg) {
  if (arg == "--") {
    options_ = Options::kEnded;
    return {};
  }
  return arg.starts_with("--") ? ParseLong(arg.substr(2))
                               : ParseShorts(arg.substr(1));
}

std::expected<void, Error> Parser::AddArgument(std::string_view arg) {
  if (spec_->arguments.empty()) {
    return std::unexpected(Fail(std::format("unexpected argument '{}'", arg)));
  }
  parsed_.arguments_.push_back(arg);
  ++next_;
  return {};
}

std::expected<void, Error> Parser::ParseLong(std::string_view arg) {
  const std::size_t equals = arg.find('=');
  const std::string_view name = arg.substr(0, equals);
  const auto option =
      std::ranges::find(spec_->options, name, &OptionSpec::long_name);
  if (name.empty() || option == spec_->options.end()) {
    return std::unexpected(Fail(std::format("unknown option '--{}'", name)));
  }
  const std::optional<std::string_view> inline_value =
      equals == std::string_view::npos
          ? std::nullopt
          : std::optional<std::string_view>(arg.substr(equals + 1));
  if (option->kind == OptionKind::kFlag) {
    if (inline_value.has_value()) {
      return std::unexpected(
          Fail(std::format("option '--{}' takes no value", name)));
    }
    return Add(*option, {});
  }
  return AddValue(*option, inline_value);
}

std::expected<void, Error> Parser::ParseShorts(std::string_view arg) {
  while (!arg.empty()) {
    const char name = arg.front();
    arg.remove_prefix(1);
    auto option = FindShort(name);
    if (!option.has_value()) {
      return std::unexpected(std::move(option.error()));
    }
    if ((*option)->kind == OptionKind::kValue) {
      // The rest of the argument, if any, is the value (-ofile).
      return AddValue(**option,
                      arg.empty() ? std::nullopt : std::optional(arg));
    }
    if (auto added = Add(**option, {}); !added.has_value()) {
      return added;
    }
  }
  return {};
}

std::expected<const OptionSpec*, Error> Parser::FindShort(char name) const {
  const auto option =
      std::ranges::find(spec_->options, name, &OptionSpec::short_name);
  if (name == '\0' || option == spec_->options.end()) {
    return std::unexpected(Fail(std::format("unknown option '-{}'", name)));
  }
  return &*option;
}

std::expected<void, Error> Parser::AddValue(
    const OptionSpec& option, std::optional<std::string_view> inline_value) {
  return TakeValue(option, inline_value).and_then([&](std::string_view value) {
    return Add(option, value);
  });
}

std::expected<std::string_view, Error> Parser::TakeValue(
    const OptionSpec& option, std::optional<std::string_view> inline_value) {
  if (inline_value.has_value()) {
    return *inline_value;
  }
  if (next_ == args_.size()) {
    return std::unexpected(
        Fail(std::format("option '{}' expects a value", OptionName(option))));
  }
  return args_.at(next_++);
}

std::expected<void, Error> Parser::Add(const OptionSpec& option,
                                       std::string_view value) {
  if (option.kind == OptionKind::kValue && !option.choices.empty() &&
      !std::ranges::contains(option.choices, value)) {
    return std::unexpected(
        Fail(std::format("option '{}' expects {}, got '{}'", OptionName(option),
                         JoinChoices(option.choices), value)));
  }
  parsed_.occurrences_.push_back(
      {.long_name = option.long_name, .value = value});
  return {};
}

Error NotACommand(const CommandSpec& program, std::string_view name) {
  return {ErrorCode::kInvalidArgument,
          std::format("'{}' is not an {} command; see '{} --help'", name,
                      program.name, program.name)};
}

std::expected<ParsedCommandLine, Error> Parse(
    const CommandSpec& program, std::span<const CommandSpec> commands,
    std::span<const char* const> args) {
  const std::vector<std::string_view> views(args.begin(), args.end());
  Parser global(program, views, {});
  auto consumed = global.Run(commands);
  if (!consumed.has_value()) {
    return std::unexpected(std::move(consumed.error()));
  }
  ParsedCommandLine line{
      .global = std::move(global.parsed()),
      .command = std::nullopt,
  };
  if (*consumed == views.size()) {
    return line;
  }
  const std::string_view name = views.at(*consumed);
  const auto command = std::ranges::find(commands, name, &CommandSpec::name);
  if (command == commands.end()) {
    return std::unexpected(NotACommand(program, name));
  }
  Parser parser(
      *command,
      {std::next(views.begin(), static_cast<std::ptrdiff_t>(*consumed + 1)),
       views.end()},
      name);
  if (auto ran = parser.Run({}); !ran.has_value()) {
    return std::unexpected(std::move(ran.error()));
  }
  line.command = std::move(parser.parsed());
  return line;
}

namespace {

// The width of a usage line before it wraps.
constexpr std::size_t kUsageWidth = 80;
// The column an option's help starts at.
constexpr std::size_t kOptionsWidth = 26;
constexpr std::size_t kOptionsGap = 2;
constexpr std::string_view kOptionsIndent = "    ";

bool IsHelp(const OptionSpec& option) { return option.long_name == "help"; }

// An option in a usage line: "[-q | --quiet]", "[--devices=<list>]".
std::string UsageToken(const OptionSpec& option) {
  const std::string long_form =
      option.kind == OptionKind::kValue
          ? std::format("--{}=<{}>", option.long_name, option.value_name)
          : std::format("--{}", option.long_name);
  if (option.short_name == '\0') {
    return std::format("[{}]", long_form);
  }
  const std::string short_form =
      option.kind == OptionKind::kValue
          ? std::format("-{} <{}>", option.short_name, option.value_name)
          : std::format("-{}", option.short_name);
  return std::format("[{} | {}]", short_form, long_form);
}

// "usage: <prefix> <tokens>", wrapped under the first token.
std::string UsageLines(std::string_view prefix,
                       std::span<const std::string> tokens) {
  std::string text = std::format("usage: {}", prefix);
  const std::size_t indent = text.size() + 1;
  std::size_t column = text.size();
  for (const std::string& token : tokens) {
    if (column > indent && column + 1 + token.size() > kUsageWidth) {
      text += '\n' + std::string(indent, ' ') + token;
      column = indent + token.size();
    } else {
      text += ' ' + token;
      column += 1 + token.size();
    }
  }
  return text + '\n';
}

// The usage tokens of a command's options, and of its arguments; the help
// option named `help_name`, if any, is left out.
std::vector<std::string> OptionTokens(const CommandSpec& spec,
                                      std::string_view help_name) {
  std::vector<std::string> tokens;
  for (const OptionSpec& option : spec.options) {
    if (option.long_name != help_name) {
      tokens.push_back(UsageToken(option));
    }
  }
  if (!spec.arguments.empty()) {
    tokens.emplace_back(spec.arguments);
  }
  return tokens;
}

// One option's line in a command's help:
// "    --devices=<list>      render on these devices".
std::string OptionHelpLine(const OptionSpec& option) {
  std::string names(kOptionsIndent);
  if (option.short_name != '\0') {
    std::format_to(std::back_inserter(names), "-{}, ", option.short_name);
  }
  std::format_to(std::back_inserter(names), "--{}", option.long_name);
  if (option.kind == OptionKind::kValue) {
    std::format_to(std::back_inserter(names), "=<{}>", option.value_name);
  }
  if (names.size() <= kOptionsWidth) {
    return std::format("{:<{}}{}\n", names, kOptionsWidth + kOptionsGap,
                       option.help);
  }
  return std::format("{}\n{:<{}}{}\n", names, "", kOptionsWidth + kOptionsGap,
                     option.help);
}

}  // namespace

std::string FormatHelp(const CommandSpec& program,
                       std::span<const CommandSpec> commands) {
  std::string help = UsageLines(program.name, OptionTokens(program, {}));
  const std::size_t indent =
      std::string_view("usage: ").size() + program.name.size() + 1;
  std::format_to(std::back_inserter(help), "{:<{}}<command> [<args>]\n\n", "",
                 indent);
  std::format_to(std::back_inserter(help), "These are the {} commands:\n",
                 program.name);
  const std::size_t width = std::ranges::max(
      commands | std::views::transform([](const CommandSpec& command) {
        return command.name.size();
      }));
  for (const CommandSpec& command : commands) {
    std::format_to(std::back_inserter(help), "   {:<{}}   {}\n", command.name,
                   width, command.summary);
  }
  std::format_to(std::back_inserter(help),
                 "\nSee '{} help <command>' to read about a specific "
                 "command.\n",
                 program.name);
  return help;
}

std::string FormatCommandHelp(const CommandSpec& program,
                              const CommandSpec& command) {
  std::string help =
      UsageLines(std::format("{} {}", program.name, command.name),
                 OptionTokens(command, "help"));
  if (std::ranges::all_of(command.options, IsHelp)) {
    return help;
  }
  help += '\n';
  for (const OptionSpec& option : command.options) {
    if (!IsHelp(option)) {
      help += OptionHelpLine(option);
    }
  }
  return help;
}

}  // namespace umbra::cli
