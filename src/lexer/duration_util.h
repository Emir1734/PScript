#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pscript {

struct DurationParseError {
    std::string message;
};

/** Parse a duration lexeme such as `1.5s` or `500ms` into whole milliseconds (rounded half away from zero). */
std::optional<std::int64_t> duration_lexeme_to_milliseconds(std::string_view lexeme,
                                                          DurationParseError* error = nullptr);

}  // namespace pscript
