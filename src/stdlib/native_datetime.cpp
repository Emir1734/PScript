#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"
#include "stdlib/stdlib.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pscript {
namespace {

constexpr std::uint32_t kDateTimeClassIndex = 0;
constexpr std::uint32_t kDurationClassIndex = 1;
constexpr std::int64_t kMicrosPerSecond = 1'000'000;

const std::vector<std::string> kDateTimeFieldNames = {"year", "month", "day", "hour", "minute", "second"};
const std::vector<std::string> kDurationFieldNames = {"seconds"};

struct BrokenDownTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[month - 1];
}

void validate_datetime_fields(int year, int month, int day, int hour, int minute, int second) {
    if (month < 1 || month > 12) {
        native_value_error("invalid datetime month");
    }
    if (day < 1 || day > days_in_month(year, month)) {
        native_value_error("invalid datetime day");
    }
    if (hour < 0 || hour > 23) {
        native_value_error("invalid datetime hour");
    }
    if (minute < 0 || minute > 59) {
        native_value_error("invalid datetime minute");
    }
    if (second < 0 || second > 59) {
        native_value_error("invalid datetime second");
    }
}

std::tm to_std_tm(const BrokenDownTime& parts) {
    std::tm tm{};
    tm.tm_year = parts.year - 1900;
    tm.tm_mon = parts.month - 1;
    tm.tm_mday = parts.day;
    tm.tm_hour = parts.hour;
    tm.tm_min = parts.minute;
    tm.tm_sec = parts.second;
    tm.tm_isdst = -1;
    return tm;
}

BrokenDownTime from_std_tm(const std::tm& tm) {
    BrokenDownTime parts;
    parts.year = tm.tm_year + 1900;
    parts.month = tm.tm_mon + 1;
    parts.day = tm.tm_mday;
    parts.hour = tm.tm_hour;
    parts.minute = tm.tm_min;
    parts.second = tm.tm_sec;
    return parts;
}

std::int64_t utc_micros_from_local_parts(const BrokenDownTime& parts) {
    validate_datetime_fields(parts.year, parts.month, parts.day, parts.hour, parts.minute, parts.second);
    std::tm tm = to_std_tm(parts);
    const std::time_t seconds = std::mktime(&tm);
    if (seconds == static_cast<std::time_t>(-1)) {
        native_value_error("invalid datetime value");
    }
    return static_cast<std::int64_t>(seconds) * kMicrosPerSecond;
}

BrokenDownTime breakdown_from_utc_micros(std::int64_t utc_micros, bool use_utc_fields) {
    const std::time_t seconds = static_cast<std::time_t>(utc_micros / kMicrosPerSecond);
    std::tm tm{};
#ifdef _WIN32
    if (use_utc_fields) {
        if (gmtime_s(&tm, &seconds) != 0) {
            native_value_error("invalid datetime value");
        }
    } else if (localtime_s(&tm, &seconds) != 0) {
        native_value_error("invalid datetime value");
    }
#else
    if (use_utc_fields) {
        if (gmtime_r(&seconds, &tm) == nullptr) {
            native_value_error("invalid datetime value");
        }
    } else if (localtime_r(&seconds, &tm) == nullptr) {
        native_value_error("invalid datetime value");
    }
#endif
    return from_std_tm(tm);
}

void populate_datetime_fields(Value& value, std::int64_t utc_micros, bool use_utc_fields) {
    const BrokenDownTime parts = breakdown_from_utc_micros(utc_micros, use_utc_fields);
    value.class_value->fields[0] = Runtime::make_integer(parts.year);
    value.class_value->fields[1] = Runtime::make_integer(parts.month);
    value.class_value->fields[2] = Runtime::make_integer(parts.day);
    value.class_value->fields[3] = Runtime::make_integer(parts.hour);
    value.class_value->fields[4] = Runtime::make_integer(parts.minute);
    value.class_value->fields[5] = Runtime::make_integer(parts.second);
}

Value make_native_datetime(std::int64_t utc_micros, bool use_utc_fields) {
    Value value = Runtime::make_class(kDateTimeClassIndex, "DateTime", kDateTimeFieldNames);
    auto data = std::make_shared<NativeDateTimeData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::DateTime);
    data->utc_micros = utc_micros;
    data->use_utc_fields = use_utc_fields;
    value.class_value->native_datetime = std::move(data);
    populate_datetime_fields(value, utc_micros, use_utc_fields);
    return value;
}

Value make_native_duration(std::int64_t micros) {
    Value value = Runtime::make_class(kDurationClassIndex, "Duration", kDurationFieldNames);
    auto data = std::make_shared<NativeDurationData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::Duration);
    data->micros = micros;
    value.class_value->native_duration = std::move(data);
    value.class_value->fields[0] = Runtime::make_float(static_cast<double>(micros) / static_cast<double>(kMicrosPerSecond));
    return value;
}

