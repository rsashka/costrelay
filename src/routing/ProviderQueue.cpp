#include "routing/ProviderQueue.h"
#include "util/Time.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <utility>

namespace proxy {

namespace {

std::string writeJson(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

// Выигрыш challenger'а над удерживаемым провайдером: при margin <= 0 достаточно
// быть не дороже, иначе — дешевле на margin процентов. Одна мера и для смены
// лидера, и для снятия привязки сессии.
bool beatsByMargin(NanoRub challenger, NanoRub incumbent, double margin_percent) {
    if (margin_percent <= 0.0) return challenger <= incumbent;
    const long double keep = 1.0L - static_cast<long double>(margin_percent) / 100.0L;
    return static_cast<long double>(challenger) <= static_cast<long double>(incumbent) * keep;
}

// Выносит выбранную позицию наверх, оставляя остальные в порядке веса. Циклический
// сдвиг здесь недопустим: офферы, стоявшие впереди лидера или привязки, уехали бы
// в хвост очереди — и в fallback, и в панели дешёвые провайдеры оказались бы
// последними.
void moveIndexToFront(std::vector<std::size_t>& order, std::size_t index) {
    const std::size_t value = order[index];
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(index));
    order.insert(order.begin(), value);
}

} // namespace

ProviderQueue::ProviderQueue(const AppConfig& config, Router& router, PriceCatalog& catalog,
                             Stats& stats)
    : config_(config), router_(router), catalog_(catalog), stats_(stats) {}

ProviderQueue::~ProviderQueue() { stop(); }

std::uint64_t ProviderQueue::priceRevision() const { return catalog_.revision(); }

std::uint64_t ProviderQueue::statsRevision() const { return stats_.revision(); }

std::shared_ptr<const ProviderQueueSnapshot> ProviderQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mu_);
    return snapshot_;
}

void ProviderQueue::build() {
    std::lock_guard<std::mutex> build_lock(build_mu_);
    buildLocked();
}

void ProviderQueue::rebuildWith(std::function<void()> apply_policy) {
    std::lock_guard<std::mutex> build_lock(build_mu_);
    apply_policy();
    buildLocked();
}

void ProviderQueue::buildLocked() {
    auto next = std::make_shared<ProviderQueueSnapshot>();
    const auto now = std::chrono::system_clock::now();
    next->built_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                            now.time_since_epoch())
                            .count();
    next->built_local = localMoscowDateTime().first;
    next->price_revision = priceRevision();
    next->stats_revision = statsRevision();
    next->provider_order_size = config_.routing.provider_order_size;
    next->fallback_attempts = config_.routing.fallback_attempts;
    next->output_weight = config_.routing.output_weight;
    next->cache_weight = config_.routing.cache_weight;
    bool leaders_changed = false;

    // Один и тот же профиль для всех: наблюдаемая средняя модели, иначе номинал.
    for (const auto& model : config_.routing.models) {
        QueueModelEntry entry;
        entry.model = model;
        const ObservedProfile observed =
            stats_.observedProfile(model, config_.routing.profile_sample_size);
        if (observed.observations > 0) {
            entry.profile = CostMix{observed.input_tokens, observed.output_tokens,
                                    observed.cached_tokens};
            entry.observations = observed.observations;
        } else {
            entry.profile = CostMix{kNominalInputTokens, kNominalOutputTokens, 0};
        }

        const PriceState state = router_.priceState(model);
        entry.stale_prices = state.stale;
        if (!state.usable && !state.has_direct) {
            entry.reason = state.has_snapshot ? kStalePricesReason : kNoPriceReason;
            entry.error = state.has_snapshot ? kPricesUnavailableCode : kNoPriceSnapshotCode;
            next->models.push_back(std::move(entry));
            continue;
        }

        const int sample = config_.routing.profile_sample_size;
        entry.quotes = router_.quotes(
            model, entry.profile,
            [this, &model, sample](const std::string& provider) {
                return stats_.latencyMs(model, provider, sample);
            });
        for (const auto& quote : entry.quotes) {
            if (quote.excluded.empty()) ++entry.eligible;
        }
        if (entry.eligible == 0) {
            entry.reason = kNoRouteCode;
            entry.error = kNoRouteCode;
        } else {
            entry.reason = entry.stale_prices ? kCostStaleReason : kCostReason;
        }
        if (applyLeader(model, entry, next->built_epoch)) leaders_changed = true;
        next->models.push_back(std::move(entry));
    }

    if (leaders_changed) saveLeaders();

    {
        std::lock_guard<std::mutex> lock(snapshot_mu_);
        snapshot_ = next;
    }
    last_price_revision_ = next->price_revision;
    last_stats_revision_ = next->stats_revision;
    last_build_at_ = std::chrono::steady_clock::now();
    priced_build_done_ = catalog_.revision() != 0;
}

