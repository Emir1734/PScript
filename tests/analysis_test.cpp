#include "analysis/diagnostics.h"
#include "analysis/lsp_support.h"
#include "analysis/query_api.h"
#include "analysis/symbol_index.h"
#include "lexer/keywords.h"

#include <iostream>
#include <string>
#include <unordered_set>

using pscript::AnalysisOptions;
using pscript::AnalysisResult;
using pscript::AnalysisSession;
using pscript::DiagnosticSeverity;
using pscript::analyze_source;
using pscript::format_source;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

bool has_error_code(const AnalysisResult& result, pscript::DiagnosticCode code) {
    for (const pscript::Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "bad.phs";
        const AnalysisResult result = analyze_source("x =\n", options);
        if (result.parse_ok || !has_error_code(result, pscript::DiagnosticCode::ParserError)) {
            fail_test("parser diagnostic", "expected parser error");
        } else {
            pass_test("parser diagnostic");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "undef.phs";
        const AnalysisResult result = analyze_source("print(missing)\n", options);
        if (result.compile_ok || !has_error_code(result, pscript::DiagnosticCode::NameError)) {
            fail_test("undefined symbol", "expected name/compile error");
        } else {
            pass_test("undefined symbol");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "dup.phs";
        const AnalysisResult result = analyze_source("int x = 1\nint x = 2\n", options);
        if (result.compile_ok) {
            fail_test("duplicate declaration", "expected compile error");
        } else {
            pass_test("duplicate declaration");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "sym.phs";
        const AnalysisResult result = analyze_source("func add(a, b):\n    return a + b\n", options);
        const auto matches = result.symbols.find_by_name("add");
        if (matches.empty()) {
            fail_test("symbol index function", "missing add");
        } else {
            pass_test("symbol index function");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "class.phs";
        const AnalysisResult result =
            analyze_source("class Player:\n    func score():\n        return 1\n", options);
        if (result.symbols.find_by_name("Player").empty()) {
            fail_test("symbol index class", "missing Player");
        } else {
            pass_test("symbol index class");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "fmt.phs";
        const std::string source = "func f():\n  print(1)\n";
        const std::string once = format_source(source, options);
        const std::string twice = format_source(once, options);
        if (once != twice) {
            fail_test("formatter idempotent", "output changed on second pass");
        } else if (once.find("    print(1)") == std::string::npos) {
            fail_test("formatter indent", "expected 4-space indent");
        } else {
            pass_test("formatter idempotent");
            pass_test("formatter indent");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "lint.phs";
        const AnalysisResult result = analyze_source("func f():\n    x = 1\n    return 1\n", options);
        bool found_unused = false;
        for (const pscript::Diagnostic& diagnostic : result.diagnostics) {
            if (diagnostic.code == pscript::DiagnosticCode::UnusedSymbol) {
                found_unused = true;
            }
        }
        if (!found_unused) {
            fail_test("lint unused", "expected unused warning");
        } else {
            pass_test("lint unused");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "hover.phs";
        AnalysisResult result = analyze_source("func greet():\n    return 1\n", options);
        AnalysisSession session(std::move(result));
        const std::string src = "func greet():\n    return 1\n";
        const auto hover = session.hover_text(src, "hover.phs", 1, 7);
        if (!hover.has_value() || hover->find("greet") == std::string::npos) {
            fail_test("hover api", "missing greet hover");
        } else {
            pass_test("hover api");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "complete.phs";
        AnalysisResult result = analyze_source("isim = \"Emir\"\n", options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site;
        const auto items = session.completion_items("i", nullptr, site);
        bool has_isim = false;
        bool has_if = false;
        for (const pscript::LspCompletionItem& item : items) {
            if (item.label == "isim") {
                has_isim = true;
            }
            if (item.label == "if") {
                has_if = true;
            }
        }
        if (!has_isim || !has_if) {
            fail_test("completion prefix i", "expected isim and if");
        } else {
            pass_test("completion prefix i");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "lenient.phs";
        options.lenient_incomplete_source = true;
        const char* fragments[] = {"p", "pr", "print", "print ", "while", "wh", "print \"merhaba\""};
        bool lenient_ok = true;
        for (const char* fragment : fragments) {
            const AnalysisResult result = analyze_source(fragment, options);
            if (!result.diagnostics.empty()) {
                lenient_ok = false;
                fail_test("lenient incomplete", fragment);
                break;
            }
        }
        if (lenient_ok) {
            pass_test("lenient incomplete");
        }
        const AnalysisResult open_string = analyze_source("print \"merhaba", options);
        if (!open_string.diagnostics.empty()) {
            fail_test("lenient open string", "expected no diagnostic while typing string");
        } else {
            pass_test("lenient open string");
        }
        const AnalysisResult valid = analyze_source("print \"merhaba\"\n", options);
        if (!valid.parse_ok || !valid.diagnostics.empty()) {
            fail_test("lenient valid print", "expected clean parse");
        } else {
            pass_test("lenient valid print");
        }
        const AnalysisResult unicode_typing = analyze_source("ç", options);
        if (!unicode_typing.diagnostics.empty()) {
            fail_test("lenient unicode typing", "expected no lexer diagnostic for ç while typing");
        } else {
            pass_test("lenient unicode typing");
        }
        const AnalysisResult unicode_string = analyze_source("print(\"merhaba ç\")\n", options);
        if (!unicode_string.parse_ok || !unicode_string.diagnostics.empty()) {
            fail_test("unicode in string literal", "expected valid source");
        } else {
            pass_test("unicode in string literal");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "kw.phs";
        AnalysisResult result = analyze_source("\n", options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site;
        const auto items = session.completion_items("w", nullptr, site);
        bool has_while = false;
        for (const pscript::LspCompletionItem& item : items) {
            if (item.label == "while") {
                has_while = true;
            }
        }
        if (!has_while) {
            fail_test("completion prefix w", "expected while keyword");
        } else {
            pass_test("completion prefix w");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "prefix.phs";
        AnalysisResult result = analyze_source("\n", options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site;
        auto has_label = [&](std::string_view prefix, std::string_view label) {
            for (const pscript::LspCompletionItem& item : session.completion_items(prefix, nullptr, site)) {
                if (item.label == label) {
                    return true;
                }
            }
            return false;
        };
        if (!has_label("p", "print") || !has_label("pr", "print") || !has_label("pri", "print")) {
            fail_test("completion print prefix", "expected print for p/pr/pri");
        } else {
            pass_test("completion print prefix");
        }
        if (!has_label("i", "int") || !has_label("in", "int") || !has_label("int", "int")) {
            fail_test("completion int prefix", "expected int for i/in/int");
        } else {
            pass_test("completion int prefix");
        }
        if (!has_label("s", "string") || !has_label("st", "str")) {
            fail_test("completion string/str prefix", "expected string and str");
        } else {
            pass_test("completion string/str prefix");
        }
        {
            std::unordered_set<std::string> labels;
            for (const pscript::LspCompletionItem& item : session.completion_items("", nullptr, site)) {
                labels.insert(item.label);
            }
            bool missing = false;
            for (std::string_view id : pscript::language_public_completion_identifiers()) {
                if (id == "continue" || id == "break" || id == "return") {
                    continue;
                }
                if (!labels.contains(std::string(id))) {
                    missing = true;
                    fail_test("completion catalog sync", std::string(id));
                    break;
                }
            }
            if (!missing) {
                pass_test("completion catalog sync");
            }
        }
        if (has_label("c", "continue")) {
            fail_test("completion continue context", "continue outside loop");
        } else {
            pass_test("completion continue context");
        }
        AnalysisResult loop_src = analyze_source("while true:\n    \n", options);
        AnalysisSession loop_session(std::move(loop_src));
        const pscript::CompletionSiteContext loop_site =
            pscript::completion_site_at("while true:\n    \n", 2, 5);
        bool has_continue = false;
        for (const pscript::LspCompletionItem& item : loop_session.completion_items("c", nullptr, loop_site)) {
            if (item.label == "continue") {
                has_continue = true;
            }
        }
        if (!has_continue) {
            fail_test("completion continue in loop", "expected continue");
        } else {
            pass_test("completion continue in loop");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "import ";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 1, 8);
        bool has_math = false;
        bool has_match = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("", nullptr, site)) {
            if (item.label == "math") {
                has_math = true;
            }
            if (item.label == "match") {
                has_match = true;
            }
        }
        if (!has_math || has_match) {
            fail_test("import module completion", "expected math, not match keyword");
        } else {
            pass_test("import module completion");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import_ma.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "import ma";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 1, 9);
        bool has_math = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("ma", nullptr, site)) {
            if (item.label == "math") {
                has_math = true;
            }
        }
        if (!has_math) {
            fail_test("import ma prefix", "expected math");
        } else {
            pass_test("import ma prefix");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "from_import.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "from math import s";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 1, 18);
        bool has_sqrt = false;
        bool has_string_type = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("s", nullptr, site)) {
            if (item.label == "sqrt") {
                has_sqrt = true;
            }
            if (item.label == "string") {
                has_string_type = true;
            }
        }
        if (site.lexical != pscript::CompletionLexicalContext::FromImportBinding) {
            fail_test("from math import binding", "expected from-import binding context");
        } else if (!has_sqrt || has_string_type) {
            fail_test("from math import binding", "expected sqrt export only");
        } else {
            pass_test("from math import binding");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "mat_ctx.phs";
        AnalysisResult result = analyze_source("x = 1\n", options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at("x = 1\nmat", 2, 3);
        bool has_match = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("mat", nullptr, site)) {
            if (item.label == "match") {
                has_match = true;
            }
        }
        if (has_match) {
            fail_test("mat prefix no loose match keyword", "match should not appear for mat");
        } else {
            pass_test("mat prefix no loose match keyword");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "string_ctx.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "print(\"mat";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 1, 11);
        if (site.lexical != pscript::CompletionLexicalContext::StringLiteral) {
            fail_test("completion string lexical", "expected string literal context");
        } else if (!session.completion_items("mat", nullptr, site).empty()) {
            fail_test("completion string lexical", "expected no completion items in string");
        } else {
            pass_test("completion string lexical");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "comment_ctx.phs";
        const std::string source = "# mat\n";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 1, 5);
        if (site.lexical != pscript::CompletionLexicalContext::Comment) {
            fail_test("completion comment lexical", "expected comment context");
        } else if (!session.completion_items("mat", nullptr, site).empty()) {
            fail_test("completion comment lexical", "expected no completion items in comment");
        } else {
            pass_test("completion comment lexical");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "member.phs";
        const std::string source = "class Player:\n    public string name\n    private int secret\n";
        const std::string completion_source = source + "Player.\n";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const std::optional<pscript::MemberAccessCompletionContext> member =
            pscript::member_access_context_at(completion_source, 4, 8);
        if (!member.has_value() || member->receiver != "Player") {
            fail_test("member completion context", "expected Player receiver");
        } else {
            bool has_name = false;
            bool has_secret = false;
            bool has_print = false;
            for (const pscript::LspCompletionItem& item :
                 session.completion_items("", &*member, pscript::CompletionSiteContext{})) {
                if (item.label == "name") {
                    has_name = true;
                }
                if (item.label == "secret") {
                    has_secret = true;
                }
                if (item.label == "print") {
                    has_print = true;
                }
            }
            if (!has_name || has_secret || has_print) {
                fail_test("member completion properties",
                          "expected public field name only (no private secret or global print)");
            } else {
                pass_test("member completion properties");
            }
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "stdlib_math_member.phs";
        options.lenient_incomplete_source = true;
        const std::string full_doc = "import math\nmath.\n";
        const std::string source = "import math\n";
        const std::string completion_source = source + "math.\n";
        AnalysisResult full_result = analyze_source(full_doc, options);
        AnalysisSession full_session(std::move(full_result));
        const std::optional<pscript::MemberAccessCompletionContext> full_member =
            pscript::member_access_context_at(full_doc, 2, 6);
        bool full_has_sqrt = false;
        if (full_member.has_value()) {
            for (const pscript::LspCompletionItem& item :
                 full_session.completion_items("", &*full_member, pscript::CompletionSiteContext{})) {
                if (item.label == "sqrt") {
                    full_has_sqrt = true;
                }
            }
        }
        if (!full_has_sqrt) {
            fail_test("math member with incomplete tail line", "expected sqrt when document ends with math.");
        } else {
            pass_test("math member with incomplete tail line");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "stdlib_math_member.phs";
        const std::string source = "import math\n";
        const std::string completion_source = source + "math.\n";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const std::optional<pscript::MemberAccessCompletionContext> member =
            pscript::member_access_context_at(completion_source, 2, 6);
        if (!member.has_value() || member->receiver != "math") {
            fail_test("math module member context", "expected math receiver");
        } else {
            bool has_sqrt = false;
            bool has_match = false;
            for (const pscript::LspCompletionItem& item :
                 session.completion_items("", &*member, pscript::CompletionSiteContext{})) {
                if (item.label == "sqrt") {
                    has_sqrt = true;
                }
                if (item.label == "match") {
                    has_match = true;
                }
            }
            if (!has_sqrt || has_match) {
                fail_test("math module member exports", "expected sqrt export, not match keyword");
            } else {
                pass_test("math module member exports");
            }
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "stdlib_string_member.phs";
        const std::string source = "import string\n";
        const std::string completion_source = source + "string.\n";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const std::optional<pscript::MemberAccessCompletionContext> member =
            pscript::member_access_context_at(completion_source, 2, 8);
        bool has_join = false;
        if (member.has_value()) {
            for (const pscript::LspCompletionItem& item :
                 session.completion_items("", &*member, pscript::CompletionSiteContext{})) {
                if (item.label == "join") {
                    has_join = true;
                }
            }
        }
        if (!has_join) {
            fail_test("string module member exports", "expected join export");
        } else {
            pass_test("string module member exports");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import_alias.phs";
        const std::string source = "import math as m\n";
        const std::string completion_source = source + "m.\n";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const std::optional<pscript::MemberAccessCompletionContext> member =
            pscript::member_access_context_at(completion_source, 2, 3);
        bool has_pow = false;
        if (member.has_value() && member->receiver == "m") {
            for (const pscript::LspCompletionItem& item :
                 session.completion_items("p", &*member, pscript::CompletionSiteContext{})) {
                if (item.label == "pow") {
                    has_pow = true;
                }
            }
        }
        if (!has_pow) {
            fail_test("import alias module member", "expected pow via alias m");
        } else {
            pass_test("import alias module member");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import_binding.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "import math\nm";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 2, 2);
        bool has_math = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("m", nullptr, site)) {
            if (item.label == "math") {
                has_math = true;
            }
        }
        if (!has_math) {
            fail_test("import binding completion", "expected math for prefix m");
        } else {
            pass_test("import binding completion");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import_alias_binding.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "import math as mm\nmm";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 2, 3);
        bool has_alias = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("mm", nullptr, site)) {
            if (item.label == "mm") {
                has_alias = true;
            }
        }
        bool has_alias_prefix = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("m", nullptr, site)) {
            if (item.label == "mm") {
                has_alias_prefix = true;
            }
        }
        if (!has_alias || !has_alias_prefix) {
            fail_test("import alias binding completion", "expected alias mm");
        } else {
            pass_test("import alias binding completion");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import_alias_member.phs";
        options.lenient_incomplete_source = true;
        const std::string source = "import math as mm\nmm.";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const std::optional<pscript::MemberAccessCompletionContext> member =
            pscript::member_access_context_at(source, 2, 4);
        bool has_sqrt = false;
        bool has_pi = false;
        bool has_pow = false;
        if (member.has_value() && member->receiver == "mm") {
            for (const pscript::LspCompletionItem& item :
                 session.completion_items("", &*member, pscript::CompletionSiteContext{})) {
                if (item.label == "sqrt") {
                    has_sqrt = true;
                }
                if (item.label == "pi") {
                    has_pi = true;
                }
                if (item.label == "pow") {
                    has_pow = true;
                }
            }
        }
        if (!has_sqrt || !has_pi || !has_pow) {
            fail_test("import alias member exports", "expected math exports through alias mm");
        } else {
            pass_test("import alias member exports");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "local.phs";
        const std::string source = "price = 10\nprint(price)\n";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source + "pri", 3, 4);
        bool has_price = false;
        bool has_print_kw = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("pri", nullptr, site)) {
            if (item.label == "price") {
                has_price = true;
            }
            if (item.label == "print" && item.kind == pscript::LspCompletionItemKind::Keyword) {
                has_print_kw = true;
            }
        }
        if (!has_price || has_print_kw) {
            fail_test("local variable completion", "expected price variable, not print keyword");
        } else {
            pass_test("local variable completion");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "import_kw.phs";
        const std::string source = "import ";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(source, 1, 8);
        bool has_match = false;
        bool has_math = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("", nullptr, site)) {
            if (item.label == "match") {
                has_match = true;
            }
            if (item.label == "math") {
                has_math = true;
            }
        }
        if (has_match || !has_math) {
            fail_test("import no statement keywords", "expected math module, not match keyword");
        } else {
            pass_test("import no statement keywords");
        }
    }

    {
        AnalysisOptions options;
        options.inject_exception_prelude = false;
        options.file = "mid_id.phs";
        const std::string source = "price = 1\n";
        const std::string doc = source + "pri";
        AnalysisResult result = analyze_source(source, options);
        AnalysisSession session(std::move(result));
        const pscript::CompletionSiteContext site = pscript::completion_site_at(doc, 2, 3);
        bool has_price = false;
        for (const pscript::LspCompletionItem& item : session.completion_items("pri", nullptr, site)) {
            if (item.label == "price") {
                has_price = true;
            }
        }
        if (!has_price) {
            fail_test("identifier prefix completion", "expected price for pri");
        } else {
            pass_test("identifier prefix completion");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All analysis tests passed\n";
    return 0;
}
