#include "stats/StatsDb.h"
#include "util/Ratio.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace proxy {
namespace {

std::int64_t epochSeconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

// Фильтр журнала: пустая строка означает «любое значение». Один и тот же
// предикат используют чтение, подсчёт и удаление записей.
constexpr const char* kRequestFilter =
    " (?1='' OR session_id=?1) AND (?2='' OR model=?2) AND (?3='' OR provider_tag=?3)"
    " AND (?4='' OR (ts>=CAST(strftime('%s',?4) AS INTEGER)"
    " AND ts<CAST(strftime('%s',?4,'+1 day') AS INTEGER)))";

// Сброс статистики кеша исключает более старые запросы из долей кеша: сброс
// задаётся для сессии, модели или провайдера и действует до следующего сброса.
// Условие общее для фильтра журнала и для условных сумм попаданий.
constexpr const char* kAfterCacheReset =
    "ts > COALESCE((SELECT MAX(c.reset_ts) FROM cache_resets c WHERE "
    "(c.scope='session' AND c.value=requests.session_id) OR "
    "(c.scope='model' AND c.value=requests.model) OR "
    "(c.scope='provider' AND c.value=requests.provider_tag)),0)";

const std::string kCacheResetFilter = std::string(" AND ") + kAfterCacheReset;

// SQL-форма того же календарного дня UTC, что utcDayString: колонка дня, по
// которой суточные агрегаты вычитаются при удалении журнала.
constexpr const char* kUtcDayColumn = "strftime('%Y-%m-%d', ts, 'unixepoch')";

// SQL-форма правила важности: 1 — предупреждение, >=2 — ошибка (issueSeverity).
// Границы названы здесь один раз, из них собираются и фильтр, и CASE-суммы.
constexpr const char* kSqlWarningCase = "CASE WHEN severity=1 THEN 1 ELSE 0 END";
constexpr const char* kSqlErrorCase = "CASE WHEN severity>=2 THEN 1 ELSE 0 END";
constexpr const char* kSqlEstimatedCase = "CASE WHEN cost_estimated<>0 THEN 1 ELSE 0 END";

// Важность записи — только по значению IssueFilter, не пользовательский ввод.
std::string issuePredicate(IssueFilter filter) {
    switch (filter) {
        case IssueFilter::Warning:
            return " AND severity=1";
        case IssueFilter::Error:
            return " AND severity>=2";
        case IssueFilter::Any:
            return " AND severity>=1";
        case IssueFilter::All:
            break;
    }
    return "";
}

} // namespace

std::string utcDayString(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d");
    return os.str();
}

bool utcDayValid(const std::string& day) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
    for (const std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (day[i] < '0' || day[i] > '9') return false;
    }
    const int year = std::stoi(day.substr(0, 4));
    const int month = std::stoi(day.substr(5, 2));
    const int day_of_month = std::stoi(day.substr(8, 2));
    if (month < 1 || month > 12 || day_of_month < 1) return false;
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return day_of_month <= (month == 2 && leap ? 29 : days[month]);
}