void ProviderQueue::notifyExternalChange() {
    // Цены после старта не ждут случайного тика: перестраиваемся на каждую смену
    // ревизии каталога, чтобы новый дешёвый оффер был виден живому запросу сразу.
    std::lock_guard<std::mutex> lock(build_mu_);
    if (!priced_build_done_.load() || priceRevision() != last_price_revision_) buildLocked();
}

void ProviderQueue::start() {
    if (running_.exchange(true)) return;
    loadLeaders();
    build();
    thread_ = std::thread([this] { loop(); });
}

void ProviderQueue::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ProviderQueue::loop() {
    std::mt19937_64 rng(std::random_device{}() ^
                        static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(this)));
    const int min_sec = std::max(1, config_.routing.queue_min_sec);
    const int max_sec = std::max(min_sec, config_.routing.queue_max_sec);
    std::uniform_int_distribution<int> delay_dist(min_sec, max_sec);
    const auto stats_interval =
        std::chrono::seconds(std::max(1, config_.routing.queue_stats_sec));

    while (running_.load()) {
        const int delay = delay_dist(rng);
        {
            std::unique_lock<std::mutex> lock(cv_mu_);
            cv_.wait_for(lock, std::chrono::seconds(delay),
                         [this] { return !running_.load(); });
        }
        if (!running_.load()) break;
        // Решение о перестроении и само перестроение — под тем же замком, что
        // защищает ревизии и снимок: иначе тик читает полуобновлённое состояние.
        std::lock_guard<std::mutex> lock(build_mu_);
        const bool price_changed = priceRevision() != last_price_revision_;
        const bool stats_changed = statsRevision() != last_stats_revision_;
        const bool stats_due = stats_changed &&
                               std::chrono::steady_clock::now() - last_build_at_ >= stats_interval;
        if (!snapshot_ || price_changed || stats_due) buildLocked();
    }
}

RoutePlan ProviderQueue::planFor(const std::string& model,
                                 const std::set<std::string>& down) const {
    RoutePlan plan;
    plan.model = model;
    const auto snap = snapshot();
    if (!snap) {
        plan.error = kNoPriceSnapshotCode;
        plan.reason = kNoPriceReason;
        return plan;
    }
    const QueueModelEntry* entry = snap->find(model);
    if (!entry) {
        plan.error = kModelNotFoundCode;
        plan.reason = kModelNotConfiguredReason;
        return plan;
    }
    return planFromEntry(*entry, model, nullptr, down);
}

// Проба идёт ровно к проверяемому провайдеру: остальные позиции очереди в
// provider_order не нужны.
RoutePlan ProviderQueue::planForProbe(const std::string& model,
                                      const std::string& provider) const {
    RoutePlan plan = planFor(model);
    plan.provider_order.clear();
    if (!provider.empty()) plan.provider_order.push_back(provider);
    return plan;
}

std::optional<RouteCandidate> ProviderQueue::quoteFor(const std::string& model,
                                                      const std::string& provider) const {
    const auto snap = snapshot();
    if (!snap) return std::nullopt;
    const QueueModelEntry* entry = snap->find(model);
    if (!entry) return std::nullopt;
    for (const auto& quote : entry->quotes) {
        if (quote.provider_tag == provider) return quote;
    }
    return std::nullopt;
}

