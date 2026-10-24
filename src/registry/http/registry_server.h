#pragma once

#include "registry/http/registry_router.h"
#include "registry/registry_config.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace pscript::registry {

class RegistryServer {
public:
    RegistryServer(RegistryConfig config, RegistryService& service);
    ~RegistryServer();

    bool start(std::string* error);
    void stop();
    int bound_port() const { return bound_port_; }

    HttpResponseBody handle_request(const pscript::HttpServerRequest& request) const;

    std::string client_key_for_request(const pscript::HttpServerRequest& request) const;

private:
    void accept_loop();

    RegistryConfig config_;
    RegistryService& service_;
    RegistryRouter router_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    pscript::SocketHandle listener_ = pscript::kInvalidSocket;
    int bound_port_ = 0;
};

}  // namespace pscript::registry
