#include "compiler/compiler_error.h"

namespace pscript {

CompilerError::CompilerError(std::string message, int line, int column)
    : std::runtime_error(message),
      message_(std::move(message)),
      line_(line),
      column_(column) {}

}  // namespace pscript
