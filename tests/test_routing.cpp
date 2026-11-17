#include "TestHelpers.h"
#include "routing/ProviderHealth.h"
#include "routing/ProviderQueue.h"
#include "routing/Router.h"
#include "util/Strings.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

#include <gtest/gtest.h>

#include <limits>

using namespace proxy;

namespace {

AppConfig makeConfig(double multiplier_b = 1.0) {
    AppConfig cfg;
    cfg.routing.models = {"mock/model-a"};
    cfg.routing.default_model = "mock/model-a";
    cfg.routing.aliases["default"] = "mock/model-a";
    cfg.routing.fallback_attempts = 1;
    cfg.routing.candidate_cap = 16;
    // Тесты проверяют порядок по ценам: гистерезис выключен и включается точечно.
    cfg.routing.provider_order_size = 1;
    cfg.routing.leader.margin_percent = 0.0;
    cfg.routing.leader.min_dwell_sec = 0;
    cfg.routing.leader.confirm_builds = 1;
    cfg.pricing.threshold_mode = "replace";
    cfg.routers.push_back(test::router("router-a", "https://a.example/v1", 1.0));
    cfg.routers.push_back(test::router("router-b", "https://b.example/v1", multiplier_b));
    return cfg;
}

void addCatalog(PriceCatalog& cat) {
    auto fast = test::offer("fast", 0.0001, 0.0003);
    auto slow = test::offer("slow", 0.0002, 0.0004);
    auto disabled = test::offer("disabled", 0.00001, 0.00001, -1);
    cat.replace(test::snapshot("mock/model-a", {fast, slow, disabled}));
}

// Наблюдаемый профиль модели — единственный вход ранжирования.
void observe(Stats& stats, const std::string& model, std::int64_t in, std::int64_t out,
             std::int64_t cached = 0, int requests = 1) {
    for (int i = 0; i < requests; ++i) {
        RequestRecord r;
        r.model = model;
        r.tokens_in = in;
        r.tokens_out = out;
        r.cache_read_tokens = cached;
        // Наблюдение всегда с меткой времени: окно профиля считает попадания
        // строго после сброса кеша, как SQL-путь по журналу.
        r.ts = std::chrono::system_clock::now();
        stats.record(r);
    }
}

struct Harness {
    AppConfig cfg;
    PriceCatalog cat;
    Router router;
    Stats stats;
    ProviderQueue queue;

    explicit Harness(AppConfig c = makeConfig())
        : cfg(std::move(c)), router(cfg, cat), stats(64), queue(cfg, router, cat, stats) {}

    RoutePlan plan(const std::string& model = "mock/model-a") {
        queue.build();
        return queue.planFor(cfg.resolveModel(model));
    }
};

} // namespace

TEST(Routing, ChoosesCheapestProviderOnBestRouter) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan("default");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.model, "mock/model-a");
    EXPECT_EQ(plan.candidates.front().provider_tag, "fast");
    EXPECT_EQ(plan.candidates.front().router_id, "router-a");
    EXPECT_GT(plan.candidates.front().cost_nano, 0);
    EXPECT_EQ(plan.provider_order.front(), "fast");
    EXPECT_EQ(plan.reason, "cost");
}

TEST(Routing, AppliesRouterPriceMultiplier) {
    Harness h(makeConfig(0.5));
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().router_id, "router-b");
    EXPECT_EQ(plan.candidates.front().provider_tag, "fast");
}

TEST(Routing, ExcludesDeprioritizedStatus) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    for (const auto& c : plan.candidates) EXPECT_NE(c.provider_tag, "disabled");
    bool rejected = false;
    for (const auto& c : plan.rejected) {
        if (c.provider_tag == "disabled") {
            rejected = true;
            EXPECT_EQ(c.excluded, "provider_deprioritized");
        }
    }
    EXPECT_TRUE(rejected);
}

TEST(Routing, AppliesProviderWhitelist) {
    auto cfg = makeConfig();
    cfg.routing.provider_whitelist.push_back({"slow", {"*"}, "allowed"});
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    for (const auto& c : plan.candidates) EXPECT_EQ(c.provider_tag, "slow");
    bool fast_rejected = false;
    for (const auto& c : plan.rejected) {
        if (c.provider_tag == "fast" && c.excluded == "provider_not_whitelisted") {
            fast_rejected = true;
        }
    }
    EXPECT_TRUE(fast_rejected);
}

TEST(Routing, WhitelistCanRejectAll) {
    auto cfg = makeConfig();
    cfg.routing.provider_whitelist.push_back({"missing", {"*"}, "allowed"});
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    const auto plan = h.plan();
    EXPECT_FALSE(plan.hasRoute());
    EXPECT_EQ(plan.reason, "no_route");
}

TEST(Routing, ContextLimitUsesModelProfile) {
    Harness h;
    addCatalog(h.cat);
    // Профиль модели больше контекста всех офферов (128000).
    observe(h.stats, "mock/model-a", 200'000, 100);
    const auto plan = h.plan();
    EXPECT_FALSE(plan.hasRoute());
    EXPECT_EQ(plan.error, "no_route");
    for (const auto& c : plan.rejected) {
        if (c.provider_tag == "disabled") continue; // отдельный фильтр статуса
        EXPECT_EQ(c.excluded, "context_too_small");
    }
}

TEST(Routing, RankingUsesCostPerMillionForKey) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    const auto& quotes = h.queue.snapshot()->find("mock/model-a")->quotes;
    for (std::size_t i = 1; i < quotes.size(); ++i) {
        if (!quotes[i].excluded.empty() || !quotes[i - 1].excluded.empty()) continue;
        EXPECT_LE(quotes[i - 1].cost_per_million_nano, quotes[i].cost_per_million_nano);
    }
}

// Снимок публикует составляющие стоимости: панель и API читают их, а не
// пересчитывают формулу (иначе два представления одной цены расходятся).
TEST(Routing, SnapshotPublishesCostComponents) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    for (const auto& c : plan.candidates) {
        EXPECT_EQ(c.cost_nano, c.input_cost_nano + c.cache_cost_nano + c.output_cost_nano +
                                   c.fixed_fee_nano)
            << c.provider_tag;
    }
}

TEST(Routing, TableOrderMatchesQueueOrder) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    const auto* entry = h.queue.snapshot()->find("mock/model-a");
    ASSERT_NE(entry, nullptr);
    std::vector<std::string> eligible;
    for (const auto& q : entry->quotes) {
        if (q.excluded.empty() &&
            std::find(eligible.begin(), eligible.end(), q.provider_tag) == eligible.end()) {
            eligible.push_back(q.provider_tag);
        }
    }
    // Таблица панели и приоритет в плане — это один и тот же список.
    EXPECT_EQ(plan.provider_priority, eligible);
    EXPECT_GE(eligible.size(), 2u);
}

