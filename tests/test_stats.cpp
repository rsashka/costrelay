#include "TestHelpers.h"
#include "stats/Stats.h"
#include "stats/StatsDb.h"

#include <gtest/gtest.h>

#include <chrono>

using namespace proxy;

TEST(Stats, AggregatesAndCapsRing) {
    Stats stats(3);
    RequestRecord r;
    r.request_id = "a";
    r.model = "m";
    r.status = 200;
    r.latency_ms = 10;
    r.tokens_in = 5;
    r.tokens_out = 7;
    r.cost_actual_nano = 1000;
    r.cost_estimated = false;
    stats.record(r);
    r.request_id = "b";
    r.status = 500;
    r.latency_ms = 30;
    r.tokens_in = 0;
    r.tokens_out = 0;
    r.cost_actual_nano = 2000;
    r.cost_estimated = false;
    stats.record(r);
    r.request_id = "c";
    r.status = 200;
    r.stream = true;
    r.tokens_in = 0;
    r.tokens_out = 0;
    r.cost_actual_nano = 0;
    stats.record(r);
    r.request_id = "d";
    stats.record(r);
    auto ov = stats.overview();
    EXPECT_EQ(ov["requests_total"].asInt64(), 4);
    EXPECT_EQ(ov["errors_total"].asInt64(), 1);
    EXPECT_EQ(ov["tokens_in_total"].asInt64(), 5);
    EXPECT_EQ(ov["tokens_out_total"].asInt64(), 7);
    EXPECT_EQ(ov["cost_nano_total"].asInt64(), 3000);
    auto recent = stats.recent(10);
    ASSERT_EQ(recent.size(), 3u);
    EXPECT_EQ(recent.front().request_id, "b");
    EXPECT_EQ(recent.back().request_id, "d");
    stats.incActiveStreams(1);
    EXPECT_EQ(stats.overview()["active_streams"].asInt64(), 1);
}

TEST(Stats, SessionsAndCacheAggregates) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "r1";
    r.token_id = "tok";
    r.session_id = "session-a";
    r.model = "m";
    r.provider_tag = "local";
    r.status = 200;
    r.latency_ms = 20;
    r.tokens_in = 100;
    r.tokens_out = 20;
    r.cache_read_tokens = 60;
    r.cache_write_tokens = 10;
    r.cost_actual_nano = 1000;
    r.cost_estimated = false;
    r.cache_saved_nano = 400;
    r.ts = std::chrono::system_clock::now();
    stats.record(r);
    r.request_id = "r2";
    r.tokens_in = 50;
    r.tokens_out = 10;
    r.cache_read_tokens = 20;
    r.cost_actual_nano = 500;
    r.cache_saved_nano = 100;
    stats.record(r);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("session-a"), 80.0 / 150.0);
    EXPECT_EQ(stats.sessionsJson(10).size(), 1u);
    auto sessions = stats.sessionsJson(10);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["session_id"].asString(), "session-a");
    EXPECT_EQ(sessions[0]["requests"].asInt64(), 2);
    EXPECT_EQ(sessions[0]["cache_read_tokens"].asInt64(), 80);
    EXPECT_EQ(sessions[0]["max_cache_read_tokens"].asInt64(), 60);
    auto filtered = stats.recentJson(10, "session-a");
    ASSERT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0]["cache_read_tokens"].asInt64(), 20);
    auto second_page = stats.recentJson(1, "session-a", "", "", 1);
    ASSERT_EQ(second_page.size(), 1u);
    EXPECT_EQ(second_page[0]["request_id"].asString(), "r1");
    EXPECT_TRUE(stats.recentJson(10, "missing").empty());
    auto ov = stats.overview();
    EXPECT_EQ(ov["cache_read_tokens_total"].asInt64(), 80);
    EXPECT_EQ(ov["cache_saved_nano"].asInt64(), 500);
    EXPECT_DOUBLE_EQ(ov["cache_hit_ratio"].asDouble(), 80.0 / 150.0);
    EXPECT_EQ(ov["sessions_total"].asInt64(), 1);
    const auto metrics = stats.prometheus();
    EXPECT_NE(metrics.find("costrelay_cache_read_tokens_total 80"), std::string::npos);
    EXPECT_NE(metrics.find("costrelay_sessions_total 1"), std::string::npos);
    EXPECT_NE(metrics.find("costrelay_cache_hit_ratio"), std::string::npos);
}

