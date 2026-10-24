#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"
#include "runtime/cli_state.h"

namespace pscript {
namespace {

std::int64_t require_index_arg(const std::vector<Value>& args, std::size_t index) {
    if (args[index].kind != ValueKind::Integer) {
        native_type_error("arg index must be an integer");
    }
    if (args[index].integer < 0) {
        native_value_error("arg index must be non-negative");
    }
    return args[index].integer;
}

}  // namespace

Value native_cli_args(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("args", args, 0);
    CliState& cli = runtime_cli_state(runtime);
    cli.ensure_parsed(runtime);
    std::vector<Value> items;
    for (const std::string& arg : cli.positionals()) {
        items.push_back(Runtime::make_string(arg));
    }
    return Runtime::make_list(std::move(items));
}

Value native_cli_arg(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("arg", args, 1);
    const std::int64_t index = require_index_arg(args, 0);
    CliState& cli = runtime_cli_state(runtime);
    cli.ensure_parsed(runtime);
    return Runtime::make_string(cli.positional_at(static_cast<std::size_t>(index)));
}

Value native_cli_option(Runtime& runtime, const std::vector<Value>& args) {
    native_require_min_argc("option", args, 1);
    if (args.size() > 2) {
        native_value_error("option expects at most 2 arguments");
    }
    const std::string& name = native_require_string_arg(args, 0, "option", "name");
    std::optional<std::string> default_value;
    bool required = true;
    if (args.size() == 2) {
        if (args[1].kind == ValueKind::String) {
            default_value = args[1].string;
            required = false;
        } else if (args[1].kind == ValueKind::Null) {
            required = true;
        } else {
            native_type_error("option default must be a string or null");
        }
    }
    CliState& cli = runtime_cli_state(runtime);
    cli.register_option(name, false, required, default_value);
    cli.ensure_parsed(runtime);
    const std::optional<std::string> value = cli.option_value(name);
    if (!value.has_value()) {
        native_value_error("required option '--" + name + "' is missing");
    }
    return Runtime::make_string(*value);
}

Value native_cli_flag(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("flag", args, 1);
    const std::string& name = native_require_string_arg(args, 0, "flag", "name");
    CliState& cli = runtime_cli_state(runtime);
    cli.register_option(name, true, false, std::nullopt);
    cli.ensure_parsed(runtime);
    return Runtime::make_bool(cli.flag(name));
}

Value native_cli_has(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("has", args, 1);
    const std::string& name = native_require_string_arg(args, 0, "has", "name");
    CliState& cli = runtime_cli_state(runtime);
    cli.ensure_parsed(runtime);
    return Runtime::make_bool(cli.has_option(name));
}

Value native_cli_command(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("command", args, 1);
    const std::string& name = native_require_string_arg(args, 0, "command", "name");
    CliState& cli = runtime_cli_state(runtime);
    cli.register_subcommand(name);
    cli.ensure_parsed(runtime);
    return Runtime::make_bool(cli.command_is(name));
}

Value native_cli_help(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("help", args, 1);
    const std::string& text = native_require_string_arg(args, 0, "help", "text");
    CliState& cli = runtime_cli_state(runtime);
    cli.set_help_text(text);
    cli.ensure_parsed(runtime);
    return Runtime::make_null();
}

Value native_cli_version(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("version", args, 1);
    const std::string& text = native_require_string_arg(args, 0, "version", "text");
    CliState& cli = runtime_cli_state(runtime);
    cli.set_version_text(text);
    cli.ensure_parsed(runtime);
    return Runtime::make_null();
}

}  // namespace pscript
