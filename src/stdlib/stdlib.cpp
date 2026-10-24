#include "stdlib/stdlib.h"

#include "runtime/runtime.h"

#include <cmath>
#include <string_view>

namespace pscript {
namespace {

std::string stdlib_canonical_path(const std::string& specifier) {
    return "<stdlib/" + specifier + ">";
}

FunctionInfo make_native_fn(std::string name, std::uint32_t arity, NativeFunctionId id, bool returns_value = true) {
    FunctionInfo fn;
    fn.name = std::move(name);
    fn.arity = arity;
    fn.returns_value = returns_value;
    fn.is_native = true;
    fn.native_function_id = static_cast<std::uint32_t>(id);
    return fn;
}

ClassInfo make_native_class(std::string name, NativeClassKind kind,
                            const std::vector<std::string>& method_names) {
    ClassInfo cls;
    cls.name = std::move(name);
    cls.native_class_kind = static_cast<std::uint32_t>(kind);
    cls.methods.reserve(method_names.size());
    for (const std::string& method_name : method_names) {
        cls.methods.push_back(ClassMethodInfo{method_name, 0xFFFFFFFFu});
    }
    return cls;
}

void attach_native_class_methods(Bytecode& bc, ClassInfo& cls) {
    for (ClassMethodInfo& method : cls.methods) {
        FunctionInfo fn;
        fn.name = method.name;
        fn.arity = 0;
        fn.returns_value = true;
        if (method.name == "push" || method.name == "enqueue" || method.name == "push_front" ||
            method.name == "push_back" || method.name == "write") {
            fn.arity = 1;
            fn.returns_value = false;
        }
        if (method.name == "flush" || method.name == "close") {
            fn.returns_value = false;
        }
        method.function_index = static_cast<std::uint32_t>(bc.functions.size());
        bc.functions.push_back(std::move(fn));
    }
}

ClassInfo make_native_class_with_fields(std::string name, NativeClassKind kind,
                                        const std::vector<std::string>& field_names,
                                        const std::vector<std::string>& method_names) {
    ClassInfo cls = make_native_class(std::move(name), kind, method_names);
    cls.layout.reserve(field_names.size());
    for (const std::string& field_name : field_names) {
        ClassFieldLayout layout;
        layout.name = field_name;
        layout.declaring_class_index = static_cast<std::uint32_t>(0);
        cls.layout.push_back(std::move(layout));
    }
    return cls;
}

Bytecode build_math_module() {
    Bytecode bc;
    bc.variables.push_back(Variable{"pi", std::nullopt});
    bc.variables.push_back(Variable{"e", std::nullopt});

    const NativeFunctionId ids[] = {
        NativeFunctionId::MathSqrt, NativeFunctionId::MathPow,   NativeFunctionId::MathFloor,
        NativeFunctionId::MathCeil, NativeFunctionId::MathSin,    NativeFunctionId::MathCos,
        NativeFunctionId::MathTan,  NativeFunctionId::MathAsin,   NativeFunctionId::MathAcos,
        NativeFunctionId::MathAtan, NativeFunctionId::MathAtan2, NativeFunctionId::MathLog,
        NativeFunctionId::MathLog10, NativeFunctionId::MathExp,
    };
    const char* names[] = {"sqrt", "pow", "floor", "ceil", "sin", "cos", "tan",
                           "asin", "acos", "atan", "atan2", "log", "log10", "exp"};
    const std::uint32_t arities[] = {1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1};

    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bc.functions.push_back(make_native_fn(names[i], arities[i], ids[i]));
    }

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;

    bc.modules.clear();
    return bc;
}

std::vector<ModuleExport> math_exports() {
    std::vector<ModuleExport> exports;
    exports.push_back(ModuleExport{"pi", ExportKind::Variable, 0});
    exports.push_back(ModuleExport{"e", ExportKind::Variable, 1});
    const char* names[] = {"sqrt", "pow", "floor", "ceil", "sin", "cos", "tan",
                           "asin", "acos", "atan", "atan2", "log", "log10", "exp"};
    for (std::uint32_t i = 0; i < 14; ++i) {
        exports.push_back(ModuleExport{names[i], ExportKind::Function, i});
    }
    return exports;
}

Bytecode build_string_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("join", 2, NativeFunctionId::StringJoin));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> string_exports() {
    return {ModuleExport{"join", ExportKind::Function, 0}};
}

