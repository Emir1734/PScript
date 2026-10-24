#pragma once

#include "compiler/bytecode.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pscript::debug {

enum class DebugState : std::uint8_t {
    Idle,
    Running,
    Paused,
    Stepping,
    Stopped,
    Exited,
    Exception,
};

enum class BreakpointState : std::uint8_t {
    Requested,
    Resolved,
    Enabled,
    Disabled,
};

enum class StepKind : std::uint8_t {
    StepNone,
    StepInto,
    StepOver,
    StepLeave,
};

enum class DebugEventKind : std::uint8_t {
    Started,
    Continued,
    Stopped,
    BreakpointHit,
    StepComplete,
    Exception,
    Exited,
    Terminated,
};

struct DebugEvent {
    DebugEventKind kind = DebugEventKind::Stopped;
    std::string message;
    SourceLocation location;
    std::string source_file;
    std::uint32_t function_index = 0xFFFFFFFFu;
};

enum class ExceptionBreakpointMode : std::uint8_t {
    None,
    Uncaught,
    All,
};

struct SourceBreakpoint {
    std::string source_file;
    std::uint32_t line = 0;
    BreakpointState state = BreakpointState::Requested;
    std::string condition;
    std::vector<std::size_t> resolved_ips;
    std::uint32_t hit_count = 0;
    std::optional<std::uint32_t> hit_count_target;
    bool one_shot = false;
};

struct FunctionBreakpoint {
    std::string function_name;
    BreakpointState state = BreakpointState::Enabled;
};

}  // namespace pscript::debug
