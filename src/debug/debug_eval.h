#pragma once

#include "debug/debug_vm_inspector.h"
#include "runtime/runtime.h"

#include <optional>
#include <string>

namespace pscript::debug {

struct EvalError {
    std::string message;
};

class ExpressionEvaluator {
public:
    explicit ExpressionEvaluator(const VmInspector& inspector);

    std::optional<Value> evaluate(const std::string& expression, EvalError& error) const;

private:
    const VmInspector& inspector_;

    std::optional<Value> lookup_name(const std::string& name) const;
    std::optional<Value> evaluate_path(const std::string& expression, EvalError& error) const;
};

}  // namespace pscript::debug
