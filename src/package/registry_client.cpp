#include "package/registry_client.h"

#include "package/json_util.h"
#include "runtime/http_ops.h"

#include <sstream>

namespace pscript::package {
namespace {

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) {
        return suffix;
    }
    if (base.back() == '/') {
        return base + suffix;
    }
    return base + '/' + suffix;
}

std::string api_prefix(const std::string& registry_url) {
    if (registry_url.rfind("http://", 0) == 0 || registry_url.rfind("https://", 0) == 0) {
        if (registry_url.size() >= 3 && registry_url.substr(registry_url.size() - 3) == "/v1") {
            return registry_url;
        }
        return join_url(registry_url, "v1");
    }
    return registry_url;
}

bool post_auth_json(const std::string& url, const std::string& json_body, RegistryAuthToken* token_out,
                    std::string* error) {
    const std::vector<std::uint8_t> payload(json_body.begin(), json_body.end());
    try {
        const HttpClientResponse response = http_request_sync("POST", url, payload, {{"Content-Type", "application/json"}});
        const std::string body(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        if (response.status == 429) {
            if (error) {
                *error = "too many authentication attempts";
            }
            return false;
        }
        if (response.status < 200 || response.status >= 300) {
            if (error) {
                *error = "authentication request failed";
            }
            return false;
        }
        if (token_out) {
            if (const std::optional<std::string> username = json_parse_string_field(body, "username")) {
                token_out->username = *username;
            }
            if (const std::optional<std::string> token = json_parse_string_field(body, "token")) {
                token_out->token = *token;
            }
            if (const std::optional<std::string> expires = json_parse_string_field(body, "expires_at")) {
                token_out->expires_at = *expires;
            }
        }
        return true;
    } catch (const std::exception&) {
        if (error) {
            *error = "authentication request failed";
        }
        return false;
    }
}

}  // namespace

bool registry_remote_register(const std::string& registry_url, const std::string& username, std::string_view password,
                              RegistryAuthToken* token_out, std::string* error) {
    return post_auth_json(join_url(api_prefix(registry_url), "auth/register"),
                          json_auth_credentials_body(username, password), token_out, error);
}

bool registry_remote_login(const std::string& registry_url, const std::string& username, std::string_view password,
                           RegistryAuthToken* token_out, std::string* error) {
    return post_auth_json(join_url(api_prefix(registry_url), "auth/login"),
                          json_auth_credentials_body(username, password), token_out, error);
}

bool registry_remote_logout(const std::string& registry_url, const std::string& bearer_token, std::string* error) {
    try {
        const HttpClientResponse response =
            http_request_sync("POST", join_url(api_prefix(registry_url), "auth/logout"), {},
                              {{"Authorization", "Bearer " + bearer_token}});
        if (response.status != 204 && (response.status < 200 || response.status >= 300)) {
            if (error) {
                *error = "logout failed";
            }
            return false;
        }
        return true;
    } catch (const std::exception&) {
        if (error) {
            *error = "logout failed";
        }
        return false;
    }
}

bool registry_remote_whoami(const std::string& registry_url, const std::string& bearer_token, std::string* username_out,
                            std::string* error) {
    try {
        const HttpClientResponse response =
            http_request_sync("GET", join_url(api_prefix(registry_url), "auth/me"), {},
                              {{"Authorization", "Bearer " + bearer_token}});
        if (response.status == 401) {
            if (error) {
                *error = "not authenticated";
            }
            return false;
        }
        if (response.status < 200 || response.status >= 300) {
            if (error) {
                *error = "whoami request failed";
            }
            return false;
        }
        const std::string body(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        if (const std::optional<std::string> username = json_parse_string_field(body, "username")) {
            if (username_out) {
                *username_out = *username;
            }
            return true;
        }
        if (error) {
            *error = "whoami response invalid";
        }
        return false;
    } catch (const std::exception&) {
        if (error) {
            *error = "whoami request failed";
        }
        return false;
    }
}

std::vector<std::string> registry_remote_search(const std::string& registry_url, const std::string& query, int limit,
                                                std::string* error) {
    std::vector<std::string> results;
    const std::string url = join_url(api_prefix(registry_url), "search?q=" + query + "&limit=" + std::to_string(limit));
    try {
        const HttpClientResponse response = http_request_sync("GET", url, {}, {});
        if (response.status < 200 || response.status >= 300) {
            if (error) {
                *error = "search request failed";
            }
            return results;
        }
        const std::string body(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        std::size_t index = 0;
        while (index < body.size()) {
            const std::size_t quote = body.find('"', index);
            if (quote == std::string::npos) {
                break;
            }
            if (quote > 0 && body[quote - 1] == ':') {
                index = quote + 1;
                continue;
            }
            const std::size_t end = body.find('"', quote + 1);
            if (end == std::string::npos) {
                break;
            }
            const std::string_view token = std::string_view(body).substr(quote + 1, end - quote - 1);
            if (!token.empty() && token.front() == '@') {
                results.emplace_back(token);
            }
            index = end + 1;
        }
        return results;
    } catch (const std::exception&) {
        if (error) {
            *error = "search request failed";
        }
        return results;
    }
}

}  // namespace pscript::package
