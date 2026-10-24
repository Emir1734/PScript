# Best-effort copy of the compile-stage LSP binary to pscript_lsp.exe for CTest/CLI.
# Does not fail the build when pscript_lsp.exe is locked by a running language server.
if(NOT EXISTS "${SRC}")
    return()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}" RESULT_VARIABLE copy_result)
if(copy_result AND NOT copy_result EQUAL 0)
    message(STATUS "LSP canonical copy skipped (destination may be in use): ${DST}")
endif()
