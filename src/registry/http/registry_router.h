#pragma once

#include "registry/service/registry_service.h"
#include "runtime/http_ops.h"

#include <cstddef>
#include <map>
#include <string>

namespace pscript::registry {

struct HttpResponseBody {
    int status = 500;
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
};

class RegistryRouter {
public:
    RegistryRouter(RegistryService& service, std::string api_prefix, int max_search_results,
                   std::size_t max_auth_body_bytes, std::size_t max_upload_bytes);

    HttpResponseBody handle(const pscript::HttpServerRequest& request, const std::string& client_key) const;

private:
    RegistryService& service_;
    std::string api_prefix_;
    int max_search_results_ = 50;
    std::size_t max_auth_body_bytes_ = 4096;
    std::size_t max_upload_bytes_ = 64ULL * 1024 * 1024;
};

}  // namespace pscript::registry
