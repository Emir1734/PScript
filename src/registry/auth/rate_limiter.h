#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace pscript::registry {

class RateLimiter {
public:
    bool allow(const std::string& bucket, std::uint32_t max_attempts, std::chrono::seconds window);

private:
    std::mutex mutex_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> buckets_;
};

}  // namespace pscript::registry
