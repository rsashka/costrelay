#include "TestHelpers.h"
#include "analytics/ProviderAnalytics.h"

#include <gtest/gtest.h>
#include <filesystem>

using namespace proxy;

namespace {
const char* kCsv =
    "id;created_at (Europe/Moscow);source;api_type;model_name;provider_name;input_tokens;"
    "output_tokens;input_tokens_cost;output_tokens_cost;input_cached_tokens;"
    "input_cached_tokens_cost;amount;latency_sec\n"
    "r1;14-11-2026 13:11:32;api;chat/completions;mock/model;ProviderA;100;20;0.001;0.002;40;"
    "0.0001;0.0031;2.5\n"
    "r2;14-11-2026 14:00:00;api;chat/completions;mock/model;ProviderA;200;30;0.002;0.003;80;"
    "0.0002;0.0052;3.5\n"
    "r3;14-11-2026 14:10:00;api;chat/completions;mock/model;providerb;50;10;0.001;0.001;0;"
    "0;0.002;1.0\n";
AnalyticsConfig cfg(const std::string& dir = {}) {
    AnalyticsConfig c;
    c.enabled = true;
    c.provider_log_dir = dir;
    c.auto_import = false;
    return c;
}
} // namespace

TEST(ProviderAnalytics, ImportsCsvAndAggregates) {
    ProviderAnalytics a(cfg());
    auto r = a.importCsvContent(kCsv, "test.csv");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.rows, 3u);
    EXPECT_FALSE(r.duplicate);
    auto summary = a.summaryJson();
    EXPECT_EQ(summary["rows"].asInt64(), 3);
    EXPECT_EQ(summary["models"].asInt64(), 1);
    EXPECT_EQ(summary["providers"].asInt64(), 2);
    EXPECT_EQ(summary["requests"].asInt64(), 3);
    EXPECT_EQ(summary["input_tokens"].asInt64(), 350);
    EXPECT_EQ(summary["cached_tokens"].asInt64(), 120);
    EXPECT_DOUBLE_EQ(summary["cache_hit_ratio"].asDouble(), 120.0 / 350.0);
    EXPECT_EQ(summary["cost_nano"].asInt64(), 10300000);
    EXPECT_NEAR(summary["avg_latency_ms"].asDouble(), 7000.0 / 3.0, 0.01);

    auto times = a.timesJson(10);
    ASSERT_EQ(times.size(), 3u); // two hours for A, one for B
    auto providers = a.providersJson(10);
    ASSERT_EQ(providers.size(), 2u);
    EXPECT_EQ(providers[0]["provider"].asString(), "ProviderA");
    EXPECT_EQ(providers[0]["p50_latency_ms"].asDouble(), 3000.0);
    EXPECT_GT(providers[0]["tokens_per_second"].asDouble(), 0.0);
    auto models = a.modelsJson(10);
    ASSERT_EQ(models.size(), 1u);
    EXPECT_EQ(models[0]["model"].asString(), "mock/model");
    EXPECT_EQ(models[0]["providers"].asInt64(), 2);
    EXPECT_GT(models[0]["cost_per_million_tokens_rub"].asDouble(), 0.0);

    auto duplicate = a.importCsvContent(kCsv, "same.csv");
    EXPECT_TRUE(duplicate.ok);
    EXPECT_TRUE(duplicate.duplicate);
    EXPECT_EQ(duplicate.rows, 0u);
}

TEST(ProviderAnalytics, RejectsUnknownSchemaAndCountsSkippedRows) {
    ProviderAnalytics a(cfg());
    auto bad = a.importCsvContent("model_name;x\nm;1\n", "bad.csv");
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.error.empty());

    const std::string csv =
        "created_at (Europe/Moscow);model_name;provider_name;input_tokens\n"
        ";m/x;A;1\n"
        "bad-time;m/x;A;1\n"
        "14-11-2026 13:00:00;m/x;A;1\n";
    auto r = a.importCsvContent(csv, "rows.csv");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.rows, 1u);
    EXPECT_EQ(r.skipped, 2u);
}

// Нечисловая ячейка в числовой колонке видна счётчиком, а не молчаливым нулём:
// по этим числам считаются веса очереди.
TEST(ProviderAnalytics, CountsNonNumericFields) {
    ProviderAnalytics a(cfg());
    const std::string csv =
        "created_at (Europe/Moscow);model_name;provider_name;input_tokens;output_tokens;amount\n"
        "14-11-2026 13:00:00;m/x;A;n/a;10;0.001\n";
    auto r = a.importCsvContent(csv, "bad-fields.csv");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.bad_fields, 1u);
    EXPECT_EQ(r.rows, 1u);
}

// Импорт каталога: несколько CSV агрегируются, не-CSV файлы игнорируются.
// Проверка не зависит от боевого data/: это свойство импорта, а не пример.
TEST(ProviderAnalytics, ImportsDirectoryOfCsvFiles) {
    test::TempDir dir;
    const std::string header =
        "created_at (Europe/Moscow);model_name;provider_name;input_tokens;output_tokens;amount\n";
    test::writeFile(dir.path() / "b.csv", header + "14-11-2026 13:00:00;m/x;A;100;10;0.001\n");
    test::writeFile(dir.path() / "a.csv", header + "14-11-2026 13:05:00;m/x;B;200;20;0.002\n");
    test::writeFile(dir.path() / "notes.txt", "ignored");

    ProviderAnalytics a(cfg(dir.path().string()));
    auto r = a.importDirectory();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.files, 2u);
    EXPECT_EQ(r.rows, 2u);
    const auto summary = a.summaryJson();
    EXPECT_EQ(summary["requests"].asInt64(), 2);
    EXPECT_EQ(summary["providers"].asInt64(), 2);
    EXPECT_EQ(summary["input_tokens"].asInt64(), 300);
    EXPECT_GT(summary["cost_nano"].asInt64(), 0);
}

TEST(ProviderAnalytics, PersistsOnlyAggregatesAndHashes) {
    test::TempDir dir;
    auto c = cfg();
    c.snapshot_file = (dir.path() / "provider-analytics.json").string();
    ProviderAnalytics a(c);
    ASSERT_TRUE(a.importCsvContent(kCsv, "test.csv").ok);
    ASSERT_TRUE(a.saveSnapshot());
    ASSERT_TRUE(std::filesystem::exists(c.snapshot_file));

    ProviderAnalytics loaded(c);
    ASSERT_TRUE(loaded.loadSnapshot());
    auto summary = loaded.summaryJson();
    EXPECT_EQ(summary["rows"].asInt64(), 3);
    const auto providers = loaded.providersJson(10);
    ASSERT_EQ(providers.size(), 2u);
    bool found = false;
    for (const auto& p : providers) {
        if (p["provider"].asString() != "ProviderA") continue;
        found = true;
        EXPECT_NEAR(std::stod(p["input_per_million_rub"].asString()), 16.667, 0.001);
        EXPECT_NEAR(std::stod(p["cache_read_per_million_rub"].asString()), 2.5, 0.001);
        EXPECT_NEAR(std::stod(p["output_per_million_rub"].asString()), 100.0, 0.001);
    }
    EXPECT_TRUE(found);
    auto again = loaded.importCsvContent(kCsv, "test.csv");
    EXPECT_TRUE(again.ok);
    EXPECT_TRUE(again.duplicate);
    EXPECT_EQ(again.rows, 0u);
}
