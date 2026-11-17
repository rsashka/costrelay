#include "TestHelpers.h"
#include "stats/StatsDb.h"

#include <sqlite3.h>

#include <gtest/gtest.h>

#include <chrono>
#include <sstream>

using namespace proxy;

namespace {
RequestRecord rec(const std::string& id, const std::string& token, int status,
                  NanoRub cost, std::int64_t tin, std::int64_t tout,
                  std::chrono::system_clock::time_point ts) {
    RequestRecord r;
    r.request_id = id;
    r.token_id = token;
    r.model = "mock/model-a";
    r.router_id = "r1";
    r.provider_tag = "fast";
    r.status = status;
    r.stream = false;
    r.latency_ms = 12;
    r.ttfb_ms = 5;
    r.bytes_in = 100;
    r.bytes_out = 200;
    r.tokens_in = tin;
    r.tokens_out = tout;
    r.cost_actual_nano = cost;
    r.cost_estimated = false;
    r.ts = ts;
    return r;
}
} // namespace

TEST(StatsDb, PersistsTotalsAndDaily) {
    test::TempDir dir;
    const auto path = (dir.path() / "stats.db").string();
    StatsDb db(path);
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(db.record(rec("a", "t1", 200, 1000, 10, 20, now)));
    ASSERT_TRUE(db.record(rec("b", "t1", 500, 2000, 5, 0, now)));
    ASSERT_TRUE(db.record(rec("c", "t2", 200, 3000, 1, 2, now)));

    auto totals = db.totals();
    EXPECT_EQ(totals["requests_total"].asInt64(), 3);
    EXPECT_EQ(totals["errors_total"].asInt64(), 1);
    EXPECT_EQ(totals["tokens_in_total"].asInt64(), 16);
    EXPECT_EQ(totals["tokens_out_total"].asInt64(), 22);
    EXPECT_EQ(totals["cost_nano_total"].asInt64(), 6000);
    EXPECT_EQ(db.tokenDayCost("t1", utcDayString(now)), 3000);

    auto daily = db.daily(30);
    ASSERT_EQ(daily.size(), 1u);
    EXPECT_EQ(daily[0]["requests"].asInt64(), 3);
    EXPECT_EQ(daily[0]["errors"].asInt64(), 1);
    EXPECT_EQ(daily[0]["cost_nano"].asInt64(), 6000);

    // A separate connection sees the persisted rows.
    StatsDb reopened(path);
    ASSERT_TRUE(reopened.ok());
    EXPECT_EQ(reopened.totals()["requests_total"].asInt64(), 3);
    EXPECT_EQ(reopened.requestCount(), 3);
}

TEST(StatsDb, UsesActualCostAndUpsertsRequest) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    const auto now = std::chrono::system_clock::now();
    auto r = rec("same", "t", 200, 1000, 1, 1, now);
    r.cost_actual_nano = 700;
    ASSERT_TRUE(db.record(r));
    r.cost_actual_nano = 400;
    ASSERT_TRUE(db.record(r)); // same request_id must not double count
    auto totals = db.totals();
    EXPECT_EQ(totals["requests_total"].asInt64(), 1);
    EXPECT_EQ(totals["cost_nano_total"].asInt64(), 400);
}

TEST(StatsDb, UsageByModelProviderIncludesCacheCost) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    auto r = rec("usage-1", "t", 200, 0, 10, 5, now);
    r.cache_read_tokens = 4;
    r.cost_input_nano = 90;
    r.cost_cache_read_nano = 30;
    r.cost_output_nano = 80;
    r.cost_actual_nano = 200;
    r.cost_estimated = false;
    ASSERT_TRUE(db.record(r));
    auto failed = rec("usage-failed", "t", 500, 999, 100, 100, now);
    ASSERT_TRUE(db.record(failed));

    const auto usage = db.usageByModelProvider();
    ASSERT_EQ(usage.size(), 1u);
    EXPECT_EQ(usage[0]["model"].asString(), "mock/model-a");
    EXPECT_EQ(usage[0]["provider"].asString(), "fast");
    EXPECT_EQ(usage[0]["requests"].asInt64(), 1);
    EXPECT_EQ(usage[0]["tokens_in"].asInt64(), 10);
    EXPECT_EQ(usage[0]["tokens_out"].asInt64(), 5);
    EXPECT_EQ(usage[0]["cache_read_tokens"].asInt64(), 4);
    EXPECT_EQ(usage[0]["cost_input_nano"].asInt64(), 90);
    EXPECT_EQ(usage[0]["cost_cache_read_nano"].asInt64(), 30);
    EXPECT_EQ(usage[0]["cost_output_nano"].asInt64(), 80);
    EXPECT_EQ(usage[0]["cost_nano"].asInt64(), 200);
}

