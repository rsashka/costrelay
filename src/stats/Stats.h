#pragma once

#include "config/Config.h"
#include "util/NanoRub.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <json/json.h>

namespace proxy {

class StatsDb;

constexpr const char* kCacheMissStreakCode = "cache_miss_streak";
constexpr const char* kCacheMissBlacklistCode = "cache_miss_blacklist";

// Итог серии промахов для запроса, который ещё не записан: пороги — правило Stats.
struct CacheMissOutcome {
    std::int64_t misses = 0;
    bool warning = false;
    bool blacklist = false;
};
// Смена провайдера обнулила кеш: запрос серии стал дороже предыдущего в
// cache_loss_spike.ratio раз. Запись обязательна.
constexpr const char* kProviderSwitchCacheLossCode = "provider_switch_cache_loss";
// Сессия переключена на другого провайдера, но порог стоимости не достигнут.
constexpr const char* kProviderAffinityRebindCode = "provider_affinity_rebind";
// Коды отказа, которые видит клиент и журнал: текст для панели — в issueText.
constexpr const char* kUpstreamEmptyCompletionCode = "upstream_empty_completion";
constexpr const char* kRouteNotBuildableCode = "route_not_buildable";
constexpr const char* kUpstreamBusyCode = "upstream_busy";

// Области сброса статистики кеша: значение приходит из панели и API, ключи
// хранятся в cache_resets и в памяти процесса.
constexpr const char* kCacheScopeProvider = "provider";
constexpr const char* kCacheScopeModel = "model";
constexpr const char* kCacheScopeSession = "session";

// Важность записи журнала: 0 — норма, 1 — предупреждение, 2 — ошибка.
constexpr int kIssueWarning = 1;
constexpr int kIssueError = 2;
// Единственное место, где статус, код и повтор после отказа превращаются в
// важность: запрос, выполненный со второй попытки, несёт предупреждение.
int issueSeverity(int status, const std::string& error_code, int attempts = 0);

// Фильтр журнала по важности: все записи, только предупреждения, только ошибки
// или любые проблемы.
enum class IssueFilter { All, Warning, Error, Any };

bool issueMatches(int severity, IssueFilter filter);

// Разбор фильтра из запроса панели: неизвестное значение — все записи.
IssueFilter issueFilterFromString(const std::string& value);

struct RequestRecord {
    std::string request_id;
    std::string token_id;
    std::string session_id;
    std::string model;
    std::string router_id;
    std::string provider_tag;
    std::string route_reason;
    int status = 0;
    bool stream = false;
    int latency_ms = 0;
    int ttfb_ms = 0;
    std::int64_t bytes_in = 0;
    std::int64_t bytes_out = 0;
    std::int64_t tokens_in = 0;
    std::int64_t tokens_out = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_write_tokens = 0;
    // Оценка запроса без отчёта провайдера (профиль модели) — не факт.
    NanoRub cost_estimate_nano = 0;
    NanoRub cost_actual_nano = 0;
    NanoRub cost_input_nano = 0;
    NanoRub cost_cache_read_nano = 0;
    NanoRub cost_output_nano = 0;
    NanoRub cache_saved_nano = 0;
    bool cost_estimated = true;
    int attempts = 0;
    std::string error_code;
    // Причина смены провайдера на этом запросе:
    // local_rebind | session_rebind | session_cost_rebind.
    std::string switch_cause;
    // JSON события потери кеша: откуда/куда, отношение, токены.
    std::string event_detail;
    std::string client_ip;
    bool captured = false;
    std::chrono::system_clock::time_point ts;
};

// Стоимость и оценка — разные величины. Факт — только цена токенов, которую
// провайдер отчитал в usage. Запрос без отчёта (оборванный или усечённый поток)
// факта не даёт: его оценка по наблюдаемому профилю модели показывается отдельно
// и в итоги не входит, иначе наш итог оказался бы больше счёта провайдера.
inline NanoRub effectiveCost(const RequestRecord& r) {
    return r.cost_estimated ? 0 : r.cost_actual_nano;
}

inline NanoRub estimatedCost(const RequestRecord& r) {
    return r.cost_estimated ? r.cost_estimate_nano : 0;
}

// Откуда взята средняя: окно последних N, журнал или память процесса. Нужен
// панели, чтобы «последние N» не выдавалось за среднюю по всему журналу.
enum class ProfileSource { None, Window, Journal, Memory };

// Средний профиль запросов модели: токены на запрос. `observations == 0` —
// статистики нет, вызывающий берёт оценку.
struct ObservedProfile {
    std::int64_t observations = 0;
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cached_tokens = 0;
    ProfileSource source = ProfileSource::None;
};

// Пределы таблиц в памяти: именованы и настраиваются (тесты и профили).
struct StatsLimits {
    std::size_t series = 20000;
    std::size_t cache_streaks = 20000;
    std::size_t sessions = 2000;
    std::size_t session_models = 20000;
};

class Stats {
  public:
    explicit Stats(int ring_size = 1000, CacheLossSpikeConfig spike = {},
                   StatsLimits limits = {}, CacheMissConfig cache_miss = {});
    void record(RequestRecord r);
    Json::Value overview() const;
    std::string prometheus() const;
    void attach(StatsDb* db);
    Json::Value recentJson(std::size_t limit = 50,
                           const std::string& session_id = {},
                           const std::string& model = {},
                           const std::string& provider = {},
                           std::size_t offset = 0,
                           IssueFilter issue = IssueFilter::All,
                           const std::string& day = {}) const;
    std::vector<RequestRecord> recent(std::size_t limit = 50) const;
    // Журнал и его счётчик: источник (журнал SQLite или кольцо памяти) выбирает
    // Stats, а не вызывающий; отбор записей — одно правило для обоих.
    Json::Value requestsJson(std::size_t limit = 50, const std::string& session_id = {},
                             const std::string& model = {}, const std::string& provider = {},
                             std::size_t offset = 0, IssueFilter issue = IssueFilter::All,
                             const std::string& day = {}) const;
    std::int64_t requestCount(const std::string& session_id = {}, const std::string& model = {},
                              const std::string& provider = {},
                              IssueFilter issue = IssueFilter::All,
                              const std::string& day = {}) const;
    // Имя источника итогов и списков: "sqlite" или "memory".
    const char* sourceName() const;
    // Список сессий: источник выбирает Stats (журнал, иначе память).
    Json::Value sessionsJson(std::size_t limit = 50) const;
    double cacheHitRatio(const std::string& session_id = {}) const;
    double cacheHitRatio(const std::string& session_id, const std::string& model) const;
    // Профиль модели для весов очереди: sample = 0 — средняя по журналу
    // (переживает перезапуск), sample > 0 — последние sample успешных запросов
    // из кольца; пустое окно после перезапуска заменяется средней по журналу.
    ObservedProfile observedProfile(const std::string& model, int sample = 0) const;
    // Наблюдаемая средняя задержка провайдера на успешных запросах модели, мс:
    // 0 — наблюдений нет, задержка в вес не входит. sample = 0 — средняя по
    // журналу, sample > 0 — последние sample успешных запросов.
    int latencyMs(const std::string& model, const std::string& provider, int sample = 0) const;
    // Подряд идущие успешные запросы без попаданий в кеш для пары
    // (сессия, модель, провайдер): пороги — `routing.cache_miss`.
    std::int64_t cacheMissStreak(const std::string& session_id, const std::string& model,
                                 const std::string& provider) const;
    // Что будет с серией, если текущий запрос попадёт в журнал: один владелец
    // порогов `routing.cache_miss`.
    CacheMissOutcome cacheMissOutcome(const std::string& session_id, const std::string& model,
                                      const std::string& provider) const;
    // Самая поздняя отметка сброса кеша для запроса (модель, сессия, провайдер).
    std::int64_t resetEpochLocked(const RequestRecord& r) const;
    // Сброс доли кеша для области (provider | model | session). Возвращает false,
    // если сброс не удалось сохранить в журнале: память сброшена, но перезапуск
    // его не увидит — вызывающий обязан показать отказ.
    bool resetCache(const std::string& scope, const std::string& value);
    // Убирает записи кольцевого буфера под фильтром журнала (после удаления из БД).
    std::size_t forgetRequests(const std::string& session_id = {},
                               const std::string& model = {},
                               const std::string& provider = {},
                               IssueFilter issue = IssueFilter::All,
                               const std::string& day = {});
    void incActiveStreams(int delta);
    // Счётчик изменений статистики: очередь провайдеров перестраивается по нему,
    // но не чаще, чем раз в настроенный интервал.
    std::uint64_t revision() const { return revision_.load(std::memory_order_relaxed); }

