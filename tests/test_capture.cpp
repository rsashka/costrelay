#include "TestHelpers.h"
#include "capture/BodyStore.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace proxy;

TEST(Capture, EncryptsAndReadsBody) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "test-body-key";
    cfg.compression = "gzip";
    cfg.max_request_bytes = 1024;
    cfg.max_response_bytes = 1024;
    cfg.retention_days = 7;
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err)) << err;
    const std::string secret = "super-secret prompt text";
    auto meta = store.put("req-0001", "request", "application/json", secret);
    ASSERT_TRUE(meta.has_value());
    EXPECT_TRUE(meta->truncated == false);
    auto got = store.get("req-0001", "request");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, secret);

    // Ciphertext on disk must not contain plaintext.
    std::ifstream in(meta->path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(raw.find(secret), std::string::npos);
    EXPECT_FALSE(store.metadataJson("req-0001").empty());
}

TEST(Capture, TruncatesToConfiguredLimit) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "k";
    cfg.max_request_bytes = 4;
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err)) << err;
    auto meta = store.put("req-0002", "request", "text/plain", "1234567890");
    ASSERT_TRUE(meta.has_value());
    EXPECT_TRUE(meta->truncated);
    auto got = store.get("req-0002", "request");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "1234");
}

TEST(Capture, RejectsPathTraversalRequestId) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "k";
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err)) << err;
    // Идентификатор, непригодный для имени файла, отвергается без исключения.
    EXPECT_FALSE(store.put("../evil", "request", "text/plain", "x").has_value());
}

TEST(Capture, ZstdRoundTrip) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "test-zstd-key";
    cfg.compression = "zstd";
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err)) << err;
    auto meta = store.put("req-z000", "response", "application/json", "zstd payload 123456");
    ASSERT_TRUE(meta.has_value());
    auto got = store.get("req-z000", "response");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "zstd payload 123456");
}

TEST(Capture, DisabledStoreIsNoop) {
    CaptureConfig cfg;
    cfg.enabled = false;
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err));
    EXPECT_FALSE(store.put("req", "request", "text/plain", "x").has_value());
    EXPECT_FALSE(store.get("req", "request").has_value());
}

TEST(Capture, UnknownBodyAndTamper) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "tamper-key";
    cfg.compression = "none";
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err));
    EXPECT_FALSE(store.get("missing", "request").has_value());
    auto meta = store.put("req-t000", "response", "application/json", "sensitive body");
    ASSERT_TRUE(meta.has_value());
    {
        std::fstream f(meta->path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(-1, std::ios::end);
        char c = 0;
        f.read(&c, 1);
        f.seekp(-1, std::ios::end);
        char flipped = static_cast<char>(c ^ 0x01);
        f.write(&flipped, 1);
    }
    EXPECT_FALSE(store.get("req-t000", "response").has_value());
}

TEST(Capture, MetadataContainsChecksumAndFlags) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "meta-key";
    cfg.max_request_bytes = 5;
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err));
    auto meta = store.put("req-m000", "request", "text/plain", "123456789");
    ASSERT_TRUE(meta.has_value());
    auto md = store.metadataJson("req-m000");
    ASSERT_TRUE(md.isArray());
    EXPECT_EQ(md[0]["kind"].asString(), "request");
    EXPECT_EQ(md[0]["truncated"].asBool(), true);
    EXPECT_EQ(md[0]["sha256"].asString().size(), 64u);
    EXPECT_GT(store.storedBytes(), 0u);
    EXPECT_EQ(store.count(), 1u);
}

TEST(Capture, GcRemovesExpiredBodies) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "gc-key";
    cfg.retention_days = 0;
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err));
    ASSERT_TRUE(store.put("req-gc00", "request", "text/plain", "old").has_value());
    store.gc();
    EXPECT_EQ(store.count(), 0u);
    EXPECT_FALSE(store.get("req-gc00", "request").has_value());
}

TEST(Capture, RejectsWhenQuotaExceeded) {
    test::TempDir dir;
    CaptureConfig cfg;
    cfg.enabled = true;
    cfg.store_dir = dir.str();
    cfg.encryption_key = "quota-key";
    cfg.max_total_bytes = 16;
    BodyStore store(cfg);
    std::string err;
    ASSERT_TRUE(store.init(err));
    EXPECT_FALSE(store.put("req-q000", "request", "text/plain", "0123456789abcdefghij").has_value());
    EXPECT_EQ(store.count(), 0u);
}
