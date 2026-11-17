#include "routing/Router.h"
#include "util/Ratio.h"
#include "util/Strings.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace proxy {

const std::vector<std::string>& providerParameters() {
    static const std::vector<std::string> names = {
        "tools", "tool_choice", "response_format", "logprobs", "top_logprobs", "seed",
        "stop", "temperature", "top_p", "presence_penalty", "frequency_penalty",
        "parallel_tool_calls", "max_tokens", "max_completion_tokens", "stream"};
    return names;
}

namespace {

// Доля попаданий -> токены, с ограничением ёмкости кеша провайдера.
std::int64_t cachedTokensForRatio(const PriceOffer& offer, std::int64_t in, double ratio) {
    ratio = std::clamp(ratio, 0.0, 1.0);
    const std::int64_t cacheable =
        offer.cache_capacity_tokens > 0 ? std::min(in, offer.cache_capacity_tokens) : in;
    return std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(static_cast<double>(cacheable) * ratio)), 0, in);
}

NanoRub scalePrice(NanoRub price, long double multiplier) {
    return static_cast<NanoRub>(std::llround(static_cast<long double>(price) * multiplier));
}

// Цены оффера с множителем и фиксированной платой роутера.
void applyPrices(RouteCandidate& c, const PriceOffer& offer, const RouterConfig& router) {
    const long double multiplier = static_cast<long double>(router.price_multiplier);
    c.prompt_price_nano = scalePrice(offer.prompt_per_token, multiplier);
    c.cache_read_price_nano = scalePrice(offer.cache_read_per_token, multiplier);
    c.cache_write_price_nano = offer.cache_write_per_token > 0
                                   ? scalePrice(offer.cache_write_per_token, multiplier)
                                   : c.prompt_price_nano;
    c.completion_price_nano = scalePrice(offer.completion_per_token, multiplier);
    c.fixed_fee_nano = router.fixed_fee_nano + scalePrice(offer.fixed_request_fee, multiplier);
    c.threshold_prompt_price_nano =
        scalePrice(offer.threshold_prompt_per_token, multiplier);
    c.threshold_tokens = offer.threshold_tokens;
    c.cache_capacity_tokens = offer.cache_capacity_tokens;
    c.offer = offer;
}

PriceOffer directOffer(const RouterConfig& router) {
    PriceOffer offer;
    offer.provider_tag = providerTagOf(router);
    offer.prompt_per_token = perMillionRubToPerTokenNano(router.prompt_per_million_rub);
    offer.completion_per_token = perMillionRubToPerTokenNano(router.completion_per_million_rub);
    offer.context_length = router.context_length;
    offer.max_completion_tokens = router.max_completion_tokens;
    offer.cache_read_per_token = perMillionRubToPerTokenNano(router.cache_read_per_million_rub);
    offer.cache_capacity_tokens = router.cache_size_tokens;
    offer.supported_parameters = providerParameters();
    offer.status = 0;
    offer.pricing_units_prompt = "token";
    offer.pricing_units_completion = "token";
    offer.chat_compatible = true;
    return offer;
}

} // namespace

CostMix mixForOffer(const PriceOffer& offer, std::int64_t in, std::int64_t out, double ratio) {
    in = std::max<std::int64_t>(0, in);
    out = std::max<std::int64_t>(0, out);
    return CostMix{in, out, cachedTokensForRatio(offer, in, ratio)};
}

RankCost rankCost(const CostBreakdown& cost, NanoRub fixed_fee, NanoRub latency_nano,
                  double output_weight, double cache_weight) {
    RankCost out;
    out.input_nano = cost.input_nano;
    out.cache_nano = static_cast<NanoRub>(std::llround(
        static_cast<long double>(cache_weight) * static_cast<long double>(cost.cache_read_nano)));
    out.output_nano = static_cast<NanoRub>(std::llround(
        static_cast<long double>(output_weight) * static_cast<long double>(cost.output_nano)));
    out.fixed_nano = fixed_fee;
    out.latency_nano = latency_nano;
    out.tokens_nano = out.input_nano + out.cache_nano + out.output_nano + out.fixed_nano;
    out.rank_nano = std::max<NanoRub>(0, out.tokens_nano + out.latency_nano);
    return out;
}

