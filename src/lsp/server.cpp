#include "lsp/server.h"

#include "analysis/query_api.h"
#include "lsp/text_sync.h"
#include "lsp/uri_util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

namespace pscript::lsp {
namespace {

const char* kSemanticLegend[] = {"namespace", "type",      "class",     "enum",       "interface", "struct",
                                 "typeParameter", "parameter", "variable", "property", "enumMember", "function",
                                 "method",        "keyword",   "modifier", "comment",  "string",     "number",
                                 "operator"};

std::string diagnostic_severity_lsp(pscript::DiagnosticSeverity severity) {
    switch (severity) {
        case pscript::DiagnosticSeverity::Error:
            return "1";
        case pscript::DiagnosticSeverity::Warning:
            return "2";
        case pscript::DiagnosticSeverity::Info:
            return "3";
    }
    return "1";
}

const JsonValue* object_field(const JsonValue& object, const char* key) {
    const std::optional<const JsonValue*> value = object.get(key);
    return value.has_value() ? *value : nullptr;
}

int json_int(const JsonValue& value) {
    if (value.kind == JsonValue::Kind::Number) {
        return static_cast<int>(value.number_value);
    }
    return 0;
}

std::string json_string(const JsonValue& value) {
    if (value.kind == JsonValue::Kind::String) {
        return value.string_value;
    }
    return {};
}

int json_int_field(const JsonValue& object, const char* key, int default_value = 0) {
    const JsonValue* value = object_field(object, key);
    return value ? json_int(*value) : default_value;
}

std::string json_string_field(const JsonValue& object, const char* key, std::string default_value = {}) {
    const JsonValue* value = object_field(object, key);
    return value ? json_string(*value) : std::move(default_value);
}

pscript::AnalysisResult analyze_text(const std::string& path, const std::string& text) {
    pscript::AnalysisOptions options;
    options.file = path;
    options.inject_exception_prelude = false;
    options.run_linter = true;
    options.lenient_incomplete_source = true;
    return pscript::analyze_source(text, options);
}

pscript::AnalysisSession analysis_session_for_document_text(const std::string& path, const std::string& text) {
    return pscript::AnalysisSession(analyze_text(path, text));
}

constexpr int kDiagnosticDebounceMs = 400;

bool completion_log_enabled() {
    const char* value = std::getenv("PSCRIPT_LSP_LOG_COMPLETION");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void json_escape_to(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char ch : text) {
        switch (ch) {
            case '"':
            case '\\':
                out.push_back('\\');
                out.push_back(ch);
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    out.push_back('"');
}

const char* completion_lexical_name(pscript::CompletionLexicalContext lexical) {
    switch (lexical) {
        case pscript::CompletionLexicalContext::Normal:
            return "Normal";
        case pscript::CompletionLexicalContext::ImportModule:
            return "ImportModule";
        case pscript::CompletionLexicalContext::FromImportModule:
            return "FromImportModule";
        case pscript::CompletionLexicalContext::FromImportBinding:
            return "FromImportBinding";
        case pscript::CompletionLexicalContext::StringLiteral:
            return "StringLiteral";
        case pscript::CompletionLexicalContext::Comment:
            return "Comment";
    }
    return "Unknown";
}

void log_completion_debug(std::string_view uri,
                          int doc_version,
                          int lsp_line,
                          int lsp_character,
                          int analysis_line,
                          int analysis_column,
                          std::string_view doc_text,
                          std::string_view prefix,
                          const pscript::CompletionSiteContext& site,
                          const pscript::AnalysisSession& session,
                          const std::vector<pscript::LspCompletionItem>& completions,
                          const std::vector<JsonValue>& lsp_items) {
    std::string json;
    json.append("{\"event\":\"completion_debug\",");
    json.append("\"uri\":");
    json_escape_to(json, uri);
    json.append(",\"documentVersion\":");
    json.append(std::to_string(doc_version));
    json.append(",\"lspPosition\":{\"line\":");
    json.append(std::to_string(lsp_line));
    json.append(",\"character\":");
    json.append(std::to_string(lsp_character));
    json.append("},\"analysisPosition\":{\"line\":");
    json.append(std::to_string(analysis_line));
    json.append(",\"column\":");
    json.append(std::to_string(analysis_column));
    json.append("},\"prefix\":");
    json_escape_to(json, prefix);
    json.append(",\"site\":{\"lexical\":");
    json_escape_to(json, completion_lexical_name(site.lexical));
    json.append(",\"inLoop\":");
    json.append(site.in_loop ? "true" : "false");
    json.append(",\"inFunction\":");
    json.append(site.in_function ? "true" : "false");
    json.append(",\"fromImportModule\":");
    json_escape_to(json, site.from_import_module);
    json.append("},\"moduleBindings\":[");
    bool first_module = true;
    for (const pscript::SymbolRecord& symbol : session.symbols().all()) {
        if (symbol.kind != pscript::SymbolKind::Module) {
            continue;
        }
        if (!first_module) {
            json.push_back(',');
        }
        first_module = false;
        json.push_back('{');
        json.append("\"name\":");
        json_escape_to(json, symbol.name);
        json.append(",\"qualified\":");
        json_escape_to(json, symbol.qualified_name);
        json.push_back('}');
    }
    json.append("],\"queryItems\":[");
    for (std::size_t i = 0; i < completions.size(); ++i) {
        if (i > 0) {
            json.push_back(',');
        }
        const pscript::LspCompletionItem& item = completions[i];
        json.push_back('{');
        json.append("\"label\":");
        json_escape_to(json, item.label);
        json.append(",\"kind\":");
        json.append(std::to_string(static_cast<int>(item.kind)));
        json.push_back('}');
    }
    json.append("],\"lspItems\":[");
    for (std::size_t i = 0; i < lsp_items.size(); ++i) {
        if (i > 0) {
            json.push_back(',');
        }
        const JsonValue& value = lsp_items[i];
        const JsonValue* label = object_field(value, "label");
        const JsonValue* kind = object_field(value, "kind");
        json.push_back('{');
        json.append("\"label\":");
        if (label && label->kind == JsonValue::Kind::String) {
            json_escape_to(json, label->string_value);
        } else {
            json.append("\"\"");
        }
        json.append(",\"kind\":");
        if (kind && kind->kind == JsonValue::Kind::Number) {
            json.append(std::to_string(static_cast<int>(kind->number_value)));
        } else {
            json.append("-1");
        }
        json.push_back('}');
    }
    json.append("],\"docTextTail\":");
    const std::size_t tail_start = doc_text.size() > 120 ? doc_text.size() - 120 : 0;
    json_escape_to(json, doc_text.substr(tail_start));
    json.append("}\n");
    std::fwrite(json.data(), 1, json.size(), stderr);
}

}  // namespace

void LspServer::write_message_unsafe(const std::string& body) {
    if (!out_) {
        return;
    }
    write_message(*out_, body);
}

void LspServer::write_message_locked(const std::string& body) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    write_message_unsafe(body);
}

void LspServer::invalidate_analysis(Document& doc) {
    doc.cached_analysis.reset();
    doc.cached_analysis_version = -1;
}

pscript::AnalysisSession LspServer::analysis_session_for_uri(const std::string& uri) {
    std::string text;
    std::string path;
    int version = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const Document* doc = document_for_uri(uri);
        if (!doc || !is_pscript_document_uri(doc->uri, doc->path)) {
            return pscript::AnalysisSession(pscript::AnalysisResult{});
        }
        if (doc->cached_analysis.has_value() && doc->cached_analysis_version == doc->version) {
            return pscript::AnalysisSession(pscript::AnalysisResult(*doc->cached_analysis));
        }
        text = doc->text;
        path = doc->path;
        version = doc->version;
    }
    pscript::AnalysisResult analysis = analyze_text(path, text);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        Document* doc = document_for_uri(uri);
        if (doc && doc->version == version) {
            doc->cached_analysis = analysis;
            doc->cached_analysis_version = version;
            return pscript::AnalysisSession(pscript::AnalysisResult(*doc->cached_analysis));
        }
    }
    return pscript::AnalysisSession(std::move(analysis));
}