TEST(Routing, InjectionSendsProviderOrderTopN) {
    auto cfg = makeConfig();
    cfg.routing.provider_order_size = 2;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    ASSERT_EQ(plan.provider_order.size(), 2u);
    EXPECT_EQ(plan.provider_order[0], "fast");
    EXPECT_EQ(plan.provider_order[1], "slow");

    Json::Value body(Json::objectValue);
    body["model"] = "mock/model-a";
    body["messages"] = Json::Value(Json::arrayValue);
    std::string err;
    body = buildUpstreamBody(body, plan, plan.candidates.front(), h.cfg, err);
    ASSERT_TRUE(err.empty());
    ASSERT_TRUE(body["provider"].isObject());
    ASSERT_TRUE(body["provider"]["order"].isArray());
    ASSERT_EQ(body["provider"]["order"].size(), 2u);
    EXPECT_EQ(body["provider"]["order"][0].asString(), "fast");
    EXPECT_EQ(body["provider"]["order"][1].asString(), "slow");
    EXPECT_FALSE(body["provider"]["allow_fallbacks"].asBool());
    EXPECT_FALSE(body["provider"].isMember("only"));

    // Панельный переключатель убирает объект provider, но не переименование модели.
    h.cfg.routing.inject_provider_options = false;
    Json::Value body2(Json::objectValue);
    body2["model"] = "mock/model-a";
    body2["provider"] = Json::Value(Json::objectValue);
    body2 = buildUpstreamBody(body2, plan, plan.candidates.front(), h.cfg, err);
    ASSERT_TRUE(err.empty());
    EXPECT_EQ(body2["model"].asString(), "mock/model-a");
    EXPECT_FALSE(body2.isMember("provider"));
}

TEST(Routing, CandidateCapLimitsPlanCandidates) {
    auto cfg = makeConfig();
    cfg.routing.candidate_cap = 1;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.size(), 1u);
    EXPECT_EQ(plan.provider_order.size(), 1u);
    EXPECT_GE(plan.provider_priority.size(), 2u); // очередь в панели остаётся полной

    // Предел кандидатов не режет бюджет попыток сам по себе: он задаётся
    // fallback_attempts, а candidate_cap лишь ограничивает список.
    cfg.routing.candidate_cap = 16;
    Harness h2(std::move(cfg));
    addCatalog(h2.cat);
    const auto full = h2.plan();
    EXPECT_EQ(full.candidates.size(), full.provider_priority.size());
}

TEST(Routing, ProviderOrderIsSingleByDefault) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    // Внешний обработчик получает одного провайдера: выбрать другого он не может.
    ASSERT_EQ(plan.provider_order.size(), 1u);
    EXPECT_EQ(plan.provider_order.front(), plan.candidates.front().provider_tag);
}

// Цены меняются так, что challenger дешевле лидера на заданный процент.
void priceChallenger(Harness& h, double slow_prompt, double slow_completion) {
    auto fast = test::offer("fast", 0.0002, 0.0006);
    auto slow = test::offer("slow", slow_prompt, slow_completion);
    auto disabled = test::offer("disabled", 0.00001, 0.00001, -1);
    h.cat.replace(test::snapshot("mock/model-a", {fast, slow, disabled}));
}

TEST(Routing, LeaderHoldsBelowMarginAndSwitchesAfterConfirmation) {
    auto cfg = makeConfig();
    cfg.routing.leader.margin_percent = 15.0;
    cfg.routing.leader.confirm_builds = 2;
    cfg.routing.leader.min_dwell_sec = 0;
    Harness h(std::move(cfg));
    priceChallenger(h, 0.0002, 0.0006);
    ASSERT_EQ(h.plan().candidates.front().provider_tag, "fast");

    // Дешевле на 10 %: ниже маржи — лидер держится.
    priceChallenger(h, 0.00018, 0.00054);
    EXPECT_EQ(h.plan().candidates.front().provider_tag, "fast");
    EXPECT_EQ(h.plan().candidates.front().provider_tag, "fast");

    // Дешевле на 50 %: challenger должен выиграть два перестроения подряд.
    priceChallenger(h, 0.0001, 0.0003);
    EXPECT_EQ(h.plan().candidates.front().provider_tag, "fast");
    const auto switched = h.plan();
    EXPECT_EQ(switched.candidates.front().provider_tag, "slow");
    EXPECT_EQ(switched.provider_order.front(), "slow");
}

// Срок удержания лидера решающий: даже при выигрыше и подтверждениях смена ждёт
// истечения min_dwell_sec.
TEST(Routing, LeaderHoldsUntilMinDwell) {
    auto cfg = makeConfig();
    cfg.routing.leader.margin_percent = 0.0;
    cfg.routing.leader.confirm_builds = 1;
    cfg.routing.leader.min_dwell_sec = 3600;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    ASSERT_EQ(h.plan().candidates.front().provider_tag, "fast");

    // slow дешевле в разы: одного подтверждения достаточно, но срок не прошёл.
    priceChallenger(h, 0.00001, 0.00003);
    EXPECT_EQ(h.plan().candidates.front().provider_tag, "fast");
    EXPECT_EQ(h.plan().candidates.front().provider_tag, "fast");
}

TEST(Routing, LeaderForcedSwitchWhenProviderExcluded) {
    auto cfg = makeConfig();
    cfg.routing.leader.margin_percent = 50.0;
    cfg.routing.leader.confirm_builds = 5;
    cfg.routing.leader.min_dwell_sec = 3600;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    ASSERT_EQ(h.plan().candidates.front().provider_tag, "fast");

    // Исключение лидера обходит гистерезис: удержание невозможно.
    h.cfg.routing.provider_blacklist.push_back({"fast", {"*"}, "blocked"});
    const auto plan = h.plan();
    EXPECT_EQ(plan.candidates.front().provider_tag, "slow");
}

// Лидер по гистерезису может оказаться дороже challenger'а. Вынос лидера наверх
// не имеет права ломать порядок остальных: дешёвый оффер остаётся следующим по
// весу, а не уезжает в хвост циклическим сдвигом.
TEST(Routing, LeaderMovedToFrontKeepsWeightOrderBehindIt) {
    auto cfg = makeConfig();
    cfg.routing.fallback_attempts = 2;
    cfg.routing.leader.margin_percent = 50.0;
    cfg.routing.leader.confirm_builds = 5;
    cfg.routing.leader.min_dwell_sec = 3600;
    Harness h(std::move(cfg));
    // Сначала лидером становится lead: в этот момент он дешевейший из трёх.
    h.cat.replace(test::snapshot("mock/model-a",
                                 {test::offer("cheap", 0.0004, 0.0008),
                                  test::offer("lead", 0.0001, 0.0002),
                                  test::offer("pricey", 0.0003, 0.0006)}));
    ASSERT_EQ(h.plan().candidates.front().provider_tag, "lead");

    // Цены развернулись: cheap дешевле lead. Лидер держится (маржа и подтверждения).
    h.cat.replace(test::snapshot("mock/model-a",
                                 {test::offer("cheap", 0.0001, 0.0002),
                                  test::offer("lead", 0.0002, 0.0004),
                                  test::offer("pricey", 0.0003, 0.0006)}));
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "lead");
    ASSERT_GE(plan.candidates.size(), 2u);
    EXPECT_EQ(plan.candidates[1].provider_tag, "cheap");
    EXPECT_EQ(plan.provider_priority,
              (std::vector<std::string>{"lead", "cheap", "pricey"}));
    EXPECT_EQ(plan.provider_order.front(), "lead");
}