CostBreakdown RouteCandidate::cost(const CostMix& mix, const std::string& threshold_mode) const {
    TokenCostRequest request;
    request.input_tokens = std::max<std::int64_t>(0, mix.input_tokens);
    request.output_tokens = std::max<std::int64_t>(0, mix.output_tokens);
    request.cached_tokens =
        std::clamp<std::int64_t>(mix.cached_tokens, 0, request.input_tokens);
    request.input_price = prompt_price_nano;
    request.cache_read_price = cache_read_price_nano;
    request.output_price = completion_price_nano;
    request.fixed_fee = fixed_fee_nano;
    request.cache_write_tokens = std::max<std::int64_t>(0, mix.cache_write_tokens);
    request.cache_write_price = cache_write_price_nano;
    request.threshold_tokens = threshold_tokens;
    request.threshold_price = threshold_prompt_price_nano;
    request.threshold_mode = threshold_mode;
    return computeTokenCost(request);
}

CostBreakdown estimateOfferCostBreakdown(const PriceOffer& offer, std::int64_t in,
                                         std::int64_t out, const std::string& threshold_mode,
                                         double cache_hit_ratio) {
    // Оффер оценивается без множителя и фиксированной платы роутера: цена самого
    // оффера. Контракт задан явно, а не значениями по умолчанию RouterConfig.
    RouterConfig identity;
    identity.price_multiplier = 1.0L;
    identity.fixed_fee_nano = 0;
    RouteCandidate candidate;
    applyPrices(candidate, offer, identity);
    return candidate.cost(mixForOffer(offer, in, out, cache_hit_ratio), threshold_mode);
}

Router::Router(const AppConfig& config, const PriceCatalog& catalog)
    : config_(config), catalog_(catalog) {}

PriceState Router::priceState(const std::string& model) const {
    PriceState state;
    const auto snap = catalog_.get(model);
    const auto now = std::chrono::system_clock::now();
    if (snap) {
        state.has_snapshot = true;
        const auto age = now > snap->fetched_at
                             ? std::chrono::duration_cast<std::chrono::seconds>(now - snap->fetched_at)
                             : std::chrono::seconds(0);
        state.stale = age > std::chrono::seconds(std::max(1, config_.pricing.stale_after_sec));
        const bool too_old = age > std::chrono::seconds(std::max(1, config_.pricing.max_stale_sec));
        const bool requires_fresh =
            config_.pricing.require_fresh &&
            age > std::chrono::seconds(std::max(1, config_.pricing.stale_after_sec));
        state.usable = !too_old && !requires_fresh;
    }
    for (const auto& router : config_.routers) {
        if (router.enabled && router.kind == "openai_compatible" &&
            routerServesModel(router, model)) {
            state.has_direct = true;
            break;
        }
    }
    return state;
}

std::string providerBlockReason(const AppConfig& config, const std::string& model,
                                const std::string& provider) {
    const std::string tag = toLower(provider);
    for (const auto& block : config.routing.provider_blacklist) {
        if (!globMatch(toLower(block.provider), tag)) continue;
        for (const auto& pattern : block.models) {
            if (pattern == "*" || globMatch(pattern, model)) {
                return block.reason.empty() ? kDefaultBlockReason : block.reason;
            }
        }
    }
    return {};
}