void LspServer::publish_diagnostics_for_document(const Document& doc, const pscript::AnalysisResult& analysis) {
    if (!is_pscript_document_uri(doc.uri, doc.path)) {
        return;
    }
    std::vector<JsonValue> diagnostics;
    for (const pscript::Diagnostic& diagnostic : analysis.diagnostics) {
        std::unordered_map<std::string, JsonValue> item;
        item.emplace("severity", JsonValue::make_number(std::stoi(diagnostic_severity_lsp(diagnostic.severity))));
        item.emplace("message", JsonValue::make_string(diagnostic.message));
        item.emplace("source", JsonValue::make_string("pscript"));
        item.emplace("range", make_range(source_range_to_lsp(diagnostic.range)));
        diagnostics.push_back(JsonValue::make_object(std::move(item)));
    }
    std::unordered_map<std::string, JsonValue> params;
    params.emplace("uri", JsonValue::make_string(doc.uri));
    params.emplace("version", JsonValue::make_number(doc.version));
    params.emplace("diagnostics", JsonValue::make_array(std::move(diagnostics)));
    std::unordered_map<std::string, JsonValue> notification;
    notification.emplace("jsonrpc", JsonValue::make_string("2.0"));
    notification.emplace("method", JsonValue::make_string("textDocument/publishDiagnostics"));
    notification.emplace("params", JsonValue::make_object(std::move(params)));
    write_message_locked(JsonValue::make_object(std::move(notification)).stringify());
}

