#pragma once

#include <cstddef>
#include <cstdint>

namespace pscript::limits {

/** Maximum VM call frames (recursion depth). */
inline constexpr std::size_t kMaxCallStackDepth = 10'000;

/** Guard against runaway loops / infinite bytecode execution. */
inline constexpr std::uint64_t kMaxInstructionExecutions = 100'000'000;

/** Upper bound on concurrently tracked async tasks (explosion guard). */
inline constexpr std::size_t kMaxTrackedAsyncTasks = 1'000'000;

/** Reject oversized compiled programs before execution. */
inline constexpr std::size_t kMaxBytecodeInstructions = 10'000'000;
inline constexpr std::size_t kMaxBytecodeConstants = 1'000'000;
inline constexpr std::size_t kMaxBytecodeFunctions = 100'000;
inline constexpr std::size_t kMaxBytecodeRegisters = 1'000'000;

}  // namespace pscript::limits
