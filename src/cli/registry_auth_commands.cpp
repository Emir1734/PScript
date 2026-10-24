#include "cli/registry_auth_commands.h"

#include "cli/exit_codes.h"
#include "cli/pipeline.h"
#include "package/registry.h"
#include "package/project.h"
#include "package/registry_client.h"
#include "package/registry_credentials.h"

#include <iostream>
#include <string>

namespace pscript {
namespace {

std::string read_password_line() {
    std::string password;
    std::getline(std::cin, password);
    return password;
}

int cmd_register(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() != 1) {
        print_cli_error(err, "register requires a username");
        return kExitUsage;
    }
    const std::string registry_url = package::default_registry_url();
    if (registry_url.rfind("http://", 0) != 0 && registry_url.rfind("https://", 0) != 0) {
        print_cli_error(err, "register requires a remote HTTP registry (set PSCRIPT_REGISTRY)");
        return kExitUsage;
    }
    err << "Password: ";
    const std::string password = read_password_line();
    package::RegistryAuthToken token;
    std::string error;
    if (!package::registry_remote_register(registry_url, args[0], password, &token, &error)) {
        print_cli_error(err, error.empty() ? "registration failed" : error);
        return kExitCompile;
    }
    package::RegistryCredentials credentials;
    credentials.registry_url = registry_url;
    credentials.username = token.username;
    credentials.token = token.token;
    if (!package::save_registry_credentials(credentials, &error)) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    out << "Registered as " << token.username << '\n';
    return kExitSuccess;
}

int cmd_login(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() != 1) {
        print_cli_error(err, "login requires a username");
        return kExitUsage;
    }
    const std::string registry_url = package::default_registry_url();
    if (registry_url.rfind("http://", 0) != 0 && registry_url.rfind("https://", 0) != 0) {
        print_cli_error(err, "login requires a remote HTTP registry (set PSCRIPT_REGISTRY)");
        return kExitUsage;
    }
    err << "Password: ";
    const std::string password = read_password_line();
    package::RegistryAuthToken token;
    std::string error;
    if (!package::registry_remote_login(registry_url, args[0], password, &token, &error)) {
        print_cli_error(err, error.empty() ? "login failed" : error);
        return kExitCompile;
    }
    package::RegistryCredentials credentials;
    credentials.registry_url = registry_url;
    credentials.username = token.username;
    credentials.token = token.token;
    if (!package::save_registry_credentials(credentials, &error)) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    out << "Logged in as " << token.username << '\n';
    return kExitSuccess;
}

int cmd_logout(std::ostream& out, std::ostream& err) {
    const std::string registry_url = package::default_registry_url();
    std::string error;
    if (const std::optional<package::RegistryCredentials> credentials = package::load_registry_credentials(&error)) {
        if (!package::registry_url_matches_configured(credentials->registry_url, &error)) {
            print_cli_error(err, error);
            return kExitCompile;
        }
        package::registry_remote_logout(registry_url, credentials->token, nullptr);
    }
    if (!package::clear_registry_credentials(&error)) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    out << "Logged out\n";
    return kExitSuccess;
}

int cmd_whoami(std::ostream& out, std::ostream& err) {
    const std::string registry_url = package::default_registry_url();
    std::string error;
    const std::optional<package::RegistryCredentials> credentials = package::load_registry_credentials(&error);
    if (!credentials) {
        print_cli_error(err, "not logged in");
        return kExitCompile;
    }
    if (!package::registry_url_matches_configured(credentials->registry_url, &error)) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    std::string username;
    if (!package::registry_remote_whoami(registry_url, credentials->token, &username, &error)) {
        print_cli_error(err, error.empty() ? "not authenticated" : error);
        return kExitCompile;
    }
    out << username << '\n';
    return kExitSuccess;
}

int cmd_search(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        print_cli_error(err, "search requires a query");
        return kExitUsage;
    }
    const std::string registry_url = package::default_registry_url();
    std::string error;
    const std::vector<std::string> results = package::registry_search_packages(registry_url, args[0], 20, &error);
    if (!error.empty() && results.empty()) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    for (const std::string& entry : results) {
        out << entry << '\n';
    }
    return kExitSuccess;
}

}  // namespace

bool is_registry_auth_command(std::string_view name) {
    return name == "register" || name == "login" || name == "logout" || name == "whoami" || name == "search";
}

int run_registry_auth_command(std::string_view command, const std::vector<std::string>& args, std::ostream& out,
                              std::ostream& err) {
    if (command == "register") {
        return cmd_register(args, out, err);
    }
    if (command == "login") {
        return cmd_login(args, out, err);
    }
    if (command == "logout") {
        return cmd_logout(out, err);
    }
    if (command == "whoami") {
        return cmd_whoami(out, err);
    }
    if (command == "search") {
        return cmd_search(args, out, err);
    }
    print_cli_error(err, "unknown registry auth command");
    return kExitUsage;
}

}  // namespace pscript
