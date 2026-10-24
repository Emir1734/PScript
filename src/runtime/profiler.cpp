#include "runtime/profiler.h"

#include "compiler/bytecode.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace pscript {
namespace {

thread_local RuntimeProfiler* g_thread_profiler = nullptr;

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::optional<std::uint64_t> parse_json_uint64(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    std::uint64_t value = 0;
    bool any = false;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        any = true;
        value = value * 10 + static_cast<std::uint64_t>(text[pos] - '0');
        ++pos;
    }
    if (!any) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> parse_json_int(std::string_view text, std::size_t& pos) {
    const auto parsed = parse_json_uint64(text, pos);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return static_cast<int>(*parsed);
}

std::optional<std::string> parse_json_string(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    if (pos >= text.size() || text[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && pos < text.size()) {
            const char esc = text[pos++];
            switch (esc) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(esc);
                    break;
            }
            continue;
        }
        out.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::size_t> find_key(std::string_view text, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t at = text.find(needle);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t colon = text.find(':', at + needle.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    return colon + 1;
}

}  // namespace

const char* profile_hotness_name(ProfileHotness hotness) {
    switch (hotness) {
        case ProfileHotness::Hot:
            return "hot";
        case ProfileHotness::Warm:
            return "warm";
        case ProfileHotness::Cold:
            return "cold";
    }
    return "cold";
}

ProfileHotness classify_hotness(double time_share) {
    if (time_share >= 0.30) {
        return ProfileHotness::Hot;
    }
    if (time_share >= 0.05) {
        return ProfileHotness::Warm;
    }
    return ProfileHotness::Cold;
}

RuntimeProfiler* thread_local_profiler() {
    return g_thread_profiler;
}

void set_thread_local_profiler(RuntimeProfiler* profiler) {
    g_thread_profiler = profiler;
}

ProfilerThreadScope::ProfilerThreadScope(RuntimeProfiler* profiler) : previous_(g_thread_profiler) {
    g_thread_profiler = profiler;
}

ProfilerThreadScope::~ProfilerThreadScope() {
    g_thread_profiler = previous_;
}

void RuntimeProfiler::set_enabled(bool enabled) {
    enabled_ = enabled;
}

void RuntimeProfiler::begin_session() {
    function_stats_.clear();
    opcode_counts_.fill(0);
    instruction_executions_ = 0;
    loop_iterations_.clear();
    call_site_counts_.clear();
    memory_ = {};
    active_frames_.clear();
    vm_wall_ns_ = 0;
    session_start_ = std::chrono::steady_clock::now();
}

void RuntimeProfiler::end_session() {
    const auto end = std::chrono::steady_clock::now();
    vm_wall_ns_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - session_start_).count());
}

void RuntimeProfiler::ensure_function_slot(std::uint32_t function_index) {
    if (function_index >= function_stats_.size()) {
        function_stats_.resize(static_cast<std::size_t>(function_index) + 1);
    }
}

std::uint64_t RuntimeProfiler::loop_key(std::uint32_t function_index, std::uint32_t ip) {
    return (static_cast<std::uint64_t>(function_index) << 32) | static_cast<std::uint64_t>(ip);
}

std::uint64_t RuntimeProfiler::call_site_key(std::uint32_t function_index, std::uint32_t ip) {
    return (static_cast<std::uint64_t>(function_index) << 32) | static_cast<std::uint64_t>(ip);
}

void RuntimeProfiler::on_instruction(Opcode opcode, std::uint32_t /*ip*/, std::uint32_t /*function_index*/) {
    if (!enabled_) {
        return;
    }
    ++instruction_executions_;
    const std::size_t slot = static_cast<std::size_t>(opcode);
    if (slot < opcode_counts_.size()) {
        ++opcode_counts_[slot];
    }
}

void RuntimeProfiler::on_function_enter(std::uint32_t function_index) {
    if (!enabled_) {
        return;
    }
    ensure_function_slot(function_index);
    ++function_stats_[function_index].calls;
    ActiveFrame frame;
    frame.function_index = function_index;
    frame.start = std::chrono::steady_clock::now();
    active_frames_.push_back(frame);
}

void RuntimeProfiler::on_function_leave(std::uint32_t function_index) {
    if (!enabled_ || active_frames_.empty()) {
        return;
    }
    ActiveFrame frame = active_frames_.back();
    active_frames_.pop_back();
    if (frame.function_index != function_index) {
        return;
    }
    const auto end = std::chrono::steady_clock::now();
    const std::uint64_t elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - frame.start).count());
    ensure_function_slot(function_index);
    FunctionStats& stats = function_stats_[function_index];
    stats.inclusive_ns += elapsed;
    stats.exclusive_ns += elapsed - frame.child_inclusive_ns;
    if (!active_frames_.empty()) {
        active_frames_.back().child_inclusive_ns += elapsed;
    }
}

