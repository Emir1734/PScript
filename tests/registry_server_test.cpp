#include "package/json_util.h"
#include "package/package_archive.h"
#include "package/package_identity.h"
#include "package/packaging.h"
#include "registry/db/registry_db.h"
#include "registry/http/registry_router.h"
#include "registry/registry_config.h"
#include "registry/auth/auth_service.h"
#include "registry/auth/password_hash.h"
#include "registry/service/registry_service.h"
#include "runtime/crypto_kdf_cipher.h"
#include "runtime/http_ops.h"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

std::uint32_t process_id_for_tests() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::filesystem::path make_temp_registry_root() {
    const auto root = std::filesystem::temp_directory_path() / ("pscript_registry_test_" +
                                                                 std::to_string(process_id_for_tests()) + "_" +
                                                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

pscript::registry::AuthSettings fast_auth_settings() {
    pscript::registry::AuthSettings settings;
    settings.argon2_params.memory_kib = 4096;
    settings.argon2_params.iterations = 1;
    settings.argon2_params.parallelism = 1;
    settings.register_rate_limit = 100;
    settings.login_rate_limit = 100;
    return settings;
}

std::unique_ptr<pscript::registry::RegistryService> make_service(const std::filesystem::path& root,
                                                               const pscript::registry::AuthSettings& auth_settings,
                                                               std::string* error) {
    const std::string db_url = "sqlite:" + (root / "registry.db").string();
    std::unique_ptr<pscript::registry::RegistryDatabase> database = pscript::registry::make_registry_database(db_url);
    pscript::registry::BlobStore blobs(root / "blobs");
    auto service =
        std::make_unique<pscript::registry::RegistryService>(std::move(database), std::move(blobs), auth_settings);
    if (!service->initialize(error)) {
        return nullptr;
    }
    return service;
}

pscript::HttpServerRequest make_post_json(const std::string& path, const std::string& json,
                                          const std::string& bearer = {}) {
    pscript::HttpServerRequest request;
    request.method = "POST";
    request.path = path;
    request.body.assign(json.begin(), json.end());
    if (!bearer.empty()) {
        request.headers["authorization"] = "Bearer " + bearer;
    }
    return request;
}

std::string extract_token(const pscript::registry::HttpResponseBody& response) {
    const std::string body(response.body.begin(), response.body.end());
    const std::size_t key = body.find("\"token\"");
    if (key == std::string::npos) {
        return {};
    }
    const std::size_t quote = body.find('"', body.find(':', key) + 1);
    const std::size_t end = body.find('"', quote + 1);
    if (quote == std::string::npos || end == std::string::npos) {
        return {};
    }
    return body.substr(quote + 1, end - quote - 1);
}

void expire_all_sessions(const std::filesystem::path& db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        return;
    }
    sqlite3_exec(db, "UPDATE auth_sessions SET expires_at='2000-01-01T00:00:00Z'", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

pscript::HttpServerRequest make_get(const std::string& path) {
    pscript::HttpServerRequest request;
    request.method = "GET";
    request.path = path;
    return request;
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>* out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "failed to read file";
        }
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    if (size < 0) {
        if (error) {
            *error = "failed to read file size";
        }
        return false;
    }
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out->data()), size)) {
        if (error) {
            *error = "failed to read file";
        }
        return false;
    }
    return true;
}

pscript::HttpServerRequest make_post_publish(const std::string& path, const std::vector<std::uint8_t>& archive,
                                             const std::string& version, const std::string& bearer) {
    pscript::HttpServerRequest request;
    request.method = "POST";
    request.path = path;
    request.body = archive;
    request.headers["content-type"] = "application/gzip";
    request.headers["x-pscript-version"] = version;
    if (!bearer.empty()) {
        request.headers["authorization"] = "Bearer " + bearer;
    }
    return request;
}

}  // namespace