Bytecode build_collections_module() {
    Bytecode bc;

    ClassInfo stack = make_native_class("Stack", NativeClassKind::Stack, {"push", "pop", "peek", "is_empty", "length"});
    attach_native_class_methods(bc, stack);
    bc.classes.push_back(std::move(stack));

    ClassInfo queue =
        make_native_class("Queue", NativeClassKind::Queue, {"enqueue", "dequeue", "peek", "is_empty", "length"});
    attach_native_class_methods(bc, queue);
    bc.classes.push_back(std::move(queue));

    ClassInfo deque = make_native_class(
        "Deque", NativeClassKind::Deque,
        {"push_front", "push_back", "pop_front", "pop_back", "front", "back", "is_empty", "length"});
    attach_native_class_methods(bc, deque);
    bc.classes.push_back(std::move(deque));

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> collections_exports() {
    return {
        ModuleExport{"Stack", ExportKind::Class, 0},
        ModuleExport{"Queue", ExportKind::Class, 1},
        ModuleExport{"Deque", ExportKind::Class, 2},
    };
}

Bytecode build_path_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {
        NativeFunctionId::PathJoin,       NativeFunctionId::PathBasename, NativeFunctionId::PathDirname,
        NativeFunctionId::PathExtension,  NativeFunctionId::PathStem,     NativeFunctionId::PathNormalize,
        NativeFunctionId::PathAbsolute,   NativeFunctionId::PathResolve,  NativeFunctionId::PathIsAbsolute,
        NativeFunctionId::PathIsRelative, NativeFunctionId::PathRelative,
    };
    const char* names[] = {"join", "basename", "dirname", "extension", "stem", "normalize",
                           "absolute", "resolve", "is_absolute", "is_relative", "relative"};
    const std::uint32_t arities[] = {2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2};

    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bc.functions.push_back(make_native_fn(names[i], arities[i], ids[i]));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> path_exports() {
    const char* names[] = {"join", "basename", "dirname", "extension", "stem", "normalize",
                           "absolute", "resolve", "is_absolute", "is_relative", "relative"};
    std::vector<ModuleExport> exports;
    exports.reserve(sizeof(names) / sizeof(names[0]));
    for (std::uint32_t i = 0; i < 11; ++i) {
        exports.push_back(ModuleExport{names[i], ExportKind::Function, i});
    }
    return exports;
}

Bytecode build_fs_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {
        NativeFunctionId::FsExists,    NativeFunctionId::FsIsFile,   NativeFunctionId::FsIsDir,
        NativeFunctionId::FsRead,    NativeFunctionId::FsWrite,    NativeFunctionId::FsAppend,
        NativeFunctionId::FsCopy,    NativeFunctionId::FsMove,     NativeFunctionId::FsRemove,
        NativeFunctionId::FsCreateDir, NativeFunctionId::FsRemoveDir, NativeFunctionId::FsListDir,
        NativeFunctionId::FsAsyncRead, NativeFunctionId::FsAsyncWrite, NativeFunctionId::FsAsyncAppend,
    };
    const char* names[] = {"exists", "is_file", "is_dir", "read", "write", "append",
                           "copy", "move", "remove", "create_dir", "remove_dir", "list_dir",
                           "async_read", "async_write", "async_append"};
    const std::uint32_t arities[] = {1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 2, 2};

    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bc.functions.push_back(make_native_fn(names[i], arities[i], ids[i]));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> fs_exports() {
    const char* names[] = {"exists", "is_file", "is_dir", "read", "write", "append",
                           "copy", "move", "remove", "create_dir", "remove_dir", "list_dir",
                           "async_read", "async_write", "async_append"};
    std::vector<ModuleExport> exports;
    exports.reserve(sizeof(names) / sizeof(names[0]));
    for (std::uint32_t i = 0; i < 15; ++i) {
        exports.push_back(ModuleExport{names[i], ExportKind::Function, i});
    }
    return exports;
}

Bytecode build_io_module() {
    Bytecode bc;

    bc.functions.push_back(make_native_fn("open", 2, NativeFunctionId::IoOpen));

    ClassInfo file_handle =
        make_native_class("FileHandle", NativeClassKind::FileHandle,
                          {"read", "write", "flush", "close", "is_open", "async_read", "async_write"});
    attach_native_class_methods(bc, file_handle);
    bc.classes.push_back(std::move(file_handle));

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> io_exports() {
    return {ModuleExport{"open", ExportKind::Function, 0}};
}

Bytecode build_os_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {
        NativeFunctionId::OsName,       NativeFunctionId::OsPlatform, NativeFunctionId::OsArch,
        NativeFunctionId::OsCwd,        NativeFunctionId::OsChdir,    NativeFunctionId::OsEnv,
        NativeFunctionId::OsSetEnv,     NativeFunctionId::OsRemoveEnv, NativeFunctionId::OsHasEnv,
        NativeFunctionId::OsHomeDir,    NativeFunctionId::OsTempDir,
    };
    const char* names[] = {"name", "platform", "arch", "cwd", "chdir", "env",
                           "set_env", "remove_env", "has_env", "home_dir", "temp_dir"};
    const std::uint32_t arities[] = {0, 0, 0, 0, 1, 0, 2, 1, 1, 0, 0};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        FunctionInfo fn = make_native_fn(names[i], arities[i], ids[i]);
        if (names[i] == "set_env" || names[i] == "remove_env" || names[i] == "chdir") {
            fn.returns_value = false;
        }
        bc.functions.push_back(std::move(fn));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> os_exports() {
    const char* names[] = {"name", "platform", "arch", "cwd", "chdir", "env",
                           "set_env", "remove_env", "has_env", "home_dir", "temp_dir"};
    std::vector<ModuleExport> exports;
    for (std::uint32_t i = 0; i < 11; ++i) {
        exports.push_back(ModuleExport{names[i], ExportKind::Function, i});
    }
    return exports;
}

Bytecode build_process_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("id", 0, NativeFunctionId::ProcessId));
    FunctionInfo exit_fn = make_native_fn("exit", 1, NativeFunctionId::ProcessExit, false);
    bc.functions.push_back(std::move(exit_fn));
    bc.functions.push_back(make_native_fn("run", 1, NativeFunctionId::ProcessRun));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> process_exports() {
    return {
        ModuleExport{"id", ExportKind::Function, 0},
        ModuleExport{"exit", ExportKind::Function, 1},
        ModuleExport{"run", ExportKind::Function, 2},
    };
}

Bytecode build_sys_module() {
    Bytecode bc;
    bc.variables.push_back(Variable{"args", std::nullopt});
    bc.variables.push_back(Variable{"version", std::nullopt});
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> sys_exports() {
    return {
        ModuleExport{"args", ExportKind::Variable, 0},
        ModuleExport{"version", ExportKind::Variable, 1},
    };
}

Bytecode build_time_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {NativeFunctionId::TimeNow, NativeFunctionId::TimeUnix,
                                    NativeFunctionId::TimeMonotonic, NativeFunctionId::TimeSleep};
    const char* names[] = {"now", "unix", "monotonic", "sleep"};
    const std::uint32_t arities[] = {0, 0, 0, 1};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        FunctionInfo fn = make_native_fn(names[i], arities[i], ids[i]);
        if (std::string_view(names[i]) == "sleep") {
            fn.returns_value = false;
        }
        bc.functions.push_back(std::move(fn));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> time_exports() {
    return {
        ModuleExport{"now", ExportKind::Function, 0},
        ModuleExport{"unix", ExportKind::Function, 1},
        ModuleExport{"monotonic", ExportKind::Function, 2},
        ModuleExport{"sleep", ExportKind::Function, 3},
    };
}

Bytecode build_datetime_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {NativeFunctionId::DateTimeNow, NativeFunctionId::DateTimeToday,
                                    NativeFunctionId::DateTimeUtcNow, NativeFunctionId::DateTimeFromUnix,
                                    NativeFunctionId::DateTimeCreate, NativeFunctionId::DateTimeParse};
    const char* names[] = {"now", "today", "utc_now", "from_unix", "create", "parse"};
    const std::uint32_t arities[] = {0, 0, 0, 1, 6, 1};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bc.functions.push_back(make_native_fn(names[i], arities[i], ids[i]));
    }

    ClassInfo datetime =
        make_native_class_with_fields("DateTime", NativeClassKind::DateTime,
                                      {"year", "month", "day", "hour", "minute", "second"}, {"format"});
    attach_native_class_methods(bc, datetime);
    bc.classes.push_back(std::move(datetime));

    ClassInfo duration = make_native_class_with_fields("Duration", NativeClassKind::Duration, {"seconds"}, {});
    bc.classes.push_back(std::move(duration));

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> datetime_exports() {
    return {
        ModuleExport{"now", ExportKind::Function, 0},
        ModuleExport{"today", ExportKind::Function, 1},
        ModuleExport{"utc_now", ExportKind::Function, 2},
        ModuleExport{"from_unix", ExportKind::Function, 3},
        ModuleExport{"create", ExportKind::Function, 4},
        ModuleExport{"parse", ExportKind::Function, 5},
        ModuleExport{"DateTime", ExportKind::Class, 0},
        ModuleExport{"Duration", ExportKind::Class, 1},
    };
}

Bytecode build_random_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("int", 2, NativeFunctionId::RandomInt));
    bc.functions.push_back(make_native_fn("float", 0, NativeFunctionId::RandomFloat));
    bc.functions.push_back(make_native_fn("choice", 1, NativeFunctionId::RandomChoice));
    FunctionInfo shuffle = make_native_fn("shuffle", 1, NativeFunctionId::RandomShuffle, false);
    FunctionInfo seed = make_native_fn("seed", 1, NativeFunctionId::RandomSeed, false);
    bc.functions.push_back(std::move(shuffle));
    bc.functions.push_back(std::move(seed));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> random_exports() {
    return {
        ModuleExport{"int", ExportKind::Function, 0},
        ModuleExport{"float", ExportKind::Function, 1},
        ModuleExport{"choice", ExportKind::Function, 2},
        ModuleExport{"shuffle", ExportKind::Function, 3},
        ModuleExport{"seed", ExportKind::Function, 4},
    };
}