  private:
    StatsLimits limits_;
    mutable std::mutex mu_;
    std::deque<RequestRecord> ring_;
    int ring_size_;
    std::atomic<std::int64_t> total_requests_{0};
    std::atomic<std::int64_t> total_errors_{0};
    std::atomic<std::int64_t> total_streams_{0};
    std::atomic<std::int64_t> total_tokens_in_{0};
    std::atomic<std::int64_t> total_tokens_out_{0};
    std::atomic<std::int64_t> total_cache_read_{0};
    std::atomic<std::int64_t> total_cache_write_{0};
    std::atomic<std::int64_t> total_cache_saved_{0};
    std::atomic<std::int64_t> total_cost_nano_{0};
    std::atomic<std::int64_t> total_cost_estimated_nano_{0};
    std::atomic<std::int64_t> total_estimated_requests_{0};
    std::atomic<std::int64_t> active_streams_{0};
    std::atomic<std::int64_t> total_latency_ms_{0};
    std::atomic<std::uint64_t> revision_{0};
    struct SessionAggregate {
        std::string session_id;
        std::int64_t requests = 0;
        std::int64_t errors = 0;
        std::int64_t bytes_in = 0;
        std::int64_t bytes_out = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        std::int64_t cache_read_tokens = 0;
        std::int64_t cache_write_tokens = 0;
        std::int64_t max_cache_read_tokens = 0;
        NanoRub cost_nano = 0;
        NanoRub cost_estimated_nano = 0;
        std::int64_t estimated_requests = 0;
        NanoRub cost_input_nano = 0;
        NanoRub cost_cache_read_nano = 0;
        NanoRub cost_output_nano = 0;
        NanoRub cache_saved_nano = 0;
        // Исключено из доли кеша сбросом: знаменатель считается без этих токенов.
        std::int64_t cache_reset_tokens = 0;
        std::int64_t latency_sum = 0;
        std::chrono::system_clock::time_point last_seen{};
        std::string last_model;
        std::string last_provider;
    };
    struct ModelCacheAggregate {
        std::int64_t tokens_in = 0;
        std::int64_t cache_read_tokens = 0;
        std::chrono::system_clock::time_point last_seen{};
    };
    struct ModelTotals {
        std::int64_t observations = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        std::int64_t cache_read_tokens = 0;
        // Исключено из доли кеша сбросом (наблюдения и входные токены).
        std::int64_t cache_reset_observations = 0;
        std::int64_t cache_reset_tokens = 0;
        struct Latency {
            std::int64_t observations = 0;
            std::int64_t latency_ms_sum = 0;
        };
        std::map<std::string, Latency> latency; // provider -> наблюдения и сумма
    };
    struct CacheStreak {
        std::int64_t misses = 0;
        std::chrono::system_clock::time_point last_seen{};
    };
    // Ряд серии (сессия, модель): с чем сравнивается текущий запрос.
    struct SeriesState {
        std::string provider;
        std::int64_t cache_read_tokens = 0;
        NanoRub cost_nano = 0;
        std::chrono::system_clock::time_point ts{};
    };
    // Инцидент потери кеша: заполняет event_detail и error_code, если тот свободен.
    // Предикат инцидента: только читает ряд и заполняет event_detail/error_code.
    // Возвращает true, если зафиксирован всплеск потери кеша.
    bool classifyIncident(RequestRecord& r, std::chrono::system_clock::time_point now);
    // Продвижение ряда серии — отдельная операция записи, не часть предиката.
    void advanceSeries(const RequestRecord& r, std::chrono::system_clock::time_point now);
    static std::string sessionKey(const RequestRecord& r);
    static std::string sessionModelKey(const std::string& session, const std::string& model);
    static std::string providerCacheKey(const std::string& session, const std::string& model,
                                        const std::string& provider);
    std::map<int, std::int64_t> status_counts_;
    std::map<std::string, std::int64_t> route_counts_;
    std::map<std::string, SessionAggregate> sessions_;
    std::map<std::string, ModelCacheAggregate> session_models_;
    std::map<std::string, ModelTotals> models_;
    std::map<std::string, CacheStreak> cache_streaks_;
    // Отметки сброса кеша (scope\x1fvalue -> ts): окно профиля считает попадания
    // только по запросам после сброса, как SQL-путь по журналу.
    std::map<std::string, std::int64_t> reset_epochs_;
    std::map<std::string, SeriesState> series_;
    std::map<std::string, std::int64_t> cache_loss_counts_;
    CacheLossSpikeConfig spike_;
    CacheMissConfig cache_miss_;
    StatsDb* db_ = nullptr;
    // Отказ журнала сообщается один раз: повтор на каждой записи не читается.
    std::atomic<bool> db_alerted_{false};
};

} // namespace proxy
