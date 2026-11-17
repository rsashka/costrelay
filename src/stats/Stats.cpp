#include "stats/Stats.h"
#include "stats/StatsDb.h"
#include "util/Ratio.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <sstream>

namespace proxy {
namespace {

std::string writeJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

bool inUtcDay(const std::chrono::system_clock::time_point& ts, const std::string& day) {
    if (day.empty() || ts.time_since_epoch().count() == 0) return day.empty();
    return utcDayString(ts) == day;
}

// Вытесняется самая старая запись, а не первый ключ (порядок в std::map —
// лексикографический, к возрасту отношения не имеет).
template <typename Map, typename Age>
void evictOldest(Map& map, Age age) {
    if (map.empty()) return;
    auto oldest = map.begin();
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (age(it->second) < age(oldest->second)) oldest = it;
    }
    map.erase(oldest);
}

std::string escapeLabel(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') { out += "\\n"; continue; }
        out.push_back(c);
    }
    return out;
}

} // namespace

int issueSeverity(int status, const std::string& error_code, int attempts) {
    if (status >= 400) return kIssueError;
    // Блокировка провайдера — уже не предупреждение, а принятое действие.
    if (error_code == kCacheMissBlacklistCode) return kIssueError;
    // Повтор после отказа — предупреждение, даже если запрос в итоге выполнен.
    if (!error_code.empty() || attempts > 1) return kIssueWarning;
    return 0;
}

bool issueMatches(int severity, IssueFilter filter) {
    switch (filter) {
        case IssueFilter::Warning:
            return severity == kIssueWarning;
        case IssueFilter::Error:
            return severity >= kIssueError;
        case IssueFilter::Any:
            return severity >= kIssueWarning;
        case IssueFilter::All:
            break;
    }
    return true;
}

// Единственное правило отбора записей журнала: им пользуются память (кольцо) и
// удаление записей, поэтому список и счётчик всегда описывают одно и то же.
bool matchesRequestFilters(const RequestRecord& r, const std::string& session_id,
                           const std::string& model, const std::string& provider,
                           IssueFilter issue, const std::string& day) {
    if (!session_id.empty() && r.session_id != session_id) return false;
    if (!model.empty() && r.model != model) return false;
    if (!provider.empty() && r.provider_tag != provider) return false;
    if (!issueMatches(issueSeverity(r.status, r.error_code, r.attempts), issue)) return false;
    if (!inUtcDay(r.ts, day)) return false;
    return true;
}

IssueFilter issueFilterFromString(const std::string& value) {
    if (value == "warning") return IssueFilter::Warning;
    if (value == "error") return IssueFilter::Error;
    if (value == "issue") return IssueFilter::Any;
    return IssueFilter::All;
}

namespace {

// Единственный расчёт профиля из накопленных сумм: средние токены на запрос.
ObservedProfile observedFrom(std::int64_t observations, std::int64_t tokens_in,
                             std::int64_t tokens_out, std::int64_t cache_read,
                             std::int64_t cache_observations) {
    ObservedProfile profile;
    if (observations <= 0 || tokens_in + tokens_out <= 0) return profile;
    profile.observations = observations;
    profile.input_tokens = tokens_in / observations;
    profile.output_tokens = tokens_out / observations;
    profile.cached_tokens =
        cache_observations > 0
            ? std::clamp<std::int64_t>(cache_read / cache_observations, 0, profile.input_tokens)
            : 0;
    return profile;
}

} // namespace

Stats::Stats(int ring_size, CacheLossSpikeConfig spike, StatsLimits limits,
             CacheMissConfig cache_miss)
    : limits_(limits),
      ring_size_(std::max(1, ring_size)),
      spike_(std::move(spike)),
      cache_miss_(cache_miss) {
    limits_.series = std::max<std::size_t>(1, limits_.series);
    limits_.cache_streaks = std::max<std::size_t>(1, limits_.cache_streaks);
    limits_.sessions = std::max<std::size_t>(1, limits_.sessions);
    limits_.session_models = std::max<std::size_t>(1, limits_.session_models);
}

void Stats::attach(StatsDb* db) {
    db_ = db;
    if (!db_ || !db_->ok()) return;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& item : db_->usageByModelProvider("", true)) {
        auto& totals = models_[item["model"].asString()];
        totals.observations += item["token_observations"].asInt64();
        totals.tokens_in += item["tokens_in"].asInt64();
        totals.tokens_out += item["tokens_out"].asInt64();
        totals.cache_read_tokens += item["cache_read_tokens"].asInt64();
        totals.cache_reset_observations +=
            item["token_observations"].asInt64() - item["cache_observations"].asInt64();
        auto& latency = totals.latency[item["provider"].asString()];
        latency.observations += item["latency_observations"].asInt64();
        latency.latency_ms_sum += item["latency_ms_sum"].asInt64();
    }
    for (const auto& item : db_->cacheResets()) {
        reset_epochs_[item["scope"].asString() + "\x1f" + item["value"].asString()] =
            item["reset_ts"].asInt64();
    }
}

