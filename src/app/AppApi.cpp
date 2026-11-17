#include "app/App.h"
#include "app/PanelQuery.h"
#include "app/ServiceControl.h"
#include "util/Http.h"
#include "util/Process.h"
#include "util/Ratio.h"
#include "util/Strings.h"

#include <fstream>

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace proxy {
namespace {

// Пределы выборки для таблиц CSV-агрегатов: часовых срезов заметно больше, чем
// строк в остальных списках панели.
constexpr std::size_t kCsvTimesLimit = 200;
// Пауза перед остановкой прокси: ответ панели обязан уйти клиенту раньше, чем
// юнит systemd начнёт остановку (мс).
constexpr int kDeferredRestartDelayMs = 1200;

Json::Value blacklistEntriesJson(const std::vector<ProviderBlockConfig>& entries) {
    Json::Value out(Json::arrayValue);
    for (const auto& block : entries) {
        Json::Value x(Json::objectValue);
        x["provider"] = block.provider;
        Json::Value models(Json::arrayValue);
        for (const auto& model : block.models) models.append(model);
        x["models"] = models;
        x["reason"] = block.reason;
        out.append(x);
    }
    return out;
}

} // namespace

std::optional<Session> ProxyApp::requireAdmin(
    const drogon::HttpRequestPtr& req,
    const std::function<void(const drogon::HttpResponsePtr&)>& cb,
    const std::string& action, bool require_csrf) {
    auto session = requireSession(req, cb, false);
    if (!session) return std::nullopt;
    if (session->role != "admin") {
        cb(errorResponse(drogon::k403Forbidden, "forbidden",
                         "admin role required for " + action));
        return std::nullopt;
    }
    if (require_csrf && !sessions_.validateCsrf(session->id, req->getHeader("x-csrf-token"))) {
        cb(errorResponse(drogon::k403Forbidden, "csrf_failed", "CSRF token mismatch"));
        return std::nullopt;
    }
    return session;
}

std::set<std::string> ProxyApp::providerTags() const {
    std::set<std::string> tags;
    for (const auto& router : config_.routers) {
        if (!router.enabled) continue;
        tags.insert(providerTagOf(router));
    }
    for (const auto& model : config_.routing.models) {
        const auto snap = catalog_.get(model);
        if (!snap) continue;
        for (const auto& offer : snap->endpoints) {
            if (!offer.provider_tag.empty()) tags.insert(offer.provider_tag);
        }
    }
    if (stats_db_ && stats_db_->ok()) {
        for (const auto& tag : stats_db_->providerTags()) tags.insert(tag.asString());
    }
    return tags;
}

// Параметры маршрутизации, которые прокси добавляет в запрос, и порядок
// провайдеров из того же снимка очереди, что и рабочий путь.
Json::Value ProxyApp::routingOptionsJson() const {
    const auto& r = config_.routing;
    Json::Value v(Json::objectValue);
    v["inject_provider_options"] = r.inject_provider_options;
    v["client_provider_policy"] = r.client_provider_policy;
    v["allow_fallbacks"] = false;
    v["country"] = r.country;
    Json::Value quant(Json::arrayValue);
    for (const auto& q : r.quantization) quant.append(q);
    v["quantization"] = quant;
    v["fallback_attempts"] = r.fallback_attempts;
    v["candidate_cap"] = r.candidate_cap;
    v["provider_order_size"] = r.provider_order_size;
    v["session_pin_sec"] = r.session_pin_sec;
    v["session_pin_max"] = r.session_pin_max;
    v["queue_min_sec"] = r.queue_min_sec;
    v["queue_max_sec"] = r.queue_max_sec;
    v["queue_stats_sec"] = r.queue_stats_sec;
    v["wait_price_rub_per_sec"] = r.wait_price_rub_per_sec;
    v["failover_wait_max_sec"] = r.failover_wait_max_sec;
    v["cache_loss_spike_ratio"] = r.cache_loss_spike.ratio;
    v["cache_loss_spike_window_sec"] = r.cache_loss_spike.window_sec;
    v["leader_margin_percent"] = r.leader.margin_percent;
    v["leader_min_dwell_sec"] = r.leader.min_dwell_sec;
    v["output_weight"] = r.output_weight;
    v["cache_weight"] = r.cache_weight;
    v["profile_sample_size"] = r.profile_sample_size;
    v["retry_attempts"] = r.retry.attempts;
    v["retry_backoff_ms"] = r.retry.backoff_ms;
    v["health_fail_threshold"] = r.health.fail_threshold;
    v["health_down_after_sec"] = r.health.down_after_sec;
    v["health_probe_interval_sec"] = r.health.probe_interval_sec;
    v["health_recover_successes"] = r.health.recover_successes;
    v["provider_health"] = proxy_.healthJson();
    const auto snapshot = queue_.snapshot();
    v["queue_built_at"] = snapshot ? static_cast<Json::Int64>(snapshot->built_epoch) : 0;
    v["queue_built_local"] = snapshot ? snapshot->built_local : std::string();

    const auto down = proxy_.downProviders();
    Json::Value models(Json::arrayValue);
    for (const auto& model : r.models) {
        const QueueModelEntry* entry = snapshot ? snapshot->find(model) : nullptr;
        const RoutePlan plan = queue_.planFor(model, down);
        Json::Value m(Json::objectValue);
        m["model"] = model;
        m["profile"] = (entry && !entry->nominal()) ? "observed" : "nominal";
        m["observations"] = entry ? static_cast<Json::Int64>(entry->observations) : 0;
        m["input_tokens"] = entry ? static_cast<Json::Int64>(entry->profile.input_tokens)
                                  : kNominalInputTokens;
        m["output_tokens"] = entry ? static_cast<Json::Int64>(entry->profile.output_tokens)
                                   : kNominalOutputTokens;
        m["cached_tokens"] = entry ? static_cast<Json::Int64>(entry->profile.cached_tokens) : 0;

        Json::Value order(Json::arrayValue);
        for (const auto& tag : plan.provider_priority) order.append(tag);
        m["reason"] = plan.reason;
        m["rejected"] = static_cast<Json::UInt64>(plan.rejected.size());
        m["order"] = order;
        m["leader"] = entry ? entry->leader : std::string();
        m["previous_leader"] = entry ? entry->previous_leader : std::string();
        m["leader_since"] = entry ? static_cast<Json::Int64>(entry->leader_since) : 0;
        m["in_request"] = Json::Value(Json::arrayValue);
        for (const auto& tag : plan.provider_order) m["in_request"].append(tag);
        m["candidates"] = static_cast<Json::UInt64>(plan.candidates.size());
        // Превью собирает тот же код, что и запрос к провайдеру.
        Json::Value inject(Json::Value(Json::nullValue));
        if (r.inject_provider_options && plan.hasRoute()) {
            std::string error;
            const Json::Value upstream =
                buildUpstreamBody(Json::Value(Json::objectValue), plan, plan.candidates.front(),
                                  config_, error);
            if (upstream.isMember("provider")) inject = upstream["provider"];
            if (!error.empty()) m["inject_error"] = error;
        }
        m["inject"] = inject;
        models.append(m);
    }
    v["models"] = models;
    return v;
}

void ProxyApp::handleRoutingOptions(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (req->method() == drogon::Get) {
        if (!requireSession(req, cb, false)) return;
        cb(jsonResponse(routingOptionsJson()));
        return;
    }
    if (!requireAdmin(req, cb, "routing options")) return;
    Json::Value root;
    std::string error;
    if (!parseJson(req->body(), root, error) || !root.isObject()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "JSON object with routing options is required"));
        return;
    }
    const auto& r = config_.routing;
    std::string field_error;
    auto fail = [&](const std::string& message) {
        if (field_error.empty()) field_error = message;
        return false;
    };
    // Ключи панели пишутся в файл конфигурации и обязаны пережить --check:
    // диапазоны здесь те же, что в AppConfig::validate.
    auto int_field = [&](const char* key, std::int64_t min, std::int64_t max,
                         std::int64_t current, std::int64_t& out) {
        if (root[key].isNull()) {
            out = current;
            return true;
        }
        if (!root[key].isInt64() && !root[key].isInt() && !root[key].isUInt()) {
            return fail(std::string(key) + " must be an integer");
        }
        const auto value = root[key].asInt64();
        if (value < min || value > max) {
            return fail(std::string(key) + " must be " + std::to_string(min) + ".." +
                        std::to_string(max));
        }
        out = value;
        return true;
    };
    auto num_field = [&](const char* key, double min, double max, double current,
                         double& out) {
        if (root[key].isNull()) {
            out = current;
            return true;
        }
        if (!root[key].isNumeric()) return fail(std::string(key) + " must be a number");
        const double value = root[key].asDouble();
        if (!(value >= min) || !(value <= max)) {
            return fail(std::string(key) + " must be in [" + std::to_string(min) + ", " +
                        std::to_string(max) + "]");
        }
        out = value;
        return true;
    };
    const bool has_toggle = root["inject_provider_options"].isBool();
    if (!root["inject_provider_options"].isNull() && !has_toggle) {
        fail("inject_provider_options must be boolean");
    }
    const bool enabled = has_toggle ? root["inject_provider_options"].asBool()
                                    : r.inject_provider_options;
    double output_weight = r.output_weight;
    double cache_weight = r.cache_weight;
    int64_t profile_sample_size = r.profile_sample_size;
    int64_t fallback_attempts = r.fallback_attempts;
    int64_t provider_order_size = r.provider_order_size;
    int64_t candidate_cap = r.candidate_cap;
    int64_t session_pin_sec = r.session_pin_sec;
    int64_t session_pin_max = r.session_pin_max;
    double wait_price_rub_per_sec = r.wait_price_rub_per_sec;
    int64_t failover_wait_max_sec = r.failover_wait_max_sec;
    int64_t queue_min_sec = r.queue_min_sec;
    int64_t queue_max_sec = r.queue_max_sec;
    int64_t queue_stats_sec = r.queue_stats_sec;
    int64_t health_fail_threshold = r.health.fail_threshold;
    int64_t health_down_after_sec = r.health.down_after_sec;
    int64_t health_probe_interval_sec = r.health.probe_interval_sec;
    int64_t health_recover_successes = r.health.recover_successes;
    if (field_error.empty()) {
        num_field("output_weight", routing_bounds::kWeightMin, routing_bounds::kWeightMax, r.output_weight, output_weight);
        num_field("cache_weight", routing_bounds::kWeightMin, routing_bounds::kWeightMax, r.cache_weight, cache_weight);
        int_field("profile_sample_size", routing_bounds::kProfileSampleMin, routing_bounds::kProfileSampleMax, r.profile_sample_size, profile_sample_size);
        int_field("fallback_attempts", routing_bounds::kFallbackAttemptsMin, routing_bounds::kFallbackAttemptsMax, r.fallback_attempts, fallback_attempts);
        int_field("provider_order_size", routing_bounds::kProviderOrderMin, routing_bounds::kProviderOrderMax, r.provider_order_size, provider_order_size);
        int_field("candidate_cap", routing_bounds::kCandidateCapMin, routing_bounds::kCandidateCapMax, r.candidate_cap, candidate_cap);
        int_field("session_pin_sec", routing_bounds::kSessionPinSecMin, routing_bounds::kSessionPinSecMax, r.session_pin_sec, session_pin_sec);
        int_field("session_pin_max", routing_bounds::kSessionPinMaxMin, routing_bounds::kSessionPinMaxMax, r.session_pin_max, session_pin_max);
        num_field("wait_price_rub_per_sec", routing_bounds::kWaitPriceMin, routing_bounds::kWaitPriceMax, r.wait_price_rub_per_sec,
                  wait_price_rub_per_sec);
        int_field("failover_wait_max_sec", routing_bounds::kFailoverWaitMinSec, routing_bounds::kFailoverWaitMaxSec, r.failover_wait_max_sec,
                  failover_wait_max_sec);
        int_field("queue_min_sec", routing_bounds::kQueueSecMin, routing_bounds::kQueueSecMax, r.queue_min_sec, queue_min_sec);
        int_field("queue_max_sec", routing_bounds::kQueueSecMin, routing_bounds::kQueueSecMax, r.queue_max_sec, queue_max_sec);
        int_field("queue_stats_sec", routing_bounds::kQueueSecMin, routing_bounds::kQueueSecMax, r.queue_stats_sec, queue_stats_sec);
        int_field("health_fail_threshold", 0, routing_bounds::kHealthFailThresholdMax, r.health.fail_threshold,
                  health_fail_threshold);
        int_field("health_down_after_sec", 0, routing_bounds::kHealthDownAfterSecMax, r.health.down_after_sec,
                  health_down_after_sec);
        int_field("health_probe_interval_sec", routing_bounds::kHealthProbeSecMin, routing_bounds::kHealthProbeSecMax,
                  r.health.probe_interval_sec, health_probe_interval_sec);
        int_field("health_recover_successes", routing_bounds::kHealthRecoverMin, routing_bounds::kHealthRecoverMax,
                  r.health.recover_successes, health_recover_successes);
    }
    const bool any_field = has_toggle || !root["output_weight"].isNull() ||
                           !root["cache_weight"].isNull() ||
                           !root["profile_sample_size"].isNull() ||
                           !root["fallback_attempts"].isNull() ||
                           !root["provider_order_size"].isNull() ||
                           !root["candidate_cap"].isNull() || !root["session_pin_sec"].isNull() ||
                           !root["session_pin_max"].isNull() ||
                           !root["wait_price_rub_per_sec"].isNull() ||
                           !root["failover_wait_max_sec"].isNull() ||
                           !root["queue_min_sec"].isNull() || !root["queue_max_sec"].isNull() ||
                           !root["queue_stats_sec"].isNull() ||
                           !root["health_fail_threshold"].isNull() ||
                           !root["health_down_after_sec"].isNull() ||
                           !root["health_probe_interval_sec"].isNull() ||
                           !root["health_recover_successes"].isNull();
    if (!any_field) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "no routing option in the request"));
        return;
    }
    if (field_error.empty() && queue_min_sec > queue_max_sec) {
        fail("queue_min_sec must be 1..queue_max_sec");
    }
    if (!field_error.empty()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", field_error));
        return;
    }

    // Файл конфигурации — источник значений при старте: сначала пишем его,
    // только потом меняем рабочую конфигурацию и перестраиваем очередь.
    if (!config_.config_path.empty()) {
        // Ключи пишутся по одному: при отказе на середине файл откатывается к
        // исходному, иначе рантайм и файл разошлись бы без следа.
        std::ifstream backup_in(config_.config_path, std::ios::binary);
        const std::string config_backup((std::istreambuf_iterator<char>(backup_in)),
                                        std::istreambuf_iterator<char>());
        if (!backup_in && !backup_in.eof()) {
            // Без читаемой копии откат невозможен: запись не начинается, иначе
            // рантайм и файл разошлись бы без следа.
            cb(errorResponse(drogon::k500InternalServerError, "config_write_failed",
                             "cannot read configuration file: " + config_.config_path));
            return;
        }
        auto save_int = [&](const char* key, std::int64_t value, std::int64_t current) {
            if (value != current) saveRoutingInt(config_.config_path, key, value);
        };
        try {
            if (has_toggle && enabled != r.inject_provider_options) {
                saveRoutingBool(config_.config_path, "inject_provider_options", enabled);
            }
            if (output_weight != r.output_weight) {
                saveRoutingNumber(config_.config_path, "output_weight", output_weight);
            }
            if (cache_weight != r.cache_weight) {
                saveRoutingNumber(config_.config_path, "cache_weight", cache_weight);
            }
            save_int("profile_sample_size", profile_sample_size, r.profile_sample_size);
            save_int("fallback_attempts", fallback_attempts, r.fallback_attempts);
            save_int("provider_order_size", provider_order_size, r.provider_order_size);
            save_int("candidate_cap", candidate_cap, r.candidate_cap);
            save_int("session_pin_sec", session_pin_sec, r.session_pin_sec);
            save_int("session_pin_max", session_pin_max, r.session_pin_max);
            if (wait_price_rub_per_sec != r.wait_price_rub_per_sec) {
                saveRoutingNumber(config_.config_path, "wait_price_rub_per_sec",
                                  wait_price_rub_per_sec);
            }
            save_int("failover_wait_max_sec", failover_wait_max_sec, r.failover_wait_max_sec);
            save_int("queue_min_sec", queue_min_sec, r.queue_min_sec);
            save_int("queue_max_sec", queue_max_sec, r.queue_max_sec);
            save_int("queue_stats_sec", queue_stats_sec, r.queue_stats_sec);
            save_int("health_fail_threshold", health_fail_threshold, r.health.fail_threshold);
            save_int("health_down_after_sec", health_down_after_sec, r.health.down_after_sec);
            save_int("health_probe_interval_sec", health_probe_interval_sec,
                     r.health.probe_interval_sec);
            save_int("health_recover_successes", health_recover_successes,
                     r.health.recover_successes);
        } catch (const std::exception& e) {
            std::ofstream out(config_.config_path, std::ios::binary | std::ios::trunc);
            out << config_backup;
            if (!out) {
                LOG_ERROR << "configuration rollback failed: " << config_.config_path;
            }
            cb(errorResponse(drogon::k500InternalServerError, "config_write_failed", e.what()));
            return;
        }
    }
    // Настройки входят в weight RUB/M и в расписание, поэтому очередь
    // перестраивается сразу: панель, explain и живой запрос получают один снимок.
    queue_.rebuildWith([&] {
        config_.routing.inject_provider_options = enabled;
        config_.routing.output_weight = output_weight;
        config_.routing.cache_weight = cache_weight;
        config_.routing.profile_sample_size = static_cast<int>(profile_sample_size);
        config_.routing.fallback_attempts = static_cast<int>(fallback_attempts);
        config_.routing.provider_order_size = static_cast<int>(provider_order_size);
        config_.routing.candidate_cap = static_cast<int>(candidate_cap);
        config_.routing.session_pin_sec = static_cast<int>(session_pin_sec);
        config_.routing.session_pin_max = static_cast<int>(session_pin_max);
        config_.routing.wait_price_rub_per_sec = wait_price_rub_per_sec;
        config_.routing.failover_wait_max_sec = static_cast<int>(failover_wait_max_sec);
        config_.routing.queue_min_sec = static_cast<int>(queue_min_sec);
        config_.routing.queue_max_sec = static_cast<int>(queue_max_sec);
        config_.routing.queue_stats_sec = static_cast<int>(queue_stats_sec);
        config_.routing.health.fail_threshold = static_cast<int>(health_fail_threshold);
        config_.routing.health.down_after_sec = static_cast<int>(health_down_after_sec);
        config_.routing.health.probe_interval_sec = static_cast<int>(health_probe_interval_sec);
        config_.routing.health.recover_successes = static_cast<int>(health_recover_successes);
    });
    Json::Value v = routingOptionsJson();
    v["ok"] = true;
    v["path"] = config_.config_path;
    v["applied"] = true;
    cb(jsonResponse(v));
}