TEST(StatsDb, PurgesOldRequestsAndAggregates) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    const auto now = std::chrono::system_clock::now();
    const auto old = now - std::chrono::hours(24 * 10);
    ASSERT_TRUE(db.record(rec("new", "t", 200, 1000, 1, 1, now)));
    ASSERT_TRUE(db.record(rec("old", "t", 200, 1000, 1, 1, old)));

    const int removed = db.purge(7, 90);
    EXPECT_EQ(removed, 1);
    // Итоги считаются по суточным агрегатам: чистка журнала их не трогает,
    // агрегаты живут дольше (aggregates_retention_days).
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 2);
    EXPECT_EQ(db.requestCount(), 1);
    EXPECT_EQ(db.daily(90).size(), 2u);

    const int removed_aggr = db.purge(7, 5);
    EXPECT_EQ(removed_aggr, 1);
    EXPECT_EQ(db.daily(90).size(), 1u);
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 1);
}

TEST(StatsDb, DeletesFilteredRequestsAndDailyContribution) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    auto other = rec("other", "t1", 200, 4000, 3, 4, now);
    other.model = "mock/model-b";
    other.provider_tag = "slow";
    other.cost_input_nano = 100;
    other.cost_cache_read_nano = 50;
    other.cost_output_nano = 200;
    ASSERT_TRUE(db.record(rec("a", "t1", 200, 1000, 10, 20, now)));
    ASSERT_TRUE(db.record(rec("b", "t1", 500, 2000, 5, 0, now)));
    ASSERT_TRUE(db.record(other));

    EXPECT_EQ(db.deleteRequests("", "mock/model-a", ""), 2);
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 1);
    EXPECT_EQ(db.filteredRequestCount("", "mock/model-a", ""), 0);
    EXPECT_EQ(db.requests("", 10, "mock/model-a", "", 0).size(), 0u);
    // Суточный агрегат уменьшен, но чужие записи дня остались.
    EXPECT_EQ(db.tokenDayCost("t1", utcDayString(now)), 4000);
    auto daily = db.daily(30);
    ASSERT_EQ(daily.size(), 1u);
    EXPECT_EQ(daily[0]["requests"].asInt64(), 1);
    EXPECT_EQ(daily[0]["errors"].asInt64(), 0);
    EXPECT_EQ(daily[0]["tokens_in"].asInt64(), 3);
    EXPECT_EQ(daily[0]["cost_nano"].asInt64(), 4000);
    EXPECT_EQ(daily[0]["cost_input_nano"].asInt64(), 100);
    EXPECT_EQ(daily[0]["cost_cache_read_nano"].asInt64(), 50);
    EXPECT_EQ(daily[0]["cost_output_nano"].asInt64(), 200);

    // Фильтр по провайдеру удаляет остаток и не уводит агрегат в минус.
    EXPECT_EQ(db.deleteRequests("", "", "slow"), 1);
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 0);
    auto empty = db.daily(30);
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0]["requests"].asInt64(), 0);
    EXPECT_EQ(empty[0]["cost_nano"].asInt64(), 0);
    EXPECT_EQ(empty[0]["cost_input_nano"].asInt64(), 0);
    EXPECT_EQ(empty[0]["cost_cache_read_nano"].asInt64(), 0);
    EXPECT_EQ(empty[0]["cost_output_nano"].asInt64(), 0);
}

TEST(StatsDb, DeleteBySessionKeepsOtherSessions) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    const auto now = std::chrono::system_clock::now();
    auto keep = rec("keep", "t1", 200, 500, 2, 3, now);
    keep.session_id = "s-keep";
    auto drop = rec("drop", "t1", 200, 700, 4, 5, now);
    drop.session_id = "s-drop";
    ASSERT_TRUE(db.record(keep));
    ASSERT_TRUE(db.record(drop));

    EXPECT_EQ(db.deleteRequests("s-drop", "", ""), 1);
    auto sessions = db.sessions(10);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["session_id"].asString(), "s-keep");
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 1);
    // Фильтр, под который ничего не подходит, ничего не удаляет.
    EXPECT_EQ(db.deleteRequests("missing", "", ""), 0);
}

TEST(StatsDb, DegradesWithoutPath) {
    StatsDb db("");
    EXPECT_FALSE(db.ok());
    EXPECT_FALSE(db.lastError().empty());
    EXPECT_FALSE(db.record(RequestRecord{}));
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 0);
}

