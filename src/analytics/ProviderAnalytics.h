#pragma once

#include "config/Config.h"
#include "util/NanoRub.h"

#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace proxy {

struct ProviderLogImportResult {
    bool ok = false;
    bool duplicate = false;
    std::size_t files = 0;
    std::size_t rows = 0;
    std::size_t skipped = 0;
    // Нечисловые значения в числовых колонках: строка не молчит нулями.
    std::size_t bad_fields = 0;
    std::string error;
};

class ProviderAnalytics {
  public:
    explicit ProviderAnalytics(AnalyticsConfig config = {});

    bool enabled() const { return config_.enabled; }

    ProviderLogImportResult importCsvContent(std::string_view content,
                                             const std::string& source);
    ProviderLogImportResult importDirectory();
    bool loadSnapshot();
    bool saveSnapshot() const;
    // Причина последнего отказа снимка: отказ виден вызывающему, а не только в
    // summaryJson.
    const std::string& lastError() const { return last_error_; }

    Json::Value summaryJson() const;
    Json::Value providersJson(std::size_t limit = 50) const;
    Json::Value modelsJson(std::size_t limit = 50) const;
    Json::Value timesJson(std::size_t limit = 200) const;

  private:
    struct Stats {
        std::string model;
        std::string provider;
        std::int64_t requests = 0;
        std::int64_t input_tokens = 0;
        std::int64_t cached_tokens = 0;
        std::int64_t cache_write_tokens = 0;
        std::int64_t output_tokens = 0;
        std::int64_t reasoning_tokens = 0;
        NanoRub input_cost = 0;
        NanoRub cache_cost = 0;
        NanoRub output_cost = 0;
        NanoRub total_cost = 0;
        double latency_sum_ms = 0;
        std::int64_t latency_max_ms = 0;
        std::int64_t max_cached_tokens = 0;
        std::string first_seen;
        std::string last_seen;
        std::int64_t first_epoch = 0;
        std::int64_t last_epoch = 0;
        std::vector<double> input_prices;
        std::vector<double> cache_prices;
        std::vector<double> output_prices;
        std::vector<double> latencies_ms;
        std::string date;
        int hour = -1;
        NanoRub input_per_token = 0;
        NanoRub cache_read_per_token = 0;
        NanoRub output_per_token = 0;
        double p50_latency_ms = 0.0;
        double p90_latency_ms = 0.0;
        double tokens_per_second = 0.0;
        double output_tokens_per_second = 0.0;
        double reasoning_ratio = 0.0;
    };
    struct ParsedRecord {
        std::string model;
        std::string provider;
        std::string date;
        int hour = 0;
        std::string seen_local;
        std::int64_t seen_epoch = 0;
        std::int64_t input_tokens = 0;
        std::int64_t cached_tokens = 0;
        std::int64_t cache_write_tokens = 0;
        std::int64_t output_tokens = 0;
        std::int64_t reasoning_tokens = 0;
        NanoRub input_cost = 0;
        NanoRub cache_cost = 0;
        NanoRub output_cost = 0;
        NanoRub total_cost = 0;
        double latency_ms = 0;
    };

    static void finalize(Stats& stats);
    // Порядок строк отчёта: больше запросов, при равенстве — дороже.
    static bool byRequestsThenCost(const Stats& a, const Stats& b);
    // Выборка передаётся по значению: расчёт процентиля не меняет накопленные
    // данные группы.
    static double percentile(std::vector<double> values, double q);
    void accumulate(Stats& stats, const ParsedRecord& record);
    void merge(const ParsedRecord& record);
    static std::string groupKey(const std::string& model, const std::string& provider);
    static std::string normalizeProvider(const std::string& provider);

    AnalyticsConfig config_;
    mutable std::mutex mu_;
    std::map<std::string, Stats> groups_;
    std::map<std::string, Stats> times_;
    std::set<std::string> imported_hashes_;
    std::size_t files_ = 0;
    std::size_t rows_ = 0;
    std::size_t skipped_ = 0;
    // Причина отказа снимка: пишется и из const-метода сохранения, поэтому
    // доступ к строке идёт под mu_.
    void noteError(const std::string& what) const;
    mutable std::string last_error_;
};

} // namespace proxy