StatsDb::StatsDb(std::string path) : path_(std::move(path)) {
    if (path_.empty()) {
        fail("stats sqlite path is empty");
        return;
    }
    std::error_code ec;
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) {
        fail("cannot create stats directory: " + ec.message());
        return;
    }
    if (sqlite3_open_v2(path_.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        fail(std::string("cannot open stats db: ") + (db_ ? sqlite3_errmsg(db_) : "unknown"));
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return;
    }
    sqlite3_busy_timeout(db_, 5000);
    std::error_code perm_ec;
    std::filesystem::permissions(path_,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, perm_ec);
    ok_ = true;
    if (!exec("PRAGMA journal_mode=WAL") || !exec("PRAGMA synchronous=NORMAL") ||
        !exec("CREATE TABLE IF NOT EXISTS requests("
              "request_id TEXT PRIMARY KEY,"
              "ts INTEGER NOT NULL,"
              "token_id TEXT NOT NULL DEFAULT '',"
              "session_id TEXT NOT NULL DEFAULT '',"
              "model TEXT NOT NULL DEFAULT '',"
              "router_id TEXT NOT NULL DEFAULT '',"
              "provider_tag TEXT NOT NULL DEFAULT '',"
              "route_reason TEXT NOT NULL DEFAULT '',"
              "status INTEGER NOT NULL DEFAULT 0,"
              "stream INTEGER NOT NULL DEFAULT 0,"
              "latency_ms INTEGER NOT NULL DEFAULT 0,"
              "ttfb_ms INTEGER NOT NULL DEFAULT 0,"
              "bytes_in INTEGER NOT NULL DEFAULT 0,"
              "bytes_out INTEGER NOT NULL DEFAULT 0,"
              "tokens_in INTEGER NOT NULL DEFAULT 0,"
              "tokens_out INTEGER NOT NULL DEFAULT 0,"
              "cache_read_tokens INTEGER NOT NULL DEFAULT 0,"
              "cache_write_tokens INTEGER NOT NULL DEFAULT 0,"
              "cost_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_estimated_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_input_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_cache_read_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_output_nano INTEGER NOT NULL DEFAULT 0,"
              "cache_saved_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_estimated INTEGER NOT NULL DEFAULT 1,"
              "attempts INTEGER NOT NULL DEFAULT 0,"
              "error_code TEXT NOT NULL DEFAULT '',"
              "switch_cause TEXT NOT NULL DEFAULT '',"
              "client_ip TEXT NOT NULL DEFAULT '',"
              "captured INTEGER NOT NULL DEFAULT 0,"
              "severity INTEGER NOT NULL DEFAULT 0,"
              "event_detail TEXT NOT NULL DEFAULT '')") ||
        !ensureColumn("requests", "event_detail", "TEXT NOT NULL DEFAULT ''") ||
        !ensureColumn("requests", "cost_estimated_nano", "INTEGER NOT NULL DEFAULT 0") ||
        !ensureColumn("requests", "switch_cause", "TEXT NOT NULL DEFAULT ''") ||
        !exec("CREATE INDEX IF NOT EXISTS idx_requests_ts ON requests(ts)") ||
        !exec("CREATE TABLE IF NOT EXISTS daily("
              "day TEXT NOT NULL,"
              "token_id TEXT NOT NULL,"
              "requests INTEGER NOT NULL DEFAULT 0,"
              "estimated_requests INTEGER NOT NULL DEFAULT 0,"
              "errors INTEGER NOT NULL DEFAULT 0,"
              "warnings INTEGER NOT NULL DEFAULT 0,"
              "tokens_in INTEGER NOT NULL DEFAULT 0,"
              "tokens_out INTEGER NOT NULL DEFAULT 0,"
              "cache_read_tokens INTEGER NOT NULL DEFAULT 0,"
              "cache_write_tokens INTEGER NOT NULL DEFAULT 0,"
              "cost_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_estimated_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_input_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_cache_read_nano INTEGER NOT NULL DEFAULT 0,"
              "cost_output_nano INTEGER NOT NULL DEFAULT 0,"
              "cache_saved_nano INTEGER NOT NULL DEFAULT 0,"
              "PRIMARY KEY(day, token_id))") ||
        !ensureColumn("daily", "estimated_requests", "INTEGER NOT NULL DEFAULT 0") ||
        !ensureColumn("daily", "cost_estimated_nano", "INTEGER NOT NULL DEFAULT 0") ||
        !ensureColumn("daily", "warnings", "INTEGER NOT NULL DEFAULT 0") ||
        !exec("CREATE TABLE IF NOT EXISTS cache_resets("
              "scope TEXT NOT NULL,"
              "value TEXT NOT NULL,"
              "reset_ts INTEGER NOT NULL,"
              "PRIMARY KEY(scope, value))") ||
        !exec("CREATE INDEX IF NOT EXISTS idx_requests_session_ts ON requests(session_id, ts)") ||
        !exec("CREATE INDEX IF NOT EXISTS idx_requests_session_model_ts ON requests(session_id, model, ts)") ||
        !exec("CREATE INDEX IF NOT EXISTS idx_requests_provider_ts ON requests(provider_tag, ts)") ||
        !exec("CREATE INDEX IF NOT EXISTS idx_requests_model_ts ON requests(model, ts)")) {
        ok_ = false;
        return;
    }

    const char* insert_request =
        "INSERT INTO requests(request_id,ts,token_id,session_id,model,router_id,provider_tag,"
        "route_reason,status,stream,latency_ms,ttfb_ms,bytes_in,bytes_out,tokens_in,tokens_out,"
        "cache_read_tokens,cache_write_tokens,cost_nano,cost_estimated_nano,"
        "cost_input_nano,cost_cache_read_nano,cost_output_nano,cache_saved_nano,cost_estimated,"
        "attempts,error_code,switch_cause,client_ip,captured,severity,event_detail) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(request_id) DO UPDATE SET ts=excluded.ts,token_id=excluded.token_id,"
        "session_id=excluded.session_id,model=excluded.model,router_id=excluded.router_id,"
        "provider_tag=excluded.provider_tag,route_reason=excluded.route_reason,"
        "status=excluded.status,stream=excluded.stream,latency_ms=excluded.latency_ms,"
        "ttfb_ms=excluded.ttfb_ms,bytes_in=excluded.bytes_in,bytes_out=excluded.bytes_out,"
        "tokens_in=excluded.tokens_in,tokens_out=excluded.tokens_out,"
        "cache_read_tokens=excluded.cache_read_tokens,"
        "cache_write_tokens=excluded.cache_write_tokens,"
        "cost_nano=excluded.cost_nano,"
        "cost_estimated_nano=excluded.cost_estimated_nano,"
        "cost_input_nano=excluded.cost_input_nano,"
        "cost_cache_read_nano=excluded.cost_cache_read_nano,"
        "cost_output_nano=excluded.cost_output_nano,cache_saved_nano=excluded.cache_saved_nano,"
        "cost_estimated=excluded.cost_estimated,attempts=excluded.attempts,"
        "error_code=excluded.error_code,switch_cause=excluded.switch_cause,"
        "client_ip=excluded.client_ip,captured=excluded.captured,"
        "severity=excluded.severity,event_detail=excluded.event_detail";
    // Знаковая дельта: повторная запись того же request_id сначала вычитает старый
    // вклад из суточных агрегатов, поэтому upsert не удваивает сутки.
    const char* daily_delta =
        "INSERT INTO daily(day,token_id,requests,estimated_requests,errors,warnings,"
        "tokens_in,tokens_out,cache_read_tokens,cache_write_tokens,cost_nano,"
        "cost_estimated_nano,cost_input_nano,cost_cache_read_nano,cost_output_nano,"
        "cache_saved_nano) "
        "VALUES(?1,?2,MAX(0,?3),MAX(0,?4),MAX(0,?5),MAX(0,?6),MAX(0,?7),MAX(0,?8),"
        "MAX(0,?9),MAX(0,?10),MAX(0,?11),MAX(0,?12),MAX(0,?13),MAX(0,?14),MAX(0,?15),"
        "MAX(0,?16)) "
        "ON CONFLICT(day,token_id) DO UPDATE SET "
        "requests=MAX(0,requests+?3),estimated_requests=MAX(0,estimated_requests+?4),"
        "errors=MAX(0,errors+?5),warnings=MAX(0,warnings+?6),"
        "tokens_in=MAX(0,tokens_in+?7),tokens_out=MAX(0,tokens_out+?8),"
        "cache_read_tokens=MAX(0,cache_read_tokens+?9),"
        "cache_write_tokens=MAX(0,cache_write_tokens+?10),"
        "cost_nano=MAX(0,cost_nano+?11),"
        "cost_estimated_nano=MAX(0,cost_estimated_nano+?12),"
        "cost_input_nano=MAX(0,cost_input_nano+?13),"
        "cost_cache_read_nano=MAX(0,cost_cache_read_nano+?14),"
        "cost_output_nano=MAX(0,cost_output_nano+?15),"
        "cache_saved_nano=MAX(0,cache_saved_nano+?16)";
    const char* select_request =
        "SELECT ts,token_id,status,tokens_in,tokens_out,cache_read_tokens,cache_write_tokens,"
        "cost_nano,cost_estimated_nano,cost_estimated,cost_input_nano,"
        "cost_cache_read_nano,cost_output_nano,cache_saved_nano,severity "
        "FROM requests WHERE request_id=?";
    if (sqlite3_prepare_v2(db_, insert_request, -1, &insert_request_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, select_request, -1, &select_request_, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, daily_delta, -1, &daily_delta_, nullptr) != SQLITE_OK) {
        ok_ = false;
        fail(std::string("cannot prepare stats statements: ") + sqlite3_errmsg(db_));
    }
}

StatsDb::~StatsDb() {
    if (insert_request_) sqlite3_finalize(insert_request_);
    if (select_request_) sqlite3_finalize(select_request_);
    if (daily_delta_) sqlite3_finalize(daily_delta_);
    if (db_) sqlite3_close(db_);
}

void StatsDb::fail(const std::string& what) const {
    error_ = what;
    ok_ = false;
}

// Завершение обхода: сбой sqlite3_step виден через fail(), а не превращается в
// пустой результат или ноль.
bool StatsDb::finishQuery(sqlite3_stmt* st, const char* what) const {
    if (!st) return true;
    const int rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) {
        fail(std::string(what) + ": " + sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool StatsDb::prepare(const std::string& sql, sqlite3_stmt** st) const {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, st, nullptr) != SQLITE_OK) {
        fail(std::string("stats query prepare failed: ") +
             (db_ ? sqlite3_errmsg(db_) : "database closed"));
        if (*st) {
            sqlite3_finalize(*st);
            *st = nullptr;
        }
        return false;
    }
    return true;
}