TEST(StatsDb, StatsAttachPersistsRecords) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    Stats stats(10);
    stats.attach(&db);
    auto r = rec("r", "t", 200, 1000, 1, 2, std::chrono::system_clock::now());
    stats.record(r);
    EXPECT_EQ(stats.overview()["requests_total"].asInt64(), 1);
    EXPECT_EQ(db.totals()["requests_total"].asInt64(), 1);
    const auto metrics = stats.prometheus();
    EXPECT_NE(metrics.find("costrelay_requests_total 1"), std::string::npos);
    EXPECT_NE(metrics.find("costrelay_route_requests_total{router=\"r1\",provider=\"fast\"} 1"),
              std::string::npos);
    EXPECT_NE(metrics.find("costrelay_requests_by_status{status=\"200\"} 1"), std::string::npos);
}
TEST(StatsDb, PersistsSessionsAndCacheBreakdown) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    auto a = rec("a", "t1", 200, 1000, 100, 20, now);
    a.session_id = "s1";
    a.cache_read_tokens = 60;
    a.cache_write_tokens = 5;
    a.cost_input_nano = 300;
    a.cost_cache_read_nano = 100;
    a.cost_output_nano = 200;
    a.cache_saved_nano = 250;
    ASSERT_TRUE(db.record(a));
    auto b = rec("b", "t1", 200, 500, 50, 10, now);
    b.session_id = "s1";
    b.cache_read_tokens = 20;
    ASSERT_TRUE(db.record(b));
    auto c = rec("c", "t2", 200, 700, 30, 5, now);
    c.session_id = "s2";
    ASSERT_TRUE(db.record(c));

    EXPECT_NEAR(db.sessionCacheHitRatio("s1"), 80.0 / 150.0, 1e-9);
    ASSERT_TRUE(db.sessionModelCacheHitRatio("s1", "mock/model-a").has_value());
    EXPECT_NEAR(*db.sessionModelCacheHitRatio("s1", "mock/model-a"), 80.0 / 150.0, 1e-9);
    auto totals = db.totals();
    EXPECT_EQ(totals["cache_read_tokens_total"].asInt64(), 80);
    EXPECT_EQ(totals["cache_write_tokens_total"].asInt64(), 5);
    EXPECT_EQ(totals["cache_saved_nano"].asInt64(), 250);
    EXPECT_DOUBLE_EQ(totals["cache_hit_ratio"].asDouble(), 80.0 / 180.0);

    auto sessions = db.sessions(10);
    ASSERT_EQ(sessions.size(), 2u);
    bool found = false;
    for (const auto& s : sessions) {
        if (s["session_id"].asString() != "s1") continue;
        found = true;
        EXPECT_EQ(s["requests"].asInt64(), 2);
        EXPECT_EQ(s["tokens_in"].asInt64(), 150);
        EXPECT_EQ(s["cache_read_tokens"].asInt64(), 80);
        EXPECT_EQ(s["max_cache_read_tokens"].asInt64(), 60);
        EXPECT_EQ(s["cost_nano"].asInt64(), 1500);
        EXPECT_EQ(s["model"].asString(), "mock/model-a");
    }
    EXPECT_TRUE(found);

    auto s1_requests = db.requests("s1", 10);
    ASSERT_EQ(s1_requests.size(), 2u);
    const std::string first = s1_requests[0]["request_id"].asString();
    const std::string second = s1_requests[1]["request_id"].asString();
    EXPECT_TRUE((first == "a" && second == "b") || (first == "b" && second == "a"));
    for (const auto& x : s1_requests) {
        EXPECT_EQ(x["session_id"].asString(), "s1");
        EXPECT_EQ(x["model"].asString(), "mock/model-a");
    }
    EXPECT_EQ(db.requests("s1", 1).size(), 1u);
    EXPECT_EQ(db.requests("", 10).size(), 3u);
    EXPECT_TRUE(db.requests("missing", 10).empty());

    auto daily = db.daily(30);
    ASSERT_EQ(daily.size(), 1u);
    EXPECT_EQ(daily[0]["cache_read_tokens"].asInt64(), 80);
    EXPECT_EQ(daily[0]["cache_write_tokens"].asInt64(), 5);
    EXPECT_EQ(daily[0]["cost_input_nano"].asInt64(), 300);
    EXPECT_EQ(daily[0]["cost_cache_read_nano"].asInt64(), 100);
    EXPECT_EQ(daily[0]["cost_output_nano"].asInt64(), 200);
    EXPECT_EQ(daily[0]["cost_input_rub"].asString(), formatRub(300));
}

TEST(StatsDb, FiltersRequestsBySeverity) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(db.record(rec("ok", "t", 200, 1000, 10, 5, now)));
    auto warning = rec("warn", "t", 200, 1000, 10, 5, now + std::chrono::seconds(1));
    warning.error_code = kCacheMissStreakCode;
    ASSERT_TRUE(db.record(warning));
    auto blocked = rec("blocked", "t", 200, 1000, 10, 5, now + std::chrono::seconds(2));
    blocked.error_code = kCacheMissBlacklistCode;
    ASSERT_TRUE(db.record(blocked));
    ASSERT_TRUE(db.record(rec("error", "t", 502, 0, 0, 0, now + std::chrono::seconds(3))));

    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All), 4);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Warning), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Error), 2);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Any), 3);

    const auto warnings = db.requests("", 10, "", "", 0, IssueFilter::Warning);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0]["request_id"].asString(), "warn");
    EXPECT_EQ(warnings[0]["severity"].asInt(), 1);
    const auto errors = db.requests("", 10, "", "", 0, IssueFilter::Error);
    ASSERT_EQ(errors.size(), 2u);
    EXPECT_EQ(errors[0]["request_id"].asString(), "error");
    EXPECT_EQ(errors[0]["severity"].asInt(), 2);
    EXPECT_EQ(errors[1]["request_id"].asString(), "blocked");

    // Удаление под фильтром важности не трогает остальные записи.
    EXPECT_EQ(db.deleteRequests("", "", "", IssueFilter::Warning), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All), 3);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Warning), 0);
    EXPECT_EQ(db.deleteRequests("", "", "", IssueFilter::Error), 2);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All), 1);
    EXPECT_EQ(db.deleteRequests("", "", "", IssueFilter::All), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All), 0);
}

