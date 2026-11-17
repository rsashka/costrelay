#pragma once

#include "config/Config.h"
#include "pricing/CostModel.h"
#include "pricing/PriceCatalog.h"
#include "util/NanoRub.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

namespace proxy {

// Оценка для модели, по которой ещё нет запросов.
constexpr std::int64_t kNominalInputTokens = 1000;
constexpr std::int64_t kNominalOutputTokens = 500;

// Профиль токенов, для которого считается цена.
struct CostMix {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cached_tokens = 0;
    // Запись в кеш: входные токены по отдельному тарифу (обычно 0 — профиль их не
    // наблюдает, стоимость запроса считается по факту usage).
    std::int64_t cache_write_tokens = 0;

    std::int64_t billedTokens() const { return input_tokens + output_tokens; }
    double cacheHitRatio() const {
        return input_tokens > 0
                   ? static_cast<double>(cached_tokens) / static_cast<double>(input_tokens)
                   : 0.0;
    }
};

// Параметры запроса, которые провайдер должен поддерживать (нужны только для
// локальных openai-compatible апстримов: они получают полный набор).
const std::vector<std::string>& providerParameters();

// Состояние цен модели в каталоге.
struct PriceState {
    bool has_snapshot = false;
    bool usable = false;
    bool stale = false;
    bool has_direct = false;
};

// Профиль запроса, по которому оффер оценивается: доля кеша модели с
// ограничением ёмкости кеша самого оффера. Один путь и у очереди, и у
// предпросмотра панели.
CostMix mixForOffer(const PriceOffer& offer, std::int64_t in, std::int64_t out, double ratio);

// Единственный расчёт веса ранжирования: слагаемые стоимости с поправочными
// весами `routing.output_weight`/`routing.cache_weight` и цена задержки.
struct RankCost {
    NanoRub input_nano = 0;   // вход и запись в кеш, без поправок
    NanoRub cache_nano = 0;   // кеш с поправкой cache_weight
    NanoRub output_nano = 0;  // выход с поправкой output_weight
    NanoRub fixed_nano = 0;
    NanoRub latency_nano = 0;
    NanoRub tokens_nano = 0;  // input + cache + output + fixed: вес без цены задержки
    NanoRub rank_nano = 0;    // tokens_nano + latency_nano
};
RankCost rankCost(const CostBreakdown& cost, NanoRub fixed_fee, NanoRub latency_nano,
                  double output_weight, double cache_weight);

// Оффер (роутер, провайдер), оценённый для профиля токенов.
struct RouteCandidate {
    std::string router_id;
    std::string provider_tag;
    NanoRub cost_nano = 0;             // реальная стоимость этого профиля
    NanoRub rank_cost_nano = 0;        // стоимость с весами кеша/выхода, без деления на токены
    NanoRub cost_per_million_nano = 0; // слагаемое веса за токены и плату, на 1M
    // Наблюдаемая задержка провайдера и её цена в тех же RUB/M: порядок очереди —
    // token cost + время по `routing.wait_price_rub_per_sec`. Нулевая задержка
    // (нет наблюдений) веса не меняет.
    int latency_ms = 0;
    NanoRub latency_cost_nano = 0;            // цена задержки за профиль запроса
    NanoRub latency_cost_per_million_nano = 0;
    NanoRub weight_per_million_nano = 0;      // cost_per_million + latency_per_million
    NanoRub cost_no_cache_nano = 0;
    NanoRub cache_saved_nano = 0;
    // Составляющие cost_nano на профиле снимка: панель и API читают их, а не
    // пересчитывают формулу стоимости. fixed-часть — fixed_fee_nano.
    NanoRub input_cost_nano = 0;
    NanoRub cache_cost_nano = 0;
    NanoRub output_cost_nano = 0;
    // Цены этого кандидата за токен с множителем роутера: то, что уходит в формулу
    // стоимости и в панель.
    NanoRub prompt_price_nano = 0;
    NanoRub completion_price_nano = 0;
    NanoRub cache_read_price_nano = 0;
    NanoRub cache_write_price_nano = 0; // 0 — запись в кеш стоит как вход
    NanoRub fixed_fee_nano = 0;
    NanoRub threshold_prompt_price_nano = 0;
    std::int64_t threshold_tokens = 0;
    std::int64_t cache_capacity_tokens = 0;
    std::int64_t cached_tokens = 0;
    double cache_hit_ratio = 0.0;
    PriceOffer offer; // лимиты и метаданные оффера; цены здесь каталожные
    std::string excluded; // пусто — кандидат допущен
    std::string excluded_detail;