Bytecode build_json_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("encode", 1, NativeFunctionId::JsonEncode));
    bc.functions.push_back(make_native_fn("decode", 1, NativeFunctionId::JsonDecode));
    bc.functions.push_back(make_native_fn("valid", 1, NativeFunctionId::JsonValid));
    bc.functions.push_back(make_native_fn("read", 1, NativeFunctionId::JsonRead));
    FunctionInfo write = make_native_fn("write", 2, NativeFunctionId::JsonWrite, false);
    bc.functions.push_back(std::move(write));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> json_exports() {
    return {
        ModuleExport{"encode", ExportKind::Function, 0},
        ModuleExport{"decode", ExportKind::Function, 1},
        ModuleExport{"valid", ExportKind::Function, 2},
        ModuleExport{"read", ExportKind::Function, 3},
        ModuleExport{"write", ExportKind::Function, 4},
    };
}

Bytecode build_re_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("is_match", 2, NativeFunctionId::ReIsMatch));
    bc.functions.push_back(make_native_fn("search", 2, NativeFunctionId::ReSearch));
    bc.functions.push_back(make_native_fn("find_all", 2, NativeFunctionId::ReFindAll));
    bc.functions.push_back(make_native_fn("replace", 3, NativeFunctionId::ReReplace));
    bc.functions.push_back(make_native_fn("split", 2, NativeFunctionId::ReSplit));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> re_exports() {
    return {
        ModuleExport{"is_match", ExportKind::Function, 0},
        ModuleExport{"search", ExportKind::Function, 1},
        ModuleExport{"find_all", ExportKind::Function, 2},
        ModuleExport{"replace", ExportKind::Function, 3},
        ModuleExport{"split", ExportKind::Function, 4},
    };
}

Bytecode build_encoding_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("encode", 1, NativeFunctionId::EncodingEncode));
    bc.functions.push_back(make_native_fn("decode", 1, NativeFunctionId::EncodingDecode));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> encoding_exports() {
    return {
        ModuleExport{"encode", ExportKind::Function, 0},
        ModuleExport{"decode", ExportKind::Function, 1},
    };
}