TEST(StatsDb, RetryIsWarningAndDayFilterIsExact) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    // Фиксированные сутки UTC: граница дня проверяется без зависимости от now.
    const auto day1 = std::chrono::system_clock::time_point(std::chrono::seconds(1767614400));
    const auto day2 = std::chrono::system_clock::time_point(std::chrono::seconds(1767700800));
    ASSERT_EQ(utcDayString(day1), "2026-01-05");
    ASSERT_EQ(utcDayString(day2), "2026-01-06");
    EXPECT_TRUE(utcDayValid("2026-01-05"));
    EXPECT_TRUE(utcDayValid("2024-02-29"));
    EXPECT_FALSE(utcDayValid("2026-02-29"));
    EXPECT_FALSE(utcDayValid("2026-13-01"));
    EXPECT_FALSE(utcDayValid("2026-1-5"));
    EXPECT_FALSE(utcDayValid(""));

    auto retried = rec("retried", "t", 200, 1000, 10, 5, day1);
    retried.attempts = 2; // повтор после отказа — предупреждение
    ASSERT_TRUE(db.record(retried));
    ASSERT_TRUE(db.record(rec("error", "t", 502, 0, 0, 0, day2)));

    const auto totals = db.totals();
    EXPECT_EQ(totals["requests_total"].asInt64(), 2);
    EXPECT_EQ(totals["warnings_total"].asInt64(), 1);
    EXPECT_EQ(totals["errors_total"].asInt64(), 1);

    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All, "2026-01-05"), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Warning, "2026-01-05"), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All, "2026-01-06"), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Warning, "2026-01-06"), 0);

    const auto first = db.requests("", 10, "", "", 0, IssueFilter::All, "2026-01-05");
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0]["request_id"].asString(), "retried");
    EXPECT_EQ(first[0]["severity"].asInt(), 1);
    const auto second = db.requests("", 10, "", "", 0, IssueFilter::All, "2026-01-06");
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0]["request_id"].asString(), "error");

    // Удаление по суткам вычитает только этот день.
    EXPECT_EQ(db.deleteRequests("", "", "", IssueFilter::All, "2026-01-05"), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All), 1);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::All, "2026-01-05"), 0);
}

TEST(StatsDb, FiltersProviderTimelineAndSpeed) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    auto a = rec("a", "t1", 200, 100, 10, 5, now - std::chrono::seconds(10));
    a.session_id = "s1";
    a.provider_tag = "fast";
    a.latency_ms = 1000;
    a.cost_actual_nano = 100;
    a.cost_estimated = false;
    a.cost_input_nano = 30;
    a.cost_cache_read_nano = 20;
    a.cost_output_nano = 50;
    ASSERT_TRUE(db.record(a));
    auto b = rec("b", "t2", 200, 200, 20, 10, now - std::chrono::seconds(5));
    b.session_id = "s2";
    b.provider_tag = "slow";
    b.latency_ms = 2000;
    b.cost_actual_nano = 200;
    b.cost_estimated = false;
    ASSERT_TRUE(db.record(b));
    auto c = rec("c", "t1", 200, 300, 30, 15, now);
    c.session_id = "s1";
    c.provider_tag = "fast";
    c.latency_ms = 3000;
    c.cost_actual_nano = 300;
    c.cost_estimated = false;
    ASSERT_TRUE(db.record(c));

    auto all = db.usageByModelProvider();
    ASSERT_EQ(all.size(), 2u);
    bool fast = false, slow = false;
    for (const auto& x : all) {
        if (x["provider"].asString() == "fast") {
            fast = true;
            EXPECT_EQ(x["requests"].asInt64(), 2);
            EXPECT_EQ(x["tokens_in"].asInt64(), 40);
            EXPECT_EQ(x["tokens_out"].asInt64(), 20);
            EXPECT_EQ(x["latency_ms_sum"].asInt64(), 4000);
            EXPECT_NEAR(x["avg_latency_ms"].asDouble(), 2000.0, 1e-9);
            EXPECT_NEAR(x["tokens_per_second"].asDouble(), 5.0, 1e-9);
        } else if (x["provider"].asString() == "slow") {
            slow = true;
            EXPECT_EQ(x["requests"].asInt64(), 1);
            EXPECT_NEAR(x["tokens_per_second"].asDouble(), 5.0, 1e-9);
        }
    }
    EXPECT_TRUE(fast);
    EXPECT_TRUE(slow);
    ASSERT_EQ(db.usageByModelProvider("s1").size(), 1u);
    EXPECT_EQ(db.usageByModelProvider("s1")[0]["requests"].asInt64(), 2);

    EXPECT_EQ(db.filteredRequestCount("", "", "fast"), 2);
    EXPECT_EQ(db.filteredRequestCount("s1", "mock/model-a", ""), 2);
    EXPECT_EQ(db.filteredRequestCount("s2"), 1);
    EXPECT_EQ(db.filteredRequestCount("missing"), 0);

    auto filtered = db.requests("", 10, "", "fast");
    ASSERT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0]["request_id"].asString(), "c");
    auto second_page = db.requests("", 1, "", "fast", 1);
    ASSERT_EQ(second_page.size(), 1u);
    EXPECT_EQ(second_page[0]["request_id"].asString(), "a");
    EXPECT_EQ(db.requests("", 10, "", "slow").size(), 1u);

    auto session_ids = db.sessions(10);
    ASSERT_EQ(session_ids.size(), 2u);
    EXPECT_EQ(session_ids[0]["session_id"].asString(), "s1");
    EXPECT_EQ(session_ids[0]["requests"].asInt64(), 2);
    auto provider_tags = db.providerTags();
    ASSERT_EQ(provider_tags.size(), 2u);
    EXPECT_EQ(provider_tags[0].asString(), "fast");
    EXPECT_EQ(provider_tags[1].asString(), "slow");

    auto providers = db.recentProviders(2);
    ASSERT_EQ(providers.size(), 2u);
    EXPECT_EQ(providers[0]["provider"].asString(), "fast");
    EXPECT_EQ(providers[1]["provider"].asString(), "slow");
    EXPECT_EQ(providers[0]["cost_nano"].asInt64(), 400);
    EXPECT_EQ(providers[0]["cost_input_nano"].asInt64(), 30);
    EXPECT_EQ(providers[0]["cost_cache_read_nano"].asInt64(), 20);
    EXPECT_EQ(providers[0]["cost_output_nano"].asInt64(), 50);
    EXPECT_EQ(providers[1]["cost_nano"].asInt64(), 200);
    EXPECT_EQ(db.providerSwitchTs("fast", "slow"),
              std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
    EXPECT_EQ(db.providerSwitchTs("slow", ""),
              std::chrono::duration_cast<std::chrono::seconds>(
                  (now - std::chrono::seconds(5)).time_since_epoch()).count());
}

