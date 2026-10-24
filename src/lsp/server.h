#pragma once

#include "analysis/lsp_support.h"
#include "analysis/query_api.h"
#include "lsp/json_util.h"

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace pscript::lsp {

class LspServer {
public:
    int run(std::istream& in, std::ostream& out);

private:
    struct Document {
        std::string uri;
        std::string path;
        std::string text;
        int version = 0;
        std::optional<pscript::AnalysisResult> cached_analysis;
        int cached_analysis_version = -1;
        std::uint64_t diagnostic_epoch = 0;
    };

    std::unordered_map<std::string, Document> documents_;
    bool shutdown_requested_ = false;
    std::ostream* out_ = nullptr;
    std::mutex state_mutex_;

    struct PendingDiagnostic {
        std::string uri;
        std::uint64_t epoch = 0;
    };
    std::mutex debounce_mutex_;
    std::condition_variable debounce_cv_;
    std::optional<PendingDiagnostic> pending_diagnostic_;
    std::thread debounce_thread_;
    bool debounce_stop_ = false;

    void start_debounce_worker();
    void stop_debounce_worker();
    void debounce_worker_main();

    void write_message_unsafe(const std::string& body);

    bool read_message(std::istream& in, std::string& body);
    void write_message(std::ostream& out, const std::string& body);
    void write_message_locked(const std::string& body);

    void handle_message(const JsonValue& message, std::ostream& out);
    JsonValue handle_request(const std::string& method, const JsonValue& params, const JsonValue& id);

    pscript::AnalysisSession analysis_session_for_uri(const std::string& uri);
    void invalidate_analysis(Document& doc);
    void schedule_debounced_diagnostics(const std::string& uri);
    void publish_diagnostics_for_document(const Document& doc, const pscript::AnalysisResult& analysis);
    void run_diagnostics_now(const std::string& uri, std::uint64_t epoch);

    Document* document_for_uri(const std::string& uri);

    static JsonValue make_position(int line, int character);
    static JsonValue make_range(const LspRange& range);
    static JsonValue make_location(const LspLocation& location);
};

}  // namespace pscript::lsp