std::int64_t Stats::resetEpochLocked(const RequestRecord& r) const {
    const auto lookup = [&](const char* scope, const std::string& value) {
        const auto it = reset_epochs_.find(std::string(scope) + "\x1f" + value);
        return it == reset_epochs_.end() ? std::int64_t{0} : it->second;
    };
    return std::max({lookup(kCacheScopeModel, r.model), lookup(kCacheScopeSession, r.session_id),
                     lookup(kCacheScopeProvider, r.provider_tag)});
}

std::string Stats::sessionModelKey(const std::string& session, const std::string& model) {
    return session + "\x1f" + model;
}

std::string Stats::providerCacheKey(const std::string& session, const std::string& model,
                                    const std::string& provider) {
    return session + "\x1f" + model + "\x1f" + provider;
}

std::string Stats::sessionKey(const RequestRecord& r) {
    if (!r.session_id.empty()) return r.session_id;
    if (!r.token_id.empty()) return "token:" + r.token_id;
    return "anonymous";
}

bool Stats::classifyIncident(RequestRecord& r, std::chrono::system_clock::time_point now) {
    if (r.model.empty()) return false;
    const std::string key = sessionModelKey(sessionKey(r), r.model);
    std::lock_guard<std::mutex> lock(mu_);
    const bool caller_code = !r.error_code.empty();
    const bool rebind = r.status < 400 && !r.switch_cause.empty();
    bool spike = false;
    const auto it = series_.find(key);
    if (it != series_.end()) {
        const SeriesState& prev = it->second;
        const NanoRub cost = effectiveCost(r);
        const NanoRub prev_cost = prev.cost_nano;
        const bool switched = !prev.provider.empty() && prev.provider != r.provider_tag;
        const bool fresh = prev.ts.time_since_epoch().count() != 0 && now >= prev.ts &&
                           now - prev.ts <= std::chrono::seconds(spike_.window_sec);
        if (r.status < 400 && !r.cost_estimated && r.tokens_in > 0 &&
            r.cache_read_tokens <= 0 && prev.cache_read_tokens > 0 && prev_cost > 0 &&
            switched && fresh && cost > 0 &&
            static_cast<long double>(cost) >=
                static_cast<long double>(prev_cost) * static_cast<long double>(spike_.ratio)) {
            spike = true;
            const double ratio = static_cast<double>(cost) / static_cast<double>(prev_cost);
            Json::Value detail(Json::objectValue);
            // Причина приходит только из ProxyService::applyRouteSwitch; пустая —
            // значит смена произошла без названной причины, и выдумывать её нельзя.
            detail["cause"] = r.switch_cause;
            detail["from"] = prev.provider;
            detail["to"] = r.provider_tag;
            detail["ratio"] = ratio;
            detail["prev_cost_nano"] = static_cast<Json::Int64>(prev_cost);
            detail["cost_nano"] = static_cast<Json::Int64>(cost);
            detail["prev_cache_read"] = static_cast<Json::Int64>(prev.cache_read_tokens);
            detail["tokens_in"] = static_cast<Json::Int64>(r.tokens_in);
            detail["gap_sec"] = static_cast<Json::Int64>(
                std::chrono::duration_cast<std::chrono::seconds>(now - prev.ts).count());
            r.event_detail = writeJson(detail);
            LOG_WARN << "cache loss on provider switch: session=" << sessionKey(r)
                     << " model=" << r.model << " from=" << prev.provider
                     << " to=" << r.provider_tag << " ratio=" << ratio
                     << " cost_nano=" << cost;
        }
    }
    // Инцидент важнее метки перепривязки; явный код вызывающего (например,
    // блокировка провайдера) не перезаписывается.
    if (!caller_code) {
        if (spike) {
            r.error_code = kProviderSwitchCacheLossCode;
        } else if (rebind) {
            r.error_code = kProviderAffinityRebindCode;
            LOG_WARN << "session rebind on provider switch: session=" << sessionKey(r)
                     << " model=" << r.model << " provider=" << r.provider_tag
                     << " cause=" << r.switch_cause;
        }
    }
    return spike;
}