void LspServer::run_diagnostics_now(const std::string& uri, std::uint64_t epoch) {
    try {
        Document snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            Document* doc = document_for_uri(uri);
            if (!doc || doc->diagnostic_epoch != epoch) {
                return;
            }
            if (!is_pscript_document_uri(doc->uri, doc->path)) {
                documents_.erase(uri);
                return;
            }
            snapshot = *doc;
        }
        pscript::AnalysisResult analysis = analyze_text(snapshot.path, snapshot.text);
        Document publish_doc;
        publish_doc.uri = snapshot.uri;
        publish_doc.version = snapshot.version;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            Document* doc = document_for_uri(uri);
            if (!doc || doc->diagnostic_epoch != epoch || doc->version != snapshot.version) {
                return;
            }
            doc->cached_analysis = analysis;
            doc->cached_analysis_version = doc->version;
            publish_doc.uri = doc->uri;
            publish_doc.version = doc->version;
            publish_doc.path = snapshot.path;
        }
        publish_diagnostics_for_document(publish_doc, analysis);
    } catch (const std::exception&) {
        // Debounced diagnostics must never terminate the language server process.
    } catch (...) {
    }
}

void LspServer::start_debounce_worker() {
    debounce_thread_ = std::thread([this]() { debounce_worker_main(); });
}

void LspServer::stop_debounce_worker() {
    {
        std::lock_guard<std::mutex> lock(debounce_mutex_);
        debounce_stop_ = true;
    }
    debounce_cv_.notify_all();
    if (debounce_thread_.joinable()) {
        debounce_thread_.join();
    }
}

void LspServer::debounce_worker_main() {
    while (true) {
        PendingDiagnostic job;
        {
            std::unique_lock<std::mutex> lock(debounce_mutex_);
            debounce_cv_.wait(lock, [this] { return pending_diagnostic_.has_value() || debounce_stop_; });
            if (debounce_stop_) {
                break;
            }
            job = *pending_diagnostic_;
            while (true) {
                debounce_cv_.wait_for(lock, std::chrono::milliseconds(kDiagnosticDebounceMs));
                if (debounce_stop_) {
                    return;
                }
                if (!pending_diagnostic_.has_value()) {
                    break;
                }
                if (pending_diagnostic_->epoch != job.epoch) {
                    job = *pending_diagnostic_;
                }
            }
            pending_diagnostic_.reset();
        }
        run_diagnostics_now(job.uri, job.epoch);
    }
}

void LspServer::schedule_debounced_diagnostics(const std::string& uri) {
    PendingDiagnostic job;
    job.uri = uri;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        Document* doc = document_for_uri(uri);
        if (!doc) {
            return;
        }
        if (!is_pscript_document_uri(doc->uri, doc->path)) {
            documents_.erase(uri);
            return;
        }
        doc->diagnostic_epoch++;
        job.epoch = doc->diagnostic_epoch;
    }
    {
        std::lock_guard<std::mutex> lock(debounce_mutex_);
        pending_diagnostic_ = job;
    }
    debounce_cv_.notify_one();
}

