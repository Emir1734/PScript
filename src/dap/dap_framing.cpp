#include "dap/dap_framing.h"

#include <cstdlib>
#include <iostream>
#include <istream>
#include <ostream>
#include <string_view>

namespace pscript::dap {

bool read_message(std::istream& in, std::string& body) {
    body.clear();
    std::string line;
    std::size_t content_length = 0;
    bool have_length = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            if (!have_length) {
                return false;
            }
            body.resize(content_length);
            in.read(body.data(), static_cast<std::streamsize>(content_length));
            return static_cast<std::size_t>(in.gcount()) == content_length;
        }
        const std::string_view prefix = "Content-Length: ";
        if (line.rfind(prefix, 0) == 0) {
            content_length = static_cast<std::size_t>(std::strtoul(line.substr(prefix.size()).c_str(), nullptr, 10));
            have_length = true;
        }
    }
    return false;
}

void write_message(std::ostream& out, const std::string& body) {
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

}  // namespace pscript::dap