void Stats::advanceSeries(const RequestRecord& r, std::chrono::system_clock::time_point now) {
    // Ряд — только успешные запросы с реальным использованием токенов.
    if (r.status >= 400 || r.tokens_in <= 0 || r.model.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    SeriesState& state = series_[sessionModelKey(sessionKey(r), r.model)];
    state.provider = r.provider_tag;
    state.cache_read_tokens = r.cache_read_tokens;
    state.cost_nano = effectiveCost(r);
    state.ts = now;
    if (series_.size() > limits_.series) {
        evictOldest(series_, [](const SeriesState& state) { return state.ts; });
    }
}

void Stats::record(RequestRecord r) {
    // Одна метка времени на всю запись: без неё память и журнал получают разные
    // времена, а серии и сутки видят эпоху 0.
    if (r.ts.time_since_epoch().count() == 0) r.ts = std::chrono::system_clock::now();
    const auto now = r.ts;
    if (classifyIncident(r, now)) {
        std::lock_guard<std::mutex> lock(mu_);
        ++cache_loss_counts_[r.model + "\x1f" + r.provider_tag];
    }
    advanceSeries(r, now);
    const NanoRub cost = effectiveCost(r);
    const NanoRub estimated = estimatedCost(r);
    revision_.fetch_add(1, std::memory_order_relaxed);
    total_requests_.fetch_add(1);
    if (r.status >= 400) total_errors_.fetch_add(1);
    if (r.stream) total_streams_.fetch_add(1);
    if (r.cost_estimated) total_estimated_requests_.fetch_add(1);
    total_tokens_in_.fetch_add(r.tokens_in);
    total_tokens_out_.fetch_add(r.tokens_out);
    total_cache_read_.fetch_add(r.cache_read_tokens);
    total_cache_write_.fetch_add(r.cache_write_tokens);
    total_cache_saved_.fetch_add(r.cache_saved_nano);
    total_cost_nano_.fetch_add(cost);
    total_cost_estimated_nano_.fetch_add(estimated);
    total_latency_ms_.fetch_add(r.latency_ms);

    if (db_ && !db_->record(r) && !db_alerted_.exchange(true)) {
        LOG_ERROR << "stats journal write failed: " << db_->lastError();
    }

    std::lock_guard<std::mutex> lock(mu_);
    ++status_counts_[r.status];
    if (!r.router_id.empty() || !r.provider_tag.empty()) {
        ++route_counts_[r.router_id + "\x1f" + r.provider_tag];
    }
    const auto key = sessionKey(r);
    auto& session = sessions_[key];
    session.session_id = key;
    ++session.requests;
    if (r.status >= 400) ++session.errors;
    session.bytes_in += r.bytes_in;
    session.bytes_out += r.bytes_out;
    session.tokens_in += r.tokens_in;
    session.tokens_out += r.tokens_out;
    session.cache_read_tokens += r.cache_read_tokens;
    session.cache_write_tokens += r.cache_write_tokens;
    session.max_cache_read_tokens =
        std::max(session.max_cache_read_tokens, r.cache_read_tokens);
    session.cost_nano += cost;
    session.cost_estimated_nano += estimated;
    if (r.cost_estimated) ++session.estimated_requests;
    session.cost_input_nano += r.cost_input_nano;
    session.cost_cache_read_nano += r.cost_cache_read_nano;
    session.cost_output_nano += r.cost_output_nano;
    session.cache_saved_nano += r.cache_saved_nano;
    session.latency_sum += r.latency_ms;
    session.last_seen = r.ts;
    session.last_model = r.model;
    session.last_provider = r.provider_tag;
    auto& mc = session_models_[sessionModelKey(key, r.model)];
    mc.tokens_in += r.tokens_in;
    mc.cache_read_tokens += r.cache_read_tokens;
    mc.last_seen = r.ts;
    if (session_models_.size() > limits_.session_models) {
        evictOldest(session_models_,
                    [](const ModelCacheAggregate& m) { return m.last_seen; });
    }
    if (r.status < 400 && !r.model.empty() && r.tokens_in > 0) {
        // Профиль модели — только запросы с реальным отчётом о токенах:
        // запрос без usage не должен разбавлять средние и задержку.
        auto& totals = models_[r.model];
        ++totals.observations;
        totals.tokens_in += r.tokens_in;
        totals.tokens_out += r.tokens_out;
        totals.cache_read_tokens += r.cache_read_tokens;
    }
    if (r.status < 400 && !r.model.empty() && r.latency_ms > 0 && !r.provider_tag.empty()) {
        auto& latency = models_[r.model].latency[r.provider_tag];
        ++latency.observations;
        latency.latency_ms_sum += r.latency_ms;
    }
    if (!r.provider_tag.empty() && r.status < 400 && r.tokens_in > 0) {
        auto& streak = cache_streaks_[providerCacheKey(key, r.model, r.provider_tag)];
        if (r.cache_read_tokens > 0) {
            streak.misses = 0;
        } else {
            ++streak.misses;
        }
        streak.last_seen = r.ts;
        if (cache_streaks_.size() > limits_.cache_streaks) {
            evictOldest(cache_streaks_,
                        [](const CacheStreak& state) { return state.last_seen; });
        }
    }
    if (sessions_.size() > limits_.sessions) {
        evictOldest(sessions_, [](const SessionAggregate& s) { return s.last_seen; });
    }
    ring_.push_back(r);
    while (static_cast<int>(ring_.size()) > ring_size_) ring_.pop_front();
}

Json::Value Stats::overview() const {
    Json::Value v(Json::objectValue);
    v["requests_total"] = static_cast<Json::Int64>(total_requests_.load());
    v["errors_total"] = static_cast<Json::Int64>(total_errors_.load());
    v["streams_total"] = static_cast<Json::Int64>(total_streams_.load());
    v["tokens_in_total"] = static_cast<Json::Int64>(total_tokens_in_.load());
    v["tokens_out_total"] = static_cast<Json::Int64>(total_tokens_out_.load());
    const auto cache_read = total_cache_read_.load();
    v["cache_read_tokens_total"] = static_cast<Json::Int64>(cache_read);
    v["cache_write_tokens_total"] = static_cast<Json::Int64>(total_cache_write_.load());
    // Доля кеша — тем же правилом, что у панели и Prometheus: сброс кеша
    // исключает прежние попадания из знаменателя.
    v["cache_hit_ratio"] = cacheHitRatio();
    v["cache_saved_nano"] = static_cast<Json::Int64>(total_cache_saved_.load());
    v["cache_saved_rub"] = formatRub(total_cache_saved_.load());
    {
        std::lock_guard<std::mutex> lock(mu_);
        v["sessions_total"] = static_cast<Json::Int64>(sessions_.size());
    }
    v["cost_nano_total"] = static_cast<Json::Int64>(total_cost_nano_.load());
    v["cost_rub_total"] = formatRub(total_cost_nano_.load());
    // Оценка запросов без отчёта провайдера: отдельная величина, не входит в итог.
    v["cost_estimated_nano"] = static_cast<Json::Int64>(total_cost_estimated_nano_.load());
    v["cost_estimated_rub"] = formatRub(total_cost_estimated_nano_.load());
    v["estimated_requests"] = static_cast<Json::Int64>(total_estimated_requests_.load());
    v["active_streams"] = static_cast<Json::Int64>(active_streams_.load());
    const auto reqs = total_requests_.load();
    v["avg_latency_ms"] = reqs > 0 ? static_cast<Json::Int64>(total_latency_ms_.load() / reqs) : 0;

    // Источник итогов — один: журнал, когда он доступен, иначе счётчики процесса.
    // Панель и API читают результат как есть, не склеивая два представления.
    if (db_ && db_->ok()) {
        const Json::Value totals = db_->totals();
        for (const auto& key : totals.getMemberNames()) {
            if (key == "enabled" || key == "path" || key == "error") continue;
            v[key] = totals[key];
        }
        v["sessions_total"] = static_cast<Json::Int64>(db_->sessionCount());
        v["stats_db"] = totals;
    }
    return v;
}

std::string Stats::prometheus() const {
    std::ostringstream os;
    os << "# HELP costrelay_requests_total Requests accepted by the proxy.\n"
       << "# TYPE costrelay_requests_total counter\n"
       << "costrelay_requests_total " << total_requests_.load() << "\n"
       << "# HELP costrelay_errors_total Requests that completed with HTTP status >= 400.\n"
       << "# TYPE costrelay_errors_total counter\n"
       << "costrelay_errors_total " << total_errors_.load() << "\n"
       << "# HELP costrelay_streams_total Streaming responses.\n"
       << "# TYPE costrelay_streams_total counter\n"
       << "costrelay_streams_total " << total_streams_.load() << "\n"
       << "# HELP costrelay_tokens_in_total Prompt tokens reported by upstream usage.\n"
       << "# TYPE costrelay_tokens_in_total counter\n"
       << "costrelay_tokens_in_total " << total_tokens_in_.load() << "\n"
       << "# HELP costrelay_tokens_out_total Completion tokens reported by upstream usage.\n"
       << "# TYPE costrelay_tokens_out_total counter\n"
       << "costrelay_tokens_out_total " << total_tokens_out_.load() << "\n"
       << "# HELP costrelay_cache_read_tokens_total Cached prompt tokens reported by upstream.\n"
       << "# TYPE costrelay_cache_read_tokens_total counter\n"
       << "costrelay_cache_read_tokens_total " << total_cache_read_.load() << "\n"
       << "# HELP costrelay_cache_write_tokens_total Cache creation tokens reported by upstream.\n"
       << "# TYPE costrelay_cache_write_tokens_total counter\n"
       << "costrelay_cache_write_tokens_total " << total_cache_write_.load() << "\n"
       << "# HELP costrelay_cache_saved_nano_total Estimated saving from cache reads in nano-RUB.\n"
       << "# TYPE costrelay_cache_saved_nano_total counter\n"
       << "costrelay_cache_saved_nano_total " << total_cache_saved_.load() << "\n"
       << "# HELP costrelay_cache_hit_ratio Observed cached prompt tokens / prompt tokens.\n"
       << "# TYPE costrelay_cache_hit_ratio gauge\n"
       << "costrelay_cache_hit_ratio " << cacheHitRatio() << "\n"
       << "# HELP costrelay_cost_nano_total Billed cost reported by upstream usage in nano-RUB.\n"
       << "# TYPE costrelay_cost_nano_total counter\n"
       << "costrelay_cost_nano_total " << total_cost_nano_.load() << "\n"
       << "# HELP costrelay_cost_estimated_nano_total Cost of requests without upstream usage,"
          " estimated from the observed model profile (not billed).\n"
       << "# TYPE costrelay_cost_estimated_nano_total counter\n"
       << "costrelay_cost_estimated_nano_total " << total_cost_estimated_nano_.load() << "\n"
       << "# HELP costrelay_cost_estimated_requests Requests without upstream token usage.\n"
       << "# TYPE costrelay_cost_estimated_requests counter\n"
       << "costrelay_cost_estimated_requests " << total_estimated_requests_.load() << "\n"
       << "# HELP costrelay_active_streams Streams currently being proxied.\n"
       << "# TYPE costrelay_active_streams gauge\n"
       << "costrelay_active_streams " << active_streams_.load() << "\n";

    std::map<int, std::int64_t> statuses;
    std::map<std::string, std::int64_t> routes;
    std::map<std::string, std::int64_t> cache_losses;
    std::size_t sessions = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        statuses = status_counts_;
        routes = route_counts_;
        cache_losses = cache_loss_counts_;
        sessions = db_ ? db_->sessionCount() : sessions_.size();
    }
    os << "# HELP costrelay_sessions_total Known request sessions.\n"
       << "# TYPE costrelay_sessions_total gauge\n"
       << "costrelay_sessions_total " << sessions << "\n";
    os << "# HELP costrelay_requests_by_status Request count by final HTTP status.\n"
       << "# TYPE costrelay_requests_by_status counter\n";
    for (const auto& [status, count] : statuses) {
        os << "costrelay_requests_by_status{status=\"" << status << "\"} " << count << "\n";
    }
    os << "# HELP costrelay_route_requests_total Requests by selected router/provider pair.\n"
       << "# TYPE costrelay_route_requests_total counter\n";
    for (const auto& [key, count] : routes) {
        const auto sep = key.find('\x1f');
        const std::string router = sep == std::string::npos ? key : key.substr(0, sep);
        const std::string provider = sep == std::string::npos ? "" : key.substr(sep + 1);
        os << "costrelay_route_requests_total{router=\"" << escapeLabel(router)
           << "\",provider=\"" << escapeLabel(provider) << "\"} " << count << "\n";
    }
    os << "# HELP costrelay_route_cache_loss_total Provider switches that dropped the cache and"
          " multiplied the cost of a session request.\n"
       << "# TYPE costrelay_route_cache_loss_total counter\n";
    for (const auto& [key, count] : cache_losses) {
        const auto sep = key.find('\x1f');
        const std::string model = sep == std::string::npos ? key : key.substr(0, sep);
        const std::string provider = sep == std::string::npos ? "" : key.substr(sep + 1);
        os << "costrelay_route_cache_loss_total{model=\"" << escapeLabel(model)
           << "\",provider=\"" << escapeLabel(provider) << "\"} " << count << "\n";
    }
    return os.str();
}

