#include "jit/jit_code.h"

#include <cstring>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace pscript::jit {
namespace {

#if defined(__linux__)
void release_executable_memory(void* memory, std::size_t size) {
    if (memory != nullptr && size > 0) {
        munmap(memory, size);
    }
}

std::size_t page_aligned_size(std::size_t size) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return size;
    }
    const std::size_t ps = static_cast<std::size_t>(page_size);
    return (size + ps - 1) & ~(ps - 1);
}

void flush_instruction_cache(void* memory, std::size_t size) {
    __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + size);
}
#endif

}  // namespace

JitCompiledCode::JitCompiledCode(JitCompiledCode&& other) noexcept {
    *this = std::move(other);
}

JitCompiledCode& JitCompiledCode::operator=(JitCompiledCode&& other) noexcept {
    if (this != &other) {
        if (executable) {
#if defined(_WIN32)
            VirtualFree(executable, 0, MEM_RELEASE);
#elif defined(__linux__)
            release_executable_memory(executable, code_size);
#endif
        }
        tier = other.tier;
        function_index = other.function_index;
        bytecode_fingerprint = std::move(other.bytecode_fingerprint);
        code_size = other.code_size;
        executable = other.executable;
        entry = other.entry;
        valid = other.valid;
        metadata = std::move(other.metadata);
        call_ic_slots = std::move(other.call_ic_slots);
        ir_instruction_count = other.ir_instruction_count;
        register_count = other.register_count;
        buffered_integer_locals = other.buffered_integer_locals;
        uses_dispatch_loop = other.uses_dispatch_loop;
        cold_calls_outside_loop = other.cold_calls_outside_loop;
        integer_local_shadow_reg = std::move(other.integer_local_shadow_reg);
        other.executable = nullptr;
        other.entry = nullptr;
        other.valid = false;
        other.code_size = 0;
        other.metadata.reset();
        other.call_ic_slots.clear();
        other.ir_instruction_count = 0;
        other.register_count = 0;
        other.buffered_integer_locals = false;
        other.integer_local_shadow_reg.clear();
    }
    return *this;
}

JitCompiledCode::~JitCompiledCode() {
    if (executable) {
#if defined(_WIN32)
        VirtualFree(executable, 0, MEM_RELEASE);
#elif defined(__linux__)
        release_executable_memory(executable, code_size);
#endif
        executable = nullptr;
    }
}

JitCodeBuffer::~JitCodeBuffer() = default;

void JitCodeBuffer::write(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    bytes_.insert(bytes_.end(), bytes, bytes + size);
}

bool JitCodeBuffer::finalize_executable(JitCompiledCode& out, std::string& error, PreProtectPatch patch,
                                        void* patch_context) {
    if (bytes_.empty()) {
        error = "empty JIT code buffer";
        return false;
    }
#if defined(_WIN32)
    void* memory =
        VirtualAlloc(nullptr, bytes_.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) {
        error = "VirtualAlloc failed";
        return false;
    }
    std::memcpy(memory, bytes_.data(), bytes_.size());
    if (patch) {
        patch(memory, bytes_.size(), patch_context);
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(memory, bytes_.size(), PAGE_EXECUTE_READ, &old_protect)) {
        VirtualFree(memory, 0, MEM_RELEASE);
        error = "VirtualProtect failed";
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), memory, bytes_.size());
    out.executable = memory;
    out.code_size = bytes_.size();
    out.entry = reinterpret_cast<JitNativeEntry>(memory);
    out.valid = true;
    return true;
#elif defined(__linux__)
    const std::size_t mapped_size = page_aligned_size(bytes_.size());
    void* memory = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        error = "mmap failed";
        return false;
    }
    std::memcpy(memory, bytes_.data(), bytes_.size());
    if (patch) {
        patch(memory, bytes_.size(), patch_context);
    }
    if (mprotect(memory, mapped_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(memory, mapped_size);
        error = "mprotect failed";
        return false;
    }
    flush_instruction_cache(memory, bytes_.size());
    out.executable = memory;
    out.code_size = mapped_size;
    out.entry = reinterpret_cast<JitNativeEntry>(memory);
    out.valid = true;
    return true;
#else
    error = "JIT executable memory is not implemented on this platform";
    return false;
#endif
}

}  // namespace pscript::jit