void attach_net_class_methods(Bytecode& bc, ClassInfo& cls) {
    for (ClassMethodInfo& method : cls.methods) {
        FunctionInfo fn;
        fn.name = method.name;
        fn.arity = 0;
        fn.returns_value = true;
        if (method.name == "write") {
            fn.arity = 1;
        } else if (method.name == "send") {
            fn.arity = 3;
        } else if (method.name == "close") {
            fn.returns_value = false;
        }
        method.function_index = static_cast<std::uint32_t>(bc.functions.size());
        bc.functions.push_back(std::move(fn));
    }
}

Bytecode build_net_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("connect", 2, NativeFunctionId::NetConnect));
    bc.functions.push_back(make_native_fn("listen", 2, NativeFunctionId::NetListen));
    bc.functions.push_back(make_native_fn("udp", 0, NativeFunctionId::NetUdp));
    bc.functions.push_back(make_native_fn("resolve", 1, NativeFunctionId::NetResolve));

    ClassInfo connection =
        make_native_class("Connection", NativeClassKind::TcpConnection, {"read", "write", "close", "is_open"});
    attach_net_class_methods(bc, connection);
    bc.classes.push_back(std::move(connection));

    ClassInfo server = make_native_class("Server", NativeClassKind::TcpServer, {"accept", "close", "is_open", "local_port"});
    attach_net_class_methods(bc, server);
    bc.classes.push_back(std::move(server));

    ClassInfo udp_socket =
        make_native_class("UdpSocket", NativeClassKind::UdpSocket, {"send", "receive", "close", "is_open", "local_port"});
    attach_net_class_methods(bc, udp_socket);
    bc.classes.push_back(std::move(udp_socket));

    ClassInfo packet = make_native_class_with_fields("Packet", NativeClassKind::None, {"data", "host", "port"}, {});
    bc.classes.push_back(std::move(packet));

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> net_exports() {
    return {
        ModuleExport{"connect", ExportKind::Function, 0},
        ModuleExport{"listen", ExportKind::Function, 1},
        ModuleExport{"udp", ExportKind::Function, 2},
        ModuleExport{"resolve", ExportKind::Function, 3},
        ModuleExport{"Connection", ExportKind::Class, 0},
        ModuleExport{"Server", ExportKind::Class, 1},
        ModuleExport{"UdpSocket", ExportKind::Class, 2},
        ModuleExport{"Packet", ExportKind::Class, 3},
    };
}

void attach_http_class_methods(Bytecode& bc, ClassInfo& cls) {
    for (ClassMethodInfo& method : cls.methods) {
        FunctionInfo fn;
        fn.name = method.name;
        fn.arity = 0;
        fn.returns_value = true;
        if (method.name == "get" || method.name == "post") {
            fn.arity = 2;
        } else if (method.name == "close" || method.name == "start") {
            fn.returns_value = false;
        }
        method.function_index = static_cast<std::uint32_t>(bc.functions.size());
        bc.functions.push_back(std::move(fn));
    }
}

Bytecode build_http_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("get", 1, NativeFunctionId::HttpGet));
    bc.functions.push_back(make_native_fn("post", 2, NativeFunctionId::HttpPost));
    bc.functions.push_back(make_native_fn("request", 4, NativeFunctionId::HttpRequest));
    bc.functions.push_back(make_native_fn("server", 1, NativeFunctionId::HttpServer));

    ClassInfo response = make_native_class_with_fields("Response", NativeClassKind::HttpResponse,
                                                       {"status", "headers", "body"}, {});
    bc.classes.push_back(std::move(response));

    ClassInfo request = make_native_class_with_fields("Request", NativeClassKind::HttpRequest,
                                                      {"method", "path", "headers", "body"}, {});
    bc.classes.push_back(std::move(request));

    ClassInfo server = make_native_class("HttpServer", NativeClassKind::HttpServer,
                                         {"get", "post", "start", "close", "is_open", "local_port"});
    attach_http_class_methods(bc, server);
    bc.classes.push_back(std::move(server));

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> http_exports() {
    return {
        ModuleExport{"get", ExportKind::Function, 0},
        ModuleExport{"post", ExportKind::Function, 1},
        ModuleExport{"request", ExportKind::Function, 2},
        ModuleExport{"server", ExportKind::Function, 3},
        ModuleExport{"Response", ExportKind::Class, 0},
        ModuleExport{"Request", ExportKind::Class, 1},
        ModuleExport{"HttpServer", ExportKind::Class, 2},
    };
}

