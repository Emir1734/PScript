#pragma once

#if defined(_WIN64) && defined(_M_X64)
#define PSCRIPT_JIT_HOST_X64 1
#define PSCRIPT_JIT_ABI_WIN64 1
#elif defined(__linux__) && defined(__x86_64__)
#define PSCRIPT_JIT_HOST_X64 1
#define PSCRIPT_JIT_ABI_SYSV 1
#endif