bool StatsDb::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "sql error";
        if (err) sqlite3_free(err);
        fail(message);
        return false;
    }
    return true;
}

bool StatsDb::hasColumn(const char* table, const char* column) {
    if (!db_) return false;
    const std::string pragma = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, pragma.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        fail(std::string("stats schema read failed: ") + sqlite3_errmsg(db_));
        return false;
    }
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        if (name && column == std::string(name)) {
            found = true;
            break;
        }
    }
    finishQuery(st, __func__);
    return found;
}

bool StatsDb::ensureColumn(const char* table, const char* column, const char* decl) {
    if (!db_) return false;
    if (hasColumn(table, column)) return true;
    const std::string alter =
        std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + decl;
    return exec(alter.c_str());
}

Json::Value StatsDb::cacheResets() const {
    Json::Value arr(Json::arrayValue);
    if (!ok_ || !db_) return arr;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT scope, value, reset_ts FROM cache_resets";
    if (!prepare(sql, &st)) return arr;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value x(Json::objectValue);
        const auto* scope = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        x["scope"] = scope ? scope : "";
        x["value"] = value ? value : "";
        x["reset_ts"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 2));
        arr.append(x);
    }
    finishQuery(st, __func__);
    return arr;
}

bool StatsDb::record(const RequestRecord& r) {
    if (!ok_ || !db_) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (!ok_) return false;
    const auto ts = epochSeconds(r.ts.time_since_epoch().count() == 0
                                     ? std::chrono::system_clock::now()
                                     : r.ts);
    const auto day = utcDayString(std::chrono::system_clock::time_point(
        std::chrono::seconds(ts)));
    int rc = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        fail(std::string("stats transaction begin failed: ") + sqlite3_errmsg(db_));
        return false;
    }
    // Вклад строки в суточные агрегаты: повторная запись того же request_id
    // сначала вычитает прежний вклад, иначе сутки удвоятся.
    struct DailyDelta {
        std::int64_t requests = 0;
        std::int64_t estimated_requests = 0;
        std::int64_t errors = 0;
        std::int64_t warnings = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        std::int64_t cache_read = 0;
        std::int64_t cache_write = 0;
        std::int64_t cost = 0;
        std::int64_t cost_estimated = 0;
        std::int64_t cost_input = 0;
        std::int64_t cost_cache_read = 0;
        std::int64_t cost_output = 0;
        std::int64_t saved = 0;
    };
    bool replaced = false;
    std::string old_day;
    std::string old_token;
    DailyDelta old;
    {
        sqlite3_stmt* st = select_request_;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, r.request_id.c_str(), -1, SQLITE_TRANSIENT);
        const int step = sqlite3_step(st);
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            // Ошибка чтения неотличима от «прежней строки нет»: без явной проверки
            // старый вклад не вычитается и суточные агрегаты удваиваются.
            sqlite3_reset(st);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            fail(std::string("stats select request failed: ") + sqlite3_errmsg(db_));
            return false;
        }
        if (step == SQLITE_ROW) {
            replaced = true;
            const auto old_ts = sqlite3_column_int64(st, 0);
            old_day = utcDayString(std::chrono::system_clock::time_point(
                std::chrono::seconds(old_ts)));
            const auto* token = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
            old_token = token ? token : "";
            old.requests = 1;
            const int old_severity = sqlite3_column_int(st, 14);
            old.errors = old_severity >= kIssueError ? 1 : 0;
            old.warnings = old_severity == kIssueWarning ? 1 : 0;
            old.tokens_in = sqlite3_column_int64(st, 3);
            old.tokens_out = sqlite3_column_int64(st, 4);
            old.cache_read = sqlite3_column_int64(st, 5);
            old.cache_write = sqlite3_column_int64(st, 6);
            old.cost = sqlite3_column_int64(st, 7);
            old.cost_estimated = sqlite3_column_int64(st, 8);
            old.estimated_requests = sqlite3_column_int(st, 9) != 0 ? 1 : 0;
            old.cost_input = sqlite3_column_int64(st, 10);
            old.cost_cache_read = sqlite3_column_int64(st, 11);
            old.cost_output = sqlite3_column_int64(st, 12);
            old.saved = sqlite3_column_int64(st, 13);
        }
        sqlite3_reset(st);
    }
    const int severity = issueSeverity(r.status, r.error_code, r.attempts);
    sqlite3_stmt* req = insert_request_;
    sqlite3_reset(req);
    sqlite3_clear_bindings(req);
    int i = 1;
    sqlite3_bind_text(req, i++, r.request_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(req, i++, ts);
    sqlite3_bind_text(req, i++, r.token_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.router_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.provider_tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.route_reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(req, i++, r.status);
    sqlite3_bind_int(req, i++, r.stream ? 1 : 0);
    sqlite3_bind_int(req, i++, r.latency_ms);
    sqlite3_bind_int(req, i++, r.ttfb_ms);
    sqlite3_bind_int64(req, i++, r.bytes_in);
    sqlite3_bind_int64(req, i++, r.bytes_out);
    sqlite3_bind_int64(req, i++, r.tokens_in);
    sqlite3_bind_int64(req, i++, r.tokens_out);
    sqlite3_bind_int64(req, i++, r.cache_read_tokens);
    sqlite3_bind_int64(req, i++, r.cache_write_tokens);
    sqlite3_bind_int64(req, i++, effectiveCost(r));
    sqlite3_bind_int64(req, i++, estimatedCost(r));
    sqlite3_bind_int64(req, i++, r.cost_input_nano);
    sqlite3_bind_int64(req, i++, r.cost_cache_read_nano);
    sqlite3_bind_int64(req, i++, r.cost_output_nano);
    sqlite3_bind_int64(req, i++, r.cache_saved_nano);
    sqlite3_bind_int(req, i++, r.cost_estimated ? 1 : 0);
    sqlite3_bind_int(req, i++, r.attempts);
    sqlite3_bind_text(req, i++, r.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.switch_cause.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(req, i++, r.client_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(req, i++, r.captured ? 1 : 0);
    sqlite3_bind_int(req, i++, severity);
    sqlite3_bind_text(req, i++, r.event_detail.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(req);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        fail(std::string("stats insert failed: ") + sqlite3_errmsg(db_));
        return false;
    }

    const DailyDelta fresh{1,
                           r.cost_estimated ? 1 : 0,
                           severity >= kIssueError ? 1 : 0,
                           severity == kIssueWarning ? 1 : 0,
                           r.tokens_in,
                           r.tokens_out,
                           r.cache_read_tokens,
                           r.cache_write_tokens,
                           effectiveCost(r),
                           estimatedCost(r),
                           r.cost_input_nano,
                           r.cost_cache_read_nano,
                           r.cost_output_nano,
                           r.cache_saved_nano};
    const auto apply_delta = [&](const std::string& delta_day, const std::string& delta_token,
                                 const DailyDelta& d, int sign) {
        sqlite3_stmt* st = daily_delta_;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, delta_day.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, delta_token.c_str(), -1, SQLITE_TRANSIENT);
        const auto bind = [&](int index, std::int64_t value) {
            sqlite3_bind_int64(st, index, value * sign);
        };
        bind(3, d.requests);
        bind(4, d.estimated_requests);
        bind(5, d.errors);
        bind(6, d.warnings);
        bind(7, d.tokens_in);
        bind(8, d.tokens_out);
        bind(9, d.cache_read);
        bind(10, d.cache_write);
        bind(11, d.cost);
        bind(12, d.cost_estimated);
        bind(13, d.cost_input);
        bind(14, d.cost_cache_read);
        bind(15, d.cost_output);
        bind(16, d.saved);
        const int step = sqlite3_step(st);
        sqlite3_reset(st);
        return step == SQLITE_DONE;
    };
    if (replaced && !apply_delta(old_day, old_token, old, -1)) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        fail(std::string("stats daily rollback failed: ") + sqlite3_errmsg(db_));
        return false;
    }
    if (!apply_delta(day, r.token_id, fresh, 1)) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        fail(std::string("stats daily insert failed: ") + sqlite3_errmsg(db_));
        return false;
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail(std::string("stats transaction commit failed: ") + sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

Json::Value StatsDb::totals() const {
    Json::Value v(Json::objectValue);
    v["enabled"] = ok_.load();
    v["path"] = path_;
    v["error"] = error_;
    if (!ok_ || !db_) return v;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    // Итоги — по суточным агрегатам: журнал ограничен requests_retention_days,
    // агрегаты живут дольше и уже содержат тот же расход. Одна строка на день и
    // токен вместо сканирования журнала.
    const char* sql =
        "SELECT COALESCE(SUM(requests),0), COALESCE(SUM(errors),0),"
        "COALESCE(SUM(warnings),0),"
        "COALESCE(SUM(tokens_in),0), COALESCE(SUM(tokens_out),0),"
        "COALESCE(SUM(cache_read_tokens),0), COALESCE(SUM(cache_write_tokens),0),"
        "COALESCE(SUM(cost_nano),0), COALESCE(SUM(cache_saved_nano),0),"
        "COALESCE(SUM(cost_estimated_nano),0),"
        "COALESCE(SUM(estimated_requests),0) "
        "FROM daily";
    if (!prepare(sql, &st)) return v;
    if (sqlite3_step(st) == SQLITE_ROW) {
        v["requests_total"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 0));
        v["errors_total"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 1));
        v["warnings_total"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 2));
        const auto tokens_in = sqlite3_column_int64(st, 3);
        const auto cache_read = sqlite3_column_int64(st, 5);
        v["tokens_in_total"] = static_cast<Json::Int64>(tokens_in);
        v["tokens_out_total"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 4));
        v["cache_read_tokens_total"] = static_cast<Json::Int64>(cache_read);
        v["cache_write_tokens_total"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 6));
        // Доля кеша — та же, что у Prometheus: сброс кеша исключает прежние
        // попадания, поэтому она читается по журналу, а не по суточным суммам.
        v["cache_hit_ratio"] = cacheRatioLocked({}, {});
        const auto cost = sqlite3_column_int64(st, 7);
        v["cost_nano_total"] = static_cast<Json::Int64>(cost);
        v["cost_rub_total"] = formatRub(cost);
        v["cache_saved_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 8));
        v["cache_saved_rub"] = formatRub(sqlite3_column_int64(st, 8));
        const auto estimated = sqlite3_column_int64(st, 9);
        v["cost_estimated_nano"] = static_cast<Json::Int64>(estimated);
        v["cost_estimated_rub"] = formatRub(estimated);
        v["estimated_requests"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 10));
    }
    finishQuery(st, __func__);
    return v;
}