Bytecode build_log_module() {
    Bytecode bc;
    bc.variables.push_back(Variable{"DEBUG", std::nullopt});
    bc.variables.push_back(Variable{"INFO", std::nullopt});
    bc.variables.push_back(Variable{"WARNING", std::nullopt});
    bc.variables.push_back(Variable{"ERROR", std::nullopt});
    const NativeFunctionId ids[] = {NativeFunctionId::LogDebug, NativeFunctionId::LogInfo, NativeFunctionId::LogWarning,
                                    NativeFunctionId::LogError, NativeFunctionId::LogSetLevel};
    const char* names[] = {"debug", "info", "warning", "error", "set_level"};
    const std::uint32_t arities[] = {2, 2, 2, 2, 1};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        FunctionInfo fn = make_native_fn(names[i], arities[i], ids[i]);
        if (std::string_view(names[i]) == "debug" || std::string_view(names[i]) == "info" ||
            std::string_view(names[i]) == "warning" || std::string_view(names[i]) == "error") {
            fn.returns_value = false;
        }
        bc.functions.push_back(std::move(fn));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> log_exports() {
    return {
        ModuleExport{"DEBUG", ExportKind::Variable, 0},
        ModuleExport{"INFO", ExportKind::Variable, 1},
        ModuleExport{"WARNING", ExportKind::Variable, 2},
        ModuleExport{"ERROR", ExportKind::Variable, 3},
        ModuleExport{"debug", ExportKind::Function, 0},
        ModuleExport{"info", ExportKind::Function, 1},
        ModuleExport{"warning", ExportKind::Function, 2},
        ModuleExport{"error", ExportKind::Function, 3},
        ModuleExport{"set_level", ExportKind::Function, 4},
    };
}

Bytecode build_crypto_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {
        NativeFunctionId::CryptoSha256,      NativeFunctionId::CryptoSha512,      NativeFunctionId::CryptoSha3_256,
        NativeFunctionId::CryptoSha3_512,    NativeFunctionId::CryptoBlake2b,   NativeFunctionId::CryptoBlake2s,
        NativeFunctionId::CryptoHmacSha256,  NativeFunctionId::CryptoHmacSha512, NativeFunctionId::CryptoRandomBytes,
        NativeFunctionId::CryptoCompare,     NativeFunctionId::CryptoHasher,    NativeFunctionId::CryptoHashFile,
        NativeFunctionId::CryptoHashIo,      NativeFunctionId::CryptoPbkdf2Sha256, NativeFunctionId::CryptoScrypt,
        NativeFunctionId::CryptoArgon2id,    NativeFunctionId::CryptoEncryptAesGcm, NativeFunctionId::CryptoDecryptAesGcm,
        NativeFunctionId::CryptoEncryptChacha20Poly1305, NativeFunctionId::CryptoDecryptChacha20Poly1305,
    };
    const char* names[] = {"sha256", "sha512", "sha3_256", "sha3_512", "blake2b", "blake2s",
                           "hmac_sha256", "hmac_sha512", "random_bytes", "compare", "hasher", "hash_file", "hash_io",
                           "pbkdf2_sha256", "scrypt", "argon2id", "encrypt_aes_gcm", "decrypt_aes_gcm",
                           "encrypt_chacha20_poly1305", "decrypt_chacha20_poly1305"};
    const std::uint32_t arities[] = {1, 1, 1, 1, 1, 1, 2, 2, 1, 2, 1, 2, 2, 4, 4, 4, 4, 5, 4, 5};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        bc.functions.push_back(make_native_fn(names[i], arities[i], ids[i]));
    }
    ClassInfo hasher = make_native_class("Hasher", NativeClassKind::CryptoHasher, {"update", "digest"});
    attach_native_class_methods(bc, hasher);
    bc.classes.push_back(std::move(hasher));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> crypto_exports() {
    return {
        ModuleExport{"sha256", ExportKind::Function, 0},
        ModuleExport{"sha512", ExportKind::Function, 1},
        ModuleExport{"sha3_256", ExportKind::Function, 2},
        ModuleExport{"sha3_512", ExportKind::Function, 3},
        ModuleExport{"blake2b", ExportKind::Function, 4},
        ModuleExport{"blake2s", ExportKind::Function, 5},
        ModuleExport{"hmac_sha256", ExportKind::Function, 6},
        ModuleExport{"hmac_sha512", ExportKind::Function, 7},
        ModuleExport{"random_bytes", ExportKind::Function, 8},
        ModuleExport{"compare", ExportKind::Function, 9},
        ModuleExport{"hasher", ExportKind::Function, 10},
        ModuleExport{"hash_file", ExportKind::Function, 11},
        ModuleExport{"hash_io", ExportKind::Function, 12},
        ModuleExport{"pbkdf2_sha256", ExportKind::Function, 13},
        ModuleExport{"scrypt", ExportKind::Function, 14},
        ModuleExport{"argon2id", ExportKind::Function, 15},
        ModuleExport{"encrypt_aes_gcm", ExportKind::Function, 16},
        ModuleExport{"decrypt_aes_gcm", ExportKind::Function, 17},
        ModuleExport{"encrypt_chacha20_poly1305", ExportKind::Function, 18},
        ModuleExport{"decrypt_chacha20_poly1305", ExportKind::Function, 19},
        ModuleExport{"Hasher", ExportKind::Class, 0},
    };
}

void attach_sql_class_methods(Bytecode& bc, ClassInfo& cls) {
    for (ClassMethodInfo& method : cls.methods) {
        FunctionInfo fn;
        fn.name = method.name;
        fn.arity = 0;
        fn.returns_value = true;
        if (method.name == "close") {
            fn.returns_value = false;
        }
        method.function_index = static_cast<std::uint32_t>(bc.functions.size());
        bc.functions.push_back(std::move(fn));
    }
}

Bytecode build_testing_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {
        NativeFunctionId::TestingEqual,     NativeFunctionId::TestingNotEqual, NativeFunctionId::TestingTrue,
        NativeFunctionId::TestingFalse,       NativeFunctionId::TestingNull,     NativeFunctionId::TestingNotNull,
        NativeFunctionId::TestingThrows,    NativeFunctionId::TestingFail,     NativeFunctionId::TestingRun,
    };
    const char* names[] = {"equal", "not_equal", "true", "false", "null", "not_null", "throws", "fail", "run"};
    const std::uint32_t arities[] = {2, 2, 1, 1, 1, 1, 1, 1, 0};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        FunctionInfo fn = make_native_fn(names[i], arities[i], ids[i]);
        if (std::string_view(names[i]) == "run") {
            fn.returns_value = false;
        }
        bc.functions.push_back(std::move(fn));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> testing_exports() {
    return {
        ModuleExport{"equal", ExportKind::Function, 0},
        ModuleExport{"not_equal", ExportKind::Function, 1},
        ModuleExport{"true", ExportKind::Function, 2},
        ModuleExport{"false", ExportKind::Function, 3},
        ModuleExport{"null", ExportKind::Function, 4},
        ModuleExport{"not_null", ExportKind::Function, 5},
        ModuleExport{"throws", ExportKind::Function, 6},
        ModuleExport{"fail", ExportKind::Function, 7},
        ModuleExport{"run", ExportKind::Function, 8},
    };
}