void RuntimeProfiler::on_call_site(std::uint32_t caller_function_index, std::uint32_t ip) {
    if (!enabled_) {
        return;
    }
    ++call_site_counts_[call_site_key(caller_function_index, ip)];
}

void RuntimeProfiler::on_loop_back(std::uint32_t function_index, std::uint32_t target_ip) {
    if (!enabled_) {
        return;
    }
    ++loop_iterations_[loop_key(function_index, target_ip)];
}

void RuntimeProfiler::on_allocation(std::size_t bytes) {
    if (!enabled_) {
        return;
    }
    ++memory_.allocation_count;
    memory_.allocation_bytes += static_cast<std::uint64_t>(bytes);
}

ProfileDocument RuntimeProfiler::finalize(const Bytecode& bytecode) const {
    ProfileDocument document;
    document.version = ProfileDocument::kFormatVersion;
    document.instruction_executions = instruction_executions_;
    document.vm_wall_ns = vm_wall_ns_;
    document.memory = memory_;

    std::uint64_t total_inclusive = 0;
    for (const FunctionStats& stats : function_stats_) {
        total_inclusive += stats.inclusive_ns;
    }
    if (total_inclusive == 0) {
        total_inclusive = vm_wall_ns_;
    }

    for (std::size_t i = 0; i < function_stats_.size(); ++i) {
        const FunctionStats& stats = function_stats_[i];
        if (stats.calls == 0 && stats.inclusive_ns == 0 && stats.exclusive_ns == 0) {
            continue;
        }
        ProfileFunctionRecord record;
        record.function_index = static_cast<std::uint32_t>(i);
        if (i < bytecode.functions.size()) {
            record.name = bytecode.functions[i].name;
        } else {
            record.name = "<fn" + std::to_string(i) + ">";
        }
        record.calls = stats.calls;
        record.inclusive_ns = stats.inclusive_ns;
        record.exclusive_ns = stats.exclusive_ns;
        const double share = total_inclusive > 0 ? static_cast<double>(stats.inclusive_ns) / static_cast<double>(total_inclusive)
                                                 : 0.0;
        record.hotness = classify_hotness(share);
        document.functions.push_back(std::move(record));
    }

    std::sort(document.functions.begin(), document.functions.end(),
              [](const ProfileFunctionRecord& left, const ProfileFunctionRecord& right) {
                  return left.inclusive_ns > right.inclusive_ns;
              });

    for (std::size_t slot = 0; slot < opcode_counts_.size(); ++slot) {
        if (opcode_counts_[slot] == 0) {
            continue;
        }
        ProfileOpcodeRecord record;
        record.name = opcode_name(static_cast<Opcode>(slot));
        record.count = opcode_counts_[slot];
        document.opcodes.push_back(std::move(record));
    }

    for (const auto& entry : loop_iterations_) {
        ProfileLoopRecord record;
        record.function_index = static_cast<std::uint32_t>(entry.first >> 32);
        record.ip = static_cast<std::uint32_t>(entry.first & 0xFFFFFFFFu);
        record.iterations = entry.second;
        document.loops.push_back(record);
    }

    for (const auto& entry : call_site_counts_) {
        ProfileCallSiteRecord record;
        record.caller_function_index = static_cast<std::uint32_t>(entry.first >> 32);
        record.ip = static_cast<std::uint32_t>(entry.first & 0xFFFFFFFFu);
        record.calls = entry.second;
        document.call_sites.push_back(record);
    }

    return document;
}