TEST(StatsDb, CacheResetFiltersHitRatios) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    const auto now = std::chrono::system_clock::now();
    auto a = rec("a", "t", 200, 1000, 100, 10, now);
    a.session_id = "s";
    a.cache_read_tokens = 80;
    ASSERT_TRUE(db.record(a));
    EXPECT_NEAR(*db.sessionModelCacheHitRatio("s", "mock/model-a"), 0.8, 1e-9);

    ASSERT_TRUE(db.setCacheReset("session", "s"));
    EXPECT_GT(db.cacheResetTs("session", "s"), 0);
    // The row predates the reset, so it no longer counts.
    ASSERT_TRUE(db.sessionModelCacheHitRatio("s", "mock/model-a").has_value());
    EXPECT_DOUBLE_EQ(*db.sessionModelCacheHitRatio("s", "mock/model-a"), 0.0);

    auto b = rec("b", "t", 200, 1000, 100, 10, now + std::chrono::seconds(5));
    b.session_id = "s";
    b.cache_read_tokens = 50;
    ASSERT_TRUE(db.record(b));
    EXPECT_NEAR(*db.sessionModelCacheHitRatio("s", "mock/model-a"), 0.5, 1e-9);
}

TEST(StatsDb, StatsUsesPersistedSessionHitRatioAfterRestart) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    auto r = rec("old", "t", 200, 1000, 100, 10, std::chrono::system_clock::now());
    r.session_id = "persisted";
    r.cache_read_tokens = 70;
    ASSERT_TRUE(db.record(r));
    Stats stats(10);
    stats.attach(&db);
    EXPECT_DOUBLE_EQ(stats.cacheHitRatio("persisted"), 0.7);
}

TEST(StatsDb, AttachSeedsObservedModelProfile) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    const auto now = std::chrono::system_clock::now();
    auto a = rec("a", "t", 200, 1000, 100, 20, now);
    a.cache_read_tokens = 40;
    ASSERT_TRUE(db.record(a));
    auto b = rec("b", "t", 200, 1000, 200, 40, now);
    b.cache_read_tokens = 60;
    ASSERT_TRUE(db.record(b));
    ASSERT_TRUE(db.record(rec("error", "t", 500, 0, 1000, 1000, now)));

    Stats stats(10);
    stats.attach(&db);
    const auto profile = stats.observedProfile("mock/model-a");
    EXPECT_EQ(profile.observations, 2);
    EXPECT_EQ(profile.input_tokens, 150);
    EXPECT_EQ(profile.output_tokens, 30);
    EXPECT_EQ(profile.cached_tokens, 50);
}

