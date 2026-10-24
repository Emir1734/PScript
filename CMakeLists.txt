cmake_minimum_required(VERSION 3.20)

project(PScript VERSION 1.0.0 LANGUAGES C CXX)

# SemVer pre-release tag (e.g. rc.1). Empty for stable 1.0.0 final releases.
set(PSCRIPT_VERSION_PRERELEASE "" CACHE STRING "SemVer pre-release suffix")
if(PSCRIPT_VERSION_PRERELEASE STREQUAL "")
    set(PSCRIPT_VERSION_STRING "${PROJECT_VERSION}")
else()
    set(PSCRIPT_VERSION_STRING "${PROJECT_VERSION}-${PSCRIPT_VERSION_PRERELEASE}")
endif()

configure_file(${CMAKE_SOURCE_DIR}/src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Test/tool executables under bin/<Config>/; CLI `pscript` stays in the build root (see pscript target).
set(PSCRIPT_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PSCRIPT_BIN_DIR}")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${PSCRIPT_BIN_DIR}/${_cfg}")
endforeach()

if (MSVC)
    add_compile_options(/utf-8)
    # Reduces linker holding the output .exe open (common cause of LNK1104 during parallel rebuild).
    add_link_options(/INCREMENTAL:NO)
endif()

add_library(pscript_lexer
    src/lexer/lexer.cpp
    src/lexer/keywords.cpp
    src/lexer/duration_util.cpp
)
target_include_directories(pscript_lexer PUBLIC src)

add_library(pscript_parser
    src/parser/parser.cpp
)
target_include_directories(pscript_parser PUBLIC src)
target_link_libraries(pscript_parser PUBLIC pscript_lexer)

add_library(pscript_compiler
    src/compiler/bytecode.cpp
    src/compiler/compiler.cpp
    src/compiler/compiler_interfaces.cpp
    src/compiler/compiler_error.cpp
    src/compiler/match_property_codegen.cpp
    src/compiler/module_loader.cpp
    src/compiler/type_compat.cpp
    src/compiler/generic.cpp
    src/compiler/pgo.cpp
    src/compiler/embed_codegen.cpp
)
target_include_directories(pscript_compiler PUBLIC src)
target_link_libraries(pscript_compiler PUBLIC pscript_parser pscript_runtime)

add_library(pscript_miniz STATIC
    deps/miniz/miniz.c
    deps/miniz/miniz_tinfl.c
    deps/miniz/miniz_tdef.c
    deps/miniz/miniz_zip.c
)
target_include_directories(pscript_miniz PUBLIC "${CMAKE_SOURCE_DIR}/deps/miniz")
if (MSVC)
    target_compile_options(pscript_miniz PRIVATE /wd4244 /wd4267 /wd4334)
endif()

add_library(pscript_stdlib
    src/stdlib/stdlib.cpp
    src/stdlib/native_functions.cpp
    src/stdlib/native_common.cpp
    src/stdlib/collections_native.cpp
    src/stdlib/native_class.cpp
    src/stdlib/native_path.cpp
    src/stdlib/native_fs.cpp
    src/stdlib/native_io.cpp
    src/stdlib/native_os.cpp
    src/stdlib/native_process.cpp
    src/stdlib/native_datetime.cpp
    src/stdlib/native_random.cpp
    src/stdlib/native_json.cpp
    src/stdlib/native_re.cpp
    src/stdlib/native_encoding.cpp
    src/stdlib/native_net.cpp
    src/stdlib/native_http.cpp
    src/stdlib/native_log.cpp
    src/stdlib/native_crypto.cpp
    src/stdlib/native_sql.cpp
    src/stdlib/native_testing.cpp
    src/stdlib/native_cli.cpp
    src/stdlib/native_task_group.cpp
    src/stdlib/native_async_stream.cpp
    src/stdlib/native_url.cpp
    src/stdlib/native_uuid.cpp
    src/stdlib/native_csv.cpp
    src/stdlib/native_xml.cpp
    src/stdlib/native_archive.cpp
    src/stdlib/archive/archive_open.cpp
    src/stdlib/archive/archive_zip.cpp
)
target_include_directories(pscript_stdlib PUBLIC src)
add_library(pscript_archive
    src/stdlib/archive/archive_security.cpp
    src/stdlib/archive/archive_gzip.cpp
    src/stdlib/archive/archive_tar.cpp
)
target_include_directories(pscript_archive PUBLIC src)
target_link_libraries(pscript_archive PUBLIC pscript_miniz)