TEST(Stats, CacheHitRatioIsPerSessionAndModel) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "a";
    r.token_id = "t";
    r.session_id = "s";
    r.model = "local/strata";
    r.provider_tag = "local";
    r.tokens_in = 100;
    r.cache_read_tokens = 90;
    r.ts = std::chrono::system_clock::now();
    stats.record(r);
    r.request_id = "b";
    r.model = "deepseek/deepseek-v4-pro";
    r.provider_tag = "remote-a";
    r.tokens_in = 100;
    r.cache_read_tokens = 10;
    stats.record(r);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s", "local/strata"), 0.9);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s", "deepseek/deepseek-v4-pro"), 0.1);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s"), 0.5);
}

TEST(Stats, ObservedProfileAveragesSuccessfulRequests) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "a";
    r.model = "mock/model-a";
    r.status = 200;
    r.tokens_in = 100;
    r.tokens_out = 20;
    r.cache_read_tokens = 40;
    stats.record(r);
    r.request_id = "b";
    r.tokens_in = 200;
    r.tokens_out = 40;
    r.cache_read_tokens = 60;
    stats.record(r);
    r.request_id = "error";
    r.status = 500;
    r.tokens_in = 1000;
    r.tokens_out = 1000;
    r.cache_read_tokens = 0;
    stats.record(r);

    const auto profile = stats.observedProfile("mock/model-a");
    EXPECT_EQ(profile.observations, 2);
    EXPECT_EQ(profile.input_tokens, 150);
    EXPECT_EQ(profile.output_tokens, 30);
    EXPECT_EQ(profile.cached_tokens, 50);
    EXPECT_EQ(stats.observedProfile("missing").observations, 0);
}

TEST(Stats, ObservedProfileWindowUsesLastRequests) {
    Stats stats(10);
    RequestRecord r;
    r.model = "mock/model-a";
    r.status = 200;
    r.tokens_in = 100;
    r.tokens_out = 10;
    r.cache_read_tokens = 50;
    r.ts = std::chrono::system_clock::time_point(std::chrono::seconds(100));
    stats.record(r);
    r.request_id = "b";
    r.tokens_in = 200;
    r.tokens_out = 20;
    r.cache_read_tokens = 100;
    r.ts = std::chrono::system_clock::time_point(std::chrono::seconds(200));
    stats.record(r);
    r.request_id = "c";
    r.tokens_in = 300;
    r.tokens_out = 30;
    r.cache_read_tokens = 150;
    r.ts = std::chrono::system_clock::time_point(std::chrono::seconds(300));
    stats.record(r);

    // Окно — последние запросы модели, а не средняя за всё время.
    const auto window = stats.observedProfile("mock/model-a", 2);
    EXPECT_EQ(window.observations, 2);
    EXPECT_EQ(window.input_tokens, 250);
    EXPECT_EQ(window.output_tokens, 25);
    EXPECT_EQ(window.cached_tokens, 125);
    const auto lifetime = stats.observedProfile("mock/model-a", 0);
    EXPECT_EQ(lifetime.observations, 3);
    EXPECT_EQ(lifetime.input_tokens, 200);
    // Окно шире кольца отдаёт все записи, а не ноль.
    EXPECT_EQ(stats.observedProfile("mock/model-a", 100).observations, 3);
    EXPECT_EQ(stats.observedProfile("missing", 5).observations, 0);
}

TEST(Stats, ObservedProfileClampsCacheToInput) {
    Stats stats(10);
    RequestRecord r;
    r.model = "mock/model-a";
    r.status = 200;
    r.tokens_in = 10;
    r.tokens_out = 1;
    r.cache_read_tokens = 100;
    stats.record(r);
    const auto profile = stats.observedProfile("mock/model-a");
    ASSERT_EQ(profile.observations, 1);
    EXPECT_EQ(profile.input_tokens, 10);
    EXPECT_EQ(profile.cached_tokens, 10);
}