Json::Value StatsDb::daily(int days) const {
    Json::Value arr(Json::arrayValue);
    if (!ok_ || !db_ || days <= 0) return arr;
    const auto cutoff = utcDayString(std::chrono::system_clock::now() -
                                     std::chrono::hours(24 * std::max(0, days - 1)));
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT day, SUM(requests), SUM(errors), SUM(warnings), SUM(tokens_in),"
        "SUM(tokens_out), SUM(cache_read_tokens), SUM(cache_write_tokens), SUM(cost_nano),"
        "SUM(cost_input_nano), SUM(cost_cache_read_nano), SUM(cost_output_nano),"
        "SUM(cache_saved_nano), SUM(cost_estimated_nano), SUM(estimated_requests) "
        "FROM daily WHERE day>=? GROUP BY day ORDER BY day ASC";
    if (!prepare(sql, &st)) return arr;
    sqlite3_bind_text(st, 1, cutoff.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value x(Json::objectValue);
        x["day"] = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        x["requests"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 1));
        x["errors"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 2));
        x["warnings"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 3));
        const auto tokens_in = sqlite3_column_int64(st, 4);
        const auto cache_read = sqlite3_column_int64(st, 6);
        x["tokens_in"] = static_cast<Json::Int64>(tokens_in);
        x["tokens_out"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 5));
        x["cache_read_tokens"] = static_cast<Json::Int64>(cache_read);
        x["cache_write_tokens"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 7));
        x["cache_hit_ratio"] = ratioOf(cache_read, tokens_in);
        const auto cost = sqlite3_column_int64(st, 8);
        x["cost_nano"] = static_cast<Json::Int64>(cost);
        x["cost_rub"] = formatRub(cost);
        const auto cost_input = sqlite3_column_int64(st, 9);
        const auto cost_cache_read = sqlite3_column_int64(st, 10);
        const auto cost_output = sqlite3_column_int64(st, 11);
        x["cost_input_nano"] = static_cast<Json::Int64>(cost_input);
        x["cost_input_rub"] = formatRub(cost_input);
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(cost_cache_read);
        x["cost_cache_read_rub"] = formatRub(cost_cache_read);
        x["cost_output_nano"] = static_cast<Json::Int64>(cost_output);
        x["cost_output_rub"] = formatRub(cost_output);
        x["cache_saved_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 12));
        const auto estimated = sqlite3_column_int64(st, 13);
        x["cost_estimated_nano"] = static_cast<Json::Int64>(estimated);
        x["cost_estimated_rub"] = formatRub(estimated);
        x["estimated_requests"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 14));
        arr.append(x);
    }
    finishQuery(st, __func__);
    return arr;
}


