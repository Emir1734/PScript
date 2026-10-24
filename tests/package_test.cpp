#include "package/atomic_file.h"
#include "package/git_clone.h"
#include "package/install.h"
#include "package/lockfile.h"
#include "package/manifest.h"
#include "package/package_archive.h"
#include "package/packaging.h"
#include "package/package_identity.h"
#include "package/project.h"
#include "package/registry.h"
#include "package/semver.h"
#include "stdlib/archive/archive_security.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::uint32_t process_id_for_tests() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

void set_registry_env(const std::filesystem::path& registry_root) {
    const std::string absolute = std::filesystem::absolute(registry_root).lexically_normal().string();
#ifdef _WIN32
    SetEnvironmentVariableA("PSCRIPT_REGISTRY", absolute.c_str());
    _putenv_s("PSCRIPT_REGISTRY", absolute.c_str());
#else
    setenv("PSCRIPT_REGISTRY", absolute.c_str(), 1);
#endif
}

}  // namespace

int main() {
    using namespace pscript::package;

    {
        const auto v = SemVer::parse("1.2.3");
        if (!v || v->major != 1 || v->minor != 2 || v->patch != 3) {
            fail_test("semver parse", "invalid");
        } else {
            pass_test("semver parse");
        }
        const auto caret = VersionConstraint::parse("^1.2.0");
        if (!caret || !caret->matches(SemVer{1, 9, 0}) || caret->matches(SemVer{2, 0, 0})) {
            fail_test("semver caret", "constraint mismatch");
        } else {
            pass_test("semver caret");
        }
    }

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / ("pscript_package_test_pid" + std::to_string(process_id_for_tests()));
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    const std::filesystem::path fixtures = temp_root / "greet_lib_src";
    {
        std::string error;
        if (!scaffold_project(fixtures, "greet_lib", &error)) {
            fail_test("fixture package scaffold", error);
            return 1;
        }
        ProjectManifest fixture_manifest;
        fixture_manifest.name = "greet_lib";
        fixture_manifest.version = "1.0.0";
        write_manifest_file((fixtures / "pscript.toml").string(), fixture_manifest, &error);
        std::ofstream module(fixtures / "src" / "greet_lib.phs", std::ios::binary | std::ios::trunc);
        module << "pub func greet():\n    return \"hello-package\"\n";
    }

    {
        std::string error;
        if (!scaffold_project(temp_root / "app", "app", &error)) {
            fail_test("scaffold project", error);
        } else {
            pass_test("scaffold project");
        }
        ProjectManifest manifest;
        manifest.name = "app";
        manifest.version = "1.0.0";
        DependencySpec dep;
        dep.name = "greet_lib";
        dep.source = DependencySourceKind::Path;
        dep.path = std::filesystem::absolute(fixtures).lexically_normal().string();
        dep.constraint.raw = "1.0.0";
        manifest.dependencies["greet_lib"] = dep;
        const ProjectPaths paths = project_paths_from_root(temp_root / "app");
        write_manifest_file(paths.manifest.string(), manifest, &error);
        InstallOptions options;
        if (!install_project(paths, options, &error)) {
            fail_test("install path dependency", error);
        } else {
            pass_test("install path dependency");
        }
        const auto lock = load_lockfile(paths.lockfile.string(), &error);
        if (!lock || lock->packages.empty()) {
            fail_test("lockfile after install", error.empty() ? "empty lockfile" : error);
        } else {
            pass_test("lockfile after install");
        }
        const auto resolved = resolve_package_module("greet_lib", (paths.src_dir / "main.phs").string());
        if (!resolved || !std::filesystem::exists(*resolved)) {
            fail_test("resolve package module", "missing module path");
        } else if (resolved->find("greet_lib.phs") == std::string::npos) {
            fail_test("resolve package module", "expected greet_lib.phs, got " + *resolved);
        } else {
            pass_test("resolve package module");
        }
    }

    {
        std::string error;
        const std::filesystem::path dual_root = temp_root / "dual_main_app";
        if (!scaffold_project(dual_root, "dual_main_app", &error)) {
            fail_test("main.phs does not override library module", error);
        } else {
            ProjectManifest manifest;
            manifest.name = "dual_main_app";
            manifest.version = "1.0.0";
            DependencySpec dep;
            dep.name = "greet_lib";
            dep.source = DependencySourceKind::Path;
            dep.path = std::filesystem::absolute(fixtures).lexically_normal().string();
            dep.constraint.raw = "1.0.0";
            manifest.dependencies["greet_lib"] = dep;
            const ProjectPaths paths = project_paths_from_root(dual_root);
            write_manifest_file(paths.manifest.string(), manifest, &error);
            InstallOptions options;
            if (!install_project(paths, options, &error)) {
                fail_test("main.phs does not override library module", error);
            } else {
                const auto resolved = resolve_package_module("greet_lib", (paths.src_dir / "main.phs").string());
                const std::filesystem::path main_scaffold = fixtures / "src" / "main.phs";
                if (!std::filesystem::exists(main_scaffold)) {
                    fail_test("main.phs does not override library module", "fixture main.phs missing");
                } else if (!resolved || resolved->find("greet_lib.phs") == std::string::npos) {
                    fail_test("main.phs does not override library module",
                              resolved ? "resolved " + *resolved : "no resolution");
                } else {
                    pass_test("main.phs does not override library module");
                }
            }
        }
        const auto explicit_main = resolve_package_module("greet_lib.main", (dual_root / "src" / "main.phs").string());
        if (!explicit_main || explicit_main->find("main.phs") == std::string::npos) {
            fail_test("import package.main resolves main.phs", explicit_main ? *explicit_main : "no path");
        } else {
            pass_test("import package.main resolves main.phs");
        }
    }

    {
        std::string error;
        if (validate_git_remote_url("https://example.com/repo.git", &error) &&
            validate_git_ref("v1.0.0", &error)) {
            pass_test("git url and ref validation ok");
        } else {
            fail_test("git url and ref validation ok", error);
        }
        if (!validate_git_remote_url("https://evil.com/x;rm -rf /", &error)) {
            pass_test("git url rejects shell metacharacters");
        } else {
            fail_test("git url rejects shell metacharacters", "accepted");
        }
        if (!validate_git_ref("main;whoami", &error)) {
            pass_test("git ref rejects shell metacharacters");
        } else {
            fail_test("git ref rejects shell metacharacters", "accepted");
        }
        const std::string redacted = redact_git_url_for_display("https://user:secret@github.com/org/repo.git");
        if (redacted.find("secret") != std::string::npos || redacted.find("user:") != std::string::npos) {
            fail_test("git url redaction", redacted);
        } else {
            pass_test("git url redaction");
        }
        const std::filesystem::path clone_dest = temp_root / "git_clone_dest";
        std::filesystem::remove_all(clone_dest, ec);
        if (run_git_clone(fixtures.string(), "not-a-real-ref", clone_dest, &error)) {
            fail_test("git clone failure not success", "reported success");
        } else if (std::filesystem::exists(clone_dest)) {
            fail_test("git clone failure cleanup", "destination left behind");
        } else {
            pass_test("git clone failure not success");
        }
    }

    {
        std::string error;
        if (!validate_package_name("@emir/utils", &error)) {
            fail_test("scoped package name", error);
        } else {
            pass_test("scoped package name");
        }
        const auto scoped = parse_scoped_package_id("@emir/utils");
        if (!scoped || scoped->username != "emir" || scoped->package != "utils") {
            fail_test("parse scoped package id", "mismatch");
        } else {
            pass_test("parse scoped package id");
        }
        if (validate_package_name("bad/name", &error)) {
            fail_test("reject invalid package name", "accepted");
        } else {
            pass_test("reject invalid package name");
        }
    }

    {
        const std::filesystem::path registry_root = temp_root / "registry";
        std::filesystem::create_directories(registry_root, ec);
        const auto version = SemVer{1, 0, 0};
        std::string error;
        if (!registry_publish_package(registry_root.string(), "greet_lib", version, fixtures.string(), "", &error)) {
            fail_test("local registry publish", error);
        } else {
            pass_test("local registry publish");
        }
        const auto versions = registry_list_versions(registry_root.string(), "greet_lib", &error);
        if (versions.empty()) {
            fail_test("local registry list", error);
        } else {
            pass_test("local registry list");
        }
        const std::filesystem::path version_dir =
            registry_package_root(registry_root, "greet_lib") / "1.0.0";
        std::filesystem::remove_all(version_dir / "source", ec);
        if (!std::filesystem::exists(version_dir / "package.tar.gz")) {
            fail_test("registry tar.gz publish", "package.tar.gz missing");
        } else {
            pass_test("registry tar.gz publish");
        }
        set_registry_env(registry_root);
        const std::filesystem::path app_root = temp_root / "tar_app";
        if (!scaffold_project(app_root, "tar_app", &error)) {
            fail_test("registry tar.gz install", error);
        } else {
            ProjectManifest app_manifest;
            app_manifest.name = "tar_app";
            app_manifest.version = "1.0.0";
            DependencySpec dep;
            dep.name = "greet_lib";
            dep.source = DependencySourceKind::Registry;
            dep.constraint = *VersionConstraint::parse("^1.0.0");
            app_manifest.dependencies["greet_lib"] = dep;
            const ProjectPaths paths = project_paths_from_root(app_root);
            write_manifest_file(paths.manifest.string(), app_manifest, &error);
            InstallOptions options;
            if (!install_project(paths, options, &error)) {
                fail_test("registry tar.gz install", error);
            } else {
                const std::filesystem::path module =
                    paths.deps_dir / "greet_lib-1.0.0" / "src" / "greet_lib.phs";
                if (!std::filesystem::exists(module)) {
                    fail_test("registry tar.gz install", "module not materialized");
                } else {
                    pass_test("registry tar.gz install");
                }
            }
        }
    }

    {
        const std::filesystem::path bad_root = temp_root / "registry_bad_archive";
        std::filesystem::create_directories(bad_root, ec);
        const std::filesystem::path package_root = registry_package_root(bad_root, "bad_pkg");
        const std::filesystem::path version_dir = package_root / "1.0.0";
        std::filesystem::create_directories(version_dir, ec);
        std::ofstream bad(version_dir / "package.tar.gz", std::ios::binary | std::ios::trunc);
        bad << "not-a-valid-gzip-archive";
        std::ofstream versions(version_dir.parent_path() / "versions.json", std::ios::trunc);
        versions << "[\"1.0.0\"]";
        set_registry_env(bad_root);
        const std::filesystem::path dest = temp_root / "bad_extract_dest";
        std::string error;
        if (registry_download_package(bad_root.string(), "bad_pkg", SemVer{1, 0, 0}, dest.string(), &error)) {
            fail_test("malformed archive rejected", "expected failure");
        } else if (std::filesystem::exists(dest)) {
            fail_test("malformed archive cleanup", "destination left behind");
        } else {
            pass_test("malformed archive rejected");
        }
    }

    {
        try {
            (void)pscript::archive::secure_extract_path(temp_root / "traversal_dest", "../evil.txt");
            fail_test("traversal archive rejected", "secure_extract_path allowed escape");
        } catch (const std::exception&) {
            pass_test("traversal archive rejected");
        }
    }

    {
        const std::filesystem::path checksum_root = temp_root / "registry_checksum";
        std::filesystem::create_directories(checksum_root, ec);
        std::string error;
        if (!registry_publish_package(checksum_root.string(), "greet_lib", SemVer{1, 0, 0}, fixtures.string(), "",
                                      &error)) {
            fail_test("checksum mismatch on install", error);
        } else {
            const std::filesystem::path version_dir =
                registry_package_root(checksum_root, "greet_lib") / "1.0.0";
            std::ofstream tamper(version_dir / "checksum.txt", std::ios::trunc);
            tamper << "0000000000000000000000000000000000000000000000000000000000000000";
            std::filesystem::remove_all(version_dir / "source", ec);
            set_registry_env(checksum_root);
            const std::filesystem::path dest = temp_root / "checksum_dest";
            if (registry_download_package(checksum_root.string(), "greet_lib", SemVer{1, 0, 0}, dest.string(),
                                          &error)) {
                fail_test("checksum mismatch on install", "expected failure");
            } else if (std::filesystem::exists(dest)) {
                fail_test("checksum mismatch cleanup", "destination left behind");
            } else {
                pass_test("checksum mismatch on install");
            }
        }
    }

    {
        const std::filesystem::path app_root = temp_root / "lockfile_fail_app";
        std::string error;
        if (!scaffold_project(app_root, "lockfile_fail_app", &error)) {
            fail_test("failed install lockfile", error);
        } else {
            ProjectManifest manifest;
            manifest.name = "lockfile_fail_app";
            manifest.version = "1.0.0";
            DependencySpec dep;
            dep.name = "missing_pkg";
            dep.source = DependencySourceKind::Registry;
            dep.constraint = *VersionConstraint::parse("^9.9.9");
            manifest.dependencies["missing_pkg"] = dep;
            const ProjectPaths paths = project_paths_from_root(app_root);
            write_manifest_file(paths.manifest.string(), manifest, &error);
            set_registry_env(temp_root / "registry");
            InstallOptions options;
            if (install_project(paths, options, &error)) {
                fail_test("failed install lockfile", "expected install failure");
            } else if (std::filesystem::exists(paths.lockfile)) {
                fail_test("failed install lockfile", "lockfile written after failure");
            } else {
                pass_test("failed install lockfile");
            }
        }
    }

    {
        const std::filesystem::path registry_root = temp_root / "registry_scoped";
        std::filesystem::create_directories(registry_root, ec);
        const std::filesystem::path scoped_src = temp_root / "scoped_pkg_src";
        std::string error;
        if (!scaffold_project(scoped_src, "scoped_pkg", &error)) {
            fail_test("scoped fixture scaffold", error);
        }
        ProjectManifest scoped_manifest;
        scoped_manifest.name = "@emir/utils";
        scoped_manifest.version = "1.0.0";
        write_manifest_file((scoped_src / "pscript.toml").string(), scoped_manifest, &error);
        if (!registry_publish_package(registry_root.string(), "@emir/utils", SemVer{1, 0, 0}, scoped_src.string(),
                                      "token-emir", &error)) {
            fail_test("scoped registry publish", error);
        } else {
            pass_test("scoped registry publish");
        }
        if (!registry_publish_package(registry_root.string(), "@ahmet/utils", SemVer{1, 0, 0}, fixtures.string(),
                                      "token-ahmet", &error)) {
            fail_test("different username same package name", error);
        } else {
            pass_test("different username same package name");
        }
        if (registry_publish_package(registry_root.string(), "@emir/utils", SemVer{1, 0, 0}, scoped_src.string(),
                                     "wrong-token", &error)) {
            fail_test("scoped publish wrong token", "should fail");
        } else {
            pass_test("scoped publish wrong token");
        }
        const auto versions = registry_list_versions(registry_root.string(), "@emir/utils", &error);
        if (versions.empty()) {
            fail_test("scoped registry list", error);
        } else {
            pass_test("scoped registry list");
        }
    }

    {
        const std::filesystem::path app_root = temp_root / "remove_dep_app";
        std::string error;
        if (!scaffold_project(app_root, "remove_dep_app", &error)) {
            fail_test("remove project dependency", error);
        } else {
            ProjectManifest manifest;
            manifest.name = "remove_dep_app";
            manifest.version = "1.0.0";
            DependencySpec keep;
            keep.name = "greet_lib";
            keep.source = DependencySourceKind::Path;
            keep.path = std::filesystem::absolute(fixtures).lexically_normal().string();
            keep.constraint.raw = "1.0.0";
            manifest.dependencies["greet_lib"] = keep;
            DependencySpec drop;
            drop.name = "extra_lib";
            drop.source = DependencySourceKind::Path;
            drop.path = std::filesystem::absolute(fixtures).lexically_normal().string();
            drop.constraint.raw = "1.0.0";
            manifest.dependencies["extra_lib"] = drop;
            const ProjectPaths paths = project_paths_from_root(app_root);
            write_manifest_file(paths.manifest.string(), manifest, &error);
            InstallOptions options;
            if (!install_project(paths, options, &error)) {
                fail_test("remove project dependency", error);
            } else if (!remove_project_dependency(paths, "extra_lib", &error)) {
                fail_test("remove project dependency", error);
            } else {
                const auto lock = load_lockfile(paths.lockfile.string(), &error);
                const auto manifest_after = load_manifest_file(paths.manifest.string(), &error);
                if (!lock || !manifest_after || manifest_after->dependencies.count("extra_lib") != 0 ||
                    manifest_after->dependencies.count("greet_lib") == 0 ||
                    find_locked_package(*lock, "extra_lib") != nullptr ||
                    find_locked_package(*lock, "greet_lib") == nullptr) {
                    fail_test("remove project dependency", "manifest or lockfile inconsistent");
                } else {
                    pass_test("remove project dependency");
                }
            }
        }
    }

    {
        const std::filesystem::path app_root = temp_root / "remove_rollback_app";
        std::string error;
        if (!scaffold_project(app_root, "remove_rollback_app", &error)) {
            fail_test("remove metadata rollback", error);
        } else {
            ProjectManifest manifest;
            manifest.name = "remove_rollback_app";
            manifest.version = "1.0.0";
            DependencySpec keep;
            keep.name = "greet_lib";
            keep.source = DependencySourceKind::Path;
            keep.path = std::filesystem::absolute(fixtures).lexically_normal().string();
            keep.constraint.raw = "1.0.0";
            manifest.dependencies["greet_lib"] = keep;
            DependencySpec drop;
            drop.name = "extra_lib";
            drop.source = DependencySourceKind::Path;
            drop.path = std::filesystem::absolute(fixtures).lexically_normal().string();
            drop.constraint.raw = "1.0.0";
            manifest.dependencies["extra_lib"] = drop;
            const ProjectPaths paths = project_paths_from_root(app_root);
            write_manifest_file(paths.manifest.string(), manifest, &error);
            std::ofstream sentinel(app_root / "keep_me.txt", std::ios::binary | std::ios::trunc);
            sentinel << "stay";
            sentinel.close();
            InstallOptions options;
            if (!install_project(paths, options, &error)) {
                fail_test("remove metadata rollback", error);
            } else {
                const auto manifest_before = read_text_file(paths.manifest);
                const auto lock_before = read_text_file(paths.lockfile);
                if (!remove_project_dependency(paths, "not_installed", &error)) {
                    const auto manifest_after_missing = read_text_file(paths.manifest);
                    if (manifest_before != manifest_after_missing) {
                        fail_test("remove missing dependency", "manifest changed");
                    } else {
                        pass_test("remove missing dependency");
                    }
                } else {
                    fail_test("remove missing dependency", "expected failure");
                }
                error.clear();
#ifndef _WIN32
                std::filesystem::permissions(paths.lockfile, std::filesystem::perms::owner_read,
                                              std::filesystem::perm_options::replace);
                const bool remove_failed = remove_project_dependency(paths, "extra_lib", &error);
                std::filesystem::permissions(paths.lockfile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                              std::filesystem::perm_options::replace);
                const auto manifest_after = read_text_file(paths.manifest);
                const auto lock_after = read_text_file(paths.lockfile);
                if (remove_failed || manifest_before != manifest_after || lock_before != lock_after ||
                    !std::filesystem::exists(app_root / "keep_me.txt")) {
                    fail_test("remove metadata rollback", error.empty() ? "metadata changed after failed remove" : error);
                } else {
                    pass_test("remove metadata rollback");
                }
#else
                // Windows often allows writes to "read-only" files for the owner; rollback is covered on Unix.
                std::cout << "SKIP  remove metadata rollback (read-only lockfile failure not deterministic on Windows)\n";
#endif
            }
        }
    }

    {
        const std::filesystem::path app_root = temp_root / "remove_resolve_fail_app";
        std::string error;
        if (!scaffold_project(app_root, "remove_resolve_fail_app", &error)) {
            fail_test("remove resolve failure", error);
        } else {
            ProjectManifest manifest;
            manifest.name = "remove_resolve_fail_app";
            manifest.version = "1.0.0";
            DependencySpec good;
            good.name = "greet_lib";
            good.source = DependencySourceKind::Path;
            good.path = std::filesystem::absolute(fixtures).lexically_normal().string();
            good.constraint.raw = "1.0.0";
            manifest.dependencies["greet_lib"] = good;
            DependencySpec bad;
            bad.name = "broken_lib";
            bad.source = DependencySourceKind::Path;
            bad.path = (temp_root / "does_not_exist_dep_path").string();
            bad.constraint.raw = "1.0.0";
            manifest.dependencies["broken_lib"] = bad;
            const ProjectPaths paths = project_paths_from_root(app_root);
            write_manifest_file(paths.manifest.string(), manifest, &error);
            const auto manifest_before = read_text_file(paths.manifest);
            if (remove_project_dependency(paths, "greet_lib", &error)) {
                fail_test("remove resolve failure", "expected resolver failure");
            } else if (read_text_file(paths.manifest) != manifest_before) {
                fail_test("remove resolve failure", "manifest changed on failed resolve");
            } else {
                pass_test("remove resolve failure");
            }
        }
    }

    {
        const std::filesystem::path atomic_path = temp_root / "atomic_write.txt";
        std::string write_error;
        if (!write_file_atomically(atomic_path, "hello-atomic", &write_error)) {
            fail_test("atomic file write", write_error);
        } else {
            const auto content = read_text_file(atomic_path);
            if (!content || *content != "hello-atomic") {
                fail_test("atomic file write", "unexpected content");
            } else {
                pass_test("atomic file write");
            }
        }
    }

    {
        const std::filesystem::path pack_root = temp_root / "pack_cli_fixture";
        std::string error;
        if (!scaffold_project(pack_root, "pack_fixture", &error)) {
            fail_test("pack_project archive", error);
        } else {
            const ProjectPaths paths = project_paths_from_root(pack_root);
            PackOptions options;
            PackResult result;
            if (!pack_project(paths, options, result, &error)) {
                fail_test("pack_project archive", error);
            } else if (!std::filesystem::exists(result.archive_path) || result.file_count == 0 ||
                       result.checksum.empty()) {
                fail_test("pack_project archive", "missing archive or metadata");
            } else {
                pass_test("pack_project archive");
            }
        }
    }

    extern int run_registry_credentials_tests();
    g_failed += run_registry_credentials_tests();

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All package tests passed\n";
    return 0;
}
