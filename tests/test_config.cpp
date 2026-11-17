#include "TestHelpers.h"
#include "config/DefaultConfig.h"
#include "util/Strings.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>

using namespace proxy;

namespace {

const char* kBase = R"YAML(
proxy: { listen: "127.0.0.1:18081" }
portal: { enabled: false }
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";

std::string replaceFirst(std::string text, const std::string& from, const std::string& to) {
    const auto pos = text.find(from);
    if (pos == std::string::npos) throw std::runtime_error("test fixture: '" + from + "' not found");
    return text.replace(pos, from.size(), to);
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

AppConfig loadText(const std::string& text, test::TempDir& dir) {
    const auto path = dir.path() / "config.yaml";
    test::writeFile(path, text);
    return AppConfig::load(path.string());
}

} // namespace

// Шаблон `--init-config` — единственная документация параметров: он обязан
// разбираться и проходить валидацию, иначе описанные ключи расходятся с кодом.
TEST(Config, InitConfigTemplateIsValid) {
    test::TempDir dir;
    setenv("COSTRELAY_TEST_PANEL_HASH",
           "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", 1);
    setenv("COSTRELAY_TEST_AGENT_HASH",
           "sha256:4444444444444444444444444444444444444444444444444444444444444444", 1);
    setenv("ROUTERAI_API_KEY", "test-routerai-key", 1);
    std::string text(proxy::kDefaultConfig);
    text = replaceFirst(text, "file:/run/secrets/panel_admin_hash",
                        "env:COSTRELAY_TEST_PANEL_HASH");
    text = replaceFirst(text, "file:/run/secrets/agent_server_hash",
                        "env:COSTRELAY_TEST_AGENT_HASH");
    auto cfg = loadText(text, dir);
    EXPECT_TRUE(cfg.portal.enabled);
    EXPECT_FALSE(cfg.routers.empty());
    EXPECT_FALSE(cfg.routing.models.empty());
    EXPECT_TRUE(cfg.modelConfigured(cfg.routing.default_model));
}

TEST(Config, LoadsDevConfig) {
    // Проверяется, что поставляемый пример конфигурации читается и описывает
    // работающий набор; точные значения не фиксируются — их меняет владелец файла.
    auto cfg = AppConfig::load(std::string(COSTRELAY_SOURCE_DIR) + "/config/costrelay.dev.yaml");
    EXPECT_FALSE(cfg.routers.empty());
    EXPECT_FALSE(cfg.routing.models.empty());
    EXPECT_TRUE(cfg.portal.enabled);
    EXPECT_FALSE(cfg.panel.users.empty());
    EXPECT_FALSE(cfg.tokens.empty());
    EXPECT_TRUE(cfg.modelConfigured(cfg.resolveModel("default")));
    EXPECT_GT(cfg.server.port, 0);
}

TEST(Config, LoadsSimpleSchema) {
    auto text = R"YAML(
proxy:
  name: "simple"
  data_dir: "./state"
  listen: "127.0.0.1:18082"
  public_url: "https://llm.example.com/"
panel: { path: "/panel" }
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
  services:
    - {id: "grafana", title: "Grafana", url: "http://127.0.0.1:3000", roles: ["admin"]}
models: ["local/llama", "remote/model"]
default_model: "local/llama"
aliases: { "default": "local/llama" }
providers:
  external:
    - {id: "routerai", type: "routerai", url: "https://routerai.ru/api/v1"}
  local:
    - id: "llama"
      url: "http://127.0.0.1:8080/v1"
      models: { "local/llama": "qwen3" }
      max_concurrent: 2
consumers:
  - {id: "agent", token_hash: "sha256:4444444444444444444444444444444444444444444444444444444444444444", models: ["*"], budget_rub_day: 12.5}
analytics: { log_dir: "./data" }
)YAML";
    test::TempDir dir;
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.server.listen, "127.0.0.1");
    EXPECT_EQ(cfg.server.port, 18082);
    EXPECT_EQ(cfg.server.public_url, "https://llm.example.com");
    ASSERT_EQ(cfg.routers.size(), 2u);
    EXPECT_EQ(cfg.routers[0].kind, "routerai");
    EXPECT_EQ(cfg.routers[1].kind, "openai_compatible");
    // Тег провайдера — производное от id: правило живёт в providerTagOf.
    EXPECT_EQ(providerTagOf(cfg.routers[1]), "llama");
    EXPECT_EQ(cfg.routers[1].model_map.at("local/llama"), "qwen3");
    EXPECT_EQ(cfg.routers[1].models, std::vector<std::string>{"local/llama"});
    ASSERT_NE(cfg.priceSourceRouter(), nullptr);
    EXPECT_EQ(cfg.priceSourceRouter()->base_url, "https://routerai.ru/api/v1");
    EXPECT_EQ(cfg.pricing.snapshot_file, (dir.path() / "state" / "prices.json").string());
    EXPECT_TRUE(cfg.portal.enabled);
    ASSERT_EQ(cfg.portal.apps.size(), 1u);
    EXPECT_EQ(cfg.portal.apps[0].id, "grafana");
    ASSERT_EQ(cfg.panel.users.size(), 1u);
    EXPECT_FALSE(cfg.server.remote_https_enabled);
    ASSERT_EQ(cfg.tokens.size(), 1u);
    EXPECT_NEAR(static_cast<double>(cfg.tokens[0].day_budget_rub), 12.5, 0.0001);
    EXPECT_TRUE(cfg.analytics.enabled);
    EXPECT_EQ(cfg.analytics.provider_log_dir, (dir.path() / "data").string());
    EXPECT_EQ(cfg.analytics.snapshot_file, (dir.path() / "state" / "provider-analytics.json").string());

}

TEST(Config, SimpleSchemaParsesIpv6Listen) {
    auto text = R"YAML(
proxy: { listen: "[::1]:18082" }
portal: { enabled: false }
models: ["m/x"]
providers:
  external:
    - {id: "routerai", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    test::TempDir dir;
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.server.listen, "::1");
    EXPECT_EQ(cfg.server.port, 18082);
    EXPECT_FALSE(cfg.portal.enabled);
}

