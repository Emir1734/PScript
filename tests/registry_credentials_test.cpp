#include "package/json_util.h"
#include "package/registry_credentials.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#endif

namespace {

using pscript::package::RegistryCredentials;
using pscript::package::clear_registry_credentials;
using pscript::package::load_registry_credentials;
using pscript::package::registry_credentials_path;
using pscript::package::save_registry_credentials;

int local_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++local_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

class CredentialsEnvGuard {
public:
    explicit CredentialsEnvGuard(const std::filesystem::path& path) : name_("PSCRIPT_REGISTRY_CREDENTIALS_FILE") {
        previous_ = std::getenv(name_);
#ifdef _WIN32
        _putenv_s(name_, path.string().c_str());
#else
        setenv(name_, path.string().c_str(), 1);
#endif
    }
    ~CredentialsEnvGuard() {
#ifdef _WIN32
        if (previous_) {
            _putenv_s(name_, previous_);
        } else {
            _putenv_s(name_, "");
        }
#else
        if (previous_) {
            setenv(name_, previous_, 1);
        } else {
            unsetenv(name_);
        }
#endif
    }

private:
    const char* name_;
    const char* previous_;
};

}  // namespace

int run_registry_credentials_tests() {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "pscript_cred_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    const std::filesystem::path cred_path = temp_root / "credentials.json";

    {
        CredentialsEnvGuard guard(cred_path);
        RegistryCredentials sample;
        sample.registry_url = "http://127.0.0.1:8741";
        sample.username = "alice";
        sample.token = "pscr_" + std::string(64, 'a');
        std::string error;
        if (!save_registry_credentials(sample, &error)) {
            fail_test("credentials save", error);
            return local_failed;
        }
        pass_test("credentials save");

        const std::optional<RegistryCredentials> loaded = load_registry_credentials(&error);
        if (!loaded || loaded->token != sample.token || loaded->username != sample.username ||
            loaded->registry_url != sample.registry_url) {
            fail_test("credentials load roundtrip", "mismatch");
        } else {
            pass_test("credentials load roundtrip");
        }

        std::ifstream raw(cred_path, std::ios::binary);
        const std::string file_body((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
        raw.close();
        if (file_body.find(sample.token) == std::string::npos) {
            fail_test("credentials token stored", "missing");
        } else if (file_body.find("Bearer") != std::string::npos) {
            fail_test("credentials file content", "unexpected bearer prefix");
        } else {
            pass_test("credentials file stores token");
        }

        std::ofstream(cred_path, std::ios::trunc) << "{not json";
        if (load_registry_credentials(&error)) {
            fail_test("credentials malformed rejected", "accepted");
        } else {
            pass_test("credentials malformed rejected");
        }

        std::ofstream(cred_path, std::ios::trunc) << "{\"token\":\"only\"}";
        if (load_registry_credentials(&error)) {
            fail_test("credentials missing fields rejected", "accepted");
        } else {
            pass_test("credentials missing fields rejected");
        }

        if (registry_credentials_path() != cred_path) {
            fail_test("credentials path override", "env not honored");
        } else {
            pass_test("credentials path override");
        }

        if (!clear_registry_credentials(&error)) {
            fail_test("credentials clear", error);
        } else if (std::filesystem::exists(cred_path)) {
            fail_test("credentials clear", "file remains");
        } else {
            pass_test("credentials clear");
        }
    }

    {
        const std::filesystem::path cred_path_escaped = temp_root / "credentials-escaped.json";
        CredentialsEnvGuard guard(cred_path_escaped);
        RegistryCredentials sample;
        sample.registry_url = "http://127.0.0.1:8741";
        sample.username = "bob";
        sample.token = "pscr_\"\\special\\token";
        std::string error;
        if (!save_registry_credentials(sample, &error)) {
            fail_test("credentials save escaped token", error);
        } else {
            const std::optional<RegistryCredentials> escaped_loaded = load_registry_credentials(&error);
            if (!escaped_loaded || escaped_loaded->token != sample.token) {
                fail_test("credentials load escaped token", "mismatch");
            } else {
                pass_test("credentials load escaped token");
            }
            clear_registry_credentials(&error);
        }
    }

    std::filesystem::remove_all(temp_root, ec);
    return local_failed;
}