RoutePlan ProviderQueue::planForSession(const std::string& model, const std::string& session_id,
                                        const std::set<std::string>& down) {
    RoutePlan plan;
    plan.model = model;
    const auto snap = snapshot();
    if (!snap) {
        plan.error = kNoPriceSnapshotCode;
        plan.reason = kNoPriceReason;
        return plan;
    }
    const QueueModelEntry* entry = snap->find(model);
    if (!entry) {
        plan.error = kModelNotFoundCode;
        plan.reason = kModelNotConfiguredReason;
        return plan;
    }
    if (session_id.empty() || config_.routing.session_pin_sec <= 0) {
        return planFromEntry(*entry, model, nullptr, down);
    }

    const std::string key = pinKey(session_id, model);
    std::string pinned;
    const auto now = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> lock(pins_mu_);
        const auto it = pins_.find(key);
        if (it != pins_.end()) {
            if (pinExpired(it->second, now)) {
                pins_.erase(it);
            } else {
                pinned = it->second.provider;
            }
        }
    }

    plan = planFromEntry(*entry, model, pinned.empty() ? nullptr : &pinned, down);
    if (!plan.hasRoute()) return plan;

    {
        std::lock_guard<std::mutex> lock(pins_mu_);
        auto& pin = pins_[key];
        if (pin.bound_at.time_since_epoch().count() == 0) pin.bound_at = now;
        pin.provider = plan.candidates.front().provider_tag;
        pin.last_used = now;
        evictPinsLocked();
    }
    return plan;
}

void ProviderQueue::rebindSession(const std::string& session_id, const std::string& model,
                                  const std::string& provider) {
    if (session_id.empty() || model.empty() || provider.empty()) return;
    if (config_.routing.session_pin_sec <= 0) return;
    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(pins_mu_);
    auto& pin = pins_[pinKey(session_id, model)];
    if (pin.bound_at.time_since_epoch().count() == 0) pin.bound_at = now;
    pin.provider = provider;
    pin.last_used = now;
    evictPinsLocked();
}

void ProviderQueue::clearSessionPin(const std::string& session_id, const std::string& model) {
    if (session_id.empty() || model.empty()) return;
    std::lock_guard<std::mutex> lock(pins_mu_);
    pins_.erase(pinKey(session_id, model));
}

std::string ProviderQueue::sessionPin(const std::string& session_id,
                                      const std::string& model) const {
    if (session_id.empty() || model.empty()) return {};
    std::lock_guard<std::mutex> lock(pins_mu_);
    const auto it = pins_.find(pinKey(session_id, model));
    if (it == pins_.end()) return {};
    if (pinExpired(it->second, std::chrono::system_clock::now())) return {};
    return it->second.provider;
}

bool ProviderQueue::pinExpired(const SessionPin& pin,
                               std::chrono::system_clock::time_point now) const {
    if (config_.routing.session_pin_sec <= 0) return false;
    return now - pin.last_used > std::chrono::seconds(config_.routing.session_pin_sec);
}

std::string ProviderQueue::pinKey(const std::string& session_id, const std::string& model) const {
    return session_id + "\x1f" + model;
}

void ProviderQueue::evictPinsLocked() const {
    const std::size_t limit = static_cast<std::size_t>(
        std::max(routing_bounds::kSessionPinMaxMin, config_.routing.session_pin_max));
    while (pins_.size() > limit) {
        auto oldest = pins_.begin();
        for (auto it = pins_.begin(); it != pins_.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used) oldest = it;
        }
        pins_.erase(oldest);
    }
}