std::int64_t current_utc_micros() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    return duration_cast<microseconds>(now.time_since_epoch()).count();
}

double numeric_to_seconds(const Value& value, std::string_view function) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    native_type_error(std::string("'") + std::string(function) + "' expects a numeric argument");
}

std::int64_t require_integer_arg(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    if (index >= args.size() || args[index].kind != ValueKind::Integer) {
        native_type_error(std::string("'") + std::string(function) + "' expects integer arguments");
    }
    return args[index].integer;
}

std::string default_format_pattern() {
    return "%Y-%m-%d %H:%M:%S";
}

std::string format_datetime(const Value& object, std::string_view pattern) {
    if (!is_native_datetime(object) || !object.class_value || !object.class_value->native_datetime) {
        native_type_error("'format' expects a DateTime receiver");
    }
    const BrokenDownTime parts =
        breakdown_from_utc_micros(object.class_value->native_datetime->utc_micros,
                                    object.class_value->native_datetime->use_utc_fields);
    std::string out;
    out.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '%' && i + 1 < pattern.size()) {
            switch (pattern[i + 1]) {
                case 'Y':
                    out += std::to_string(parts.year);
                    break;
                case 'm': {
                    std::ostringstream stream;
                    stream << std::setw(2) << std::setfill('0') << parts.month;
                    out += stream.str();
                    break;
                }
                case 'd': {
                    std::ostringstream stream;
                    stream << std::setw(2) << std::setfill('0') << parts.day;
                    out += stream.str();
                    break;
                }
                case 'H': {
                    std::ostringstream stream;
                    stream << std::setw(2) << std::setfill('0') << parts.hour;
                    out += stream.str();
                    break;
                }
                case 'M': {
                    std::ostringstream stream;
                    stream << std::setw(2) << std::setfill('0') << parts.minute;
                    out += stream.str();
                    break;
                }
                case 'S': {
                    std::ostringstream stream;
                    stream << std::setw(2) << std::setfill('0') << parts.second;
                    out += stream.str();
                    break;
                }
                default:
                    native_value_error("invalid datetime format pattern");
            }
            ++i;
            continue;
        }
        out.push_back(pattern[i]);
    }
    return out;
}

bool parse_two_digits(std::string_view text, int& out) {
    if (text.size() != 2 || text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') {
        return false;
    }
    out = (text[0] - '0') * 10 + (text[1] - '0');
    return true;
}

bool parse_four_digits(std::string_view text, int& out) {
    if (text.size() != 4) {
        return false;
    }
    out = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        out = out * 10 + (ch - '0');
    }
    return true;
}

BrokenDownTime parse_datetime_text(const std::string& text) {
    // Deterministic format: YYYY-MM-DD HH:MM:SS
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != ' ' || text[13] != ':' ||
        text[16] != ':') {
        native_value_error("invalid datetime string");
    }
    BrokenDownTime parts;
    if (!parse_four_digits(text.substr(0, 4), parts.year) || !parse_two_digits(text.substr(5, 2), parts.month) ||
        !parse_two_digits(text.substr(8, 2), parts.day) || !parse_two_digits(text.substr(11, 2), parts.hour) ||
        !parse_two_digits(text.substr(14, 2), parts.minute) || !parse_two_digits(text.substr(17, 2), parts.second)) {
        native_value_error("invalid datetime string");
    }
    validate_datetime_fields(parts.year, parts.month, parts.day, parts.hour, parts.minute, parts.second);
    return parts;
}

Value datetime_now_impl(bool midnight_local) {
    std::int64_t utc_micros = current_utc_micros();
    if (midnight_local) {
        BrokenDownTime parts = breakdown_from_utc_micros(utc_micros, false);
        parts.hour = 0;
        parts.minute = 0;
        parts.second = 0;
        utc_micros = utc_micros_from_local_parts(parts);
    }
    return make_native_datetime(utc_micros, false);
}

}  // namespace

bool is_native_datetime(const Value& value) {
    return value.kind == ValueKind::Class && value.class_value && value.class_value->native_datetime;
}

bool is_native_duration(const Value& value) {
    return value.kind == ValueKind::Class && value.class_value && value.class_value->native_duration;
}

Value datetime_add(const Value& datetime, const Value& duration) {
    if (!is_native_datetime(datetime) || !is_native_duration(duration)) {
        native_type_error("unsupported operand type");
    }
    const std::int64_t micros = datetime.class_value->native_datetime->utc_micros + duration.class_value->native_duration->micros;
    return make_native_datetime(micros, datetime.class_value->native_datetime->use_utc_fields);
}

Value datetime_subtract_duration(const Value& datetime, const Value& duration) {
    if (!is_native_datetime(datetime) || !is_native_duration(duration)) {
        native_type_error("unsupported operand type");
    }
    const std::int64_t micros = datetime.class_value->native_datetime->utc_micros - duration.class_value->native_duration->micros;
    return make_native_datetime(micros, datetime.class_value->native_datetime->use_utc_fields);
}