TEST(Config, PanelAllowsNetworkListenerByDefault) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: { listen: "0.0.0.0:18081" }
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.panel.allowed_networks,
              (std::vector<std::string>{"0.0.0.0/0", "::/0"}));
}

TEST(Config, SimpleSchemaTlsMakesPortalHttpsOnly) {
    test::TempDir dir;
    const auto cert = (dir.path() / "cert.pem").string();
    const auto key = (dir.path() / "key.pem").string();
    test::writeFile(cert, "cert");
    test::writeFile(key, "key");
    std::ostringstream text;
    text << R"YAML(
proxy:
  listen: "127.0.0.1:18082"
  tls:
    listen: "0.0.0.0:18443"
    cert_file: ")YAML" << cert << R"YAML("
    key_file: ")YAML" << key << R"YAML("
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
models: ["m/x"]
providers:
  external:
    - {id: "routerai", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    auto cfg = loadText(text.str(), dir);
    EXPECT_TRUE(cfg.server.remote_https_enabled);
    EXPECT_EQ(cfg.server.remote_https_port, 18443);
    EXPECT_TRUE(cfg.server.require_https);
}

TEST(Config, RequireHttpsCanBeDisabledWithTls) {
    test::TempDir dir;
    const auto cert = (dir.path() / "cert.pem").string();
    const auto key = (dir.path() / "key.pem").string();
    test::writeFile(cert, "cert");
    test::writeFile(key, "key");
    std::ostringstream text;
    text << R"YAML(
proxy:
  listen: "127.0.0.1:18082"
  require_https: false
  tls:
    listen: "0.0.0.0:18443"
    cert_file: ")YAML" << cert << R"YAML("
    key_file: ")YAML" << key << R"YAML("
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
models: ["m/x"]
providers:
  external:
    - {id: "routerai", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    auto cfg = loadText(text.str(), dir);
    EXPECT_TRUE(cfg.server.remote_https_enabled);
    EXPECT_FALSE(cfg.server.require_https);
}

TEST(Config, ResolvesProviderAndConsumerSecrets) {
    test::TempDir dir;
    setenv("COSTRELAY_TEST_API_KEY", "secret-from-env", 1);
    const auto hash_path = dir.path() / "token.hash";
    test::writeFile(hash_path,
                    "sha256:5555555555555555555555555555555555555555555555555555555555555555\n");
    std::ostringstream text;
    text << R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1", api_key: "env:COSTRELAY_TEST_API_KEY"}
consumers:
  - {id: "a", token_hash: "file:)YAML" << hash_path.string() << R"YAML("}
)YAML";
    auto cfg = loadText(text.str(), dir);
    EXPECT_EQ(cfg.routers[0].api_key, "secret-from-env");
    EXPECT_EQ(cfg.tokens[0].token_hash,
              "sha256:5555555555555555555555555555555555555555555555555555555555555555");
}

TEST(Config, RejectsMissingProviders) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsNonHttpPublicUrl) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081", public_url: "llm.example.com"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsAliasToUnknownModel) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
aliases: { "a": "m/y" }
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsDuplicateProviderIds) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://a.example/v1"}
    - {id: "r1", type: "routerai", url: "https://b.example/v1"}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsCaptureWithoutKey) {
    test::TempDir dir;
    auto text = std::string(kBase) + R"YAML(
capture: { enabled: true }
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsExternalOpenAiWithoutPrice) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "remote", type: "openai", url: "https://remote.example/v1", models: ["m/*"]}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, DefaultsModelFromList) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.routing.default_model, "m/x");
}

TEST(Config, ReadsStatsAndConsumerLimits) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers:
  - id: "limited"
    token_hash: "sha256:1111111111111111111111111111111111111111111111111111111111111111"
    rpm: 10
    tpm: 2000
    max_concurrent: 2
    budget_rub_day: 12.5
stats:
  ring_buffer_size: 50
)YAML";
    auto cfg = loadText(text, dir);
    ASSERT_EQ(cfg.tokens.size(), 1u);
    EXPECT_EQ(cfg.tokens[0].rpm, 10);
    EXPECT_EQ(cfg.tokens[0].tpm, 2000);
    EXPECT_EQ(cfg.tokens[0].max_concurrent, 2);
    EXPECT_NEAR(static_cast<double>(cfg.tokens[0].day_budget_rub), 12.5, 0.0001);
    EXPECT_EQ(cfg.stats.ring_size, 50);
}

TEST(Config, ResolvesCredentialSecretRefs) {
    test::TempDir dir;
    const auto hash_path = dir.path() / "admin.hash";
    test::writeFile(hash_path, "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA\n");
    setenv("COSTRELAY_TEST_TOKEN_HASH",
           "sha256:3333333333333333333333333333333333333333333333333333333333333333", 1);
    std::ostringstream text;
    text << R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal:
  users:
    - username: "admin"
      password_hash: "file:)YAML" << hash_path.string() << R"YAML("
      role: "admin"
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers:
  - {id: "dev", token_hash: "env:COSTRELAY_TEST_TOKEN_HASH"}
)YAML";
    auto cfg = loadText(text.str(), dir);
    ASSERT_EQ(cfg.panel.users.size(), 1u);
    EXPECT_EQ(cfg.panel.users[0].password_hash,
              "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA");
    ASSERT_EQ(cfg.tokens.size(), 1u);
    EXPECT_EQ(cfg.tokens[0].token_hash,
              "sha256:3333333333333333333333333333333333333333333333333333333333333333");
}