RoutePlan ProviderQueue::planFromEntry(const QueueModelEntry& entry, const std::string& model,
                                       const std::string* pinned,
                                       const std::set<std::string>& down) const {
    RoutePlan plan;
    plan.model = model;
    plan.estimated_in = entry.profile.input_tokens;
    plan.estimated_out = entry.profile.output_tokens;
    plan.cache_hit_ratio = entry.profile.cacheHitRatio();
    plan.stale_prices = entry.stale_prices;
    plan.reason = entry.reason;
    plan.error = entry.error;

    // Порядок очереди по весу: `order` — допущенные, первый из них дешевейший.
    // Отклонённые — только в rejected. Неработающий провайдер остаётся оффером
    // снимка, но в план не попадает: запрос сразу идёт следующему по очереди.
    std::vector<std::size_t> order;
    order.reserve(entry.quotes.size());
    for (std::size_t i = 0; i < entry.quotes.size(); ++i) {
        const auto& quote = entry.quotes[i];
        if (!quote.excluded.empty()) {
            plan.rejected.push_back(quote);
            continue;
        }
        if (down.count(quote.provider_tag) > 0) {
            RouteCandidate blocked = quote;
            blocked.excluded = kProviderDown;
            plan.rejected.push_back(std::move(blocked));
            continue;
        }
        order.push_back(i);
    }

    // Привязка сессии существует только против сброса кеша: пока лучший оффер не
    // дешевле провайдера сессии на `routing.leader.margin_percent`, он идёт первым.
    // Порог превышен — привязка снимается сразу, ожидания подтверждений лидера нет.
    bool pin_held = false;
    bool pin_dropped_by_cost = false;
    if (pinned && !pinned->empty()) {
        const auto pin_it = std::find_if(order.begin(), order.end(), [&](std::size_t index) {
            return entry.quotes[index].provider_tag == *pinned;
        });
        if (pin_it == order.end()) {
            plan.session_rebind_from = *pinned;
            plan.session_rebind_reason =
                down.count(*pinned) > 0 ? kProviderDown : kPinDropNotEligible;
        } else if (pin_it != order.begin() &&
                   beatsByMargin(entry.quotes[order.front()].weight_per_million_nano,
                                 entry.quotes[*pin_it].weight_per_million_nano,
                                 config_.routing.leader.margin_percent)) {
            plan.session_rebind_from = *pinned;
            plan.session_rebind_reason = kPinDropCostMargin;
            pin_dropped_by_cost = true;
        } else {
            pin_held = true;
            moveIndexToFront(order, static_cast<std::size_t>(pin_it - order.begin()));
        }
    }

    // Первый оффер плана: привязка, иначе лучший по цене при снятой по стоимости
    // привязке, иначе удерживаемый лидер модели.
    if (!pin_held && !pin_dropped_by_cost) {
        const auto leader_it = std::find_if(order.begin(), order.end(), [&](std::size_t index) {
            return entry.quotes[index].provider_tag == entry.leader;
        });
        if (leader_it != order.end()) {
            moveIndexToFront(order, static_cast<std::size_t>(leader_it - order.begin()));
        }
    }

    std::set<std::string> seen_tags;
    for (const std::size_t index : order) {
        const auto& tag = entry.quotes[index].provider_tag;
        if (seen_tags.insert(tag).second) plan.provider_priority.push_back(tag);
    }

    // Список кандидатов ограничен `routing.candidate_cap`; сколько из них будет
    // реально опрошено, решает бюджет попыток `routing.fallback_attempts` у
    // потребителя плана.
    const std::size_t candidate_cap = static_cast<std::size_t>(
        std::max(routing_bounds::kCandidateCapMin, config_.routing.candidate_cap));
    const std::size_t order_cap = static_cast<std::size_t>(
        std::max(routing_bounds::kProviderOrderMin, config_.routing.provider_order_size));
    for (const std::size_t index : order) {
        if (plan.candidates.size() >= candidate_cap) break;
        plan.candidates.push_back(entry.quotes[index]);
    }
    // Все допущенные офферы снимка не работают: причина отказа — не цены и не
    // фильтры, а доступность провайдеров.
    if (order.empty() && entry.eligible > 0) plan.error = kProviderDown;
    // provider.order — очередь тегов начиная с лидера или привязки; в запрос уходит
    // не больше provider_order_size. Один элемент — смены у внешнего обработчика нет.
    std::set<std::string> order_tags;
    for (const std::size_t index : order) {
        if (plan.provider_order.size() >= order_cap) break;
        const auto& quote = entry.quotes[index];
        const auto* router = config_.findRouter(quote.router_id);
        if (router && router->kind != "openai_compatible" &&
            order_tags.insert(quote.provider_tag).second) {
            plan.provider_order.push_back(quote.provider_tag);
        }
    }
    return plan;
}

