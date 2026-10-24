#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

class Runtime;

class CliState {
    struct RegisteredOption {
        std::string long_name;
        std::optional<std::string> short_name;
        bool is_flag = false;
        bool required = false;
        std::optional<std::string> default_value;
    };

public:
    void set_help_text(std::string text);
    void set_version_text(std::string text);
    void register_subcommand(std::string name);

    void register_option(std::string long_name, bool is_flag, bool required, std::optional<std::string> default_value,
                         std::optional<std::string> short_name = std::nullopt);

    void ensure_parsed(Runtime& runtime);

    const std::vector<std::string>& positionals() const { return positionals_; }
    std::string positional_at(std::size_t index) const;
    bool has_option(std::string_view long_name) const;
    std::optional<std::string> option_value(std::string_view long_name) const;
    bool flag(std::string_view long_name) const;
    bool command_is(std::string_view name) const;
    const std::optional<std::string>& active_command() const { return active_command_; }

    void print_help(Runtime& runtime) const;

private:
    void parse(Runtime& runtime);
    const RegisteredOption* find_option(std::string_view long_name) const;

    std::string help_text_;
    std::string version_text_;
    std::vector<std::string> subcommands_;
    std::vector<RegisteredOption> options_;
    bool parsed_ = false;
    std::vector<std::string> positionals_;
    std::map<std::string, std::optional<std::string>> values_;
    std::optional<std::string> active_command_;
};

CliState& runtime_cli_state(Runtime& runtime);

}  // namespace pscript
