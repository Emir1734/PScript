#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

namespace pscript {
namespace {

[[noreturn]] void testing_fail(std::string message) {
    throw RuntimeError("Testing assertion failed: " + std::move(message), RuntimeErrorCategory::Value);
}

}  // namespace

Value native_testing_equal(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("equal", args, 2);
    if (!Runtime::equals(args[0], args[1])) {
        testing_fail("expected " + Runtime::format(args[1]) + ", got " + Runtime::format(args[0]));
    }
    return Runtime::make_null();
}

Value native_testing_not_equal(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("not_equal", args, 2);
    if (Runtime::equals(args[0], args[1])) {
        testing_fail("expected values to differ, got " + Runtime::format(args[0]));
    }
    return Runtime::make_null();
}

Value native_testing_true(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("true", args, 1);
    if (args[0].kind != ValueKind::Bool || !args[0].boolean) {
        testing_fail("expected true, got " + Runtime::format(args[0]));
    }
    return Runtime::make_null();
}

Value native_testing_false(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("false", args, 1);
    if (args[0].kind != ValueKind::Bool || args[0].boolean) {
        testing_fail("expected false, got " + Runtime::format(args[0]));
    }
    return Runtime::make_null();
}

Value native_testing_null(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("null", args, 1);
    if (args[0].kind != ValueKind::Null) {
        testing_fail("expected null, got " + Runtime::format(args[0]));
    }
    return Runtime::make_null();
}

Value native_testing_not_null(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("not_null", args, 1);
    if (args[0].kind == ValueKind::Null) {
        testing_fail("expected non-null value");
    }
    return Runtime::make_null();
}

Value native_testing_throws(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("throws", args, 1);
    if (args[0].kind != ValueKind::Function || !args[0].function) {
        native_type_error("throws expects a function");
    }
    try {
        runtime.invoke_callable(args[0], {});
    } catch (const RuntimeError&) {
        return Runtime::make_null();
    } catch (...) {
        return Runtime::make_null();
    }
    testing_fail("expected function to throw");
}

Value native_testing_fail(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("fail", args, 1);
    const std::string& message = native_require_string_arg(args, 0, "fail", "message");
    testing_fail(message);
}

Value native_testing_run(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("run", args, 0);
    runtime.run_tests(std::nullopt);
    return Runtime::make_null();
}

}  // namespace pscript