TEST(StatsDb, PersistsEventDetail) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    const auto now = std::chrono::system_clock::now();
    auto r = rec("evt", "t", 200, 1000, 100, 20, now);
    r.error_code = kProviderSwitchCacheLossCode;
    r.event_detail = R"({"cause":"upstream_switch","from":"fast","to":"slow","ratio":5.0})";
    ASSERT_TRUE(db.record(r));

    const auto rows = db.requests("", 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["error_code"].asString(), kProviderSwitchCacheLossCode);
    EXPECT_EQ(rows[0]["event_detail"].asString(), r.event_detail);
    EXPECT_EQ(rows[0]["severity"].asInt(), 1);

    // Журнал фильтруется по важности: инцидент — предупреждение.
    EXPECT_EQ(db.requests("", 10, "", "", 0, IssueFilter::Warning).size(), 1u);
    EXPECT_EQ(db.requests("", 10, "", "", 0, IssueFilter::Error).size(), 0u);
}

TEST(StatsDb, AddsEventDetailColumnToExistingSchema) {
    test::TempDir dir;
    const auto path = (dir.path() / "stats.db").string();
    // Схема до появления event_detail: колонка добавляется при открытии.
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(
                  raw,
                  "CREATE TABLE requests(request_id TEXT PRIMARY KEY, ts INTEGER NOT NULL,"
                  " token_id TEXT NOT NULL DEFAULT '', session_id TEXT NOT NULL DEFAULT '',"
                  " model TEXT NOT NULL DEFAULT '', router_id TEXT NOT NULL DEFAULT '',"
                  " provider_tag TEXT NOT NULL DEFAULT '', route_reason TEXT NOT NULL DEFAULT '',"
                  " status INTEGER NOT NULL DEFAULT 0, stream INTEGER NOT NULL DEFAULT 0,"
                  " latency_ms INTEGER NOT NULL DEFAULT 0, ttfb_ms INTEGER NOT NULL DEFAULT 0,"
                  " bytes_in INTEGER NOT NULL DEFAULT 0, bytes_out INTEGER NOT NULL DEFAULT 0,"
                  " tokens_in INTEGER NOT NULL DEFAULT 0, tokens_out INTEGER NOT NULL DEFAULT 0,"
                  " cache_read_tokens INTEGER NOT NULL DEFAULT 0,"
                  " cache_write_tokens INTEGER NOT NULL DEFAULT 0,"
                  " cache_size_tokens INTEGER NOT NULL DEFAULT 0, cost_nano INTEGER NOT NULL DEFAULT 0,"
                  " cost_input_nano INTEGER NOT NULL DEFAULT 0,"
                  " cost_cache_read_nano INTEGER NOT NULL DEFAULT 0,"
                  " cost_output_nano INTEGER NOT NULL DEFAULT 0,"
                  " cache_saved_nano INTEGER NOT NULL DEFAULT 0,"
                  " cost_estimated INTEGER NOT NULL DEFAULT 1, attempts INTEGER NOT NULL DEFAULT 0,"
                  " error_code TEXT NOT NULL DEFAULT '', client_ip TEXT NOT NULL DEFAULT '',"
                  " captured INTEGER NOT NULL DEFAULT 0, severity INTEGER NOT NULL DEFAULT 0)",
                  nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    StatsDb db(path);
    ASSERT_TRUE(db.ok()) << db.lastError();
    auto r = rec("evt", "t", 200, 1000, 100, 20, std::chrono::system_clock::now());
    r.event_detail = R"({"from":"fast","to":"slow"})";
    EXPECT_TRUE(db.record(r));
    const auto rows = db.requests("", 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["event_detail"].asString(), r.event_detail);
}

TEST(StatsDb, EstimateStaysOutOfTotalsAndDaily) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(db.record(rec("fact", "t", 200, 1000, 100, 10, now)));
    auto estimate = rec("estimate", "t", 200, 1000, 0, 0, now);
    estimate.cost_actual_nano = 0;
    estimate.cost_estimate_nano = 40'000'000;
    estimate.cost_estimated = true;
    ASSERT_TRUE(db.record(estimate));

    const auto totals = db.totals();
    EXPECT_EQ(totals["cost_nano_total"].asInt64(), 1000);
    EXPECT_EQ(totals["cost_estimated_nano"].asInt64(), 40'000'000);
    EXPECT_EQ(totals["estimated_requests"].asInt64(), 1);

    const auto daily = db.daily(30);
    ASSERT_EQ(daily.size(), 1u);
    EXPECT_EQ(daily[0]["requests"].asInt64(), 2);
    EXPECT_EQ(daily[0]["cost_nano"].asInt64(), 1000);
    EXPECT_EQ(daily[0]["cost_estimated_nano"].asInt64(), 40'000'000);
    EXPECT_EQ(daily[0]["estimated_requests"].asInt64(), 1);

    const auto rows = db.requests("", 10);
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& row : rows) {
        if (!row["cost_estimated"].asBool()) continue;
        EXPECT_EQ(row["cost_nano"].asInt64(), 0);
        EXPECT_EQ(row["cost_rub"].asString(), formatRub(0));
        EXPECT_EQ(row["cost_estimated_nano"].asInt64(), 40'000'000);
    }

    // Удаление журнала вычитает и оценку: суточные агрегаты сходятся с фактом.
    EXPECT_EQ(db.deleteRequests(), 2);
    const auto after = db.totals();
    EXPECT_EQ(after["cost_nano_total"].asInt64(), 0);
    EXPECT_EQ(after["cost_estimated_nano"].asInt64(), 0);
    EXPECT_EQ(after["estimated_requests"].asInt64(), 0);
}

