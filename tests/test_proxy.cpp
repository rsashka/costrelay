#include "TestHelpers.h"
#include "proxy/ProxyService.h"
#include "proxy/UpstreamFault.h"
#include "routing/ProviderQueue.h"

#include <gtest/gtest.h>

using namespace proxy;

// Классификация ответа апстрима — один владелец на ретраи, терминальный ответ,
// журнал и проверку доступности. Проверяются все ветки, включая те, где старые
// копии расходились: 401/403 — проблема без повтора, 400/422 — не проблема.
TEST(ProxyService, UpstreamFaultClassificationIsSingleOwner) {
    const auto auth = classifyUpstream(403);
    EXPECT_TRUE(auth.problem);
    EXPECT_FALSE(auth.retryable);
    EXPECT_STREQ(upstreamFaultCode(auth.fault), "upstream_auth_failed");
    EXPECT_TRUE(classifyUpstream(401).problem);
    EXPECT_FALSE(classifyUpstream(401).retryable);

    EXPECT_FALSE(classifyUpstream(400).problem);
    EXPECT_FALSE(classifyUpstream(400).retryable);
    EXPECT_FALSE(classifyUpstream(422).problem);
    EXPECT_FALSE(classifyUpstream(200).problem);

    for (const long status : {0L, 404L, 408L, 429L, 500L, 503L}) {
        EXPECT_TRUE(classifyUpstream(status).problem) << status;
        EXPECT_TRUE(classifyUpstream(status).retryable) << status;
    }
    EXPECT_STREQ(upstreamFaultCode(classifyUpstream(429).fault), "rate_limited");
    EXPECT_STREQ(upstreamFaultCode(classifyUpstream(0).fault), "upstream_timeout");
    EXPECT_STREQ(upstreamFaultCode(classifyUpstream(408).fault), "upstream_timeout");
    EXPECT_STREQ(upstreamFaultCode(classifyUpstream(404).fault), "provider_unavailable");
    EXPECT_STREQ(upstreamFaultCode(classifyUpstream(500).fault), "upstream_error");
}

TEST(ProxyService, ModelsJsonAndExplain) {
    AppConfig cfg;
    cfg.routing.models = {"mock/model-a"};
    cfg.routing.default_model = "mock/model-a";
    cfg.routing.aliases["fast"] = "mock/model-a";
    cfg.pricing.threshold_mode = "replace";
    cfg.routers.push_back(test::router("router-a", "https://a.example/v1"));
    PriceCatalog catalog;
    catalog.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)}));
    Router router(cfg, catalog);
    Stats stats;
    ProviderQueue queue(cfg, router, catalog, stats);
    queue.build();
    CaptureConfig cap;
    cap.enabled = false;
    BodyStore bodies(cap);
    std::string err;
    ASSERT_TRUE(bodies.init(err));
    ProxyService proxy(cfg, catalog, queue, stats, bodies);
    auto models = proxy.modelsJson();
    EXPECT_EQ(models["object"].asString(), "list");
    EXPECT_EQ(models["data"][0]["id"].asString(), "mock/model-a");
    auto explain = proxy.explain("mock/model-a");
    EXPECT_EQ(explain["model"].asString(), "mock/model-a");
    EXPECT_EQ(explain["candidates"][0]["provider"].asString(), "fast");
    EXPECT_EQ(explain["order"][0].asString(), "fast");
}

namespace {

struct Harness {
    AppConfig cfg;
    PriceCatalog catalog;
    Router router;
    Stats stats;
    ProviderQueue queue;
    CaptureConfig cap;
    BodyStore bodies;
    ProxyService proxy;
    Harness()
        : router(cfg, catalog), stats(10), queue(cfg, router, catalog, stats), bodies(cap),
          proxy(cfg, catalog, queue, stats, bodies) {
        cfg.routing.models = {"mock/model-a"};
        cfg.routing.default_model = "mock/model-a";
        cfg.routing.aliases["default"] = "mock/model-a";
        cfg.routing.client_provider_policy = "strip";
        cfg.pricing.threshold_mode = "replace";
        cfg.routers.push_back(test::router("router-a", "https://a.example/v1"));
        cap.enabled = false;
        std::string err;
        bodies.init(err);
        catalog.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)}));
        queue.build();
    }
};

drogon::HttpRequestPtr makeRequest(const std::string& body, const std::string& path = "/v1/chat/completions") {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath(path);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    req->setBody(body);
    return req;
}

