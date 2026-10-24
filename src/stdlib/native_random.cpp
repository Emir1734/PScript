#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace pscript {
namespace {

std::mt19937_64& random_engine() {
    static std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

double numeric_to_double(const Value& value, std::string_view function) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    native_type_error(std::string("'") + std::string(function) + "' expects a numeric argument");
}

std::int64_t require_integer(const Value& value, std::string_view function, std::string_view what) {
    if (value.kind != ValueKind::Integer) {
        native_type_error(std::string("'") + std::string(function) + "' expects an integer " + std::string(what));
    }
    return value.integer;
}

const std::vector<Value>* sequence_items(const Value& value, std::string_view function) {
    if (value.kind == ValueKind::List && value.list) {
        return &value.list->items;
    }
    if (value.kind == ValueKind::Tuple && value.tuple) {
        return &value.tuple->items;
    }
    native_type_error(std::string("'") + std::string(function) + "' expects a list or tuple");
}

}  // namespace

Value native_random_int(const std::vector<Value>& args) {
    native_require_argc("int", args, 2);
    const std::int64_t min_value = require_integer(args[0], "int", "minimum");
    const std::int64_t max_value = require_integer(args[1], "int", "maximum");
    if (min_value > max_value) {
        native_value_error("'int' expects minimum <= maximum");
    }
    std::uniform_int_distribution<std::int64_t> dist(min_value, max_value);
    return Runtime::make_integer(dist(random_engine()));
}

Value native_random_float(const std::vector<Value>& args) {
    if (args.empty()) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return Runtime::make_float(dist(random_engine()));
    }
    if (args.size() == 2) {
        const double min_value = numeric_to_double(args[0], "float");
        const double max_value = numeric_to_double(args[1], "float");
        if (min_value >= max_value) {
            native_value_error("'float' expects minimum < maximum");
        }
        std::uniform_real_distribution<double> dist(min_value, max_value);
        return Runtime::make_float(dist(random_engine()));
    }
    native_value_error("'float' expects 0 or 2 arguments");
}

Value native_random_choice(const std::vector<Value>& args) {
    native_require_argc("choice", args, 1);
    const std::vector<Value>* items = sequence_items(args[0], "choice");
    if (items->empty()) {
        native_value_error("'choice' expects a non-empty sequence");
    }
    std::uniform_int_distribution<std::size_t> dist(0, items->size() - 1);
    return items->at(dist(random_engine()));
}

Value native_random_shuffle(const std::vector<Value>& args) {
    native_require_argc("shuffle", args, 1);
    if (args[0].kind != ValueKind::List || !args[0].list) {
        native_type_error("'shuffle' expects a list");
    }
    std::vector<Value>& items = args[0].list->items;
    if (items.size() > 1) {
        std::shuffle(items.begin(), items.end(), random_engine());
    }
    return Runtime::make_null();
}

Value native_random_seed(const std::vector<Value>& args) {
    native_require_argc("seed", args, 1);
    std::uint64_t seed = 0;
    if (args[0].kind == ValueKind::Integer) {
        seed = static_cast<std::uint64_t>(args[0].integer);
    } else if (args[0].kind == ValueKind::Float) {
        seed = static_cast<std::uint64_t>(args[0].floating);
    } else if (args[0].kind == ValueKind::String) {
        seed = std::hash<std::string>{}(args[0].string);
    } else {
        native_type_error("'seed' expects an integer, float, or string");
    }
    random_engine().seed(seed);
    return Runtime::make_null();
}

}  // namespace pscript