void ProxyApp::handleRoutingPreview(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    const std::string requested = req->getParameter("model").empty()
                                      ? config_.routing.default_model
                                      : req->getParameter("model");
    const std::string resolved = config_.resolveModel(requested);
    if (!config_.modelConfigured(resolved)) {
        cb(errorResponse(drogon::k404NotFound, "model_not_found", "unknown model: " + requested));
        return;
    }
    int sample = config_.routing.profile_sample_size;
    if (!req->getParameter("sample").empty()) {
        try {
            sample = std::stoi(req->getParameter("sample"));
        } catch (...) {
            cb(errorResponse(drogon::k400BadRequest, "invalid_request", "invalid sample"));
            return;
        }
    }
    sample = std::max(routing_bounds::kProfileSampleMin,
                      std::min(sample, routing_bounds::kProfileSampleMax));
    auto weight_param = [&](const char* name, double current, double& out) {
        if (req->getParameter(name).empty()) {
            out = current;
            return true;
        }
        try {
            out = std::stod(req->getParameter(name));
        } catch (...) {
            return false;
        }
        return std::isfinite(out) && out >= routing_bounds::kWeightMin &&
               out <= routing_bounds::kWeightMax;
    };
    double output_weight = config_.routing.output_weight;
    double cache_weight = config_.routing.cache_weight;
    double wait_price = config_.routing.wait_price_rub_per_sec;
    if (!weight_param("output_weight", config_.routing.output_weight, output_weight) ||
        !weight_param("cache_weight", config_.routing.cache_weight, cache_weight) ||
        !weight_param("wait_price_rub_per_sec", config_.routing.wait_price_rub_per_sec,
                      wait_price)) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "output_weight, cache_weight and wait_price_rub_per_sec "
                         "must be in [0, 100]"));
        return;
    }
    // Предпросмотр — тот же расчёт очереди, но на временной копии конфигурации:
    // рабочий снимок не меняется, значения применяются только сохранением.
    AppConfig preview_config = config_;
    preview_config.routing.output_weight = output_weight;
    preview_config.routing.cache_weight = cache_weight;
    preview_config.routing.wait_price_rub_per_sec = wait_price;
    const ObservedProfile profile = stats_.observedProfile(resolved, sample);
    const bool observed = profile.observations > 0;
    const CostMix mix{observed ? profile.input_tokens : kNominalInputTokens,
                      observed ? profile.output_tokens : kNominalOutputTokens,
                      observed ? profile.cached_tokens : 0};
    Router preview_router(preview_config, catalog_);
    const auto quotes = preview_router.quotes(
        resolved, mix,
        [this, &resolved, sample](const std::string& provider) {
            return stats_.latencyMs(resolved, provider, sample);
        });
    const auto snapshot = queue_.snapshot();
    const QueueModelEntry* entry = snapshot ? snapshot->find(resolved) : nullptr;
    const std::string leader = entry ? entry->leader : std::string();

    // Тот же профиль и те же цены, что у рабочей очереди: карточки предпросмотра
    // показывают вклад компонентов на средний запрос «сейчас» и «с новыми
    // значениями», не пересчитывая рабочий маршрут. Стоимость считается здесь
    // заново осознанно: у предпросмотра другой вход (ещё не сохранённые веса),
    // поэтому опубликованные значения снимка к нему неприменимы.
    const CostMix current_mix =
        entry && entry->profile.input_tokens + entry->profile.output_tokens > 0
            ? entry->profile
            : mix;
    const RouteCandidate* current_quote = nullptr;
    if (entry) {
        for (const auto& candidate : entry->quotes) {
            if (candidate.excluded.empty() && candidate.provider_tag == entry->leader) {
                current_quote = &candidate;
                break;
            }
        }
    }
    const RouteCandidate* preview_quote = nullptr;
    for (const auto& candidate : quotes) {
        if (candidate.excluded.empty()) {
            preview_quote = &candidate;
            break;
        }
    }
    struct ProfileCosts {
        std::string provider;
        bool observed = false;
        std::int64_t input_tokens = 0;
        std::int64_t output_tokens = 0;
        std::int64_t cached_tokens = 0;
        double cache_hit_ratio = 0.0;
        NanoRub input_nano = 0;  // вход вместе с записью в кеш
        NanoRub cache_nano = 0;  // кеш с поправочным весом
        NanoRub output_nano = 0; // выход с поправочным весом
        NanoRub latency_nano = 0;
        NanoRub fixed_nano = 0;
        NanoRub total_nano = 0;  // фактическая стоимость среднего запроса
        NanoRub weight_nano = 0; // стоимость по формуле веса, на запрос
    };
    const auto profile_costs = [&](const CostMix& profile, const RouteCandidate* quote,
                                   double output_w, double cache_w,
                                   bool observed_profile) {
        ProfileCosts pc;
        pc.provider = quote ? quote->provider_tag : std::string();
        pc.observed = observed_profile;
        pc.input_tokens = profile.input_tokens;
        pc.output_tokens = profile.output_tokens;
        pc.cached_tokens = profile.cached_tokens;
        pc.cache_hit_ratio = profile.cacheHitRatio();
        if (!quote) return pc;
        // Тот же профиль и та же формула, что у очереди: доля кеша ограничивается
        // ёмкостью кеша оффера (mixForOffer), вес считает rankCost.
        const CostMix effective =
            mixForOffer(quote->offer, profile.input_tokens, profile.output_tokens,
                        profile.cacheHitRatio());
        const CostBreakdown cost = quote->cost(effective, config_.pricing.threshold_mode);
        // Цена задержки — из оффера: её посчитал тот же расчёт очереди, с теми же
        // (ещё не сохранёнными) весами предпросмотра. Пересчёт здесь давал бы
        // другую цену ожидания — по сохранённому `wait_price_rub_per_sec`.
        const NanoRub latency_nano = quote->latency_cost_nano;
        const RankCost rank =
            rankCost(cost, quote->fixed_fee_nano, latency_nano, output_w, cache_w);
        pc.input_tokens = effective.input_tokens;
        pc.output_tokens = effective.output_tokens;
        pc.cached_tokens = effective.cached_tokens;
        pc.cache_hit_ratio = effective.cacheHitRatio();
        pc.input_nano = rank.input_nano;
        pc.cache_nano = rank.cache_nano;
        pc.output_nano = rank.output_nano;
        pc.latency_nano = rank.latency_nano;
        pc.fixed_nano = rank.fixed_nano;
        pc.total_nano = cost.total_nano;
        pc.weight_nano = rank.rank_nano;
        return pc;
    };
    const ProfileCosts current =
        profile_costs(current_mix, current_quote, config_.routing.output_weight,
                      config_.routing.cache_weight,
                      entry ? !entry->nominal() : false);
    const ProfileCosts preview =
        profile_costs(mix, preview_quote, output_weight, cache_weight, observed);
    const auto costs_json = [](const ProfileCosts& pc) {
        Json::Value x(Json::objectValue);
        x["provider"] = pc.provider;
        x["observed"] = pc.observed;
        x["input_tokens"] = static_cast<Json::Int64>(pc.input_tokens);
        x["output_tokens"] = static_cast<Json::Int64>(pc.output_tokens);
        x["cached_tokens"] = static_cast<Json::Int64>(pc.cached_tokens);
        x["cache_hit_ratio"] = pc.cache_hit_ratio;
        x["input_rub"] = formatRubDisplay(pc.input_nano);
        x["cache_rub"] = formatRubDisplay(pc.cache_nano);
        x["output_rub"] = formatRubDisplay(pc.output_nano);
        x["latency_rub"] = formatRubDisplay(pc.latency_nano);
        x["fixed_rub"] = formatRubDisplay(pc.fixed_nano);
        x["total_rub"] = formatRubDisplay(pc.total_nano);
        x["weight_rub"] = formatRubDisplay(pc.weight_nano);
        return x;
    };

    Json::Value v(Json::objectValue);
    v["model"] = resolved;
    v["sample"] = sample;
    v["output_weight"] = output_weight;
    v["cache_weight"] = cache_weight;
    v["wait_price_rub_per_sec"] = wait_price;
    v["observed"] = observed;
    v["profile_source"] = profile.source == ProfileSource::Window
                              ? "window"
                              : (profile.source == ProfileSource::Journal
                                     ? "journal"
                                     : (profile.source == ProfileSource::Memory ? "memory"
                                                                                : "none"));
    v["observations"] = static_cast<Json::Int64>(profile.observations);
    v["input_tokens"] = static_cast<Json::Int64>(mix.input_tokens);
    v["output_tokens"] = static_cast<Json::Int64>(mix.output_tokens);
    v["cached_tokens"] = static_cast<Json::Int64>(mix.cached_tokens);
    v["cache_hit_ratio"] = mix.cacheHitRatio();
    v["leader"] = leader;
    v["preview_leader"] = preview_quote ? preview_quote->provider_tag : std::string();
    v["leader_weight_rub"] = Json::Value(Json::nullValue);
    v["current"] = costs_json(current);
    v["preview"] = costs_json(preview);
    Json::Value rows(Json::arrayValue);
    for (const auto& c : quotes) {
        Json::Value row(Json::objectValue);
        row["provider"] = c.provider_tag;
        row["router"] = c.router_id;
        row["weight_rub"] = formatRub(c.weight_per_million_nano, 4);
        row["token_weight_rub"] = formatRub(c.cost_per_million_nano, 4);
        row["latency_ms"] = c.latency_ms;
        row["latency_weight_rub"] = formatRub(c.latency_cost_per_million_nano, 4);
        row["cost_rub"] = formatRubDisplay(c.cost_nano);
        row["cache_hit_ratio"] = c.cache_hit_ratio;
        row["cache_read_rub"] = formatRubPerMillion(c.cache_read_price_nano, 4);
        row["excluded"] = c.excluded;
        row["excluded_detail"] = c.excluded_detail;
        if (c.excluded.empty() && c.provider_tag == leader) {
            v["leader_weight_rub"] = formatRub(c.weight_per_million_nano);
        }
        rows.append(row);
    }
    v["rows"] = rows;
    cb(jsonResponse(v));
}