Json::Value Stats::recentJson(std::size_t limit, const std::string& session_id,
                              const std::string& model,
                              const std::string& provider, std::size_t offset,
                              IssueFilter issue, const std::string& day) const {
    Json::Value arr(Json::arrayValue);
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t matched = 0;
    std::size_t emitted = 0;
    for (std::size_t i = ring_.size(); i > 0 && emitted < limit; --i) {
        const auto& r = ring_[i - 1];
        if (!matchesRequestFilters(r, session_id, model, provider, issue, day)) continue;
        if (matched++ < offset) continue;
        ++emitted;
        Json::Value x(Json::objectValue);
        x["request_id"] = r.request_id;
        x["ts"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::seconds>(r.ts.time_since_epoch()).count());
        x["token_id"] = r.token_id;
        x["model"] = r.model;
        x["router"] = r.router_id;
        x["provider"] = r.provider_tag;
        x["route_reason"] = r.route_reason;
        x["status"] = r.status;
        x["stream"] = r.stream;
        x["latency_ms"] = r.latency_ms;
        x["ttfb_ms"] = r.ttfb_ms;
        x["bytes_in"] = static_cast<Json::Int64>(r.bytes_in);
        x["bytes_out"] = static_cast<Json::Int64>(r.bytes_out);
        x["session_id"] = r.session_id;
        x["tokens_in"] = static_cast<Json::Int64>(r.tokens_in);
        x["tokens_out"] = static_cast<Json::Int64>(r.tokens_out);
        x["cache_read_tokens"] = static_cast<Json::Int64>(r.cache_read_tokens);
        x["cache_write_tokens"] = static_cast<Json::Int64>(r.cache_write_tokens);
        x["cache_hit_ratio"] = ratioOf(r.cache_read_tokens, r.tokens_in);
        const auto cost = effectiveCost(r);
        x["cost_nano"] = static_cast<Json::Int64>(cost);
        x["cost_rub"] = formatRub(cost);
        x["cost_input_nano"] = static_cast<Json::Int64>(r.cost_input_nano);
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(r.cost_cache_read_nano);
        x["cost_output_nano"] = static_cast<Json::Int64>(r.cost_output_nano);
        x["cache_saved_nano"] = static_cast<Json::Int64>(r.cache_saved_nano);
        x["cache_saved_rub"] = formatRub(r.cache_saved_nano);
        x["cost_estimated"] = r.cost_estimated;
        x["cost_estimated_nano"] = static_cast<Json::Int64>(estimatedCost(r));
        x["cost_estimated_rub"] = formatRub(estimatedCost(r));
        x["attempts"] = r.attempts;
        x["error_code"] = r.error_code;
        x["severity"] = issueSeverity(r.status, r.error_code, r.attempts);
        x["switch_cause"] = r.switch_cause;
        x["event_detail"] = r.event_detail;
        x["captured"] = r.captured;
        x["client_ip"] = r.client_ip;
        arr.append(x);
    }
    return arr;
}