TEST(Config, ResolvesRawAgentTokenFromFileAndEnv) {
    test::TempDir dir;
    const auto token_path = dir.path() / "agent.token";
    test::writeFile(token_path, "raw-token-from-file\n");
    setenv("COSTRELAY_TEST_RAW_TOKEN", "raw-token-from-env", 1);
    std::ostringstream text;
    text << R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers:
  - {id: "file", token_hash: "file:)YAML" << token_path.string() << R"YAML("}
  - {id: "env", token: "env:COSTRELAY_TEST_RAW_TOKEN"}
)YAML";
    auto cfg = loadText(text.str(), dir);
    ASSERT_EQ(cfg.tokens.size(), 2u);
    EXPECT_EQ(cfg.tokens[0].token_hash, "sha256:" + sha256Hex("raw-token-from-file"));
    EXPECT_EQ(cfg.tokens[1].token_hash, "sha256:" + sha256Hex("raw-token-from-env"));
}

TEST(Config, RejectsLiteralRawToken) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers:
  - {id: "bad", token: "raw-secret"}
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsPlaintextCredentials) {
    test::TempDir dir;
    auto base = std::string(R"YAML(
proxy: {listen: "127.0.0.1:18081"}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML");
    auto password = base + R"YAML(
portal:
  users: [{username: "a", password: "secret"}]
)YAML";
    EXPECT_THROW(loadText(password, dir), ConfigError);

    auto token = std::string(R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: [{id: "dev", token: "secret"}]
)YAML");
    EXPECT_THROW(loadText(token, dir), ConfigError);
}

TEST(Config, RejectsNegativeLimits) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers:
  - {id: "bad", token_hash: "sha256:1111111111111111111111111111111111111111111111111111111111111111", rpm: -1}
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsMissingTlsFiles) {
    test::TempDir dir;
    auto text = R"YAML(
proxy:
  listen: "127.0.0.1:18081"
  tls:
    listen: "0.0.0.0:18443"
    cert_file: "/no/such/cert.pem"
    key_file: "/no/such/key.pem"
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, ParsesPricingAndProviderPriceKnobs) {
    test::TempDir dir;
    auto text = std::string(R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1", price_multiplier: 1.5, fixed_fee_rub: 0.25}
consumers: []
pricing:
  threshold_mode: "excess"
  stale_after_sec: 60
  max_stale_sec: 120
  require_fresh: true
  timeout: "3s"
  max_body_bytes: 4096
  refresh: { hot_interval: 30, max_endpoints_per_model: 7 }
stats:
  requests_retention_days: 3
  aggregates_retention_days: 30
)YAML");
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.pricing.threshold_mode, "excess");
    EXPECT_EQ(cfg.pricing.stale_after_sec, 60);
    EXPECT_EQ(cfg.pricing.max_stale_sec, 120);
    EXPECT_TRUE(cfg.pricing.require_fresh);
    EXPECT_EQ(cfg.pricing.timeout_ms, 3000);
    EXPECT_EQ(cfg.pricing.max_body_bytes, 4096u);
    EXPECT_EQ(cfg.pricing.refresh.hot_interval_sec, 30);
    EXPECT_EQ(cfg.pricing.refresh.max_endpoints_per_model, 7);
    ASSERT_EQ(cfg.routers.size(), 1u);
    EXPECT_DOUBLE_EQ(static_cast<double>(cfg.routers[0].price_multiplier), 1.5);
    EXPECT_EQ(cfg.routers[0].fixed_fee_nano, 250'000'000);
    EXPECT_EQ(cfg.stats.requests_retention_days, 3);
    EXPECT_EQ(cfg.stats.aggregates_retention_days, 30);
}

TEST(Config, ParsesPortalApps) {
    setenv("COSTRELAY_PORTAL_KEY", "portal-secret", 1);
    test::TempDir dir;
    const auto token_path = dir.path() / "portal-token";
    test::writeFile(token_path, "portal-file-secret\n");
    std::string text = std::string(R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
  services:
    - id: "grafana"
      title: "Grafana"
      url: "http://127.0.0.1:3000"
      roles: ["admin"]
      timeout: "30s"
      max_response_bytes: 1048576
      headers:
        X-Key: "env:COSTRELAY_PORTAL_KEY"
        Authorization: "Bearer file:)YAML") +
                       token_path.string() + R"YAML("
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    auto cfg = loadText(text, dir);
    EXPECT_TRUE(cfg.portal.enabled);
    ASSERT_EQ(cfg.portal.apps.size(), 1u);
    const auto& app = cfg.portal.apps[0];
    EXPECT_EQ(app.id, "grafana");
    EXPECT_EQ(app.title, "Grafana");
    EXPECT_EQ(app.url, "http://127.0.0.1:3000");
    EXPECT_EQ(app.roles, std::vector<std::string>{"admin"});
    EXPECT_TRUE(app.verify_tls);
    EXPECT_EQ(app.timeout_ms, 30000);
    EXPECT_EQ(app.max_response_bytes, 1048576u);
    EXPECT_EQ(app.headers.at("X-Key"), "portal-secret");
    EXPECT_EQ(app.headers.at("Authorization"), "Bearer portal-file-secret");
}

TEST(Config, ParsesProviderWhitelistAndMatches) {
    test::TempDir dir;
    auto text = std::string(R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "routerai", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
whitelist:
  - provider: "routerai"
    models: ["deepseek/*"]
  - provider: "local-*"
)YAML");
    auto cfg = loadText(text, dir);
    ASSERT_EQ(cfg.routing.provider_whitelist.size(), 2u);
    EXPECT_EQ(cfg.routing.provider_whitelist[0].provider, "routerai");
    EXPECT_EQ(cfg.routing.provider_whitelist[0].models,
              std::vector<std::string>{"deepseek/*"});
    EXPECT_EQ(cfg.routing.provider_whitelist[1].models, std::vector<std::string>{"*"});
    EXPECT_TRUE(providerAllowedByList(cfg.routing.provider_whitelist, "deepseek/v4", "routerai"));
    EXPECT_FALSE(providerAllowedByList(cfg.routing.provider_whitelist, "other/x", "routerai"));
    EXPECT_TRUE(providerAllowedByList(cfg.routing.provider_whitelist, "any", "local-strata"));
    EXPECT_FALSE(providerAllowedByList(cfg.routing.provider_whitelist, "any", "other"));
}