Value datetime_subtract_datetime(const Value& left, const Value& right) {
    if (!is_native_datetime(left) || !is_native_datetime(right)) {
        native_type_error("unsupported operand type");
    }
    const std::int64_t micros =
        left.class_value->native_datetime->utc_micros - right.class_value->native_datetime->utc_micros;
    return make_native_duration(micros);
}

Value duration_negate(const Value& duration) {
    if (!is_native_duration(duration)) {
        native_type_error("unsupported operand type");
    }
    return make_native_duration(-duration.class_value->native_duration->micros);
}

int compare_datetime(const Value& left, const Value& right) {
    const auto left_micros = left.class_value->native_datetime->utc_micros;
    const auto right_micros = right.class_value->native_datetime->utc_micros;
    if (left_micros < right_micros) {
        return -1;
    }
    if (left_micros > right_micros) {
        return 1;
    }
    return 0;
}

int compare_duration(const Value& left, const Value& right) {
    const auto left_micros = left.class_value->native_duration->micros;
    const auto right_micros = right.class_value->native_duration->micros;
    if (left_micros < right_micros) {
        return -1;
    }
    if (left_micros > right_micros) {
        return 1;
    }
    return 0;
}

bool datetime_values_equal(const Value& left, const Value& right) {
    return left.class_value->native_datetime->utc_micros == right.class_value->native_datetime->utc_micros;
}

bool duration_values_equal(const Value& left, const Value& right) {
    return left.class_value->native_duration->micros == right.class_value->native_duration->micros;
}

Value native_datetime_now(const std::vector<Value>& args) {
    native_require_argc("now", args, 0);
    return datetime_now_impl(false);
}

Value native_datetime_today(const std::vector<Value>& args) {
    native_require_argc("today", args, 0);
    return datetime_now_impl(true);
}

Value native_datetime_utc_now(const std::vector<Value>& args) {
    native_require_argc("utc_now", args, 0);
    return make_native_datetime(current_utc_micros(), true);
}

Value native_datetime_from_unix(const std::vector<Value>& args) {
    native_require_argc("from_unix", args, 1);
    const double seconds = numeric_to_seconds(args[0], "from_unix");
    const double micros = seconds * static_cast<double>(kMicrosPerSecond);
    if (!std::isfinite(micros)) {
        native_value_error("invalid unix timestamp");
    }
    return make_native_datetime(static_cast<std::int64_t>(std::llround(micros)), true);
}

Value native_datetime_create(const std::vector<Value>& args) {
    native_require_argc("create", args, 6);
    BrokenDownTime parts;
    parts.year = static_cast<int>(require_integer_arg(args, 0, "create"));
    parts.month = static_cast<int>(require_integer_arg(args, 1, "create"));
    parts.day = static_cast<int>(require_integer_arg(args, 2, "create"));
    parts.hour = static_cast<int>(require_integer_arg(args, 3, "create"));
    parts.minute = static_cast<int>(require_integer_arg(args, 4, "create"));
    parts.second = static_cast<int>(require_integer_arg(args, 5, "create"));
    return make_native_datetime(utc_micros_from_local_parts(parts), false);
}

Value native_datetime_parse(const std::vector<Value>& args) {
    native_require_argc("parse", args, 1);
    const std::string& text = native_require_string(args[0], "parse", "value");
    const BrokenDownTime parts = parse_datetime_text(text);
    return make_native_datetime(utc_micros_from_local_parts(parts), false);
}

Value native_datetime_format_method(const Value& object, const std::vector<Value>& args) {
    std::string pattern = default_format_pattern();
    if (args.size() > 1) {
        native_value_error("'format' expects at most 1 argument");
    }
    if (args.size() == 1) {
        pattern = native_require_string(args[0], "format", "pattern");
    }
    return Runtime::make_string(format_datetime(object, pattern));
}

Value native_time_now(const std::vector<Value>& args) {
    native_require_argc("now", args, 0);
    return datetime_now_impl(false);
}

Value native_time_unix(const std::vector<Value>& args) {
    native_require_argc("unix", args, 0);
    const double seconds =
        static_cast<double>(current_utc_micros()) / static_cast<double>(kMicrosPerSecond);
    return Runtime::make_float(seconds);
}

Value native_time_monotonic(const std::vector<Value>& args) {
    native_require_argc("monotonic", args, 0);
    using namespace std::chrono;
    const double seconds = duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
    return Runtime::make_float(seconds);
}

Value native_time_sleep(const std::vector<Value>& args) {
    native_require_argc("sleep", args, 1);
    const double seconds = numeric_to_seconds(args[0], "sleep");
    if (seconds < 0.0) {
        native_value_error("'sleep' expects a non-negative duration");
    }
    if (seconds == 0.0) {
        return Runtime::make_null();
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return Runtime::make_null();
}

}  // namespace pscript