bool ProviderQueue::applyLeader(const std::string& model, QueueModelEntry& entry,
                                std::int64_t now) {
    const RouteCandidate* best = nullptr;
    const RouteCandidate* current = nullptr;
    LeaderState& state = leaders_[model];
    for (const auto& quote : entry.quotes) {
        if (!quote.excluded.empty()) continue;
        if (!best) best = &quote;
        if (!current && !state.provider.empty() && quote.provider_tag == state.provider) {
            current = &quote;
        }
    }
    if (!best) return false;

    const auto& cfg = config_.routing.leader;
    // Сохранению подлежит любое изменение состояния, а не только смена лидера:
    // иначе прогресс подтверждения challenger'а теряется при перезапуске и
    // `routing.leader.confirm_builds` молча требует больше перестроений.
    bool changed = false;
    if (state.provider.empty() || !current) {
        // Лидера нет или он перестал быть допущенным: удержание невозможно.
        if (!state.provider.empty()) {
            LOG_WARN << "route leader forced: model=" << model << " provider=" << state.provider
                     << " new=" << best->provider_tag << " reason=not_eligible";
        }
        const std::string before = state.provider;
        state = LeaderState{};
        state.previous = before;
        state.provider = best->provider_tag;
        state.since = now;
        changed = true;
    } else if (best->provider_tag != state.provider) {
        const bool margin_ok = beatsByMargin(best->weight_per_million_nano,
                                             current->weight_per_million_nano,
                                             cfg.margin_percent);
        if (margin_ok) {
            if (state.challenger == best->provider_tag) {
                ++state.challenger_hits;
            } else {
                state.challenger = best->provider_tag;
                state.challenger_hits = 1;
            }
            changed = true;
        } else if (!state.challenger.empty() || state.challenger_hits != 0) {
            state.challenger.clear();
            state.challenger_hits = 0;
            changed = true;
        }
        if (!state.challenger.empty() && state.challenger_hits >= cfg.confirm_builds &&
            now - state.since >= cfg.min_dwell_sec) {
            LOG_INFO << "route leader switched: model=" << model << " provider=" << state.provider
                     << " new=" << state.challenger
                     << " weight_old=" << current->weight_per_million_nano
                     << " weight_new=" << best->weight_per_million_nano;
            state.previous = state.provider;
            state.provider = best->provider_tag;
            state.since = now;
            state.challenger.clear();
            state.challenger_hits = 0;
            changed = true;
        }
    } else if (!state.challenger.empty() || state.challenger_hits != 0) {
        state.challenger.clear();
        state.challenger_hits = 0;
        changed = true;
    }
    entry.leader = state.provider;
    entry.previous_leader = state.previous;
    entry.leader_since = state.since;
    return changed;
}

std::string ProviderQueue::leaderStatePath() const {
    if (!config_.routing.leader.state_file.empty()) return config_.routing.leader.state_file;
    return config_.data_dir + "/routing-leader.json";
}

void ProviderQueue::loadLeaders() {
    const std::string path = leaderStatePath();
    std::ifstream in(path);
    if (!in) return;
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string error;
    if (!Json::parseFromStream(builder, in, &root, &error)) {
        LOG_WARN << "route leader state ignored: " << path << " " << error;
        return;
    }
    const auto& models = root["models"];
    if (!models.isObject()) return;
    for (const auto& name : models.getMemberNames()) {
        const auto& value = models[name];
        LeaderState state;
        state.provider = value["provider"].asString();
        state.previous = value["previous"].asString();
        state.since = value["since"].asInt64();
        state.challenger = value["challenger"].asString();
        state.challenger_hits = value["challenger_hits"].asInt();
        if (!state.provider.empty()) leaders_[name] = state;
    }
    LOG_INFO << "route leader state loaded: models=" << leaders_.size();
}

void ProviderQueue::saveLeaders() const {
    Json::Value models(Json::objectValue);
    for (const auto& model : config_.routing.models) {
        const auto it = leaders_.find(model);
        if (it == leaders_.end() || it->second.provider.empty()) continue;
        Json::Value value(Json::objectValue);
        value["provider"] = it->second.provider;
        value["previous"] = it->second.previous;
        value["since"] = static_cast<Json::Int64>(it->second.since);
        value["challenger"] = it->second.challenger;
        value["challenger_hits"] = it->second.challenger_hits;
        models[model] = value;
    }
    Json::Value root(Json::objectValue);
    root["models"] = models;

    const std::string path = leaderStatePath();
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            LOG_WARN << "route leader state not writable: " << path;
            return;
        }
        out << writeJson(root);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) LOG_WARN << "route leader state save failed: " << path << " " << ec.message();
}

