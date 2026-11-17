#pragma once

#include "util/NanoRub.h"

#include <cstdint>
#include <string>

namespace proxy {

struct CostBreakdown {
    NanoRub input_nano = 0;
    NanoRub cache_read_nano = 0;
    NanoRub cache_write_nano = 0;
    NanoRub output_nano = 0;
    NanoRub total_nano = 0;
    NanoRub saved_nano = 0;
    std::int64_t cached_tokens = 0;
    double cache_hit_ratio = 0.0;
};

// Единственная формула стоимости токенов и кеша: цены — nanoRUB за токен.
struct TokenCostRequest {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cached_tokens = 0;
    NanoRub input_price = 0;
    NanoRub cache_read_price = 0;
    NanoRub output_price = 0;
    NanoRub fixed_fee = 0;
    std::int64_t cache_write_tokens = 0;
    // Цена записи в кеш за токен; 0 — отдельного тарифа нет, пишется по цене входа.
    NanoRub cache_write_price = 0;
    std::int64_t threshold_tokens = 0;
    NanoRub threshold_price = 0;
    std::string threshold_mode = "replace";
};

CostBreakdown computeTokenCost(const TokenCostRequest& request);

} // namespace proxy
