#include "registry/http/registry_router.h"

#include "package/package_identity.h"
#include "package/semver.h"
#include "registry/auth/auth_service.h"
#include "registry/http/json_body.h"
#include "registry/registry_json.h"
#include "runtime/http_ops.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pscript::registry {
namespace {

constexpr int kStatusOk = 200;
constexpr int kStatusCreated = 201;
constexpr int kStatusNoContent = 204;
constexpr int kStatusBadRequest = 400;
constexpr int kStatusUnauthorized = 401;
constexpr int kStatusForbidden = 403;
constexpr int kStatusNotFound = 404;
constexpr int kStatusMethodNotAllowed = 405;
constexpr int kStatusConflict = 409;
constexpr int kStatusPayloadTooLarge = 413;
constexpr int kStatusTooManyRequests = 429;
constexpr int kStatusNotImplemented = 501;
constexpr int kStatusServiceUnavailable = 503;

HttpResponseBody json_response(int status, const std::string& json) {
    HttpResponseBody response;
    response.status = status;
    response.headers["Content-Type"] = "application/json; charset=utf-8";
    response.body.assign(json.begin(), json.end());
    return response;
}

HttpResponseBody error_response(int status, const std::string& message) {
    return json_response(status, "{\"error\":" + json_string(message) + "}");
}

std::string auth_token_json(const AuthTokenResponse& token) {
    std::ostringstream out;
    out << "{\"username\":" << json_string(token.username) << ",\"token\":" << json_string(token.token)
        << ",\"expires_at\":" << json_string(token.expires_at) << '}';
    return out.str();
}

std::string strip_query(std::string_view path) {
    const std::size_t query = path.find('?');
    if (query == std::string::npos) {
        return std::string(path);
    }
    return std::string(path.substr(0, query));
}

std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    if (!path.empty() && path.front() == '/') {
        start = 1;
    }
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            if (start < path.size()) {
                parts.emplace_back(path.substr(start));
            }
            break;
        }
        if (slash > start) {
            parts.emplace_back(path.substr(start, slash - start));
        }
        start = slash + 1;
    }
    return parts;
}

