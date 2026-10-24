#pragma once

#include "analysis/diagnostics.h"
#include "parser/ast.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

enum class SymbolKind {
    Module,
    Function,
    Method,
    Constructor,
    Class,
    Struct,
    Enum,
    Interface,
    Property,
    Variable,
    Parameter,
    Import,
};

enum class SymbolVisibility { Public, Protected, Private };

struct SymbolRecord {
    std::uint32_t id = 0;
    SymbolKind kind = SymbolKind::Variable;
    std::string name;
    std::string qualified_name;
    std::optional<std::string> type_signature;
    SourceRange range;
    std::optional<std::uint32_t> parent_id;
    SymbolVisibility visibility = SymbolVisibility::Public;
    std::string file;
};

class SymbolIndex {
public:
    void clear();
    std::uint32_t add(SymbolRecord symbol);
    const std::vector<SymbolRecord>& all() const { return symbols_; }
    const SymbolRecord* find_by_qualified_name(const std::string& qualified) const;
    const SymbolRecord* find_at_position(const std::string& file, int line, int column) const;
    std::vector<const SymbolRecord*> find_by_name(std::string_view simple_name) const;

private:
    std::vector<SymbolRecord> symbols_;
    std::uint32_t next_id_ = 1;
};

void build_symbol_index(const Program& program, const std::string& file, SymbolIndex& index);

}  // namespace pscript