Json::Value StatsDb::providerTags() const {
    Json::Value arr(Json::arrayValue);
    if (!ok_ || !db_) return arr;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT DISTINCT provider_tag FROM requests WHERE provider_tag<>'' "
        "ORDER BY provider_tag";
    if (!prepare(sql, &st)) return arr;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const auto* provider = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        if (provider && *provider) arr.append(provider);
    }
    finishQuery(st, __func__);
    return arr;
}

Json::Value StatsDb::sessions(int limit) const {
    Json::Value arr(Json::arrayValue);
    if (!ok_ || !db_ || limit <= 0) return arr;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT r.session_id, COUNT(*), COALESCE(SUM(CASE WHEN r.status>=400 THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(r.tokens_in),0), COALESCE(SUM(r.tokens_out),0),"
        "COALESCE(SUM(r.cache_read_tokens),0), COALESCE(SUM(r.cache_write_tokens),0),"
        "COALESCE(MAX(r.cache_read_tokens),0),"
        "COALESCE(SUM(r.cost_nano),0),"
        "COALESCE(SUM(r.cost_input_nano),0), COALESCE(SUM(r.cost_cache_read_nano),0),"
        "COALESCE(SUM(r.cost_output_nano),0), COALESCE(SUM(r.cache_saved_nano),0),"
        "COALESCE(MAX(r.ts),0), COALESCE(AVG(r.latency_ms),0),"
        "(SELECT model FROM requests x WHERE x.session_id=r.session_id ORDER BY x.ts DESC LIMIT 1),"
        "(SELECT provider_tag FROM requests x WHERE x.session_id=r.session_id ORDER BY x.ts DESC LIMIT 1),"
        "COALESCE(SUM(r.bytes_in),0), COALESCE(SUM(r.bytes_out),0),"
        "COALESCE(SUM(r.cost_estimated_nano),0),"
        "COALESCE(SUM(CASE WHEN r.cost_estimated<>0 THEN 1 ELSE 0 END),0) "
        "FROM requests r GROUP BY r.session_id ORDER BY MAX(r.ts) DESC LIMIT ?";
    if (!prepare(sql, &st)) return arr;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value x(Json::objectValue);
        const auto* session = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        const auto requests = sqlite3_column_int64(st, 1);
        const auto tokens_in = sqlite3_column_int64(st, 3);
        const auto cache_read = sqlite3_column_int64(st, 5);
        const auto cost = sqlite3_column_int64(st, 8);
        x["session_id"] = session ? session : "";
        x["requests"] = static_cast<Json::Int64>(requests);
        x["errors"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 2));
        x["tokens_in"] = static_cast<Json::Int64>(tokens_in);
        x["tokens_out"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 4));
        x["cache_read_tokens"] = static_cast<Json::Int64>(cache_read);
        x["cache_write_tokens"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 6));
        x["max_cache_read_tokens"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 7));
        x["cache_hit_ratio"] = ratioOf(cache_read, tokens_in);
        x["cost_nano"] = static_cast<Json::Int64>(cost);
        x["cost_rub"] = formatRub(cost);
        x["avg_cost_rub"] = formatRub(requests > 0 ? cost / requests : 0);
        x["cost_input_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 9));
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 10));
        x["cost_output_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 11));
        x["cache_saved_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 12));
        x["cache_saved_rub"] = formatRub(sqlite3_column_int64(st, 12));
        x["last_seen"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 13));
        x["avg_latency_ms"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 14));
        const auto* model = reinterpret_cast<const char*>(sqlite3_column_text(st, 15));
        const auto* provider = reinterpret_cast<const char*>(sqlite3_column_text(st, 16));
        x["model"] = model ? model : "";
        x["provider"] = provider ? provider : "";
        x["bytes_in"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 17));
        x["bytes_out"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 18));
        x["cost_estimated_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 19));
        x["cost_estimated_rub"] = formatRub(sqlite3_column_int64(st, 19));
        x["estimated_requests"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 20));
        arr.append(x);
    }
    finishQuery(st, __func__);
    return arr;
}

std::int64_t StatsDb::filteredRequestCount(const std::string& session_id,
                                           const std::string& model,
                                           const std::string& provider,
                                           IssueFilter issue,
                                           const std::string& day) const {
    if (!ok_ || !db_) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM requests WHERE") +
                            kRequestFilter + issuePredicate(issue);
    if (!prepare(sql.c_str(), &st)) return 0;
    sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, day.c_str(), -1, SQLITE_TRANSIENT);
    std::int64_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int64(st, 0);
    finishQuery(st, __func__);
    return count;
}