TEST(Routing, LeaderSurvivesRestartFromStateFile) {
    test::TempDir dir;
    auto cfg = makeConfig();
    cfg.routing.leader.state_file = (dir.path() / "leader.json").string();
    cfg.routing.leader.margin_percent = 50.0;
    cfg.routing.leader.confirm_builds = 1;
    cfg.routing.leader.min_dwell_sec = 0;
    {
        Harness h(cfg);
        addCatalog(h.cat);
        const auto plan = h.plan();
        ASSERT_EQ(plan.candidates.front().provider_tag, "fast");
    }
    // Рестарт: цены слегка развернулись к slow, но лидер прочитан из файла и
    // держится, потому что выигрыш ниже маржи. Без файла лидером стал бы slow.
    Harness h2(cfg);
    auto fast = test::offer("fast", 0.0002, 0.0006);
    auto slow = test::offer("slow", 0.00018, 0.00054);
    h2.cat.replace(test::snapshot("mock/model-a", {fast, slow}));
    h2.queue.start();
    h2.queue.stop();
    const auto plan = h2.queue.planFor("mock/model-a");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "fast");
}

TEST(Routing, LeaderStateIsFreshWithoutFile) {
    // Тот же расклад без файла состояния: лидер инициализируется по ценам.
    test::TempDir dir;
    auto cfg = makeConfig();
    cfg.routing.leader.state_file = (dir.path() / "leader.json").string();
    cfg.routing.leader.margin_percent = 50.0;
    cfg.routing.leader.confirm_builds = 1;
    cfg.routing.leader.min_dwell_sec = 0;
    Harness h(std::move(cfg));
    auto fast = test::offer("fast", 0.0002, 0.0006);
    auto slow = test::offer("slow", 0.00018, 0.00054);
    h.cat.replace(test::snapshot("mock/model-a", {fast, slow}));
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "slow");
}

TEST(Routing, SessionPinHoldsWhileGainBelowMargin) {
    auto cfg = makeConfig();
    cfg.routing.fallback_attempts = 2;
    cfg.routing.session_pin_sec = 600;
    // Порог выше выигрыша: сброс кеша не окупается, привязка держится.
    cfg.routing.leader.margin_percent = 50.0;
    cfg.routing.leader.confirm_builds = 1;
    cfg.routing.leader.min_dwell_sec = 0;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    const auto first = h.queue.planForSession("mock/model-a", "s1");
    ASSERT_TRUE(first.hasRoute());
    EXPECT_EQ(first.candidates.front().provider_tag, "fast");
    EXPECT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "fast");

    // slow дешевле на 10 % — ниже порога: и лидер, и привязка остаются на fast.
    priceChallenger(h, 0.00018, 0.00054);
    h.queue.build();
    EXPECT_EQ(h.queue.planFor("mock/model-a").candidates.front().provider_tag, "fast");
    const auto pinned = h.queue.planForSession("mock/model-a", "s1");
    ASSERT_TRUE(pinned.hasRoute());
    EXPECT_EQ(pinned.candidates.front().provider_tag, "fast");
    EXPECT_EQ(pinned.provider_order.front(), "fast");
    EXPECT_TRUE(pinned.session_rebind_from.empty());
}

// Удержанная привязка сессии тоже только выносится наверх: офферы дешевле неё
// остаются следующими по весу, а не уезжают в конец очереди.
TEST(Routing, HeldSessionPinKeepsWeightOrderBehindIt) {
    auto cfg = makeConfig();
    cfg.routing.fallback_attempts = 2;
    cfg.routing.session_pin_sec = 600;
    cfg.routing.leader.margin_percent = 50.0;
    cfg.routing.leader.confirm_builds = 5;
    cfg.routing.leader.min_dwell_sec = 3600;
    Harness h(std::move(cfg));
    h.cat.replace(test::snapshot("mock/model-a",
                                 {test::offer("cheap", 0.0004, 0.0008),
                                  test::offer("pin", 0.0001, 0.0002),
                                  test::offer("pricey", 0.0003, 0.0006)}));
    h.queue.build();
    ASSERT_EQ(h.queue.planForSession("mock/model-a", "s1").candidates.front().provider_tag, "pin");
    ASSERT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "pin");

    // cheap дешевле pin, но выигрыш ниже маржи: привязка держится.
    h.cat.replace(test::snapshot("mock/model-a",
                                 {test::offer("cheap", 0.00016, 0.00032),
                                  test::offer("pin", 0.0002, 0.0004),
                                  test::offer("pricey", 0.0003, 0.0006)}));
    h.queue.build();
    const auto plan = h.queue.planForSession("mock/model-a", "s1");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "pin");
    EXPECT_TRUE(plan.session_rebind_from.empty());
    ASSERT_GE(plan.candidates.size(), 2u);
    EXPECT_EQ(plan.candidates[1].provider_tag, "cheap");
    EXPECT_EQ(plan.provider_priority,
              (std::vector<std::string>{"pin", "cheap", "pricey"}));
}

TEST(Routing, SessionRebindsImmediatelyAboveMargin) {
    auto cfg = makeConfig();
    cfg.routing.fallback_attempts = 2;
    cfg.routing.session_pin_sec = 600;
    cfg.routing.leader.margin_percent = 15.0;
    // Подтверждения лидера привязку не задерживают: одного перестроения достаточно.
    cfg.routing.leader.confirm_builds = 2;
    cfg.routing.leader.min_dwell_sec = 1800;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    ASSERT_EQ(h.queue.planForSession("mock/model-a", "s1").candidates.front().provider_tag, "fast");
    ASSERT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "fast");

    // slow дешевле вчетверо: сессия уходит на него сразу, лидер ещё ждёт подтверждений.
    priceChallenger(h, 0.00005, 0.00015);
    h.queue.build();
    EXPECT_EQ(h.queue.planFor("mock/model-a").candidates.front().provider_tag, "fast");

    const auto plan = h.queue.planForSession("mock/model-a", "s1");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "slow");
    EXPECT_EQ(plan.provider_order.front(), "slow");
    EXPECT_EQ(plan.session_rebind_from, "fast");
    EXPECT_EQ(plan.session_rebind_reason, kPinDropCostMargin);
    EXPECT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "slow");
}

TEST(Routing, LeaderStateKeepsPreviousProviderAcrossRestart) {
    test::TempDir dir;
    const auto state_file = (dir.path() / "leader.json").string();
    auto cfg = makeConfig();
    cfg.routing.fallback_attempts = 2;
    cfg.routing.leader.confirm_builds = 1;
    cfg.routing.leader.min_dwell_sec = 0;
    cfg.routing.leader.state_file = state_file;
    {
        Harness h(cfg);
        addCatalog(h.cat);
        h.queue.build();
        ASSERT_EQ(h.queue.explain("mock/model-a")["leader"].asString(), "fast");
        priceChallenger(h, 0.00001, 0.00003);
        h.queue.build();
        const auto after = h.queue.explain("mock/model-a");
        ASSERT_EQ(after["leader"].asString(), "slow");
        EXPECT_EQ(after["previous_leader"].asString(), "fast");
    }
    {
        // Перезапуск: лидер и предыдущий провайдер восстанавливаются из файла.
        Harness h(cfg);
        priceChallenger(h, 0.00001, 0.00003);
        h.queue.start();
        h.queue.stop();
        const auto restored = h.queue.explain("mock/model-a");
        EXPECT_EQ(restored["leader"].asString(), "slow");
        EXPECT_EQ(restored["previous_leader"].asString(), "fast");
        EXPECT_GT(restored["leader_since"].asInt64(), 0);
    }
}