bool LspServer::read_message(std::istream& in, std::string& body) {
    std::string line;
    int content_length = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        const std::string_view header = line;
        const std::string_view prefix = "Content-Length: ";
        if (header.rfind(prefix, 0) == 0) {
            try {
                content_length = std::stoi(std::string(header.substr(prefix.size())));
            } catch (const std::exception&) {
                return false;
            }
        }
    }
    if (content_length < 0) {
        return false;
    }
    body.resize(static_cast<std::size_t>(content_length));
    in.read(body.data(), content_length);
    return static_cast<std::size_t>(content_length) == body.size();
}

void LspServer::write_message(std::ostream& out, const std::string& body) {
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

JsonValue LspServer::make_position(int line, int character) {
    std::unordered_map<std::string, JsonValue> fields;
    fields.emplace("line", JsonValue::make_number(line));
    fields.emplace("character", JsonValue::make_number(character));
    return JsonValue::make_object(std::move(fields));
}

JsonValue LspServer::make_range(const LspRange& range) {
    std::unordered_map<std::string, JsonValue> fields;
    fields.emplace("start", make_position(range.start.line, range.start.character));
    fields.emplace("end", make_position(range.end.line, range.end.character));
    return JsonValue::make_object(std::move(fields));
}

JsonValue LspServer::make_location(const LspLocation& location) {
    std::unordered_map<std::string, JsonValue> fields;
    fields.emplace("uri", JsonValue::make_string(location.uri));
    fields.emplace("range", make_range(location.range));
    return JsonValue::make_object(std::move(fields));
}

LspServer::Document* LspServer::document_for_uri(const std::string& uri) {
    const auto it = documents_.find(uri);
    return it == documents_.end() ? nullptr : &it->second;
}

void LspServer::handle_message(const JsonValue& message, std::ostream& out) {
    const JsonValue* id = object_field(message, "id");
    const JsonValue* method_value = object_field(message, "method");
    if (!method_value || method_value->kind != JsonValue::Kind::String) {
        return;
    }
    const std::string method = method_value->string_value;
    const JsonValue* params = object_field(message, "params");
    const JsonValue empty_params = JsonValue::make_object({});
    const JsonValue& params_ref = params ? *params : empty_params;

    if (method == "exit") {
        std::exit(shutdown_requested_ ? 0 : 1);
    }

    if (id) {
        JsonValue result = handle_request(method, params_ref, *id);
        std::unordered_map<std::string, JsonValue> response;
        response.emplace("jsonrpc", JsonValue::make_string("2.0"));
        response.emplace("id", *id);
        response.emplace("result", std::move(result));
        write_message_locked(JsonValue::make_object(std::move(response)).stringify());
        return;
    }

    if (method == "initialized") {
        return;
    }
    if (method == "textDocument/didOpen") {
        try {
            const JsonValue* text_document = object_field(params_ref, "textDocument");
            if (!text_document) {
                return;
            }
            Document doc;
            doc.uri = json_string_field(*text_document, "uri");
            doc.path = uri_to_path(doc.uri);
            doc.text = json_string_field(*text_document, "text");
            doc.version = json_int_field(*text_document, "version");
            if (!is_pscript_document_uri(doc.uri, doc.path)) {
                return;
            }
            pscript::AnalysisResult analysis = analyze_text(doc.path, doc.text);
            Document publish_doc;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                documents_[doc.uri] = std::move(doc);
                Document& opened = documents_[doc.uri];
                opened.cached_analysis = analysis;
                opened.cached_analysis_version = opened.version;
                publish_doc.uri = opened.uri;
                publish_doc.path = opened.path;
                publish_doc.version = opened.version;
            }
            publish_diagnostics_for_document(publish_doc, analysis);
        } catch (const std::exception&) {
        } catch (...) {
        }
        return;
    }
    if (method == "textDocument/didChange") {
        const JsonValue* text_document = object_field(params_ref, "textDocument");
        const JsonValue* content_changes = object_field(params_ref, "contentChanges");
        if (!text_document || !content_changes || content_changes->kind != JsonValue::Kind::Array ||
            content_changes->array_value.empty()) {
            return;
        }
        const std::string uri = json_string_field(*text_document, "uri");
        std::string schedule_uri;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            Document* doc = document_for_uri(uri);
            if (!doc) {
                return;
            }
            if (!is_pscript_document_uri(doc->uri, doc->path)) {
                documents_.erase(uri);
                return;
            }
            doc->version = json_int_field(*text_document, "version", doc->version);
            for (const JsonValue& change : content_changes->array_value) {
                doc->text = apply_text_document_content_change(doc->text, change);
            }
            invalidate_analysis(*doc);
            schedule_uri = uri;
        }
        schedule_debounced_diagnostics(schedule_uri);
        return;
    }
    if (method == "textDocument/didSave") {
        const JsonValue* text_document = object_field(params_ref, "textDocument");
        if (!text_document) {
            return;
        }
        try {
            const std::string uri = json_string_field(*text_document, "uri");
            std::string path;
            std::string text;
            Document publish_doc;
            pscript::AnalysisResult analysis;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                Document* doc = document_for_uri(uri);
                if (!doc) {
                    return;
                }
                if (!is_pscript_document_uri(doc->uri, doc->path)) {
                    documents_.erase(uri);
                    return;
                }
                doc->diagnostic_epoch++;
                invalidate_analysis(*doc);
                path = doc->path;
                text = doc->text;
                publish_doc.uri = doc->uri;
                publish_doc.path = doc->path;
                publish_doc.version = doc->version;
            }
            analysis = analyze_text(path, text);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                Document* doc = document_for_uri(uri);
                if (!doc) {
                    return;
                }
                doc->cached_analysis = analysis;
                doc->cached_analysis_version = doc->version;
                publish_doc.version = doc->version;
                publish_doc.path = doc->path;
            }
            publish_diagnostics_for_document(publish_doc, analysis);
        } catch (const std::exception&) {
        } catch (...) {
        }
        return;
    }
    if (method == "textDocument/didClose") {
        const JsonValue* text_document = object_field(params_ref, "textDocument");
        if (text_document) {
            const std::string uri = json_string_field(*text_document, "uri");
            if (!uri.empty()) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                documents_.erase(uri);
            }
        }
    }
}