Json::Value StatsDb::requests(const std::string& session_id, int limit,
                              const std::string& model,
                              const std::string& provider, int offset,
                              IssueFilter issue, const std::string& day) const {
    Json::Value arr(Json::arrayValue);
    if (!ok_ || !db_ || limit <= 0) return arr;
    limit = std::clamp(limit, 1, 500);
    offset = std::max(0, offset);
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const std::string sql =
        std::string("SELECT request_id, ts, token_id, session_id, model, router_id, provider_tag,"
        " route_reason, status, stream, latency_ms, ttfb_ms, bytes_in, bytes_out,"
        " tokens_in, tokens_out, cache_read_tokens, cache_write_tokens,"
        " cost_nano, cost_input_nano, cost_cache_read_nano, cost_output_nano,"
        " cache_saved_nano, cost_estimated, attempts, error_code, switch_cause, captured, client_ip,"
        " severity,"
        " event_detail, cost_estimated_nano "
        "FROM requests WHERE") +
        kRequestFilter + issuePredicate(issue) + " ORDER BY ts DESC LIMIT ?5 OFFSET ?6";
    if (!prepare(sql.c_str(), &st)) return arr;
    sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, day.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, limit);
    sqlite3_bind_int(st, 6, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value x(Json::objectValue);
        const auto text = [&](int col) {
            const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
            return std::string(value ? value : "");
        };
        x["request_id"] = text(0);
        x["ts"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 1));
        x["token_id"] = text(2);
        x["session_id"] = text(3);
        x["model"] = text(4);
        x["router"] = text(5);
        x["provider"] = text(6);
        x["route_reason"] = text(7);
        x["status"] = sqlite3_column_int(st, 8);
        x["stream"] = sqlite3_column_int(st, 9) != 0;
        x["latency_ms"] = sqlite3_column_int(st, 10);
        x["ttfb_ms"] = sqlite3_column_int(st, 11);
        x["bytes_in"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 12));
        x["bytes_out"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 13));
        const auto tokens_in = sqlite3_column_int64(st, 14);
        const auto cache_read = sqlite3_column_int64(st, 16);
        x["tokens_in"] = static_cast<Json::Int64>(tokens_in);
        x["tokens_out"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 15));
        x["cache_read_tokens"] = static_cast<Json::Int64>(cache_read);
        x["cache_write_tokens"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 17));
        x["cache_hit_ratio"] = ratioOf(cache_read, tokens_in);
        const auto cost = sqlite3_column_int64(st, 18);
        x["cost_nano"] = static_cast<Json::Int64>(cost);
        x["cost_rub"] = formatRub(cost);
        x["cost_input_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 19));
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 20));
        x["cost_output_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 21));
        x["cache_saved_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 22));
        x["cache_saved_rub"] = formatRub(sqlite3_column_int64(st, 22));
        x["cost_estimated"] = sqlite3_column_int(st, 23) != 0;
        x["attempts"] = sqlite3_column_int(st, 24);
        x["error_code"] = text(25);
        x["switch_cause"] = text(26);
        x["captured"] = sqlite3_column_int(st, 27) != 0;
        x["client_ip"] = text(28);
        x["severity"] = sqlite3_column_int(st, 29);
        x["event_detail"] = text(30);
        x["cost_estimated_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 31));
        x["cost_estimated_rub"] = formatRub(sqlite3_column_int64(st, 31));
        arr.append(x);
    }
    finishQuery(st, __func__);
    return arr;
}

int StatsDb::deleteRequests(const std::string& session_id, const std::string& model,
                            const std::string& provider, IssueFilter issue,
                            const std::string& day) {
    if (!ok_ || !db_) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail(std::string("delete transaction begin failed: ") + sqlite3_errmsg(db_));
        return -1;
    }
    // Сначала фиксируем вклад удаляемых строк в суточные агрегаты, затем удаляем:
    // иначе «сегодня» и бюджеты останутся с расходом, которого уже нет в журнале.
    struct DayDelta {
        std::string day;
        std::string token_id;
        std::int64_t requests = 0;
        std::int64_t errors = 0;
        std::int64_t warnings = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        std::int64_t cache_read = 0;
        std::int64_t cache_write = 0;
        std::int64_t cost = 0;
        std::int64_t cost_input = 0;
        std::int64_t cost_cache_read = 0;
        std::int64_t cost_output = 0;
        std::int64_t saved = 0;
        std::int64_t cost_estimated = 0;
        std::int64_t estimated_requests = 0;
    };
    std::vector<DayDelta> deltas;
    {
        sqlite3_stmt* st = nullptr;
        std::string sql = std::string("SELECT ") + kUtcDayColumn +
                          ", token_id, COUNT(*)," +
                          " COALESCE(SUM(" + kSqlErrorCase + "),0)," +
                          " COALESCE(SUM(" + kSqlWarningCase + "),0)," +
                          " COALESCE(SUM(tokens_in),0), COALESCE(SUM(tokens_out),0)," +
                          " COALESCE(SUM(cache_read_tokens),0),"
                          " COALESCE(SUM(cache_write_tokens),0)," +
                          " COALESCE(SUM(cost_nano),0), COALESCE(SUM(cost_input_nano),0)," +
                          " COALESCE(SUM(cost_cache_read_nano),0),"
                          " COALESCE(SUM(cost_output_nano),0)," +
                          " COALESCE(SUM(cache_saved_nano),0)," +
                          " COALESCE(SUM(cost_estimated_nano),0)," +
                          " COALESCE(SUM(" + kSqlEstimatedCase + "),0)" +
                          " FROM requests WHERE";
        sql += kRequestFilter + issuePredicate(issue) + " GROUP BY 1, 2";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            fail(std::string("cannot collect deleted aggregates: ") + sqlite3_errmsg(db_));
            return -1;
        }
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, provider.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, day.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const auto text = [&](int col) {
                const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
                return std::string(value ? value : "");
            };
            DayDelta d;
            d.day = text(0);
            d.token_id = text(1);
            d.requests = sqlite3_column_int64(st, 2);
            d.errors = sqlite3_column_int64(st, 3);
            d.warnings = sqlite3_column_int64(st, 4);
            d.tokens_in = sqlite3_column_int64(st, 5);
            d.tokens_out = sqlite3_column_int64(st, 6);
            d.cache_read = sqlite3_column_int64(st, 7);
            d.cache_write = sqlite3_column_int64(st, 8);
            d.cost = sqlite3_column_int64(st, 9);
            d.cost_input = sqlite3_column_int64(st, 10);
            d.cost_cache_read = sqlite3_column_int64(st, 11);
            d.cost_output = sqlite3_column_int64(st, 12);
            d.saved = sqlite3_column_int64(st, 13);
            d.cost_estimated = sqlite3_column_int64(st, 14);
            d.estimated_requests = sqlite3_column_int64(st, 15);
            deltas.push_back(std::move(d));
        }
        finishQuery(st, __func__);
    }

    bool ok = true;
    if (!deltas.empty()) {
        sqlite3_stmt* st = nullptr;
        const char* update =
            " UPDATE daily SET requests=MAX(0,requests-?1),"
            " estimated_requests=MAX(0,estimated_requests-?2),"
            " errors=MAX(0,errors-?3),"
            " warnings=MAX(0,warnings-?4),"
            " tokens_in=MAX(0,tokens_in-?5), tokens_out=MAX(0,tokens_out-?6),"
            " cache_read_tokens=MAX(0,cache_read_tokens-?7),"
            " cache_write_tokens=MAX(0,cache_write_tokens-?8),"
            " cost_nano=MAX(0,cost_nano-?9),"
            " cost_estimated_nano=MAX(0,cost_estimated_nano-?10),"
            " cost_input_nano=MAX(0,cost_input_nano-?11),"
            " cost_cache_read_nano=MAX(0,cost_cache_read_nano-?12),"
            " cost_output_nano=MAX(0,cost_output_nano-?13),"
            " cache_saved_nano=MAX(0,cache_saved_nano-?14)"
            " WHERE day=?15 AND token_id=?16";
        if (!prepare(update, &st)) {
            ok = false;
        } else {
            for (const auto& d : deltas) {
                sqlite3_reset(st);
                sqlite3_clear_bindings(st);
                sqlite3_bind_int64(st, 1, d.requests);
                sqlite3_bind_int64(st, 2, d.estimated_requests);
                sqlite3_bind_int64(st, 3, d.errors);
                sqlite3_bind_int64(st, 4, d.warnings);
                sqlite3_bind_int64(st, 5, d.tokens_in);
                sqlite3_bind_int64(st, 6, d.tokens_out);
                sqlite3_bind_int64(st, 7, d.cache_read);
                sqlite3_bind_int64(st, 8, d.cache_write);
                sqlite3_bind_int64(st, 9, d.cost);
                sqlite3_bind_int64(st, 10, d.cost_estimated);
                sqlite3_bind_int64(st, 11, d.cost_input);
                sqlite3_bind_int64(st, 12, d.cost_cache_read);
                sqlite3_bind_int64(st, 13, d.cost_output);
                sqlite3_bind_int64(st, 14, d.saved);
                sqlite3_bind_text(st, 15, d.day.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 16, d.token_id.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) != SQLITE_DONE) { ok = false; break; }
            }
            finishQuery(st, __func__);
        }
    }

    int removed = 0;
    if (ok) {
        sqlite3_stmt* st = nullptr;
        const std::string sql = std::string("DELETE FROM requests WHERE") + kRequestFilter +
                                issuePredicate(issue);
        if (prepare(sql, &st)) {
            sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, provider.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, day.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_DONE) {
                removed = sqlite3_changes(db_);
            } else {
                ok = false;
            }
            finishQuery(st, __func__);
        } else {
            ok = false;
        }
    }
    if (!ok) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        fail(std::string("request log delete failed: ") + sqlite3_errmsg(db_));
        return -1;
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        fail(std::string("request log delete commit failed: ") + sqlite3_errmsg(db_));
        return -1;
    }
    return removed;
}