TEST(Stats, CacheMissStreakCountsConsecutiveMisses) {
    Stats stats(10);
    RequestRecord r;
    r.session_id = "s";
    r.model = "mock/model-a";
    r.provider_tag = "fast";
    r.status = 200;
    r.tokens_in = 100;
    for (int i = 0; i < 4; ++i) {
        r.request_id = "miss" + std::to_string(i);
        r.cache_read_tokens = 0;
        stats.record(r);
    }
    EXPECT_EQ(stats.cacheMissStreak("s", "mock/model-a", "fast"), 4);
    r.request_id = "hit";
    r.cache_read_tokens = 50;
    stats.record(r);
    EXPECT_EQ(stats.cacheMissStreak("s", "mock/model-a", "fast"), 0);
    EXPECT_EQ(stats.cacheMissStreak("s", "mock/model-a", "other"), 0);
}

// Ограничение таблицы серий вытесняет самую старую запись, а не первый ключ
// std::map (лексикографически меньший): иначе теряется активная серия, и порог
// чёрного списка по кешу не срабатывает.
TEST(Stats, CacheStreakEvictionDropsOldestNotFirstKey) {
    const int limit = 4;
    Stats stats(4, CacheLossSpikeConfig{}, StatsLimits{limit, limit, limit});
    const int count = limit + 1;
    for (int i = 0; i < count; ++i) {
        RequestRecord r;
        r.request_id = "r" + std::to_string(i);
        // Ключи вставляются в порядке, обратном лексикографическому: первый по
        // алфавиту ключ — самый новый, вытесняться должен самый старый.
        const int index = count - 1 - i;
        r.session_id = "s" + std::to_string(100000 + index);
        r.model = "mock/model-a";
        r.provider_tag = "fast";
        r.status = 200;
        r.tokens_in = 10;
        r.cache_read_tokens = 0;
        r.ts = std::chrono::system_clock::time_point(std::chrono::seconds(1000 + i));
        stats.record(r);
    }
    const std::string newest = "s" + std::to_string(100000);
    const std::string oldest = "s" + std::to_string(100000 + (count - 1));
    EXPECT_EQ(stats.cacheMissStreak(newest, "mock/model-a", "fast"), 1);
    EXPECT_EQ(stats.cacheMissStreak(oldest, "mock/model-a", "fast"), 0);
}

// Метрика доли кеша проверяется значением, а не только именем: иначе любое
// неверное число проходит набор.
// Пороги серии промахов — правило Stats: потребитель получает готовый итог.
// Пороги серии промахов — ключ `routing.cache_miss`, а не константы модуля:
// предупреждение и чёрный список обязаны следовать конфигурации.
TEST(Stats, CacheMissThresholdsComeFromConfig) {
    Stats stats(10, CacheLossSpikeConfig{}, StatsLimits{}, CacheMissConfig{1, 3});
    RequestRecord r;
    r.session_id = "s";
    r.model = "m";
    r.provider_tag = "p";
    r.status = 200;
    r.tokens_in = 10;
    r.cache_read_tokens = 0;
    const auto first = stats.cacheMissOutcome("s", "m", "p");
    EXPECT_EQ(first.misses, 1);
    EXPECT_TRUE(first.warning) << "warn=1: предупреждение с первого промаха";
    EXPECT_FALSE(first.blacklist);
    for (int i = 0; i < 3; ++i) {
        r.request_id = "m" + std::to_string(i);
        stats.record(r);
    }
    const auto fourth = stats.cacheMissOutcome("s", "m", "p");
    EXPECT_EQ(fourth.misses, 4);
    EXPECT_TRUE(fourth.warning);
    EXPECT_TRUE(fourth.blacklist) << "blacklist=3: четвёртый промах блокирует";
}