bool save_profile_document(const ProfileDocument& document, const std::string& path, std::string& error) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"format\": \"pscript-profile\",\n";
    json << "  \"version\": " << document.version << ",\n";
    json << "  \"pgo_compat_version\": " << document.pgo_compat_version << ",\n";
    json << "  \"program_fingerprint\": \"" << json_escape(document.program_fingerprint) << "\",\n";
    json << "  \"instruction_executions\": " << document.instruction_executions << ",\n";
    json << "  \"vm_wall_ns\": " << document.vm_wall_ns << ",\n";
    json << "  \"memory\": {\"allocation_count\": " << document.memory.allocation_count << ", \"allocation_bytes\": "
         << document.memory.allocation_bytes << "},\n";

    json << "  \"functions\": [\n";
    for (std::size_t i = 0; i < document.functions.size(); ++i) {
        const ProfileFunctionRecord& fn = document.functions[i];
        json << "    {\"function_index\": " << fn.function_index << ", \"name\": \"" << json_escape(fn.name)
             << "\", \"calls\": " << fn.calls << ", \"inclusive_ns\": " << fn.inclusive_ns << ", \"exclusive_ns\": "
             << fn.exclusive_ns << ", \"hotness\": \"" << profile_hotness_name(fn.hotness) << "\"}";
        json << (i + 1 < document.functions.size() ? ",\n" : "\n");
    }
    json << "  ],\n";

    json << "  \"opcodes\": [\n";
    for (std::size_t i = 0; i < document.opcodes.size(); ++i) {
        const ProfileOpcodeRecord& op = document.opcodes[i];
        json << "    {\"name\": \"" << json_escape(op.name) << "\", \"count\": " << op.count << "}";
        json << (i + 1 < document.opcodes.size() ? ",\n" : "\n");
    }
    json << "  ],\n";

    json << "  \"loops\": [\n";
    for (std::size_t i = 0; i < document.loops.size(); ++i) {
        const ProfileLoopRecord& loop = document.loops[i];
        json << "    {\"function_index\": " << loop.function_index << ", \"ip\": " << loop.ip << ", \"iterations\": "
             << loop.iterations << "}";
        json << (i + 1 < document.loops.size() ? ",\n" : "\n");
    }
    json << "  ],\n";

    json << "  \"call_sites\": [\n";
    for (std::size_t i = 0; i < document.call_sites.size(); ++i) {
        const ProfileCallSiteRecord& site = document.call_sites[i];
        json << "    {\"caller_function_index\": " << site.caller_function_index << ", \"ip\": " << site.ip
             << ", \"calls\": " << site.calls << "}";
        json << (i + 1 < document.call_sites.size() ? ",\n" : "\n");
    }
    json << "  ]\n";
    json << "}\n";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write profile file '" + path + "'";
        return false;
    }
    out << json.str();
    if (!out) {
        error = "cannot write profile file '" + path + "'";
        return false;
    }
    return true;
}