TEST(Routing, PriceRefreshRebuildsSnapshotWithoutTick) {
    test::TempDir dir;
    auto cfg = makeConfig();
    cfg.routing.leader.state_file = (dir.path() / "leader.json").string();
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.start();
    h.queue.stop();
    ASSERT_EQ(h.queue.planFor("mock/model-a").candidates.front().provider_tag, "fast");

    // Новый прайс без сигнала загрузчика ждёт случайного тика, сигнал — не ждёт.
    priceChallenger(h, 0.00001, 0.00003);
    EXPECT_EQ(h.queue.planFor("mock/model-a").candidates.front().provider_tag, "fast");
    h.queue.notifyExternalChange();
    EXPECT_EQ(h.queue.planFor("mock/model-a").candidates.front().provider_tag, "slow");
}

TEST(Routing, SessionRebindsWhenPinnedProviderExcluded) {
    auto cfg = makeConfig();
    cfg.routing.session_pin_sec = 600;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    ASSERT_EQ(h.queue.planForSession("mock/model-a", "s1").candidates.front().provider_tag, "fast");

    h.cfg.routing.provider_blacklist.push_back({"fast", {"*"}, "blocked"});
    h.queue.build();
    const auto plan = h.queue.planForSession("mock/model-a", "s1");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "slow");
    EXPECT_EQ(plan.session_rebind_from, "fast");
    EXPECT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "slow");
}

TEST(Routing, SessionPinEvictsOldestByLimit) {
    auto cfg = makeConfig();
    cfg.routing.session_pin_sec = 600;
    cfg.routing.session_pin_max = 1;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    h.queue.planForSession("mock/model-a", "s1");
    h.queue.planForSession("mock/model-a", "s2");
    EXPECT_TRUE(h.queue.sessionPin("s1", "mock/model-a").empty());
    EXPECT_EQ(h.queue.sessionPin("s2", "mock/model-a"), "fast");
}

// TTL бездействия: истёкшую привязку и planForSession, и sessionPin считают
// отсутствующей — одно правило.
TEST(Routing, SessionPinExpiresAfterIdle) {
    auto cfg = makeConfig();
    cfg.routing.session_pin_sec = 1;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    ASSERT_EQ(h.queue.planForSession("mock/model-a", "s1").candidates.front().provider_tag, "fast");
    ASSERT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "fast");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(h.queue.sessionPin("s1", "mock/model-a").empty());
    h.queue.build();
    EXPECT_EQ(h.queue.planForSession("mock/model-a", "s1").candidates.front().provider_tag, "fast");
    EXPECT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "fast");
}

// Вытеснение по пределу — LRU, а не порядок вставки: обращение к s1 сохраняет его.
TEST(Routing, SessionPinEvictsLeastRecentlyUsed) {
    auto cfg = makeConfig();
    cfg.routing.session_pin_sec = 600;
    cfg.routing.session_pin_max = 2;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    h.queue.planForSession("mock/model-a", "s1");
    h.queue.planForSession("mock/model-a", "s2");
    h.queue.planForSession("mock/model-a", "s1"); // s1 свежее s2
    h.queue.planForSession("mock/model-a", "s3");

    EXPECT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "fast");
    EXPECT_TRUE(h.queue.sessionPin("s2", "mock/model-a").empty());
    EXPECT_EQ(h.queue.sessionPin("s3", "mock/model-a"), "fast");
}

TEST(Routing, SessionPinDisabledByZero) {
    auto cfg = makeConfig();
    cfg.routing.session_pin_sec = 0;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    h.queue.planForSession("mock/model-a", "s1");
    EXPECT_TRUE(h.queue.sessionPin("s1", "mock/model-a").empty());
}

TEST(Routing, PlanUsesModelProfileNotRequest) {
    Harness h;
    addCatalog(h.cat);
    observe(h.stats, "mock/model-a", 4000, 800, 3000, 4);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.estimated_in, 4000);
    EXPECT_EQ(plan.estimated_out, 800);
    EXPECT_NEAR(plan.cache_hit_ratio, 0.75, 0.001);
    EXPECT_NEAR(plan.candidates.front().cache_hit_ratio, 0.75, 0.01);
}

TEST(Routing, CostThresholdReplaceAndExcess) {
    auto o = test::offer("p", 0.0001, 0.0003);
    o.threshold_tokens = 1000;
    o.threshold_prompt_per_token = rubToNano(0.0005);
    // 2000 in, 100 out.
    const auto replace = estimateOfferCostBreakdown(o, 2000, 100, "replace").total_nano;
    const auto excess = estimateOfferCostBreakdown(o, 2000, 100, "excess").total_nano;
    EXPECT_EQ(replace, safeMulTokens(2000, rubToNano(0.0005)) + safeMulTokens(100, rubToNano(0.0003)));
    EXPECT_EQ(excess, safeMulTokens(1000, rubToNano(0.0001)) +
                           safeMulTokens(1000, rubToNano(0.0005)) +
                           safeMulTokens(100, rubToNano(0.0003)));
    EXPECT_LT(excess, replace);
}

TEST(Routing, NoPriceSnapshot) {
    Harness h;
    const auto plan = h.plan();
    EXPECT_FALSE(plan.hasRoute());
    EXPECT_EQ(plan.error, "no_price_snapshot");
    EXPECT_EQ(plan.reason, "no_price");
}

TEST(Routing, ReportsRejectedCandidates) {
    auto cfg = makeConfig();
    cfg.routers.resize(1);
    Harness h(std::move(cfg));
    h.cat.replace(test::snapshot("mock/model-a", {test::offer("disabled", 0.00001, 0.00001, -1)}));
    const auto plan = h.plan();
    EXPECT_FALSE(plan.hasRoute());
    ASSERT_FALSE(plan.rejected.empty());
    EXPECT_EQ(plan.rejected.front().excluded, "provider_deprioritized");
    auto ex = h.queue.explain("mock/model-a");
    EXPECT_EQ(ex["rejected"][0]["excluded"].asString(), "provider_deprioritized");
}

TEST(Routing, BuildsCanonicalUpstreamBody) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan("default");
    ASSERT_TRUE(plan.hasRoute());
    Json::Value body(Json::objectValue);
    body["model"] = "default";
    std::string err;
    const auto built = buildUpstreamBody(body, plan, plan.candidates.front(), h.cfg, err);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(built["model"].asString(), "mock/model-a");
    ASSERT_TRUE(built["provider"].isObject());
    EXPECT_EQ(built["provider"]["order"][0].asString(), "fast");
}

TEST(Routing, RejectsStalePricesWhenRequired) {
    auto cfg = makeConfig();
    cfg.pricing.require_fresh = true;
    cfg.pricing.stale_after_sec = 1;
    cfg.pricing.max_stale_sec = 2;
    cfg.routers.resize(1);
    Harness h(std::move(cfg));
    auto snap = test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)});
    snap.fetched_at = std::chrono::system_clock::now() - std::chrono::seconds(3);
    h.cat.replace(snap);
    const auto plan = h.plan();
    EXPECT_FALSE(plan.hasRoute());
    EXPECT_EQ(plan.error, "prices_unavailable");
    EXPECT_EQ(plan.reason, "stale_prices");
}