std::vector<RequestRecord> Stats::recent(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    const std::size_t start = ring_.size() > limit ? ring_.size() - limit : 0;
    return std::vector<RequestRecord>(ring_.begin() + static_cast<std::ptrdiff_t>(start), ring_.end());
}

Json::Value Stats::sessionsJson(std::size_t limit) const {
    // Источник списка сессий один: журнал, когда он доступен, иначе память
    // процесса. Панель и API не выбирают источник сами.
    if (db_ && db_->ok()) return db_->sessions(static_cast<int>(limit));
    std::vector<SessionAggregate> rows;
    {
        std::lock_guard<std::mutex> lock(mu_);
        rows.reserve(sessions_.size());
        for (const auto& [_, s] : sessions_) rows.push_back(s);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.last_seen != b.last_seen) return a.last_seen > b.last_seen;
        return a.requests > b.requests;
    });
    if (rows.size() > limit) rows.resize(limit);
    Json::Value arr(Json::arrayValue);
    for (const auto& s : rows) {
        Json::Value x(Json::objectValue);
        x["session_id"] = s.session_id;
        x["requests"] = static_cast<Json::Int64>(s.requests);
        x["errors"] = static_cast<Json::Int64>(s.errors);
        x["bytes_in"] = static_cast<Json::Int64>(s.bytes_in);
        x["bytes_out"] = static_cast<Json::Int64>(s.bytes_out);
        x["tokens_in"] = static_cast<Json::Int64>(s.tokens_in);
        x["tokens_out"] = static_cast<Json::Int64>(s.tokens_out);
        x["cache_read_tokens"] = static_cast<Json::Int64>(s.cache_read_tokens);
        x["cache_write_tokens"] = static_cast<Json::Int64>(s.cache_write_tokens);
        x["max_cache_read_tokens"] = static_cast<Json::Int64>(s.max_cache_read_tokens);
        x["cache_hit_ratio"] = ratioOf(s.cache_read_tokens, s.tokens_in);
        x["cost_nano"] = static_cast<Json::Int64>(s.cost_nano);
        x["cost_rub"] = formatRub(s.cost_nano);
        x["cost_input_nano"] = static_cast<Json::Int64>(s.cost_input_nano);
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(s.cost_cache_read_nano);
        x["cost_output_nano"] = static_cast<Json::Int64>(s.cost_output_nano);
        x["avg_cost_rub"] = formatRub(s.requests > 0 ? s.cost_nano / s.requests : 0);
        x["cost_estimated_nano"] = static_cast<Json::Int64>(s.cost_estimated_nano);
        x["cost_estimated_rub"] = formatRub(s.cost_estimated_nano);
        x["estimated_requests"] = static_cast<Json::Int64>(s.estimated_requests);
        x["cache_saved_nano"] = static_cast<Json::Int64>(s.cache_saved_nano);
        x["avg_latency_ms"] = static_cast<Json::Int64>(
            s.requests > 0 ? s.latency_sum / s.requests : 0);
        x["last_seen"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::seconds>(
                s.last_seen.time_since_epoch()).count());
        x["model"] = s.last_model;
        x["provider"] = s.last_provider;
        arr.append(x);
    }
    return arr;
}