void ProxyApp::handleCacheReset(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireAdmin(req, cb, "cache reset")) return;
    Json::Value root;
    std::string error;
    if (!parseJson(req->body(), root, error) || !root.isObject()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "JSON body with scope and value is required"));
        return;
    }
    const std::string scope = trim(root["scope"].asString());
    const std::string value = trim(root["value"].asString());
    const bool known_scope = scope == kCacheScopeProvider || scope == kCacheScopeModel ||
                             scope == kCacheScopeSession;
    if (!known_scope || value.empty() || value.find_first_of("\r\n") != std::string::npos) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "scope must be provider, model or session and value must be non-empty"));
        return;
    }
    if (!stats_.resetCache(scope, value)) {
        cb(errorResponse(drogon::k503ServiceUnavailable, "cache_reset_failed",
                         "cache reset applied in memory but not persisted"));
        return;
    }
    Json::Value v(Json::objectValue);
    v["ok"] = true;
    v["scope"] = scope;
    v["value"] = value;
    cb(jsonResponse(v));
}

void ProxyApp::handleOverview(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireSession(req, cb, false);
    if (!session) return;
    (void)session;
    Json::Value v = overviewJson();
    v["catalog_models"] = static_cast<Json::UInt64>(catalog_.size());
    v["routers"] = static_cast<Json::UInt64>(config_.routers.size());
    v["capture_enabled"] = config_.capture.enabled;
    v["body_count"] = static_cast<Json::UInt64>(bodies_.count());
    v["body_bytes"] = static_cast<Json::UInt64>(bodies_.storedBytes());
    v["price_last_error"] = fetcher_.lastError();
    v["instance"] = config_.instance_name;
    v["provider_analytics"] = analytics_.summaryJson();
    cb(jsonResponse(v));
}