std::optional<ProfileDocument> load_profile_document(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open profile file '" + path + "'";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!starts_with(text, "{")) {
        error = "invalid profile file (expected JSON object)";
        return std::nullopt;
    }
    if (text.find("\"format\"") == std::string::npos || text.find("pscript-profile") == std::string::npos) {
        error = "invalid profile format";
        return std::nullopt;
    }

    ProfileDocument document;
    if (const std::optional<std::size_t> version_pos = find_key(text, "version")) {
        std::size_t pos = *version_pos;
        const std::optional<int> version = parse_json_int(text, pos);
        if (!version.has_value() || *version != ProfileDocument::kFormatVersion) {
            error = "unsupported profile version";
            return std::nullopt;
        }
        document.version = *version;
    } else {
        error = "missing profile version";
        return std::nullopt;
    }

    if (const std::optional<std::size_t> pgo_pos = find_key(text, "pgo_compat_version")) {
        std::size_t pos = *pgo_pos;
        if (const std::optional<int> pgo_version = parse_json_int(text, pos)) {
            document.pgo_compat_version = *pgo_version;
        }
    }
    if (const std::optional<std::size_t> fp_pos = find_key(text, "program_fingerprint")) {
        std::size_t pos = *fp_pos;
        if (const std::optional<std::string> fingerprint = parse_json_string(text, pos)) {
            document.program_fingerprint = *fingerprint;
        }
    }

    if (const std::optional<std::size_t> insn_pos = find_key(text, "instruction_executions")) {
        std::size_t pos = *insn_pos;
        if (const std::optional<std::uint64_t> value = parse_json_uint64(text, pos)) {
            document.instruction_executions = *value;
        }
    }
    if (const std::optional<std::size_t> wall_pos = find_key(text, "vm_wall_ns")) {
        std::size_t pos = *wall_pos;
        if (const std::optional<std::uint64_t> value = parse_json_uint64(text, pos)) {
            document.vm_wall_ns = *value;
        }
    }

    if (const std::optional<std::size_t> alloc_count_pos = find_key(text, "allocation_count")) {
        std::size_t pos = *alloc_count_pos;
        if (const std::optional<std::uint64_t> value = parse_json_uint64(text, pos)) {
            document.memory.allocation_count = *value;
        }
    }
    if (const std::optional<std::size_t> alloc_bytes_pos = find_key(text, "allocation_bytes")) {
        std::size_t pos = *alloc_bytes_pos;
        if (const std::optional<std::uint64_t> value = parse_json_uint64(text, pos)) {
            document.memory.allocation_bytes = *value;
        }
    }

    const std::size_t functions_key = text.find("\"functions\"");
    if (functions_key != std::string::npos) {
        std::size_t pos = text.find('{', functions_key);
        while (pos != std::string::npos && pos < text.size()) {
            if (text[pos] == ']') {
                break;
            }
            if (text[pos] != '{') {
                ++pos;
                continue;
            }
            ProfileFunctionRecord record;
            const std::optional<std::size_t> name_key = find_key(text.substr(pos), "name");
            if (name_key.has_value()) {
                std::size_t name_pos = pos + *name_key;
                if (const std::optional<std::string> name = parse_json_string(text, name_pos)) {
                    record.name = *name;
                }
            }
            if (const std::optional<std::size_t> calls_key = find_key(text.substr(pos), "calls")) {
                std::size_t calls_pos = pos + *calls_key;
                if (const std::optional<std::uint64_t> calls = parse_json_uint64(text, calls_pos)) {
                    record.calls = *calls;
                }
            }
            if (const std::optional<std::size_t> inclusive_key = find_key(text.substr(pos), "inclusive_ns")) {
                std::size_t inclusive_pos = pos + *inclusive_key;
                if (const std::optional<std::uint64_t> inclusive = parse_json_uint64(text, inclusive_pos)) {
                    record.inclusive_ns = *inclusive;
                }
            }
            if (const std::optional<std::size_t> exclusive_key = find_key(text.substr(pos), "exclusive_ns")) {
                std::size_t exclusive_pos = pos + *exclusive_key;
                if (const std::optional<std::uint64_t> exclusive = parse_json_uint64(text, exclusive_pos)) {
                    record.exclusive_ns = *exclusive;
                }
            }
            if (const std::optional<std::size_t> hotness_key = find_key(text.substr(pos), "hotness")) {
                std::size_t hotness_pos = pos + *hotness_key;
                if (const std::optional<std::string> hotness = parse_json_string(text, hotness_pos)) {
                    if (*hotness == "hot") {
                        record.hotness = ProfileHotness::Hot;
                    } else if (*hotness == "warm") {
                        record.hotness = ProfileHotness::Warm;
                    } else {
                        record.hotness = ProfileHotness::Cold;
                    }
                }
            }
            if (!record.name.empty() || record.calls > 0) {
                document.functions.push_back(record);
            }
            pos = text.find('}', pos + 1);
            if (pos == std::string::npos) {
                break;
            }
            ++pos;
        }
    }

    return document;
}

void write_profile_report(const ProfileDocument& document, std::ostream& out) {
    out << "PScript runtime profile (v" << document.version << ")\n";
    out << "Instructions: " << document.instruction_executions << "\n";
    out << "VM wall time: " << (document.vm_wall_ns / 1000000) << "ms\n";
    out << "Allocations: " << document.memory.allocation_count << " (" << document.memory.allocation_bytes
        << " bytes)\n\n";

    out << "Function          Calls       Time       Hotness\n";
    out << "------------------------------------------------\n";
    for (const ProfileFunctionRecord& fn : document.functions) {
        const std::uint64_t time_ms = fn.inclusive_ns / 1000000;
        out << fn.name;
        if (fn.name.size() < 16) {
            out << std::string(16 - fn.name.size(), ' ');
        } else {
            out << ' ';
        }
        out << fn.calls;
        if (fn.calls < 10000000) {
            out << std::string(12 - std::to_string(fn.calls).size(), ' ');
        } else {
            out << ' ';
        }
        out << time_ms << "ms";
        if (time_ms < 10000000) {
            out << std::string(11 - std::to_string(time_ms).size(), ' ');
        } else {
            out << ' ';
        }
        out << profile_hotness_name(fn.hotness) << '\n';
    }

    if (!document.opcodes.empty()) {
        out << "\nTop opcodes:\n";
        std::vector<ProfileOpcodeRecord> sorted = document.opcodes;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ProfileOpcodeRecord& left, const ProfileOpcodeRecord& right) { return left.count > right.count; });
        const std::size_t limit = std::min<std::size_t>(sorted.size(), 10);
        for (std::size_t i = 0; i < limit; ++i) {
            out << "  " << sorted[i].name << ": " << sorted[i].count << '\n';
        }
    }
}

}  // namespace pscript
