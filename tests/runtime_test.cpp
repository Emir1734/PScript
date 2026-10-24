#include "runtime/alloc_guard.h"
#include "runtime/iterator.h"
#include "runtime/runtime.h"
#include "runtime/async_io.h"
#include "runtime/crypto_ops.h"
#include "runtime/crypto_kdf_cipher.h"
#include "runtime/sql_connection.h"

#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

using pscript::Runtime;
using pscript::RuntimeError;
using pscript::TaskData;
using pscript::TaskState;
using pscript::Value;
using pscript::ValueKind;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

}  // namespace

int main() {
    {
        std::ostringstream out;
        Runtime runtime(out);
        if (!runtime.has_builtin("print")) {
            fail_test("runtime creation", "print built-in missing");
        } else {
            pass_test("runtime creation");
        }
    }

    {
        Value list = Runtime::make_list({Runtime::make_integer(1)});
        const Value copied = list;
        if (!list.list || !copied.list || list.list.get() != copied.list.get()) {
            fail_test("value list copy shares storage", "expected shared list data");
        } else {
            pass_test("value list copy shares storage");
        }
    }

    {
        const Value integer = Runtime::make_integer(10);
        const Value floating = Runtime::make_float(1.5);
        const Value str = Runtime::make_string("hi");
        const Value boolean = Runtime::make_bool(true);
        const Value null = Runtime::make_null();
        if (integer.kind != ValueKind::Integer || integer.integer != 10 ||
            floating.kind != ValueKind::Float || Runtime::format(floating) != "1.5" ||
            Runtime::format(str) != "hi" || Runtime::format(boolean) != "true" ||
            Runtime::format(null) != "null") {
            fail_test("value helpers", "unexpected Value formatting");
        } else {
            pass_test("value helpers");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        bool called = false;
        runtime.register_builtin("probe", [&](Runtime&, const std::vector<Value>& args) {
            called = args.size() == 1 && args[0].kind == ValueKind::Integer && args[0].integer == 7;
            return Runtime::make_bool(called);
        });
        const Value result = runtime.call("probe", {Runtime::make_integer(7)});
        if (!runtime.has_builtin("probe") || !called || result.kind != ValueKind::Bool || !result.boolean) {
            fail_test("builtin registration", "probe was not called correctly");
        } else {
            pass_test("builtin registration");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        runtime.print({Runtime::make_string("hello")});
        runtime.print({Runtime::make_integer(10)});
        runtime.print({Runtime::make_float(1.5)});
        runtime.print({Runtime::make_bool(false)});
        runtime.print({Runtime::make_null()});
        if (out.str() != "hello\n10\n1.5\nfalse\nnull\n") {
            fail_test("print values", "got \"" + out.str() + "\"");
        } else {
            pass_test("print values");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        const std::string turkish = "\xC3\xA7\xC4\x9F\xC4\xB1";
        const std::string emoji = "\xF0\x9F\x98\x80";
        runtime.print({Runtime::make_string(turkish)});
        runtime.print({Runtime::make_string(emoji)});
        if (out.str() != turkish + "\n" + emoji + "\n" ||
            Runtime::string_length(Runtime::make_string(turkish)).integer != 6 ||
            Runtime::string_length(Runtime::make_string(emoji)).integer != 4) {
            fail_test("utf-8 print bytes", "utf-8 bytes were not preserved or length changed");
        } else {
            pass_test("utf-8 print bytes");
        }
    }

    {
        const Value text = Runtime::make_string("ab");
        const Value zero = Runtime::make_integer(0);
        const Value one = Runtime::make_integer(1);
        if (Runtime::get_index(text, zero).integer != 97 ||
            Runtime::get_index(text, one).integer != 98) {
            fail_test("string byte index", "unexpected code units");
        } else {
            pass_test("string byte index");
        }
    }

    {
        if (!Runtime::is_truthy(Runtime::make_bool(true)) || Runtime::is_truthy(Runtime::make_bool(false)) ||
            !Runtime::is_truthy(Runtime::make_integer(10)) || Runtime::is_truthy(Runtime::make_integer(0)) ||
            !Runtime::is_truthy(Runtime::make_float(1.5)) || Runtime::is_truthy(Runtime::make_float(0.0)) ||
            !Runtime::is_truthy(Runtime::make_string("hello")) || Runtime::is_truthy(Runtime::make_string("")) ||
            Runtime::is_truthy(Runtime::make_null()) || Runtime::is_truthy(Runtime::make_list({})) ||
            !Runtime::is_truthy(Runtime::make_list({Runtime::make_integer(1)})) ||
            Runtime::is_truthy(Runtime::make_dict({})) ||
            !Runtime::is_truthy(Runtime::make_dict({{"a", Runtime::make_integer(1)}}))) {
            fail_test("is_truthy", "unexpected truthiness");
        } else {
            pass_test("is_truthy");
        }
    }

    {
        const Value list = Runtime::make_list({Runtime::make_integer(10), Runtime::make_string("hi")});
        const Value dict = Runtime::make_dict({{"name", Runtime::make_string("Emir")}});
        if (Runtime::format(list) != "[10, hi]" || Runtime::format(dict) != "{\"name\": Emir}" ||
            Runtime::format(Runtime::make_list({})) != "[]" || Runtime::format(Runtime::make_dict({})) != "{}") {
            fail_test("collection format", Runtime::format(list) + " " + Runtime::format(dict));
        } else {
            pass_test("collection format");
        }
    }

    {
        Value list = Runtime::make_list({Runtime::make_integer(10), Runtime::make_integer(20)});
        Runtime::set_index(list, Runtime::make_integer(1), Runtime::make_integer(50));
        const Value got = Runtime::get_index(list, Runtime::make_integer(1));
        if (got.kind != ValueKind::Integer || got.integer != 50) {
            fail_test("list get/set", "expected 50");
        } else {
            pass_test("list get/set");
        }
    }

    {
        Value dict = Runtime::make_dict({{"score", Runtime::make_integer(100)}});
        Runtime::set_index(dict, Runtime::make_string("score"), Runtime::make_integer(150));
        Runtime::set_index(dict, Runtime::make_string("level"), Runtime::make_integer(5));
        const Value score = Runtime::get_index(dict, Runtime::make_string("score"));
        const Value level = Runtime::get_index(dict, Runtime::make_string("level"));
        if (score.integer != 150 || level.integer != 5) {
            fail_test("dict get/set", "update or insert failed");
        } else {
            pass_test("dict get/set");
        }
    }

    {
        Value list = Runtime::make_list({Runtime::make_integer(30), Runtime::make_integer(10)});
        Runtime::list_append(list, Runtime::make_integer(20));
        Runtime::list_sort(list);
        if (Runtime::format(list) != "[10, 20, 30]" || Runtime::list_length(list).integer != 3 ||
            !Runtime::list_contains(list, Runtime::make_integer(20)).boolean) {
            fail_test("list api helpers", Runtime::format(list));
        } else {
            pass_test("list api helpers");
        }
    }

    {
        const Value text = Runtime::make_string("Hello");
        if (Runtime::string_length(text).integer != 5 ||
            Runtime::format(Runtime::string_replace(text, Runtime::make_string("H"), Runtime::make_string("Y"))) !=
                "Yello") {
            fail_test("string api helpers", "unexpected string helpers");
        } else {
            pass_test("string api helpers");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("nope", {});
            fail_test("unknown builtin", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message().rfind("unknown built-in", 0) != 0) {
                fail_test("unknown builtin", err.message());
            } else {
                pass_test("unknown builtin");
            }
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        if (!runtime.has_builtin("range")) {
            fail_test("range builtin exists", "range built-in missing");
        } else {
            pass_test("range builtin exists");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        const Value a = runtime.call("range", {Runtime::make_integer(10)});
        const Value b = runtime.call("range", {Runtime::make_integer(2), Runtime::make_integer(10)});
        const Value c = runtime.call("range", {Runtime::make_integer(2), Runtime::make_integer(10), Runtime::make_integer(2)});
        const Value d = runtime.call("range", {Runtime::make_integer(10), Runtime::make_integer(0), Runtime::make_integer(-2)});
        const Value empty = runtime.call("range", {Runtime::make_integer(5), Runtime::make_integer(1)});
        if (a.kind != ValueKind::Range || Runtime::format(a) != "range(0, 10, 1)" ||
            Runtime::iter_len(a).integer != 10 || Runtime::iter_get(a, Runtime::make_integer(0)).integer != 0 ||
            Runtime::iter_get(a, Runtime::make_integer(9)).integer != 9 ||
            Runtime::iter_len(b).integer != 8 || Runtime::iter_get(b, Runtime::make_integer(0)).integer != 2 ||
            Runtime::iter_len(c).integer != 4 || Runtime::format(c) != "range(2, 10, 2)" ||
            Runtime::iter_get(c, Runtime::make_integer(3)).integer != 8 ||
            Runtime::iter_len(d).integer != 5 || Runtime::iter_get(d, Runtime::make_integer(0)).integer != 10 ||
            Runtime::iter_get(d, Runtime::make_integer(4)).integer != 2 ||
            Runtime::iter_len(empty).integer != 0 || Runtime::is_truthy(empty) || !Runtime::is_truthy(a)) {
            fail_test("range values", "unexpected range semantics");
        } else {
            pass_test("range values");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("range", {Runtime::make_integer(1), Runtime::make_integer(2), Runtime::make_integer(0)});
            fail_test("range zero step", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "range step cannot be zero") {
                fail_test("range zero step", err.message());
            } else {
                pass_test("range zero step");
            }
        }
    }

    {
        const Value list = Runtime::make_list({Runtime::make_integer(7)});
        if (Runtime::iter_len(list).integer != 1 || Runtime::iter_get(list, Runtime::make_integer(0)).integer != 7) {
            fail_test("list iter", "expected list iteration helpers");
        } else {
            pass_test("list iter");
        }
    }

    {
        std::istringstream in("Emir\n");
        std::ostringstream out;
        Runtime runtime(out, in);
        const Value name = runtime.call("input", {});
        if (name.kind != ValueKind::String || name.string != "Emir") {
            fail_test("input no prompt runtime", "unexpected input result");
        } else {
            pass_test("input no prompt runtime");
        }
    }

    {
        std::istringstream in("Ada\n");
        std::ostringstream out;
        Runtime runtime(out, in);
        const Value name = runtime.call("input", {Runtime::make_string("Name: ")});
        if (name.string != "Ada" || out.str() != "Name: ") {
            fail_test("input prompt runtime", "prompt/value mismatch");
        } else {
            pass_test("input prompt runtime");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        const Value i = runtime.call("int", {Runtime::make_string("10")});
        const Value f = runtime.call("float", {Runtime::make_string("1.5")});
        const Value s = runtime.call("str", {Runtime::make_integer(10)});
        const Value b = runtime.call("bool", {Runtime::make_string("true")});
        if (i.integer != 10 || f.floating != 1.5 || s.string != "10" || !b.boolean) {
            fail_test("conversions runtime", "unexpected conversion results");
        } else {
            pass_test("conversions runtime");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("int", {Runtime::make_string("nope")});
            fail_test("invalid int conversion", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "cannot convert string to int") {
                fail_test("invalid int conversion", err.message());
            } else {
                pass_test("invalid int conversion");
            }
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        const Value t = runtime.call("type", {Runtime::make_integer(10)});
        const Value f = runtime.call("type", {Runtime::make_float(1.5)});
        const Value s = runtime.call("type", {Runtime::make_string("hi")});
        const Value b = runtime.call("type", {Runtime::make_bool(true)});
        const Value n = runtime.call("type", {Runtime::make_null()});
        const Value l = runtime.call("type", {Runtime::make_list({Runtime::make_integer(1)})});
        const Value d = runtime.call("type", {Runtime::make_dict({{"a", Runtime::make_integer(1)}})});
        if (t.string != "int" || f.string != "float" || s.string != "string" || b.string != "bool" ||
            n.string != "null" || l.string != "list" || d.string != "dict") {
            fail_test("type builtin runtime", "unexpected type names");
        } else {
            pass_test("type builtin runtime");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        const Value i = runtime.call("int", {Runtime::make_string("42")});
        const Value fl = runtime.call("float", {Runtime::make_string("3.14")});
        const Value st = runtime.call("str", {Runtime::make_integer(123)});
        const Value bo = runtime.call("bool", {Runtime::make_integer(1)});
        if (i.integer != 42 || fl.floating != 3.14 || st.string != "123" || !bo.boolean) {
            fail_test("conversion builtins runtime", "unexpected results");
        } else {
            pass_test("conversion builtins runtime");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("float", {Runtime::make_string("not-a-number")});
            fail_test("invalid float conversion", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "cannot convert string to float") {
                fail_test("invalid float conversion", err.message());
            } else {
                pass_test("invalid float conversion");
            }
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("bool", {Runtime::make_string("maybe")});
            fail_test("invalid bool conversion", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "cannot convert string to bool") {
                fail_test("invalid bool conversion", err.message());
            } else {
                pass_test("invalid bool conversion");
            }
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("int", {Runtime::make_list({Runtime::make_integer(1)})});
            fail_test("unsupported int conversion", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "cannot convert list to int") {
                fail_test("unsupported int conversion", err.message());
            } else {
                pass_test("unsupported int conversion");
            }
        }
    }

    {
        const Value list = Runtime::make_list({Runtime::make_integer(1), Runtime::make_integer(2)});
        const Value dict = Runtime::make_dict({{"a", Runtime::make_integer(1)}});
        const bool list_ok = Runtime::contains(Runtime::make_integer(2), list).boolean;
        const bool dict_ok = Runtime::contains(Runtime::make_string("a"), dict).boolean;
        const bool str_ok = Runtime::contains(Runtime::make_string("pp"), Runtime::make_string("apple")).boolean;
        const bool range_ok = Runtime::contains(Runtime::make_integer(2), Runtime::make_range(0, 5, 1)).boolean;
        if (!list_ok || !dict_ok || !str_ok || !range_ok) {
            fail_test("membership runtime", "unexpected contains result");
        } else {
            pass_test("membership runtime");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            Runtime::contains(Runtime::make_integer(1), Runtime::make_integer(2));
            fail_test("invalid membership runtime", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "cannot use 'in' with int") {
                fail_test("invalid membership runtime", err.message());
            } else {
                pass_test("invalid membership runtime");
            }
        }
    }

    {
        Value player = Runtime::make_struct(0, "Player", {"name", "number"});
        Runtime::set_field(player, 0, Runtime::make_string("Emir"));
        Runtime::set_field(player, 1, Runtime::make_integer(10));
        const Value name = Runtime::get_field(player, 0);
        if (name.kind != ValueKind::String || name.string != "Emir") {
            fail_test("struct get/set field", "unexpected field value");
        } else {
            pass_test("struct get/set field");
        }
    }
    {
        Value left = Runtime::make_enum(0, 0, "Color", "RED");
        Value right = Runtime::make_enum(0, 0, "Color", "RED");
        Value other = Runtime::make_enum(1, 0, "Status", "RED");
        if (!Runtime::equals(left, right) || Runtime::equals(left, other)) {
            fail_test("enum equality", "unexpected enum compare result");
        } else {
            pass_test("enum equality");
        }
    }
    {
        const Value empty_a = Runtime::make_string("");
        const Value empty_b = Runtime::make_string("");
        const Value hello_a = Runtime::make_string("hello");
        Value hello_b = Runtime::make_string("hello");
        const Value hello_c = Runtime::make_string("hellp");
        const Value utf8 = Runtime::make_string("\xC3\xA7\xC4\x9F");
        Value utf8_copy = Runtime::make_string("\xC3\xA7\xC4\x9F");
        std::string embedded;
        embedded.push_back('a');
        embedded.push_back('\0');
        embedded.push_back('b');
        const Value embedded_a = Runtime::make_string(embedded);
        Value embedded_b = Runtime::make_string(embedded);
        const Value embedded_other = Runtime::make_string("a");
        const Value integer = Runtime::make_integer(1);
        if (!Runtime::equals(empty_a, empty_b) || !Runtime::equals(hello_a, hello_b) ||
            Runtime::equals(hello_a, hello_c) || !Runtime::equals(utf8, utf8_copy) ||
            !Runtime::equals(embedded_a, embedded_b) || Runtime::equals(embedded_a, embedded_other) ||
            Runtime::equals(hello_a, integer) || Runtime::equals(integer, hello_a)) {
            fail_test("string equality", "unexpected string compare result");
        } else {
            pass_test("string equality");
        }
        if (!Runtime::strings_equal(hello_a.string, hello_b.string) ||
            Runtime::strings_equal(hello_a.string, hello_c.string)) {
            fail_test("strings_equal helper", "unexpected compare result");
        } else {
            pass_test("strings_equal helper");
        }
        const Value& same_ref = hello_b;
        if (!Runtime::equals(hello_b, same_ref)) {
            fail_test("string same value ref", "expected identity equality");
        } else {
            pass_test("string same value ref");
        }
    }
    {
        Value a = Runtime::make_struct(0, "Player", {"name"});
        Value b = Runtime::make_struct(0, "Player", {"name"});
        Runtime::set_field(a, 0, Runtime::make_string("A"));
        Runtime::set_field(b, 0, Runtime::make_string("A"));
        Value c = Runtime::make_struct(1, "Team", {"name"});
        Runtime::set_field(c, 0, Runtime::make_string("A"));
        if (!Runtime::equals(a, b) || Runtime::equals(a, c)) {
            fail_test("struct equality", "unexpected struct compare result");
        } else {
            pass_test("struct equality");
        }
    }
    {
        try {
            Runtime::get_field(Runtime::make_integer(1), 0);
            fail_test("struct field on int", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "cannot access field on non-object value") {
                fail_test("struct field on int", err.message());
            } else {
                pass_test("struct field on int");
            }
        }
    }

    {
        auto task = std::make_shared<TaskData>();
        task->state = TaskState::Pending;
        const Value value = Runtime::make_task(task);
        if (value.kind != ValueKind::Task || !value.task || value.task->state != TaskState::Pending) {
            fail_test("task value pending", "unexpected task state");
        } else {
            pass_test("task value pending");
        }
    }
    {
        auto task = std::make_shared<TaskData>();
        task->state = TaskState::Completed;
        task->result = Runtime::make_integer(7);
        const Value left = Runtime::make_task(task);
        const Value right = Runtime::make_task(task);
        if (!Runtime::equals(left, right) || Runtime::format(left) != "<task>") {
            fail_test("task completed value", "unexpected task representation");
        } else {
            pass_test("task completed value");
        }
    }
    {
        std::ostringstream out;
        Runtime runtime(out);
        auto task = std::make_shared<TaskData>();
        task->state = TaskState::Failed;
        const Value task_value = Runtime::make_task(task);
        const Value type_name = runtime.call("type", {task_value});
        if (type_name.string != "task") {
            fail_test("task kind name", "unexpected kind name");
        } else {
            pass_test("task kind name");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        if (Runtime::format(runtime.call("abs", {Runtime::make_integer(-4)})) != "4" ||
            Runtime::format(runtime.call("min", {Runtime::make_integer(2), Runtime::make_integer(1)})) != "1" ||
            Runtime::format(runtime.call("max", {Runtime::make_float(1.0), Runtime::make_float(2.5)})) != "2.5" ||
            Runtime::format(runtime.call("round", {Runtime::make_float(2.5)})) != "3") {
            fail_test("numeric builtins", "unexpected builtin results");
        } else {
            pass_test("numeric builtins");
        }
    }
    {
        std::ostringstream out;
        Runtime runtime(out);
        try {
            runtime.call("clamp", {Runtime::make_integer(1), Runtime::make_integer(5), Runtime::make_integer(0)});
            fail_test("clamp invalid bounds", "expected RuntimeError");
        } catch (const RuntimeError& err) {
            if (err.message() != "clamp bounds are invalid") {
                fail_test("clamp invalid bounds", err.message());
            } else {
                pass_test("clamp invalid bounds");
            }
        }
    }
    {
        const Value text = Runtime::make_string("  Ab  ");
        if (Runtime::format(Runtime::string_trim(text)) != "Ab" ||
            Runtime::format(Runtime::string_lower(Runtime::make_string("AbC"))) != "abc" ||
            Runtime::format(Runtime::string_upper(Runtime::make_string("AbC"))) != "ABC" ||
            Runtime::format(Runtime::string_split(Runtime::make_string("a,b"), Runtime::make_string(","))) != "[a, b]") {
            fail_test("string stdlib methods", "unexpected string method results");
        } else {
            pass_test("string stdlib methods");
        }
    }

    {
        if (!pscript::AsyncIoService::run_socket_readiness_self_test()) {
            fail_test("async io socket readiness", "socket poll self test failed");
        } else {
            pass_test("async io socket readiness");
        }
    }

    if (pscript::crypto_backend_available()) {
        const std::vector<std::uint8_t> sha256_empty =
            pscript::crypto_digest(pscript::CryptoHashAlgorithm::Sha256, nullptr, 0);
        const std::uint8_t expected_sha256[] = {0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14};
        if (sha256_empty.size() != 32 ||
            !std::equal(expected_sha256, expected_sha256 + sizeof(expected_sha256), sha256_empty.begin())) {
            fail_test("crypto sha256 empty vector", "digest mismatch");
        } else {
            pass_test("crypto sha256 empty vector");
        }
        if (!pscript::crypto_secure_compare(sha256_empty.data(), sha256_empty.size(), sha256_empty.data(),
                                            sha256_empty.size()) ||
            pscript::crypto_secure_compare(sha256_empty.data(), sha256_empty.size(), expected_sha256,
                                           sizeof(expected_sha256))) {
            fail_test("crypto secure compare", "unexpected compare result");
        } else {
            pass_test("crypto secure compare");
        }
        {
            const std::filesystem::path path = std::filesystem::temp_directory_path() / "pscript_crypto_hash_file.txt";
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "abc";
            out.close();
            const std::vector<std::uint8_t> file_hash =
                pscript::crypto_digest_file(pscript::CryptoHashAlgorithm::Sha256, path.string());
            static const std::uint8_t expected_abc[] = {0xba, 0x78, 0x16};
            if (file_hash.size() != 32 ||
                !std::equal(expected_abc, expected_abc + sizeof(expected_abc), file_hash.begin())) {
                fail_test("crypto hash file abc", "digest mismatch");
            } else {
                pass_test("crypto hash file abc");
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        {
            static const char kAbc[] = "abc";
            const std::vector<std::uint8_t> one_shot_abc = pscript::crypto_digest(
                pscript::CryptoHashAlgorithm::Sha256, reinterpret_cast<const std::uint8_t*>(kAbc), 3);
            static const std::uint8_t expected_abc[] = {0xba, 0x78, 0x16};
            if (one_shot_abc.size() != 32 ||
                !std::equal(expected_abc, expected_abc + sizeof(expected_abc), one_shot_abc.begin())) {
                fail_test("crypto digest abc one-shot", "digest mismatch");
            } else {
                pass_test("crypto digest abc one-shot");
            }
        }
        {
            pscript::CryptoHasher hasher;
            hasher.reset(pscript::CryptoHashAlgorithm::Sha256);
            hasher.update(reinterpret_cast<const std::uint8_t*>("a"), 1);
            hasher.update(reinterpret_cast<const std::uint8_t*>("b"), 1);
            const std::vector<std::uint8_t> streamed = hasher.digest();
            static const std::uint8_t expected_ab[] = {0xfb, 0x8e, 0x20};
            if (streamed.size() != 32 ||
                !std::equal(expected_ab, expected_ab + sizeof(expected_ab), streamed.begin())) {
                fail_test("crypto streaming hasher ab", "digest mismatch");
            } else {
                pass_test("crypto streaming hasher ab");
            }
        }
        static const char kKey[] = "Jefe";
        static const char kData[] = "what do ya want for nothing?";
        const std::vector<std::uint8_t> hmac = pscript::crypto_hmac(
            pscript::CryptoHmacAlgorithm::Sha256, reinterpret_cast<const std::uint8_t*>(kKey), sizeof(kKey) - 1,
            reinterpret_cast<const std::uint8_t*>(kData), sizeof(kData) - 1);
        static const std::uint8_t expected_hmac[] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
                                                     0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7};
        if (hmac.size() != 32 || !std::equal(expected_hmac, expected_hmac + sizeof(expected_hmac), hmac.begin())) {
            fail_test("crypto hmac sha256 rfc4231", "digest mismatch");
        } else {
            pass_test("crypto hmac sha256 rfc4231");
        }
        if (pscript::crypto_kdf_pbkdf2_sha256_available()) {
            static const char kPassword[] = "password";
            static const char kSalt[] = "salt";
            const std::vector<std::uint8_t> pbkdf2 = pscript::crypto_pbkdf2_sha256(
                reinterpret_cast<const std::uint8_t*>(kPassword), sizeof(kPassword) - 1,
                reinterpret_cast<const std::uint8_t*>(kSalt), sizeof(kSalt) - 1, 1, 20);
            static const std::uint8_t expected_pbkdf2[] = {0x12, 0x0f, 0xb6, 0xcf};
            if (pbkdf2.size() != 20 ||
                !std::equal(expected_pbkdf2, expected_pbkdf2 + sizeof(expected_pbkdf2), pbkdf2.begin())) {
                fail_test("crypto pbkdf2 sha256 rfc6070", "digest mismatch");
            } else {
                pass_test("crypto pbkdf2 sha256 rfc6070");
            }
        }
        if (pscript::crypto_aead_aes_gcm_available()) {
            const std::vector<std::uint8_t> key(32, 0);
            const std::vector<std::uint8_t> nonce(12, 0);
            static const char kPlain[] = "hello";
            const pscript::CryptoAeadResult encrypted = pscript::crypto_encrypt_aes_gcm(
                key.data(), key.size(), nonce.data(), nonce.size(),
                reinterpret_cast<const std::uint8_t*>(kPlain), sizeof(kPlain) - 1, nullptr, 0);
            try {
                const std::vector<std::uint8_t> decrypted = pscript::crypto_decrypt_aes_gcm(
                    key.data(), key.size(), nonce.data(), nonce.size(), encrypted.ciphertext.data(),
                    encrypted.ciphertext.size(), encrypted.tag.data(), encrypted.tag.size(), nullptr, 0);
                if (decrypted.size() != sizeof(kPlain) - 1 ||
                    std::memcmp(decrypted.data(), kPlain, sizeof(kPlain) - 1) != 0) {
                    fail_test("crypto aes gcm roundtrip", "plaintext mismatch");
                } else {
                    pass_test("crypto aes gcm roundtrip");
                }
            } catch (const RuntimeError&) {
                fail_test("crypto aes gcm roundtrip", "decrypt failed");
            }
            std::vector<std::uint8_t> bad_tag = encrypted.tag;
            bad_tag[0] ^= 0x01;
            try {
                pscript::crypto_decrypt_aes_gcm(key.data(), key.size(), nonce.data(), nonce.size(),
                                                encrypted.ciphertext.data(), encrypted.ciphertext.size(), bad_tag.data(),
                                                bad_tag.size(), nullptr, 0);
                fail_test("crypto aes gcm bad tag", "expected authentication failure");
            } catch (const RuntimeError& error) {
                if (error.message() != "authentication failed") {
                    fail_test("crypto aes gcm bad tag", "unexpected error message");
                } else {
                    pass_test("crypto aes gcm bad tag");
                }
            }
        }
        if (pscript::crypto_aead_chacha20_poly1305_available()) {
            static const std::uint8_t chacha_key[] = {
                0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
                0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
            static const std::uint8_t chacha_nonce[] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0xea, 0x00,
                                                        0x00, 0x00, 0x00, 0x00};
            static const char kSunscreen[] =
                "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
                "sunscreen would be it.";
            const pscript::CryptoAeadResult encrypted = pscript::crypto_encrypt_chacha20_poly1305(
                chacha_key, sizeof(chacha_key), chacha_nonce, sizeof(chacha_nonce),
                reinterpret_cast<const std::uint8_t*>(kSunscreen), sizeof(kSunscreen) - 1, nullptr, 0);
            static const std::uint8_t expected_tag[] = {0xec, 0x0b, 0x72, 0xa3, 0x15, 0x4b, 0x40, 0x27,
                                                        0xea, 0x3d, 0x17, 0xeb, 0x8a, 0x93, 0x18, 0x2c};
            if (encrypted.tag.size() != sizeof(expected_tag) ||
                !std::equal(expected_tag, expected_tag + sizeof(expected_tag), encrypted.tag.begin())) {
                fail_test("crypto chacha20 poly1305 rfc8439 tag", "tag mismatch");
            } else {
                pass_test("crypto chacha20 poly1305 rfc8439 tag");
            }
            const std::vector<std::uint8_t> decrypted = pscript::crypto_decrypt_chacha20_poly1305(
                chacha_key, sizeof(chacha_key), chacha_nonce, sizeof(chacha_nonce), encrypted.ciphertext.data(),
                encrypted.ciphertext.size(), encrypted.tag.data(), encrypted.tag.size(), nullptr, 0);
            if (decrypted.size() != sizeof(kSunscreen) - 1 ||
                std::memcmp(decrypted.data(), kSunscreen, sizeof(kSunscreen) - 1) != 0) {
                fail_test("crypto chacha20 poly1305 rfc8439", "plaintext mismatch");
            } else {
                pass_test("crypto chacha20 poly1305 rfc8439");
            }
        }
#if defined(PSCRIPT_HAS_OPENSSL)
        if (pscript::crypto_kdf_scrypt_available()) {
            static const char kPassword[] = "password";
            static const char kSalt[] = "salt";
            pscript::CryptoKdfScryptParams params;
            params.n = 1024;
            params.r = 8;
            params.p = 16;
            const std::vector<std::uint8_t> scrypt_out = pscript::crypto_scrypt(
                reinterpret_cast<const std::uint8_t*>(kPassword), sizeof(kPassword) - 1,
                reinterpret_cast<const std::uint8_t*>(kSalt), sizeof(kSalt) - 1, params, 64);
            static const std::uint8_t expected_scrypt[] = {0xfd, 0xba, 0xbe, 0x1c, 0x9d, 0x34, 0x72, 0x00};
            if (scrypt_out.size() != 64 ||
                !std::equal(expected_scrypt, expected_scrypt + sizeof(expected_scrypt), scrypt_out.begin())) {
                fail_test("crypto scrypt rfc7914", "digest mismatch");
            } else {
                pass_test("crypto scrypt rfc7914");
            }
        }
#endif
    } else {
        fail_test("crypto backend", "not available");
    }

    {
        pscript::alloc_guard::test_fail_hook() = [](std::size_t) { return true; };
        bool caught = false;
        try {
            (void)Runtime::make_list(std::vector<Value>{Runtime::make_integer(1)});
        } catch (const RuntimeError& err) {
            caught = err.message().find("out of memory") != std::string::npos;
        }
        pscript::alloc_guard::test_fail_hook() = nullptr;
        if (!caught) {
            fail_test("alloc guard list", "expected out of memory");
        } else {
            pass_test("alloc guard list");
        }
    }

    {
        pscript::alloc_guard::test_fail_hook() = [](std::size_t) { return true; };
        bool caught = false;
        try {
            (void)Runtime::make_dict({{"k", Runtime::make_integer(1)}});
        } catch (const RuntimeError& err) {
            caught = err.message().find("out of memory") != std::string::npos;
        }
        pscript::alloc_guard::test_fail_hook() = nullptr;
        if (!caught) {
            fail_test("alloc guard dict", "expected out of memory");
        } else {
            pass_test("alloc guard dict");
        }
    }

    {
        const Value joined = Runtime::concat_strings("ab", "cd");
        if (joined.kind != ValueKind::String || joined.string != "abcd") {
            fail_test("concat strings", "unexpected result");
        } else {
            pass_test("concat strings");
        }
    }

    {
        const std::filesystem::path temp =
            std::filesystem::temp_directory_path() / "pscript_file_handle_lifecycle.txt";
        std::error_code remove_ec;
        std::filesystem::remove(temp, remove_ec);
        {
            pscript::NativeFileHandleData handle;
            handle.path = temp.string();
            handle.mode = "w";
            handle.stream = std::make_shared<std::fstream>(temp, std::ios::out | std::ios::binary);
            handle.closed = false;
            if (!handle.stream->is_open()) {
                fail_test("file handle lifecycle", "stream not open");
            }
        }
        std::fstream verify(temp, std::ios::in | std::ios::binary);
        if (!verify.is_open()) {
            fail_test("file handle lifecycle", "file not accessible after handle destroyed");
        } else {
            pass_test("file handle lifecycle");
        }
        std::filesystem::remove(temp, remove_ec);
    }

    if (pscript::sql_backend_available()) {
        try {
            const std::shared_ptr<pscript::SqlConnectionHandle> memory = pscript::SqlConnectionHandle::open(":memory:");
            if (!memory->is_open()) {
                fail_test("sql memory open", "connection not open");
            } else {
                pass_test("sql memory open");
            }
            memory->close();
            if (memory->is_open()) {
                fail_test("sql close", "connection still open");
            } else {
                pass_test("sql close");
            }
            memory->close();
            pass_test("sql double close");
        } catch (const std::exception& ex) {
            fail_test("sql connection lifecycle", ex.what());
        }
        {
            std::ostringstream out;
            pscript::Runtime runtime(out);
            auto data = std::make_shared<pscript::NativeDatabaseData>();
            data->connection = pscript::SqlConnectionHandle::open(":memory:");
            data->closed = false;
            runtime.register_sql_database(data);
            runtime.close_all_sql_databases();
            if (data->connection->is_open()) {
                fail_test("sql runtime shutdown cleanup", "connection still open");
            } else {
                pass_test("sql runtime shutdown cleanup");
            }
        }
    } else {
        fail_test("sql backend", "not available");
    }

    {
        std::vector<std::pair<std::string, Value>> entries;
        entries.emplace_back("a", Runtime::make_integer(1));
        entries.emplace_back("b", Runtime::make_integer(2));
        entries.emplace_back("c", Runtime::make_integer(3));
        Value dict = Runtime::make_dict(std::move(entries));
        const Value keys = Runtime::dict_keys(dict);
        if (!keys.list || keys.list->items.size() != 3) {
            fail_test("dict insertion order keys", "unexpected key count");
        } else if (keys.list->items[0].string != "a" || keys.list->items[2].string != "c") {
            fail_test("dict insertion order keys", "order changed");
        } else {
            pass_test("dict insertion order keys");
        }
        const Value found = Runtime::get_index(dict, Runtime::make_string("b"));
        if (found.kind != ValueKind::Integer || found.integer != 2) {
            fail_test("dict hash lookup", "wrong value");
        } else {
            pass_test("dict hash lookup");
        }
    }

    {
        Value dict = Runtime::make_dict({});
        for (std::int64_t i = 0; i < 2048; ++i) {
            Runtime::set_index(dict, Runtime::make_string("k" + std::to_string(i)), Runtime::make_integer(i));
        }
        if (!dict.dict || dict.dict->entries.size() != 2048 || dict.dict->key_index.size() != 2048) {
            fail_test("dict rehash growth", "unexpected size after growth");
        } else if (Runtime::dict_lookup(*dict.dict, "k1024")->integer != 1024) {
            fail_test("dict rehash growth", "lookup failed after rehash");
        } else {
            pass_test("dict rehash growth");
        }
        Runtime::set_index(dict, Runtime::make_string("k1024"), Runtime::make_integer(99));
        if (Runtime::get_index(dict, Runtime::make_string("k1024")).integer != 99) {
            fail_test("dict overwrite key", "value not updated");
        } else {
            pass_test("dict overwrite key");
        }
        std::string embedded;
        embedded.push_back('a');
        embedded.push_back('\0');
        embedded.push_back('b');
        Runtime::set_index(dict, Runtime::make_string(embedded), Runtime::make_integer(42));
        if (!Runtime::dict_contains_key(dict, Runtime::make_string(embedded)).boolean ||
            Runtime::get_index(dict, Runtime::make_string(embedded)).integer != 42) {
            fail_test("dict embedded null key", "lookup failed");
        } else {
            pass_test("dict embedded null key");
        }
        const Value utf8_key = Runtime::make_string("\xC3\xA7");
        Runtime::set_index(dict, utf8_key, Runtime::make_integer(7));
        if (!Runtime::dict_contains_key(dict, Runtime::make_string("\xC3\xA7")).boolean) {
            fail_test("dict utf-8 key", "contains failed");
        } else {
            pass_test("dict utf-8 key");
        }
        if (Runtime::dict_contains_key(dict, Runtime::make_string("missing")).boolean) {
            fail_test("dict miss lookup", "expected false");
        } else {
            pass_test("dict miss lookup");
        }
        Runtime::dict_remove(dict, Runtime::make_string("k0"));
        if (Runtime::dict_contains_key(dict, Runtime::make_string("k0")).boolean ||
            !Runtime::dict_contains_key(dict, Runtime::make_string("k1")).boolean) {
            fail_test("dict remove reindex", "index inconsistent after remove");
        } else {
            pass_test("dict remove reindex");
        }
        const Value keys = Runtime::dict_keys(dict);
        if (!keys.list || keys.list->items.empty() || keys.list->items[0].string == "k0") {
            fail_test("dict remove insertion order", "removed key still visible");
        } else {
            pass_test("dict remove insertion order");
        }
    }

    {
        Value set = Runtime::make_set({Runtime::make_integer(1), Runtime::make_integer(2)});
        Runtime::set_add(set, Runtime::make_integer(3));
        if (!Runtime::set_contains(set, Runtime::make_integer(2)).boolean) {
            fail_test("set indexed lookup", "missing member");
        } else {
            pass_test("set indexed lookup");
        }
    }

    {
        auto count_iter = [](const Value& iterable) {
            Value it = runtime_get_iter(iterable);
            if (!is_native_iterator(it) || it.kind != ValueKind::Iterator) {
                return -1;
            }
            int count = 0;
            Value item;
            while (native_iterator_try_next(it, item)) {
                ++count;
            }
            Value again;
            if (native_iterator_try_next(it, again)) {
                return -2;
            }
            return count;
        };

        if (count_iter(Runtime::make_list({})) != 0) {
            fail_test("native list iterator empty", "wrong count");
        } else {
            pass_test("native list iterator empty");
        }
        if (count_iter(Runtime::make_list({Runtime::make_integer(10), Runtime::make_integer(20)})) != 2) {
            fail_test("native list iterator", "wrong count");
        } else {
            pass_test("native list iterator");
        }
        if (count_iter(Runtime::make_tuple({Runtime::make_integer(1)})) != 1) {
            fail_test("native tuple iterator", "wrong count");
        } else {
            pass_test("native tuple iterator");
        }
        Value dict = Runtime::make_dict({{"first", Runtime::make_integer(1)}, {"second", Runtime::make_integer(2)}});
        Value it = runtime_get_iter(dict);
        Value k1;
        Value k2;
        if (!native_iterator_try_next(it, k1) || k1.string != "first" || !native_iterator_try_next(it, k2) ||
            k2.string != "second") {
            fail_test("native dict iterator order", "keys mismatch");
        } else {
            pass_test("native dict iterator order");
        }
        Value bytes = Runtime::make_bytes({97, 98});
        Value byte_item;
        it = runtime_get_iter(bytes);
        if (!native_iterator_try_next(it, byte_item) || byte_item.integer != 97) {
            fail_test("native bytes iterator", "wrong byte");
        } else {
            pass_test("native bytes iterator");
        }
        try {
            Value empty_it = runtime_get_iter(Runtime::make_list({}));
            native_iterator_next(empty_it);
            fail_test("native iterator stop", "expected StopIteration");
        } catch (const RuntimeError& err) {
            if (err.message() == "StopIteration") {
                pass_test("native iterator stop");
            } else {
                fail_test("native iterator stop", err.message());
            }
        }
    }

    {
        std::cout << "representation sizeof(Value)=" << sizeof(Value) << " sizeof(Cell)=" << sizeof(pscript::Cell)
                  << " sizeof(optional<Value>)=" << sizeof(std::optional<Value>) << '\n';
        pass_test("value representation sizes logged");
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All runtime tests passed\n";
    return 0;
}
