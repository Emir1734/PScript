#pragma once

namespace pscript {

// Developer CLI exit codes (distinct from process.exit() codes returned by the VM).
inline constexpr int kExitSuccess = 0;
inline constexpr int kExitCompile = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitRuntime = 3;
inline constexpr int kExitToolPending = 4;

}  // namespace pscript