std::vector<RouteCandidate> Router::quotes(
    const std::string& model, const CostMix& mix,
    const std::function<int(const std::string& provider)>& latency_ms) const {
    std::vector<RouteCandidate> out;
    const std::string resolved = config_.resolveModel(model);
    const auto state = priceState(resolved);
    auto snap = catalog_.get(resolved);

    struct Source {
        PriceOffer offer;
        const RouterConfig* router;
        bool direct;
    };
    std::vector<Source> sources;
    if (state.usable && snap) {
        for (const auto& offer : snap->endpoints) {
            for (const auto& router : config_.routers) {
                if (!router.enabled || router.kind != "routerai" ||
                    !routerServesModel(router, resolved)) {
                    continue;
                }
                sources.push_back({offer, &router, false});
            }
        }
    }
    for (const auto& router : config_.routers) {
        if (!router.enabled || router.kind != "openai_compatible" ||
            !routerServesModel(router, resolved)) {
            continue;
        }
        sources.push_back({directOffer(router), &router, true});
    }

    // Поправочные веса ранжирования: элемент счёта входит в вес с множителем
    // (1 — как в формуле стоимости). Порядок считается один раз здесь, панель и
    // живой запрос читают только снимок очереди.
    const long double output_weight = static_cast<long double>(config_.routing.output_weight);
    const long double cache_weight = static_cast<long double>(config_.routing.cache_weight);

    for (const auto& source : sources) {
        // Цена — всегда текущая каталожная: историческая статистика цену не
        // подменяет, иначе устаревшая цена выигрывает у актуальной.
        const PriceOffer& offer = source.offer;
        const auto& router = *source.router;
        const bool direct = source.direct;

        RouteCandidate c;
        c.router_id = router.id;
        c.provider_tag = direct ? providerTagOf(router) : offer.provider_tag;
        applyPrices(c, offer, router);

        // Статические фильтры: политика и мёртвые офферы. Поведение запроса
        // (параметры, max_tokens) здесь не участвует — очередь одна для всех.
        if (offer.status < 0) c.excluded = kProviderDeprioritized;
        if (c.excluded.empty() && !offer.chat_compatible) c.excluded = kUnsupportedPricingUnit;
        if (c.excluded.empty() && !direct && !config_.routing.country.empty() &&
            std::find(offer.countries.begin(), offer.countries.end(),
                      config_.routing.country) == offer.countries.end()) {
            c.excluded = kCountryMismatch;
        }
        if (c.excluded.empty() && !direct && !config_.routing.quantization.empty() &&
            std::find(config_.routing.quantization.begin(), config_.routing.quantization.end(),
                      offer.quantization) == config_.routing.quantization.end()) {
            c.excluded = kQuantizationMismatch;
        }
        if (c.excluded.empty() && offer.context_length > 0 &&
            mix.input_tokens + mix.output_tokens > offer.context_length) {
            c.excluded = kContextTooSmall;
        }
        if (c.excluded.empty() && offer.max_prompt_tokens &&
            mix.input_tokens > *offer.max_prompt_tokens) {
            c.excluded = kContextTooSmall;
        }
        if (c.excluded.empty()) {
            const std::string reason = providerBlockReason(config_, resolved, c.provider_tag);
            if (!reason.empty()) {
                c.excluded = kProviderBlacklisted;
                c.excluded_detail = reason;
            }
        }
        if (c.excluded.empty() && !config_.routing.provider_whitelist.empty() &&
            !providerAllowedByList(config_.routing.provider_whitelist, resolved, c.provider_tag)) {
            c.excluded = kProviderNotWhitelisted;
            c.excluded_detail = kReasonNotInWhitelist;
        }

        // Доля кеша — модельная (средняя наблюдаемая), а не запросная.
        const CostMix effective = mixForOffer(
            offer, mix.input_tokens, mix.output_tokens, mix.cacheHitRatio());

        const CostBreakdown cost = c.cost(effective, config_.pricing.threshold_mode);
        CostMix no_cache = effective;
        no_cache.cached_tokens = 0;
        c.cost_nano = cost.total_nano;
        c.input_cost_nano = cost.input_nano;
        c.cache_cost_nano = cost.cache_read_nano;
        c.output_cost_nano = cost.output_nano;
        c.cost_no_cache_nano =
            c.cost(no_cache, config_.pricing.threshold_mode).total_nano;
        c.cache_saved_nano = std::max<NanoRub>(0, c.cost_no_cache_nano - c.cost_nano);
        c.cached_tokens = cost.cached_tokens;
        c.cache_hit_ratio = cost.cache_hit_ratio;
        // Время — такой же элемент счёта, как токены: наблюдаемая задержка
        // провайдера по цене `routing.wait_price_rub_per_sec` переводится в RUB/M
        // на профиле запроса. Единственный порядок — weight, а не цена токенов.
        c.latency_ms = latency_ms ? std::max(0, latency_ms(c.provider_tag)) : 0;
        if (c.latency_ms > 0) {
            const long double seconds = static_cast<long double>(c.latency_ms) / 1000.0L;
            c.latency_cost_nano =
                rubToNano(static_cast<long double>(config_.routing.wait_price_rub_per_sec) *
                          seconds);
        }
        // Ключ ранжирования — единственная формула веса (rankCost): очередь и
        // предпросмотр панели считают одинаково.
        const RankCost rank = rankCost(cost, c.fixed_fee_nano, c.latency_cost_nano,
                                       static_cast<double>(output_weight),
                                       static_cast<double>(cache_weight));
        c.rank_cost_nano = rank.rank_nano;
        // Слагаемые веса: токены и плата — отдельно от цены задержки, иначе
        // задержка входит в вес дважды (она уже в rank_cost_nano).
        c.cost_per_million_nano =
            perMillionFromTotal(rank.tokens_nano, effective.billedTokens());
        c.latency_cost_per_million_nano =
            perMillionFromTotal(c.latency_cost_nano, effective.billedTokens());
        c.weight_per_million_nano =
            c.cost_per_million_nano + c.latency_cost_per_million_nano;
        out.push_back(std::move(c));
    }

    std::sort(out.begin(), out.end(), [](const RouteCandidate& a, const RouteCandidate& b) {
        if (a.excluded.empty() != b.excluded.empty()) return a.excluded.empty();
        if (a.weight_per_million_nano != b.weight_per_million_nano) {
            return a.weight_per_million_nano < b.weight_per_million_nano;
        }
        if (a.provider_tag != b.provider_tag) return a.provider_tag < b.provider_tag;
        return a.router_id < b.router_id;
    });
    return out;
}

