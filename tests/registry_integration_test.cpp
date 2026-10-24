#include "package/checksum.h"
#include "package/json_util.h"
#include "package/package_archive.h"
#include "package/packaging.h"
#include "package/project.h"
#include "package/registry_credentials.h"
#include "registry/db/registry_db.h"
#include "registry/http/registry_router.h"
#include "registry/service/registry_service.h"
#include "runtime/crypto_kdf_cipher.h"
#include "runtime/http_ops.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using pscript::package::json_auth_credentials_body;

int local_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++local_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
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

pscript::HttpServerRequest make_get(const std::string& path) {
    pscript::HttpServerRequest request;
    request.method = "GET";
    request.path = path;
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

bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>* out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "read failed";
        }
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

pscript::registry::AuthSettings fast_auth_settings() {
    pscript::registry::AuthSettings settings;
    settings.argon2_params.memory_kib = 4096;
    settings.argon2_params.iterations = 1;
    settings.argon2_params.parallelism = 1;
    return settings;
}

std::unique_ptr<pscript::registry::RegistryService> make_service(const std::filesystem::path& root,
                                                                 std::string* error) {
    const std::string db_url = "sqlite:" + (root / "registry.db").string();
    std::unique_ptr<pscript::registry::RegistryDatabase> database = pscript::registry::make_registry_database(db_url);
    pscript::registry::BlobStore blobs(root / "blobs");
    auto service =
        std::make_unique<pscript::registry::RegistryService>(std::move(database), std::move(blobs), fast_auth_settings());
    if (!service->initialize(error)) {
        return nullptr;
    }
    return service;
}

}  // namespace

int run_registry_integration_tests() {
    if (!pscript::crypto_kdf_argon2id_available()) {
        std::cout << "SKIP  registry integration (Argon2id unavailable)\n";
        return 0;
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "pscript_registry_e2e";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    std::string error;
    std::unique_ptr<pscript::registry::RegistryService> service = make_service(root, &error);
    if (!service) {
        fail_test("integration service", error);
        return local_failed;
    }
    pscript::registry::RegistryRouter router(*service, "/v1", 20, 4096, 64ULL * 1024 * 1024);

    const std::string password = "correct \"horse\" \\battery!";
    const auto register_response = router.handle(
        make_post_json("/v1/auth/register", json_auth_credentials_body("e2euser", password)), "e2e-client");
    const std::string token = extract_token(register_response);
    if (register_response.status != 201 || token.empty()) {
        fail_test("integration register", std::to_string(register_response.status));
        return local_failed;
    }
    pass_test("integration register");

    pscript::HttpServerRequest me_request = make_get("/v1/auth/me");
    me_request.headers["authorization"] = "Bearer " + token;
    const auto me_auth = router.handle(me_request, "e2e-client");
    if (me_auth.status != 200) {
        fail_test("integration whoami", std::to_string(me_auth.status));
    } else {
        pass_test("integration whoami");
    }

    pscript::package::RegistryCredentials saved;
    saved.registry_url = "http://127.0.0.1:8741";
    saved.username = "e2euser";
    saved.token = token;
#ifdef _WIN32
    _putenv_s("PSCRIPT_REGISTRY_CREDENTIALS_FILE", (root / "creds.json").string().c_str());
#else
    setenv("PSCRIPT_REGISTRY_CREDENTIALS_FILE", (root / "creds.json").string().c_str(), 1);
#endif
    if (!pscript::package::save_registry_credentials(saved, &error)) {
        fail_test("integration credentials save", error);
    }

    const std::filesystem::path pkg_root = root / "publisher";
    std::filesystem::create_directories(pkg_root / "src");
    std::ofstream(pkg_root / "pscript.toml")
        << "[package]\nname=\"@e2euser/widget\"\nversion=\"1.0.0\"\n\n[project]\nname=\"widget\"\nversion=\"1.0.0\"\n";
    std::ofstream(pkg_root / "src" / "widget.phs") << "pub func ping():\n    return 1\n";
    const std::filesystem::path archive = root / "widget-1.0.0.tar.gz";
    if (!pscript::package::create_directory_tar_gz(pkg_root, archive, &error)) {
        fail_test("integration archive", error);
        return local_failed;
    }
    std::vector<std::uint8_t> pub_bytes;
    if (!read_file_bytes(archive, &pub_bytes, &error)) {
        fail_test("integration archive read", error);
        return local_failed;
    }
    const auto published = router.handle(
        make_post_publish("/v1/packages/e2euser/widget/publish", pub_bytes, "1.0.0", token), "e2e-client");
    if (published.status != 201) {
        fail_test("integration publish", std::to_string(published.status));
    } else {
        pass_test("integration publish");
    }

    const auto search = router.handle(make_get("/v1/search?q=widget"), "e2e-client");
    if (search.status != 200) {
        fail_test("integration search", std::to_string(search.status));
    } else {
        pass_test("integration search");
    }

    const auto download = router.handle(make_get("/v1/packages/e2euser/widget/1.0.0/download"), "e2e-client");
    if (download.status != 200 || download.body.empty()) {
        fail_test("integration download", std::to_string(download.status));
    } else {
        pass_test("integration download");
    }

    const std::filesystem::path verify_archive = root / "verify.tar.gz";
    std::ofstream out(verify_archive, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(download.body.data()), static_cast<std::streamsize>(download.body.size()));
    out.close();
    const std::filesystem::path verify_root = root / "verify";
    std::filesystem::create_directories(verify_root);
    const std::string expected_checksum = pscript::package::sha256_directory(pkg_root.string(), &error);
    if (!pscript::package::extract_tar_gz_archive(verify_archive, verify_root, &error)) {
        fail_test("integration extract download", error);
    } else {
        const std::string downloaded_checksum = pscript::package::sha256_directory(verify_root.string(), &error);
        if (downloaded_checksum != expected_checksum) {
            fail_test("integration checksum", "mismatch");
        } else {
            pass_test("integration checksum");
        }
    }

    const auto duplicate = router.handle(
        make_post_publish("/v1/packages/e2euser/widget/publish", pub_bytes, "1.0.0", token), "e2e-client");
    if (duplicate.status != 409) {
        fail_test("integration duplicate publish", std::to_string(duplicate.status));
    } else {
        pass_test("integration duplicate publish rejected");
    }

    const auto logout = router.handle(make_post_json("/v1/auth/logout", "{}", token), "e2e-client");
    if (logout.status != 204) {
        fail_test("integration logout", std::to_string(logout.status));
    } else if (!pscript::package::clear_registry_credentials(&error)) {
        fail_test("integration logout clear", error);
    } else if (router.handle(me_request, "e2e-client").status != 401) {
        fail_test("integration revoked token", "still authorized");
    } else {
        pass_test("integration logout");
    }

    router.handle(make_post_json("/v1/auth/register", json_auth_credentials_body("intruder", password)), "e2e-client");
    const auto intruder_login = router.handle(
        make_post_json("/v1/auth/login", json_auth_credentials_body("intruder", password)), "e2e-client");
    const std::string intruder_token = extract_token(intruder_login);
    const auto forbidden = router.handle(
        make_post_publish("/v1/packages/e2euser/widget/publish", pub_bytes, "2.0.0", intruder_token), "e2e-client");
    if (forbidden.status != 403) {
        fail_test("integration cross account publish", std::to_string(forbidden.status));
    } else {
        pass_test("integration cross account publish rejected");
    }

    service.reset();
    std::filesystem::remove_all(root, ec);
    return local_failed;
}