TEST(Routing, UsesStalePricesWithWarningWhenAllowed) {
    auto cfg = makeConfig();
    cfg.pricing.require_fresh = false;
    cfg.pricing.stale_after_sec = 1;
    cfg.pricing.max_stale_sec = 60;
    cfg.routers.resize(1);
    Harness h(std::move(cfg));
    auto snap = test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)});
    snap.fetched_at = std::chrono::system_clock::now() - std::chrono::seconds(3);
    h.cat.replace(snap);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_TRUE(plan.stale_prices);
    EXPECT_EQ(plan.reason, "cost_stale");
}

TEST(Routing, CountryAndQuantizationFilters) {
    auto cfg = makeConfig();
    cfg.routing.country = "ru";
    cfg.routing.quantization = {"fp8"};
    cfg.routers.resize(1);
    Harness h(std::move(cfg));
    auto us = test::offer("us", 0.00001, 0.00001);
    us.countries = {"us"};
    auto wrong_q = test::offer("wrong-q", 0.00002, 0.00002);
    wrong_q.quantization = "bf16";
    h.cat.replace(test::snapshot("mock/model-a", {us, wrong_q}));
    const auto plan = h.plan();
    EXPECT_FALSE(plan.hasRoute());
    EXPECT_GE(plan.rejected.size(), 2u);
}

TEST(Routing, FixedFeeIsIncludedInCost) {
    // Свойство: комиссия сдвигает стоимость ровно на свою величину, а не
    // «укладывается» в заведомо больший порог.
    auto with_fee = makeConfig();
    with_fee.routers.resize(1);
    with_fee.routers[0].fixed_fee_nano = 1'000'000;
    Harness h_fee(std::move(with_fee));
    h_fee.cat.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)}));
    const auto fee_plan = h_fee.plan();
    ASSERT_TRUE(fee_plan.hasRoute());

    auto without_fee = makeConfig();
    without_fee.routers.resize(1);
    Harness h_free(std::move(without_fee));
    h_free.cat.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)}));
    const auto free_plan = h_free.plan();
    ASSERT_TRUE(free_plan.hasRoute());

    const auto& paid = fee_plan.candidates.front();
    const auto& free = free_plan.candidates.front();
    EXPECT_EQ(paid.fixed_fee_nano, 1'000'000);
    EXPECT_EQ(paid.cost_nano - free.cost_nano, 1'000'000);
    EXPECT_EQ(paid.rank_cost_nano - free.rank_cost_nano, 1'000'000);
    EXPECT_GT(paid.cost_per_million_nano, free.cost_per_million_nano);
}

TEST(Routing, DirectProviderWorksWithoutPriceSnapshot) {
    AppConfig cfg;
    cfg.routing.models = {"local/llama"};
    cfg.routing.default_model = "local/llama";
    cfg.routing.aliases["default"] = "local/llama";
    RouterConfig local = test::router("local-llama", "http://127.0.0.1:8080/v1");
    local.kind = "openai_compatible";
    local.provider_tag = "local-llama";
    local.model_map["local/llama"] = "llama-3.1-8b-instruct";
    local.prompt_per_million_rub = 10.0L;
    local.completion_per_million_rub = 30.0L;
    local.context_length = 131072;
    local.max_completion_tokens = 8192;
    cfg.routers.push_back(local);
    Harness h(std::move(cfg));

    const auto plan = h.plan("default");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().router_id, "local-llama");
    EXPECT_EQ(plan.candidates.front().provider_tag, "local-llama");
    EXPECT_GT(plan.candidates.front().cost_nano, 0);
    EXPECT_TRUE(plan.provider_order.empty());

    std::string error;
    auto body = buildUpstreamBody(Json::Value(), plan, plan.candidates.front(), h.cfg, error);
    EXPECT_FALSE(error.empty()); // non-object input is rejected
    Json::Value original(Json::objectValue);
    original["model"] = "default";
    original["provider"] = Json::Value(Json::objectValue);
    body = buildUpstreamBody(original, plan, plan.candidates.front(), h.cfg, error);
    ASSERT_TRUE(error.empty());
    EXPECT_EQ(body["model"].asString(), "llama-3.1-8b-instruct");
    EXPECT_FALSE(body.isMember("provider"));
}

TEST(Routing, DirectProviderCanWinAndQueueOrderStaysRouterAiOnly) {
    auto cfg = makeConfig();
    RouterConfig local = test::router("local-llama", "http://127.0.0.1:8080/v1");
    local.kind = "openai_compatible";
    local.provider_tag = "local-llama";
    local.prompt_per_million_rub = 0;
    local.completion_per_million_rub = 0;
    local.context_length = 131072;
    cfg.routers.push_back(local);
    Harness h(std::move(cfg));
    addCatalog(h.cat);

    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().router_id, "local-llama");
    EXPECT_EQ(plan.provider_priority.front(), "local-llama");
    ASSERT_FALSE(plan.provider_order.empty());
    for (const auto& tag : plan.provider_order) EXPECT_NE(tag, "local-llama");
    const RouteCandidate* routerai = nullptr;
    for (const auto& c : plan.candidates) {
        if (c.router_id == "router-a") { routerai = &c; break; }
    }
    ASSERT_NE(routerai, nullptr);
    std::string error;
    Json::Value original(Json::objectValue);
    auto body = buildUpstreamBody(original, plan, *routerai, h.cfg, error);
    ASSERT_TRUE(error.empty());
    EXPECT_EQ(body["model"].asString(), "mock/model-a");
    ASSERT_TRUE(body.isMember("provider"));
    EXPECT_TRUE(body["provider"].isMember("order"));
}

TEST(Routing, DirectProviderSurvivesStaleSnapshot) {
    AppConfig cfg;
    cfg.routing.models = {"local/model"};
    cfg.pricing.max_stale_sec = 1;
    cfg.pricing.require_fresh = true;
    RouterConfig local = test::router("local", "http://127.0.0.1:8080/v1");
    local.kind = "openai_compatible";
    local.provider_tag = "local";
    cfg.routers.push_back(local);
    Harness h(std::move(cfg));
    auto snap = test::snapshot("local/model", {test::offer("remote", 0.1, 0.2)});
    snap.fetched_at = std::chrono::system_clock::now() - std::chrono::hours(1);
    h.cat.replace(snap);
    const auto plan = h.plan("local/model");
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().router_id, "local");
    EXPECT_TRUE(plan.stale_prices);
}

TEST(Routing, CacheCostUsesObservedHitRatioAndCapacity) {
    PriceOffer o = test::offer("cached", 0.0001, 0.0003);
    o.cache_read_per_token = rubToNano(0.00002);
    o.cache_capacity_tokens = 1000;
    PriceOffer no_cache_offer = o;
    const auto no_cache = estimateOfferCostBreakdown(no_cache_offer, 2000, 100, "replace", 0.0);
    const auto cached = estimateOfferCostBreakdown(o, 2000, 100, "replace", 0.5);
    EXPECT_EQ(no_cache.cached_tokens, 0);
    EXPECT_EQ(cached.cached_tokens, 500);
    EXPECT_LT(cached.total_nano, no_cache.total_nano);
    EXPECT_GT(cached.saved_nano, 0);
    EXPECT_EQ(cached.cache_read_nano, safeMulTokens(500, rubToNano(0.00002)));
    EXPECT_EQ(cached.input_nano, safeMulTokens(1500, rubToNano(0.0001)));
}