TEST(StatsDb, ReopenKeepsAggregatesOlderThanJournal) {
    // Суточные агрегаты живут дольше журнала: открытие базы не пересобирает их
    // из requests и не теряет дни, которых в журнале уже нет.
    test::TempDir dir;
    const auto path = (dir.path() / "stats.db").string();
    const auto now = std::chrono::system_clock::now();
    const auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const std::string old_day = utcDayString(now - std::chrono::hours(24 * 40));
    const std::string today = utcDayString(now);
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(
                      raw,
                      "CREATE TABLE requests(request_id TEXT PRIMARY KEY, ts INTEGER NOT NULL,"
                      " token_id TEXT NOT NULL DEFAULT '', session_id TEXT NOT NULL DEFAULT '',"
                      " model TEXT NOT NULL DEFAULT '', router_id TEXT NOT NULL DEFAULT '',"
                      " provider_tag TEXT NOT NULL DEFAULT '', route_reason TEXT NOT NULL DEFAULT '',"
                      " status INTEGER NOT NULL DEFAULT 0, stream INTEGER NOT NULL DEFAULT 0,"
                      " latency_ms INTEGER NOT NULL DEFAULT 0, ttfb_ms INTEGER NOT NULL DEFAULT 0,"
                      " bytes_in INTEGER NOT NULL DEFAULT 0, bytes_out INTEGER NOT NULL DEFAULT 0,"
                      " tokens_in INTEGER NOT NULL DEFAULT 0, tokens_out INTEGER NOT NULL DEFAULT 0,"
                      " cache_read_tokens INTEGER NOT NULL DEFAULT 0,"
                      " cache_write_tokens INTEGER NOT NULL DEFAULT 0,"
                      " cache_size_tokens INTEGER NOT NULL DEFAULT 0,"
                      " cost_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_input_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_cache_read_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_output_nano INTEGER NOT NULL DEFAULT 0,"
                      " cache_saved_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_estimated INTEGER NOT NULL DEFAULT 1,"
                      " attempts INTEGER NOT NULL DEFAULT 0,"
                      " error_code TEXT NOT NULL DEFAULT '', client_ip TEXT NOT NULL DEFAULT '',"
                      " captured INTEGER NOT NULL DEFAULT 0, severity INTEGER NOT NULL DEFAULT 0,"
                      " event_detail TEXT NOT NULL DEFAULT '')",
                      nullptr, nullptr, nullptr),
                  SQLITE_OK);
        // Схема прежней версии: в daily нет колонки warnings — она добавляется
        // идемпотентно, без пересборки таблицы.
        ASSERT_EQ(sqlite3_exec(
                      raw,
                      "CREATE TABLE daily(day TEXT NOT NULL, token_id TEXT NOT NULL,"
                      " requests INTEGER NOT NULL DEFAULT 0, errors INTEGER NOT NULL DEFAULT 0,"
                      " tokens_in INTEGER NOT NULL DEFAULT 0, tokens_out INTEGER NOT NULL DEFAULT 0,"
                      " cache_read_tokens INTEGER NOT NULL DEFAULT 0,"
                      " cache_write_tokens INTEGER NOT NULL DEFAULT 0,"
                      " cost_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_input_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_cache_read_nano INTEGER NOT NULL DEFAULT 0,"
                      " cost_output_nano INTEGER NOT NULL DEFAULT 0,"
                      " cache_saved_nano INTEGER NOT NULL DEFAULT 0,"
                      " PRIMARY KEY(day, token_id))",
                      nullptr, nullptr, nullptr),
                  SQLITE_OK);
        // День сорокадневной давности: в журнале его уже нет, в агрегатах есть.
        std::ostringstream old_daily;
        old_daily << "INSERT INTO daily(day,token_id,requests,cost_nano)"
                  << " VALUES('" << old_day << "','t',5,777)";
        ASSERT_EQ(sqlite3_exec(raw, old_daily.str().c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        std::ostringstream insert;
        insert << "INSERT INTO requests(request_id,ts,token_id,status,cost_nano,cost_estimated,"
                  "severity)"
               << " VALUES('legacy'," << ts << ",'t',500,1000,1,2)";
        ASSERT_EQ(sqlite3_exec(raw, insert.str().c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);
    }

    StatsDb db(path);
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto daily = db.daily(90);
    bool kept_old_day = false;
    for (const auto& row : daily) {
        if (row["day"].asString() == old_day) {
            kept_old_day = true;
            EXPECT_EQ(row["requests"].asInt64(), 5);
            EXPECT_EQ(row["cost_nano"].asInt64(), 777);
        }
    }
    EXPECT_TRUE(kept_old_day) << "агрегаты старше журнала не должны исчезать";
    // Колонка добавлена идемпотентно: повторное открытие ничего не меняет.
    {
        StatsDb again(path);
        ASSERT_TRUE(again.ok()) << again.lastError();
        bool still_there = false;
        for (const auto& row : again.daily(90)) {
            if (row["day"].asString() == old_day) still_there = true;
        }
        EXPECT_TRUE(still_there);
    }
    (void)today;
}

TEST(StatsDb, AttachSeedsProfileWithoutRequestsWithoutUsage) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();
    ASSERT_TRUE(db.record(rec("u1", "t", 200, 1000, 1000, 100, now)));
    auto estimate = rec("u2", "t", 200, 1000, 0, 0, now);
    estimate.cost_actual_nano = 0;
    estimate.cost_estimate_nano = 5000;
    estimate.cost_estimated = true;
    estimate.latency_ms = 30;
    ASSERT_TRUE(db.record(estimate));

    Stats stats;
    stats.attach(&db);
    const auto profile = stats.observedProfile("mock/model-a");
    EXPECT_EQ(profile.observations, 1);
    EXPECT_EQ(profile.input_tokens, 1000);
    EXPECT_EQ(profile.output_tokens, 100);

    const auto usage = db.usageByModelProvider("", true);
    ASSERT_EQ(usage.size(), 1u);
    EXPECT_EQ(usage[0]["token_observations"].asInt64(), 1);
    EXPECT_EQ(usage[0]["latency_observations"].asInt64(), 2);
    EXPECT_EQ(usage[0]["requests"].asInt64(), 2);
}