bool StatsDb::setCacheReset(const std::string& scope, const std::string& value) {
    if (!ok_ || !db_ || scope.empty() || value.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO cache_resets(scope,value,reset_ts) VALUES(?,?,?) "
        "ON CONFLICT(scope,value) DO UPDATE SET reset_ts=excluded.reset_ts";
    if (!prepare(sql, &st)) return false;
    sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, epochSeconds(std::chrono::system_clock::now()));
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    finishQuery(st, __func__);
    return ok;
}

std::int64_t StatsDb::cacheResetTs(const std::string& scope, const std::string& value) const {
    if (!ok_ || !db_ || scope.empty() || value.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT reset_ts FROM cache_resets WHERE scope=? AND value=?";
    if (!prepare(sql, &st)) return 0;
    sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    std::int64_t ts = 0;
    if (sqlite3_step(st) == SQLITE_ROW) ts = sqlite3_column_int64(st, 0);
    finishQuery(st, __func__);
    return ts;
}

double StatsDb::cacheRatioLocked(const std::string& extra_where,
                                 const std::vector<std::string>& binds) const {
    if (!ok_ || !db_) return 0.0;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string("SELECT COALESCE(SUM(tokens_in),0), "
                                        "COALESCE(SUM(cache_read_tokens),0) FROM requests "
                                        "WHERE 1=1") +
                            extra_where + kCacheResetFilter;
    if (!prepare(sql, &st)) return 0.0;
    for (std::size_t i = 0; i < binds.size(); ++i) {
        sqlite3_bind_text(st, static_cast<int>(i + 1), binds[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    double ratio = 0.0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ratio = ratioOf(sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 0));
    }
    finishQuery(st, __func__);
    return ratio;
}

double StatsDb::cacheHitRatio() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cacheRatioLocked({}, {});
}

double StatsDb::sessionCacheHitRatio(const std::string& session_id) const {
    if (session_id.empty()) return 0.0;
    std::lock_guard<std::mutex> lock(mu_);
    return cacheRatioLocked(" AND session_id=?", {session_id});
}

std::optional<double> StatsDb::sessionModelCacheHitRatio(const std::string& session_id,
                                                         const std::string& model) const {
    if (session_id.empty() || model.empty() || !ok_.load() || !db_) return std::nullopt;
    // Известность определяет наличие записей пары, а не значение доли: 0 %
    // попаданий — такой же факт, как и любое другое значение.
    {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        if (!prepare("SELECT COUNT(*) FROM requests WHERE session_id=?1 AND model=?2", &st)) {
            return std::nullopt;
        }
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
        const bool has_rows = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0;
        finishQuery(st, __func__);
        if (!has_rows) return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mu_);
    return cacheRatioLocked(" AND session_id=? AND model=?", {session_id, model});
}

Json::Value StatsDb::emptyDailyRow(const std::string& day) {
    Json::Value x(Json::objectValue);
    x["day"] = day;
    x["requests"] = static_cast<Json::Int64>(0);
    x["estimated_requests"] = static_cast<Json::Int64>(0);
    x["errors"] = static_cast<Json::Int64>(0);
    x["warnings"] = static_cast<Json::Int64>(0);
    x["tokens_in"] = static_cast<Json::Int64>(0);
    x["tokens_out"] = static_cast<Json::Int64>(0);
    x["cache_read_tokens"] = static_cast<Json::Int64>(0);
    x["cache_write_tokens"] = static_cast<Json::Int64>(0);
    x["cache_hit_ratio"] = 0.0;
    x["cost_nano"] = static_cast<Json::Int64>(0);
    x["cost_rub"] = formatRub(0);
    x["cost_estimated_nano"] = static_cast<Json::Int64>(0);
    x["cost_estimated_rub"] = formatRub(0);
    x["cost_input_nano"] = static_cast<Json::Int64>(0);
    x["cost_input_rub"] = formatRub(0);
    x["cost_cache_read_nano"] = static_cast<Json::Int64>(0);
    x["cost_cache_read_rub"] = formatRub(0);
    x["cost_output_nano"] = static_cast<Json::Int64>(0);
    x["cost_output_rub"] = formatRub(0);
    x["cache_saved_nano"] = static_cast<Json::Int64>(0);
    return x;
}

Json::Value StatsDb::usageByModelProvider(const std::string& session_id,
                                          bool apply_cache_reset,
                                          const std::string& model) const {
    Json::Value arr(Json::arrayValue);
    if (!ok_.load() || !db_) return arr;
    const std::string sql =
        std::string("SELECT model, provider_tag, COUNT(*),"
                    " COALESCE(SUM(tokens_in),0), COALESCE(SUM(tokens_out),0),"
                    " COALESCE(SUM(") +
        std::string(apply_cache_reset
                        ? std::string("CASE WHEN ") + kAfterCacheReset +
                              " THEN cache_read_tokens ELSE 0 END"
                        : "cache_read_tokens") +
        "),0),"
        " COALESCE(SUM(cost_input_nano),0), COALESCE(SUM(cost_cache_read_nano),0),"
        " COALESCE(SUM(cost_output_nano),0), COALESCE(SUM(cost_nano),0),"
        " COALESCE(SUM(cache_saved_nano),0), COALESCE(SUM(latency_ms),0),"
        " COALESCE(SUM(CASE WHEN tokens_in>0 THEN 1 ELSE 0 END),0),"
        " COALESCE(SUM(CASE WHEN latency_ms>0 THEN 1 ELSE 0 END),0),"
        " COALESCE(SUM(" +
        std::string(apply_cache_reset
                        ? std::string("CASE WHEN ") + kAfterCacheReset +
                              " AND tokens_in>0 THEN 1 ELSE 0 END"
                        : "CASE WHEN tokens_in>0 THEN 1 ELSE 0 END") +
        "),0) "
        "FROM requests WHERE status < 400 AND (?1='' OR session_id=?1)"
        " AND (?2='' OR model=?2) "
        "GROUP BY model, provider_tag ORDER BY model, provider_tag";
    sqlite3_stmt* st = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (!prepare(sql.c_str(), &st)) return arr;
    sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, model.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value x(Json::objectValue);
        const auto requests = sqlite3_column_int64(st, 2);
        const auto tokens_in = sqlite3_column_int64(st, 3);
        const auto tokens_out = sqlite3_column_int64(st, 4);
        const auto cache_read = sqlite3_column_int64(st, 5);
        const auto latency = sqlite3_column_int64(st, 11);
        x["model"] = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        x["provider"] = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        x["requests"] = static_cast<Json::Int64>(requests);
        x["tokens_in"] = static_cast<Json::Int64>(tokens_in);
        x["tokens_out"] = static_cast<Json::Int64>(tokens_out);
        x["cache_read_tokens"] = static_cast<Json::Int64>(cache_read);
        x["cache_hit_ratio"] = ratioOf(cache_read, tokens_in);
        x["cost_input_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 6));
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 7));
        x["cost_output_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 8));
        x["cost_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 9));
        x["cache_saved_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 10));
        x["latency_ms_sum"] = static_cast<Json::Int64>(latency);
        x["token_observations"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 12));
        x["latency_observations"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 13));
        x["cache_observations"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 14));
        x["avg_latency_ms"] = static_cast<Json::Int64>(requests > 0 ? latency / requests : 0);
        x["tokens_per_second"] = latency > 0
                                     ? static_cast<double>(tokens_out) * 1000.0 /
                                           static_cast<double>(latency)
                                     : 0.0;
        arr.append(x);
    }
    finishQuery(st, __func__);
    return arr;
}

