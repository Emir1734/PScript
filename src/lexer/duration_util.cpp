#include "lexer/duration_util.h"

#include <cmath>
#include <cstdlib>

namespace pscript {
namespace {

enum class Unit { Ms, S, M, H };

bool parse_unit_suffix(std::string_view text, Unit& unit, std::size_t& suffix_len) {
    if (text.size() >= 2 && text.substr(text.size() - 2) == "ms") {
        unit = Unit::Ms;
        suffix_len = 2;
        return true;
    }
    if (text.empty()) {
        return false;
    }
    const char c = text.back();
    if (c == 's') {
        unit = Unit::S;
        suffix_len = 1;
        return true;
    }
    if (c == 'm') {
        unit = Unit::M;
        suffix_len = 1;
        return true;
    }
    if (c == 'h') {
        unit = Unit::H;
        suffix_len = 1;
        return true;
    }
    return false;
}

}  // namespace

std::optional<std::int64_t> duration_lexeme_to_milliseconds(std::string_view lexeme, DurationParseError* error) {
    Unit unit = Unit::Ms;
    std::size_t suffix_len = 0;
    if (!parse_unit_suffix(lexeme, unit, suffix_len)) {
        if (error) {
            error->message = "duration literal requires a unit suffix (ms, s, m, or h)";
        }
        return std::nullopt;
    }
    const std::string_view numeric = lexeme.substr(0, lexeme.size() - suffix_len);
    if (numeric.empty()) {
        if (error) {
            error->message = "invalid duration literal";
        }
        return std::nullopt;
    }

    const std::string numeric_text(numeric);
    char* end = nullptr;
    const double value = std::strtod(numeric_text.c_str(), &end);
    if (end != numeric_text.c_str() + static_cast<std::ptrdiff_t>(numeric_text.size()) || value < 0.0) {
        if (error) {
            error->message = "invalid duration literal";
        }
        return std::nullopt;
    }

    double ms = 0.0;
    switch (unit) {
        case Unit::Ms:
            ms = value;
            break;
        case Unit::S:
            ms = value * 1000.0;
            break;
        case Unit::M:
            ms = value * 60.0 * 1000.0;
            break;
        case Unit::H:
            ms = value * 60.0 * 60.0 * 1000.0;
            break;
    }

    if (!std::isfinite(ms)) {
        if (error) {
            error->message = "duration literal out of range";
        }
        return std::nullopt;
    }

    const double rounded = ms >= 0.0 ? std::floor(ms + 0.5) : std::ceil(ms - 0.5);
    return static_cast<std::int64_t>(rounded);
}

}  // namespace pscript