double Stats::cacheHitRatio(const std::string& session_id) const {
    std::int64_t in = 0;
    std::int64_t cached = 0;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (session_id.empty()) {
            for (const auto& [_, s] : sessions_) {
                in += s.tokens_in - s.cache_reset_tokens;
                cached += s.cache_read_tokens;
            }
            known = !sessions_.empty();
        } else if (const auto it = sessions_.find(session_id); it != sessions_.end()) {
            in = it->second.tokens_in - it->second.cache_reset_tokens;
            cached = it->second.cache_read_tokens;
            known = true;
        }
    }
    if (known) return ratioOf(cached, in);
    if (!db_) return 0.0;
    return session_id.empty() ? db_->cacheHitRatio() : db_->sessionCacheHitRatio(session_id);
}

std::int64_t Stats::cacheMissStreak(const std::string& session_id, const std::string& model,
                                    const std::string& provider) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = cache_streaks_.find(providerCacheKey(session_id, model, provider));
    return it == cache_streaks_.end() ? 0 : it->second.misses;
}

CacheMissOutcome Stats::cacheMissOutcome(const std::string& session_id,
                                        const std::string& model,
                                        const std::string& provider) const {
    CacheMissOutcome out;
    out.misses = cacheMissStreak(session_id, model, provider) + 1;
    out.warning = out.misses >= cache_miss_.warn;
    out.blacklist = out.misses >= cache_miss_.blacklist;
    return out;
}