TEST(Config, SavesBlacklistBlock) {
    test::TempDir dir;
    const auto path = dir.path() / "config.yaml";
    test::writeFile(path, R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  external:
    - {id: "routerai", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
# keep this comment
blacklist:
  - {provider: "old", models: ["*"], reason: "old reason"}
# trailing section
stats: {enabled: false}
)YAML");
    std::vector<ProviderBlockConfig> entries;
    entries.push_back({"routerai", {"deepseek/*"}, "blocked by test"});
    saveProviderBlockList(path.string(), "blacklist", entries);
    const auto saved = readTextFile(path);
    EXPECT_NE(saved.find("# keep this comment"), std::string::npos);
    EXPECT_NE(saved.find("# trailing section"), std::string::npos);
    auto cfg = AppConfig::load(path.string());
    ASSERT_EQ(cfg.routing.provider_blacklist.size(), 1u);
    EXPECT_EQ(cfg.routing.provider_blacklist[0].provider, "routerai");
    EXPECT_EQ(cfg.routing.provider_blacklist[0].models,
              std::vector<std::string>{"deepseek/*"});
    EXPECT_EQ(cfg.routing.provider_blacklist[0].reason, "blocked by test");
    EXPECT_EQ(cfg.config_path, path.string());

    saveProviderBlockList(path.string(), "blacklist", {});
    const auto cleared = readTextFile(path);
    EXPECT_NE(cleared.find("blacklist: []"), std::string::npos);
}

TEST(Config, RejectsInvalidPortalApps) {
    test::TempDir dir;
    auto base = std::string(R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
  services:
)YAML");
    auto write = [&](const std::string& services) {
        test::writeFile(dir.path() / "config.yaml", base + services);
    };

    write(R"YAML(    - {id: "Bad Id", url: "http://127.0.0.1:3000"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "ok", url: "http://127.0.0.1:3000"}
    - {id: "ok", url: "http://127.0.0.1:3001"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "panel", url: "http://127.0.0.1:3000"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "v1", url: "http://127.0.0.1:3000"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "ok", url: "ftp://127.0.0.1/file"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "ok", url: "http:///path"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "ok", url: "http://user:pass@127.0.0.1:3000"}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);

    write(R"YAML(    - {id: "ok", url: "http://127.0.0.1:3000", roles: ["root"]}
)YAML");
    EXPECT_THROW(AppConfig::load((dir.path() / "config.yaml").string()), ConfigError);
}

TEST(Config, RejectsServiceConflictingWithPanelPath) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
panel: {path: "/admin/panel"}
portal:
  users:
    - {username: "admin", password_hash: "$argon2id$v=19$m=65536,t=2,p=1$c2FsdA$aGFzaA", role: "admin"}
  services:
    - {id: "admin", url: "http://127.0.0.1:3000"}
models: ["m/x"]
providers:
  external:
    - {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1"}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, RejectsDuplicateDirectProviderTags) {
    test::TempDir dir;
    auto text = R"YAML(
proxy: {listen: "127.0.0.1:18081"}
portal: {enabled: false}
models: ["m/x"]
providers:
  local:
    - {id: "dup", url: "http://127.0.0.1:8080/v1"}
    - {id: "dup", url: "http://127.0.0.1:8081/v1"}
consumers: []
)YAML";
    EXPECT_THROW(loadText(text, dir), ConfigError);
}

TEST(Config, ParsesAnalyticsSection) {
    test::TempDir dir;
    auto text = std::string(kBase) + R"YAML(
analytics:
  log_dir: "./logs"
  snapshot: "./state/analytics.json"
)YAML";
    auto cfg = loadText(text, dir);
    EXPECT_TRUE(cfg.analytics.enabled);
    EXPECT_EQ(cfg.analytics.provider_log_dir, (dir.path() / "logs").string());
    EXPECT_EQ(cfg.analytics.snapshot_file, (dir.path() / "state" / "analytics.json").string());

}

TEST(Config, ParsesProviderBlacklist) {
    test::TempDir dir;
    auto text = std::string(kBase) + R"YAML(
blacklist:
  - {provider: "novita", reason: "security"}
  - {provider: "byteplus", models: ["deepseek/*"], reason: "quantization"}
)YAML";
    auto cfg = loadText(text, dir);
    ASSERT_EQ(cfg.routing.provider_blacklist.size(), 2u);
    EXPECT_EQ(cfg.routing.provider_blacklist[0].provider, "novita");
    EXPECT_EQ(cfg.routing.provider_blacklist[1].models,
              std::vector<std::string>{"deepseek/*"});
}

TEST(Config, ParsesCaptureAnalyticsAndServerLimits) {
    test::TempDir dir;
    auto text = replaceFirst(std::string(kBase), R"(listen: "127.0.0.1:18081")",
                             R"(listen: "127.0.0.1:18081", max_body_bytes: "1m")") + R"YAML(
capture:
  enabled: true
  key: "file:./body_key"
  compression: "zstd"
  retention_days: 3
  max_total_bytes: "2g"
  max_request_bytes: "1m"
  max_response_bytes: "4m"
  max_stream_bytes: "512k"
  capture_stream: false
  capture_errors: false
  sample_rate: 0.25
analytics:
  log_dir: "./logs"
  max_file_bytes: "64m"
  max_samples_per_group: 500
)YAML";
    test::writeFile(dir.path() / "body_key", "secret");
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.server.max_body_bytes, 1024u * 1024u);
    EXPECT_EQ(cfg.capture.retention_days, 3);
    EXPECT_EQ(cfg.capture.max_total_bytes, 2ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(cfg.capture.max_request_bytes, 1024u * 1024u);
    EXPECT_EQ(cfg.capture.max_response_bytes, 4u * 1024u * 1024u);
    EXPECT_EQ(cfg.capture.max_stream_bytes, 512u * 1024u);
    EXPECT_FALSE(cfg.capture.capture_stream);
    EXPECT_FALSE(cfg.capture.capture_errors);
    EXPECT_DOUBLE_EQ(cfg.capture.sample_rate, 0.25);
    EXPECT_EQ(cfg.analytics.max_file_bytes, 64u * 1024u * 1024u);
    EXPECT_EQ(cfg.analytics.max_samples_per_group, 500);

    // Опечатка в размере — отказ, а не молчаливый дефолт.
    EXPECT_THROW(loadText(replaceFirst(std::string(kBase), R"(listen: "127.0.0.1:18081")",
                                       R"(listen: "127.0.0.1:18081", max_body_bytes: plenty)"),
                           dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "capture: { max_total_bytes: 10x }\n", dir),
                 ConfigError);
}

