#include "TestHelpers.h"
#include "auth/Auth.h"
#include "util/Strings.h"

#include <gtest/gtest.h>

#include <thread>

using namespace proxy;

TEST(Auth, PasswordHashRoundTrip) {
    auto hash = PasswordHash::hash("correct horse battery staple");
    EXPECT_TRUE(PasswordHash::verify("correct horse battery staple", hash));
    EXPECT_FALSE(PasswordHash::verify("wrong", hash));
    EXPECT_NE(hash.find("$argon2"), std::string::npos);
}

TEST(Auth, TokenStoreHashes) {
    TokenConfig a;
    a.id = "a";
    a.token_hash = "sha256:" + sha256Hex("secret-a");
    a.models = {"mock/*"};
    TokenConfig b;
    b.id = "b";
    b.token_hash = "sha256:" + sha256Hex("secret-b");
    b.models = {"*"};
    TokenStore store({a, b});
    auto pa = store.authenticate("Bearer secret-a");
    ASSERT_TRUE(pa.has_value());
    EXPECT_EQ(pa->token_id, "a");
    EXPECT_TRUE(pa->canUseModel("mock/model-a"));
    EXPECT_FALSE(pa->canUseModel("other/model"));
    auto pb = store.authenticate("Bearer secret-b");
    ASSERT_TRUE(pb.has_value());
    EXPECT_TRUE(pb->canUseModel("other/model"));
    EXPECT_FALSE(store.authenticate("Bearer wrong").has_value());
    EXPECT_FALSE(store.authenticate("").has_value());
}

TEST(Auth, TokenExpiry) {
    TokenConfig a;
    a.id = "a";
    a.token_hash = "sha256:" + sha256Hex("secret");
    a.expires_at = "2000-01-01T00:00:00Z";
    TokenStore store({a});
    EXPECT_FALSE(store.authenticate("Bearer secret").has_value());
}

TEST(Auth, SessionStoreAndCsrf) {
    SessionStore sessions(3600, 60);
    auto s = sessions.create("admin", "admin");
    ASSERT_TRUE(s.has_value());
    auto got = sessions.touch(s->id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->username, "admin");
    EXPECT_TRUE(sessions.validateCsrf(s->id, s->csrf));
    EXPECT_FALSE(sessions.validateCsrf(s->id, "bad"));
    sessions.destroy(s->id);
    EXPECT_FALSE(sessions.touch(s->id).has_value());
}

TEST(Auth, LoginLimiter) {
    LoginLimiter limiter(2, 60, 60);
    EXPECT_TRUE(limiter.allow("ip|u"));
    limiter.failure("ip|u");
    EXPECT_TRUE(limiter.allow("ip|u"));
    limiter.failure("ip|u");
    EXPECT_FALSE(limiter.allow("ip|u"));
    limiter.success("ip|u");
    EXPECT_TRUE(limiter.allow("ip|u"));
}

TEST(Auth, BearerAndCidr) {
    EXPECT_EQ(bearerToken("Bearer abc"), "abc");
    EXPECT_EQ(bearerToken("bearer  abc "), "abc");
    EXPECT_TRUE(bearerToken("Basic abc").empty());
    EXPECT_TRUE(ipInCidr("127.0.0.1", "127.0.0.1/32"));
    EXPECT_FALSE(ipInCidr("192.168.1.1", "10.0.0.0/8"));
    EXPECT_TRUE(ipInCidr("10.1.2.3", "10.0.0.0/8"));
}

// Префикс IPv6 не игнорируется: иначе ::/0 и 2001:db8::/32 не совпадают ни с
// одним клиентом, а настройка молча закрывает доступ.
TEST(Auth, CidrMatchesIpv6Prefix) {
    EXPECT_TRUE(ipInCidr("2001:db8::1", "2001:db8::/32"));
    EXPECT_TRUE(ipInCidr("2001:db8:ffff::1", "2001:db8::/32"));
    EXPECT_FALSE(ipInCidr("2001:db9::1", "2001:db8::/32"));
    EXPECT_TRUE(ipInCidr("2001:db8::1", "::/0"));
    EXPECT_FALSE(ipInCidr("2001:db8::1", "2001:db8::/128"));
    EXPECT_TRUE(ipInCidr("2001:db8::1", "2001:db8::1"));
    EXPECT_FALSE(ipInCidr("2001:db8::1", "2001:db8::2"));
    EXPECT_FALSE(ipInCidr("2001:db8::1", "2001:db8::/129"));
}

TEST(Auth, LoopbackDetection) {
    EXPECT_TRUE(isLoopbackIp("127.0.0.1"));
    EXPECT_TRUE(isLoopbackIp("127.0.0.5"));
    EXPECT_TRUE(isLoopbackIp("::1"));
    EXPECT_TRUE(isLoopbackIp("::ffff:127.0.0.1"));
    EXPECT_FALSE(isLoopbackIp("192.168.1.215"));
    EXPECT_FALSE(isLoopbackIp("::ffff:192.168.1.215"));
    EXPECT_FALSE(isLoopbackIp("10.1.2.3"));
    EXPECT_FALSE(isLoopbackIp(""));
}

TEST(Auth, TokenAllowedNetworks) {
    TokenConfig t;
    t.id = "net";
    t.token_hash = "sha256:" + sha256Hex("net-secret");
    t.allowed_networks = {"10.0.0.0/8"};
    TokenStore store({t});
    EXPECT_FALSE(store.authenticate("Bearer net-secret", "127.0.0.1").has_value());
    EXPECT_TRUE(store.authenticate("Bearer net-secret", "10.1.2.3").has_value());
}

TEST(Auth, RejectsMalformedTokenHash) {
    TokenConfig t;
    t.id = "bad";
    t.token_hash = "sha256:not-hex";
    EXPECT_THROW(TokenStore({t}), std::runtime_error);
}

TEST(Auth, SessionsExpire) {
    SessionStore sessions(1, 10);
    auto s = sessions.create("admin", "admin");
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(sessions.touch(s->id).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(sessions.touch(s->id).has_value());
}