bool Stats::resetCache(const std::string& scope, const std::string& value) {
    if (value.empty()) return true;
    revision_.fetch_add(1, std::memory_order_relaxed);
    bool persisted = true;
    if (db_) {
        persisted = db_->setCacheReset(scope, value);
        if (!persisted) LOG_WARN << "cache reset not persisted: scope=" << scope
                                 << " value=" << value;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) {
        const auto ts = db_->cacheResetTs(scope, value);
        if (ts > 0) reset_epochs_[scope + "\x1f" + value] = ts;
    }
    const std::string prefix = value + "\x1f";
    const std::string suffix = "\x1f" + value;
    const std::string infix = "\x1f" + value + "\x1f";
    auto startsWith = [&](const std::string& key) { return key.rfind(prefix, 0) == 0; };
    auto endsWith = [&](const std::string& key) {
        return key.size() > suffix.size() &&
               key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    auto hasInfix = [&](const std::string& key) {
        return key.find(infix) != std::string::npos;
    };
    auto drop = [](auto& map, auto match) {
        for (auto it = map.begin(); it != map.end();) {
            if (match(it->first)) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    };
    if (scope == "session") {
        // Сброс меняет только попадания: вход/выход и запись в кеш остаются.
        const auto it = sessions_.find(value);
        if (it != sessions_.end()) {
            it->second.cache_read_tokens = 0;
            it->second.cache_reset_tokens = it->second.tokens_in;
        }
        drop(session_models_, startsWith);
        drop(cache_streaks_, startsWith);
    } else if (scope == "model") {
        const auto it = models_.find(value);
        if (it != models_.end()) {
            it->second.cache_read_tokens = 0;
            it->second.cache_reset_observations = it->second.observations;
            it->second.cache_reset_tokens = it->second.tokens_in;
        }
        drop(session_models_, endsWith);
        drop(cache_streaks_, hasInfix);
    } else if (scope == "provider") {
        drop(cache_streaks_, endsWith);
    }
    return persisted;
}

// Доля кеша для пары (сессия, модель): память, затем хранилище, затем общая доля
// модели — та же величина, что показывает панель.
double Stats::cacheHitRatio(const std::string& session_id, const std::string& model) const {
    if (model.empty()) return cacheHitRatio(session_id);
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = session_models_.find(sessionModelKey(session_id, model));
        if (it != session_models_.end() && it->second.tokens_in > 0) {
            return ratioOf(it->second.cache_read_tokens, it->second.tokens_in);
        }
    }
    if (db_) {
        if (const auto ratio = db_->sessionModelCacheHitRatio(session_id, model)) {
            return *ratio;
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = models_.find(model);
    if (it == models_.end()) return 0.0;
    return ratioOf(it->second.cache_read_tokens,
                   it->second.tokens_in - it->second.cache_reset_tokens);
}

std::size_t Stats::forgetRequests(const std::string& session_id, const std::string& model,
                                  const std::string& provider, IssueFilter issue,
                                  const std::string& day) {
    revision_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    const auto before = ring_.size();
    ring_.erase(std::remove_if(ring_.begin(), ring_.end(),
                               [&](const RequestRecord& r) {
                                   return matchesRequestFilters(r, session_id, model, provider,
                                                                issue, day);
                               }),
                ring_.end());
    return before - ring_.size();
}

std::int64_t Stats::requestCount(const std::string& session_id, const std::string& model,
                                 const std::string& provider, IssueFilter issue,
                                 const std::string& day) const {
    if (db_ && db_->ok()) {
        return db_->filteredRequestCount(session_id, model, provider, issue, day);
    }
    std::int64_t count = 0;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& r : ring_) {
        if (matchesRequestFilters(r, session_id, model, provider, issue, day)) ++count;
    }
    return count;
}

Json::Value Stats::requestsJson(std::size_t limit, const std::string& session_id,
                                const std::string& model, const std::string& provider,
                                std::size_t offset, IssueFilter issue,
                                const std::string& day) const {
    if (db_ && db_->ok()) {
        return db_->requests(session_id, static_cast<int>(limit), model, provider,
                             static_cast<int>(offset), issue, day);
    }
    return recentJson(limit, session_id, model, provider, offset, issue, day);
}

const char* Stats::sourceName() const { return db_ && db_->ok() ? "sqlite" : "memory"; }

ObservedProfile Stats::observedProfile(const std::string& model, int sample) const {
    if (sample > 0) {
        // Скользящее окно: последние `sample` успешных запросов модели из кольца.
        // Сброс кеша не меняет средние вход/выход, но обнуляет попадания: запросы
        // до сброса (модели, сессии или провайдера) в долю кеша не входят.
        std::lock_guard<std::mutex> lock(mu_);
        std::int64_t observations = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        std::int64_t cache_read = 0;
        std::int64_t cache_observations = 0;
        for (auto it = ring_.rbegin(); it != ring_.rend() && observations < sample; ++it) {
            const RequestRecord& r = *it;
            if (r.model != model || r.status >= 400 || r.tokens_in <= 0) continue;
            ++observations;
            tokens_in += r.tokens_in;
            tokens_out += r.tokens_out;
            const std::int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
                                        r.ts.time_since_epoch())
                                        .count();
            // Строго после сброса: та же граница, что в SQL-фильтре сброса.
            if (ts > resetEpochLocked(r)) {
                ++cache_observations;
                cache_read += r.cache_read_tokens;
            }
        }
        ObservedProfile window =
            observedFrom(observations, tokens_in, tokens_out, cache_read, cache_observations);
        if (window.observations > 0) {
            window.source = ProfileSource::Window;
            return window;
        }
    }
    if (db_ && db_->ok()) {
        // Журнал — тот же источник с тем же правилом сброса, что и окно: профиль
        // не зависит от перезапуска процесса.
        std::int64_t observations = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        std::int64_t cache_read = 0;
        std::int64_t cache_observations = 0;
        for (const auto& item : db_->usageByModelProvider("", true, model)) {
            observations += item["token_observations"].asInt64();
            tokens_in += item["tokens_in"].asInt64();
            tokens_out += item["tokens_out"].asInt64();
            cache_read += item["cache_read_tokens"].asInt64();
            cache_observations += item["cache_observations"].asInt64();
        }
        ObservedProfile journal =
            observedFrom(observations, tokens_in, tokens_out, cache_read, cache_observations);
        if (journal.observations > 0) {
            journal.source = ProfileSource::Journal;
            return journal;
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = models_.find(model);
    if (it == models_.end()) return ObservedProfile{};
    const auto& totals = it->second;
    ObservedProfile memory = observedFrom(
        totals.observations, totals.tokens_in, totals.tokens_out, totals.cache_read_tokens,
        totals.observations - totals.cache_reset_observations);
    if (memory.observations > 0) memory.source = ProfileSource::Memory;
    return memory;
}

int Stats::latencyMs(const std::string& model, const std::string& provider, int sample) const {
    if (provider.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    if (sample > 0) {
        std::int64_t observations = 0;
        std::int64_t latency_sum = 0;
        for (auto it = ring_.rbegin(); it != ring_.rend() && observations < sample; ++it) {
            const RequestRecord& r = *it;
            if (r.model != model || r.provider_tag != provider || r.status >= 400 ||
                r.latency_ms <= 0) {
                continue;
            }
            ++observations;
            latency_sum += r.latency_ms;
        }
        if (observations > 0) {
            return static_cast<int>(latency_sum / observations);
        }
    }
    const auto it = models_.find(model);
    if (it == models_.end()) return 0;
    const auto lit = it->second.latency.find(provider);
    if (lit == it->second.latency.end() || lit->second.observations <= 0) return 0;
    return static_cast<int>(lit->second.latency_ms_sum / lit->second.observations);
}

void Stats::incActiveStreams(int delta) { active_streams_.fetch_add(delta); }

} // namespace proxy