void ProxyApp::handleDailyStats(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    int days = panel_limits::kDailyDaysDefault;
    try {
        if (!req->getParameter("days").empty()) {
            days = std::stoi(req->getParameter("days"));
        }
    } catch (...) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", "invalid days"));
        return;
    }
    days = std::max(1, std::min(days, panel_limits::kDailyDaysMax));
    Json::Value v(Json::objectValue);
    v["days"] = days;
    v["enabled"] = stats_db_ && stats_db_->ok();
    v["path"] = stats_db_ ? stats_db_->path() : "";
    v["daily"] = stats_db_ ? stats_db_->daily(days) : Json::Value(Json::arrayValue);
    cb(jsonResponse(v));
}

void ProxyApp::handleSessions(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    const auto limit = queryLimit(req, panel_limits::kListDefault, panel_limits::kListMax);
    if (!limit) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", "limit must be a number"));
        return;
    }
    Json::Value v(Json::objectValue);
    v["source"] = stats_.sourceName();
    v["sessions"] = stats_.sessionsJson(*limit);
    cb(jsonResponse(v));
}

void ProxyApp::handleMetrics(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!config_.stats.prometheus) {
        cb(errorResponse(drogon::k404NotFound, "not_found", "metrics are disabled"));
        return;
    }
    if (config_.stats.require_token) {
        auto principal = tokens_.authenticate(req->getHeader("authorization"), clientIp(req));
        if (!principal || !principal->hasScope("stats:read")) {
            cb(errorResponse(drogon::k401Unauthorized, "unauthorized",
                             "valid Bearer token with stats:read scope required"));
            return;
        }
    }
    std::string body = stats_.prometheus();
    body += "# HELP costrelay_catalog_models Models present in the price catalog.\n";
    body += "# TYPE costrelay_catalog_models gauge\n";
    body += "costrelay_catalog_models " + std::to_string(catalog_.size()) + "\n";
    body += "# HELP costrelay_up Proxy process is running.\n";
    body += "# TYPE costrelay_up gauge\ncostrelay_up 1\n";
    body += "# HELP costrelay_stats_db_available Persistent statistics store is available.\n";
    body += "# TYPE costrelay_stats_db_available gauge\n";
    body += std::string("costrelay_stats_db_available ") +
            (stats_db_ && stats_db_->ok() ? "1" : "0") + "\n";
    auto resp = textResponse(body, drogon::k200OK,
                             "text/plain; version=0.0.4; charset=utf-8");
    resp->addHeader("Cache-Control", "no-store");
    cb(resp);
}