TEST(Stats, CacheMissOutcomeOwnsThresholds) {
    Stats stats(10);
    RequestRecord r;
    r.session_id = "s";
    r.model = "m";
    r.provider_tag = "p";
    r.status = 200;
    r.tokens_in = 10;
    const auto first = stats.cacheMissOutcome("s", "m", "p");
    EXPECT_EQ(first.misses, 1);
    EXPECT_FALSE(first.warning);
    EXPECT_FALSE(first.blacklist);
    for (int i = 0; i < 4; ++i) stats.record(r);
    const auto fifth = stats.cacheMissOutcome("s", "m", "p");
    EXPECT_EQ(fifth.misses, 5);
    EXPECT_TRUE(fifth.warning);
    EXPECT_TRUE(fifth.blacklist);
}

TEST(Stats, PrometheusCacheHitRatioValue) {
    Stats stats(10);
    RequestRecord r;
    r.model = "m";
    r.status = 200;
    r.tokens_in = 100;
    r.tokens_out = 10;
    r.cache_read_tokens = 25;
    stats.record(r);
    const auto metrics = stats.prometheus();
    EXPECT_NE(metrics.find("costrelay_cache_hit_ratio 0.25"), std::string::npos) << metrics;
}

TEST(Stats, ResetCacheClearsScope) {
    Stats stats(10);
    RequestRecord r;
    r.session_id = "s";
    r.model = "mock/model-a";
    r.provider_tag = "fast";
    r.status = 200;
    r.tokens_in = 100;
    r.tokens_out = 10;
    r.cache_read_tokens = 50;
    stats.record(r);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s", "mock/model-a"), 0.5);
    ASSERT_EQ(stats.observedProfile("mock/model-a").cached_tokens, 50);

    stats.resetCache("model", "mock/model-a");
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s", "mock/model-a"), 0.0);
    EXPECT_EQ(stats.observedProfile("mock/model-a").cached_tokens, 0);
    EXPECT_EQ(stats.observedProfile("mock/model-a").input_tokens, 100);

    r.request_id = "again";
    stats.record(r);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s", "mock/model-a"), 0.5);
    stats.resetCache("session", "s");
    // Наблюдений пары (сессия, модель) нет: берётся общая доля модели.
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("s", "mock/model-a"), 0.5);
    EXPECT_EQ(stats.cacheMissStreak("s", "mock/model-a", "fast"), 0);
}

TEST(Stats, ForgetRequestsDropsMatchingRingEntries) {
    Stats stats(10);
    RequestRecord r;
    r.session_id = "s1";
    r.model = "mock/model-a";
    r.provider_tag = "fast";
    r.status = 200;
    r.tokens_in = 10;
    r.request_id = "first";
    stats.record(r);
    r.session_id = "s2";
    r.request_id = "second";
    stats.record(r);
    r.session_id = "s1";
    r.provider_tag = "slow";
    r.request_id = "third";
    stats.record(r);
    ASSERT_EQ(stats.recent(10).size(), 3u);

    EXPECT_EQ(stats.forgetRequests("s1", "mock/model-a", ""), 2u);
    auto left = stats.recent(10);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].session_id, "s2");
    EXPECT_EQ(stats.forgetRequests("", "", ""), 1u);
    EXPECT_TRUE(stats.recent(10).empty());
}

TEST(Stats, ZeroActualCostIsNotReplacedByEstimate) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "free";
    r.token_id = "t";
    r.session_id = "free";
    r.status = 200;
    r.cost_estimate_nano = 5000;
    r.cost_actual_nano = 0;
    r.cost_estimated = false;
    r.ts = std::chrono::system_clock::now();
    stats.record(r);
    EXPECT_EQ(stats.overview()["cost_nano_total"].asInt64(), 0);
    EXPECT_EQ(stats.sessionsJson(1)[0]["cost_nano"].asInt64(), 0);
}