Bytecode build_cli_module() {
    Bytecode bc;
    const NativeFunctionId ids[] = {
        NativeFunctionId::CliArgs,    NativeFunctionId::CliArg,     NativeFunctionId::CliOption,
        NativeFunctionId::CliFlag,    NativeFunctionId::CliHas,     NativeFunctionId::CliCommand,
        NativeFunctionId::CliHelp,    NativeFunctionId::CliVersion,
    };
    const char* names[] = {"args", "arg", "option", "flag", "has", "command", "help", "version"};
    const std::uint32_t arities[] = {0, 1, 2, 1, 1, 1, 1, 1};
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        FunctionInfo fn = make_native_fn(names[i], arities[i], ids[i]);
        if (std::string_view(names[i]) == "help" || std::string_view(names[i]) == "version") {
            fn.returns_value = false;
        }
        bc.functions.push_back(std::move(fn));
    }
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> cli_exports() {
    return {
        ModuleExport{"args", ExportKind::Function, 0},
        ModuleExport{"arg", ExportKind::Function, 1},
        ModuleExport{"option", ExportKind::Function, 2},
        ModuleExport{"flag", ExportKind::Function, 3},
        ModuleExport{"has", ExportKind::Function, 4},
        ModuleExport{"command", ExportKind::Function, 5},
        ModuleExport{"help", ExportKind::Function, 6},
        ModuleExport{"version", ExportKind::Function, 7},
    };
}

Bytecode build_sql_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("open", 1, NativeFunctionId::SqlOpen));
    ClassInfo database = make_native_class(
        "Database", NativeClassKind::Database,
        {"close", "is_open", "begin", "execute", "query", "query_one", "prepare", "async_execute", "async_query",
         "async_query_one", "async_prepare"});
    attach_sql_class_methods(bc, database);
    bc.classes.push_back(std::move(database));

    ClassInfo rows = make_native_class("Rows", NativeClassKind::SqlRows, {"close", "iter"});
    attach_sql_class_methods(bc, rows);
    bc.classes.push_back(std::move(rows));

    ClassInfo statement =
        make_native_class("Statement", NativeClassKind::SqlStatement, {"close", "execute", "query", "query_one"});
    attach_sql_class_methods(bc, statement);
    bc.classes.push_back(std::move(statement));

    ClassInfo row = make_native_class("Row", NativeClassKind::None, {});
    bc.classes.push_back(std::move(row));

    ClassInfo execute_result =
        make_native_class_with_fields("ExecuteResult", NativeClassKind::None, {"affected_rows", "last_insert_id"}, {});
    bc.classes.push_back(std::move(execute_result));

    ClassInfo transaction = make_native_class(
        "Transaction", NativeClassKind::SqlTransaction,
        {"execute", "query", "query_one", "prepare", "async_execute", "async_query", "async_query_one",
         "async_prepare", "savepoint", "rollback_to", "release", "commit", "rollback"});
    attach_sql_class_methods(bc, transaction);
    bc.classes.push_back(std::move(transaction));

    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> sql_exports() {
    return {
        ModuleExport{"open", ExportKind::Function, 0},
        ModuleExport{"Database", ExportKind::Class, 0},
        ModuleExport{"Rows", ExportKind::Class, 1},
        ModuleExport{"Statement", ExportKind::Class, 2},
        ModuleExport{"Row", ExportKind::Class, 3},
        ModuleExport{"ExecuteResult", ExportKind::Class, 4},
        ModuleExport{"Transaction", ExportKind::Class, 5},
    };
}

Bytecode build_url_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("parse", 1, NativeFunctionId::UrlParse));
    bc.functions.push_back(make_native_fn("build", 1, NativeFunctionId::UrlBuild));
    bc.functions.push_back(make_native_fn("join", 2, NativeFunctionId::UrlJoin));
    bc.functions.push_back(make_native_fn("encode", 1, NativeFunctionId::UrlEncode));
    bc.functions.push_back(make_native_fn("decode", 1, NativeFunctionId::UrlDecode));
    bc.functions.push_back(make_native_fn("parse_query", 1, NativeFunctionId::UrlParseQuery));
    bc.functions.push_back(make_native_fn("build_query", 1, NativeFunctionId::UrlBuildQuery));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> url_exports() {
    return {
        ModuleExport{"parse", ExportKind::Function, 0},
        ModuleExport{"build", ExportKind::Function, 1},
        ModuleExport{"join", ExportKind::Function, 2},
        ModuleExport{"encode", ExportKind::Function, 3},
        ModuleExport{"decode", ExportKind::Function, 4},
        ModuleExport{"parse_query", ExportKind::Function, 5},
        ModuleExport{"build_query", ExportKind::Function, 6},
    };
}

Bytecode build_uuid_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("v4", 0, NativeFunctionId::UuidV4));
    bc.functions.push_back(make_native_fn("v7", 0, NativeFunctionId::UuidV7));
    bc.functions.push_back(make_native_fn("parse", 1, NativeFunctionId::UuidParse));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> uuid_exports() {
    return {
        ModuleExport{"v4", ExportKind::Function, 0},
        ModuleExport{"v7", ExportKind::Function, 1},
        ModuleExport{"parse", ExportKind::Function, 2},
    };
}