Json::Value ProviderQueue::explain(const std::string& model,
                                   const std::set<std::string>& down) const {
    const auto snap = snapshot();
    Json::Value v(Json::objectValue);
    v["model"] = model;
    v["built_at"] = snap ? static_cast<Json::Int64>(snap->built_epoch) : 0;
    v["provider_order_size"] = snap ? snap->provider_order_size : 0;
    v["fallback_attempts"] = snap ? snap->fallback_attempts : 0;
    v["output_weight"] = snap ? snap->output_weight : 0.0;
    v["cache_weight"] = snap ? snap->cache_weight : 0.0;
    if (!snap) {
        v["reason"] = kNoPriceReason;
        v["error"] = kNoPriceSnapshotCode;
        v["candidates"] = Json::Value(Json::arrayValue);
        v["rejected"] = Json::Value(Json::arrayValue);
        return v;
    }
    const QueueModelEntry* entry = snap->find(model);
    if (!entry) {
        v["reason"] = kModelNotConfiguredReason;
        v["error"] = kModelNotFoundCode;
        v["candidates"] = Json::Value(Json::arrayValue);
        v["rejected"] = Json::Value(Json::arrayValue);
        return v;
    }
    v["reason"] = entry->reason;
    v["error"] = entry->error;
    v["stale_prices"] = entry->stale_prices;
    v["leader"] = entry->leader;
    v["previous_leader"] = entry->previous_leader;
    v["leader_since"] = static_cast<Json::Int64>(entry->leader_since);
    v["estimated_in"] = static_cast<Json::Int64>(entry->profile.input_tokens);
    v["estimated_out"] = static_cast<Json::Int64>(entry->profile.output_tokens);
    v["cache_hit_ratio"] = entry->profile.cacheHitRatio();
    v["observations"] = static_cast<Json::Int64>(entry->observations);

    const RoutePlan plan = planFromEntry(*entry, model, nullptr, down);
    Json::Value order(Json::arrayValue);
    Json::Value arr(Json::arrayValue);
    Json::Value rejected(Json::arrayValue);
    for (const auto& c : entry->quotes) {
        Json::Value x(Json::objectValue);
        x["router"] = c.router_id;
        x["provider"] = c.provider_tag;
        x["cost_nano"] = static_cast<Json::Int64>(c.cost_nano);
        x["cost_rub"] = formatRub(c.cost_nano);
        x["token_weight_nano"] = static_cast<Json::Int64>(c.cost_per_million_nano);
        x["token_weight_rub"] = formatRub(c.cost_per_million_nano);
        x["latency_ms"] = c.latency_ms;
        x["latency_weight_nano"] = static_cast<Json::Int64>(c.latency_cost_per_million_nano);
        x["weight_nano"] = static_cast<Json::Int64>(c.weight_per_million_nano);
        x["weight_rub"] = formatRub(c.weight_per_million_nano);
        x["cost_no_cache_nano"] = static_cast<Json::Int64>(c.cost_no_cache_nano);
        x["cache_saved_nano"] = static_cast<Json::Int64>(c.cache_saved_nano);
        x["cache_hit_ratio"] = c.cache_hit_ratio;
        x["cache_read_price_nano"] = static_cast<Json::Int64>(c.cache_read_price_nano);
        x["cache_capacity_tokens"] = static_cast<Json::Int64>(c.cache_capacity_tokens);
        x["excluded"] = c.excluded;
        x["excluded_detail"] = c.excluded_detail;
        if (c.excluded.empty()) {
            arr.append(x);
        } else {
            rejected.append(x);
        }
    }
    // Порядок — из плана: один владелец (лидер, привязка, неработающие).
    for (const auto& tag : plan.provider_priority) order.append(tag);
    v["order"] = order;
    v["candidates"] = arr;
    v["rejected"] = rejected;
    return v;
}

} // namespace proxy