void ProxyApp::handleProviderLogs(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    const auto limit = queryLimit(req, panel_limits::kListDefault, panel_limits::kListMax);
    if (!limit) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", "limit must be a number"));
        return;
    }
    Json::Value v(Json::objectValue);
    v["summary"] = analytics_.summaryJson();
    v["providers"] = analytics_.providersJson(*limit);
    v["models"] = analytics_.modelsJson(*limit);
    v["times"] = analytics_.timesJson(kCsvTimesLimit);
    cb(jsonResponse(v));
}

void ProxyApp::handleProviderLogImport(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireAdmin(req, cb, "provider logs")) return;
    if (req->body().empty()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", "CSV body is required"));
        return;
    }
    if (req->body().size() > config_.analytics.max_file_bytes) {
        cb(errorResponse(drogon::k413RequestEntityTooLarge, "too_large",
                         "CSV exceeds analytics.max_file_bytes"));
        return;
    }
    auto result = analytics_.importCsvContent(req->body(), "panel-upload");
    if (result.ok && !config_.analytics.snapshot_file.empty() && !analytics_.saveSnapshot()) {
        LOG_WARN << "provider analytics snapshot not saved: " << analytics_.lastError();
    }
    if (result.ok && !result.duplicate) queue_.build();
    Json::Value v(Json::objectValue);
    v["ok"] = result.ok;
    v["duplicate"] = result.duplicate;
    v["files"] = static_cast<Json::UInt64>(result.files);
    v["rows"] = static_cast<Json::UInt64>(result.rows);
    v["skipped"] = static_cast<Json::UInt64>(result.skipped);
    v["bad_fields"] = static_cast<Json::UInt64>(result.bad_fields);
    v["error"] = result.error;
    v["summary"] = analytics_.summaryJson();
    if (!result.ok) {
        cb(jsonResponse(v, drogon::k400BadRequest));
        return;
    }
    cb(jsonResponse(v));
}

