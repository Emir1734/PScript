#include "registry/registry_json.h"

#include "package/json_util.h"

#include <sstream>

namespace pscript::registry {

std::string json_escape(std::string_view text) {
    return package::json_escape(text);
}

std::string json_string(std::string_view text) {
    return package::json_string(text);
}

std::string json_string_array(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << json_string(values[i]);
    }
    out << ']';
    return out.str();
}

std::string json_search_results(const std::vector<std::string>& package_ids) {
    std::ostringstream out;
    out << "{\"packages\":[";
    for (std::size_t i = 0; i < package_ids.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << json_string(package_ids[i]);
    }
    out << "]}";
    return out.str();
}

}  // namespace pscript::registry