TEST(Routing, ObservedCacheRatioCanChangeCheapestProvider) {
    auto cfg = makeConfig();
    cfg.routers.resize(1);
    Harness h(std::move(cfg));
    auto cheap_prompt = test::offer("cache-heavy", 0.0001, 0.0003);
    cheap_prompt.cache_read_per_token = rubToNano(0.00001);
    auto cheap_uncached = test::offer("uncached-heavy", 0.00008, 0.0003);
    cheap_uncached.cache_read_per_token = rubToNano(0.00008);
    h.cat.replace(test::snapshot("mock/model-a", {cheap_prompt, cheap_uncached}));

    const auto no_cache = h.router.quotes("mock/model-a", CostMix{2000, 100, 0});
    ASSERT_FALSE(no_cache.empty());
    EXPECT_EQ(no_cache.front().provider_tag, "uncached-heavy");

    const auto with_cache = h.router.quotes("mock/model-a", CostMix{2000, 100, 1800});
    ASSERT_FALSE(with_cache.empty());
    EXPECT_EQ(with_cache.front().provider_tag, "cache-heavy");
    EXPECT_GT(with_cache.front().cache_saved_nano, 0);
    EXPECT_LT(with_cache.front().cost_nano, with_cache.front().cost_no_cache_nano);
}

TEST(Routing, CacheWeightChangesRanking) {
    auto cfg = makeConfig();
    cfg.routers.resize(1);
    cfg.routing.cache_weight = 1.0;
    Harness h(std::move(cfg));
    auto prompt_cheap = test::offer("prompt-cheap", 0.00001, 0.0003);
    prompt_cheap.cache_read_per_token = rubToNano(0.00005);
    auto cache_cheap = test::offer("cache-cheap", 0.0004, 0.0003);
    cache_cheap.cache_read_per_token = rubToNano(0.00001);
    h.cat.replace(test::snapshot("mock/model-a", {prompt_cheap, cache_cheap}));
    const CostMix mix{2000, 100, 1800}; // кеш — основной объём входа

    const auto weight1 = h.router.quotes("mock/model-a", mix);
    ASSERT_FALSE(weight1.empty());
    EXPECT_EQ(weight1.front().provider_tag, "prompt-cheap");
    EXPECT_EQ(weight1.front().rank_cost_nano, weight1.front().cost_nano);

    // Вес кеша > 1 усиливает влияние цены кеша на порядок: выигрывает провайдер
    // с дешёвым кешем, хотя его фактическая стоимость чуть выше.
    h.cfg.routing.cache_weight = 2.0;
    const auto weight2 = h.router.quotes("mock/model-a", mix);
    ASSERT_FALSE(weight2.empty());
    EXPECT_EQ(weight2.front().provider_tag, "cache-cheap");
    const auto breakdown = weight2.front().cost(mix, h.cfg.pricing.threshold_mode);
    EXPECT_EQ(weight2.front().rank_cost_nano, weight2.front().cost_nano + breakdown.cache_read_nano);
    EXPECT_GT(weight2.front().cost_nano, weight1.front().cost_nano);
}

TEST(Routing, OutputWeightChangesRanking) {
    auto cfg = makeConfig();
    cfg.routers.resize(1);
    cfg.routing.output_weight = 1.0;
    Harness h(std::move(cfg));
    auto output_cheap = test::offer("output-cheap", 0.0004, 0.00001);
    auto output_heavy = test::offer("output-heavy", 0.00001, 0.0004);
    h.cat.replace(test::snapshot("mock/model-a", {output_cheap, output_heavy}));
    const CostMix mix{2000, 1000, 0}; // выход сопоставим со входом

    const auto weight1 = h.router.quotes("mock/model-a", mix);
    ASSERT_FALSE(weight1.empty());
    EXPECT_EQ(weight1.front().provider_tag, "output-heavy");
    EXPECT_EQ(weight1.front().rank_cost_nano, weight1.front().cost_nano);

    // Вес выхода > 1 усиливает влияние цены выхода: выигрывает провайдер с
    // дешёвым выходом, хотя его фактическая стоимость выше.
    h.cfg.routing.output_weight = 100.0;
    const auto weight2 = h.router.quotes("mock/model-a", mix);
    ASSERT_FALSE(weight2.empty());
    EXPECT_EQ(weight2.front().provider_tag, "output-cheap");
    const auto breakdown = weight2.front().cost(mix, h.cfg.pricing.threshold_mode);
    EXPECT_EQ(weight2.front().rank_cost_nano,
              weight2.front().cost_nano + 99 * breakdown.output_nano);
    EXPECT_GT(weight2.front().cost_nano, weight1.front().cost_nano);
}

TEST(Routing, ProviderBlacklistOverridesCheapestPrice) {
    auto cfg = makeConfig();
    cfg.routers.resize(1);
    ProviderBlockConfig block;
    block.provider = "Blocked*";
    block.models = {"mock/*"};
    block.reason = "security review";
    cfg.routing.provider_blacklist.push_back(block);
    Harness h(std::move(cfg));
    auto cheap = test::offer("blocked-provider", 0.00001, 0.00001);
    auto allowed = test::offer("allowed-provider", 0.0005, 0.0005);
    h.cat.replace(test::snapshot("mock/model-a", {cheap, allowed}));
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "allowed-provider");
    bool blocked = false;
    for (const auto& c : plan.rejected) {
        if (c.provider_tag == "blocked-provider") {
            blocked = true;
            EXPECT_EQ(c.excluded, "provider_blacklisted");
            EXPECT_EQ(c.excluded_detail, "security review");
        }
    }
    EXPECT_TRUE(blocked);
}

TEST(Routing, QueueRebuildsAfterPriceChange) {
    Harness h;
    addCatalog(h.cat);
    const auto first = h.plan();
    ASSERT_TRUE(first.hasRoute());
    EXPECT_EQ(first.candidates.front().provider_tag, "fast");

    // Новый каталог: дешевле всех оказался другой провайдер.
    h.cat.replace(test::snapshot("mock/model-a", {test::offer("late", 0.000001, 0.000001)}));
    const auto second = h.plan();
    ASSERT_TRUE(second.hasRoute());
    EXPECT_EQ(second.candidates.front().provider_tag, "late");
}

