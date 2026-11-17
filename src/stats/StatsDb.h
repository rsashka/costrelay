#pragma once

#include "stats/Stats.h"
#include "util/NanoRub.h"

#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace proxy {

// UTC calendar day used by both persisted statistics and the daily budget.
std::string utcDayString(std::chrono::system_clock::time_point tp);
bool utcDayValid(const std::string& day);

// SQLite-backed request history. Отказ хранилища не фатален: прокси продолжает
// работать и сообщает ошибку.
class StatsDb {
  public:
    explicit StatsDb(std::string path);
    ~StatsDb();
    StatsDb(const StatsDb&) = delete;
    StatsDb& operator=(const StatsDb&) = delete;

    bool ok() const { return ok_.load(); }
    const std::string& lastError() const { return error_; }
    const std::string& path() const { return path_; }

    bool record(const RequestRecord& r);
    Json::Value totals() const;
    Json::Value daily(int days = 30) const;
    Json::Value sessions(int limit = 50) const;
    Json::Value providerTags() const;
    // day — календарные сутки UTC (YYYY-MM-DD), пусто — все дни журнала.
    Json::Value requests(const std::string& session_id = {}, int limit = 200,
                         const std::string& model = {},
                         const std::string& provider = {},
                         int offset = 0,
                         IssueFilter issue = IssueFilter::All,
                         const std::string& day = {}) const;
    std::int64_t filteredRequestCount(const std::string& session_id = {},
                                      const std::string& model = {},
                                      const std::string& provider = {},
                                      IssueFilter issue = IssueFilter::All,
                                      const std::string& day = {}) const;
    // Удаляет записи журнала под фильтром и вычитает их вклад из суточных
    // агрегатов. Возвращает число удалённых записей, -1 при отказе.
    int deleteRequests(const std::string& session_id = {},
                       const std::string& model = {},
                       const std::string& provider = {},
                       IssueFilter issue = IssueFilter::All,
                       const std::string& day = {});
    Json::Value recentProviders(int limit = 2) const;
    std::int64_t providerSwitchTs(const std::string& latest,
                                  const std::string& previous) const;
    // apply_cache_reset — учитывать сбросы кеша (для наблюдаемого профиля модели).
    // model непустой — только эта модель (использует индекс по модели).
    Json::Value usageByModelProvider(const std::string& session_id = {},
                                     bool apply_cache_reset = false,
                                     const std::string& model = {}) const;
    // Все отметки сброса кеша: (scope, value, reset_ts) для оконного профиля.
    Json::Value cacheResets() const;
    NanoRub tokenDayCost(const std::string& token_id, const std::string& day) const;
    double sessionCacheHitRatio(const std::string& session_id) const;
    double cacheHitRatio() const;
    // Доля кеша пары (сессия, модель); nullopt — записей пары нет вовсе, поэтому
    // честный ноль попаданий не путается с отсутствием данных.
    std::optional<double> sessionModelCacheHitRatio(const std::string& session_id,
                                                    const std::string& model) const;
    // Пустая строка суточного агрегата: тот же набор полей, что у daily().
    static Json::Value emptyDailyRow(const std::string& day);
    std::int64_t requestCount() const;
    // Сброс статистики кеша для области (provider | model | session): доли кеша
    // считаются только по запросам после сброса.
    bool setCacheReset(const std::string& scope, const std::string& value);
    std::int64_t cacheResetTs(const std::string& scope, const std::string& value) const;
    std::size_t sessionCount() const;
    int purge(int request_retention_days, int aggregate_retention_days);

  private:
    bool exec(const char* sql);
    // Добавляет колонку, если её ещё нет в таблице: схема живёт одной версией.
    bool ensureColumn(const char* table, const char* column, const char* decl);
    bool hasColumn(const char* table, const char* column);
    // Единственное определение наблюдаемой доли кеша: журнал с фильтром сброса.
    // Условие отбора добавляет вызывающий; mu_ удерживает он же, поэтому тот же
    // расчёт обслуживает totals(), Prometheus и панель.
    double cacheRatioLocked(const std::string& extra_where,
                            const std::vector<std::string>& binds) const;
    // Единственный путь подготовки запроса: отказ виден через fail(), а не
    // превращается в пустой результат.
    bool prepare(const std::string& sql, sqlite3_stmt** st) const;
    void fail(const std::string& what) const;
    // Завершает обход и делает сбой шага видимым (fail), а не пустым результатом.
    bool finishQuery(sqlite3_stmt* st, const char* what) const;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_request_ = nullptr;
    sqlite3_stmt* select_request_ = nullptr;
    sqlite3_stmt* daily_delta_ = nullptr;
    mutable std::mutex mu_;
    std::string path_;
    mutable std::string error_;
    mutable std::atomic<bool> ok_{false};
};

} // namespace proxy