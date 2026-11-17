#pragma once

#include "config/Config.h"
#include "pricing/PriceCatalog.h"
#include "util/NanoRub.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace test {

class TempDir {
  public:
    TempDir() {
        static std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("proxy-test-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }
    std::string str() const { return path_.string(); }

  private:
    std::filesystem::path path_;
};

inline void writeFile(const std::filesystem::path& path, const std::string& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

inline proxy::PriceOffer offer(const std::string& tag, double prompt, double completion,
                               int status = 0, int64_t context = 128000,
                               int64_t max_completion = 4096) {
    proxy::PriceOffer o;
    o.provider_tag = tag;
    o.prompt_per_token = proxy::rubToNano(prompt);
    o.completion_per_token = proxy::rubToNano(completion);
    o.status = status;
    o.context_length = context;
    o.max_completion_tokens = max_completion;
    o.supported_parameters = {"tools", "temperature", "max_tokens", "stream", "response_format"};
    o.countries = {"ru"};
    return o;
}

inline proxy::ModelPriceSnapshot snapshot(const std::string& model,
                                          std::vector<proxy::PriceOffer> offers) {
    proxy::ModelPriceSnapshot s;
    s.model_id = model;
    s.endpoints = std::move(offers);
    s.fetched_at = std::chrono::system_clock::now();
    return s;
}

inline proxy::RouterConfig router(const std::string& id, const std::string& base,
                                  double multiplier = 1.0) {
    proxy::RouterConfig r;
    r.id = id;
    r.base_url = base;
    r.models = {"*"};
    r.price_multiplier = multiplier;
    r.max_concurrent = 8;
    r.timeouts.connect_ms = 1000;
    r.timeouts.total_ms = 10000;
    return r;
}

} // namespace test
