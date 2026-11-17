#pragma once

#include <map>
#include <mutex>
#include <string>

namespace proxy {

// Счётчик одновременных запросов к апстриму; limit <= 0 — без ограничения.
class ConcurrencyLimiter {
  public:
    bool tryAcquire(const std::string& key, int limit) {
        if (limit <= 0) return true;
        std::lock_guard<std::mutex> lock(mu_);
        auto& used = counts_[key];
        if (used >= limit) return false;
        ++used;
        return true;
    }

    void release(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = counts_.find(key);
        if (it == counts_.end()) return;
        if (--it->second <= 0) counts_.erase(it);
    }

  private:
    mutable std::mutex mu_;
    std::map<std::string, int> counts_;
};

} // namespace proxy