JsonValue LspServer::handle_request(const std::string& method, const JsonValue& params, const JsonValue& id) {
    (void)id;
    try {
    if (method == "shutdown") {
        shutdown_requested_ = true;
        return JsonValue::make_null();
    }
    if (method == "initialize") {
        std::unordered_map<std::string, JsonValue> server_info;
        server_info.emplace("name", JsonValue::make_string("pscript-lsp"));
        server_info.emplace("version", JsonValue::make_string("0.1.1"));

        std::unordered_map<std::string, JsonValue> completion_provider;
        completion_provider.emplace("triggerCharacters", JsonValue::make_array({JsonValue::make_string(".")}));

        std::unordered_map<std::string, JsonValue> semantic_tokens_provider;
        std::vector<JsonValue> legend_types;
        for (const char* entry : kSemanticLegend) {
            legend_types.push_back(JsonValue::make_string(entry));
        }
        std::unordered_map<std::string, JsonValue> legend;
        legend.emplace("tokenTypes", JsonValue::make_array(std::move(legend_types)));
        legend.emplace("tokenModifiers", JsonValue::make_array({}));
        semantic_tokens_provider.emplace("legend", JsonValue::make_object(std::move(legend)));
        semantic_tokens_provider.emplace("full", JsonValue::make_bool(true));

        std::unordered_map<std::string, JsonValue> capabilities;
        capabilities.emplace("textDocumentSync", JsonValue::make_number(2));
        capabilities.emplace("completionProvider", JsonValue::make_object(std::move(completion_provider)));
        capabilities.emplace("hoverProvider", JsonValue::make_bool(true));
        capabilities.emplace("definitionProvider", JsonValue::make_bool(true));
        capabilities.emplace("referencesProvider", JsonValue::make_bool(true));
        capabilities.emplace("renameProvider", JsonValue::make_bool(true));
        capabilities.emplace("semanticTokensProvider", JsonValue::make_object(std::move(semantic_tokens_provider)));

        std::unordered_map<std::string, JsonValue> result;
        result.emplace("capabilities", JsonValue::make_object(std::move(capabilities)));
        result.emplace("serverInfo", JsonValue::make_object(std::move(server_info)));
        return JsonValue::make_object(std::move(result));
    }

    const JsonValue* text_document = object_field(params, "textDocument");
    const JsonValue* position = object_field(params, "position");
    std::string uri;
    if (text_document) {
        uri = json_string_field(*text_document, "uri");
    }
    std::string doc_text;
    std::string doc_path;
    int doc_version = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const Document* doc = document_for_uri(uri);
        if (!doc) {
            return JsonValue::make_null();
        }
        if (!is_pscript_document_uri(doc->uri, doc->path)) {
            return JsonValue::make_null();
        }
        doc_text = doc->text;
        doc_path = doc->path;
        doc_version = doc->version;
    }

    const bool needs_fresh_analysis = method == "textDocument/diagnostic" ||
                                      method == "textDocument/completion" ||
                                      method == "textDocument/semanticTokens/full" ||
                                      method == "textDocument/hover" ||
                                      method == "textDocument/definition" ||
                                      method == "textDocument/references" ||
                                      method == "textDocument/rename";
    pscript::AnalysisSession session =
        needs_fresh_analysis ? analysis_session_for_document_text(doc_path, doc_text)
                             : analysis_session_for_uri(uri);

    if (method == "textDocument/diagnostic") {
        if (!is_pscript_document_uri(uri, doc_path)) {
            std::unordered_map<std::string, JsonValue> result;
            result.emplace("kind", JsonValue::make_string("full"));
            result.emplace("items", JsonValue::make_array({}));
            return JsonValue::make_object(std::move(result));
        }
        std::vector<JsonValue> diagnostics;
        for (const pscript::Diagnostic& diagnostic : session.diagnostics()) {
            std::unordered_map<std::string, JsonValue> item;
            item.emplace("severity", JsonValue::make_number(std::stoi(diagnostic_severity_lsp(diagnostic.severity))));
            item.emplace("message", JsonValue::make_string(diagnostic.message));
            item.emplace("source", JsonValue::make_string("pscript"));
            const LspRange range = source_range_to_lsp(diagnostic.range);
            item.emplace("range", make_range(range));
            diagnostics.push_back(JsonValue::make_object(std::move(item)));
        }
        std::unordered_map<std::string, JsonValue> result;
        result.emplace("kind", JsonValue::make_string("full"));
        result.emplace("items", JsonValue::make_array(std::move(diagnostics)));
        return JsonValue::make_object(std::move(result));
    }

    int line = 0;
    int character = 0;
    if (position) {
        line = json_int_field(*position, "line") + 1;
        character = json_int_field(*position, "character") + 1;
    }

    if (method == "textDocument/completion") {
        const std::optional<pscript::MemberAccessCompletionContext> member =
            pscript::member_access_context_at(doc_text, line, character);
        const pscript::MemberAccessCompletionContext* member_ptr = member.has_value() ? &*member : nullptr;
        std::string prefix;
        if (member_ptr != nullptr) {
            prefix = pscript::member_access_name_prefix(doc_text, line, character);
        } else {
            const std::optional<pscript::WordPrefixAtPosition> word =
                pscript::word_prefix_at_position(doc_text, line, character);
            prefix = word.has_value() ? word->prefix : std::string{};
        }
        const pscript::CompletionSiteContext site =
            pscript::completion_site_at(doc_text, line, character);
        const std::vector<pscript::LspCompletionItem> query_items =
            session.completion_items(prefix, member_ptr, site);
        std::vector<JsonValue> items;
        for (const pscript::LspCompletionItem& completion : query_items) {
            std::unordered_map<std::string, JsonValue> item;
            item.emplace("label", JsonValue::make_string(completion.label));
            item.emplace("kind", JsonValue::make_number(static_cast<int>(completion.kind)));
            item.emplace("filterText", JsonValue::make_string(completion.label));
            if (completion.detail.has_value()) {
                item.emplace("detail", JsonValue::make_string(*completion.detail));
            }
            // Omit textEdit: pinning replace range can drop all items after Backspace when the
            // client keeps a stale range. filterText keeps client-side prefix filtering stable.
            if (completion.insert_text.has_value()) {
                item.emplace("insertText", JsonValue::make_string(*completion.insert_text));
            }
            items.push_back(JsonValue::make_object(std::move(item)));
        }
        if (completion_log_enabled()) {
            log_completion_debug(uri,
                                 doc_version,
                                 line - 1,
                                 character - 1,
                                 line,
                                 character,
                                 doc_text,
                                 prefix,
                                 site,
                                 session,
                                 query_items,
                                 items);
        }
        std::unordered_map<std::string, JsonValue> result;
        // Prefix-filtered for this request; client may filter locally on further typing.
        result.emplace("isIncomplete", JsonValue::make_bool(false));
        result.emplace("items", JsonValue::make_array(std::move(items)));
        return JsonValue::make_object(std::move(result));
    }

    if (method == "textDocument/hover") {
        const auto hover = session.hover_text(doc_text, doc_path, line, character);
        if (!hover.has_value()) {
            return JsonValue::make_null();
        }
        std::unordered_map<std::string, JsonValue> contents;
        contents.emplace("kind", JsonValue::make_string("markdown"));
        contents.emplace("value", JsonValue::make_string("```pscript\n" + *hover + "\n```"));
        std::unordered_map<std::string, JsonValue> result;
        result.emplace("contents", JsonValue::make_object(std::move(contents)));
        return JsonValue::make_object(std::move(result));
    }

    if (method == "textDocument/definition") {
        const pscript::SymbolRecord* symbol = session.goto_definition(doc_path, line, character);
        if (!symbol) {
            const std::optional<std::string> id = identifier_at_position(doc_text, line, character);
            if (id.has_value()) {
                const std::vector<const pscript::SymbolRecord*> matches = session.symbols().find_by_name(*id);
                if (!matches.empty()) {
                    symbol = matches.front();
                }
            }
        }
        if (!symbol) {
            return JsonValue::make_null();
        }
        LspLocation location;
        location.uri = uri;
        location.range = source_range_to_lsp(symbol->range);
        return make_location(location);
    }

    if (method == "textDocument/references") {
        std::vector<JsonValue> locations;
        for (const LspLocation& ref :
             session.references_in_source(doc_text, doc_path, uri, line, character)) {
            locations.push_back(make_location(ref));
        }
        return JsonValue::make_array(std::move(locations));
    }

    if (method == "textDocument/rename") {
        const JsonValue* new_name_value = object_field(params, "newName");
        if (!new_name_value) {
            return JsonValue::make_null();
        }
        const std::string new_name = json_string(*new_name_value);
        const std::vector<LspTextEdit> edits = session.rename_in_source(doc_text, line, character, new_name);
        if (edits.empty()) {
            return JsonValue::make_null();
        }
        std::vector<JsonValue> text_edits;
        for (const LspTextEdit& edit : edits) {
            std::unordered_map<std::string, JsonValue> item;
            item.emplace("range", make_range(edit.range));
            item.emplace("newText", JsonValue::make_string(edit.new_text));
            text_edits.push_back(JsonValue::make_object(std::move(item)));
        }
        std::unordered_map<std::string, JsonValue> changes;
        changes.emplace(uri, JsonValue::make_array(std::move(text_edits)));
        std::unordered_map<std::string, JsonValue> result;
        result.emplace("changes", JsonValue::make_object(std::move(changes)));
        return JsonValue::make_object(std::move(result));
    }

    if (method == "textDocument/semanticTokens/full") {
        const std::vector<LspSemanticToken> tokens = session.semantic_tokens(doc_text);
        std::vector<JsonValue> data;
        int prev_line = 0;
        int prev_char = 0;
        for (const LspSemanticToken& token : tokens) {
            const int delta_line = token.line - prev_line;
            int delta_start = token.start_char;
            if (delta_line == 0) {
                delta_start -= prev_char;
            }
            data.push_back(JsonValue::make_number(delta_line));
            data.push_back(JsonValue::make_number(delta_start));
            data.push_back(JsonValue::make_number(token.length));
            data.push_back(JsonValue::make_number(static_cast<int>(token.type)));
            data.push_back(JsonValue::make_number(token.modifiers));
            prev_line = token.line;
            prev_char = token.start_char;
        }
        std::unordered_map<std::string, JsonValue> result;
        result.emplace("data", JsonValue::make_array(std::move(data)));
        return JsonValue::make_object(std::move(result));
    }

    return JsonValue::make_null();
    } catch (const std::exception&) {
        return JsonValue::make_null();
    } catch (...) {
        return JsonValue::make_null();
    }
}

int LspServer::run(std::istream& in, std::ostream& out) {
    out_ = &out;
    start_debounce_worker();
    while (true) {
        std::string body;
        if (!read_message(in, body)) {
            break;
        }
        const std::optional<JsonValue> message = parse_json(body);
        if (!message.has_value()) {
            continue;
        }
        handle_message(*message, out);
    }
    stop_debounce_worker();
    return 0;
}

}  // namespace pscript::lsp
