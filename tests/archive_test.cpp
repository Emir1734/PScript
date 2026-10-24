#include "compiler/compiler.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "stdlib/archive/archive_security.h"

#include "vm/vm.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

using pscript::CompileOptions;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile_with_builtins;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

std::string run_source(std::string_view source) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(std::string(source)), runtime);
    vm.run();
    return out.str();
}

void expect_output(std::string_view name, std::string_view source, std::string_view expected) {
    try {
        const std::string actual = run_source(source);
        if (actual != expected) {
            fail_test(name, "expected '" + std::string(expected) + "', got '" + actual + "'");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_runtime_error(std::string_view name, std::string_view source, std::string_view substring) {
    try {
        (void)run_source(source);
        fail_test(name, "expected runtime error");
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (message.find(substring) == std::string::npos) {
            fail_test(name, "unexpected error: " + message);
            return;
        }
        pass_test(name);
    }
}

std::filesystem::path test_dir(std::string_view label) {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("pscript-archive-" + std::string(label) + "-" + std::to_string(++counter));
    std::filesystem::create_directories(dir);
    return dir;
}

void write_bytes_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string phs_path(const std::filesystem::path& path) {
    return path.generic_string();
}

}  // namespace

int run_archive_hard_tests();
int main() {
    const auto base = test_dir("main");
    const std::string base_str = base.string();
    write_bytes_file(base / "hello.txt", "hello");

    expect_output("archive module import", "import archive\nprint(\"archive\")\n", "archive\n");

    expect_output("archive zip roundtrip",
                  "import archive\nimport encoding\n"
                  "a = archive.create(\"" +
                      phs_path(base / "pack.zip") + "\")\n"
                  "a.write(\"hello.txt\", encoding.encode(\"zip-data\"))\n"
                  "a.close()\n"
                  "r = archive.open(\"" +
                      phs_path(base / "pack.zip") + "\", \"r\")\n"
                  "print(encoding.decode(r.read(\"hello.txt\")))\n"
                  "r.close()\n",
                  "zip-data\n");

    expect_output("archive tar roundtrip",
                  "import archive\nimport encoding\n"
                  "a = archive.create(\"" +
                      phs_path(base / "pack.tar") + "\", \"tar\")\n"
                  "a.write(\"data.bin\", encoding.encode(\"tar-bytes\"))\n"
                  "a.close()\n"
                  "r = archive.open(\"" +
                      phs_path(base / "pack.tar") + "\")\n"
                  "print(encoding.decode(r.read(\"data.bin\")))\n"
                  "r.close()\n",
                  "tar-bytes\n");

    expect_output("archive tar.gz roundtrip",
                  "import archive\nimport encoding\n"
                  "a = archive.create(\"" +
                      phs_path(base / "pack.tar.gz") + "\", \"tar.gz\")\n"
                  "a.write(\"inner.txt\", encoding.encode(\"gz-data\"))\n"
                  "a.close()\n"
                  "r = archive.open(\"" +
                      phs_path(base / "pack.tar.gz") + "\")\n"
                  "print(encoding.decode(r.read(\"inner.txt\")))\n"
                  "r.close()\n",
                  "gz-data\n");

    expect_output("archive zip list",
                  "import archive\nimport encoding\n"
                  "a = archive.create(\"" +
                      phs_path(base / "list.zip") + "\")\n"
                  "a.write(\"a.txt\", encoding.encode(\"1\"))\n"
                  "a.write(\"b.txt\", encoding.encode(\"2\"))\n"
                  "a.close()\n"
                  "r = archive.open(\"" +
                      phs_path(base / "list.zip") + "\")\n"
                  "names = r.list()\n"
                  "print(names.contains(\"a.txt\"))\n"
                  "print(names.contains(\"b.txt\"))\n"
                  "r.close()\n",
                  "true\ntrue\n");

    expect_runtime_error("archive rejects traversal write",
                         "import archive\nimport encoding\n"
                         "a = archive.create(\"" +
                             phs_path(base / "bad.zip") + "\")\n"
                         "a.write(\"../evil.txt\", encoding.encode(\"x\"))\n"
                         "a.close()\n",
                         "invalid archive entry path");

    try {
        const auto dest = test_dir("secure");
        pscript::archive::secure_extract_path(dest, "safe/file.txt");
        pass_test("archive secure extract normal path");
    } catch (const std::exception& ex) {
        fail_test("archive secure extract normal path", ex.what());
    }

    try {
        const auto dest = test_dir("escape");
        pscript::archive::secure_extract_path(dest, "../outside.txt");
        fail_test("archive secure extract blocks traversal", "no exception");
    } catch (const std::exception&) {
        pass_test("archive secure extract blocks traversal");
    }

    try {
        const auto normalized = pscript::archive::normalize_archive_entry_path("foo/bar.txt", false);
        if (!normalized || *normalized != "foo/bar.txt") {
            fail_test("archive path normalize", "unexpected value");
        } else {
            pass_test("archive path normalize");
        }
    } catch (const std::exception& ex) {
        fail_test("archive path normalize", ex.what());
    }

    expect_output("archive zip append",
                  "import archive\nimport encoding\n"
                  "p = \"" +
                      phs_path(base / "append.zip") + "\"\n"
                  "a = archive.create(p)\n"
                  "a.write(\"first.txt\", encoding.encode(\"1\"))\n"
                  "a.close()\n"
                  "b = archive.open(p, \"a\")\n"
                  "b.write(\"second.txt\", encoding.encode(\"2\"))\n"
                  "b.close()\n"
                  "r = archive.open(p)\n"
                  "print(r.list().contains(\"second.txt\"))\n"
                  "r.close()\n",
                  "true\n");

    expect_output("archive iterator",
                  "import archive\nimport encoding\n"
                  "a = archive.create(\"" +
                      phs_path(base / "iter.zip") + "\")\n"
                  "a.write(\"x\", encoding.encode(\"1\"))\n"
                  "a.close()\n"
                  "r = archive.open(\"" +
                      phs_path(base / "iter.zip") + "\")\n"
                  "found = false\n"
                  "for e in r:\n"
                  "    if e[\"name\"] == \"x\":\n"
                  "        found = true\n"
                  "print(found)\n"
                  "r.close()\n",
                  "true\n");

    expect_output("archive open_entry io",
                  "import archive\nimport encoding\n"
                  "a = archive.create(\"" +
                      phs_path(base / "io.zip") + "\")\n"
                  "a.write(\"f.txt\", encoding.encode(\"payload\"))\n"
                  "a.close()\n"
                  "r = archive.open(\"" +
                      phs_path(base / "io.zip") + "\")\n"
                  "h = r.open_entry(\"f.txt\")\n"
                  "print(h.read())\n"
                  "h.close()\n"
                  "r.close()\n",
                  "payload\n");

    if (g_failed != 0) {
        std::cerr << g_failed << " archive test(s) failed\n";
        return 1;
    }
    if (run_archive_hard_tests() != 0) {
        return 1;
    }
    std::cout << "archive tests passed\n";
    return 0;
}