target_link_libraries(pscript_stdlib PUBLIC pscript_runtime pscript_miniz pscript_archive)
target_link_libraries(pscript_compiler PUBLIC pscript_stdlib)

add_library(pscript_runtime
    src/runtime/runtime.cpp
    src/runtime/async_io.cpp
    src/runtime/task_group.cpp
    src/runtime/iterator.cpp
    src/runtime/net_ops.cpp
    src/runtime/http_ops.cpp
    src/runtime/tls_ops.cpp
    src/runtime/crypto_ops.cpp
    src/runtime/crypto_kdf_cipher.cpp
    src/runtime/sql_connection.cpp
    src/runtime/sql_ops.cpp
    src/runtime/sql_database_resources.cpp
    src/runtime/sql_session.cpp
    src/runtime/sql_transaction.cpp
    src/runtime/sql_async.cpp
    src/runtime/cli_state.cpp
    src/runtime/alloc_guard.cpp
    src/runtime/profiler.cpp
    src/runtime/opcode_name.cpp
    src/runtime/native_lifecycle.cpp
    src/runtime/builtin_dispatch.cpp
    src/runtime/collection_ops.cpp
)
target_include_directories(pscript_runtime PUBLIC src ${CMAKE_BINARY_DIR}/generated)
find_package(SQLite3 QUIET)
if (SQLite3_FOUND)
    target_link_libraries(pscript_runtime PUBLIC SQLite::SQLite3)
else()
    if (EXISTS "${CMAKE_SOURCE_DIR}/deps/sqlite3/sqlite3.c")
        add_library(pscript_sqlite3 STATIC "${CMAKE_SOURCE_DIR}/deps/sqlite3/sqlite3.c")
        target_include_directories(pscript_sqlite3 PUBLIC "${CMAKE_SOURCE_DIR}/deps/sqlite3")
        if (MSVC)
            target_compile_options(pscript_sqlite3 PRIVATE /wd4018 /wd4244 /wd4267 /wd4996)
        endif()
        target_link_libraries(pscript_runtime PUBLIC pscript_sqlite3)
    else()
        message(FATAL_ERROR "SQLite3 not found and bundled amalgamation missing at deps/sqlite3/sqlite3.c")
    endif()