Bytecode build_csv_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("parse", 1, NativeFunctionId::CsvParse));
    bc.functions.push_back(make_native_fn("stringify", 1, NativeFunctionId::CsvStringify));
    bc.functions.push_back(make_native_fn("reader", 1, NativeFunctionId::CsvReader));
    bc.functions.push_back(make_native_fn("writer", 1, NativeFunctionId::CsvWriter));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> csv_exports() {
    return {
        ModuleExport{"parse", ExportKind::Function, 0},
        ModuleExport{"stringify", ExportKind::Function, 1},
        ModuleExport{"reader", ExportKind::Function, 2},
        ModuleExport{"writer", ExportKind::Function, 3},
    };
}

Bytecode build_xml_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("parse", 1, NativeFunctionId::XmlParse));
    bc.functions.push_back(make_native_fn("stringify", 1, NativeFunctionId::XmlStringify));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> xml_exports() {
    return {
        ModuleExport{"parse", ExportKind::Function, 0},
        ModuleExport{"stringify", ExportKind::Function, 1},
    };
}

Bytecode build_archive_module() {
    Bytecode bc;
    bc.functions.push_back(make_native_fn("open", 4, NativeFunctionId::ArchiveOpen));
    bc.functions.push_back(make_native_fn("create", 4, NativeFunctionId::ArchiveCreate));
    ClassInfo archive = make_native_class(
        "Archive", NativeClassKind::Archive,
        {"close", "is_open", "list", "entries", "read", "write", "add", "extract", "extract_all", "iter",
         "open_entry"});
    attach_sql_class_methods(bc, archive);
    bc.classes.push_back(std::move(archive));
    bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
    bc.instruction_locations.push_back(SourceLocation{1, 1});
    bc.register_count = 1;
    return bc;
}

std::vector<ModuleExport> archive_exports() {
    return {
        ModuleExport{"open", ExportKind::Function, 0},
        ModuleExport{"create", ExportKind::Function, 1},
        ModuleExport{"Archive", ExportKind::Class, 0},
    };
}

struct StdlibSpec {
    StdlibModuleId id;
    Bytecode (*build)();
    std::vector<ModuleExport> (*exports)();
};

const StdlibSpec& stdlib_spec(StdlibModuleId id) {
    static const StdlibSpec specs[] = {
        {StdlibModuleId::Math, build_math_module, math_exports},
        {StdlibModuleId::String, build_string_module, string_exports},
        {StdlibModuleId::Collections, build_collections_module, collections_exports},
        {StdlibModuleId::Path, build_path_module, path_exports},
        {StdlibModuleId::Fs, build_fs_module, fs_exports},
        {StdlibModuleId::Io, build_io_module, io_exports},
        {StdlibModuleId::Os, build_os_module, os_exports},
        {StdlibModuleId::Process, build_process_module, process_exports},
        {StdlibModuleId::Sys, build_sys_module, sys_exports},
        {StdlibModuleId::Time, build_time_module, time_exports},
        {StdlibModuleId::DateTime, build_datetime_module, datetime_exports},
        {StdlibModuleId::Random, build_random_module, random_exports},
        {StdlibModuleId::Json, build_json_module, json_exports},
        {StdlibModuleId::Re, build_re_module, re_exports},
        {StdlibModuleId::Encoding, build_encoding_module, encoding_exports},
        {StdlibModuleId::Net, build_net_module, net_exports},
        {StdlibModuleId::Http, build_http_module, http_exports},
        {StdlibModuleId::Log, build_log_module, log_exports},
        {StdlibModuleId::Crypto, build_crypto_module, crypto_exports},
        {StdlibModuleId::Sql, build_sql_module, sql_exports},
        {StdlibModuleId::Testing, build_testing_module, testing_exports},
        {StdlibModuleId::Cli, build_cli_module, cli_exports},
        {StdlibModuleId::Url, build_url_module, url_exports},
        {StdlibModuleId::Uuid, build_uuid_module, uuid_exports},
        {StdlibModuleId::Csv, build_csv_module, csv_exports},
        {StdlibModuleId::Xml, build_xml_module, xml_exports},
        {StdlibModuleId::Archive, build_archive_module, archive_exports},
    };
    return specs[static_cast<std::size_t>(id)];
}

}  // namespace

const std::vector<std::string_view>& stdlib_module_specifiers() {
    static const std::vector<std::string_view> kModules = {
        "math",     "string",      "collections", "path",    "fs",       "io",      "os",     "process",
        "sys",      "time",        "datetime",    "random",  "json",     "re",      "encoding", "net",
        "http",     "log",         "crypto",      "sql",     "testing",  "cli",
        "url",      "uuid",        "csv",         "xml",         "archive",
    };
    return kModules;
}

bool is_stdlib_module_specifier(const std::string& specifier) {
    for (const std::string_view module : stdlib_module_specifiers()) {
        if (specifier == module) {
            return true;
        }
    }
    return false;
}

std::vector<std::string_view> stdlib_module_export_names(const std::string& specifier) {
    static thread_local std::vector<std::string> owned_names;
    static thread_local std::vector<std::string_view> views;
    owned_names.clear();
    views.clear();
    if (!is_stdlib_module_specifier(specifier)) {
        return {};
    }
    const std::vector<ModuleExport> exports = stdlib_spec(stdlib_module_id(specifier)).exports();
    owned_names.reserve(exports.size());
    views.reserve(exports.size());
    for (const ModuleExport& export_info : exports) {
        owned_names.push_back(export_info.name);
        views.push_back(owned_names.back());
    }
    return views;
}