std::string query_param(const std::string& path, std::string_view key) {
    const std::size_t query = path.find('?');
    if (query == std::string::npos) {
        return {};
    }
    const std::string_view query_string = std::string_view(path).substr(query + 1);
    std::size_t index = 0;
    while (index < query_string.size()) {
        const std::size_t amp = query_string.find('&', index);
        const std::string_view pair =
            query_string.substr(index, amp == std::string::npos ? std::string::npos : amp - index);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            const std::string_view name = pair.substr(0, eq);
            if (name == key) {
                return std::string(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) {
            break;
        }
        index = amp + 1;
    }
    return {};
}

int parse_limit(const std::string& raw, int default_limit, int max_limit) {
    if (raw.empty()) {
        return default_limit;
    }
    try {
        const int parsed = std::stoi(raw);
        if (parsed <= 0) {
            return default_limit;
        }
        return std::min(parsed, max_limit);
    } catch (...) {
        return default_limit;
    }
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::optional<std::string> extract_bearer_token(const pscript::HttpServerRequest& request) {
    const std::optional<std::string> header = pscript::http_header_lookup(request.headers, "Authorization");
    if (!header || header->size() < 8) {
        return std::nullopt;
    }
    constexpr std::string_view kPrefix = "Bearer ";
    if (header->compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    return header->substr(kPrefix.size());
}

std::string body_to_string(const std::vector<std::uint8_t>& body) {
    return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

}  // namespace

RegistryRouter::RegistryRouter(RegistryService& service, std::string api_prefix, int max_search_results,
                               std::size_t max_auth_body_bytes, std::size_t max_upload_bytes)
    : service_(service),
      api_prefix_(std::move(api_prefix)),
      max_search_results_(max_search_results),
      max_auth_body_bytes_(max_auth_body_bytes),
      max_upload_bytes_(max_upload_bytes) {
    if (!api_prefix_.empty() && api_prefix_.back() == '/') {
        api_prefix_.pop_back();
    }
}

HttpResponseBody RegistryRouter::handle(const pscript::HttpServerRequest& request,
                                        const std::string& client_key) const {
    const std::string path = strip_query(request.path);
    AuthService& auth = service_.auth();

    if (path == api_prefix_ + "/auth/register" && request.method == "POST") {
        if (request.body.size() > max_auth_body_bytes_) {
            return error_response(kStatusPayloadTooLarge, "request body too large");
        }
        const std::optional<AuthCredentialsJson> credentials =
            parse_auth_credentials_json(body_to_string(request.body), max_auth_body_bytes_);
        if (!credentials) {
            return error_response(kStatusBadRequest, "invalid request body");
        }
        AuthTokenResponse token;
        std::string error;
        const AuthError result =
            auth.register_user(client_key, credentials->username, credentials->password, &token, &error);
        switch (result) {
            case AuthError::Ok:
                return json_response(kStatusCreated, auth_token_json(token));
            case AuthError::UsernameTaken:
                return error_response(kStatusConflict, error.empty() ? "username already taken" : error);
            case AuthError::InvalidInput:
                return error_response(kStatusBadRequest, error.empty() ? "invalid request" : error);
            case AuthError::RateLimited:
                return error_response(kStatusTooManyRequests, "too many requests");
            case AuthError::Unavailable:
                return error_response(kStatusServiceUnavailable, error.empty() ? "registration unavailable" : error);
            default:
                return error_response(kStatusBadRequest, "invalid request");
        }
    }

    if (path == api_prefix_ + "/auth/login" && request.method == "POST") {
        if (request.body.size() > max_auth_body_bytes_) {
            return error_response(kStatusPayloadTooLarge, "request body too large");
        }
        const std::optional<AuthCredentialsJson> credentials =
            parse_auth_credentials_json(body_to_string(request.body), max_auth_body_bytes_);
        if (!credentials) {
            return error_response(kStatusBadRequest, "invalid request body");
        }
        AuthTokenResponse token;
        std::string error;
        const AuthError result = auth.login(client_key, credentials->username, credentials->password, &token, &error);
        switch (result) {
            case AuthError::Ok:
                return json_response(kStatusOk, auth_token_json(token));
            case AuthError::InvalidCredentials:
                return error_response(kStatusUnauthorized, "invalid credentials");
            case AuthError::RateLimited:
                return error_response(kStatusTooManyRequests, "too many requests");
            case AuthError::Unavailable:
                return error_response(kStatusServiceUnavailable, "login unavailable");
            default:
                return error_response(kStatusUnauthorized, "invalid credentials");
        }
    }

    if (path == api_prefix_ + "/auth/logout" && request.method == "POST") {
        const std::optional<std::string> bearer = extract_bearer_token(request);
        if (!bearer) {
            return error_response(kStatusUnauthorized, "unauthorized");
        }
        std::string auth_error;
        if (!auth.authenticate_bearer(*bearer, &auth_error)) {
            return error_response(kStatusUnauthorized, "unauthorized");
        }
        std::string error;
        if (auth.logout(*bearer, &error) != AuthError::Ok) {
            return error_response(kStatusServiceUnavailable, "logout failed");
        }
        HttpResponseBody response;
        response.status = kStatusNoContent;
        return response;
    }

    if (path == api_prefix_ + "/auth/me" && (request.method == "GET" || request.method == "HEAD")) {
        const std::optional<std::string> bearer = extract_bearer_token(request);
        if (!bearer) {
            return error_response(kStatusUnauthorized, "unauthorized");
        }
        std::string auth_error;
        const std::optional<AuthenticatedPrincipal> principal = auth.authenticate_bearer(*bearer, &auth_error);
        if (!principal) {
            return error_response(kStatusUnauthorized, "unauthorized");
        }
        HttpResponseBody response =
            json_response(kStatusOk, "{\"username\":" + json_string(principal->username) + '}');
        if (request.method == "HEAD") {
            response.body.clear();
        }
        return response;
    }

    if (request.method != "GET" && request.method != "HEAD" && request.method != "POST") {
        return error_response(kStatusMethodNotAllowed, "method not allowed");
    }

    if (path == "/health" || path == api_prefix_ + "/health") {
        std::string error;
        const RegistryHealth health = service_.health(&error);
        if (!health.ok) {
            return json_response(kStatusServiceUnavailable, "{\"status\":\"unavailable\",\"database\":\"down\"}");
        }
        return json_response(kStatusOk, "{\"status\":\"ok\",\"database\":\"ok\"}");
    }

    if (starts_with(path, api_prefix_ + "/search")) {
        const std::string q = query_param(request.path, "q");
        if (q.empty()) {
            return error_response(kStatusBadRequest, "missing search query");
        }
        const int limit = parse_limit(query_param(request.path, "limit"), 20, max_search_results_);
        std::string error;
        const std::vector<std::string> packages = service_.search(q, limit, &error);
        if (!error.empty()) {
            return error_response(kStatusServiceUnavailable, "search failed");
        }
        HttpResponseBody response = json_response(kStatusOk, json_search_results(packages));
        if (request.method == "HEAD") {
            response.body.clear();
        }
        return response;
    }

    const std::string packages_prefix = api_prefix_ + "/packages/";
    if (starts_with(path, packages_prefix)) {
        const std::vector<std::string> parts = split_path(path.substr(packages_prefix.size()));
        if (parts.size() == 3 && parts[2] == "publish" && request.method == "POST") {
            package::ScopedPackageId package_id{parts[0], parts[1]};
            if (!package::is_valid_username(package_id.username) ||
                !package::is_valid_unscoped_package_name(package_id.package)) {
                return error_response(kStatusBadRequest, "invalid package name");
            }
            const std::optional<std::string> bearer = extract_bearer_token(request);
            if (!bearer) {
                return error_response(kStatusUnauthorized, "unauthorized");
            }
            std::string auth_error;
            const std::optional<AuthenticatedPrincipal> principal = auth.authenticate_bearer(*bearer, &auth_error);
            if (!principal) {
                return error_response(kStatusUnauthorized, "unauthorized");
            }
            if (principal->username != package_id.username) {
                return error_response(kStatusForbidden, "forbidden");
            }
            if (request.body.empty()) {
                return error_response(kStatusBadRequest, "missing package archive");
            }
            if (request.body.size() > max_upload_bytes_) {
                return error_response(kStatusPayloadTooLarge, "archive too large");
            }
            const std::optional<std::string> version_header =
                pscript::http_header_lookup(request.headers, "X-PScript-Version");
            if (!version_header) {
                return error_response(kStatusBadRequest, "missing package version");
            }
            const auto version = package::SemVer::parse(*version_header);
            if (!version) {
                return error_response(kStatusBadRequest, "invalid version");
            }
            const std::filesystem::path temp_archive =
                std::filesystem::temp_directory_path() / "pscript_registry_publish_upload.tar.gz";
            {
                std::ofstream out(temp_archive, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(request.body.data()),
                          static_cast<std::streamsize>(request.body.size()));
                if (!out) {
                    return error_response(kStatusServiceUnavailable, "publish failed");
                }
            }
            RegistryService::PublishVersionResult published;
            std::string publish_error;
            const bool ok = service_.publish_version(*principal, package_id, *version, temp_archive, &published,
                                                     &publish_error);
            std::error_code remove_ec;
            std::filesystem::remove(temp_archive, remove_ec);
            if (!ok) {
                if (publish_error == "forbidden" || publish_error == "invalid package identity") {
                    return error_response(publish_error == "forbidden" ? kStatusForbidden : kStatusBadRequest,
                                          publish_error == "forbidden" ? "forbidden" : "invalid package identity");
                }
                if (publish_error == "package version already exists") {
                    return error_response(kStatusConflict, "package version already exists");
                }
                if (publish_error == "archive exceeds upload size limit") {
                    return error_response(kStatusPayloadTooLarge, "archive too large");
                }
                return error_response(kStatusBadRequest, "publish failed");
            }
            std::ostringstream json;
            json << "{\"package\":" << json_string(published.canonical_id) << ",\"version\":"
                 << json_string(published.version) << ",\"checksum_sha256\":" << json_string(published.checksum_sha256)
                 << '}';
            return json_response(kStatusCreated, json.str());
        }
        if (parts.size() == 3 && parts[2] == "versions") {
            package::ScopedPackageId package_id{parts[0], parts[1]};
            if (!package::is_valid_username(package_id.username) ||
                !package::is_valid_unscoped_package_name(package_id.package)) {
                return error_response(kStatusBadRequest, "invalid package name");
            }
            std::string error;
            const std::vector<std::string> versions = service_.list_versions(package_id, &error);
            if (!error.empty()) {
                return error_response(kStatusServiceUnavailable, "failed to list versions");
            }
            HttpResponseBody response = json_response(kStatusOk, json_string_array(versions));
            if (request.method == "HEAD") {
                response.body.clear();
            }
            return response;
        }
        if (parts.size() == 4 && parts[3] == "download") {
            package::ScopedPackageId package_id{parts[0], parts[1]};
            if (!package::is_valid_username(package_id.username) ||
                !package::is_valid_unscoped_package_name(package_id.package)) {
                return error_response(kStatusBadRequest, "invalid package name");
            }
            const auto version = package::SemVer::parse(parts[2]);
            if (!version) {
                return error_response(kStatusBadRequest, "invalid version");
            }
            std::vector<std::uint8_t> bytes;
            std::string error;
            if (!service_.read_version_archive(package_id, *version, &bytes, &error)) {
                if (error == "package version not found") {
                    return error_response(kStatusNotFound, "package version not found");
                }
                if (error == "package archive checksum mismatch") {
                    return error_response(kStatusServiceUnavailable, "package integrity check failed");
                }
                return error_response(kStatusNotFound, "package version not found");
            }
            HttpResponseBody response;
            response.status = kStatusOk;
            response.headers["Content-Type"] = "application/gzip";
            response.headers["Content-Disposition"] = "attachment; filename=\"package.tar.gz\"";
            if (request.method == "HEAD") {
                response.headers["Content-Length"] = std::to_string(bytes.size());
            } else {
                response.body = std::move(bytes);
            }
            return response;
        }
    }

    if (request.method == "POST") {
        return error_response(kStatusNotFound, "not found");
    }

    return error_response(kStatusNotFound, "not found");
}

}  // namespace pscript::registry