endif()
target_compile_definitions(pscript_runtime PUBLIC PSCRIPT_HAS_SQLITE=1)
set(PSCRIPT_ARGON2_SRC "${CMAKE_SOURCE_DIR}/deps/argon2_upstream/src")
if (EXISTS "${PSCRIPT_ARGON2_SRC}/argon2.c")
    add_library(pscript_argon2 STATIC
        "${PSCRIPT_ARGON2_SRC}/argon2.c"
        "${PSCRIPT_ARGON2_SRC}/core.c"
        "${PSCRIPT_ARGON2_SRC}/encoding.c"
        "${PSCRIPT_ARGON2_SRC}/thread.c"
        "${PSCRIPT_ARGON2_SRC}/ref.c"
        "${PSCRIPT_ARGON2_SRC}/blake2/blake2b.c"
    )
    target_include_directories(pscript_argon2 PUBLIC "${CMAKE_SOURCE_DIR}/deps/argon2_upstream/include")
    target_include_directories(pscript_argon2 PRIVATE "${PSCRIPT_ARGON2_SRC}")
    if (UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(pscript_argon2 PUBLIC Threads::Threads)
    endif()
    if (MSVC)
        target_compile_options(pscript_argon2 PRIVATE /wd4244 /wd4267 /wd4996)
        target_compile_definitions(pscript_argon2 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(pscript_runtime PUBLIC pscript_argon2)
    target_compile_definitions(pscript_runtime PUBLIC PSCRIPT_HAS_LIBARGON2=1)
endif()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)
else()
    find_package(OpenSSL QUIET COMPONENTS Crypto SSL)
endif()
if (OpenSSL_FOUND)
    target_link_libraries(pscript_runtime PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(pscript_runtime PUBLIC PSCRIPT_HAS_OPENSSL=1)
endif()
if (WIN32)
    target_link_libraries(pscript_runtime PUBLIC ws2_32 secur32 crypt32 bcrypt)
endif()
if (UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(pscript_runtime PUBLIC Threads::Threads)
endif()

add_library(pscript_package
    src/package/atomic_file.cpp
    src/package/checksum.cpp
    src/package/git_clone.cpp
    src/package/json_util.cpp
    src/package/install.cpp
    src/package/lockfile.cpp
    src/package/manifest.cpp
    src/package/package_archive.cpp
    src/package/packaging.cpp
    src/package/package_identity.cpp
    src/package/project.cpp
    src/package/registry.cpp
    src/package/registry_client.cpp
    src/package/registry_credentials.cpp
    src/package/semver.cpp
    src/package/toml.cpp
)
target_include_directories(pscript_package PUBLIC src)
target_link_libraries(pscript_package PUBLIC pscript_runtime pscript_archive)

add_library(pscript_registry
    src/registry/registry_config.cpp
    src/registry/registry_json.cpp
    src/registry/auth/password_hash.cpp
    src/registry/auth/rate_limiter.cpp
    src/registry/auth/auth_service.cpp
    src/registry/auth/authz.cpp
    src/registry/db/migrations.cpp
    src/registry/db/registry_db.cpp
    src/registry/db/sqlite_registry_db.cpp
    src/registry/storage/blob_store.cpp
    src/registry/service/registry_service.cpp
    src/registry/http/json_body.cpp
    src/registry/http/registry_router.cpp
    src/registry/http/registry_server.cpp
)
target_include_directories(pscript_registry PUBLIC src)
target_link_libraries(pscript_registry PUBLIC pscript_package pscript_runtime)

if(NOT PostgreSQL_FOUND AND EXISTS "C:/Program Files/PostgreSQL/18")
    set(PostgreSQL_ROOT "C:/Program Files/PostgreSQL/18")
endif()
find_package(PostgreSQL)
if(PostgreSQL_FOUND)
    target_sources(pscript_registry PRIVATE
        src/registry/db/postgres_registry_db.cpp
        src/registry/db/postgres_migrations.cpp)
    target_include_directories(pscript_registry PRIVATE ${PostgreSQL_INCLUDE_DIRS})
    target_link_libraries(pscript_registry PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(pscript_registry PUBLIC PSCRIPT_HAS_LIBPQ=1)
    message(STATUS "Registry PostgreSQL backend: enabled (libpq)")
else()
    message(STATUS "Registry PostgreSQL backend: disabled (libpq not found)")
endif()

add_executable(pscript_registry_server src/registry/main.cpp)
target_link_libraries(pscript_registry_server PRIVATE pscript_registry)

target_link_libraries(pscript_compiler PUBLIC pscript_package)

add_library(pscript_jit
    src/jit/jit_ir.cpp
    src/jit/jit_ic.cpp
    src/jit/jit_helpers.cpp
    src/jit/jit_code.cpp
    src/jit/jit_backend_x64.cpp
    src/jit/jit_compiler.cpp
    src/jit/jit_runtime.cpp
    src/jit/jit_deopt.cpp
    src/jit/jit_opt_profile.cpp
    src/jit/jit_opt_passes.cpp
    src/jit/jit_escape.cpp
    src/jit/jit_call_eligibility.cpp
)
target_include_directories(pscript_jit PUBLIC src)
target_link_libraries(pscript_jit PUBLIC pscript_runtime pscript_compiler)

add_library(pscript_vm
    src/vm/exception_support.cpp
    src/vm/vm.cpp
    src/vm/vm_debug.cpp
    src/vm/jit_bridge.cpp
)
target_include_directories(pscript_vm PUBLIC src)
target_link_libraries(pscript_vm PUBLIC pscript_compiler pscript_runtime pscript_jit)

add_library(pscript_debug
    src/debug/debug_mapping.cpp
    src/debug/debug_value.cpp
    src/debug/debug_session.cpp
    src/debug/debug_eval.cpp
    src/debug/debug_vm_inspector.cpp
)
target_include_directories(pscript_debug PUBLIC src)
target_link_libraries(pscript_debug PUBLIC pscript_vm pscript_compiler pscript_parser)

add_library(pscript_analysis
    src/analysis/diagnostics.cpp
    src/analysis/symbol_index.cpp
    src/analysis/linter.cpp
    src/analysis/format.cpp
    src/analysis/lsp_support.cpp
    src/analysis/analysis.cpp
)
target_include_directories(pscript_analysis PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(pscript_analysis PUBLIC pscript_compiler)

add_library(pscript_cli
    src/cli/cli.cpp
    src/cli/pipeline.cpp
    src/cli/dev_cli.cpp
    src/cli/package_commands.cpp
    src/cli/profile_commands.cpp
    src/cli/workspace_commands.cpp
    src/cli/lib_commands.cpp
    src/cli/registry_auth_commands.cpp
)
target_include_directories(pscript_cli PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(pscript_cli PUBLIC pscript_vm pscript_analysis pscript_package)

add_executable(pscript
    src/main.cpp
)
target_link_libraries(pscript PRIVATE pscript_cli)
set_target_properties(pscript PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)

add_executable(pscript_lexer_tests
    tests/lexer_test.cpp
)
target_link_libraries(pscript_lexer_tests PRIVATE pscript_lexer)

add_executable(pscript_parser_tests
    tests/parser_test.cpp
)
target_link_libraries(pscript_parser_tests PRIVATE pscript_parser)

add_executable(pscript_compiler_tests
    tests/compiler_test.cpp
)
target_link_libraries(pscript_compiler_tests PRIVATE pscript_compiler)
if (MSVC)
    target_link_options(pscript_compiler_tests PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_vm_tests
    tests/vm_test.cpp
)
target_link_libraries(pscript_vm_tests PRIVATE pscript_vm)
if (MSVC)
    target_link_options(pscript_vm_tests PRIVATE "/STACK:33554432")
endif()
if (OpenSSL_FOUND)
    target_compile_definitions(pscript_vm_tests PRIVATE PSCRIPT_HAS_OPENSSL=1)
endif()

add_executable(pscript_additional_tests
    tests/additional_improvements_test.cpp
)
target_link_libraries(pscript_additional_tests PRIVATE pscript_vm pscript_compiler pscript_parser)
if (MSVC)
    target_link_options(pscript_additional_tests PRIVATE "/STACK:33554432")
endif()
if (OpenSSL_FOUND)
    target_compile_definitions(pscript_additional_tests PRIVATE PSCRIPT_HAS_OPENSSL=1)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/build/race_only_test.cpp")
    add_executable(race_only_test build/race_only_test.cpp)
    target_link_libraries(race_only_test PRIVATE pscript_vm)
endif()

add_executable(pscript_runtime_tests
    tests/runtime_test.cpp
)
target_link_libraries(pscript_runtime_tests PRIVATE pscript_runtime)

add_executable(pscript_runtime_stress_tests
    tests/runtime_stress_test.cpp
)
target_link_libraries(pscript_runtime_stress_tests PRIVATE pscript_vm pscript_compiler pscript_parser)

add_executable(pscript_profiling_tests
    tests/profiling_test.cpp
)
target_link_libraries(pscript_profiling_tests PRIVATE pscript_vm pscript_compiler pscript_parser)
if (MSVC)
    target_link_options(pscript_profiling_tests PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_pgo_tests
    tests/pgo_test.cpp
)
target_link_libraries(pscript_pgo_tests PRIVATE pscript_vm pscript_compiler pscript_parser)

add_executable(pscript_jit_tests
    tests/jit_test.cpp
)
target_include_directories(pscript_jit_tests PRIVATE tests)
target_link_libraries(pscript_jit_tests PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_jit_tests PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_jit_large_heavy_repro
    tests/jit_large_heavy_repro.cpp
)
target_include_directories(pscript_jit_large_heavy_repro PRIVATE tests)
target_link_libraries(pscript_jit_large_heavy_repro PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_jit_large_heavy_repro PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_phase6_triple_capture_repro
    tests/phase6_triple_capture_repro.cpp
)
target_link_libraries(pscript_phase6_triple_capture_repro PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_phase6_triple_capture_repro PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_jit_ic_repro
    tests/jit_ic_av_repro.cpp
)
target_link_libraries(pscript_jit_ic_repro PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_jit_ic_repro PRIVATE "/STACK:33554432")
endif()

add_executable(jit_capture_matrix_only
    tests/jit_capture_matrix_only.cpp
)
target_link_libraries(jit_capture_matrix_only PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if (MSVC)
    target_link_options(pscript_pgo_tests PRIVATE "/STACK:33554432")
endif()
if (MSVC)
    target_link_options(pscript_runtime_stress_tests PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_analysis_tests
    tests/analysis_test.cpp
)
target_link_libraries(pscript_analysis_tests PRIVATE pscript_analysis)

add_executable(pscript_lsp
    src/lsp/main.cpp
    src/lsp/server.cpp
    src/lsp/json_util.cpp
    src/lsp/text_sync.cpp
    src/lsp/uri_util.cpp
)
target_include_directories(pscript_lsp PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(pscript_lsp PRIVATE pscript_analysis)
# Link output is pscript_lsp_compile.exe so MSVC can replace the binary while the IDE runs
# a copied runtime from extension globalStorage (see editors/vscode).
set_target_properties(pscript_lsp PROPERTIES OUTPUT_NAME "pscript_lsp_compile")
if (WIN32)
    set(PSCRIPT_LSP_CANONICAL_NAME "pscript_lsp.exe")
else()
    set(PSCRIPT_LSP_CANONICAL_NAME "pscript_lsp")
endif()
add_custom_command(
    TARGET pscript_lsp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:pscript_lsp>"
            -DDST="$<TARGET_FILE_DIR:pscript_lsp>/${PSCRIPT_LSP_CANONICAL_NAME}"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_lsp_canonical.cmake"
    COMMENT "Sync canonical LSP binary from compile-stage output (non-fatal if locked)"
)

add_executable(pscript_lsp_tests
    tests/lsp_test.cpp
    src/lsp/json_util.cpp
    src/lsp/text_sync.cpp
    src/lsp/uri_util.cpp
)
target_link_libraries(pscript_lsp_tests PRIVATE pscript_analysis)
target_include_directories(pscript_lsp_tests PRIVATE src ${CMAKE_BINARY_DIR}/generated)

add_executable(pscript_lsp_server_stdio_tests
    tests/lsp_server_process_test.cpp
    src/lsp/json_util.cpp
)
target_link_libraries(pscript_lsp_server_stdio_tests PRIVATE pscript_analysis)
target_include_directories(pscript_lsp_server_stdio_tests PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(pscript_lsp_server_stdio_tests PRIVATE
    PSCRIPT_LSP_EXE="$<TARGET_FILE_DIR:pscript_lsp>/${PSCRIPT_LSP_CANONICAL_NAME}")
add_dependencies(pscript_lsp_server_stdio_tests pscript_lsp)

add_executable(pscript_dap
    src/dap/main.cpp
    src/dap/dap_adapter.cpp
    src/dap/dap_framing.cpp
    src/lsp/json_util.cpp
)
target_include_directories(pscript_dap PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(pscript_dap PRIVATE pscript_debug pscript_compiler pscript_parser pscript_vm)
set_target_properties(pscript_dap PROPERTIES OUTPUT_NAME "pscript_dap_compile")
if (WIN32)
    set(PSCRIPT_DAP_CANONICAL_NAME "pscript_dap.exe")
else()
    set(PSCRIPT_DAP_CANONICAL_NAME "pscript_dap")
endif()
add_custom_command(
    TARGET pscript_dap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC="$<TARGET_FILE:pscript_dap>"
            -DDST="$<TARGET_FILE_DIR:pscript_dap>/${PSCRIPT_DAP_CANONICAL_NAME}"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_lsp_canonical.cmake"
    COMMENT "Sync canonical DAP binary from compile-stage output (non-fatal if locked)"
)

add_executable(pscript_dap_stdio_tests
    tests/dap_stdio_test.cpp
    src/lsp/json_util.cpp
)
target_link_libraries(pscript_dap_stdio_tests PRIVATE pscript_debug)
target_include_directories(pscript_dap_stdio_tests PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(pscript_dap_stdio_tests PRIVATE
    PSCRIPT_DAP_EXE="$<TARGET_FILE_DIR:pscript_dap>/${PSCRIPT_DAP_CANONICAL_NAME}")
add_dependencies(pscript_dap_stdio_tests pscript_dap)

add_executable(pscript_cli_tests
    tests/cli_test.cpp
)
target_link_libraries(pscript_cli_tests PRIVATE pscript_cli)
target_compile_definitions(pscript_cli_tests PRIVATE PSCRIPT_EXECUTABLE="$<TARGET_FILE:pscript>")
add_dependencies(pscript_cli_tests pscript)

add_executable(pscript_package_tests
    tests/package_test.cpp
    tests/registry_credentials_test.cpp
)
target_link_libraries(pscript_package_tests PRIVATE pscript_package)
target_compile_definitions(pscript_package_tests PRIVATE PSCRIPT_TEST_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")

add_executable(pscript_registry_tests
    tests/registry_server_test.cpp
    tests/registry_integration_test.cpp
)
target_link_libraries(pscript_registry_tests PRIVATE pscript_registry)

if(PostgreSQL_FOUND)
    add_executable(pscript_registry_postgres_tests tests/registry_postgres_test.cpp)
    target_link_libraries(pscript_registry_postgres_tests PRIVATE pscript_registry)
    target_compile_definitions(pscript_registry_postgres_tests PRIVATE PSCRIPT_HAS_LIBPQ=1)
endif()

enable_testing()
add_test(NAME lexer COMMAND pscript_lexer_tests)
add_test(NAME parser COMMAND pscript_parser_tests)
add_test(NAME compiler COMMAND pscript_compiler_tests)
add_test(NAME vm COMMAND pscript_vm_tests)
add_executable(pscript_archive_tests
    tests/archive_test.cpp
    tests/archive_hard_tests.cpp
)
target_link_libraries(pscript_archive_tests PRIVATE pscript_vm)
if(MSVC)
    target_link_options(pscript_archive_tests PRIVATE "/STACK:33554432")
endif()
add_test(NAME archive COMMAND pscript_archive_tests)

# Zip corruption/zip64 must not run in the VM-linked test binary (MSVC CRT/miniz interaction).
add_executable(pscript_archive_zip_hard_tests
    tests/archive_zip_hard_main.cpp
    tests/archive_zip_corruption_tests.cpp
)
target_link_libraries(pscript_archive_zip_hard_tests PRIVATE pscript_stdlib)
if(MSVC)
    target_link_options(pscript_archive_zip_hard_tests PRIVATE "/STACK:33554432")
endif()
add_test(NAME archive_zip_hard COMMAND pscript_archive_zip_hard_tests)
set_tests_properties(archive_zip_hard PROPERTIES TIMEOUT 1800)
add_test(NAME additional_improvements COMMAND pscript_additional_tests)
add_test(NAME runtime COMMAND pscript_runtime_tests)
add_test(NAME runtime_stress COMMAND pscript_runtime_stress_tests)
add_test(NAME profiling COMMAND pscript_profiling_tests)
add_test(NAME pgo COMMAND pscript_pgo_tests)
add_test(NAME jit COMMAND "$<TARGET_FILE:pscript_jit_tests>")

add_executable(pscript_jit_opt_tests
    tests/jit_opt_test.cpp
)
target_include_directories(pscript_jit_opt_tests PRIVATE tests)
target_link_libraries(pscript_jit_opt_tests PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_jit_opt_tests PRIVATE "/STACK:33554432")
endif()
add_test(NAME jit_opt COMMAND "$<TARGET_FILE:pscript_jit_opt_tests>")

# Set Z: alternate output name so link succeeds when pscript_jit_opt_tests.exe is locked.
add_executable(pscript_jit_opt_tests_z tests/jit_opt_test.cpp)
target_include_directories(pscript_jit_opt_tests_z PRIVATE tests)
target_link_libraries(pscript_jit_opt_tests_z PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_jit_opt_tests_z PRIVATE "/STACK:33554432")
endif()

add_executable(closure_nested_repro
    tests/closure_nested_repro.cpp
)
target_link_libraries(closure_nested_repro PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
target_include_directories(closure_nested_repro PRIVATE tests ${CMAKE_BINARY_DIR}/generated src)
if(MSVC)
    target_link_options(closure_nested_repro PRIVATE "/STACK:33554432")
endif()

add_executable(parallel_map_test
    tests/parallel_map_test.cpp
)
target_link_libraries(parallel_map_test PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
target_include_directories(parallel_map_test PRIVATE tests ${CMAKE_BINARY_DIR}/generated src)
if(MSVC)
    target_link_options(parallel_map_test PRIVATE "/STACK:33554432")
endif()
add_test(NAME parallel_map COMMAND parallel_map_test)

add_executable(jit_poly_baseline_repro
    tests/jit_poly_baseline_repro.cpp
)
target_link_libraries(jit_poly_baseline_repro PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
target_include_directories(jit_poly_baseline_repro PRIVATE tests ${CMAKE_BINARY_DIR}/generated src)
if(MSVC)
    target_link_options(jit_poly_baseline_repro PRIVATE "/STACK:33554432")
endif()

add_executable(crash_nested_repro
    tests/crash_nested_repro.cpp
)
target_link_libraries(crash_nested_repro PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
target_include_directories(crash_nested_repro PRIVATE tests ${CMAKE_BINARY_DIR}/generated src)
if(MSVC)
    target_link_options(crash_nested_repro PRIVATE "/STACK:33554432")
endif()

add_executable(pscript_jit_deopt_tests
    tests/jit_deopt_test.cpp
)
target_link_libraries(pscript_jit_deopt_tests PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_jit_deopt_tests PRIVATE "/STACK:33554432")
endif()
add_test(NAME jit_deopt COMMAND "$<TARGET_FILE:pscript_jit_deopt_tests>")

add_executable(pscript_stabilization_tests
    tests/stabilization_test.cpp
)
target_link_libraries(pscript_stabilization_tests PRIVATE pscript_vm pscript_compiler pscript_parser pscript_jit)
if(MSVC)
    target_link_options(pscript_stabilization_tests PRIVATE "/STACK:33554432")
endif()
add_test(NAME stabilization COMMAND pscript_stabilization_tests)

add_executable(pscript_compatibility_tests
    tests/compatibility_test.cpp
)
target_link_libraries(pscript_compatibility_tests PRIVATE pscript_vm pscript_compiler pscript_parser pscript_stdlib)
target_include_directories(pscript_compatibility_tests PRIVATE ${CMAKE_BINARY_DIR}/generated src)
add_test(NAME compatibility COMMAND pscript_compatibility_tests)

add_executable(pscript_debug_runtime_tests
    tests/debug_runtime_test.cpp
)
target_link_libraries(pscript_debug_runtime_tests PRIVATE pscript_debug pscript_vm pscript_compiler pscript_parser)
if (MSVC)
    target_link_options(pscript_debug_runtime_tests PRIVATE "/STACK:33554432")
endif()
set_target_properties(pscript_debug_runtime_tests PROPERTIES
    OUTPUT_NAME "debug_runtime_tests"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
add_test(NAME debug_runtime COMMAND "$<TARGET_FILE:pscript_debug_runtime_tests>")

add_executable(pscript_debug_matrix_tests
    tests/debug_matrix_test.cpp
)
set_target_properties(pscript_debug_matrix_tests PROPERTIES OUTPUT_NAME "debug_matrix_tests")
target_include_directories(pscript_debug_matrix_tests PRIVATE tests)
target_link_libraries(pscript_debug_matrix_tests PRIVATE pscript_debug pscript_vm pscript_compiler pscript_parser pscript_jit)
if (MSVC)
    target_link_options(pscript_debug_matrix_tests PRIVATE "/STACK:33554432")
endif()
set_target_properties(pscript_debug_matrix_tests PROPERTIES
    OUTPUT_NAME "debug_matrix_tests"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
add_test(NAME debug_matrix COMMAND "$<TARGET_FILE:pscript_debug_matrix_tests>")

add_executable(pscript_benchmark
    tests/pscript_benchmark.cpp
)
target_include_directories(pscript_benchmark PRIVATE tests ${CMAKE_BINARY_DIR}/generated src)
target_link_libraries(pscript_benchmark PRIVATE pscript_vm pscript_compiler pscript_parser pscript_lexer pscript_jit)
if(MSVC)
    target_link_options(pscript_benchmark PRIVATE "/STACK:33554432")
endif()

add_executable(crosslang_cpp
    benchmarks/crosslang/crosslang_bench_main.cpp
)
target_include_directories(crosslang_cpp PRIVATE benchmarks/crosslang)
if(MSVC)
    target_compile_options(crosslang_cpp PRIVATE $<$<CONFIG:Release>:/O2>)
endif()
set_target_properties(crosslang_cpp PROPERTIES
    OUTPUT_NAME "crosslang_cpp"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

add_executable(crosslang_pscript
    benchmarks/crosslang/crosslang_pscript_runner.cpp
)
target_include_directories(crosslang_pscript PRIVATE benchmarks/crosslang tests ${CMAKE_BINARY_DIR}/generated src)
target_link_libraries(crosslang_pscript PRIVATE pscript_vm pscript_compiler pscript_parser pscript_lexer pscript_jit)
if(MSVC)
    target_link_options(crosslang_pscript PRIVATE "/STACK:33554432")
endif()
set_target_properties(crosslang_pscript PROPERTIES
    OUTPUT_NAME "crosslang_pscript"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

add_test(NAME analysis COMMAND pscript_analysis_tests)
add_test(NAME lsp COMMAND pscript_lsp_tests)
add_test(NAME lsp_server_stdio COMMAND pscript_lsp_server_stdio_tests)
add_test(NAME dap_stdio COMMAND pscript_dap_stdio_tests)
add_test(NAME cli COMMAND pscript_cli_tests)
add_test(NAME package COMMAND pscript_package_tests)
add_test(NAME registry COMMAND pscript_registry_tests)
if(PostgreSQL_FOUND)
    add_test(NAME registry_postgres COMMAND pscript_registry_postgres_tests)
    set_tests_properties(registry_postgres PROPERTIES
        LABELS "postgres"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(DEFINED ENV{PSCRIPT_REGISTRY_POSTGRES_TEST_URL})
        set_tests_properties(registry_postgres PROPERTIES
            ENVIRONMENT "PSCRIPT_REGISTRY_POSTGRES_TEST_URL=$ENV{PSCRIPT_REGISTRY_POSTGRES_TEST_URL}")
    elseif(PSCRIPT_REGISTRY_POSTGRES_TEST_URL)
        set_tests_properties(registry_postgres PROPERTIES
            ENVIRONMENT "PSCRIPT_REGISTRY_POSTGRES_TEST_URL=${PSCRIPT_REGISTRY_POSTGRES_TEST_URL}")
    endif()
endif()
set(PSCRIPT_REGISTRY_POSTGRES_TEST_URL "$ENV{PSCRIPT_REGISTRY_POSTGRES_TEST_URL}" CACHE STRING
    "PostgreSQL integration test URL (re-configure CMake after setting)")
set_tests_properties(lexer parser compiler vm runtime runtime_stress profiling pgo jit jit_opt jit_deopt stabilization compatibility debug_runtime debug_matrix analysis lsp lsp_server_stdio dap_stdio cli package registry parallel_map PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
