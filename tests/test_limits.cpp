#include "TestHelpers.h"
#include "limits/ConcurrencyLimiter.h"
#include "limits/QuotaManager.h"
#include "stats/StatsDb.h"

#include <gtest/gtest.h>

using namespace proxy;

namespace {
QuotaLimits limits(int rpm = 0, int tpm = 0, int concurrent = 0, NanoRub budget = 0) {
    QuotaLimits l;
    l.rpm = rpm;
    l.tpm = tpm;
    l.max_concurrent = concurrent;
    l.day_budget_nano = budget;
    return l;
}
} // namespace

TEST(Quota, UnlimitedAndRpmWindow) {
    QuotaManager q;
    auto now = std::chrono::system_clock::now();
    auto d = q.acquire("t", limits(), 100, 500, now);
    EXPECT_TRUE(d.allowed);

    auto l = limits(2);
    auto a = q.acquire("rpm", l, 0, 0, now);
    ASSERT_TRUE(a.allowed);
    auto b = q.acquire("rpm", l, 0, 0, now);
    ASSERT_TRUE(b.allowed);
    auto c = q.acquire("rpm", l, 0, 0, now);
    EXPECT_FALSE(c.allowed);
    EXPECT_EQ(c.status, 429);
    EXPECT_EQ(c.code, "token_rpm_exceeded");
    EXPECT_GT(c.retry_after_sec, 0);
    q.settle(a.ticket, 0, 0);
    q.settle(b.ticket, 0, 0);
    EXPECT_EQ(q.concurrent("rpm"), 0);
    auto later = q.acquire("rpm", l, 0, 0, now + std::chrono::seconds(61));
    EXPECT_TRUE(later.allowed);
}

TEST(Quota, TpmAndSettlement) {
    QuotaManager q;
    auto now = std::chrono::system_clock::now();
    auto l = limits(0, 100);
    auto a = q.acquire("tpm", l, 60, 0, now);
    ASSERT_TRUE(a.allowed);
    q.settle(a.ticket, 60, 0);
    auto b = q.acquire("tpm", l, 50, 0, now);
    EXPECT_FALSE(b.allowed);
    EXPECT_EQ(b.code, "token_tpm_exceeded");
    auto c = q.acquire("tpm", l, 40, 0, now + std::chrono::seconds(61));
    EXPECT_TRUE(c.allowed);
    // Actual usage larger than the reservation is reflected in the window.
    q.settle(c.ticket, 95, 0);
    auto d = q.acquire("tpm", l, 10, 0, now + std::chrono::seconds(62));
    EXPECT_FALSE(d.allowed);
}

TEST(Quota, ConcurrencyAndRelease) {
    QuotaManager q;
    auto now = std::chrono::system_clock::now();
    auto l = limits(0, 0, 1);
    auto a = q.acquire("c", l, 0, 0, now);
    ASSERT_TRUE(a.allowed);
    EXPECT_EQ(q.concurrent("c"), 1);
    auto b = q.acquire("c", l, 0, 0, now);
    EXPECT_FALSE(b.allowed);
    EXPECT_EQ(b.code, "token_concurrency_exceeded");
    q.settle(a.ticket, 0, 0);
    EXPECT_EQ(q.concurrent("c"), 0);
    auto c = q.acquire("c", l, 0, 0, now);
    EXPECT_TRUE(c.allowed);
    q.release(c.ticket);
    EXPECT_EQ(q.concurrent("c"), 0);
    EXPECT_TRUE(q.acquire("c", l, 0, 0, now).allowed);
}

TEST(Quota, DailyBudgetReservationAndRefund) {
    QuotaManager q;
    auto now = std::chrono::system_clock::now();
    auto l = limits(0, 0, 0, 1000);
    auto a = q.acquire("b", l, 0, 600, now);
    ASSERT_TRUE(a.allowed);
    auto b = q.acquire("b", l, 0, 500, now);
    EXPECT_FALSE(b.allowed);
    EXPECT_EQ(b.code, "budget_exceeded");
    EXPECT_GT(b.retry_after_sec, 0);
    q.settle(a.ticket, 0, 300);
    EXPECT_EQ(q.spentToday("b", now), 300);
    auto c = q.acquire("b", l, 0, 500, now);
    EXPECT_TRUE(c.allowed);
    q.settle(c.ticket, 0, 500);
    EXPECT_EQ(q.spentToday("b", now), 800);
    auto d = q.acquire("b", l, 0, 300, now);
    EXPECT_FALSE(d.allowed);
    auto tomorrow = q.acquire("b", l, 0, 900, now + std::chrono::hours(24));
    EXPECT_TRUE(tomorrow.allowed);
}

TEST(Quota, ReleaseRefundsBudget) {
    QuotaManager q;
    auto now = std::chrono::system_clock::now();
    auto l = limits(0, 0, 1, 1000);
    auto a = q.acquire("x", l, 10, 700, now);
    ASSERT_TRUE(a.allowed);
    EXPECT_EQ(q.spentToday("x", now), 700);
    q.release(a.ticket);
    EXPECT_EQ(q.spentToday("x", now), 0);
    EXPECT_EQ(q.concurrent("x"), 0);
}

TEST(Quota, SeedsBudgetFromPersistedStats) {
    test::TempDir dir;
    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok());
    RequestRecord r;
    r.request_id = "old";
    r.token_id = "seed";
    r.status = 200;
    r.cost_actual_nano = 700;
    r.cost_estimated = false;
    r.ts = std::chrono::system_clock::now();
    ASSERT_TRUE(db.record(r));

    QuotaManager q;
    q.attach(&db);
    auto now = std::chrono::system_clock::now();
    auto d = q.acquire("seed", limits(0, 0, 0, 1000), 0, 400, now);
    EXPECT_FALSE(d.allowed);
    EXPECT_EQ(d.code, "budget_exceeded");
    EXPECT_EQ(q.spentToday("seed", now), 700);
}
TEST(Quota, UpstreamConcurrencyLimiter) {
    ConcurrencyLimiter limiter;
    EXPECT_TRUE(limiter.tryAcquire("r", 1));
    EXPECT_FALSE(limiter.tryAcquire("r", 1));
    EXPECT_TRUE(limiter.tryAcquire("other", 1));
    limiter.release("r");
    EXPECT_TRUE(limiter.tryAcquire("r", 1));
    EXPECT_TRUE(limiter.tryAcquire("unlimited", 0));
}
