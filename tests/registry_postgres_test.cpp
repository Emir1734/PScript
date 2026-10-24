#include "registry/db/registry_db.h"
#include "registry/service/registry_service.h"
#include "registry/auth/auth_service.h"
#include "runtime/crypto_kdf_cipher.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

bool postgres_test_url_allowed(const std::string& url) {
    const std::size_t slash = url.rfind('/');
    if (slash == std::string::npos) {
        return false;
    }
    const std::string db_name = url.substr(slash + 1);
    return db_name.find("test") != std::string::npos || db_name.find("integration") != std::string::npos;
}

}  // namespace

int main() {
#ifndef PSCRIPT_HAS_LIBPQ
    std::cout << "SKIP  postgres tests (libpq not built)\n";
    return 0;
#else
    const char* url = std::getenv("PSCRIPT_REGISTRY_POSTGRES_TEST_URL");
    if (url == nullptr || url[0] == '\0') {
        std::cout << "SKIP  postgres tests (set PSCRIPT_REGISTRY_POSTGRES_TEST_URL to a dedicated *_test database)\n";
        return 0;
    }
    const std::string connection(url);
    if (!postgres_test_url_allowed(connection)) {
        std::cerr << "Refusing to run postgres tests: database name must contain 'test' or 'integration'\n";
        return 2;
    }
    if (!pscript::crypto_kdf_argon2id_available()) {
        std::cout << "SKIP  postgres tests (Argon2id unavailable)\n";
        return 0;
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "pscript_pg_test_blobs";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    std::string error;
    std::unique_ptr<pscript::registry::RegistryDatabase> database =
        pscript::registry::make_registry_database(connection);
    if (!database) {
        fail_test("postgres database factory", "null");
        return 1;
    }
    pscript::registry::BlobStore blobs(root);
    pscript::registry::AuthSettings auth;
    auth.argon2_params.memory_kib = 4096;
    auth.argon2_params.iterations = 1;
    auth.argon2_params.parallelism = 1;
    pscript::registry::RegistryService service(std::move(database), std::move(blobs), auth);
    if (!service.initialize(&error)) {
        fail_test("postgres service init", error);
        return 1;
    }
    pass_test("postgres migrate");

    std::uint32_t run_id = 0;
#ifdef _WIN32
    run_id = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    run_id = static_cast<std::uint32_t>(getpid());
#endif
    const std::string username = "pguser_" + std::to_string(run_id);
    pscript::registry::AuthTokenResponse token;
    if (service.auth().register_user("pg-client", username, "correct horse battery", &token, &error) !=
        pscript::registry::AuthError::Ok) {
        fail_test("postgres register", error);
    } else {
        pass_test("postgres register");
    }

    if (!service.auth().authenticate_bearer(token.token, &error)) {
        fail_test("postgres bearer", error);
    } else {
        pass_test("postgres bearer");
    }

    std::filesystem::remove_all(root, ec);
    if (g_failed != 0) {
        std::cerr << g_failed << " postgres test(s) failed\n";
        return 1;
    }
    std::cout << "all postgres registry tests passed\n";
    return 0;
#endif
}
