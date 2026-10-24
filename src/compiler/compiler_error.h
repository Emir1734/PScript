#pragma once

#include <stdexcept>
#include <string>

namespace pscript {

class CompilerError : public std::runtime_error {
public:
    CompilerError(std::string message, int line, int column);

    int line() const { return line_; }
    int column() const { return column_; }
    const std::string& message() const { return message_; }

private:
    std::string message_;
    int line_;
    int column_;
};

}  // namespace pscript
