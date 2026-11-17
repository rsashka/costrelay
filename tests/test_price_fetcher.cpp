#include "TestHelpers.h"
#include "TestHttpServer.h"
#include "pricing/PriceFetcher.h"

#include <gtest/gtest.h>

using namespace proxy;

namespace {

std::string priceBody() {
    return R"JSON({"data":{"id":"mock/model-a","endpoints":[{
      "tag":"fast","status":0,"context_length":128000,"max_completion_tokens":4096,
      "supported_parameters":["tools","temperature","max_tokens"],
      "pricing":{"prompt":0.0001,"completion":0.0003},
      "pricing_units":{"prompt":"token","completion":"token"},
      "countries":["ru"]
    }]}})JSON";
}

AppConfig configFor(const std::string& base) {
    AppConfig cfg;
    cfg.routing.models = {"mock/model-a"};
    // Источник прайса — RouterAI-апстрим: его адрес и есть база эндпоинтов.
    RouterConfig router;
    router.id = "routerai";
    router.kind = "routerai";
    router.base_url = base;
    router.models = {"mock/*"};
    cfg.routers.push_back(router);
    cfg.pricing.timeout_ms = 2000;
    cfg.pricing.max_body_bytes = 1024 * 1024;
    return cfg;
}

} // namespace

TEST(PriceFetcher, FetchesAndReplacesCatalog) {
    test::TestHttpServer server([](const std::string& req) {
        EXPECT_NE(req.find("GET /api/v1/models/mock/model-a/endpoints"), std::string::npos);
        return test::httpResponse(priceBody());
    });
    auto cfg = configFor("http://127.0.0.1:" + std::to_string(server.port()) + "/api/v1");
    PriceCatalog catalog;
    PriceFetcher fetcher(cfg, catalog);
    ASSERT_TRUE(fetcher.fetchModel("mock/model-a"));
    auto snap = catalog.get("mock/model-a");
    ASSERT_TRUE(snap);
    ASSERT_EQ(snap->endpoints.size(), 1u);
    EXPECT_EQ(snap->endpoints[0].provider_tag, "fast");
    EXPECT_EQ(snap->endpoints[0].prompt_per_token, rubToNano(0.0001));
    EXPECT_TRUE(fetcher.lastError().empty());
}

TEST(PriceFetcher, KeepsErrorOnFailure) {
    test::TestHttpServer server([](const std::string&) {
        return test::httpResponse(R"({"error":{"message":"boom"}})", 500);
    });
    auto cfg = configFor("http://127.0.0.1:" + std::to_string(server.port()) + "/api/v1");
    PriceCatalog catalog;
    PriceFetcher fetcher(cfg, catalog);
    EXPECT_FALSE(fetcher.fetchModel("mock/model-a"));
    EXPECT_FALSE(catalog.get("mock/model-a"));
    EXPECT_FALSE(fetcher.lastError().empty());
}

TEST(PriceFetcher, RejectsInvalidModelId) {
    auto cfg = configFor("http://127.0.0.1:1/api/v1");
    PriceCatalog catalog;
    PriceFetcher fetcher(cfg, catalog);
    EXPECT_THROW(fetcher.fetchModel("not-a-model"), std::runtime_error);
}

TEST(PriceFetcher, UsesEtagAndAccepts304) {
    test::TestHttpServer server([](const std::string& req) {
        EXPECT_NE(req.find("If-None-Match: v1"), std::string::npos);
        std::ostringstream os;
        os << "HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n";
        return os.str();
    });
    auto cfg = configFor("http://127.0.0.1:" + std::to_string(server.port()) + "/api/v1");
    PriceCatalog catalog;
    auto snap = test::snapshot("mock/model-a", {test::offer("fast", 0.0001, 0.0003)});
    snap.etag = "v1";
    catalog.replace(snap);
    PriceFetcher fetcher(cfg, catalog);
    ASSERT_TRUE(fetcher.fetchModel("mock/model-a"));
    ASSERT_TRUE(catalog.get("mock/model-a"));
    EXPECT_EQ(catalog.get("mock/model-a")->etag, "v1");
}

TEST(PriceFetcher, SkipsDirectOpenAiCompatibleModels) {
    AppConfig cfg = configFor("http://127.0.0.1:1/api/v1");
    cfg.routing.models = {"local/model"};
    RouterConfig local = test::router("local", "http://127.0.0.1:8080/v1");
    local.kind = "openai_compatible";
    local.models = {"local/*"};
    cfg.routers.push_back(local);
    PriceCatalog catalog;
    PriceFetcher fetcher(cfg, catalog);
    EXPECT_TRUE(fetcher.refreshNow());
    EXPECT_EQ(catalog.size(), 0u);
    EXPECT_TRUE(fetcher.lastError().empty());
}
