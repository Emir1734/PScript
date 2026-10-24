#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

[[noreturn]] void csv_fail(std::string message) {
    native_value_error(std::move(message));
}

struct CsvOptions {
    char delimiter = ',';
    char quote = '"';
    bool header = false;
};

CsvOptions parse_options(const Value& value) {
    CsvOptions opts;
    if (value.kind == ValueKind::Null) {
        return opts;
    }
    if (value.kind != ValueKind::Dict || !value.dict) {
        csv_fail("options must be a dict");
    }
    for (const auto& entry : value.dict->entries) {
        if (entry.first == "delimiter" && entry.second.kind == ValueKind::String && entry.second.string.size() == 1) {
            opts.delimiter = entry.second.string[0];
        } else if (entry.first == "quote" && entry.second.kind == ValueKind::String && entry.second.string.size() == 1) {
            opts.quote = entry.second.string[0];
        } else if (entry.first == "header" && entry.second.kind == ValueKind::Bool) {
            opts.header = entry.second.boolean;
        }
    }
    return opts;
}

std::vector<std::string> parse_row(std::string_view line, const CsvOptions& opts) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_quotes) {
            if (ch == opts.quote) {
                if (i + 1 < line.size() && line[i + 1] == opts.quote) {
                    current.push_back(opts.quote);
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == opts.quote) {
            in_quotes = true;
            continue;
        }
        if (ch == opts.delimiter) {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (in_quotes) {
        csv_fail("unterminated quoted field");
    }
    fields.push_back(current);
    return fields;
}

std::string escape_field(std::string_view field, const CsvOptions& opts) {
    bool needs_quotes = false;
    for (char ch : field) {
        if (ch == opts.delimiter || ch == opts.quote || ch == '\n' || ch == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::string(field);
    }
    std::string out;
    out.push_back(opts.quote);
    for (char ch : field) {
        if (ch == opts.quote) {
            out.push_back(opts.quote);
        }
        out.push_back(ch);
    }
    out.push_back(opts.quote);
    return out;
}

std::vector<std::vector<std::string>> parse_csv_text(std::string_view text, const CsvOptions& opts) {
    std::vector<std::vector<std::string>> rows;
    std::string line;
    for (char ch : text) {
        if (ch == '\n') {
            if (!line.empty() || !rows.empty()) {
                rows.push_back(parse_row(line, opts));
            }
            line.clear();
            continue;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
    }
    if (!line.empty()) {
        rows.push_back(parse_row(line, opts));
    }
    return rows;
}

}  // namespace

Value native_csv_parse(const std::vector<Value>& args) {
    native_require_min_argc("parse", args, 1);
    const std::string& text = native_require_string(args[0], "parse", "text");
    const CsvOptions opts = args.size() > 1 ? parse_options(args[1]) : CsvOptions{};
    const auto rows = parse_csv_text(text, opts);
    std::vector<Value> out_rows;
    out_rows.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<Value> items;
        items.reserve(row.size());
        for (const std::string& field : row) {
            items.push_back(Runtime::make_string(field));
        }
        out_rows.push_back(Runtime::make_list(std::move(items)));
    }
    return Runtime::make_list(std::move(out_rows));
}

Value native_csv_stringify(const std::vector<Value>& args) {
    native_require_min_argc("stringify", args, 1);
    if (args[0].kind != ValueKind::List || !args[0].list) {
        native_type_error("'stringify' expects a list");
    }
    const CsvOptions opts = args.size() > 1 ? parse_options(args[1]) : CsvOptions{};
    std::string out;
    bool first_row = true;
    for (const Value& row_value : args[0].list->items) {
        if (row_value.kind != ValueKind::List || !row_value.list) {
            native_type_error("'stringify' expects list of list rows");
        }
        if (!first_row) {
            out.push_back('\n');
        }
        first_row = false;
        for (std::size_t i = 0; i < row_value.list->items.size(); ++i) {
            if (i > 0) {
                out.push_back(opts.delimiter);
            }
            const Value& cell = row_value.list->items[i];
            const std::string& text = cell.kind == ValueKind::String ? cell.string : Runtime::format(cell);
            out += escape_field(text, opts);
        }
    }
    return Runtime::make_string(out);
}

Value native_csv_reader(const std::vector<Value>& args) {
    native_require_min_argc("reader", args, 1);
    const std::string& text = native_require_string(args[0], "reader", "text");
    const CsvOptions opts = args.size() > 1 ? parse_options(args[1]) : CsvOptions{};
    const auto rows = parse_csv_text(text, opts);
    std::vector<Value> row_values;
    row_values.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<Value> items;
        for (const std::string& field : row) {
            items.push_back(Runtime::make_string(field));
        }
        row_values.push_back(Runtime::make_list(std::move(items)));
    }
    return Runtime::make_dict({{"rows", Runtime::make_list(std::move(row_values))},
                               {"index", Runtime::make_integer(0)},
                               {"header", Runtime::make_bool(opts.header)},
                               {"delimiter", Runtime::make_string(std::string(1, opts.delimiter))}});
}

Value native_csv_writer(const std::vector<Value>& args) {
    if (args.size() == 2 && args[0].kind == ValueKind::Dict && args[0].dict) {
        const CsvOptions opts = parse_options(args[0]);
        if (args[1].kind != ValueKind::List || !args[1].list) {
            native_type_error("'writer' row must be a list");
        }
        std::string line;
        for (std::size_t i = 0; i < args[1].list->items.size(); ++i) {
            if (i > 0) {
                line.push_back(opts.delimiter);
            }
            const Value& cell = args[1].list->items[i];
            const std::string& text = cell.kind == ValueKind::String ? cell.string : Runtime::format(cell);
            line += escape_field(text, opts);
        }
        std::vector<Value> lines;
        for (const auto& entry : args[0].dict->entries) {
            if (entry.first == "lines" && entry.second.kind == ValueKind::List && entry.second.list) {
                lines = entry.second.list->items;
                break;
            }
        }
        lines.push_back(Runtime::make_string(line));
        return Runtime::make_dict({{"lines", Runtime::make_list(std::move(lines))},
                                   {"delimiter", Runtime::make_string(std::string(1, opts.delimiter))},
                                   {"quote", Runtime::make_string(std::string(1, opts.quote))}});
    }
    const CsvOptions opts = args.empty() ? CsvOptions{} : parse_options(args[0]);
    return Runtime::make_dict({{"lines", Runtime::make_list({})},
                               {"delimiter", Runtime::make_string(std::string(1, opts.delimiter))},
                               {"quote", Runtime::make_string(std::string(1, opts.quote))}});
}

}  // namespace pscript