TEST(Config, ParsesRoutingOptions) {
    test::TempDir dir;
    auto text = std::string(kBase) + R"YAML(
routing:
  client_provider_policy: "reject"
  country: "RU"
  quantization: ["fp8", "bf16"]
  inject_provider_options: false
  provider_order_size: 2
  queue_min_sec: 120
  queue_max_sec: 240
  queue_stats_sec: 1800
  cache_loss_spike: { ratio: 2.5, window_sec: 600 }
  cache_miss: { warn: 3, blacklist: 7 }
  leader: { margin_percent: 25, min_dwell_sec: 60, confirm_builds: 3, state_file: "./leader.json" }
  session_pin_sec: 120
  session_pin_max: 64
  wait_price_rub_per_sec: 0.5
  failover_wait_max_sec: 30
  output_weight: 1.5
  cache_weight: 3.5
  candidate_cap: 48
  profile_sample_size: 30
  retry: { attempts: 2, backoff_ms: 100 }
  health_fail_threshold: 5
  health_down_after_sec: 120
  health_probe_interval_sec: 30
  health_recover_successes: 2
)YAML";
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.routing.client_provider_policy, "reject");
    EXPECT_EQ(cfg.routing.country, "RU");
    EXPECT_EQ(cfg.routing.quantization, (std::vector<std::string>{"fp8", "bf16"}));
    EXPECT_FALSE(cfg.routing.inject_provider_options);
    EXPECT_EQ(cfg.routing.provider_order_size, 2);
    EXPECT_EQ(cfg.routing.queue_min_sec, 120);
    EXPECT_EQ(cfg.routing.queue_max_sec, 240);
    EXPECT_EQ(cfg.routing.queue_stats_sec, 1800);
    EXPECT_DOUBLE_EQ(cfg.routing.cache_loss_spike.ratio, 2.5);
    EXPECT_EQ(cfg.routing.cache_loss_spike.window_sec, 600);
    EXPECT_EQ(cfg.routing.cache_miss.warn, 3);
    EXPECT_EQ(cfg.routing.cache_miss.blacklist, 7);
    EXPECT_DOUBLE_EQ(cfg.routing.leader.margin_percent, 25.0);
    EXPECT_EQ(cfg.routing.leader.min_dwell_sec, 60);
    EXPECT_EQ(cfg.routing.leader.confirm_builds, 3);
    EXPECT_FALSE(cfg.routing.leader.state_file.empty());
    EXPECT_EQ(cfg.routing.session_pin_sec, 120);
    EXPECT_EQ(cfg.routing.session_pin_max, 64);
    EXPECT_DOUBLE_EQ(cfg.routing.wait_price_rub_per_sec, 0.5);
    EXPECT_EQ(cfg.routing.failover_wait_max_sec, 30);
    EXPECT_DOUBLE_EQ(cfg.routing.output_weight, 1.5);
    EXPECT_DOUBLE_EQ(cfg.routing.cache_weight, 3.5);
    EXPECT_EQ(cfg.routing.candidate_cap, 48);
    EXPECT_EQ(cfg.routing.profile_sample_size, 30);
    EXPECT_EQ(cfg.routing.retry.attempts, 2);
    EXPECT_EQ(cfg.routing.retry.backoff_ms, 100);
    EXPECT_EQ(cfg.routing.health.fail_threshold, 5);
    EXPECT_EQ(cfg.routing.health.down_after_sec, 120);
    EXPECT_EQ(cfg.routing.health.probe_interval_sec, 30);
    EXPECT_EQ(cfg.routing.health.recover_successes, 2);

    auto defaults = loadText(std::string(kBase) + "routing: {}\n", dir);
    EXPECT_EQ(defaults.routing.client_provider_policy, "strip");
    EXPECT_TRUE(defaults.routing.inject_provider_options);
    EXPECT_EQ(defaults.routing.provider_order_size, 1);
    EXPECT_EQ(defaults.routing.queue_min_sec, 600);
    EXPECT_EQ(defaults.routing.queue_max_sec, 900);
    EXPECT_EQ(defaults.routing.queue_stats_sec, 3600);
    EXPECT_EQ(defaults.routing.cache_miss.warn, 2);
    EXPECT_EQ(defaults.routing.cache_miss.blacklist, 5);
    EXPECT_DOUBLE_EQ(defaults.routing.cache_loss_spike.ratio, 3.0);
    EXPECT_EQ(defaults.routing.cache_loss_spike.window_sec, 900);
    EXPECT_DOUBLE_EQ(defaults.routing.leader.margin_percent, 15.0);
    EXPECT_EQ(defaults.routing.leader.min_dwell_sec, 1800);
    EXPECT_EQ(defaults.routing.leader.confirm_builds, 2);
    EXPECT_EQ(defaults.routing.session_pin_sec, 1800);
    EXPECT_EQ(defaults.routing.session_pin_max, 4096);
    EXPECT_DOUBLE_EQ(defaults.routing.wait_price_rub_per_sec, 0.001);
    EXPECT_EQ(defaults.routing.failover_wait_max_sec, 60);
    EXPECT_FALSE(defaults.routing.leader.state_file.empty());
    EXPECT_DOUBLE_EQ(defaults.routing.output_weight, 1.0);
    EXPECT_DOUBLE_EQ(defaults.routing.cache_weight, 2.0);
    EXPECT_EQ(defaults.routing.candidate_cap, 32);
    EXPECT_EQ(defaults.routing.profile_sample_size, 30);
    EXPECT_EQ(defaults.routing.retry.attempts, 1);
    EXPECT_EQ(defaults.routing.retry.backoff_ms, 500);
    EXPECT_EQ(defaults.routing.health.fail_threshold, 3);
    EXPECT_EQ(defaults.routing.health.down_after_sec, 60);
    EXPECT_EQ(defaults.routing.health.probe_interval_sec, 60);
    EXPECT_EQ(defaults.routing.health.recover_successes, 1);

    auto broken = std::string(kBase) + "routing: { output_weight: 101 }\n";
    EXPECT_THROW(loadText(broken, dir), ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { provider_order_size: 0 }\n", dir),
                 ConfigError);
    // Порог чёрного списка не может быть ниже предупреждения, и оба — в границах.
    EXPECT_THROW(loadText(std::string(kBase) +
                              "routing: { cache_miss: { warn: 5, blacklist: 2 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) +
                              "routing: { cache_miss: { warn: 0, blacklist: 1 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) +
                              "routing: { cache_miss: { warn: 1, blacklist: 1001 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { cache_loss_spike: { ratio: 1.0 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { leader: { confirm_builds: 0 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { profile_sample_size: 100001 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { candidate_cap: 0 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { retry: { attempts: 11 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { retry: { backoff_ms: -1 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { health_fail_threshold: -1 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { health_down_after_sec: -1 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { health_probe_interval_sec: 0 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { health_recover_successes: 0 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { session_pin_max: 0 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { wait_price_rub_per_sec: -1 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { wait_price_rub_per_sec: 101 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { failover_wait_max_sec: -1 }\n", dir),
                 ConfigError);
    // Бюджет попыток — часть схемы: без валидации конфиг мог задать тысячи попыток.
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { fallback_attempts: 11 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { fallback_attempts: -1 }\n", dir),
                 ConfigError);
    EXPECT_EQ(loadText(std::string(kBase) + "routing: { fallback_attempts: 10 }\n", dir)
                  .routing.fallback_attempts,
              10);
}

