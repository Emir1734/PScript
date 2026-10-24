#include "registry/auth/rate_limiter.h"

#include <map>

namespace pscript::registry {
namespace {

constexpr std::size_t kMaxBuckets = 8192;
constexpr std::size_t kMaxEntriesPerBucket = 256;

void trim_bucket(std::deque<std::chrono::steady_clock::time_point>& entries,
                 std::chrono::steady_clock::time_point now, std::chrono::seconds window) {
    const auto cutoff = now - window;
    while (!entries.empty() && entries.front() < cutoff) {
        entries.pop_front();
    }
    while (entries.size() > kMaxEntriesPerBucket) {
        entries.pop_front();
    }
}

}  // namespace

bool RateLimiter::allow(const std::string& bucket, std::uint32_t max_attempts, std::chrono::seconds window) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    if (buckets_.size() >= kMaxBuckets && buckets_.find(bucket) == buckets_.end()) {
        buckets_.erase(buckets_.begin());
    }
    std::deque<std::chrono::steady_clock::time_point>& entries = buckets_[bucket];
    trim_bucket(entries, now, window);
    if (entries.size() >= max_attempts) {
        return false;
    }
    entries.push_back(now);
    return true;
}

}  // namespace pscript::registry
