#include "TestHelpers.h"
#include "pricing/PriceCatalog.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace proxy;

namespace {

const char* kJson = R"JSON({
  "data": {
    "id": "mock/model-a",
    "architecture": {"input_modalities": ["text"], "output_modalities": ["text"]},
    "endpoints": [
      {
        "tag": "fast",
        "status": 0,
        "context_length": 128000,
        "max_completion_tokens": 4096,
        "max_prompt_tokens": 120000,
        "supported_parameters": ["tools", "temperature", "max_tokens"],
        "pricing": {"prompt": 0.0001, "completion": 0.0003, "input_cache_read": 0.00002},
        "pricing_units": {"prompt": "token", "completion": "token"},
        "countries": ["ru"],
        "variable_pricings": [
          {"type": "prompt-threshold", "prompt": 0.0006, "threshold": 200000}
        ]
      },
      {
        "tag": "image-ish",
        "status": -1,
        "context_length": 8000,
        "max_completion_tokens": 1000,
        "supported_parameters": ["temperature"],
        "pricing": {"prompt": 1.0, "completion": 2.0},
        "pricing_units": {"prompt": "image", "completion": "image"},
        "countries": []
      }
    ]
  }
})JSON";

Json::Value parse(const std::string& text) {
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    std::istringstream in(text);
    EXPECT_TRUE(Json::parseFromStream(b, in, &root, &errs)) << errs;
    return root;
}

} // namespace

TEST(PriceCatalog, ParsesEndpoints) {
    auto snap = PriceCatalog::parseEndpoints(parse(kJson), "mock/model-a");
    ASSERT_EQ(snap.endpoints.size(), 2u);
    const auto& fast = snap.endpoints.front();
    EXPECT_EQ(fast.provider_tag, "fast");
    EXPECT_EQ(fast.prompt_per_token, 100000);
    EXPECT_EQ(fast.completion_per_token, 300000);
    EXPECT_EQ(fast.cache_read_per_token, 20000);
    EXPECT_EQ(fast.threshold_tokens, 200000);
    EXPECT_EQ(fast.threshold_prompt_per_token, 600000);
    EXPECT_EQ(fast.context_length, 128000);
    EXPECT_EQ(fast.max_completion_tokens, 4096);
    ASSERT_TRUE(fast.max_prompt_tokens.has_value());
    EXPECT_EQ(*fast.max_prompt_tokens, 120000);
    EXPECT_TRUE(fast.chat_compatible);
    const auto& img = snap.endpoints.back();
    EXPECT_EQ(img.provider_tag, "image-ish");
    EXPECT_FALSE(img.chat_compatible);
    EXPECT_EQ(img.status, -1);
}

TEST(PriceCatalog, RejectsMismatchedModel) {
    EXPECT_THROW(PriceCatalog::parseEndpoints(parse(kJson), "other/model"), std::runtime_error);
}

TEST(PriceCatalog, RejectsNoEndpoints) {
    Json::Value root = parse(R"({"data":{"id":"m/x","endpoints":[]}})");
    EXPECT_THROW(PriceCatalog::parseEndpoints(root, "m/x"), std::runtime_error);
}

TEST(PriceCatalog, ReplaceAndGet) {
    PriceCatalog cat;
    auto snap = PriceCatalog::parseEndpoints(parse(kJson), "mock/model-a");
    EXPECT_TRUE(cat.replace(snap));
    auto got = cat.get("mock/model-a");
    ASSERT_TRUE(got);
    EXPECT_EQ(got->endpoints.size(), 2u);
    EXPECT_EQ(cat.size(), 1u);
    EXPECT_EQ(cat.models().size(), 1u);
    EXPECT_FALSE(cat.get("nope"));
}

TEST(PriceCatalog, SnapshotRoundTripPreservesAge) {
    test::TempDir dir;
    const auto path = dir.path() / "prices.json";
    PriceCatalog cat;
    auto snap = PriceCatalog::parseEndpoints(parse(kJson), "mock/model-a");
    snap.fetched_at = std::chrono::system_clock::now() - std::chrono::hours(2);
    // Отдельный тариф записи в кеш обязан пережить снимок: потеря цены занижает
    // стоимость запроса после перезапуска.
    snap.endpoints[0].cache_write_per_token = 50000;
    const auto expected = snap.fetched_at;
    ASSERT_TRUE(cat.replace(snap));
    ASSERT_TRUE(cat.saveSnapshot(path.string()));
    PriceCatalog loaded;
    ASSERT_TRUE(loaded.loadFromFile(path.string()));
    auto got = loaded.get("mock/model-a");
    ASSERT_TRUE(got);
    EXPECT_EQ(got->endpoints.size(), 2u);
    EXPECT_EQ(got->endpoints[0].supported_parameters.size(), 3u);
    EXPECT_EQ(got->endpoints[0].countries.size(), 1u);
    EXPECT_EQ(got->endpoints[0].pricing_units_prompt, "token");
    EXPECT_EQ(got->endpoints[0].cache_read_per_token, 20000);
    EXPECT_EQ(got->endpoints[0].cache_write_per_token, 50000);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::seconds>(
                  expected - got->fetched_at).count(), 0);
}