Json::Value StatsDb::recentProviders(int limit) const {
    Json::Value arr(Json::arrayValue);
    if (!ok_ || !db_ || limit <= 0) return arr;
    limit = std::clamp(limit, 1, 50);
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT provider_tag, COUNT(*), COALESCE(SUM(cost_nano),0),"
        " COALESCE(SUM(cost_input_nano),0), COALESCE(SUM(cost_cache_read_nano),0),"
        " COALESCE(SUM(cost_output_nano),0),"
        " COALESCE(MIN(ts),0), COALESCE(MAX(ts),0) "
        "FROM requests WHERE provider_tag<>'' "
        "GROUP BY provider_tag ORDER BY MAX(ts) DESC LIMIT ?";
    if (!prepare(sql, &st)) return arr;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value x(Json::objectValue);
        x["provider"] = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        x["requests"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 1));
        const auto cost = sqlite3_column_int64(st, 2);
        x["cost_nano"] = static_cast<Json::Int64>(cost);
        x["cost_rub"] = formatRub(cost);
        x["cost_input_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 3));
        x["cost_cache_read_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 4));
        x["cost_output_nano"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 5));
        x["first_ts"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 6));
        x["last_ts"] = static_cast<Json::Int64>(sqlite3_column_int64(st, 7));
        arr.append(x);
    }
    finishQuery(st, __func__);
    return arr;
}

std::int64_t StatsDb::providerSwitchTs(const std::string& latest,
                                       const std::string& previous) const {
    if (!ok_ || !db_ || latest.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    std::int64_t ts = 0;
    if (previous.empty()) {
        const char* sql = "SELECT COALESCE(MIN(ts),0) FROM requests WHERE provider_tag=?";
        if (!prepare(sql, &st)) return 0;
        sqlite3_bind_text(st, 1, latest.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        const char* sql =
            "SELECT COALESCE(MIN(ts),0) FROM requests WHERE provider_tag=? AND ts>="
            "(SELECT COALESCE(MAX(ts),0) FROM requests WHERE provider_tag=?)";
        if (!prepare(sql, &st)) return 0;
        sqlite3_bind_text(st, 1, latest.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, previous.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) == SQLITE_ROW) ts = sqlite3_column_int64(st, 0);
    finishQuery(st, __func__);
    return ts;
}

NanoRub StatsDb::tokenDayCost(const std::string& token_id, const std::string& day) const {
    if (!ok_ || !db_ || token_id.empty() || day.empty()) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT COALESCE(SUM(cost_nano),0) FROM daily WHERE day=? AND token_id=?";
    if (!prepare(sql, &st)) return 0;
    sqlite3_bind_text(st, 1, day.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, token_id.c_str(), -1, SQLITE_TRANSIENT);
    NanoRub cost = 0;
    if (sqlite3_step(st) == SQLITE_ROW) cost = sqlite3_column_int64(st, 0);
    finishQuery(st, __func__);
    return cost;
}

std::size_t StatsDb::sessionCount() const {
    if (!ok_ || !db_) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    if (!prepare("SELECT COUNT(DISTINCT session_id) FROM requests", &st)) return 0;
    std::size_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        count = static_cast<std::size_t>(sqlite3_column_int64(st, 0));
    }
    finishQuery(st, __func__);
    return count;
}

std::int64_t StatsDb::requestCount() const {
    if (!ok_ || !db_) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_stmt* st = nullptr;
    if (!prepare("SELECT COUNT(*) FROM requests", &st)) return 0;
    std::int64_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int64(st, 0);
    finishQuery(st, __func__);
    return count;
}

int StatsDb::purge(int request_retention_days, int aggregate_retention_days) {
    if (!ok_ || !db_) return 0;
    if (request_retention_days < 0) request_retention_days = 0;
    if (aggregate_retention_days < 0) aggregate_retention_days = 0;
    std::lock_guard<std::mutex> lock(mu_);
    int removed = 0;
    const auto now = std::chrono::system_clock::now();
    if (request_retention_days > 0) {
        const auto cutoff = epochSeconds(now - std::chrono::hours(24 * request_retention_days));
        sqlite3_stmt* st = nullptr;
        if (prepare("DELETE FROM requests WHERE ts<?", &st)) {
            sqlite3_bind_int64(st, 1, cutoff);
            if (sqlite3_step(st) != SQLITE_DONE) fail(sqlite3_errmsg(db_));
            removed += sqlite3_changes(db_);
            finishQuery(st, __func__);
        }
    }
    if (aggregate_retention_days > 0) {
        const auto cutoff = utcDayString(now - std::chrono::hours(24 * aggregate_retention_days));
        sqlite3_stmt* st = nullptr;
        if (prepare("DELETE FROM daily WHERE day<?", &st)) {
            sqlite3_bind_text(st, 1, cutoff.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) != SQLITE_DONE) fail(sqlite3_errmsg(db_));
            removed += sqlite3_changes(db_);
            finishQuery(st, __func__);
        }
    }
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
    return removed;
}

} // namespace proxy