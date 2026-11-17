#pragma once

#include "config/Config.h"
#include "pricing/PriceCatalog.h"
#include "routing/Router.h"
#include "stats/Stats.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

namespace proxy {

// Очередь одной модели: все офферы в единственном порядке (RUB/M с весом кеша)
// на модельном профиле запросов. Первые `eligible` позиций — допущенные.
// `leader` — удерживаемый провайдер (гистерезис): он идёт первым в плане, даже
// если по текущим ценам challenger немного дешевле.
struct QueueModelEntry {
    std::string model;
    CostMix profile;
    std::int64_t observations = 0;
    // «Оценка вместо наблюдений» — производное от observations: второго поля нет.
    bool nominal() const { return observations == 0; }
    std::vector<RouteCandidate> quotes; // порядок очереди; допущенные — первыми
    std::size_t eligible = 0;
    bool stale_prices = false;
    std::string reason = kNoRouteCode;
    std::string error;
    std::string leader;
    std::string previous_leader;   // кто был лидером до текущего
    std::int64_t leader_since = 0; // epoch секунд
};

// Снимок очереди: одна версия данных для панели, explain и живых запросов.
struct ProviderQueueSnapshot {
    std::int64_t built_epoch = 0;
    std::string built_local;
    std::uint64_t price_revision = 0;
    std::uint64_t stats_revision = 0;
    // Политика, которой построен этот порядок: explain читает её из снимка, а не
    // из живой конфигурации, иначе порядок и его объяснение расходятся.
    int provider_order_size = 0;
    int fallback_attempts = 0;
    double output_weight = 0.0;
    double cache_weight = 0.0;
    std::vector<QueueModelEntry> models;

    const QueueModelEntry* find(const std::string& model) const {
        for (const auto& entry : models) {
            if (entry.model == model) return &entry;
        }
        return nullptr;
    }
};

// Единственный источник порядка провайдеров. Очередь перестраивается по
// расписанию (`routing.queue_min_sec`…`routing.queue_max_sec` — цены;
// `routing.queue_stats_sec` — не чаще для статистики) и публикуется атомарным
// снимком. Живой запрос, панель и explain читают только этот снимок: per-request
// пересчёта маршрута нет.
class ProviderQueue {
  public:
    ProviderQueue(const AppConfig& config, Router& router, PriceCatalog& catalog, Stats& stats);
    ~ProviderQueue();

    ProviderQueue(const ProviderQueue&) = delete;
    ProviderQueue& operator=(const ProviderQueue&) = delete;

    // Первичная сборка и фоновая нить расписания.
    void start();
    void stop();
    // Ручное перестроение (изменение политики из панели): без ожидания тика.
    void build();
    // Меняет политику и сразу перестраивает очередь под тем же замком: снимок
    // никогда не читает конфигурацию в момент её изменения.
    void rebuildWith(std::function<void()> apply_policy);
    // Сигнал от загрузчика цен. Цены, полученные после старта, не должны ждать
    // случайного тика: очередь перестраивается на каждую смену ревизии каталога.
    void notifyExternalChange();

    std::shared_ptr<const ProviderQueueSnapshot> snapshot() const;

    // План запроса: верх очереди. Никаких токенов запроса и параметров клиента.
    // `down` — провайдеры, ушедшие из маршрута по правилу routing.health_*: они
    // остаются в снимке, но в план живого запроса не попадают.
    RoutePlan planFor(const std::string& model, const std::set<std::string>& down = {}) const;
    // План минимальной проверки одного провайдера: порядок берётся у владельца
    // (planFor), но в provider_order остаётся только проверяемый тег — проба не
    // уходит на резерв очереди.
    RoutePlan planForProbe(const std::string& model, const std::string& provider) const;
    // План живого запроса: то же, но первым идёт провайдер, закреплённый за
    // (сессия, модель). Привязка не меняет снимок и не пересчитывает цены — только
    // выбирает первую позицию среди допущенных кандидатов снимка. Держится она
    // только против сброса кеша: как только лучший оффер дешевле провайдера сессии
    // на `routing.leader.margin_percent`, привязка снимается сразу.
    // Побочный эффект назван: создаёт/продлевает запись таблицы привязок и
    // вытесняет самую старую по её пределу.
    RoutePlan planForSession(const std::string& model, const std::string& session_id,
                             const std::set<std::string>& down = {});
    // Оффер модели по тегу провайдера из снимка: проверка доступности идёт тем же
    // провайдером и тем же router'ом, что живой запрос.
    std::optional<RouteCandidate> quoteFor(const std::string& model,
                                           const std::string& provider) const;
    // Провайдер отказал и запрос ушёл на другого: привязка следует за фактом.
    void rebindSession(const std::string& session_id, const std::string& model,
                       const std::string& provider);
    // Запрос не удался ни у кого: привязку снимаем, следующий запрос выберет заново.
    void clearSessionPin(const std::string& session_id, const std::string& model);
    // Закреплённый провайдер (session, model); пусто — привязки нет.
    std::string sessionPin(const std::string& session_id, const std::string& model) const;
    // Разбор очереди: `order` — тот же порядок, что уходит в план (лидер/привязка
    // первыми, неработающие исключены), а `candidates` — подробности всех офферов.
    Json::Value explain(const std::string& model, const std::set<std::string>& down = {}) const;

  private:
    struct LeaderState {
        std::string provider;
        std::string previous;
        std::int64_t since = 0;
        std::string challenger;
        int challenger_hits = 0;
    };
    struct SessionPin {
        std::string provider;
        std::chrono::system_clock::time_point bound_at{};
        std::chrono::system_clock::time_point last_used{};
    };

    void loop();
    void buildLocked();
    // Лидер модели по текущим ценам с учётом гистерезиса. Возвращает true, если
    // состояние изменилось и его нужно сохранить.
    bool applyLeader(const std::string& model, QueueModelEntry& entry, std::int64_t now);
    RoutePlan planFromEntry(const QueueModelEntry& entry, const std::string& model,
                            const std::string* pinned,
                            const std::set<std::string>& down) const;
    std::string leaderStatePath() const;
    void loadLeaders();
    void saveLeaders() const;
    std::string pinKey(const std::string& session_id, const std::string& model) const;
    // Просрочка привязки — одно правило для плана и для чтения sessionPin.
    bool pinExpired(const SessionPin& pin, std::chrono::system_clock::time_point now) const;
    void evictPinsLocked() const;
    std::uint64_t priceRevision() const;
    std::uint64_t statsRevision() const;

    const AppConfig& config_;
    Router& router_;
    PriceCatalog& catalog_;
    Stats& stats_;

    mutable std::mutex snapshot_mu_;
    std::shared_ptr<const ProviderQueueSnapshot> snapshot_;

    std::mutex build_mu_;
    std::uint64_t last_price_revision_ = 0;
    std::uint64_t last_stats_revision_ = 0;
    std::chrono::steady_clock::time_point last_build_at_{};
    std::atomic<bool> priced_build_done_{false};

    // Состояние гистерезиса и привязок: изменяется при сборке и на живых запросах.
    std::map<std::string, LeaderState> leaders_;
    mutable std::mutex pins_mu_;
    mutable std::map<std::string, SessionPin> pins_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
};

} // namespace proxy