void ProxyApp::handleProviderLogReload(const drogon::HttpRequestPtr& req,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireAdmin(req, cb, "provider logs")) return;
    auto result = analytics_.importDirectory();
    if (result.ok && !config_.analytics.snapshot_file.empty() && !analytics_.saveSnapshot()) {
        LOG_WARN << "provider analytics snapshot not saved: " << analytics_.lastError();
    }
    if (result.ok) queue_.build();
    Json::Value v(Json::objectValue);
    v["ok"] = result.ok;
    v["files"] = static_cast<Json::UInt64>(result.files);
    v["rows"] = static_cast<Json::UInt64>(result.rows);
    v["skipped"] = static_cast<Json::UInt64>(result.skipped);
    v["bad_fields"] = static_cast<Json::UInt64>(result.bad_fields);
    v["error"] = result.error;
    v["summary"] = analytics_.summaryJson();
    cb(jsonResponse(v, result.ok ? drogon::k200OK : drogon::k400BadRequest));
}

void ProxyApp::handleBlacklistGet(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    Json::Value v(Json::objectValue);
    v["entries"] = blacklistEntriesJson(config_.routing.provider_blacklist);
    cb(jsonResponse(v));
}

void ProxyApp::handleBlacklistSave(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireAdmin(req, cb, "blacklist")) return;
    Json::Value root;
    std::string error;
    if (!parseJson(req->body(), root, error) || !root.isObject() ||
        !root["entries"].isArray()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "JSON body with an entries array is required"));
        return;
    }
    std::vector<ProviderBlockConfig> entries;
    for (const auto& item : root["entries"]) {
        if (!item.isObject()) {
            cb(errorResponse(drogon::k400BadRequest, "invalid_request", "entry must be an object"));
            return;
        }
        ProviderBlockConfig b;
        b.provider = trim(item["provider"].asString());
        b.reason = trim(item["reason"].asString());
        if (b.reason.empty()) b.reason = kDefaultBlockReason;
        if (b.provider.empty() || b.provider.find_first_of("\r\n") != std::string::npos ||
            b.reason.find_first_of("\r\n") != std::string::npos) {
            cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                             "provider is required and fields must not contain CR/LF"));
            return;
        }
        auto models = item["models"];
        b.models.clear();
        if (models.isArray()) {
            for (const auto& m : models) {
                if (m.isString()) {
                    const std::string value = trim(m.asString());
                    if (!value.empty()) b.models.push_back(value);
                }
            }
        } else if (models.isString()) {
            for (const auto& value : split(models.asString(), ',')) {
                const std::string trimmed = trim(value);
                if (!trimmed.empty()) b.models.push_back(trimmed);
            }
        }
        if (b.models.empty()) b.models.push_back("*");
        for (const auto& m : b.models) {
            if (m.find_first_of("\r\n") != std::string::npos) {
                cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                                 "models must not contain CR/LF"));
                return;
            }
        }
        entries.push_back(std::move(b));
    }
    if (entries.size() > panel_limits::kListMax) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", "too many blacklist entries"));
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(blacklist_mu_);
        saveProviderBlockList(config_.config_path, "blacklist", entries);
        // Политика меняется и очередь перестраивается под одним замком.
        queue_.rebuildWith([&] { config_.routing.provider_blacklist = std::move(entries); });
    } catch (const std::exception& e) {
        cb(errorResponse(drogon::k500InternalServerError, "config_write_failed", e.what()));
        return;
    }
    Json::Value v(Json::objectValue);
    v["ok"] = true;
    v["path"] = config_.config_path;
    v["applied"] = true;
    v["entries"] = blacklistEntriesJson(config_.routing.provider_blacklist);
    cb(jsonResponse(v));
}