// Опечатка в значении — отказ загрузки, а не молчаливая подмена дефолтом:
// иначе --check пропускает конфиг, который ведёт себя не так, как записан.
TEST(Config, RejectsMalformedScalarValues) {
    test::TempDir dir;
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { queue_min_sec: \"5min\" }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { queue_min_sec: abc }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { output_weight: abc }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { cache_weight: .nan }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { inject_provider_options: maybe }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "routing: { session_pin_max: many }\n", dir),
                 ConfigError);
    // Краевые допустимые значения принимаются: граница проверяется с обеих сторон.
    const auto accepted = loadText(
        std::string(kBase) +
            "routing: { output_weight: 100, cache_weight: 100, wait_price_rub_per_sec: 100, "
            "profile_sample_size: 100000, candidate_cap: 256, queue_min_sec: 1, "
            "queue_max_sec: 1 }\n",
        dir);
    EXPECT_DOUBLE_EQ(accepted.routing.output_weight, 100.0);
    EXPECT_EQ(accepted.routing.profile_sample_size, 100000);
    EXPECT_EQ(accepted.routing.candidate_cap, 256);
}

TEST(Config, RelativePathsBelongToConfigDirectory) {
    test::TempDir dir;
    auto text = std::string(kBase) + R"YAML(
analytics:
  log_dir: "../logs"
)YAML";
    text = replaceFirst(text, "proxy: { listen: \"127.0.0.1:18081\" }",
                        "proxy: { listen: \"127.0.0.1:18081\", data_dir: \"./state\" }");
    const auto path = dir.path() / "nested" / "config.yaml";
    test::writeFile(path, text);
    auto cfg = AppConfig::load(path.string());
    // A relative path must resolve against the configuration file, so the same
    // file describes the same state regardless of the process working directory.
    EXPECT_EQ(cfg.data_dir, (dir.path() / "nested" / "state").string());
    EXPECT_EQ(cfg.server.tls_cert_file, "");
    EXPECT_EQ(cfg.pricing.snapshot_file, (dir.path() / "nested" / "state" / "prices.json").string());
    EXPECT_EQ(cfg.analytics.provider_log_dir, (dir.path() / "logs").string());
}

TEST(Config, RelativePathsIgnoreWorkingDirectory) {
    test::TempDir config_dir;
    test::TempDir other_cwd;
    auto text = replaceFirst(std::string(kBase), "proxy: { listen: \"127.0.0.1:18081\" }",
                             "proxy: { listen: \"127.0.0.1:18081\", data_dir: \"./state\" }");
    const auto path = config_dir.path() / "config.yaml";
    test::writeFile(path, text);
    const auto expected = (config_dir.path() / "state").string();

    const auto saved = std::filesystem::current_path();
    std::filesystem::current_path(other_cwd.path());
    auto cfg = AppConfig::load(path.string());
    std::filesystem::current_path(saved);

    EXPECT_EQ(cfg.data_dir, expected);
    EXPECT_EQ(cfg.stats.sqlite_path, "");
}

TEST(Config, AbsolutePathsArePreserved) {
    test::TempDir dir;
    auto text = replaceFirst(std::string(kBase),
                             "proxy: { listen: \"127.0.0.1:18081\" }",
                             "proxy: { listen: \"127.0.0.1:18081\", data_dir: \"/var/lib/costrelay\" }");
    auto cfg = loadText(text, dir);
    EXPECT_EQ(cfg.data_dir, "/var/lib/costrelay");
    EXPECT_EQ(cfg.pricing.snapshot_file, "/var/lib/costrelay/prices.json");
}

TEST(Config, FileSecretsResolveAgainstConfigDirectory) {
    test::TempDir dir;
    test::writeFile(dir.path() / "secret.txt", "s3cret\n");
    auto text = replaceFirst(std::string(kBase), "consumers: []",
                             "consumers:\n  - {id: \"agent\", token_hash: \"file:./secret.txt\", models: [\"*\"]}");
    auto cfg = loadText(text, dir);
    ASSERT_EQ(cfg.tokens.size(), 1u);
    EXPECT_EQ(cfg.tokens[0].token_hash, "sha256:" + proxy::sha256Hex("s3cret"));
}