TEST(StatsDb, CacheResetIsConsistentBetweenWindowAndJournal) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    Stats stats(10);
    stats.attach(&db);
    const auto now = std::chrono::system_clock::now();
    RequestRecord a;
    a.request_id = "a";
    a.session_id = "s";
    a.model = "m";
    a.provider_tag = "p";
    a.status = 200;
    a.tokens_in = 1000;
    a.tokens_out = 10;
    a.cache_read_tokens = 500;
    a.cost_actual_nano = 1;
    a.cost_estimated = false;
    a.latency_ms = 10;
    a.ts = now - std::chrono::seconds(10);
    stats.record(a);
    EXPECT_EQ(db.cacheResetTs("session", "s"), 0);
    stats.resetCache("session", "s");
    RequestRecord b = a;
    b.request_id = "b";
    b.ts = now + std::chrono::seconds(5);
    stats.record(b);

    // Окно (последние 30) и журнал (средняя) дают одно и то же: попадания до
    // сброса не учитываются, вход/выход считаются по всем запросам.
    const auto window = stats.observedProfile("m", 30);
    const auto journal = stats.observedProfile("m", 0);
    EXPECT_EQ(window.cached_tokens, 500);
    EXPECT_EQ(journal.cached_tokens, 500);
    EXPECT_EQ(window.input_tokens, 1000);
    EXPECT_EQ(journal.input_tokens, window.input_tokens);
    EXPECT_EQ(journal.output_tokens, window.output_tokens);
    EXPECT_EQ(journal.observations, 2);

    // После перезапуска профиль тот же: журнал — единственный источник.
    Stats restarted(10);
    restarted.attach(&db);
    const auto after_restart = restarted.observedProfile("m", 0);
    EXPECT_EQ(after_restart.cached_tokens, journal.cached_tokens);
    EXPECT_EQ(after_restart.input_tokens, journal.input_tokens);
    EXPECT_EQ(after_restart.output_tokens, journal.output_tokens);
}

// Доля кеша — одна величина для итогов и для Prometheus: сброс кеша исключает
// прежние попадания, поэтому totals() не может показывать суточную (несбросную)
// долю. На прежнем коде totals() делил суммы daily и расходился с cacheHitRatio().
TEST(StatsDb, CacheShareAfterResetMatchesRatioQuery) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    const auto now = std::chrono::system_clock::now();

    auto before = rec("before", "t1", 200, 10, 100, 10, now - std::chrono::seconds(10));
    before.cache_read_tokens = 0;
    ASSERT_TRUE(db.record(before));
    ASSERT_TRUE(db.setCacheReset("provider", "fast"));

    auto after = rec("after", "t1", 200, 10, 100, 10, now + std::chrono::seconds(5));
    after.cache_read_tokens = 100;
    ASSERT_TRUE(db.record(after));

    // Итоги остаются суммой за всё время, а доля кеша считается по запросам после
    // сброса — ровно так же, как её видит Prometheus.
    const auto totals = db.totals();
    EXPECT_EQ(totals["tokens_in_total"].asInt64(), 200);
    EXPECT_EQ(totals["cache_read_tokens_total"].asInt64(), 100);
    EXPECT_DOUBLE_EQ(totals["cache_hit_ratio"].asDouble(), 1.0);
    EXPECT_DOUBLE_EQ(totals["cache_hit_ratio"].asDouble(), db.cacheHitRatio());
}