TEST(Routing, CheaperInEveryComponentWinsForAnyMix) {
    Harness h;
    auto baidu = test::offer("baidu", 5.956e-06, 2.3824e-05, 0, 2'000'000, 393216);
    baidu.cache_read_per_token = rubToNano(1.19e-07);
    auto river = test::offer("river", 1.1278e-05, 4.5111e-05, 0, 2'000'000, 393216);
    river.cache_read_per_token = rubToNano(2.2556e-07);
    h.cat.replace(test::snapshot("mock/model-a", {river, baidu}));

    // Дешевле по всем трём компонентам — значит первый при любом миксе
    // токенов: вход, выход и кеш не могут переставить порядок.
    const CostMix mixes[] = {
        {1000, 500, 0},
        {200'000, 200, 190'000},
        {100, 5000, 0},
        {50'000, 50'000, 25'000},
    };
    for (const auto& mix : mixes) {
        const auto quotes = h.router.quotes("mock/model-a", mix);
        ASSERT_FALSE(quotes.empty());
        EXPECT_EQ(quotes.front().provider_tag, "baidu")
            << mix.input_tokens << "/" << mix.output_tokens << "/" << mix.cached_tokens;
        // Цена в кандидате — текущая каталожная, без подмены статистикой.
        EXPECT_EQ(quotes.front().prompt_price_nano, baidu.prompt_per_token);
        EXPECT_EQ(quotes.front().cache_read_price_nano, baidu.cache_read_per_token);
        EXPECT_EQ(quotes.front().completion_price_nano, baidu.completion_per_token);
    }

    observe(h.stats, "mock/model-a", 200'000, 200, 190'000);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "baidu");
    EXPECT_EQ(plan.provider_order.front(), "baidu");
}

TEST(Routing, DownProviderIsExcludedAndPinRebinds) {
    auto cfg = makeConfig();
    cfg.routing.fallback_attempts = 2;
    cfg.routing.session_pin_sec = 600;
    cfg.routing.leader.margin_percent = 50.0;
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    h.queue.build();
    ASSERT_EQ(h.queue.planForSession("mock/model-a", "s1").candidates.front().provider_tag, "fast");
    ASSERT_EQ(h.queue.sessionPin("s1", "mock/model-a"), "fast");

    // Провайдер сессии не работает: план сразу идёт следующему по очереди,
    // привязка снимается с причиной provider_down.
    const auto plan = h.queue.planForSession("mock/model-a", "s1", {"fast"});
    ASSERT_TRUE(plan.hasRoute());
    EXPECT_EQ(plan.candidates.front().provider_tag, "slow");
    EXPECT_EQ(plan.provider_order.front(), "slow");
    EXPECT_EQ(plan.session_rebind_from, "fast");
    EXPECT_EQ(plan.session_rebind_reason, std::string(kProviderDown));
    bool rejected = false;
    for (const auto& candidate : plan.rejected) {
        if (candidate.provider_tag == "fast") {
            rejected = true;
            EXPECT_EQ(candidate.excluded, std::string(kProviderDown));
        }
    }
    EXPECT_TRUE(rejected);
}

TEST(Routing, AllProvidersDownGivesProviderDownError) {
    Harness h;
    addCatalog(h.cat);
    h.queue.build();
    const auto plan = h.queue.planFor("mock/model-a", {"fast", "slow"});
    EXPECT_FALSE(plan.hasRoute());
    EXPECT_EQ(plan.error, std::string(kProviderDown));
}

TEST(Routing, HealthMarksProviderDownAfterStreakAndRecovers) {
    AppConfig cfg;
    cfg.routing.health.fail_threshold = 2;
    cfg.routing.health.down_after_sec = 0;
    cfg.routing.health.probe_interval_sec = 60;
    cfg.routing.health.recover_successes = 2;
    ProviderHealth health(cfg);
    EXPECT_FALSE(health.isDown("fast"));
    health.noteFailure("fast", "router-a", "mock/model-a", 500, "boom");
    EXPECT_FALSE(health.isDown("fast"));
    health.noteFailure("fast", "router-a", "mock/model-a", 500, "boom");
    EXPECT_TRUE(health.isDown("fast"));
    // Проверка ещё не наступила: интервал не прошёл.
    EXPECT_TRUE(health.dueProbes().empty());

    health.noteProbeResult("fast", true, 200, "");
    EXPECT_TRUE(health.isDown("fast")); // нужен второй успех подряд
    health.noteProbeResult("fast", true, 200, "");
    EXPECT_FALSE(health.isDown("fast"));
    const auto snapshot = health.snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].fail_streak, 0);
    EXPECT_EQ(snapshot[0].down_since, 0);
}

