#include "stdlib/native_common.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace pscript {
namespace {

std::string log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warning:
            return "WARNING";
        case LogLevel::Error:
            return "ERROR";
    }
    return "INFO";
}

std::string format_log_timestamp() {
    using Clock = std::chrono::system_clock;
    const std::time_t seconds = Clock::to_time_t(Clock::now());
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &seconds);
#else
    localtime_r(&seconds, &local_time);
#endif
    std::ostringstream out;
    out << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

void append_structured_fields(std::ostringstream& out, const Value& data) {
    if (data.kind != ValueKind::Dict || !data.dict) {
        return;
    }
    for (const auto& entry : data.dict->entries) {
        out << ' ' << entry.first << '=' << Runtime::format(entry.second);
    }
}

void write_log(Runtime& runtime, LogLevel level, std::string_view function, const std::vector<Value>& args) {
    if (static_cast<int>(level) < static_cast<int>(runtime.log_level())) {
        return;
    }
    native_require_min_argc(function, args, 1);
    if (args.size() > 2) {
        native_value_error(std::string(function) + " expects at most 2 arguments");
    }
    const std::string& message = native_require_string_arg(args, 0, function, "message");
    const Value* fields = nullptr;
    if (args.size() >= 2) {
        if (args[1].kind != ValueKind::Dict) {
            native_type_error("log structured data must be a dict");
        }
        fields = &args[1];
    }

    std::ostringstream line;
    line << format_log_timestamp() << ' ' << log_level_name(level) << ' ';
    if (const std::optional<NativeCallSite>& site = runtime.native_call_site(); site.has_value()) {
        line << site->file << ':' << site->line << ' ';
    }
    line << message;
    if (fields != nullptr) {
        append_structured_fields(line, *fields);
    }
    runtime.error_output() << line.str() << '\n';
    runtime.error_output().flush();
}

}  // namespace

Value native_log_debug(Runtime& runtime, const std::vector<Value>& args) {
    write_log(runtime, LogLevel::Debug, "debug", args);
    return Runtime::make_null();
}

Value native_log_info(Runtime& runtime, const std::vector<Value>& args) {
    write_log(runtime, LogLevel::Info, "info", args);
    return Runtime::make_null();
}

Value native_log_warning(Runtime& runtime, const std::vector<Value>& args) {
    write_log(runtime, LogLevel::Warning, "warning", args);
    return Runtime::make_null();
}

Value native_log_error(Runtime& runtime, const std::vector<Value>& args) {
    write_log(runtime, LogLevel::Error, "error", args);
    return Runtime::make_null();
}

Value native_log_set_level(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("set_level", args, 1);
    const std::string& level_text = native_require_string_arg(args, 0, "set_level", "level");
    LogLevel level = LogLevel::Info;
    if (!Runtime::parse_log_level(level_text, level)) {
        native_value_error("invalid log level '" + level_text + "'");
    }
    runtime.set_log_level(level);
    return Runtime::make_null();
}

}  // namespace pscript
