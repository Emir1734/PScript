#include "compiler/module_loader.h"

#include "package/install.h"
#include "parser/parser.h"
#include "stdlib/stdlib.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace pscript {
namespace {

std::filesystem::path parent_directory(const std::string& file_path) {
    if (file_path.empty() || file_path == "<stdin>") {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(file_path).parent_path();
}

bool contains_path_traversal(const std::string& specifier) {
    return specifier.find("..") != std::string::npos || specifier.find('/') != std::string::npos ||
           specifier.find('\\') != std::string::npos;
}

std::filesystem::path specifier_to_relative_path(const std::string& specifier) {
    std::filesystem::path result;
    std::size_t start = 0;
    while (start < specifier.size()) {
        const std::size_t dot = specifier.find('.', start);
        const std::string part = dot == std::string::npos ? specifier.substr(start) : specifier.substr(start, dot - start);
        if (!part.empty()) {
            result /= part;
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    result.replace_extension(".phs");
    return result;
}

}  // namespace

void ModuleCompileCache::set_root(Bytecode* root) {
    root_ = root;
}

void ModuleCompileCache::set_compile_state(std::uint32_t module_index, ModuleCompileState state) {
    compile_states_[module_index] = state;
}

ModuleCompileState ModuleCompileCache::compile_state(std::uint32_t module_index) const {
    const auto found = compile_states_.find(module_index);
    if (found == compile_states_.end()) {
        return ModuleCompileState::Loaded;
    }
    return found->second;
}

void ModuleCompileCache::refresh_module_exports(std::uint32_t module_index,
                                                const std::vector<ModuleExport>& exports) {
    if (!root_ || module_index >= root_->modules.size()) {
        return;
    }
    root_->modules[module_index].exports = exports;
}

std::optional<std::string> resolve_module_path(const std::string& specifier, const std::string& importer_file) {
    if (specifier.empty() || contains_path_traversal(specifier)) {
        return std::nullopt;
    }
    if (const std::optional<std::string> package_path = package::resolve_package_module(specifier, importer_file)) {
        return package_path;
    }
    const std::filesystem::path base = parent_directory(importer_file);
    const std::filesystem::path candidate = base / specifier_to_relative_path(specifier);
    if (!std::filesystem::exists(candidate)) {
        const std::filesystem::path flat = base / (specifier + ".phs");
        if (std::filesystem::exists(flat)) {
            return flat.lexically_normal().string();
        }
        return std::nullopt;
    }
    return candidate.lexically_normal().string();
}

std::string canonicalize_path(const std::string& path) {
    return std::filesystem::weakly_canonical(std::filesystem::path(path)).string();
}

std::optional<std::string> read_module_source(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return std::nullopt;
    }
    return buffer.str();
}

std::uint32_t ModuleCompileCache::compile_module(const std::string& specifier, const CompileOptions& importer_options,
                                                 Bytecode& root, const Location& loc) {
    set_root(&root);
    const std::optional<std::string> resolved = resolve_module_path(specifier, importer_options.source_file);
    if (!resolved) {
        if (is_stdlib_module_specifier(specifier)) {
            const std::string canonical = "<stdlib/" + specifier + ">";
            const auto found = path_to_module_index_.find(canonical);
            if (found != path_to_module_index_.end()) {
                return found->second;
            }
            const std::uint32_t index = register_stdlib_module(specifier, root);
            path_to_module_index_[canonical] = index;
            set_compile_state(index, ModuleCompileState::Loaded);
            return index;
        }
        ModuleDescriptor pending;
        pending.specifier = specifier;
        pending.bytecode_index = 0xFFFFFFFFu;
        const std::uint32_t index = static_cast<std::uint32_t>(root.modules.size());
        root.modules.push_back(std::move(pending));
        set_compile_state(index, ModuleCompileState::Failed);
        return index;
    }
    const std::string canonical = canonicalize_path(*resolved);
    const auto found = path_to_module_index_.find(canonical);
    if (found != path_to_module_index_.end()) {
        return found->second;
    }
    return compile_module_file(specifier, canonical, importer_options, root, loc);
}

std::uint32_t ModuleCompileCache::compile_module_file(const std::string& specifier, const std::string& path,
                                                      const CompileOptions& importer_options, Bytecode& root,
                                                      const Location& loc) {
    const std::optional<std::string> source = read_module_source(path);
    if (!source) {
        ModuleDescriptor pending;
        pending.specifier = specifier;
        pending.canonical_path = path;
        const std::uint32_t index = static_cast<std::uint32_t>(root.modules.size());
        root.modules.push_back(std::move(pending));
        path_to_module_index_[path] = index;
        set_compile_state(index, ModuleCompileState::Failed);
        return index;
    }

    ModuleDescriptor placeholder;
    placeholder.specifier = specifier;
    placeholder.canonical_path = path;
    const std::uint32_t module_index = static_cast<std::uint32_t>(root.modules.size());
    root.modules.push_back(placeholder);
    path_to_module_index_[path] = module_index;
    set_compile_state(module_index, ModuleCompileState::Loading);
    current_compiling_index_ = module_index;

    CompileOptions module_options;
    module_options.source_file = path;
    module_options.source_line_base = 0;

    try {
        Compiler compiler;
        compiler.set_compile_options(module_options);
        compiler.set_module_mode(true);
        compiler.set_module_cache(this);
        compiler.set_root_bytecode(&root);
        compiler.set_compiling_module_index(module_index);
        const Bytecode module_bc = compiler.compile(parse(*source));

        const std::uint32_t bytecode_index = static_cast<std::uint32_t>(root.module_bytecodes.size());
        root.module_bytecodes.push_back(module_bc);

        root.modules[module_index].specifier = specifier;
        root.modules[module_index].canonical_path = path;
        root.modules[module_index].bytecode_index = bytecode_index;
        root.modules[module_index].exports = compiler.module_exports();
        set_compile_state(module_index, ModuleCompileState::Loaded);
    } catch (...) {
        set_compile_state(module_index, ModuleCompileState::Failed);
        current_compiling_index_ = 0xFFFFFFFFu;
        throw;
    }

    current_compiling_index_ = 0xFFFFFFFFu;
    return module_index;
}

bool ModuleCompileCache::has_export(std::uint32_t module_index, const std::string& name) const {
    return find_export(module_index, name) != nullptr;
}

const ModuleExport* ModuleCompileCache::find_export(std::uint32_t module_index, const std::string& name) const {
    if (!root_ || module_index >= root_->modules.size()) {
        return nullptr;
    }
    for (const ModuleExport& export_info : root_->modules[module_index].exports) {
        if (export_info.name == name) {
            return &export_info;
        }
    }
    return nullptr;
}

const ModuleDescriptor& ModuleCompileCache::descriptor(std::uint32_t module_index) const {
    static const ModuleDescriptor empty;
    if (!root_ || module_index >= root_->modules.size()) {
        return empty;
    }
    return root_->modules[module_index];
}

}  // namespace pscript
