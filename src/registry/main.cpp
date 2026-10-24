#include "registry/http/registry_server.h"
#include "registry/registry_config.h"
#include "registry/service/registry_service.h"
#include "registry/db/registry_db.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    std::string error;
    const std::optional<pscript::registry::RegistryConfig> config =
        pscript::registry::load_registry_config_from_environment(&error);
    if (!config) {
        std::cerr << "registry configuration error: " << error << '\n';
        return 2;
    }

    std::unique_ptr<pscript::registry::RegistryDatabase> database =
        pscript::registry::make_registry_database(config->database_url);
    if (!database) {
        std::cerr << "registry database backend is not available for the configured URL\n";
        std::cerr << "use sqlite:path for local development or postgres:// when built with libpq\n";
        return 2;
    }

    pscript::registry::BlobStore blob_store(config->storage_dir);
    blob_store.max_upload_bytes = config->max_upload_bytes;
    pscript::registry::RegistryService service(std::move(database), std::move(blob_store), config->auth);
    if (!service.initialize(&error)) {
        std::cerr << "registry startup failed: " << error << '\n';
        return 2;
    }

    pscript::registry::RegistryServer server(*config, service);
    if (!server.start(&error)) {
        std::cerr << "registry listen failed: " << error << '\n';
        return 2;
    }

    std::cout << "pscript registry listening on " << config->bind_host << ':' << server.bound_port() << '\n';
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}
