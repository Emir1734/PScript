#include "analysis/lsp_support.h"
#include "analysis/query_api.h"
#include "lsp/json_util.h"
#include "lsp/text_sync.h"
#include "lsp/uri_util.h"

#include <iostream>
#include <sstream>
#include <string>

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

}  // namespace

int main() {
    {
        const std::string source = "func greet():\n    print(greet)\n";
        const auto id = pscript::identifier_at_position(source, 1, 7);
        if (!id.has_value() || *id != "greet") {
            fail_test("identifier at position", "unexpected identifier");
        } else {
            pass_test("identifier at position");
        }
    }

    {
        const std::string source = "func greet():\n    print(greet)\n";
        const std::vector<pscript::LspLocation> refs =
            pscript::find_identifier_occurrences(source, "greet", "file:///test.phs");
        if (refs.size() < 2) {
            fail_test("identifier references", "expected at least 2 occurrences");
        } else {
            pass_test("identifier references");
        }
    }

    {
        const std::string source = "func greet():\n    print(greet)\n";
        const std::vector<pscript::LspTextEdit> edits =
            pscript::rename_identifier_at(source, 1, 7, "hello");
        if (edits.size() < 2) {
            fail_test("rename edits", "expected multiple edits");
        } else if (edits.front().new_text != "hello") {
            fail_test("rename edits", "bad new text");
        } else {
            pass_test("rename edits");
        }
    }

    {
        const std::optional<pscript::WordPrefixAtPosition> prefix =
            pscript::word_prefix_at_position("print", 1, 6);
        if (!prefix.has_value() || prefix->prefix != "print") {
            fail_test("word prefix print keyword", prefix.has_value() ? prefix->prefix : "missing");
        } else {
            pass_test("word prefix print keyword");
        }
    }

    {
        const std::optional<pscript::WordPrefixAtPosition> prefix =
            pscript::word_prefix_at_position("pr", 1, 3);
        if (!prefix.has_value() || prefix->prefix != "pr") {
            fail_test("word prefix pr", prefix.has_value() ? prefix->prefix : "missing");
        } else {
            pass_test("word prefix pr");
        }
    }

    {
        const std::string source = "x = pri + 10";
        const std::optional<pscript::WordPrefixAtPosition> prefix =
            pscript::word_prefix_at_position(source, 1, 7);
        if (!prefix.has_value() || prefix->prefix != "pri") {
            fail_test("word prefix mid-line pri", prefix.has_value() ? prefix->prefix : "missing");
        } else if (prefix->replace_range.start.character != 4 || prefix->replace_range.end.character != 7) {
            fail_test("word prefix mid-line pri range", "unexpected replace range");
        } else {
            pass_test("word prefix mid-line pri");
        }
    }

    {
        const std::string source = "print(pri)";
        const std::optional<pscript::WordPrefixAtPosition> prefix =
            pscript::word_prefix_at_position(source, 1, 9);
        if (!prefix.has_value() || prefix->prefix != "pri") {
            fail_test("word prefix inside call pri", prefix.has_value() ? prefix->prefix : "missing");
        } else if (prefix->replace_range.start.character != 6 || prefix->replace_range.end.character != 9) {
            fail_test("word prefix inside call pri range", "unexpected replace range");
        } else {
            pass_test("word prefix inside call pri");
        }
    }

    {
        const std::string source = "import math\nmath.\n";
        if (pscript::member_access_name_prefix(source, 2, 6) != "") {
            fail_test("member access prefix after dot", "expected empty suffix");
        } else if (pscript::member_access_name_prefix("import math\nmath.sq", 2, 9) != "sq") {
            fail_test("member access prefix partial", "expected sq");
        } else {
            pass_test("member access name prefix");
        }
    }

    {
        pscript::AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "sem.phs";
        pscript::AnalysisResult result = pscript::analyze_source("func greet():\n    return 1\n", options);
        pscript::AnalysisSession session(std::move(result));
        const std::vector<pscript::LspSemanticToken> tokens =
            session.semantic_tokens("func greet():\n    return 1\n");
        if (tokens.empty()) {
            fail_test("semantic tokens", "expected tokens");
        } else {
            pass_test("semantic tokens");
        }
    }

    {
        const auto expect_builtin = [&](std::string_view name, std::string_view source, int line_0, int col_0,
                                        pscript::LspSemanticTokenType expected) {
            pscript::AnalysisOptions options;
            options.inject_exception_prelude = false;
            options.file = "builtin_sem.phs";
            pscript::AnalysisResult result = pscript::analyze_source(std::string(source), options);
            pscript::AnalysisSession session(std::move(result));
            const std::vector<pscript::LspSemanticToken> tokens = session.semantic_tokens(source);
            bool found = false;
            for (const pscript::LspSemanticToken& token : tokens) {
                if (token.line == line_0 && token.start_char == col_0 &&
                    token.length == static_cast<int>(name.size())) {
                    if (token.type != expected) {
                        fail_test(std::string(name) + " semantic", "unexpected token type");
                    } else {
                        found = true;
                    }
                    break;
                }
            }
            if (!found) {
                fail_test(std::string(name) + " semantic", "token not found");
            }
        };
        expect_builtin("int", "int\n", 0, 0, pscript::LspSemanticTokenType::Type);
        expect_builtin("float", "float\n", 0, 0, pscript::LspSemanticTokenType::Type);
        expect_builtin("str", "str\n", 0, 0, pscript::LspSemanticTokenType::Type);
        expect_builtin("string", "string\n", 0, 0, pscript::LspSemanticTokenType::Type);
        expect_builtin("bool", "bool\n", 0, 0, pscript::LspSemanticTokenType::Type);
        expect_builtin("int", "x = int(\"123\")\n", 0, 4, pscript::LspSemanticTokenType::Function);
        expect_builtin("int", "x: int\n", 0, 3, pscript::LspSemanticTokenType::Type);
        if (pscript::fallback_semantic_for_identifier("int", "") == pscript::LspSemanticTokenType::Type) {
            pass_test("builtin semantic fallback int");
        } else {
            fail_test("builtin semantic fallback int", "expected type");
        }
    }

    {
        const auto token_type_at = [](std::string_view source, int line_0, int col_0, int length) {
            pscript::AnalysisOptions options;
            options.inject_exception_prelude = false;
            options.file = "sem_class.phs";
            options.lenient_incomplete_source = true;
            pscript::AnalysisResult result = pscript::analyze_source(std::string(source), options);
            pscript::AnalysisSession session(std::move(result));
            for (const pscript::LspSemanticToken& token : session.semantic_tokens(source)) {
                if (token.line == line_0 && token.start_char == col_0 && token.length == length) {
                    return token.type;
                }
            }
            return pscript::LspSemanticTokenType::Comment;
        };
        const std::string variables = "x = 10\nmy_variable = 20\n";
        if (token_type_at(variables, 0, 0, 1) != pscript::LspSemanticTokenType::Variable ||
            token_type_at(variables, 1, 0, 11) != pscript::LspSemanticTokenType::Variable) {
            fail_test("variable semantic stays variable", "x or my_variable misclassified");
        } else {
            pass_test("variable semantic stays variable");
        }
        const std::string shadowed = "func value():\n    return 1\nvalue = 2\nprint(value)\n";
        if (token_type_at(shadowed, 3, 6, 5) != pscript::LspSemanticTokenType::Variable) {
            fail_test("variable shadows function semantic", "use of value became a function");
        } else {
            pass_test("variable shadows function semantic");
        }
        const std::string typed = "class Player:\n    public string name\n\nplayer = Player()\nplayer.name\n";
        if (token_type_at(typed, 0, 6, 6) != pscript::LspSemanticTokenType::Class ||
            token_type_at(typed, 3, 0, 6) != pscript::LspSemanticTokenType::Variable ||
            token_type_at(typed, 4, 7, 4) != pscript::LspSemanticTokenType::Property) {
            fail_test("class variable member semantic", "Player/player/name misclassified");
        } else {
            pass_test("class variable member semantic");
        }
        const std::string primitive_decls =
            "int playerCount = 10\nfloat speed = 3.14\nstr name = \"test\"\nstring title = \"hello\"\n";
        if (token_type_at(primitive_decls, 0, 0, 3) != pscript::LspSemanticTokenType::Type ||
            token_type_at(primitive_decls, 0, 4, 11) != pscript::LspSemanticTokenType::Variable ||
            token_type_at(primitive_decls, 1, 0, 5) != pscript::LspSemanticTokenType::Type ||
            token_type_at(primitive_decls, 1, 6, 5) != pscript::LspSemanticTokenType::Variable ||
            token_type_at(primitive_decls, 2, 0, 3) != pscript::LspSemanticTokenType::Type ||
            token_type_at(primitive_decls, 2, 4, 4) != pscript::LspSemanticTokenType::Variable ||
            token_type_at(primitive_decls, 3, 0, 6) != pscript::LspSemanticTokenType::Type ||
            token_type_at(primitive_decls, 3, 7, 5) != pscript::LspSemanticTokenType::Variable) {
            fail_test("primitive declaration types semantic", "type keywords and bound names misclassified");
        } else {
            pass_test("primitive declaration types semantic");
        }
        const std::string lone_types = "int\nfloat\nstr\nstring\n";
        if (token_type_at(lone_types, 0, 0, 3) != pscript::LspSemanticTokenType::Type ||
            token_type_at(lone_types, 1, 0, 5) != pscript::LspSemanticTokenType::Type ||
            token_type_at(lone_types, 2, 0, 3) != pscript::LspSemanticTokenType::Type ||
            token_type_at(lone_types, 3, 0, 6) != pscript::LspSemanticTokenType::Type) {
            fail_test("lone primitive type keywords", "standalone int/float/str/string must stay Type");
        } else {
            pass_test("lone primitive type keywords");
        }
        const std::string module_use = "import math as mm\nmm.sqrt(4)\nmm.pi\n";
        if (token_type_at(module_use, 1, 0, 2) != pscript::LspSemanticTokenType::Namespace ||
            token_type_at(module_use, 1, 3, 4) != pscript::LspSemanticTokenType::Function ||
            token_type_at(module_use, 2, 3, 2) != pscript::LspSemanticTokenType::Variable) {
            fail_test("module member semantic", "alias or export misclassified");
        } else {
            pass_test("module member semantic");
        }
        const std::string incomplete = "class Player:\n    public string name\nplayer = Player()\nmy_variable";
        if (token_type_at(incomplete, 0, 6, 6) != pscript::LspSemanticTokenType::Class ||
            token_type_at(incomplete, 2, 0, 6) != pscript::LspSemanticTokenType::Variable ||
            token_type_at(incomplete, 3, 0, 11) != pscript::LspSemanticTokenType::Variable) {
            fail_test("incomplete edit keeps classifications", "symbols dropped or variable recolored");
        } else {
            pass_test("incomplete edit keeps classifications");
        }
    }

    {
        const std::optional<pscript::lsp::JsonValue> value =
            pscript::lsp::parse_json(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
        if (!value.has_value() || value->kind != pscript::lsp::JsonValue::Kind::Object) {
            fail_test("json parse", "invalid json");
        } else {
            pass_test("json parse");
        }
    }

    {
        const std::optional<pscript::lsp::JsonValue> change =
            pscript::lsp::parse_json(
                R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"text":"ç"})");
        if (!change.has_value()) {
            fail_test("incremental unicode insert", "invalid change json");
        } else {
            const std::string updated = pscript::lsp::apply_text_document_content_change("", *change);
            if (updated != "ç") {
                fail_test("incremental unicode insert", updated);
            } else {
                pass_test("incremental unicode insert");
            }
        }
    }

    {
        const std::optional<pscript::lsp::JsonValue> value =
            pscript::lsp::parse_json(R"({"text":"\u00e7"})");
        const std::optional<const pscript::lsp::JsonValue*> text_field =
            value.has_value() ? value->get("text") : std::nullopt;
        const pscript::lsp::JsonValue* text = text_field.has_value() ? *text_field : nullptr;
        if (text == nullptr || text->kind != pscript::lsp::JsonValue::Kind::String || text->string_value != "ç") {
            fail_test("json unicode escape", "expected decoded ç");
        } else {
            pass_test("json unicode escape");
        }
    }

    {
        const std::string path = pscript::lsp::uri_to_path("file:///C:/tmp/sample.phs");
        if (path.find("sample.phs") == std::string::npos) {
            fail_test("uri decode", path);
        } else {
            pass_test("uri decode");
        }
    }

    {
        bool threw = false;
        try {
            pscript::word_prefix_at_position("print(\"hello", 1, 13);
        } catch (...) {
            threw = true;
        }
        if (threw) {
            fail_test("word prefix incomplete string", "must not throw LexerError");
        } else {
            pass_test("word prefix incomplete string");
        }
    }

    {
        pscript::AnalysisOptions options;
        options.file = "unicode.phs";
        options.lenient_incomplete_source = true;
        options.run_linter = true;
        bool unicode_ok = true;
        for (const char* fragment : {"ç", "ö"}) {
            pscript::AnalysisResult unicode = pscript::analyze_source(fragment, options);
            pscript::AnalysisSession session(std::move(unicode));
            try {
                (void)session.semantic_tokens(fragment);
                (void)session.completion_items("", nullptr, pscript::CompletionSiteContext{});
            } catch (...) {
                unicode_ok = false;
                fail_test("lenient unicode fragment", fragment);
            }
        }
        if (unicode_ok) {
            pass_test("lenient unicode fragment");
        }
    }

    {
        pscript::AnalysisOptions options;
        options.file = "unicode_string.phs";
        options.lenient_incomplete_source = true;
        options.run_linter = true;
        bool string_ok = true;
        for (const char* source : {"print(\"ç\")\n", "print(\"ö\")\n"}) {
            pscript::AnalysisResult result = pscript::analyze_source(source, options);
            if (!result.parse_ok) {
                string_ok = false;
                fail_test("unicode string literal parse", source);
                continue;
            }
            pscript::AnalysisSession session(std::move(result));
            try {
                (void)session.semantic_tokens(source);
            } catch (...) {
                string_ok = false;
                fail_test("unicode string literal semantic", source);
            }
        }
        if (string_ok) {
            pass_test("unicode string literal");
        }
    }

    {
        pscript::AnalysisOptions options;
        options.file = "sem.phs";
        options.lenient_incomplete_source = true;
        pscript::AnalysisResult result = pscript::analyze_source("print(\"hello", options);
        pscript::AnalysisSession session(std::move(result));
        bool threw = false;
        try {
            session.semantic_tokens("print(\"hello");
        } catch (...) {
            threw = true;
        }
        if (threw) {
            fail_test("semantic tokens incomplete string", "must not throw LexerError");
        } else {
            pass_test("semantic tokens incomplete string");
        }
    }

    {
        if (!pscript::lsp::is_pscript_document_uri("file:///C:/tmp/x.phs", "C:/tmp/x.phs")) {
            fail_test("pscript uri guard", "expected .phs file uri");
        } else if (pscript::lsp::is_pscript_document_uri("/", "/")) {
            fail_test("pscript uri guard", "root uri must be rejected");
        } else if (pscript::lsp::is_pscript_document_uri("file:///", "/")) {
            fail_test("pscript uri guard", "file:/// root must be rejected");
        } else if (pscript::lsp::is_pscript_document_uri("file:///C:/tmp/x.phs", "C:/tmp/x.json")) {
            fail_test("pscript uri guard", "uri/path mismatch must be rejected");
        } else {
            pass_test("pscript uri guard");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All lsp tests passed\n";
    return 0;
}