    // Стоимость профиля по ценам этого кандидата.
    CostBreakdown cost(const CostMix& mix, const std::string& threshold_mode) const;
};

// Причина снятия привязки сессии (RoutePlan::session_rebind_reason).
constexpr const char* kPinDropNotEligible = "not_eligible";
constexpr const char* kPinDropCostMargin = "cost_margin";

// Провайдер не работает (правило routing.health_*): он исключён из плана живого
// запроса, а привязка сессии с него снимается без ожидания.
constexpr const char* kProviderDown = "provider_down";
// Причины исключения оффера из маршрута: значение ставит Router, читают панель,
// API и explain — литерал в одном месте, а не в каждом потребителе.
constexpr const char* kProviderBlacklisted = "provider_blacklisted";
constexpr const char* kProviderDeprioritized = "provider_deprioritized";
constexpr const char* kUnsupportedPricingUnit = "unsupported_pricing_unit";
constexpr const char* kCountryMismatch = "country_mismatch";
constexpr const char* kQuantizationMismatch = "quantization_mismatch";
constexpr const char* kContextTooSmall = "context_too_small";
constexpr const char* kProviderNotWhitelisted = "provider_not_whitelisted";
// Состояние маршрута модели: причина (reason) и код отказа (error) плана. Их
// ставит ProviderQueue, читают панель, API и журнал.
constexpr const char* kNoRouteCode = "no_route";
constexpr const char* kNoPriceReason = "no_price";
constexpr const char* kNoPriceSnapshotCode = "no_price_snapshot";
constexpr const char* kModelNotConfiguredReason = "model_not_configured";
constexpr const char* kModelNotFoundCode = "model_not_found";
constexpr const char* kStalePricesReason = "stale_prices";
constexpr const char* kPricesUnavailableCode = "prices_unavailable";
constexpr const char* kCostReason = "cost";
constexpr const char* kCostStaleReason = "cost_stale";
constexpr const char* kReasonNotInWhitelist = "not in provider whitelist";

// Готовый план запроса: верх очереди на момент последнего перестроения.
struct RoutePlan {
    std::string model;
    std::int64_t estimated_in = 0;  // профиль очереди (для панели и логов)
    std::int64_t estimated_out = 0;
    double cache_hit_ratio = 0.0;
    std::vector<RouteCandidate> candidates; // допущены, порядок очереди
    std::vector<RouteCandidate> rejected;   // исключены фильтрами, с причиной
    std::string reason = kNoRouteCode;
    bool stale_prices = false;
    std::string error;
    // Непусто — план переключился вопреки привязке: провайдер сессии исчез из
    // допущенных (`kPinDropNotEligible`), не работает (`kProviderDown`) или другой
    // оффер дешевле его на порог `routing.leader.margin_percent`
    // (`kPinDropCostMargin`).
    std::string session_rebind_from;
    std::string session_rebind_reason;
    // Теги в порядке очереди: priority — все допущенные (панель),
    // order — верх provider_order_size для объекта provider.order в запросе.
    std::vector<std::string> provider_priority;
    std::vector<std::string> provider_order;

    bool hasRoute() const { return !candidates.empty(); }
};

class Router {
  public:
    Router(const AppConfig& config, const PriceCatalog& catalog);

    // Офферы модели, оценённые для профиля и упорядоченные по
    // weight_per_million_nano — единственный порядок провайдеров: стоимость
    // токенов плюс цена наблюдаемой задержки. Профиль здесь модельный
    // (наблюдаемый), а не запросный: ранжирование не зависит от конкретного
    // запроса. Цены — только текущий каталог; статистика задаёт пропорции
    // токенов и задержку, но никогда не цену.
    std::vector<RouteCandidate> quotes(
        const std::string& model, const CostMix& mix,
        const std::function<int(const std::string& provider)>& latency_ms = {}) const;

    // Состояние каталога цен модели: свежесть, наличие прямых апстримов.
    PriceState priceState(const std::string& model) const;

  private:
    const AppConfig& config_;
    const PriceCatalog& catalog_;
};

CostBreakdown estimateOfferCostBreakdown(const PriceOffer& offer, std::int64_t in,
                                         std::int64_t out,
                                         const std::string& threshold_mode,
                                         double cache_hit_ratio = 0.0);

// Объект `provider` для RouterAI — то, что уходит в запрос: provider.order с
// верхом очереди. Fallback внутри RouterAI выключен: список — это наша очередь.
Json::Value providerPayload(const RoutePlan& plan, const RouteCandidate& candidate,
                            const AppConfig& config);

// Тело запроса к провайдеру: модель, объект provider, переименование модели.
Json::Value buildUpstreamBody(const Json::Value& original, const RoutePlan& plan,
                              const RouteCandidate& candidate, const AppConfig& config,
                              std::string& error);

// Причина блокировки пары (модель, провайдер); пусто — не заблокирована.
std::string providerBlockReason(const AppConfig& config, const std::string& model,
                                const std::string& provider);

} // namespace proxy