void ProxyApp::blacklistProviderFromStats(const std::string& provider, const std::string& model,
                                          const std::string& reason) {
    if (provider.empty() || model.empty()) return;
    std::lock_guard<std::mutex> lock(blacklist_mu_);
    // Пара (модель, провайдер) уже исключена — второй записи не нужно.
    if (!providerBlockReason(config_, model, provider).empty()) return;
    std::vector<ProviderBlockConfig> entries = config_.routing.provider_blacklist;
    ProviderBlockConfig entry;
    entry.provider = provider;
    entry.models = {model};
    entry.reason = reason;
    entries.push_back(std::move(entry));
    bool saved = false;
    if (!config_.config_path.empty()) {
        try {
            saveProviderBlockList(config_.config_path, "blacklist", entries);
            saved = true;
        } catch (const std::exception& e) {
            LOG_ERROR << "auto blacklist write failed: " << e.what();
        }
    }
    // Провайдер исключён: политика меняется и очередь перестраивается под одним замком.
    queue_.rebuildWith([&] { config_.routing.provider_blacklist = std::move(entries); });
    LOG_WARN << "auto blacklist provider=" << provider << " model=" << model
             << " reason=" << reason << (saved ? "" : " (не сохранено в конфиг)");
}

void ProxyApp::handleRouters(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    Json::Value arr(Json::arrayValue);
    for (const auto& r : config_.routers) {
        if (!routerVisible(config_, r)) continue;
        Json::Value x(Json::objectValue);
        x["id"] = r.id;
        x["base_url"] = r.base_url;
        x["kind"] = r.kind;
        x["enabled"] = r.enabled;
        x["direct"] = (r.kind == "openai_compatible");
        x["provider_tag"] = providerTagOf(r);
        x["price_multiplier"] = static_cast<double>(r.price_multiplier);
        x["prompt_per_million_rub"] = static_cast<double>(r.prompt_per_million_rub);
        x["completion_per_million_rub"] = static_cast<double>(r.completion_per_million_rub);
        x["context_length"] = r.context_length;
        x["max_completion_tokens"] = r.max_completion_tokens;
        x["max_concurrent"] = r.max_concurrent;
        x["http2"] = r.http2;
        Json::Value model_map(Json::objectValue);
        for (const auto& [from, to] : r.model_map) model_map[from] = to;
        x["model_map"] = model_map;
        Json::Value models(Json::arrayValue);
        for (const auto& m : r.models) models.append(m);
        x["models"] = models;
        arr.append(x);
    }
    Json::Value v(Json::objectValue);
    v["routers"] = arr;
    cb(jsonResponse(v));
}

void ProxyApp::handleExplain(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    const std::string model = req->getParameter("model").empty()
                                  ? config_.routing.default_model
                                  : req->getParameter("model");
    const std::string resolved = config_.resolveModel(model);
    if (!config_.modelConfigured(resolved)) {
        cb(errorResponse(drogon::k404NotFound, "model_not_found", "unknown model: " + model));
        return;
    }
    // Explain показывает текущую очередь как есть: маршрут не пересчитывается
    // ни под токены запроса, ни под параметры клиента.
    cb(jsonResponse(proxy_.explain(resolved)));
}

void ProxyApp::handleRequests(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (!requireSession(req, cb, false)) return;
    const auto limit = queryLimit(req, panel_limits::kLogDefault, panel_limits::kLogMax);
    const auto offset = queryOffset(req);
    if (!limit || !offset) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "limit and offset must be numbers"));
        return;
    }
    const std::string session_id = trim(req->getParameter("session_id"));
    const std::string model = trim(req->getParameter("model"));
    const std::string provider = trim(req->getParameter("provider"));
    const std::string issue_param = trim(req->getParameter("issue"));
    const std::string day = trim(req->getParameter("day"));
    if (!day.empty() && !utcDayValid(day)) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "day must be YYYY-MM-DD"));
        return;
    }
    const IssueFilter issue = issueFilterFromString(issue_param);
    Json::Value v(Json::objectValue);
    v["source"] = stats_.sourceName();
    v["limit"] = static_cast<Json::Int64>(*limit);
    v["offset"] = *offset;
    if (!session_id.empty()) v["session_id"] = session_id;
    if (!model.empty()) v["model"] = model;
    if (!provider.empty()) v["provider"] = provider;
    if (!issue_param.empty()) v["issue"] = issue_param;
    if (!day.empty()) v["day"] = day;
    v["requests"] = stats_.requestsJson(*limit, session_id, model, provider, *offset, issue, day);
    cb(jsonResponse(v));
}

