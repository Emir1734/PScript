#pragma once

#include "jit/jit_config.h"
#include "jit/jit_deopt.h"
#include "jit/jit_ic.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pscript::jit {

struct JitExecState;

using JitNativeEntry = void (*)(JitExecState*);

struct JitCompiledCode {
    JitTier tier = JitTier::BaselineJit;
    std::uint32_t function_index = 0;
    std::string bytecode_fingerprint;
    std::size_t code_size = 0;
    void* executable = nullptr;
    JitNativeEntry entry = nullptr;
    bool valid = false;
    std::shared_ptr<JitCompiledMetadata> metadata;
    std::vector<JitCallIcSlot> call_ic_slots;
    std::uint32_t ir_instruction_count = 0;
    std::uint32_t register_count = 0;
    bool buffered_integer_locals = false;
    std::vector<std::int32_t> integer_local_shadow_reg;
    // True when emit_dispatch_function (IR dispatch loop + jump table) was used.
    bool uses_dispatch_loop = false;
    bool cold_calls_outside_loop = false;

    JitCompiledCode() = default;
    JitCompiledCode(JitCompiledCode&& other) noexcept;
    JitCompiledCode& operator=(JitCompiledCode&& other) noexcept;
    ~JitCompiledCode();
    JitCompiledCode(const JitCompiledCode&) = delete;
    JitCompiledCode& operator=(const JitCompiledCode&) = delete;
};

class JitCodeBuffer {
public:
    JitCodeBuffer() = default;
    JitCodeBuffer(const JitCodeBuffer&) = delete;
    JitCodeBuffer& operator=(const JitCodeBuffer&) = delete;
    ~JitCodeBuffer();

    void write(const void* data, std::size_t size);
    using PreProtectPatch = void (*)(void* base, std::size_t size, void* context);
    bool finalize_executable(JitCompiledCode& out, std::string& error, PreProtectPatch patch = nullptr,
                             void* patch_context = nullptr);

private:
    std::vector<std::uint8_t> bytes_;
};

}  // namespace pscript::jit