StdlibModuleId stdlib_module_id(const std::string& specifier) {
    if (specifier == "math") {
        return StdlibModuleId::Math;
    }
    if (specifier == "string") {
        return StdlibModuleId::String;
    }
    if (specifier == "collections") {
        return StdlibModuleId::Collections;
    }
    if (specifier == "path") {
        return StdlibModuleId::Path;
    }
    if (specifier == "fs") {
        return StdlibModuleId::Fs;
    }
    if (specifier == "io") {
        return StdlibModuleId::Io;
    }
    if (specifier == "os") {
        return StdlibModuleId::Os;
    }
    if (specifier == "process") {
        return StdlibModuleId::Process;
    }
    if (specifier == "sys") {
        return StdlibModuleId::Sys;
    }
    if (specifier == "time") {
        return StdlibModuleId::Time;
    }
    if (specifier == "datetime") {
        return StdlibModuleId::DateTime;
    }
    if (specifier == "random") {
        return StdlibModuleId::Random;
    }
    if (specifier == "json") {
        return StdlibModuleId::Json;
    }
    if (specifier == "re") {
        return StdlibModuleId::Re;
    }
    if (specifier == "encoding") {
        return StdlibModuleId::Encoding;
    }
    if (specifier == "net") {
        return StdlibModuleId::Net;
    }
    if (specifier == "http") {
        return StdlibModuleId::Http;
    }
    if (specifier == "log") {
        return StdlibModuleId::Log;
    }
    if (specifier == "crypto") {
        return StdlibModuleId::Crypto;
    }
    if (specifier == "sql") {
        return StdlibModuleId::Sql;
    }
    if (specifier == "testing") {
        return StdlibModuleId::Testing;
    }
    if (specifier == "cli") {
        return StdlibModuleId::Cli;
    }
    if (specifier == "url") {
        return StdlibModuleId::Url;
    }
    if (specifier == "uuid") {
        return StdlibModuleId::Uuid;
    }
    if (specifier == "csv") {
        return StdlibModuleId::Csv;
    }
    if (specifier == "xml") {
        return StdlibModuleId::Xml;
    }
    if (specifier == "archive") {
        return StdlibModuleId::Archive;
    }
    return StdlibModuleId::Math;
}

std::uint32_t register_stdlib_module(const std::string& specifier, Bytecode& root) {
    const std::string canonical = stdlib_canonical_path(specifier);

    const StdlibModuleId module_id = stdlib_module_id(specifier);
    const StdlibSpec& spec = stdlib_spec(module_id);

    ModuleDescriptor descriptor;
    descriptor.specifier = specifier;
    descriptor.canonical_path = canonical;
    descriptor.is_native = true;
    descriptor.bytecode_index = static_cast<std::uint32_t>(root.module_bytecodes.size());
    descriptor.exports = spec.exports();

    const std::uint32_t module_index = static_cast<std::uint32_t>(root.modules.size());
    root.modules.push_back(descriptor);
    root.module_bytecodes.push_back(spec.build());
    return module_index;
}

void initialize_native_module(std::uint32_t module_index, const Bytecode& root,
                              std::vector<std::optional<Value>>& globals, Runtime& runtime) {
    if (module_index >= root.modules.size() || !root.modules[module_index].is_native) {
        return;
    }
    const ModuleDescriptor& descriptor = root.modules[module_index];
    if (descriptor.specifier == "math") {
        globals.assign(root.module_bytecodes[descriptor.bytecode_index].variables.size(), std::nullopt);
        if (globals.size() >= 2) {
            globals[0] = Runtime::make_float(3.14159265358979323846);
            globals[1] = Runtime::make_float(2.71828182845904523536);
        }
        return;
    }
    if (descriptor.specifier == "sys") {
        globals.assign(root.module_bytecodes[descriptor.bytecode_index].variables.size(), std::nullopt);
        if (globals.size() >= 2) {
            std::vector<Value> args;
            args.reserve(runtime.program_args().size());
            for (const std::string& arg : runtime.program_args()) {
                args.push_back(Runtime::make_string(arg));
            }
            globals[0] = Runtime::make_list(std::move(args));
            globals[1] = Runtime::make_string(Runtime::version_string());
        }
        return;
    }
    if (descriptor.specifier == "log") {
        globals.assign(root.module_bytecodes[descriptor.bytecode_index].variables.size(), std::nullopt);
        if (globals.size() >= 4) {
            globals[0] = Runtime::make_string("DEBUG");
            globals[1] = Runtime::make_string("INFO");
            globals[2] = Runtime::make_string("WARNING");
            globals[3] = Runtime::make_string("ERROR");
        }
    }
}

void append_root_async_classes(Bytecode& bc) {
    ClassInfo task_group = make_native_class("TaskGroup", NativeClassKind::TaskGroup, {"spawn", "wait", "enter", "exit"});
    for (ClassMethodInfo& method : task_group.methods) {
        FunctionInfo fn;
        fn.name = method.name;
        fn.returns_value = true;
        fn.is_native = true;
        if (method.name == "spawn") {
            fn.arity = 1;
        } else if (method.name == "exit") {
            fn.arity = 1;
        } else {
            fn.arity = 0;
        }
        method.function_index = static_cast<std::uint32_t>(bc.functions.size());
        bc.functions.push_back(std::move(fn));
    }
    bc.classes.push_back(std::move(task_group));

    ClassInfo stream = make_native_class("AsyncStream", NativeClassKind::AsyncStream,
                                         {"iter", "anext", "async_read", "async_write", "close"});
    for (ClassMethodInfo& method : stream.methods) {
        FunctionInfo fn;
        fn.name = method.name;
        fn.returns_value = true;
        fn.is_native = true;
        if (method.name == "async_write") {
            fn.arity = 1;
        } else {
            fn.arity = 0;
        }
        method.function_index = static_cast<std::uint32_t>(bc.functions.size());
        bc.functions.push_back(std::move(fn));
    }
    bc.classes.push_back(std::move(stream));
}

}  // namespace pscript
