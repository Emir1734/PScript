#pragma once

#include "compiler/bytecode.h"
#include "compiler/compiler.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pscript {

std::optional<std::string> resolve_module_path(const std::string& specifier, const std::string& importer_file);
std::string canonicalize_path(const std::string& path);
std::optional<std::string> read_module_source(const std::string& path);

enum class ModuleCompileState {
    Loading,
    Loaded,
    Failed,
};

class ModuleCompileCache {
public:
    void set_root(Bytecode* root);
    std::uint32_t compile_module(const std::string& specifier, const CompileOptions& importer_options, Bytecode& root,
                                 const Location& loc);
    bool has_export(std::uint32_t module_index, const std::string& name) const;
    const ModuleExport* find_export(std::uint32_t module_index, const std::string& name) const;
    const ModuleDescriptor& descriptor(std::uint32_t module_index) const;
    ModuleCompileState compile_state(std::uint32_t module_index) const;
    void refresh_module_exports(std::uint32_t module_index, const std::vector<ModuleExport>& exports);

private:
    Bytecode* root_ = nullptr;
    std::unordered_map<std::string, std::uint32_t> path_to_module_index_;
    std::unordered_map<std::uint32_t, ModuleCompileState> compile_states_;
    std::uint32_t current_compiling_index_ = 0xFFFFFFFFu;

    std::uint32_t compile_module_file(const std::string& specifier, const std::string& path,
                                      const CompileOptions& importer_options, Bytecode& root, const Location& loc);
    void set_compile_state(std::uint32_t module_index, ModuleCompileState state);
};

}  // namespace pscript