int main() {
    using namespace pscript::registry;
    using namespace pscript::package;

    {
        RegistryConfig config;
        config.bind_host = "0.0.0.0";
        config.allow_public_bind = false;
        std::string error;
        if (validate_registry_config(config, &error)) {
            fail_test("config public bind rejected", "expected failure");
        } else {
            pass_test("config public bind rejected");
        }
    }

    {
        RegistryConfig config;
        config.database_url = "sqlite:test.db";
        config.storage_dir = "blobs";
        config.bind_host = "127.0.0.1";
        std::string error;
        if (!validate_registry_config(config, &error)) {
            fail_test("config valid localhost", error);
        } else {
            pass_test("config valid localhost");
        }
    }

    {
        if (!is_valid_username("alice") || is_valid_username("1bad")) {
            fail_test("username validation", "unexpected");
        } else {
            pass_test("username validation");
        }
        ScopedPackageId id{"alice", "demo"};
        if (id.canonical() != "@alice/demo") {
            fail_test("package identity canonical", id.canonical());
        } else {
            pass_test("package identity canonical");
        }
    }

    const bool argon2_available = pscript::crypto_kdf_argon2id_available();
    std::string error;

    if (argon2_available) {
        pscript::CryptoKdfArgon2Params params;
        params.memory_kib = 4096;
        params.iterations = 1;
        params.parallelism = 1;
        const auto hash = pscript::registry::hash_password_argon2id("correct horse battery", params, &error);
        if (!hash || !pscript::registry::verify_password_argon2id("correct horse battery", *hash) ||
            pscript::registry::verify_password_argon2id("wrong password here", *hash)) {
            fail_test("password hash roundtrip", error);
        } else {
            pass_test("password hash roundtrip");
        }
        if (pscript::registry::verify_password_argon2id("x", "$argon2id$v=19$m=4096,t=1,p=1$invalid")) {
            fail_test("malformed hash rejected", "accepted");
        } else {
            pass_test("malformed hash rejected");
        }
    }

    const std::filesystem::path root = make_temp_registry_root();
    std::unique_ptr<RegistryService> service = make_service(root, fast_auth_settings(), &error);
    if (!service) {
        fail_test("service init", error);
        return 1;
    }

    {
        if (!service->initialize(&error)) {
            fail_test("migration idempotent", error);
        } else {
            pass_test("migration idempotent");
        }
    }

    {
        RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        const auto response = router.handle(make_get("/health"), "test-client");
        if (response.status != 200) {
            fail_test("health endpoint", std::to_string(response.status));
        } else {
            pass_test("health endpoint");
        }
    }

    {
        const std::filesystem::path pkg_root = root / "pkg_src";
        std::filesystem::create_directories(pkg_root / "src");
        std::ofstream(pkg_root / "pscript.toml") << "[package]\nname=\"@devuser/demo\"\nversion=\"1.0.0\"\n";
        const std::filesystem::path archive = root / "demo-1.0.0.tar.gz";
        if (!create_directory_tar_gz(pkg_root, archive, &error)) {
            fail_test("ingest archive create", error);
        } else {
            ScopedPackageId package_id{"devuser", "demo"};
            if (!service->ingest_version(package_id, SemVer{1, 0, 0}, archive, &error)) {
                fail_test("ingest version", error);
            } else {
                pass_test("ingest version");
                if (service->ingest_version(package_id, SemVer{1, 0, 0}, archive, &error)) {
                    fail_test("duplicate version rejected", "second ingest succeeded");
                } else {
                    pass_test("duplicate version rejected");
                }
            }
        }
    }

    {
        RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        const auto search = router.handle(make_get("/v1/search?q=demo"), "test-client");
        if (search.status != 200) {
            fail_test("search endpoint", std::to_string(search.status));
        } else {
            const std::string body(search.body.begin(), search.body.end());
            if (body.find("@devuser/demo") == std::string::npos) {
                fail_test("search endpoint", body);
            } else {
                pass_test("search endpoint");
            }
        }
        const auto versions = router.handle(make_get("/v1/packages/devuser/demo/versions"), "test-client");
        if (versions.status != 200) {
            fail_test("versions endpoint", std::to_string(versions.status));
        } else {
            const std::string body(versions.body.begin(), versions.body.end());
            if (body.find("1.0.0") == std::string::npos) {
                fail_test("versions endpoint", body);
            } else {
                pass_test("versions endpoint");
            }
        }
        const auto download = router.handle(make_get("/v1/packages/devuser/demo/1.0.0/download"), "test-client");
        if (download.status != 200 || download.body.empty()) {
            fail_test("download endpoint", std::to_string(download.status));
        } else {
            pass_test("download endpoint");
        }
    }

    {
        RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        const auto response = router.handle(make_post_json("/v1/packages/devuser/demo/publish", "{}"), "test-client");
        if (response.status != 401) {
            fail_test("publish requires auth", std::to_string(response.status));
        } else {
            pass_test("publish requires auth");
        }
    }

    if (argon2_available) {
        const std::filesystem::path pub_src = root / "pub_src";
        std::filesystem::create_directories(pub_src / "src");
        std::ofstream(pub_src / "pscript.toml") << "[package]\nname=\"@pubuser/widget\"\nversion=\"1.0.0\"\n";
        const std::filesystem::path pub_archive = root / "pub-widget-1.0.0.tar.gz";
        std::vector<std::uint8_t> pub_bytes;
        if (!create_directory_tar_gz(pub_src, pub_archive, &error)) {
            fail_test("publish archive create", error);
        } else if (!read_file_bytes(pub_archive, &pub_bytes, &error)) {
            fail_test("publish archive read", error);
        } else {
            RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
            router.handle(make_post_json("/v1/auth/register",
                                         R"({"username":"pubuser","password":"correct horse battery"})"),
                          "pub-client");
            const auto login = router.handle(
                make_post_json("/v1/auth/login", R"({"username":"pubuser","password":"correct horse battery"})"),
                "pub-client");
            const std::string pub_token = extract_token(login);
            if (login.status != 200 || pub_token.empty()) {
                fail_test("publish login", std::to_string(login.status));
            } else {
                const auto published = router.handle(
                    make_post_publish("/v1/packages/pubuser/widget/publish", pub_bytes, "1.0.0", pub_token),
                    "pub-client");
                if (published.status != 201) {
                    fail_test("publish authenticated", std::to_string(published.status));
                } else {
                    pass_test("publish authenticated");
                    const auto duplicate = router.handle(
                        make_post_publish("/v1/packages/pubuser/widget/publish", pub_bytes, "1.0.0", pub_token),
                        "pub-client");
                    if (duplicate.status != 409) {
                        fail_test("publish duplicate version", std::to_string(duplicate.status));
                    } else {
                        pass_test("publish duplicate version");
                    }
                }
            }
        }
    }

    if (argon2_available) {
        AuthTokenResponse direct_token;
        if (service->auth().register_user("direct-client", "carol", "correct horse battery", &direct_token, &error) !=
            AuthError::Ok) {
            fail_test("auth service register", error);
        } else if (!service->auth().authenticate_bearer(direct_token.token, &error)) {
            fail_test("auth service bearer", error);
        } else {
            pass_test("auth service bearer");
        }

        RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        const auto register_response =
            router.handle(make_post_json("/v1/auth/register",
                                       R"({"username":"alice","password":"correct horse battery"})"),
                          "auth-client");
        if (register_response.status != 201) {
            fail_test("auth register", std::to_string(register_response.status));
        } else {
            pass_test("auth register");
        }
        const auto duplicate = router.handle(
            make_post_json("/v1/auth/register", R"({"username":"Alice","password":"correct horse battery"})"),
            "auth-client");
        if (duplicate.status != 409) {
            fail_test("auth duplicate username", std::to_string(duplicate.status));
        } else {
            pass_test("auth duplicate username");
        }
        const auto bad_login = router.handle(
            make_post_json("/v1/auth/login", R"({"username":"alice","password":"wrong password here"})"), "auth-client");
        if (bad_login.status != 401) {
            fail_test("auth invalid login", std::to_string(bad_login.status));
        } else {
            pass_test("auth invalid login");
        }
        const auto login = router.handle(
            make_post_json("/v1/auth/login", R"({"username":"alice","password":"correct horse battery"})"),
            "auth-client");
        const std::string token = extract_token(login);
        if (login.status != 200 || token.empty()) {
            fail_test("auth login", std::to_string(login.status));
        } else {
            pass_test("auth login");
            pscript::HttpServerRequest me_request = make_get("/v1/auth/me");
            me_request.headers["authorization"] = "Bearer " + token;
            const auto me = router.handle(me_request, "auth-client");
            if (me.status != 200) {
                fail_test("auth me", std::to_string(me.status));
            } else {
                pass_test("auth me");
            }
            const auto logout = router.handle(make_post_json("/v1/auth/logout", "{}", token), "auth-client");
            if (logout.status != 204) {
                fail_test("auth logout", std::to_string(logout.status));
            } else {
                pass_test("auth logout");
                const auto me_after = router.handle(me_request, "auth-client");
                if (me_after.status != 401) {
                    fail_test("auth revoked token", std::to_string(me_after.status));
                } else {
                    pass_test("auth revoked token");
                }
            }
        }
        const auto login_for_expire = router.handle(
            make_post_json("/v1/auth/login", R"({"username":"alice","password":"correct horse battery"})"),
            "auth-client");
        const std::string expire_token = extract_token(login_for_expire);
        expire_all_sessions(root / "registry.db");
        pscript::HttpServerRequest expired_me = make_get("/v1/auth/me");
        expired_me.headers["authorization"] = "Bearer " + expire_token;
        const auto expired = router.handle(expired_me, "auth-client");
        if (expired.status != 401) {
            fail_test("auth expired token", std::to_string(expired.status));
        } else {
            pass_test("auth expired token");
        }
        const auto oversized = router.handle(
            make_post_json("/v1/auth/login", std::string(5000, 'a')), "auth-client");
        if (oversized.status != 413 && oversized.status != 400) {
            fail_test("auth oversized body", std::to_string(oversized.status));
        } else {
            pass_test("auth oversized body");
        }
    } else {
        std::cout << "SKIP  auth tests (Argon2id unavailable in this build)\n";
        RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        const auto unavailable = router.handle(
            make_post_json("/v1/auth/register", R"({"username":"nouser","password":"correct horse battery"})"),
            "auth-client");
        if (unavailable.status != 503) {
            fail_test("auth unavailable without argon2", std::to_string(unavailable.status));
        } else {
            pass_test("auth unavailable without argon2");
        }
    }

    if (argon2_available) {
        const std::filesystem::path authz_root = make_temp_registry_root();
        std::unique_ptr<RegistryService> authz_service = make_service(authz_root, fast_auth_settings(), &error);
        if (!authz_service) {
            fail_test("authz service init", error);
        } else {
        ScopedPackageId bob_pkg{"bob", "tools"};
        const std::filesystem::path pkg_root = authz_root / "bob_pkg";
        std::filesystem::create_directories(pkg_root / "src");
        std::ofstream(pkg_root / "pscript.toml") << "[package]\nname=\"@bob/tools\"\nversion=\"1.0.0\"\n";
        const std::filesystem::path archive = authz_root / "bob-tools.tar.gz";
        if (!create_directory_tar_gz(pkg_root, archive, &error)) {
            fail_test("authz ingest archive", error);
        } else if (!authz_service->ingest_version(bob_pkg, SemVer{1, 0, 0}, archive, &error)) {
            fail_test("authz ingest", error);
        } else {
            RegistryRouter router(*authz_service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
            router.handle(make_post_json("/v1/auth/register",
                                         R"({"username":"alice","password":"correct horse battery"})"),
                          "authz-client");
            const auto login = router.handle(
                make_post_json("/v1/auth/login", R"({"username":"alice","password":"correct horse battery"})"),
                "authz-client");
            const std::string token = extract_token(login);
            if (login.status != 200 || token.empty()) {
                fail_test("authz login", std::to_string(login.status));
            } else {
            const auto forbidden =
                router.handle(make_post_json("/v1/packages/bob/tools/publish", "{}", token), "authz-client");
            if (forbidden.status != 403) {
                fail_test("authz cross account publish", std::to_string(forbidden.status));
            } else {
                pass_test("authz cross account publish");
            }
            }
        }
        }
        authz_service.reset();
        std::error_code authz_remove_ec;
        std::filesystem::remove_all(authz_root, authz_remove_ec);
    }

    if (argon2_available) {
        const std::filesystem::path rate_root = make_temp_registry_root();
        pscript::registry::AuthSettings rate_settings = fast_auth_settings();
        rate_settings.register_rate_limit = 2;
        std::unique_ptr<RegistryService> rate_service = make_service(rate_root, rate_settings, &error);
        if (!rate_service) {
            fail_test("rate limit service init", error);
        } else {
        RegistryRouter router(*rate_service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        router.handle(make_post_json("/v1/auth/register",
                                     R"({"username":"user1","password":"correct horse battery"})"),
                      "rate-client");
        router.handle(make_post_json("/v1/auth/register",
                                     R"({"username":"user2","password":"correct horse battery"})"),
                      "rate-client");
        const auto limited = router.handle(make_post_json("/v1/auth/register",
                                                          R"({"username":"user3","password":"correct horse battery"})"),
                                           "rate-client");
        if (limited.status != 429) {
            fail_test("auth rate limit", std::to_string(limited.status));
        } else {
            pass_test("auth rate limit");
        }
        rate_service.reset();
        std::error_code rate_remove_ec;
        std::filesystem::remove_all(rate_root, rate_remove_ec);
        }
    }

    if (argon2_available) {
        RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);
        const std::string special_password = "quote\"backslash\\ok!12";
        const auto special_register = router.handle(
            make_post_json("/v1/auth/register", json_auth_credentials_body("jsonuser", special_password)), "json-client");
        const auto special_login = router.handle(
            make_post_json("/v1/auth/login", json_auth_credentials_body("jsonuser", special_password)), "json-client");
        if (special_register.status != 201 || special_login.status != 200) {
            fail_test("auth json special password",
                      std::to_string(special_register.status) + "/" + std::to_string(special_login.status));
        } else {
            pass_test("auth json special password");
        }
    }

    extern int run_registry_integration_tests();
    g_failed += run_registry_integration_tests();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "all registry tests passed\n";
    return 0;
}