TEST(Stats, IssueSeverityClassifiesStatusAndCodes) {
    EXPECT_EQ(issueSeverity(200, ""), 0);
    // Повтор после отказа — предупреждение, даже если запрос выполнен.
    EXPECT_EQ(issueSeverity(200, "", 1), 0);
    EXPECT_EQ(issueSeverity(200, "", 2), 1);
    EXPECT_EQ(issueSeverity(200, kCacheMissStreakCode), 1);
    EXPECT_EQ(issueSeverity(200, kCacheMissBlacklistCode), 2);
    EXPECT_EQ(issueSeverity(429, ""), 2);
    EXPECT_EQ(issueSeverity(500, kCacheMissStreakCode), 2);

    EXPECT_TRUE(issueMatches(0, IssueFilter::All));
    EXPECT_TRUE(issueMatches(1, IssueFilter::All));
    EXPECT_TRUE(issueMatches(2, IssueFilter::All));
    EXPECT_FALSE(issueMatches(0, IssueFilter::Any));
    EXPECT_FALSE(issueMatches(2, IssueFilter::Warning));
    EXPECT_TRUE(issueMatches(1, IssueFilter::Warning));
    EXPECT_TRUE(issueMatches(2, IssueFilter::Error));
    EXPECT_FALSE(issueMatches(1, IssueFilter::Error));

    EXPECT_EQ(issueFilterFromString("warning"), IssueFilter::Warning);
    EXPECT_EQ(issueFilterFromString("error"), IssueFilter::Error);
    EXPECT_EQ(issueFilterFromString("issue"), IssueFilter::Any);
    EXPECT_EQ(issueFilterFromString("nonsense"), IssueFilter::All);
}

TEST(Stats, RecentJsonFiltersByIssue) {
    Stats stats(10);
    RequestRecord r;
    r.session_id = "s";
    r.model = "m";
    r.provider_tag = "fast";
    r.status = 200;
    r.ts = std::chrono::system_clock::now();
    r.request_id = "ok";
    stats.record(r);
    r.request_id = "warn";
    r.error_code = kCacheMissStreakCode;
    stats.record(r);
    r.request_id = "error";
    r.error_code = "";
    r.status = 502;
    stats.record(r);

    const auto all = stats.recentJson(10);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0]["severity"].asInt(), 2);
    EXPECT_EQ(all[1]["severity"].asInt(), 1);
    EXPECT_EQ(all[2]["severity"].asInt(), 0);

    const auto warnings = stats.recentJson(10, "", "", "", 0, IssueFilter::Warning);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0]["request_id"].asString(), "warn");
    const auto errors = stats.recentJson(10, "", "", "", 0, IssueFilter::Error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0]["request_id"].asString(), "error");
    EXPECT_EQ(stats.recentJson(10, "", "", "", 0, IssueFilter::Any).size(), 2u);

    // Удаление из кольцевого буфера учитывает фильтр важности.
    EXPECT_EQ(stats.forgetRequests("", "", "", IssueFilter::Warning), 1u);
    auto left = stats.recent(10);
    ASSERT_EQ(left.size(), 2u);
    EXPECT_EQ(stats.forgetRequests("", "", "", IssueFilter::Error), 1u);
    EXPECT_EQ(stats.recent(10).size(), 1u);
}

namespace {

RequestRecord seriesRecord(const std::string& id, const std::string& provider,
                           std::int64_t tokens_in, std::int64_t cache_read, NanoRub cost,
                           std::chrono::system_clock::time_point ts,
                           const std::string& cause = {}) {
    RequestRecord r;
    r.request_id = id;
    r.session_id = "s1";
    r.model = "mock/model-a";
    r.provider_tag = provider;
    r.status = 200;
    r.tokens_in = tokens_in;
    r.tokens_out = 100;
    r.cache_read_tokens = cache_read;
    r.cost_actual_nano = cost;
    r.cost_estimated = false;
    r.switch_cause = cause;
    r.ts = ts;
    return r;
}

} // namespace

TEST(Stats, RecordsCacheLossSpikeOnProviderSwitch) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    stats.record(seriesRecord("b", "slow", 10500, 0, 5'000'000, t0 + std::chrono::seconds(10)));

    const auto rows = stats.recentJson(10);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["error_code"].asString(), kProviderSwitchCacheLossCode);
    EXPECT_EQ(rows[0]["severity"].asInt(), 1);
    const std::string detail = rows[0]["event_detail"].asString();
    ASSERT_FALSE(detail.empty());
    // Причина пустая: смена без названной причины — поле остаётся пустым.
    EXPECT_NE(detail.find("\"cause\":\"\""), std::string::npos);
    EXPECT_NE(detail.find("\"from\":\"fast\""), std::string::npos);
    EXPECT_NE(detail.find("\"to\":\"slow\""), std::string::npos);
    EXPECT_NE(detail.find("\"gap_sec\":10"), std::string::npos);
    EXPECT_NE(stats.prometheus().find(
                  "costrelay_route_cache_loss_total{model=\"mock/model-a\",provider=\"slow\"} 1"),
              std::string::npos);
    // Запись видна в журнале под фильтром предупреждений.
    EXPECT_EQ(stats.recentJson(10, "", "", "", 0, IssueFilter::Warning).size(), 1u);
}