long call(ProxyService& proxy, const drogon::HttpRequestPtr& req, Principal p,
          std::string* out = nullptr) {
    long status = 0;
    proxy.handleChat(req, std::move(p), [&](const drogon::HttpResponsePtr& resp) {
        status = static_cast<long>(resp->getStatusCode());
        if (out) *out = std::string(resp->body());
    });
    return status;
}

Principal principal(const std::string& token_id = "t", const std::string& scope = "proxy:invoke",
                    const std::string& model = "*") {
    Principal p;
    p.token_id = token_id;
    p.scopes = {scope};
    p.models = {model};
    return p;
}

} // namespace

TEST(ProxyService, RejectsMalformedRequestAndAcl) {
    Harness h;
    EXPECT_EQ(call(h.proxy, makeRequest("{bad"), principal()), 400);
    EXPECT_EQ(call(h.proxy, makeRequest(R"({})"), principal()), 400);
    EXPECT_EQ(call(h.proxy, makeRequest(R"({"model":"unknown","messages":[]})"), principal()), 404);
    EXPECT_EQ(call(h.proxy, makeRequest(R"({"model":"default","messages":[]})"),
                   principal("t", "proxy:invoke", "other/model")), 403);
    EXPECT_EQ(call(h.proxy, makeRequest(R"({"model":"default","messages":[]})"),
                   principal("t", "stats:read", "*")), 403);
}

TEST(ProxyService, RejectsClientProviderInRejectMode) {
    Harness h;
    h.cfg.routing.client_provider_policy = "reject";
    Harness h2; // cfg is not propagated into the constructed router; test early policy only
    // Reconstruct with reject policy.
    AppConfig cfg = h.cfg;
    PriceCatalog catalog;
    catalog.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)}));
    Router router(cfg, catalog);
    Stats stats;
    ProviderQueue queue(cfg, router, catalog, stats);
    queue.build();
    CaptureConfig cap;
    BodyStore bodies(cap);
    std::string err;
    ASSERT_TRUE(bodies.init(err));
    ProxyService proxy(cfg, catalog, queue, stats, bodies);
    EXPECT_EQ(call(proxy, makeRequest(R"({"model":"default","messages":[],"provider":{"only":["slow"]}})"),
                   principal()), 400);
}

TEST(ProxyService, NoRouteReturns503) {
    Harness h;
    PriceCatalog empty;
    Router router(h.cfg, empty);
    Stats stats;
    ProviderQueue queue(h.cfg, router, empty, stats);
    queue.build();
    CaptureConfig cap;
    BodyStore bodies(cap);
    std::string err;
    ASSERT_TRUE(bodies.init(err));
    ProxyService proxy(h.cfg, empty, queue, stats, bodies);
    std::string body;
    EXPECT_EQ(call(proxy, makeRequest(R"({"model":"default","messages":[]})"), principal(), &body),
              503);
    // Отказ называет причину: клиент и журнал видят код, а не только статус.
    EXPECT_NE(body.find("no_price_snapshot"), std::string::npos) << body;
    EXPECT_NE(body.find("error"), std::string::npos) << body;
}

TEST(ProxyService, RejectsWhenQuotaIsExhausted) {
    AppConfig cfg;
    cfg.routing.models = {"mock/model-a"};
    cfg.routing.default_model = "mock/model-a";
    cfg.pricing.threshold_mode = "replace";
    cfg.routers.push_back(test::router("router-a", "https://a.example/v1"));
    PriceCatalog catalog;
    catalog.replace(test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)}));
    Router router(cfg, catalog);
    Stats stats;
    ProviderQueue queue(cfg, router, catalog, stats);
    queue.build();
    CaptureConfig cap;
    BodyStore bodies(cap);
    std::string err;
    ASSERT_TRUE(bodies.init(err));
    QuotaManager quotas;
    ProxyService proxy(cfg, catalog, queue, stats, bodies, &quotas);

    QuotaLimits limits;
    limits.rpm = 1;
    auto pre = quotas.acquire("limited", limits, 10, 0);
    ASSERT_TRUE(pre.allowed);
    Principal p = principal("limited");
    p.rpm = 1;
    auto req = makeRequest(R"({"model":"mock/model-a","messages":[{"role":"user","content":"hi"}]})");
    EXPECT_EQ(call(proxy, req, p), 429);

    QuotaLimits budget;
    budget.day_budget_nano = 1000;
    auto pre_budget = quotas.acquire("budget", budget, 0, 900);
    ASSERT_TRUE(pre_budget.allowed);
    Principal pb = principal("budget");
    pb.day_budget_nano = 1000;
    auto req2 = makeRequest(R"({"model":"mock/model-a","messages":[{"role":"user","content":"hi"}]})");
    EXPECT_EQ(call(proxy, req2, pb), 429);
}
