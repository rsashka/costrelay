#pragma once

#include "util/NanoRub.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

namespace proxy {

struct PriceOffer {
    std::string provider_tag;
    NanoRub prompt_per_token = 0;
    NanoRub completion_per_token = 0;
    NanoRub cache_read_per_token = 0;
    NanoRub cache_write_per_token = 0; // 0 — тарифа нет: запись стоит как вход
    NanoRub fixed_request_fee = 0;
    std::int64_t threshold_tokens = 0;          // 0 = no prompt threshold
    NanoRub threshold_prompt_per_token = 0;
    std::int64_t context_length = 0;
    std::int64_t max_completion_tokens = 0;
    std::int64_t cache_capacity_tokens = 0; // 0 = без ограничения
    std::optional<std::int64_t> max_prompt_tokens;
    std::vector<std::string> supported_parameters;
    int status = 0;
    std::string quantization;
    std::vector<std::string> countries;
    std::string pricing_units_prompt = "token";
    std::string pricing_units_completion = "token";
    bool chat_compatible = true;
};

struct ModelPriceSnapshot {
    std::string model_id;
    std::vector<PriceOffer> endpoints;
    std::string etag;
    std::chrono::system_clock::time_point fetched_at{};
};

class PriceCatalog {
  public:
    PriceCatalog() = default;
    bool replace(ModelPriceSnapshot snapshot);
    std::shared_ptr<const ModelPriceSnapshot> get(const std::string& model) const;
    std::vector<std::string> models() const;
    std::size_t size() const;
    // Счётчик успешных обновлений каталога: очередь провайдеров перестраивается,
    // когда ревизия изменилась.
    std::uint64_t revision() const { return revision_.load(std::memory_order_relaxed); }

    static ModelPriceSnapshot parseEndpoints(const Json::Value& root,
                                             const std::string& expected_model);
    bool loadFromFile(const std::string& path);
    bool saveSnapshot(const std::string& path) const;

  private:
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<const ModelPriceSnapshot>> models_;
    std::atomic<std::uint64_t> revision_{0};
};

} // namespace proxy
