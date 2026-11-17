#include "pricing/CostModel.h"
#include "util/Ratio.h"

#include <algorithm>

namespace proxy {
namespace {

// Стоимость входных токенов при пороге prompt-threshold.
NanoRub promptCost(const TokenCostRequest& request, std::int64_t uncached) {
    const bool thresholded = request.threshold_tokens > 0 &&
                             request.input_tokens >= request.threshold_tokens &&
                             request.threshold_price > 0;
    const NanoRub prompt_price = thresholded ? request.threshold_price
                                             : request.input_price;
    if (thresholded && request.threshold_mode == "excess") {
        const auto first = std::min<std::int64_t>(uncached, request.threshold_tokens);
        return safeMulTokens(first, request.input_price) +
               safeMulTokens(uncached - first, request.threshold_price);
    }
    return safeMulTokens(uncached, prompt_price);
}

} // namespace

CostBreakdown computeTokenCost(const TokenCostRequest& request) {
    CostBreakdown out;
    const auto input_tokens = std::max<std::int64_t>(0, request.input_tokens);
    const auto output_tokens = std::max<std::int64_t>(0, request.output_tokens);
    out.cached_tokens = std::clamp<std::int64_t>(request.cached_tokens, 0, input_tokens);
    out.cache_hit_ratio = ratioOf(out.cached_tokens, input_tokens);

    const auto uncached = input_tokens - out.cached_tokens;
    const auto fixed_fee = std::max<NanoRub>(0, request.fixed_fee);
    const auto output_nano = safeMulTokens(output_tokens, request.output_price);
    // Запись в кеш — входные токены: каталог может задать отдельный тариф, иначе
    // они стоят как обычный вход.
    const auto write_price = request.cache_write_price > 0 ? request.cache_write_price
                                                           : request.input_price;
    const auto write_nano =
        safeMulTokens(std::max<std::int64_t>(0, request.cache_write_tokens), write_price);
    out.cache_write_nano = write_nano;
    const NanoRub uncached_nano = promptCost(request, uncached);
    out.input_nano = safeAdd(uncached_nano, write_nano);
    out.cache_read_nano = safeMulTokens(out.cached_tokens, request.cache_read_price);
    out.output_nano = output_nano;
    out.total_nano = safeAdd(safeAdd(out.input_nano, out.cache_read_nano),
                             safeAdd(output_nano, fixed_fee));
    // Экономия кеша — разница «весь вход без попаданий» и факта по входу; запись,
    // выход и фикс. плата в обеих частях одинаковы и не влияют.
    out.saved_nano = std::max<NanoRub>(
        0, promptCost(request, input_tokens) - safeAdd(uncached_nano, out.cache_read_nano));
    return out;
}

} // namespace proxy