TEST(Config, SavesRoutingWeightsIntoConfig) {
    test::TempDir dir;
    const auto path = dir.path() / "config.yaml";
    test::writeFile(path, std::string(kBase) + R"YAML(
routing:
  inject_provider_options: true
  cache_weight: 2.0        # вес цены кеша
)YAML");

    saveRoutingNumber(path.string(), "output_weight", 1.5);
    saveRoutingNumber(path.string(), "cache_weight", 3.0);
    saveRoutingBool(path.string(), "inject_provider_options", false);

    const auto saved = readTextFile(path);
    EXPECT_NE(saved.find("output_weight: 1.5"), std::string::npos);
    // Комментарий-документация переживает перезапись значения.
    EXPECT_NE(saved.find("cache_weight: 3.0        # вес цены кеша"), std::string::npos);
    EXPECT_NE(saved.find("inject_provider_options: false"), std::string::npos);

    // Восстановление при старте: то, что записала панель, читается из файла.
    auto reloaded = AppConfig::load(path.string());
    EXPECT_DOUBLE_EQ(reloaded.routing.output_weight, 1.5);
    EXPECT_DOUBLE_EQ(reloaded.routing.cache_weight, 3.0);
    EXPECT_FALSE(reloaded.routing.inject_provider_options);

    saveRoutingNumber(path.string(), "cache_weight", 2.0);
    EXPECT_NE(readTextFile(path).find("cache_weight: 2.0"), std::string::npos);
    EXPECT_DOUBLE_EQ(AppConfig::load(path.string()).routing.cache_weight, 2.0);
    // Значение вне диапазона не переживает загрузку — поэтому панель
    // проверяет диапазон до записи в файл.
    saveRoutingNumber(path.string(), "output_weight", 1000.0);
    EXPECT_THROW(AppConfig::load(path.string()), ConfigError);
}

TEST(Config, RejectsWrongNodeTypeInsteadOfSilentDefault) {
    test::TempDir dir;
    // Опечатка в структуре обязана быть видимой: тип узла проверяется, а не
    // подменяется значением по умолчанию.
    EXPECT_THROW(loadText(replaceFirst(kBase, "listen: \"127.0.0.1:18081\"",
                                       "listen: \"127.0.0.1:18081\", threads: [2]"),
                          dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "\nwhitelist: { provider: r1 }\n", dir), ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "\nproxy_extra: 1\nblacklist: { provider: r1 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(replaceFirst(kBase, "consumers: []", "consumers: {}"), dir),
                 ConfigError);
}

TEST(Config, RejectsSecretLiteralsForUpstreamAndCapture) {
    test::TempDir dir;
    // Секреты — только ссылки file:/env:; литерал ключа не принимается.
    EXPECT_THROW(loadText(replaceFirst(kBase, "url: \"https://routerai.ru/api/v1\"",
                                       "url: \"https://routerai.ru/api/v1\", api_key: \"sk-live\""),
                          dir),
                 ConfigError);
    const std::string with_capture =
        "capture: { enabled: true, key: \"plain-secret\" }\n";
    EXPECT_THROW(loadText(std::string(kBase) + with_capture, dir), ConfigError);
}

TEST(Config, RejectsMalformedListenPort) {
    test::TempDir dir;
    EXPECT_THROW(loadText(replaceFirst(kBase, "127.0.0.1:18081", "127.0.0.1:abc"), dir),
                 ConfigError);
    EXPECT_THROW(loadText(replaceFirst(kBase, "127.0.0.1:18081", "[::1"), dir), ConfigError);
}

TEST(Config, PanelNetworksAreValidatedAndNeverWidenedByMistake) {
    test::TempDir dir;
    auto cfg = loadText(replaceFirst(kBase, "portal: { enabled: false }",
                                     "portal: { enabled: false }\npanel: { allowed_networks: [\"10.0.0.0/8\"] }"),
                        dir);
    ASSERT_EQ(cfg.panel.allowed_networks.size(), 1u);
    EXPECT_EQ(cfg.panel.allowed_networks[0], "10.0.0.0/8");
    EXPECT_THROW(loadText(std::string(kBase) + "\npanel: { allowed_networks: [\"10.0.0.0/33\"] }\n", dir),
                 ConfigError);
    // Неверный тип не открывает панель всей сети, а отвергается.
    EXPECT_THROW(loadText(std::string(kBase) + "\npanel: { allowed_networks: { net: \"0.0.0.0/0\" } }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "\npanel: { allowed_networks: [] }\n", dir), ConfigError);
}

TEST(Config, PanelSessionAndLoginLimitsComeFromConfiguration) {
    test::TempDir dir;
    const std::string panel =
        "\npanel: { session_absolute_ttl_sec: 3600, session_idle_ttl_sec: 600,"
        " cookie_name: \"sid\", login_attempts: 7, login_window_sec: 60, login_lockout_sec: 90 }\n";
    auto cfg = loadText(std::string(kBase) + panel, dir);
    EXPECT_EQ(cfg.panel.absolute_ttl_sec, 3600);
    EXPECT_EQ(cfg.panel.idle_ttl_sec, 600);
    EXPECT_EQ(cfg.panel.cookie_name, "sid");
    EXPECT_EQ(cfg.panel.login_attempts, 7);
    EXPECT_EQ(cfg.panel.login_window_sec, 60);
    EXPECT_EQ(cfg.panel.login_lockout_sec, 90);
    // Границы — из panel_bounds, а не из литералов по месту.
    EXPECT_THROW(loadText(std::string(kBase) + "\npanel: { session_idle_ttl_sec: 7200,"
                                   " session_absolute_ttl_sec: 3600 }\n", dir),
                 ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "\npanel: { login_attempts: 0 }\n", dir), ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "\npanel: { cookie_name: \"bad name\" }\n", dir), ConfigError);
}

