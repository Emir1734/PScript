#pragma once

#include "package/semver.h"

#include <string>
#include <vector>

namespace pscript::package {

std::vector<SemVer> registry_list_versions(const std::string& registry_base, const std::string& package_name,
                                           std::string* error = nullptr);
bool registry_download_package(const std::string& registry_base, const std::string& package_name,
                               const SemVer& version, const std::string& destination_dir, std::string* error = nullptr);
bool registry_publish_package(const std::string& registry_base, const std::string& package_name,
                              const SemVer& version, const std::string& source_dir, const std::string& token,
                              std::string* error = nullptr);

std::vector<std::string> registry_search_packages(const std::string& registry_base, const std::string& query, int limit,
                                                  std::string* error = nullptr);

}  // namespace pscript::package