TEST(Routing, HealthMarksProviderDownAfterDuration) {
    AppConfig cfg;
    cfg.routing.health.fail_threshold = 0;
    cfg.routing.health.down_after_sec = 1;
    cfg.routing.health.probe_interval_sec = 60;
    ProviderHealth health(cfg);
    health.noteFailure("fast", "router-a", "mock/model-a", 0, "timeout");
    EXPECT_FALSE(health.isDown("fast"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    health.noteFailure("fast", "router-a", "mock/model-a", 0, "timeout");
    EXPECT_TRUE(health.isDown("fast"));
    // Успешный живой запрос не подменяет проверку доступности.
    health.noteSuccess("fast");
    EXPECT_TRUE(health.isDown("fast"));
}

TEST(Routing, HealthSuccessResetsFailureStreak) {
    AppConfig cfg;
    cfg.routing.health.fail_threshold = 3;
    cfg.routing.health.down_after_sec = 0;
    ProviderHealth health(cfg);
    health.noteFailure("fast", "router-a", "mock/model-a", 500, "boom");
    health.noteFailure("fast", "router-a", "mock/model-a", 500, "boom");
    health.noteSuccess("fast");
    health.noteFailure("fast", "router-a", "mock/model-a", 500, "boom");
    EXPECT_FALSE(health.isDown("fast"));
}

TEST(Routing, LatencyPriceEntersWeightAndOrder) {
    AppConfig cfg = makeConfig();
    cfg.routing.wait_price_rub_per_sec = 1.0; // 1 RUB за секунду ожидания
    PriceCatalog cat;
    addCatalog(cat);
    Router router(cfg, cat);
    const CostMix mix{1000, 500, 0};
    const auto quotes = router.quotes("mock/model-a", mix, [](const std::string& provider) {
        return provider == "fast" ? 2000 : 0;
    });
    ASSERT_GE(quotes.size(), 2u);
    // fast дешевле по токенам, но 2 с ожидания дороже: первым идёт slow.
    EXPECT_EQ(quotes.front().provider_tag, "slow");
    const std::int64_t billed = mix.input_tokens + mix.output_tokens;
    for (const auto& c : quotes) {
        EXPECT_EQ(c.weight_per_million_nano,
                  c.cost_per_million_nano + c.latency_cost_per_million_nano);
        // Вес — это rank_cost (токены плюс время) на 1M, а не он же с добавленной
        // ещё раз ценой задержки: иначе задержка входит в порядок дважды.
        EXPECT_NEAR(static_cast<double>(perMillionFromTotal(c.rank_cost_nano, billed)),
                    static_cast<double>(c.weight_per_million_nano), 2.0);
        if (c.provider_tag == "fast") {
            EXPECT_EQ(c.latency_ms, 2000);
            EXPECT_GT(c.latency_cost_per_million_nano, 0);
            EXPECT_GT(c.weight_per_million_nano, c.cost_per_million_nano);
        }
    }
    const auto plain = router.quotes("mock/model-a", mix);
    ASSERT_FALSE(plain.empty());
    EXPECT_EQ(plain.front().provider_tag, "fast");
    EXPECT_EQ(plain.front().latency_ms, 0);
    EXPECT_EQ(plain.front().latency_cost_per_million_nano, 0);
}

TEST(Routing, QueueUsesObservedLatencyInWeight) {
    auto cfg = makeConfig();
    cfg.routing.wait_price_rub_per_sec = 1.0;
    cfg.routers.resize(1); // один оффер на провайдера: план без дублей по роутерам
    Harness h(std::move(cfg));
    addCatalog(h.cat);
    RequestRecord fast;
    fast.request_id = "lat-1";
    fast.model = "mock/model-a";
    fast.provider_tag = "fast";
    fast.status = 200;
    fast.latency_ms = 2000;
    fast.tokens_in = 1000;
    fast.tokens_out = 500;
    h.stats.record(fast);
    EXPECT_EQ(h.stats.latencyMs("mock/model-a", "fast"), 2000);
    EXPECT_EQ(h.stats.latencyMs("mock/model-a", "fast", 30), 2000);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    // Наблюдаемая задержка вошла в вес: очередь ушла от дешёвого, но медленного.
    EXPECT_EQ(plan.candidates.front().provider_tag, "slow");
    EXPECT_EQ(plan.candidates.front().latency_ms, 0);
    ASSERT_GE(plan.candidates.size(), 2u);
    EXPECT_EQ(plan.candidates[1].provider_tag, "fast");
    EXPECT_EQ(plan.candidates[1].latency_ms, 2000);
    EXPECT_GT(plan.candidates[1].latency_cost_per_million_nano, 0);
}

TEST(Routing, CacheWriteTokensPricedAsInputUnlessOfferHasTariff) {
    TokenCostRequest req;
    req.input_tokens = 1000;
    req.output_tokens = 100;
    req.cached_tokens = 400;
    req.cache_write_tokens = 200;
    req.input_price = rubToNano(0.0001);
    req.cache_read_price = rubToNano(0.00002);
    req.output_price = rubToNano(0.0003);
    const auto with_input_price = computeTokenCost(req);
    // Отдельного тарифа нет: запись в кеш стоит как входной токен.
    EXPECT_EQ(with_input_price.cache_write_nano, safeMulTokens(200, rubToNano(0.0001)));
    EXPECT_EQ(with_input_price.input_nano,
              safeMulTokens(600, rubToNano(0.0001)) + with_input_price.cache_write_nano);
    EXPECT_EQ(with_input_price.cache_read_nano, safeMulTokens(400, rubToNano(0.00002)));

    req.cache_write_price = rubToNano(0.00005);
    const auto with_write_price = computeTokenCost(req);
    EXPECT_EQ(with_write_price.cache_write_nano, safeMulTokens(200, rubToNano(0.00005)));
    EXPECT_EQ(with_write_price.total_nano,
              with_input_price.total_nano - safeMulTokens(200, rubToNano(0.0001)) +
                  safeMulTokens(200, rubToNano(0.00005)));
    // Экономия попаданий в кеш не зависит от тарифа записи.
    EXPECT_EQ(with_write_price.saved_nano, with_input_price.saved_nano);
}

TEST(Routing, CostSaturatesInsteadOfOverflowing) {
    TokenCostRequest req;
    req.input_tokens = 4;
    req.output_tokens = 4;
    req.input_price = std::numeric_limits<NanoRub>::max() / 2;
    req.output_price = std::numeric_limits<NanoRub>::max() / 2;
    req.fixed_fee = std::numeric_limits<NanoRub>::max() / 2;
    EXPECT_EQ(computeTokenCost(req).total_nano, std::numeric_limits<NanoRub>::max());
}

// Опубликованный вес — тот же расчёт, что у панели и API: если очередь начнёт
// считать порядок по-своему, свойство перестанет выполняться.
TEST(Routing, PublishedWeightIsTheSharedFormula) {
    auto cfg = makeConfig();
    cfg.routing.output_weight = 1.0;
    cfg.routing.cache_weight = 2.3;
    Harness h(cfg);
    h.cat.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003),
                                                  test::offer("slow", 0.0002, 0.0004)}));
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    ASSERT_FALSE(plan.candidates.empty());
    for (const auto& c : plan.candidates) {
        const CostMix effective =
            mixForOffer(c.offer, plan.estimated_in, plan.estimated_out, plan.cache_hit_ratio);
        const CostBreakdown cost = c.cost(effective, h.cfg.pricing.threshold_mode);
        const RankCost rank =
            rankCost(cost, c.fixed_fee_nano, c.latency_cost_nano,
                     h.cfg.routing.output_weight, h.cfg.routing.cache_weight);
        EXPECT_EQ(rank.rank_nano, c.rank_cost_nano) << c.provider_tag;
        EXPECT_EQ(rank.input_nano, c.input_cost_nano) << c.provider_tag;
        EXPECT_EQ(perMillionFromTotal(rank.tokens_nano, effective.billedTokens()),
                  c.cost_per_million_nano)
            << c.provider_tag;
        EXPECT_EQ(c.cost_per_million_nano + c.latency_cost_per_million_nano,
                  c.weight_per_million_nano)
            << c.provider_tag;
        // Слагаемые веса — токены и задержка порознь: rank_cost_nano включает
        // задержку, cost_per_million_nano — нет.
        EXPECT_EQ(rank.tokens_nano + rank.latency_nano, c.rank_cost_nano) << c.provider_tag;
        EXPECT_EQ(effective.cached_tokens, c.cached_tokens) << c.provider_tag;
    }
}

// Прогресс подтверждения challenger'а — часть состояния лидера: он сохраняется в
// файл, иначе после перезапуска `routing.leader.confirm_builds` требует больше
// перестроений, чем настроено.
TEST(Routing, ChallengerProgressSurvivesRestart) {
    test::TempDir dir;
    auto cfg = makeConfig();
    cfg.routing.leader.state_file = (dir.path() / "leader.json").string();
    cfg.routing.leader.margin_percent = 0.0;
    cfg.routing.leader.confirm_builds = 2;
    cfg.routing.leader.min_dwell_sec = 0;
    {
        Harness h(cfg);
        addCatalog(h.cat);
        h.queue.build();
        ASSERT_EQ(h.queue.explain("mock/model-a")["leader"].asString(), "fast");
        // Первое подтверждение challenger'а: лидер ещё не сменился, но прогресс
        // обязан попасть в файл состояния.
        priceChallenger(h, 0.00001, 0.00003);
        h.queue.build();
        EXPECT_EQ(h.queue.explain("mock/model-a")["leader"].asString(), "fast");
    }
    {
        // Перезапуск: одного построения достаточно, чтобы подтверждений стало два.
        Harness h(cfg);
        priceChallenger(h, 0.00001, 0.00003);
        h.queue.start();
        h.queue.stop();
        EXPECT_EQ(h.queue.explain("mock/model-a")["leader"].asString(), "slow");
    }
}

// Источник кода route_not_buildable: кандидат ссылается на исчезнувший апстрим,
// и сборка тела обязана назвать причину, а не отдать пустой объект.
TEST(Routing, BuildUpstreamBodyReportsMissingRouter) {
    Harness h;
    addCatalog(h.cat);
    const auto plan = h.plan();
    ASSERT_TRUE(plan.hasRoute());
    h.cfg.routers.clear();
    Json::Value body(Json::objectValue);
    body["model"] = "mock/model-a";
    std::string err;
    const Json::Value built = buildUpstreamBody(body, plan, plan.candidates.front(), h.cfg, err);
    EXPECT_EQ(err, "router_not_found");
    EXPECT_FALSE(built.isMember("provider"));
}
