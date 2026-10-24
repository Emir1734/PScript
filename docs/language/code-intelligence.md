# Code Intelligence (Tooling Bölüm 2)

Shared analysis stack under `src/analysis/`:

```text
Source → Lexer → Parser → AST → compile() checks + linter → Diagnostics + SymbolIndex
```

## Diagnostics

`Diagnostic` carries severity (`error` / `warning` / `info`), code (`lexer`, `parser`, `type`, `unused`, …), message, file, and `SourceRange` (line/column).

Semantic errors reuse the existing `compile()` pipeline (no second type checker).

## Symbol index

`SymbolIndex` collects definitions from the AST (functions, classes, structs, enums, interfaces, modules, parameters). `AnalysisSession` exposes LSP-oriented queries: `goto_definition`, `find_references`, `completion_candidates`, `hover_text`.

## Formatter

`format_source` adjusts indentation and trailing whitespace while preserving `:` vs `{}` delimiters in the original source.

## Linter (v1)

- unused locals (simple use/def within a block)
- unreachable code after `return` / `throw` / `break` / `continue`
- empty expression statements

## API entry points

- `analyze_source(source, options)` → `AnalysisResult`
- `format_source(source, options)` → formatted text
- `AnalysisSession` for tooling queries

Editor integration (VS Code extension + `pscript_lsp`) is documented in [Editor Integration](editor-integration.md).