TEST(Stats, CacheLossSpikeRequiresProviderSwitch) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    stats.record(seriesRecord("b", "fast", 10000, 0, 5'000'000, t0 + std::chrono::seconds(5)));
    EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
    EXPECT_TRUE(stats.recent(10)[1].event_detail.empty());
}

TEST(Stats, CacheLossSpikeRequiresPreviousCache) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 0, 1'000'000, t0));
    stats.record(seriesRecord("b", "slow", 10000, 0, 5'000'000, t0 + std::chrono::seconds(5)));
    EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
}

TEST(Stats, CacheLossSpikeWindowLimitsSeries) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    stats.record(seriesRecord("b", "slow", 10000, 0, 5'000'000, t0 + std::chrono::seconds(3600)));
    // Истечение кеша по времени — не инцидент смены провайдера.
    EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
}

TEST(Stats, CacheLossSpikeIgnoresEstimatedCost) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    auto second = seriesRecord("b", "slow", 10500, 0, 5'000'000, t0 + std::chrono::seconds(5));
    second.cost_estimated = true;
    stats.record(second);
    EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
}

TEST(Stats, CacheLossSpikeTreatsFreeProviderAsNoSeries) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    // Бесплатный предыдущий запрос: отношения стоимости нет, абсолютных порогов нет.
    stats.record(seriesRecord("a", "strata", 10000, 8000, 0, t0));
    stats.record(seriesRecord("b", "slow", 10000, 0, 5'000'000, t0 + std::chrono::seconds(5)));
    EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
}

TEST(Stats, CacheLossSpikeBelowRatioMarksRebind) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    stats.record(seriesRecord("b", "slow", 10500, 0, 2'000'000, t0 + std::chrono::seconds(5),
                             "local_rebind"));
    const auto rows = stats.recentJson(10);
    EXPECT_EQ(rows[0]["error_code"].asString(), kProviderAffinityRebindCode);
    EXPECT_TRUE(rows[0]["event_detail"].asString().empty());
}

TEST(Stats, CacheLossSpikeKeepsExistingErrorCode) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    auto second = seriesRecord("b", "slow", 10500, 0, 5'000'000, t0 + std::chrono::seconds(5));
    second.error_code = kCacheMissBlacklistCode;
    stats.record(second);
    const auto rows = stats.recentJson(10);
    // Более важный код не перезаписывается, но событие и счётчик на месте.
    EXPECT_EQ(rows[0]["error_code"].asString(), kCacheMissBlacklistCode);
    EXPECT_EQ(rows[0]["severity"].asInt(), 2);
    EXPECT_FALSE(rows[0]["event_detail"].asString().empty());
}

TEST(Stats, RebindIsRecordedWithoutPreviousRequest) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    // Первый запрос серии сразу потребовал перепривязки: предыдущего ряда нет.
    stats.record(seriesRecord("a", "slow", 10000, 0, 1'000'000, t0, "local_rebind"));
    const auto rows = stats.recentJson(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["error_code"].asString(), kProviderAffinityRebindCode);
    EXPECT_EQ(rows[0]["switch_cause"].asString(), "local_rebind");
}

TEST(Stats, PinDropByCostIsRecordedWithCause) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    // Снятие привязки по порогу стоимости: причина в ряду, инцидент — всплеск кеша.
    stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
    stats.record(seriesRecord("b", "slow", 10000, 0, 5'000'000, t0 + std::chrono::seconds(5),
                              "session_cost_rebind"));
    const auto rows = stats.recentJson(10);
    EXPECT_EQ(rows[0]["error_code"].asString(), kProviderSwitchCacheLossCode);
    EXPECT_EQ(rows[0]["switch_cause"].asString(), "session_cost_rebind");
    EXPECT_NE(rows[0]["event_detail"].asString().find("session_cost_rebind"),
              std::string::npos);
}