Json::Value providerPayload(const RoutePlan& plan, const RouteCandidate& candidate,
                            const AppConfig& config) {
    Json::Value provider(Json::objectValue);
    // Текущая попытка — первая: в запросе ровно та попытка, которую делает прокси,
    // а резервы очереди идут следом, пока их помещает provider_order_size. При
    // provider_order_size = 1 внешний обработчик не может сменить провайдера.
    const std::size_t cap =
        static_cast<std::size_t>(std::max(1, config.routing.provider_order_size));
    Json::Value order(Json::arrayValue);
    if (!candidate.provider_tag.empty()) order.append(candidate.provider_tag);
    for (const auto& tag : plan.provider_order) {
        if (order.size() >= cap) break;
        if (tag == candidate.provider_tag) continue;
        order.append(tag);
    }
    provider["order"] = order;
    provider["allow_fallbacks"] = false;
    if (!config.routing.country.empty()) provider["country"] = config.routing.country;
    return provider;
}

Json::Value buildUpstreamBody(const Json::Value& original, const RoutePlan& plan,
                              const RouteCandidate& candidate, const AppConfig& config,
                              std::string& error) {
    error.clear();
    if (!original.isObject()) {
        error = "request body must be a JSON object";
        return {};
    }
    Json::Value body = original;
    if (body.isMember("max_completion_tokens")) {
        body["max_tokens"] = body["max_completion_tokens"];
        body.removeMember("max_completion_tokens");
    }
    const RouterConfig* router = config.findRouter(candidate.router_id);
    if (!router) {
        error = "router_not_found";
        return {};
    }
    body.removeMember("provider");
    body["model"] = plan.model;
    if (router->kind == "openai_compatible") {
        auto mapped = router->model_map.find(plan.model);
        if (mapped != router->model_map.end() && !mapped->second.empty()) {
            body["model"] = mapped->second;
        }
        return body;
    }
    if (config.routing.inject_provider_options &&
        (!plan.provider_order.empty() || !config.routing.country.empty())) {
        body["provider"] = providerPayload(plan, candidate, config);
    }
    return body;
}

} // namespace proxy