void ProxyApp::handleRequestsDelete(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireAdmin(req, cb, "request log");
    if (!session) return;
    if (!stats_db_ || !stats_db_->ok()) {
        cb(errorResponse(drogon::k409Conflict, "stats_disabled",
                         "request log deletion requires the SQLite statistics store"));
        return;
    }
    Json::Value root;
    std::string error;
    if (!parseJson(req->body(), root, error) || !root.isObject()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "JSON body with session_id/model/provider filters is required"));
        return;
    }
    const std::string session_id = trim(root["session_id"].asString());
    const std::string model = trim(root["model"].asString());
    const std::string provider = trim(root["provider"].asString());
    const std::string issue_param = trim(root["issue"].asString());
    const std::string day = trim(root["day"].asString());
    if (!day.empty() && !utcDayValid(day)) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "day must be YYYY-MM-DD"));
        return;
    }
    const IssueFilter issue = issueFilterFromString(issue_param);
    // Пустой фильтр = весь журнал: удаление без фильтров требует явного "all".
    const bool all = root["all"].isBool() && root["all"].asBool();
    if (session_id.empty() && model.empty() && provider.empty() && day.empty() &&
        issue == IssueFilter::All && !all) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "filters or {\"all\": true} are required to delete the whole log"));
        return;
    }
    const int deleted = stats_db_->deleteRequests(session_id, model, provider, issue, day);
    if (deleted < 0) {
        cb(errorResponse(drogon::k500InternalServerError, "delete_failed", "cannot delete request log"));
        return;
    }
    stats_.forgetRequests(session_id, model, provider, issue, day);
    LOG_INFO << "panel log delete user=" << session->username
             << " deleted=" << deleted << " session=" << (session_id.empty() ? "*" : session_id)
             << " model=" << (model.empty() ? "*" : model) << " provider="
             << (provider.empty() ? "*" : provider)
             << " issue=" << (issue_param.empty() ? "*" : issue_param);
    Json::Value v(Json::objectValue);
    v["ok"] = true;
    v["deleted"] = deleted;
    cb(jsonResponse(v));
}

void ProxyApp::handleStatsPage(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireSession(req, cb, true);
    if (!session) return;
    cb(statsPage(*session, req));
}

void ProxyApp::handleProvidersPage(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireSession(req, cb, true);
    if (!session) return;
    cb(providersPage(*session, req));
}

void ProxyApp::handleRoutingPage(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireSession(req, cb, true);
    if (!session) return;
    cb(routingPage(*session));
}

void ProxyApp::handleServiceRestart(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireAdmin(req, cb, "service restart");
    if (!session) return;
    std::string service;
    Json::Value root;
    std::string error;
    if (parseJson(req->body(), root, error) && root.isObject() &&
        root["service"].isString()) {
        service = trim(root["service"].asString());
    }
    if (service.empty()) service = trim(req->getParameter("service"));
    const auto* target = findServiceTarget(service);
    if (!target) {
        std::string keys;
        for (const auto& item : serviceTargets()) {
            if (!keys.empty()) keys += ", ";
            keys += item.key;
        }
        cb(errorResponse(drogon::k400BadRequest, "invalid_request",
                         "service must be one of: " + keys));
        return;
    }
    if (!systemUnitInstalled(target->unit)) {
        cb(errorResponse(drogon::k503ServiceUnavailable, "service_unit_missing",
                         std::string("unit ") + target->unit +
                             " is not installed; run sudo deploy/install-service.sh"));
        return;
    }

    LOG_INFO << "panel service restart user=" << session->username
             << " service=" << target->key << " unit=" << target->unit;

    const auto command = restartServiceCommand(target->unit);
    if (deferredRestart(target->key)) {
        std::thread([unit = std::string(target->unit)]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(kDeferredRestartDelayMs));
            const auto result = runProcess(restartServiceCommand(unit));
            if (!result.ok()) {
                LOG_WARN << "proxy restart failed unit=" << unit
                         << " exit=" << result.exit_code << " output=" << trim(result.output);
            }
        }).detach();
        Json::Value v(Json::objectValue);
        v["ok"] = true;
        v["accepted"] = true;
        v["service"] = target->key;
        v["unit"] = target->unit;
        v["message"] = "CostRelay перезапускается";
        cb(jsonResponse(v));
        return;
    }

    const auto result = runProcess(command);
    Json::Value v(Json::objectValue);
    v["service"] = target->key;
    v["unit"] = target->unit;
    v["exit_code"] = result.exit_code;
    v["ok"] = result.ok();
    v["output"] = trim(result.output);
    if (result.ok()) {
        v["message"] = std::string(target->label) + ": перезапуск запущен";
        cb(jsonResponse(v));
    } else {
        v["message"] = "не удалось запустить перезапуск (проверьте sudo-правила)";
        cb(jsonResponse(v, drogon::k502BadGateway));
    }
}

void ProxyApp::handleBody(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = requireAdmin(req, cb, "bodies", false);
    if (!session) return;
    const std::string id = req->getParameter("request_id");
    const std::string kind = req->getParameter("kind").empty() ? "response" : req->getParameter("kind");
    if (id.empty()) {
        cb(errorResponse(drogon::k400BadRequest, "invalid_request", "request_id is required"));
        return;
    }
    auto body = bodies_.get(id, kind);
    if (!body) {
        cb(errorResponse(drogon::k404NotFound, "not_found", "body not captured or expired"));
        return;
    }
    LOG_INFO << "panel body access user=" << session->username << " request_id=" << id
             << " kind=" << kind;
    auto resp = textResponse(*body, drogon::k200OK, "text/plain; charset=utf-8");
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + id + "-" + kind + ".txt\"");
    resp->addHeader("Cache-Control", "no-store");
    cb(resp);
}


} // namespace proxy