TEST(Stats, LatencyPerProviderAndWindow) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "l1";
    r.model = "m";
    r.provider_tag = "p1";
    r.status = 200;
    r.latency_ms = 100;
    stats.record(r);
    r.request_id = "l2";
    r.latency_ms = 300;
    stats.record(r);
    EXPECT_EQ(stats.latencyMs("m", "p1"), 200);
    EXPECT_EQ(stats.latencyMs("m", "p1", 1), 300);
    EXPECT_EQ(stats.latencyMs("m", "p2"), 0);
    // Отказ провайдера в среднюю задержку не входит.
    r.request_id = "l3";
    r.provider_tag = "p2";
    r.status = 500;
    r.latency_ms = 900;
    stats.record(r);
    EXPECT_EQ(stats.latencyMs("m", "p2"), 0);
}

TEST(Stats, EstimateOfRequestWithoutUsageStaysOutOfTotals) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "fact";
    r.session_id = "s";
    r.model = "m";
    r.status = 200;
    r.tokens_in = 100;
    r.tokens_out = 10;
    r.cost_actual_nano = 500;
    r.cost_estimated = false;
    r.ts = std::chrono::system_clock::now();
    stats.record(r);
    r.request_id = "estimate";
    r.tokens_in = 0;
    r.tokens_out = 0;
    r.cost_actual_nano = 0;
    r.cost_estimate_nano = 40'000'000;
    r.cost_estimated = true;
    stats.record(r);

    const auto ov = stats.overview();
    EXPECT_EQ(ov["cost_nano_total"].asInt64(), 500);
    EXPECT_EQ(ov["cost_estimated_nano"].asInt64(), 40'000'000);
    EXPECT_EQ(ov["estimated_requests"].asInt64(), 1);

    const auto sessions = stats.sessionsJson(10);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["cost_nano"].asInt64(), 500);
    EXPECT_EQ(sessions[0]["cost_estimated_nano"].asInt64(), 40'000'000);
    EXPECT_EQ(sessions[0]["estimated_requests"].asInt64(), 1);
    EXPECT_EQ(sessions[0]["requests"].asInt64(), 2);

    const auto rows = stats.recentJson(2);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]["cost_nano"].asInt64(), 0);
    EXPECT_TRUE(rows[0]["cost_estimated"].asBool());
    EXPECT_EQ(rows[0]["cost_estimated_rub"].asString(), formatRub(40'000'000));
    EXPECT_EQ(rows[1]["cost_estimated_rub"].asString(), formatRub(0));
}

TEST(Stats, ProfileAndLatencySkipRequestsWithoutUsage) {
    Stats stats(10);
    RequestRecord r;
    r.request_id = "with-usage";
    r.model = "m";
    r.provider_tag = "p";
    r.status = 200;
    r.latency_ms = 100;
    r.tokens_in = 1000;
    r.tokens_out = 100;
    r.ts = std::chrono::system_clock::now();
    stats.record(r);
    // Оборванный поток: отчёта о токенах нет, в профиль не входит.
    r.request_id = "no-usage";
    r.tokens_in = 0;
    r.tokens_out = 0;
    r.cost_estimate_nano = 1000;
    r.cost_estimated = true;
    stats.record(r);

    const auto profile = stats.observedProfile("m");
    EXPECT_EQ(profile.observations, 1);
    EXPECT_EQ(profile.input_tokens, 1000);
    EXPECT_EQ(profile.output_tokens, 100);
    EXPECT_EQ(stats.latencyMs("m", "p"), 100);
}