TEST(Config, UpstreamTlsAndEnabledComeFromConfiguration) {
    test::TempDir dir;
    auto disabled = replaceFirst(kBase, "url: \"https://routerai.ru/api/v1\"",
                                 "url: \"https://routerai.ru/api/v1\", enabled: false");
    EXPECT_THROW(loadText(disabled, dir), ConfigError);  // единственный апстрим выключен
    auto pair = replaceFirst(kBase, "url: \"https://routerai.ru/api/v1\"",
                             "url: \"https://routerai.ru/api/v1\","
                             " client_cert_file: \"/nonexistent/cert.pem\"");
    EXPECT_THROW(loadText(pair, dir), ConfigError);  // сертификат без ключа
    auto ca = replaceFirst(kBase, "url: \"https://routerai.ru/api/v1\"",
                           "url: \"https://routerai.ru/api/v1\", ca_file: \"/nonexistent/ca.pem\"");
    EXPECT_THROW(loadText(ca, dir), ConfigError);
}

TEST(Config, PricingRefreshIsValidatedAndNested) {
    test::TempDir dir;
    auto cfg = loadText(
        std::string(kBase) + "\npricing: { refresh: { hot_interval: \"120s\", max_endpoints_per_model: 10 } }\n",
        dir);
    EXPECT_EQ(cfg.pricing.refresh.hot_interval_sec, 120);
    EXPECT_EQ(cfg.pricing.refresh.max_endpoints_per_model, 10);
    EXPECT_THROW(loadText(std::string(kBase) + "\npricing: { refresh: { hot_interval: 0 } }\n", dir),
                 ConfigError);
    EXPECT_THROW(
        loadText(std::string(kBase) + "\npricing: { refresh: { max_endpoints_per_model: 0 } }\n", dir),
        ConfigError);
}

// Запись настройки не трогает одноимённый ключ внутри вложенного блока: иначе
// сохранение из панели меняло бы не тот параметр.
TEST(Config, SaveRoutingKeyKeepsNestedBlock) {
    test::TempDir dir;
    const auto path = dir.path() / "config.yaml";
    test::writeFile(path, std::string(kBase) + R"YAML(
routing:
  session_pin_sec: 1800
  leader:
    min_dwell_sec: 999
)YAML");
    saveRoutingInt(path.string(), "session_pin_sec", 60);
    const auto saved = readTextFile(path);
    EXPECT_NE(saved.find("  session_pin_sec: 60"), std::string::npos) << saved;
    EXPECT_NE(saved.find("    min_dwell_sec: 999"), std::string::npos) << saved;
    auto reloaded = AppConfig::load(path.string());
    EXPECT_EQ(reloaded.routing.session_pin_sec, 60);
    EXPECT_EQ(reloaded.routing.leader.min_dwell_sec, 999);  // вложенный блок цел
}

// Таймауты апстрима и прайса — пороги защиты от вечного запроса: ноль в libcurl
// означает «ждать бесконечно», поэтому validate обязан его отвергнуть.
TEST(Config, RejectsZeroAndInvertedTimeouts) {
    test::TempDir dir;
    const std::string upstream = R"YAML(- {id: "r1", type: "routerai", url: "https://routerai.ru/api/v1")YAML";
    EXPECT_THROW(
        loadText(replaceFirst(std::string(kBase), upstream,
                              upstream + R"YAML(, timeouts: { connect: "0s", total: "0s" })YAML"),
                 dir),
        ConfigError);
    EXPECT_THROW(
        loadText(replaceFirst(std::string(kBase), upstream,
                              upstream + R"YAML(, timeouts: { connect: "10s", total: "5s" })YAML"),
                 dir),
        ConfigError);
    EXPECT_THROW(loadText(std::string(kBase) + "pricing: { timeout: \"0s\" }\n", dir), ConfigError);
}

// Срок действия токена — тот же разбор RFC3339, что у рантайма: иначе --check
// подтверждает конфигурацию, которую прокси потом отвергнет.
TEST(Config, ValidatesTokenExpiryLikeRuntime) {
    test::TempDir dir;
    auto base = replaceFirst(std::string(kBase), "consumers: []",
                             R"YAML(consumers:
  - {id: "a", token_hash: "sha256:4444444444444444444444444444444444444444444444444444444444444444", expires_at: "not-a-date"}
)YAML");
    EXPECT_THROW(loadText(base, dir), ConfigError);

    auto upper = replaceFirst(std::string(kBase), "consumers: []",
                              R"YAML(consumers:
  - {id: "a", token_hash: "sha256:4444444444444444444444444444444444444444444444444444444444444444", expires_at: "2030-01-02T03:04:05Z"}
)YAML");
    const auto cfg = loadText(upper, dir);
    ASSERT_EQ(cfg.tokens.size(), 1u);
    EXPECT_EQ(cfg.tokens[0].expires_at, "2030-01-02T03:04:05Z");
}

// Некорректная запись в отображении конфигурации — ошибка, а не молчаливое
// исчезновение ключа: опечатка не должна давать «правильную» конфигурацию.
TEST(Config, RejectsNonScalarMapEntries) {
    test::TempDir dir;
    EXPECT_THROW(loadText(std::string(kBase) + "aliases: { \"a\": [\"m/x\"] }\n", dir), ConfigError);
    EXPECT_THROW(
        loadText(replaceFirst(std::string(kBase), R"YAML(url: "https://routerai.ru/api/v1")YAML",
                              R"YAML(url: "https://routerai.ru/api/v1", models: { "m/x": ["y"] })YAML"),
                 dir),
        ConfigError);
}

// Верхний регистр hex — тот же хеш: нормализация живёт в одном месте и не
// зависит от того, кто сравнивает значение.
TEST(Config, AcceptsUppercaseTokenHash) {
    test::TempDir dir;
    auto text = replaceFirst(
        std::string(kBase), "consumers: []",
        R"YAML(consumers:
  - {id: "a", token_hash: "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}
)YAML");
    const auto cfg = loadText(text, dir);
    ASSERT_EQ(cfg.tokens.size(), 1u);
    EXPECT_EQ(normalizeSha256Hash(cfg.tokens[0].token_hash),
              std::string(64, 'a'));
}

