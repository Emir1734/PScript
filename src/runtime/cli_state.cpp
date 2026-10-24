#include "runtime/cli_state.h"
#include "runtime/runtime.h"

#include <sstream>

namespace pscript {
namespace {

[[noreturn]] void cli_fail(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
}

bool is_help_token(const std::string& token) {
    return token == "--help" || token == "-h" || token == "-help";
}

bool is_version_token(const std::string& token) {
    return token == "--version" || token == "-V" || token == "-version";
}

}  // namespace

void CliState::set_help_text(std::string text) {
    help_text_ = std::move(text);
}

void CliState::set_version_text(std::string text) {
    version_text_ = std::move(text);
}

void CliState::register_subcommand(std::string name) {
    subcommands_.push_back(std::move(name));
}

void CliState::register_option(std::string long_name, bool is_flag, bool required,
                                 std::optional<std::string> default_value, std::optional<std::string> short_name) {
    for (const RegisteredOption& existing : options_) {
        if (existing.long_name == long_name) {
            return;
        }
    }
    RegisteredOption option;
    option.long_name = std::move(long_name);
    option.short_name = std::move(short_name);
    option.is_flag = is_flag;
    option.required = required && !is_flag;
    option.default_value = std::move(default_value);
    options_.push_back(std::move(option));
}

const CliState::RegisteredOption* CliState::find_option(std::string_view long_name) const {
    for (const RegisteredOption& option : options_) {
        if (option.long_name == long_name) {
            return &option;
        }
    }
    return nullptr;
}

void CliState::ensure_parsed(Runtime& runtime) {
    if (!parsed_) {
        parse(runtime);
        parsed_ = true;
    }
}

void CliState::parse(Runtime& runtime) {
    const std::vector<std::string>& args = runtime.program_args();
    for (const std::string& token : args) {
        if (is_help_token(token)) {
            print_help(runtime);
            runtime.request_process_exit(0);
            return;
        }
        if (is_version_token(token)) {
            const std::string& version = version_text_.empty() ? Runtime::version_string() : version_text_;
            runtime.output() << version << '\n';
            runtime.request_process_exit(0);
            return;
        }
    }

    values_.clear();
    positionals_.clear();
    active_command_.reset();

    for (const RegisteredOption& option : options_) {
        if (option.is_flag) {
            values_[option.long_name] = std::nullopt;
        } else if (option.default_value.has_value()) {
            values_[option.long_name] = option.default_value;
        }
    }

    std::size_t index = 0;
    while (index < args.size()) {
        const std::string& token = args[index];
        if (token == "--") {
            for (++index; index < args.size(); ++index) {
                positionals_.push_back(args[index]);
            }
            break;
        }
        if (token.rfind("--", 0) == 0) {
            std::string key = token.substr(2);
            std::string value;
            const std::size_t eq = key.find('=');
            if (eq != std::string::npos) {
                value = key.substr(eq + 1);
                key = key.substr(0, eq);
            }
            const RegisteredOption* option = find_option(key);
            if (option == nullptr) {
                cli_fail("unknown option '--" + key + "'");
            }
            if (option->is_flag) {
                if (!value.empty()) {
                    cli_fail("flag '--" + key + "' does not take a value");
                }
                values_[option->long_name] = std::string{};
                ++index;
                continue;
            }
            if (value.empty()) {
                if (index + 1 >= args.size()) {
                    cli_fail("option '--" + key + "' requires a value");
                }
                value = args[index + 1];
                ++index;
            }
            values_[option->long_name] = std::move(value);
            ++index;
            continue;
        }
        if (token.size() >= 2 && token[0] == '-' && token[1] != '-') {
            for (std::size_t i = 1; i < token.size(); ++i) {
                const std::string short_key(1, token[i]);
                const RegisteredOption* option = nullptr;
                for (const RegisteredOption& candidate : options_) {
                    if (candidate.short_name == short_key) {
                        option = &candidate;
                        break;
                    }
                }
                if (option == nullptr) {
                    cli_fail("unknown option '-" + short_key + "'");
                }
                if (option->is_flag) {
                    values_[option->long_name] = std::string{};
                    continue;
                }
                if (i + 1 < token.size()) {
                    values_[option->long_name] = token.substr(i + 1);
                    break;
                }
                if (index + 1 >= args.size()) {
                    cli_fail("option '-" + short_key + "' requires a value");
                }
                values_[option->long_name] = args[index + 1];
                ++index;
                break;
            }
            ++index;
            continue;
        }

        positionals_.push_back(token);
        ++index;
    }

    if (!subcommands_.empty() && !positionals_.empty()) {
        const std::string& candidate = positionals_[0];
        for (const std::string& command : subcommands_) {
            if (command == candidate) {
                active_command_ = candidate;
                positionals_.erase(positionals_.begin());
                break;
            }
        }
    }

    for (const RegisteredOption& option : options_) {
        if (option.required && !values_.contains(option.long_name)) {
            cli_fail("required option '--" + option.long_name + "' is missing");
        }
        if (option.required && values_.contains(option.long_name) && !option.is_flag &&
            !values_[option.long_name].has_value()) {
            cli_fail("required option '--" + option.long_name + "' is missing");
        }
    }
}

std::string CliState::positional_at(std::size_t index) const {
    if (index >= positionals_.size()) {
        cli_fail("argument index out of range");
    }
    return positionals_[index];
}

bool CliState::has_option(std::string_view long_name) const {
    return values_.find(std::string(long_name)) != values_.end();
}

std::optional<std::string> CliState::option_value(std::string_view long_name) const {
    const auto it = values_.find(std::string(long_name));
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool CliState::flag(std::string_view long_name) const {
    const auto it = values_.find(std::string(long_name));
    if (it == values_.end()) {
        return false;
    }
    return it->second.has_value();
}

bool CliState::command_is(std::string_view name) const {
    return active_command_.has_value() && *active_command_ == name;
}

void CliState::print_help(Runtime& runtime) const {
    std::ostringstream out;
    out << "Usage:";
    if (!subcommands_.empty()) {
        out << " <command>";
    }
    out << " [options]";
    if (!positionals_.empty() || subcommands_.empty()) {
        out << " [arguments...]";
    }
    out << '\n';
    if (!help_text_.empty()) {
        out << help_text_ << '\n';
    }
    if (!subcommands_.empty()) {
        out << "\nCommands:\n";
        for (const std::string& command : subcommands_) {
            out << "  " << command << '\n';
        }
    }
    if (!options_.empty()) {
        out << "\nOptions:\n";
        for (const RegisteredOption& option : options_) {
            out << "  --" << option.long_name;
            if (option.short_name.has_value()) {
                out << ", -" << *option.short_name;
            }
            if (option.is_flag) {
                out << "          flag";
            } else if (option.default_value.has_value()) {
                out << "          default: " << *option.default_value;
            } else if (option.required) {
                out << "          required";
            }
            out << '\n';
        }
    }
    out << "  --help, -h       show this help\n";
    out << "  --version, -V    show version\n";
    runtime.output() << out.str();
}

CliState& runtime_cli_state(Runtime& runtime) {
    return runtime.cli_state();
}

}  // namespace pscript
