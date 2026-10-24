# Editor Integration (Tooling Bölüm 3)

PScript VS Code support lives in `editors/vscode/`. The extension provides syntax highlighting, snippets, and language configuration for both `:` and `{ }` block styles. Language features are backed by the C++ `pscript_lsp` server, which reuses `analyze_source` and `AnalysisSession` from `src/analysis/`.

## Architecture

```text
VS Code Extension (TypeScript)
        ↓ stdio LSP
pscript_lsp (C++)
        ↓
AnalysisSession / SymbolIndex / Diagnostics
        ↓
Lexer / Parser / compile() / linter
```

No second parser or type checker runs in the extension.

## Local development

1. Build the language server (Debug or Release):

   ```text
   cmake --build build --config Debug --target pscript_lsp
   ```

   The language server executable is `build/Debug/pscript_lsp.exe` (MSVC multi-config layout). Do **not** point the extension at `build/Debug/Debug/pscript_lsp.exe`; that nested path appears if the CMake binary directory is `build/Debug` instead of `build`, and the file is often an outdated copy.

2. Install extension dependencies and compile:

   ```text
   cd editors/vscode
   npm install
   npm run compile
   npm test
   ```

3. Launch the extension: open `editors/vscode` in VS Code and use **Run Extension** (F5), or install a VSIX (below).

4. Optional setting `pscript.languageServer.path` points to `pscript_lsp.exe` when the workspace build output is not auto-detected.

## VSIX packaging

```text
cd editors/vscode
npm install
npm run package
```

`npm run prepare-assets` copies the repository icon from `assets/icon.png` into `editors/vscode/assets/icon.png` for packaging. The extension `package.json` uses `"icon": "assets/icon.png"`. Marketplace publishing is out of scope for this document.

## LSP features

| Feature | LSP method | Analysis API |
|---------|------------|--------------|
| Diagnostics | `textDocument/publishDiagnostics` | `analyze_source` |
| Completion | `textDocument/completion` | `AnalysisSession::completion_candidates` |
| Hover | `textDocument/hover` | `hover_text` |
| Go to definition | `textDocument/definition` | `goto_definition` |
| References | `textDocument/references` | `references_in_source` |
| Rename | `textDocument/rename` | `rename_in_source` |
| Semantic tokens | `textDocument/semanticTokens/full` | `semantic_tokens` |

## `.phs` support

The extension registers language id `pscript` for `*.phs` files. TextMate grammar covers comments, strings, numbers, keywords, modifiers, and common types. Indentation rules treat both trailing `:` headers and `{` blocks as indent-increasing contexts.