// Таблица (сессия, модель) вытесняется по возрасту, а не по первому ключу
// std::map: иначе пропадает доля кеша активной пары.
TEST(Stats, SessionModelEvictionDropsOldestNotFirstKey) {
    const std::size_t limit = 3;
    Stats stats(8, CacheLossSpikeConfig{}, StatsLimits{limit, limit, limit, limit});
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::seconds(1000));
    auto add = [&](const std::string& session, const std::string& model, std::int64_t cached,
                   int seconds) {
        RequestRecord r;
        r.request_id = session + "-" + model;
        r.session_id = session;
        r.model = model;
        r.status = 200;
        r.tokens_in = 100;
        r.cache_read_tokens = cached;
        r.ts = t0 + std::chrono::seconds(seconds);
        stats.record(r);
    };
    // Пара (s, old): 10 % попаданий. Другая сессия по той же модели даёт
    // модельной доле другое значение — по нему видно, что пара вытеснена.
    add("s", "old", 10, 1);
    add("other", "old", 100, 2);
    add("s", "mid", 50, 3);
    EXPECT_NEAR(stats.cacheHitRatio("s", "old"), 0.1, 1e-9);
    add("s", "new", 90, 4);  // четвёртый ключ: вытесняется самый старый (s, old)
    // Пара (s, old) вытеснена: остаётся модельная доля (10 + 100) / 200.
    EXPECT_NEAR(stats.cacheHitRatio("s", "old"), 110.0 / 200.0, 1e-9);
    EXPECT_NEAR(stats.cacheHitRatio("s", "mid"), 0.5, 1e-9);
    EXPECT_NEAR(stats.cacheHitRatio("s", "new"), 0.9, 1e-9);
}

// Ноль попаданий — факт, а не отсутствие данных: пара (сессия, модель) без
// попаданий не подменяется долей всей модели.
TEST(Stats, ZeroSessionModelCacheRatioIsAFact) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    Stats stats(8);
    stats.attach(&db);
    const auto t0 = std::chrono::system_clock::time_point(std::chrono::seconds(2000));
    auto add = [&](const std::string& session, std::int64_t cached, int seconds) {
        RequestRecord r;
        r.request_id = session + "-" + std::to_string(seconds);
        r.session_id = session;
        r.model = "m";
        r.status = 200;
        r.tokens_in = 100;
        r.cache_read_tokens = cached;
        r.ts = t0 + std::chrono::seconds(seconds);
        stats.record(r);
    };
    add("zero", 0, 1);
    add("full", 100, 2);
    // Модельная доля — 0.5; доля пары (zero) — честный ноль.
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("zero", "m"), 0.0);
    EXPECT_NEAR(stats.cacheHitRatio("full", "m"), 1.0, 1e-9);
}

// Негативные случаи инцидента: без отчёта о токенах, при отказе и при попадании
// в кеш инцидент не пишется — правило проверяется с двух сторон.
TEST(Stats, CacheLossSpikeRequiresUsageSuccessAndMiss) {
    const auto t0 = std::chrono::system_clock::now();
    {
        Stats stats(64);
        stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
        stats.record(seriesRecord("b", "slow", 0, 0, 5'000'000,
                                  t0 + std::chrono::seconds(5)));
        EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
    }
    {
        Stats stats(64);
        stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
        auto failed = seriesRecord("b", "slow", 10000, 0, 5'000'000,
                                   t0 + std::chrono::seconds(5));
        failed.status = 500;
        stats.record(failed);
        EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
    }
    {
        Stats stats(64);
        stats.record(seriesRecord("a", "fast", 10000, 8000, 1'000'000, t0));
        // Попадание в кеш у нового провайдера: кеш не потерян.
        stats.record(seriesRecord("b", "slow", 10000, 4000, 5'000'000,
                                  t0 + std::chrono::seconds(5)));
        EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
    }
}

// Метка перепривязки — только у успешного запроса с названной причиной.
TEST(Stats, AffinityRebindRequiresSuccessAndCause) {
    Stats stats(64);
    const auto t0 = std::chrono::system_clock::now();
    auto failed = seriesRecord("a", "slow", 10000, 0, 1'000'000, t0, "local_rebind");
    failed.status = 500;
    stats.record(failed);
    EXPECT_TRUE(stats.recent(10)[0].error_code.empty());

    auto no_cause = seriesRecord("b", "slow", 10000, 0, 1'000'000,
                                 t0 + std::chrono::seconds(5));
    stats.record(no_cause);
    EXPECT_TRUE(stats.recent(10)[1].error_code.empty());
}
