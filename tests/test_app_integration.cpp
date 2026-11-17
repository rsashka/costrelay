#include "TestHelpers.h"
#include "auth/Auth.h"
#include "app/Endpoints.h"
#include "app/PanelQuery.h"
#include "TestHttpServer.h"
#include "TestWsServer.h"
#include "stats/StatsDb.h"
#include "upstream/CurlClient.h"
#include "util/Strings.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace proxy;

namespace {

pid_t startProcess(const std::vector<std::string>& argv, const std::string& log_path) {
    pid_t pid = fork();
    if (pid == 0) {
        FILE* f = std::freopen(log_path.c_str(), "a", stdout);
        if (!f) _exit(127);
        dup2(fileno(f), STDERR_FILENO);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    return pid;
}

void stopProcess(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

// Процесс живёт ровно до конца области видимости: падение ASSERT не оставляет
// mock и costrelayd висеть на портах для следующего теста. reset() останавливает
// его раньше — например, чтобы сбросить буфер лога перед чтением файла.
struct ScopedProcess {
    pid_t pid = -1;
    ScopedProcess(const std::vector<std::string>& argv, const std::string& log_path)
        : pid(startProcess(argv, log_path)) {}
    ~ScopedProcess() { reset(); }
    ScopedProcess(const ScopedProcess&) = delete;
    ScopedProcess& operator=(const ScopedProcess&) = delete;
    operator pid_t() const { return pid; }
    void reset() {
        if (pid > 0) stopProcess(pid);
        pid = -1;
    }
};

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool waitForLog(const std::filesystem::path& path, const std::string& needle, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (readTextFile(path).find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::string csrfFromPage(const std::string& body) {
    const std::string marker = "<meta name=\"csrf-token\" content=\"";
    const auto pos = body.find(marker);
    if (pos == std::string::npos) return {};
    const auto end = body.find('"', pos + marker.size());
    if (end == std::string::npos) return {};
    return body.substr(pos + marker.size(), end - pos - marker.size());
}

bool httpGet(const std::string& url, long& status, std::string& body, CurlHttpResponse* out = nullptr) {
    CurlHttpRequest req;
    req.method = "GET";
    req.url = url;
    req.http2 = false;
    req.verify_tls = false;
    req.timeouts.total_ms = 3000;
    req.timeouts.connect_ms = 1000;
    CurlHttpResponse resp;
    CurlClient client;
    client.perform(req, resp);
    status = resp.status;
    body = resp.body;
    if (out) *out = resp;
    return true;
}

const std::string& adminPasswordHash() {
    static const std::string hash = PasswordHash::hash("admin");
    return hash;
}
const std::string& viewerPasswordHash() {
    static const std::string hash = PasswordHash::hash("viewer");
    return hash;
}
std::string tokenHash(const std::string& token) { return "sha256:" + sha256Hex(token); }


// Переменная окружения живёт ровно столько, сколько тест: падение ASSERT не
// оставляет её следующему тесту.
struct ScopedEnvVar {
    std::string name;
    ScopedEnvVar(const char* key, std::string value) : name(key) {
        setenv(key, value.c_str(), 1);
    }
    ~ScopedEnvVar() { unsetenv(name.c_str()); }
    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
};

std::string configFor(int app_port, int mock_port, const std::string& dir,
                      bool tls = false, int https_port = 0,
                      const std::string& cert = "", const std::string& key = "",
                      bool panel_enabled = true, bool panel_plaintext = true,
                      bool metrics_require_token = false, int ws_port = 0) {
    (void)panel_plaintext;
    std::ostringstream cfg;
    cfg << R"YAML(
proxy:
  name: "integration"
  data_dir: ")YAML" << dir << R"YAML("
  listen: "127.0.0.1:)YAML" << app_port << R"YAML("
  threads: 2
)YAML";
    if (tls) {
        cfg << R"YAML(  tls:
    listen: "0.0.0.0:)YAML" << (https_port > 0 ? https_port : 18443) << R"YAML("
    cert_file: ")YAML" << cert << R"YAML("
    key_file: ")YAML" << key << R"YAML("
)YAML";
    }
    cfg << R"YAML(
panel:
  path: "/panel"
  allowed_networks: ["127.0.0.1/32", "::1/128"]

portal:
  enabled: )YAML" << (panel_enabled ? "true" : "false") << R"YAML(
  users:
    - username: "admin"
      password_hash: ")YAML" << adminPasswordHash() << R"YAML("
      role: "admin"
    - username: "viewer"
      password_hash: ")YAML" << viewerPasswordHash() << R"YAML("
      role: "viewer"
)YAML";
    if (ws_port > 0) {
        cfg << R"YAML(  services:
    - id: "localws"
      url: "http://127.0.0.1:)YAML" << ws_port << R"YAML("
      roles: ["admin"]
)YAML";
    }
    cfg << R"YAML(
models: ["mock/model-a"]
default_model: "mock/model-a"
aliases: { "default": "mock/model-a" }

providers:
  external:
    - id: "mock-routerai"
      type: "routerai"
      url: "http://127.0.0.1:)YAML" << mock_port << R"YAML(/api/v1"

consumers:
  - id: "dev"
    token_hash: ")YAML" << tokenHash("dev-token") << R"YAML("
    scopes: ["proxy:invoke"]
    models: ["*"]
  - id: "limited"
    token_hash: ")YAML" << tokenHash("limited-token") << R"YAML("
    scopes: ["proxy:invoke"]
    models: ["other/model"]
  - id: "stats-only"
    token_hash: ")YAML" << tokenHash("stats-token") << R"YAML("
    scopes: ["stats:read"]
    models: ["*"]
  - id: "tight"
    token_hash: ")YAML" << tokenHash("tight-token") << R"YAML("
    scopes: ["proxy:invoke"]
    models: ["*"]
    rpm: 1
    max_concurrent: 1
  - id: "budget"
    token_hash: ")YAML" << tokenHash("budget-token") << R"YAML("
    scopes: ["proxy:invoke"]
    models: ["*"]
    budget_rub_day: 0.000001

routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0 }

stats:
  sqlite_path: ")YAML" << dir << R"YAML(/stats.db"
  ring_buffer_size: 100
  prometheus: { enabled: true, require_token: )YAML"
        << (metrics_require_token ? "true" : "false") << R"YAML( }

capture:
  enabled: true
  key: "file:)YAML" << (std::filesystem::path(dir) / "body.key").string()
        << R"YAML("
  compression: "zstd"
)YAML";
    // Секрет задаётся ссылкой на файл: литералов в конфигурации нет.
    std::ofstream(std::filesystem::path(dir) / "body.key") << "integration-body-key";
    return cfg.str();
}
} // namespace

TEST(AppIntegration, PanelPaginationLimits) {
    auto req = drogon::HttpRequest::newHttpRequest();
    // Запрошенный предел зажимается сверху; 0 поднимается до 1; мусор — отказ.
    EXPECT_EQ(*queryLimit(req, 50, 500), 50u);
    req->setParameter("limit", "7");
    EXPECT_EQ(*queryLimit(req, 50, 500), 7u);
    req->setParameter("limit", "0");
    EXPECT_EQ(*queryLimit(req, 50, 500), 1u);
    req->setParameter("limit", "500");
    EXPECT_EQ(*queryLimit(req, 50, 500), 500u);
    req->setParameter("limit", "501");
    EXPECT_EQ(*queryLimit(req, 50, 500), 500u);
    req->setParameter("limit", "1000000000");
    EXPECT_EQ(*queryLimit(req, 50, 500), 500u);
    // Непредставимое число — битое значение, а не «очень большой предел».
    req->setParameter("limit", "99999999999999999999");
    EXPECT_FALSE(queryLimit(req, 50, 500).has_value());
    req->setParameter("limit", "12abc");
    EXPECT_FALSE(queryLimit(req, 50, 500).has_value());
    req->setParameter("offset", "-1");
    EXPECT_FALSE(queryOffset(req).has_value());
    req->setParameter("offset", "5");
    EXPECT_EQ(*queryOffset(req), 5);
}

TEST(AppIntegration, EndpointUrlFormatting) {
    AppConfig cfg;
    cfg.server.listen = "127.0.0.1";
    cfg.server.port = 18081;
    cfg.portal.enabled = true;
    cfg.panel.base_path = "/panel";

    auto urls = panelUrls(cfg);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://127.0.0.1:18081/panel");

    cfg.server.listen = "0.0.0.0";
    cfg.panel.base_path = "/ops";
    urls = panelUrls(cfg);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://127.0.0.1:18081/ops");

    cfg.server.listen = "::1";
    urls = panelUrls(cfg);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://[::1]:18081/ops");

    cfg.server.listen = "::";
    urls = panelUrls(cfg);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "http://[::1]:18081/ops");

    cfg.server.listen = "127.0.0.1";
    cfg.server.remote_https_enabled = true;
    cfg.server.remote_https_port = 18443;
    cfg.server.tls_cert_file = "cert.pem";
    cfg.server.tls_key_file = "key.pem";
    // TLS-листенер включает HTTPS: панель по HTTP больше не адресуется.
    urls = panelUrls(cfg);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://127.0.0.1:18443/ops");

    // The HTTPS listener is IPv4-only (0.0.0.0), even for an IPv6 HTTP bind.
    cfg.server.listen = "::1";
    urls = panelUrls(cfg);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://127.0.0.1:18443/ops");

    cfg.portal.enabled = false;
    EXPECT_TRUE(panelUrls(cfg).empty());

    // Agent API endpoints are independent of the panel and always include HTTP.
    cfg.server.listen = "0.0.0.0";
    cfg.server.port = 18081;
    cfg.server.remote_https_enabled = false;
    auto api = agentApiUrls(cfg);
    ASSERT_EQ(api.size(), 1u);
    EXPECT_EQ(api[0], "http://127.0.0.1:18081/v1");

    cfg.server.remote_https_enabled = true;
    cfg.server.remote_https_port = 18443;
    cfg.server.tls_cert_file = "cert.pem";
    cfg.server.tls_key_file = "key.pem";
    api = agentApiUrls(cfg);
    ASSERT_EQ(api.size(), 2u);
    EXPECT_EQ(api[0], "http://127.0.0.1:18081/v1");
    EXPECT_EQ(api[1], "https://127.0.0.1:18443/v1");

    cfg.server.public_url = "https://llm.example.com";
    api = agentApiUrls(cfg);
    ASSERT_EQ(api.size(), 2u);
    EXPECT_EQ(api[0], "http://127.0.0.1:18081/v1");
    EXPECT_EQ(api[1], "https://llm.example.com/v1");

    // public_url is authoritative for the panel URL too.
    cfg.portal.enabled = true;
    cfg.panel.base_path = "/ops";
    auto panel = panelUrls(cfg);
    ASSERT_EQ(panel.size(), 1u);
    EXPECT_EQ(panel[0], "https://llm.example.com/ops");
}

TEST(AppIntegration, ProxyPanelAndSse) {
    const int app_port = 18180 + (getpid() % 100);
    const int mock_port = 19180 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    std::string cfg = configFor(app_port, mock_port, dir.str());
    test::writeFile(config_path, cfg);
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ready) {
        std::ifstream in(log);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ADD_FAILURE() << "app did not become ready\n" << contents;
        app.reset();
        mock.reset();
        return;
    }

    EXPECT_TRUE(waitForLog(log, "web UI URL: http://127.0.0.1:" +
                                    std::to_string(app_port) + "/"))
        << readTextFile(log);

    long status = 0;
    std::string body;
    httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/v1/models", status, body);
    EXPECT_EQ(status, 401);
    CurlHttpRequest models_req;
    models_req.method = "GET";
    models_req.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/models";
    models_req.http2 = false;
    models_req.verify_tls = false;
    models_req.headers = {"Authorization: Bearer dev-token"};
    models_req.timeouts.total_ms = 3000;
    CurlHttpResponse models_resp;
    CurlClient client;
    ASSERT_TRUE(client.perform(models_req, models_resp));
    EXPECT_EQ(models_resp.status, 200);
    EXPECT_NE(models_resp.body.find("mock/model-a"), std::string::npos);

    CurlHttpRequest chat;
    chat.method = "POST";
    chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    chat.http2 = false;
    chat.verify_tls = false;
    chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}],"max_completion_tokens":10})";
    chat.timeouts.total_ms = 5000;
    CurlHttpResponse chat_resp;
    ASSERT_TRUE(client.perform(chat, chat_resp));
    EXPECT_EQ(chat_resp.status, 200);
    EXPECT_NE(chat_resp.body.find("hello from fast"), std::string::npos);
    ASSERT_TRUE(chat_resp.headers.count("x-proxy-request-id"));
    const std::string request_id = chat_resp.headers.at("x-proxy-request-id");

    auto postBody = [&](const std::string& body, long& st, std::string& out) {
        CurlHttpRequest r;
        r.method = "POST";
        r.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
        r.http2 = false;
        r.verify_tls = false;
        r.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
        r.body = body;
        r.timeouts.total_ms = 3000;
        CurlHttpResponse resp;
        client.perform(r, resp);
        st = resp.status;
        out = resp.body;
    };
    long bad_status = 0;
    std::string bad_body;
    postBody("{bad json", bad_status, bad_body);
    EXPECT_EQ(bad_status, 400);
    postBody(R"({})", bad_status, bad_body);
    EXPECT_EQ(bad_status, 400);
    postBody(R"({"model":"missing/model","messages":[]})", bad_status, bad_body);
    EXPECT_EQ(bad_status, 404);
    postBody(R"({"model":"mock/model-a@provider=slow","messages":[]})", bad_status, bad_body);
    EXPECT_EQ(bad_status, 400);
    postBody(R"({"model":"default","messages":[{"role":"user","content":"hi"}],"provider":{"only":["slow"]}})", bad_status, bad_body);
    EXPECT_EQ(bad_status, 200);
    EXPECT_NE(bad_body.find("hello from fast"), std::string::npos);

    auto postWithToken = [&](const std::string& token, long& st, std::string& out) {
        CurlHttpRequest r;
        r.method = "POST";
        r.url = chat.url;
        r.http2 = false;
        r.verify_tls = false;
        r.headers = {"Authorization: Bearer " + token, "Content-Type: application/json"};
        r.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":10})";
        r.timeouts.total_ms = 3000;
        CurlHttpResponse resp;
        client.perform(r, resp);
        st = resp.status;
        out = resp.body;
    };
    long tight_status = 0;
    std::string tight_body;
    postWithToken("tight-token", tight_status, tight_body);
    EXPECT_EQ(tight_status, 200) << tight_body;
    postWithToken("tight-token", tight_status, tight_body);
    EXPECT_EQ(tight_status, 429);
    EXPECT_NE(tight_body.find("token_rpm_exceeded"), std::string::npos);
    long budget_status = 0;
    std::string budget_body;
    postWithToken("budget-token", budget_status, budget_body);
    EXPECT_EQ(budget_status, 429);
    EXPECT_NE(budget_body.find("budget_exceeded"), std::string::npos);

    CurlHttpRequest stream;
    stream.method = "POST";
    stream.url = chat.url;
    stream.http2 = false;
    stream.verify_tls = false;
    stream.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    stream.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true})";
    stream.timeouts.total_ms = 5000;
    CurlHttpResponse stream_resp;
    std::string sse;
    ASSERT_TRUE(client.perform(stream, stream_resp, [&](const char* d, std::size_t n) {
        sse.append(d, n);
        return true;
    }));
    EXPECT_NE(sse.find("hello"), std::string::npos);
    EXPECT_NE(sse.find("[DONE]"), std::string::npos);
    ASSERT_TRUE(stream_resp.headers.count("x-proxy-request-id"));
    const std::string stream_request_id = stream_resp.headers.at("x-proxy-request-id");

    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    login.timeouts.total_ms = 3000;
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    EXPECT_EQ(login_resp.status, 302);
    ASSERT_TRUE(login_resp.headers.count("set-cookie"));
    std::string cookie = login_resp.headers.at("set-cookie");
    auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    CurlHttpRequest overview;
    overview.method = "GET";
    overview.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/overview";
    overview.http2 = false;
    overview.verify_tls = false;
    overview.headers = {"Cookie: " + cookie};
    overview.timeouts.total_ms = 3000;
    CurlHttpResponse overview_resp;
    ASSERT_TRUE(client.perform(overview, overview_resp));
    EXPECT_EQ(overview_resp.status, 200);
    EXPECT_NE(overview_resp.body.find("\"requests_total\""), std::string::npos);
    EXPECT_NE(overview_resp.body.find("\"stats_db\""), std::string::npos);

    CurlHttpRequest sessions_api;
    sessions_api.method = "GET";
    sessions_api.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/sessions?limit=10";
    sessions_api.http2 = false;
    sessions_api.verify_tls = false;
    sessions_api.headers = {"Cookie: " + cookie};
    sessions_api.timeouts.total_ms = 3000;
    CurlHttpResponse sessions_resp;
    ASSERT_TRUE(client.perform(sessions_api, sessions_resp));
    EXPECT_EQ(sessions_resp.status, 200);
    EXPECT_NE(sessions_resp.body.find("\"source\":\"sqlite\""), std::string::npos);
    EXPECT_NE(sessions_resp.body.find("\"session_id\""), std::string::npos);

    CurlHttpRequest cache_dashboard;
    cache_dashboard.method = "GET";
    cache_dashboard.url = "http://127.0.0.1:" + std::to_string(app_port) + "/";
    cache_dashboard.http2 = false;
    cache_dashboard.verify_tls = false;
    cache_dashboard.headers = {"Cookie: " + cookie};
    cache_dashboard.timeouts.total_ms = 3000;
    CurlHttpResponse cache_dashboard_resp;
    ASSERT_TRUE(client.perform(cache_dashboard, cache_dashboard_resp));
    EXPECT_EQ(cache_dashboard_resp.status, 200);
    EXPECT_NE(cache_dashboard_resp.body.find("Cache hit"), std::string::npos);
    EXPECT_NE(cache_dashboard_resp.body.find("Сессии"), std::string::npos);

    long metrics_status = 0;
    std::string metrics_body;
    httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/metrics", metrics_status, metrics_body);
    EXPECT_EQ(metrics_status, 200);
    EXPECT_NE(metrics_body.find("costrelay_requests_total"), std::string::npos);
    EXPECT_NE(metrics_body.find("costrelay_stats_db_available 1"), std::string::npos);

    CurlHttpRequest stats_api;
    stats_api.method = "GET";
    stats_api.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/stats/daily?days=7";
    stats_api.http2 = false;
    stats_api.verify_tls = false;
    stats_api.headers = {"Cookie: " + cookie};
    stats_api.timeouts.total_ms = 3000;
    CurlHttpResponse stats_api_resp;
    ASSERT_TRUE(client.perform(stats_api, stats_api_resp));
    EXPECT_EQ(stats_api_resp.status, 200);
    EXPECT_NE(stats_api_resp.body.find("\"enabled\":true"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "stats.db"));

    CurlHttpRequest body_req;
    body_req.method = "GET";
    body_req.url = "http://127.0.0.1:" + std::to_string(app_port) +
                   "/panel/api/request-body?request_id=" + request_id + "&kind=request";
    body_req.http2 = false;
    body_req.verify_tls = false;
    body_req.headers = {"Cookie: " + cookie};
    body_req.timeouts.total_ms = 3000;
    CurlHttpResponse body_resp;
    ASSERT_TRUE(client.perform(body_req, body_resp));
    EXPECT_EQ(body_resp.status, 200);
    EXPECT_NE(body_resp.body.find("\"hi\""), std::string::npos);

    CurlHttpRequest sse_body;
    sse_body.method = "GET";
    sse_body.url = "http://127.0.0.1:" + std::to_string(app_port) +
                   "/panel/api/request-body?request_id=" + stream_request_id + "&kind=sse";
    sse_body.http2 = false;
    sse_body.verify_tls = false;
    sse_body.headers = {"Cookie: " + cookie};
    CurlHttpResponse sse_body_resp;
    ASSERT_TRUE(client.perform(sse_body, sse_body_resp));
    EXPECT_EQ(sse_body_resp.status, 200);
    EXPECT_NE(sse_body_resp.body.find("data:"), std::string::npos);
    EXPECT_NE(sse_body_resp.body.find("[DONE]"), std::string::npos);

    // Unauthorized panel API and bad login.
    CurlHttpRequest panel_anon;
    panel_anon.method = "GET";
    panel_anon.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/overview";
    panel_anon.http2 = false;
    panel_anon.verify_tls = false;
    CurlHttpResponse panel_anon_resp;
    client.perform(panel_anon, panel_anon_resp);
    EXPECT_EQ(panel_anon_resp.status, 401);

    CurlHttpRequest bad_login;
    bad_login.method = "POST";
    bad_login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    bad_login.http2 = false;
    bad_login.verify_tls = false;
    bad_login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    bad_login.body = "username=admin&password=wrong";
    CurlHttpResponse bad_login_resp;
    client.perform(bad_login, bad_login_resp);
    EXPECT_EQ(bad_login_resp.status, 401);

    // Model ACL and scope checks on the API token.
    auto postChat = [&](const std::string& token, long& st, std::string& out) {
        CurlHttpRequest r;
        r.method = "POST";
        r.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
        r.http2 = false;
        r.verify_tls = false;
        r.headers = {"Authorization: Bearer " + token, "Content-Type: application/json"};
        r.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}]})";
        r.timeouts.total_ms = 3000;
        CurlHttpResponse resp;
        client.perform(r, resp);
        st = resp.status;
        out = resp.body;
    };
    long limited_status = 0;
    std::string limited_body;
    postChat("limited-token", limited_status, limited_body);
    EXPECT_EQ(limited_status, 403);
    long scope_status = 0;
    std::string scope_body;
    postChat("stats-token", scope_status, scope_body);
    EXPECT_EQ(scope_status, 403);

    // Viewer cannot read captured bodies.
    CurlHttpRequest viewer_login;
    viewer_login.method = "POST";
    viewer_login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    viewer_login.http2 = false;
    viewer_login.verify_tls = false;
    viewer_login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    viewer_login.body = "username=viewer&password=viewer";
    CurlHttpResponse viewer_login_resp;
    ASSERT_TRUE(client.perform(viewer_login, viewer_login_resp));
    ASSERT_TRUE(viewer_login_resp.headers.count("set-cookie"));
    std::string viewer_cookie = viewer_login_resp.headers.at("set-cookie");
    auto vsemi = viewer_cookie.find(';');
    if (vsemi != std::string::npos) viewer_cookie = viewer_cookie.substr(0, vsemi);
    CurlHttpRequest viewer_body;
    viewer_body.method = "GET";
    viewer_body.url = "http://127.0.0.1:" + std::to_string(app_port) +
                      "/panel/api/request-body?request_id=" + request_id + "&kind=request";
    viewer_body.http2 = false;
    viewer_body.verify_tls = false;
    viewer_body.headers = {"Cookie: " + viewer_cookie};
    CurlHttpResponse viewer_body_resp;
    client.perform(viewer_body, viewer_body_resp);
    EXPECT_EQ(viewer_body_resp.status, 403);

    // Panel diagnostics endpoints work for admin.
    for (const char* path : {"/panel/api/routers", "/panel/api/requests", "/panel/api/explain?model=mock/model-a"}) {
        CurlHttpRequest r;
        r.method = "GET";
        r.url = "http://127.0.0.1:" + std::to_string(app_port) + path;
        r.http2 = false;
        r.verify_tls = false;
        r.headers = {"Cookie: " + cookie};
        CurlHttpResponse rr;
        client.perform(r, rr);
        EXPECT_EQ(rr.status, 200) << path;
    }

    // Logout requires CSRF, then invalidates the session.
    CurlHttpRequest logout_no_csrf;
    logout_no_csrf.method = "POST";
    logout_no_csrf.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/logout";
    logout_no_csrf.http2 = false;
    logout_no_csrf.verify_tls = false;
    logout_no_csrf.headers = {"Cookie: " + cookie};
    CurlHttpResponse logout_no_csrf_resp;
    client.perform(logout_no_csrf, logout_no_csrf_resp);
    EXPECT_EQ(logout_no_csrf_resp.status, 403);

    CurlHttpRequest dashboard;
    dashboard.method = "GET";
    dashboard.url = "http://127.0.0.1:" + std::to_string(app_port) + "/";
    dashboard.http2 = false;
    dashboard.verify_tls = false;
    dashboard.headers = {"Cookie: " + cookie};
    CurlHttpResponse dashboard_resp;
    ASSERT_TRUE(client.perform(dashboard, dashboard_resp));
    ASSERT_EQ(dashboard_resp.status, 200);
    EXPECT_NE(dashboard_resp.body.find("<h2>Точки подключения агентов</h2>"),
              std::string::npos);
    EXPECT_NE(dashboard_resp.body.find("http://127.0.0.1:" + std::to_string(app_port) + "/v1"),
              std::string::npos);
    EXPECT_NE(dashboard_resp.body.find("rpm 1 · параллельно 1"), std::string::npos);
    EXPECT_NE(dashboard_resp.body.find("0.000001 ₽"), std::string::npos);
    const std::string csrf = csrfFromPage(dashboard_resp.body);
    ASSERT_FALSE(csrf.empty());

    CurlHttpRequest logout;
    logout.method = "POST";
    logout.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/logout";
    logout.http2 = false;
    logout.verify_tls = false;
    logout.headers = {"Cookie: " + cookie, "Content-Type: application/x-www-form-urlencoded"};
    logout.body = "csrf=" + csrf;
    CurlHttpResponse logout_resp;
    ASSERT_TRUE(client.perform(logout, logout_resp));
    EXPECT_EQ(logout_resp.status, 302);

    CurlHttpRequest after_logout;
    after_logout.method = "GET";
    after_logout.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/overview";
    after_logout.http2 = false;
    after_logout.verify_tls = false;
    after_logout.headers = {"Cookie: " + cookie};
    CurlHttpResponse after_logout_resp;
    client.perform(after_logout, after_logout_resp);
    EXPECT_EQ(after_logout_resp.status, 401);

    app.reset();
    mock.reset();
}

TEST(AppIntegration, RetriesSameProviderAfterRetryableError) {
    const int app_port = 18286 + (getpid() % 100);
    const int mock_port = 19286 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    // Повтор у того же провайдера включён: первая попытка получает 500 от мока.
    std::string cfg_text = configFor(app_port, mock_port, dir.str());
    const std::string base_routing = "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0 }";
    ASSERT_NE(cfg_text.find(base_routing), std::string::npos);
    cfg_text.replace(cfg_text.find(base_routing), base_routing.size(),
                     "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0, retry: { attempts: 1, backoff_ms: 10 } }");
    test::writeFile(config_path, cfg_text);
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedEnvVar flaky_guard("MOCK_FLAKY_PROVIDERS", "fast:1");
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    // Стрим: первая попытка у fast получает 500, повтор у того же провайдера
    // отдаёт поток — заголовки ещё не ушли, поэтому повтор допустим.
    CurlHttpRequest stream_chat;
    stream_chat.method = "POST";
    stream_chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    stream_chat.http2 = false;
    stream_chat.verify_tls = false;
    stream_chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    stream_chat.body =
        R"({"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true})";
    stream_chat.timeouts.total_ms = 5000;
    CurlHttpResponse stream_resp;
    ASSERT_TRUE(client.perform(stream_chat, stream_resp,
                               [&](const char* data, std::size_t len) {
                                   stream_resp.body.append(data, len);
                                   return true;
                               }));
    EXPECT_EQ(stream_resp.status, 200);
    EXPECT_NE(stream_resp.body.find("from fast"), std::string::npos);

    CurlHttpRequest chat;
    chat.method = "POST";
    chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    chat.http2 = false;
    chat.verify_tls = false;
    chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}]})";
    chat.timeouts.total_ms = 5000;
    CurlHttpResponse chat_resp;
    ASSERT_TRUE(client.perform(chat, chat_resp));
    EXPECT_EQ(chat_resp.status, 200);
    // Тот же провайдер ответил со второй попытки, без смены маршрута.
    EXPECT_NE(chat_resp.body.find("hello from fast"), std::string::npos);
    EXPECT_EQ(chat_resp.headers.count("x-proxy-provider"), 1u);

    // Журнал: попыток 2 и запись об ошибке провайдера и повторе.
    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    ASSERT_TRUE(login_resp.headers.count("set-cookie"));
    std::string cookie = login_resp.headers.at("set-cookie");
    const auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    CurlHttpRequest logs;
    logs.method = "GET";
    logs.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/requests?limit=5";
    logs.http2 = false;
    logs.verify_tls = false;
    logs.headers = {"Cookie: " + cookie};
    CurlHttpResponse logs_resp;
    ASSERT_TRUE(client.perform(logs, logs_resp));
    ASSERT_EQ(logs_resp.status, 200);
    EXPECT_NE(logs_resp.body.find("\"attempts\":2"), std::string::npos);
    // event_detail — вложенный JSON, поэтому кавычки экранированы.
    EXPECT_NE(logs_resp.body.find("retries"), std::string::npos);
    EXPECT_NE(logs_resp.body.find("\\\"provider\\\":\\\"fast\\\""), std::string::npos);
    EXPECT_NE(logs_resp.body.find("\\\"status\\\":500"), std::string::npos);
    EXPECT_NE(logs_resp.body.find("\\\"attempt\\\":1"), std::string::npos);

    // Панель показывает повтор в журнале логов.
    CurlHttpRequest stats;
    stats.method = "GET";
    stats.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/stats";
    stats.http2 = false;
    stats.verify_tls = false;
    stats.headers = {"Cookie: " + cookie};
    stats.timeouts.total_ms = 3000;
    CurlHttpResponse stats_resp;
    ASSERT_TRUE(client.perform(stats, stats_resp));
    ASSERT_EQ(stats_resp.status, 200);
    EXPECT_NE(stats_resp.body.find("отказ: fast 500 (попытка 1)"), std::string::npos);

    app.reset();
    mock.reset();
}

TEST(AppIntegration, FallsBackToNextProvider) {
    const int app_port = 18280 + (getpid() % 100);
    const int mock_port = 19280 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path, configFor(app_port, mock_port, dir.str()));
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedEnvVar fail_guard("MOCK_FAIL_PROVIDERS", "fast");
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    auto chat = [&](bool stream, std::string& out) {
        CurlHttpRequest req;
        req.method = "POST";
        req.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
        req.http2 = false;
        req.verify_tls = false;
        req.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
        req.body = stream
                       ? R"({"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true})"
                       : R"({"model":"default","messages":[{"role":"user","content":"hi"}]})";
        req.timeouts.total_ms = 5000;
        CurlHttpResponse resp;
        if (stream) {
            EXPECT_TRUE(client.perform(req, resp, [&](const char* d, std::size_t n) {
                out.append(d, n);
                return true;
            }));
        } else {
            EXPECT_TRUE(client.perform(req, resp));
            out = resp.body;
        }
    };

    std::string nonstream;
    chat(false, nonstream);
    EXPECT_NE(nonstream.find("hello from slow"), std::string::npos);

    std::string sse;
    chat(true, sse);
    EXPECT_NE(sse.find("hello"), std::string::npos) << "SSE: " << sse;
    EXPECT_NE(sse.find("from slow"), std::string::npos) << "SSE: " << sse;
    EXPECT_NE(sse.find("[DONE]"), std::string::npos) << "SSE: " << sse;

    app.reset();
    mock.reset();

    // Перепривязка сессии к другому провайдеру записана в лог процесса: смена
    // провайдера больше не проходит молча.
    std::ifstream log_in(log.string());
    const std::string log_text((std::istreambuf_iterator<char>(log_in)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(log_text.find("session rebind on provider switch"), std::string::npos);

    // Состояние лидера переживает перезапуск: файл создан при первой сборке очереди.
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "routing-leader.json"));

    // Журнал записал событие перепривязки: причина смены видна в статистике.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2((dir.path() / "stats.db").string().c_str(), &db,
                              SQLITE_OPEN_READONLY, nullptr),
              SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
                                 "SELECT provider_tag, severity, event_detail FROM requests"
                                 " WHERE error_code='provider_affinity_rebind'",
                                 -1, &st, nullptr),
              SQLITE_OK);
    bool rebind_row = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        rebind_row = true;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "slow");
        EXPECT_EQ(sqlite3_column_int(st, 1), 1);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    EXPECT_TRUE(rebind_row);
}

TEST(AppIntegration, HttpsPanelOnly) {
    const int app_port = 18380 + (getpid() % 100);
    const int https_port = 18480 + (getpid() % 100);
    const int mock_port = 19380 + (getpid() % 100);
    test::TempDir dir;
    const auto cert = (dir.path() / "cert.pem").string();
    const auto key = (dir.path() / "key.pem").string();
    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout \"" + key + "\" -out \"" + cert +
        "\" -days 1 -subj \"/CN=localhost\" >/dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path, configFor(app_port, mock_port, dir.str(), true, https_port, cert, key, true, false));
    const auto log = dir.path() / "process.log";
    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    CurlClient client;
    auto get = [&](const std::string& url, long& status, std::string& body) {
        CurlHttpRequest req;
        req.method = "GET";
        req.url = url;
        req.http2 = false;
        req.verify_tls = false;
        req.timeouts.total_ms = 3000;
        req.timeouts.connect_ms = 1000;
        CurlHttpResponse resp;
        client.perform(req, resp);
        status = resp.status;
        body = resp.body;
    };

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        get("https://127.0.0.1:" + std::to_string(https_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ready) {
        std::ifstream in(log);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ADD_FAILURE() << "HTTPS app did not become ready\n" << contents;
        app.reset();
        mock.reset();
        return;
    }

    EXPECT_TRUE(waitForLog(log, "web UI URL: https://127.0.0.1:" +
                                    std::to_string(https_port) + "/"))
        << readTextFile(log);
    EXPECT_EQ(readTextFile(log).find("web UI URL: http://"), std::string::npos);

    long status = 0;
    std::string body;
    get("http://127.0.0.1:" + std::to_string(app_port) + "/healthz", status, body);
    EXPECT_EQ(status, 200);
    get("http://127.0.0.1:" + std::to_string(app_port) + "/panel/login", status, body);
    EXPECT_EQ(status, 403);

    // Local agents always use plain HTTP: the loopback API is not gated on TLS.
    get("http://127.0.0.1:" + std::to_string(app_port) + "/v1/models", status, body);
    EXPECT_EQ(status, 401);
    EXPECT_EQ(body.find("insecure_api"), std::string::npos);

    CurlHttpRequest http_chat;
    http_chat.method = "POST";
    http_chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    http_chat.http2 = false;
    http_chat.verify_tls = false;
    http_chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    http_chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}]})";
    http_chat.timeouts.total_ms = 5000;
    CurlHttpResponse http_chat_resp;
    ASSERT_TRUE(client.perform(http_chat, http_chat_resp));
    EXPECT_EQ(http_chat_resp.status, 200) << http_chat_resp.body;
    EXPECT_NE(http_chat_resp.body.find("hello from fast"), std::string::npos);

    get("https://127.0.0.1:" + std::to_string(https_port) + "/panel/login", status, body);
    EXPECT_EQ(status, 200);
    EXPECT_NE(body.find("CostRelay"), std::string::npos);

    CurlHttpRequest login;
    login.method = "POST";
    login.url = "https://127.0.0.1:" + std::to_string(https_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    login.timeouts.total_ms = 3000;
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    EXPECT_EQ(login_resp.status, 302);
    ASSERT_TRUE(login_resp.headers.count("set-cookie"));
    std::string cookie = login_resp.headers.at("set-cookie");
    auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    CurlHttpRequest https_chat;
    https_chat.method = "POST";
    https_chat.url = "https://127.0.0.1:" + std::to_string(https_port) + "/v1/chat/completions";
    https_chat.http2 = false;
    https_chat.verify_tls = false;
    https_chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    https_chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}]})";
    https_chat.timeouts.total_ms = 5000;
    CurlHttpResponse https_chat_resp;
    ASSERT_TRUE(client.perform(https_chat, https_chat_resp));
    EXPECT_EQ(https_chat_resp.status, 200);
    EXPECT_NE(https_chat_resp.body.find("hello from fast"), std::string::npos);

    CurlHttpRequest overview;
    overview.method = "GET";
    overview.url = "https://127.0.0.1:" + std::to_string(https_port) + "/panel/api/overview";
    overview.http2 = false;
    overview.verify_tls = false;
    overview.headers = {"Cookie: " + cookie};
    overview.timeouts.total_ms = 3000;
    CurlHttpResponse overview_resp;
    ASSERT_TRUE(client.perform(overview, overview_resp));
    EXPECT_EQ(overview_resp.status, 200);

    app.reset();
    mock.reset();
}

TEST(AppIntegration, PortalListsAndProxiesLocalApps) {
    const int app_port = 18680 + (getpid() % 100);
    const int https_port = 18780 + (getpid() % 100);
    test::TempDir dir;
    const auto cert = (dir.path() / "cert.pem").string();
    const auto key = (dir.path() / "key.pem").string();
    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout \"" + key + "\" -out \"" + cert +
        "\" -days 1 -subj \"/CN=localhost\" >/dev/null 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0);

    auto seen = std::make_shared<std::vector<std::string>>();
    auto seen_mutex = std::make_shared<std::mutex>();
    auto local_port = std::make_shared<uint16_t>(0);
    test::TestHttpServer local([&, local_port](const std::string& req) {
        {
            std::lock_guard<std::mutex> lock(*seen_mutex);
            seen->push_back(req);
        }
        if (req.rfind("POST ", 0) == 0) {
            const std::string body = "data: hello\n\n";
            std::ostringstream os;
            os << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: text/event-stream\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << body;
            return os.str();
        }
        const std::string body = "<html><body>local app</body></html>";
        std::ostringstream os;
        os << "HTTP/1.1 302 Found\r\n"
           << "Location: http://127.0.0.1:" << *local_port << "/base/landing?x=1\r\n"
           << "Set-Cookie: app_session=abc; Path=/; HttpOnly\r\n"
           << "Set-Cookie: sub=def; Path=/sub; Domain=example.com\r\n"
           << "Content-Type: text/html; charset=utf-8\r\n"
           << "X-Upstream: yes\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n"
           << body;
        return os.str();
    });
    *local_port = local.port();
    const auto portal_token = dir.path() / "portal-token";
    test::writeFile(portal_token, "portal-upstream-token\n");

    std::ostringstream cfg;
    cfg << R"YAML(
proxy:
  name: "portal-integration"
  data_dir: ")YAML" << dir.str() << R"YAML("
  listen: "127.0.0.1:)YAML" << app_port << R"YAML("
  threads: 2
  tls:
    listen: "0.0.0.0:)YAML" << https_port << R"YAML("
    cert_file: ")YAML" << cert << R"YAML("
    key_file: ")YAML" << key << R"YAML("

panel:
  allowed_networks: ["127.0.0.1/32", "::1/128"]

portal:
  users:
    - username: "admin"
      password_hash: ")YAML" << adminPasswordHash() << R"YAML("
      role: "admin"
    - username: "viewer"
      password_hash: ")YAML" << viewerPasswordHash() << R"YAML("
      role: "viewer"
  services:
    - id: "localapp"
      title: "Local App"
      url: "http://127.0.0.1:)YAML" << local.port() << R"YAML(/base"
      roles: ["admin"]
      timeout: "10s"
      headers:
        Authorization: "Bearer file:)YAML" << portal_token.string() << R"YAML("

models: ["local/model"]
providers:
  local:
    - id: "dummy"
      url: "http://127.0.0.1:1/v1"
      models: ["local/*"]
consumers: []
stats: { enabled: false }
)YAML";
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path, cfg.str());
    const auto log = dir.path() / "process.log";
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    CurlClient client;
    auto get = [&](const std::string& url, const std::vector<std::string>& headers,
                   long& status, std::string& body, CurlHttpResponse* out = nullptr) {
        CurlHttpRequest req;
        req.method = "GET";
        req.url = url;
        req.http2 = false;
        req.verify_tls = false;
        req.headers = headers;
        req.timeouts.total_ms = 5000;
        req.timeouts.connect_ms = 1000;
        CurlHttpResponse resp;
        client.perform(req, resp);
        status = resp.status;
        body = resp.body;
        if (out) *out = resp;
    };

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        get("https://127.0.0.1:" + std::to_string(https_port) + "/readyz", {}, status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready) << readTextFile(log);
    auto login = [&](const std::string& user, const std::string& password) {
        CurlHttpRequest req;
        req.method = "POST";
        req.url = "https://127.0.0.1:" + std::to_string(https_port) + "/panel/login";
        req.http2 = false;
        req.verify_tls = false;
        req.headers = {"Content-Type: application/x-www-form-urlencoded"};
        req.body = "username=" + user + "&password=" + password;
        req.timeouts.total_ms = 5000;
        CurlHttpResponse resp;
        client.perform(req, resp);
        EXPECT_EQ(resp.status, 302);
        EXPECT_EQ(resp.headers.count("location") ? resp.headers.at("location") : "",
                  "/");
        std::string cookie = resp.headers.count("set-cookie") ? resp.headers.at("set-cookie") : "";
        const auto semi = cookie.find(';');
        if (semi != std::string::npos) cookie = cookie.substr(0, semi);
        return cookie;
    };

    const std::string admin_cookie = login("admin", "admin");
    ASSERT_FALSE(admin_cookie.empty());

    long status = 0;
    std::string body;
    CurlHttpResponse root;
    get("https://127.0.0.1:" + std::to_string(https_port) + "/",
        {"Cookie: " + admin_cookie, "Accept: text/html"}, status, body, &root);
    EXPECT_EQ(status, 200);
    EXPECT_NE(body.find("Local App"), std::string::npos);
    EXPECT_NE(body.find("/localapp/"), std::string::npos);
    EXPECT_NE(body.find(">Главная<"), std::string::npos);
    EXPECT_EQ(body.find(">Панель<"), std::string::npos);
    EXPECT_NE(body.find("Провайдеры и цены"), std::string::npos);
    EXPECT_EQ(body.find("Последние запросы"), std::string::npos);
    EXPECT_EQ(body.find("id=\"session-log-modal\""), std::string::npos);

    CurlHttpResponse panel;
    get("https://127.0.0.1:" + std::to_string(https_port) + "/panel",
        {"Cookie: " + admin_cookie, "Accept: text/html"}, status, body, &panel);
    EXPECT_EQ(status, 404);

    // The bare app path is redirected to the slash-terminated prefix.
    CurlHttpResponse bare;
    get("https://127.0.0.1:" + std::to_string(https_port) + "/localapp?x=1",
        {"Cookie: " + admin_cookie, "Accept: text/html"}, status, body, &bare);
    EXPECT_EQ(status, 302);
    EXPECT_EQ(bare.headers.count("location") ? bare.headers.at("location") : "",
              "/localapp/?x=1");

    // HTTP is upgraded to the HTTPS listener before authentication.
    CurlHttpResponse upgrade;
    get("http://127.0.0.1:" + std::to_string(app_port) + "/localapp/",
        {"Accept: text/html"}, status, body, &upgrade);
    EXPECT_EQ(status, 308);
    EXPECT_EQ(upgrade.headers.count("location") ? upgrade.headers.at("location") : "",
              "https://127.0.0.1:" + std::to_string(https_port) + "/localapp/");

    // A browser navigation without a session goes to the panel login.
    CurlHttpResponse anonymous;
    get("https://127.0.0.1:" + std::to_string(https_port) + "/localapp/",
        {"Accept: text/html"}, status, body, &anonymous);
    EXPECT_EQ(status, 302);
    EXPECT_EQ(anonymous.headers.count("location") ? anonymous.headers.at("location") : "",
              "/panel/login");

    // A viewer cannot open an admin-only application.
    const std::string viewer_cookie = login("viewer", "viewer");
    ASSERT_FALSE(viewer_cookie.empty());
    get("https://127.0.0.1:" + std::to_string(https_port) + "/localapp/",
        {"Cookie: " + viewer_cookie, "Accept: text/html"}, status, body);
    EXPECT_EQ(status, 403);
    get("https://127.0.0.1:" + std::to_string(https_port) + "/",
        {"Cookie: " + viewer_cookie, "Accept: text/html"}, status, body);
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body.find("Local App"), std::string::npos);
    EXPECT_EQ(body.find(">Панель<"), std::string::npos);

    // The first real upstream request exercises header filtering and rewrites.
    CurlHttpResponse proxied;
    get("https://127.0.0.1:" + std::to_string(https_port) + "/localapp/sub/page?x=1",
        {"Cookie: " + admin_cookie + "; appclient=1", "Accept: text/html",
         "Authorization: Bearer must-not-leak",
         "Origin: https://127.0.0.1:" + std::to_string(https_port),
         "Referer: https://127.0.0.1:" + std::to_string(https_port) +
             "/localapp/sub/page"}, status, body, &proxied);
    EXPECT_EQ(status, 302);
    EXPECT_NE(body.find("local app"), std::string::npos);
    EXPECT_EQ(proxied.headers.count("location") ? proxied.headers.at("location") : "",
              "/localapp/base/landing?x=1");
    EXPECT_EQ(proxied.headers.count("x-upstream") ? proxied.headers.at("x-upstream") : "", "yes");
    bool cookie_path_ok = false;
    bool sub_cookie_path_ok = false;
    bool cookie_domain_removed = true;
    int set_cookie_count = 0;
    for (const auto& line : proxied.header_lines) {
        if (line.rfind("Set-Cookie:", 0) == 0 || line.rfind("set-cookie:", 0) == 0) {
            ++set_cookie_count;
            if (line.find("app_session=abc") != std::string::npos &&
                line.find("Path=/localapp;") != std::string::npos) cookie_path_ok = true;
            if (line.find("sub=def") != std::string::npos &&
                line.find("Path=/localapp/sub;") != std::string::npos) {
                sub_cookie_path_ok = true;
            }
            if (line.find("Domain=") != std::string::npos) cookie_domain_removed = false;
        }
    }
    std::ostringstream header_dump;
    for (const auto& line : proxied.header_lines) header_dump << line << "\n";
    EXPECT_EQ(set_cookie_count, 2);
    int content_type_count = 0;
    for (const auto& line : proxied.header_lines) {
        if (line.rfind("Content-Type:", 0) == 0 || line.rfind("content-type:", 0) == 0) {
            ++content_type_count;
        }
    }
    EXPECT_EQ(content_type_count, 1);
    EXPECT_TRUE(cookie_path_ok) << header_dump.str();
    EXPECT_TRUE(sub_cookie_path_ok) << header_dump.str();
    EXPECT_TRUE(cookie_domain_removed) << header_dump.str();
    std::string upstream_request;
    {
        std::lock_guard<std::mutex> lock(*seen_mutex);
        ASSERT_EQ(seen->size(), 1u);
        upstream_request = seen->at(0);
    }
    EXPECT_NE(upstream_request.find("GET /base/sub/page?x=1 HTTP/1.1"), std::string::npos);
    EXPECT_NE(upstream_request.find("X-Forwarded-Prefix: /localapp"), std::string::npos);
    EXPECT_NE(upstream_request.find("X-Forwarded-Proto: https"), std::string::npos);
    EXPECT_NE(upstream_request.find("Authorization: Bearer portal-upstream-token"),
              std::string::npos);
    EXPECT_EQ(upstream_request.find("proxy_session"), std::string::npos);
    EXPECT_EQ(upstream_request.find("must-not-leak"), std::string::npos);
    EXPECT_NE(upstream_request.find("Origin: http://127.0.0.1:" +
                                    std::to_string(local.port())),
              std::string::npos);
    EXPECT_NE(upstream_request.find("Referer: http://127.0.0.1:" +
                                    std::to_string(local.port()) + "/base/sub/page"),
              std::string::npos) << upstream_request;
    EXPECT_NE(upstream_request.find("appclient=1"), std::string::npos) << upstream_request;

    // POST bodies and streamed responses pass through unchanged.
    CurlHttpRequest post;
    post.method = "POST";
    post.url = "https://127.0.0.1:" + std::to_string(https_port) + "/localapp/form?x=2";
    post.http2 = false;
    post.verify_tls = false;
    post.headers = {"Cookie: " + admin_cookie + "; appclient=1",
                    "Content-Type: application/x-www-form-urlencoded"};
    post.body = "hello=1";
    post.timeouts.total_ms = 5000;
    CurlHttpResponse streamed;
    ASSERT_TRUE(client.perform(post, streamed));
    EXPECT_EQ(streamed.status, 200);
    EXPECT_EQ(streamed.headers.count("content-type") ? streamed.headers.at("content-type") : "",
              "text/event-stream");
    EXPECT_NE(streamed.body.find("data: hello"), std::string::npos);
    {
        std::lock_guard<std::mutex> lock(*seen_mutex);
        ASSERT_EQ(seen->size(), 2u);
        EXPECT_NE(seen->at(1).find("POST /base/form?x=2 HTTP/1.1"), std::string::npos);
        EXPECT_NE(seen->at(1).find("hello=1"), std::string::npos);
        EXPECT_NE(seen->at(1).find("content-type: application/x-www-form-urlencoded"),
                  std::string::npos);
    }

    app.reset();
}

TEST(AppIntegration, DashboardShowsSortablePriceCatalog) {
    const int app_port = 19080 + (getpid() % 100);
    test::TempDir dir;
    test::writeFile(dir.path() / "prices.json", R"JSON({
  "models": [{
    "model_id": "mock/model-a",
    "version": 1,
    "endpoints": [
      {"tag": "fast", "prompt": 0.0001, "completion": 0.0002, "cache_read": 0.00005,
       "context_length": 131072, "max_completion_tokens": 4096, "status": 1},
      {"tag": "slow", "prompt": 0.0002, "completion": 0.0003, "cache_read": 0.0001,
       "context_length": 32768, "max_completion_tokens": 2048, "status": 1}
    ]
  }]
})JSON");
    {
        StatsDb seed((dir.path() / "stats.db").string());
        ASSERT_TRUE(seed.ok()) << seed.lastError();
        RequestRecord r;
        r.request_id = "price-seed";
        r.session_id = "price-session";
        r.model = "mock/model-a";
        r.provider_tag = "fast";
        r.status = 200;
        r.tokens_in = 10;
        r.tokens_out = 5;
        r.cache_read_tokens = 4;
        r.cost_input_nano = 90;
        r.cost_cache_read_nano = 30;
        r.cost_output_nano = 80;
        r.cost_actual_nano = 200;
        r.cost_estimated = false;
        r.latency_ms = 1000;
        r.ts = std::chrono::system_clock::now();
        ASSERT_TRUE(seed.record(r));
        RequestRecord other;
        other.request_id = "other-seed";
        other.session_id = "other-session";
        other.model = "other/model";
        other.provider_tag = "fast";
        other.status = 200;
        other.tokens_in = 1;
        other.tokens_out = 1;
        other.ts = std::chrono::system_clock::now();
        ASSERT_TRUE(seed.record(other));
    }
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path, configFor(app_port, 19980 + (getpid() % 100), dir.str()));
    const auto log = dir.path() / "process.log";
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready) << readTextFile(log);

    CurlClient client;
    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    login.timeouts.total_ms = 3000;
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    ASSERT_EQ(login_resp.status, 302);
    std::string cookie = login_resp.headers.count("set-cookie")
                             ? login_resp.headers.at("set-cookie")
                             : "";
    const auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    CurlHttpRequest page;
    page.method = "GET";
    page.url = "http://127.0.0.1:" + std::to_string(app_port) + "/";
    page.http2 = false;
    page.verify_tls = false;
    page.headers = {"Cookie: " + cookie, "Accept: text/html"};
    page.timeouts.total_ms = 3000;
    CurlHttpResponse page_resp;
    ASSERT_TRUE(client.perform(page, page_resp));
    ASSERT_EQ(page_resp.status, 200);
    EXPECT_EQ(page_resp.body.find("id=\"price-model\""), std::string::npos);
    EXPECT_EQ(page_resp.body.find("id=\"price-open\""), std::string::npos);
    EXPECT_EQ(page_resp.body.find("id=\"price-table\""), std::string::npos);
    EXPECT_EQ(page_resp.body.find("id=\"session-log-modal\""), std::string::npos);
    EXPECT_EQ(page_resp.body.find("id=\"blacklist-open\""), std::string::npos);
    EXPECT_NE(page_resp.body.find("href=\"/panel/stats?session_id=price-session\""),
              std::string::npos);
    EXPECT_NE(page_resp.body.find("href=\"/panel/providers\""), std::string::npos);
    EXPECT_NE(page_resp.body.find("href=\"/panel/providers#blacklist\""),
              std::string::npos);
    EXPECT_EQ(page_resp.body.find("CSRF: <code>"), std::string::npos);
    EXPECT_EQ(page_resp.body.find("Последние запросы"), std::string::npos);

    CurlHttpRequest stats_page;
    stats_page.method = "GET";
    stats_page.url = "http://127.0.0.1:" + std::to_string(app_port) +
                     "/panel/stats?session_id=price-session&model=mock/model-a";
    stats_page.http2 = false;
    stats_page.verify_tls = false;
    stats_page.headers = {"Cookie: " + cookie, "Accept: text/html"};
    stats_page.timeouts.total_ms = 3000;
    CurlHttpResponse stats_resp;
    ASSERT_TRUE(client.perform(stats_page, stats_resp));
    ASSERT_EQ(stats_resp.status, 200);
    EXPECT_NE(stats_resp.body.find("name=\"session_id\""), std::string::npos);
    EXPECT_NE(stats_resp.body.find("value=\"price-session\" selected"), std::string::npos);
    EXPECT_NE(stats_resp.body.find("id=\"requests\""), std::string::npos);
    EXPECT_NE(stats_resp.body.find("Страница 1 из 1"), std::string::npos);
    EXPECT_NE(stats_resp.body.find("price-seed"), std::string::npos);
    EXPECT_EQ(stats_resp.body.find("other-seed"), std::string::npos);
    EXPECT_EQ(stats_resp.body.find("id=\"price-table\""), std::string::npos);
    EXPECT_EQ(stats_resp.body.find("id=\"prices\""), std::string::npos);
    EXPECT_EQ(stats_resp.body.find("id=\"blacklist-table\""), std::string::npos);
    EXPECT_EQ(stats_resp.body.find("Скорость, tok/s"), std::string::npos);

    CurlHttpRequest providers_page;
    providers_page.method = "GET";
    providers_page.url = "http://127.0.0.1:" + std::to_string(app_port) +
                         "/panel/providers?model=mock/model-a";
    providers_page.http2 = false;
    providers_page.verify_tls = false;
    providers_page.headers = {"Cookie: " + cookie, "Accept: text/html"};
    providers_page.timeouts.total_ms = 3000;
    CurlHttpResponse providers_resp;
    ASSERT_TRUE(client.perform(providers_page, providers_resp));
    ASSERT_EQ(providers_resp.status, 200);
    EXPECT_NE(providers_resp.body.find("id=\"price-table\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"speed\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"in\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"weight\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"req\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"cachepct\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"saved\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("id=\"blacklist-table\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("id=\"blacklist-save\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-model='mock/model-a'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-provider='fast'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-provider='slow'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-req='1'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-cachepct='40'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-speed='5.000000'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-saved='13333333333'"), std::string::npos);
    // weight is nanoRUB per 1M tokens: факт 120 RUB/M плюс вес кеша 2.0
    // (кешированный вход 4 из 10 токенов) — 133.3333 RUB/M.
    EXPECT_NE(providers_resp.body.find("data-weight='133333333333'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find(">133.3333</td>"), std::string::npos);
    // Распределение стоимости профиля (10 вход / 4 кеш / 5 выход) по ценам fast:
    // 600000 / 200000 / 1000000 nanoRUB => 33% / 11% / 56%.
    EXPECT_NE(providers_resp.body.find("data-sort=\"mixin\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"mixcache\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-sort=\"mixout\""), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-mixin='33'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-mixcache='11'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-mixout='56'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-mixin='39'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-mixcache='13'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("data-mixout='48'"), std::string::npos);
    EXPECT_NE(providers_resp.body.find("in 40.0000 · cache 13.3333 · out 66.6667 RUB/M"),
              std::string::npos);

    // На главной — только текущий и предыдущий маршрут; настройки, порядок и
    // предпросмотр живут на странице маршрутизации.
    CurlHttpRequest main_page;
    main_page.method = "GET";
    main_page.url = "http://127.0.0.1:" + std::to_string(app_port) + "/";
    main_page.http2 = false;
    main_page.verify_tls = false;
    main_page.headers = {"Cookie: " + cookie, "Accept: text/html"};
    main_page.timeouts.total_ms = 3000;
    CurlHttpResponse main_resp;
    ASSERT_TRUE(client.perform(main_page, main_resp));
    ASSERT_EQ(main_resp.status, 200);
    EXPECT_NE(main_resp.body.find("Текущий и предыдущий маршрут"), std::string::npos);
    EXPECT_EQ(main_resp.body.find("id=\"cache-reset\""), std::string::npos);

    CurlHttpRequest routing_page = main_page;
    routing_page.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/routing";
    routing_page.timeouts.total_ms = 3000;
    CurlHttpResponse routing_resp;
    ASSERT_TRUE(client.perform(routing_page, routing_resp));
    ASSERT_EQ(routing_resp.status, 200);
    // The routing preview is produced by buildUpstreamBody and ordered by the
    // same quotes as the price table: cheapest provider first.
    // The profile comes from real statistics: one seeded request of
    // 10 input / 5 output / 4 cached tokens.
    EXPECT_NE(routing_resp.body.find("вход 10 · выход 5 · кеш 4 токенов"), std::string::npos);
    EXPECT_NE(routing_resp.body.find("наблюдений: 1"), std::string::npos);
    EXPECT_NE(routing_resp.body.find("fast → slow</td>"), std::string::npos);
    // Один провайдер в provider.order: внешний обработчик не выбирает сам.
    EXPECT_NE(routing_resp.body.find("\"order\":[\"fast\"]"), std::string::npos);
    EXPECT_NE(routing_resp.body.find("\"allow_fallbacks\":false"), std::string::npos);
    EXPECT_NE(routing_resp.body.find("id=\"cache-reset\""), std::string::npos);
    EXPECT_NE(routing_resp.body.find("id=\"preview-cards\""), std::string::npos);
    EXPECT_NE(routing_resp.body.find("id=\"preview-table\""), std::string::npos);
    // Сброс кеша выше предпросмотра: предпросмотр — последний блок страницы.
    EXPECT_LT(routing_resp.body.find("Сброс статистики кеша"),
              routing_resp.body.find("Предпросмотр весов на последних запросах"));

    // Предпросмотр — тот же расчёт на выборке последних запросов и ещё не
    // сохранённых весах; рабочий снимок он не меняет.
    CurlHttpRequest preview_req = main_page;
    preview_req.url = "http://127.0.0.1:" + std::to_string(app_port) +
                      "/panel/api/routing-preview?model=mock/model-a&sample=1"
                      "&output_weight=1.5&cache_weight=3";
    preview_req.headers = {"Cookie: " + cookie, "Accept: application/json"};
    preview_req.timeouts.total_ms = 3000;
    CurlHttpResponse preview_resp;
    ASSERT_TRUE(client.perform(preview_req, preview_resp));
    ASSERT_EQ(preview_resp.status, 200);
    EXPECT_NE(preview_resp.body.find("\"sample\":1"), std::string::npos);
    EXPECT_NE(preview_resp.body.find("\"observed\":true"), std::string::npos);
    EXPECT_NE(preview_resp.body.find("\"observations\":1"), std::string::npos);
    EXPECT_NE(preview_resp.body.find("\"output_weight\":1.5"), std::string::npos);
    EXPECT_NE(preview_resp.body.find("\"provider\":\"fast\""), std::string::npos);
    EXPECT_NE(preview_resp.body.find("\"rows\""), std::string::npos);

    const std::string csrf = csrfFromPage(page_resp.body);
    ASSERT_FALSE(csrf.empty());

    CurlHttpRequest session_logs;
    session_logs.method = "GET";
    session_logs.url = "http://127.0.0.1:" + std::to_string(app_port) +
                       "/panel/api/requests?limit=50&session_id=price-session";
    session_logs.http2 = false;
    session_logs.verify_tls = false;
    session_logs.headers = {"Cookie: " + cookie};
    session_logs.timeouts.total_ms = 3000;
    CurlHttpResponse session_logs_resp;
    ASSERT_TRUE(client.perform(session_logs, session_logs_resp));
    ASSERT_EQ(session_logs_resp.status, 200);
    EXPECT_NE(session_logs_resp.body.find("\"source\":\"sqlite\""), std::string::npos);
    EXPECT_NE(session_logs_resp.body.find("price-seed"), std::string::npos);
    EXPECT_EQ(session_logs_resp.body.find("other-seed"), std::string::npos);

    CurlHttpRequest blacklist_get;
    blacklist_get.method = "GET";
    blacklist_get.url = "http://127.0.0.1:" + std::to_string(app_port) +
                        "/panel/api/blacklist";
    blacklist_get.http2 = false;
    blacklist_get.verify_tls = false;
    blacklist_get.headers = {"Cookie: " + cookie};
    blacklist_get.timeouts.total_ms = 3000;
    CurlHttpResponse blacklist_get_resp;
    ASSERT_TRUE(client.perform(blacklist_get, blacklist_get_resp));
    ASSERT_EQ(blacklist_get_resp.status, 200);
    EXPECT_NE(blacklist_get_resp.body.find("\"entries\""), std::string::npos);

    CurlHttpRequest blacklist_post;
    blacklist_post.method = "POST";
    blacklist_post.url = "http://127.0.0.1:" + std::to_string(app_port) +
                         "/panel/api/blacklist";
    blacklist_post.http2 = false;
    blacklist_post.verify_tls = false;
    blacklist_post.headers = {"Cookie: " + cookie,
                              "Content-Type: application/json",
                              "X-CSRF-Token: " + csrf};
    blacklist_post.body =
        R"({"entries":[{"provider":"test-provider","models":["*"],"reason":"test"},)"
        R"({"provider":"slow","models":["mock/model-a"],"reason":"медленный"}]})";
    blacklist_post.timeouts.total_ms = 3000;
    CurlHttpResponse blacklist_post_resp;
    ASSERT_TRUE(client.perform(blacklist_post, blacklist_post_resp));
    ASSERT_EQ(blacklist_post_resp.status, 200);
    EXPECT_NE(blacklist_post_resp.body.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(blacklist_post_resp.body.find("\"entries\""), std::string::npos);
    EXPECT_NE(blacklist_post_resp.body.find("test-provider"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("test-provider"), std::string::npos);
    auto reloaded = AppConfig::load(config_path.string());
    ASSERT_EQ(reloaded.routing.provider_blacklist.size(), 2u);
    EXPECT_EQ(reloaded.routing.provider_blacklist[0].provider, "test-provider");
    EXPECT_EQ(reloaded.routing.provider_blacklist[0].models,
              std::vector<std::string>{"*"});
    EXPECT_EQ(reloaded.routing.provider_blacklist[1].provider, "slow");
    EXPECT_EQ(reloaded.routing.provider_blacklist[1].models,
              std::vector<std::string>{"mock/model-a"});

    // The saved list is reflected without a restart.
    CurlHttpResponse blacklist_after_resp;
    ASSERT_TRUE(client.perform(blacklist_get, blacklist_after_resp));
    ASSERT_EQ(blacklist_after_resp.status, 200);
    EXPECT_NE(blacklist_after_resp.body.find("test-provider"), std::string::npos);

    CurlHttpRequest cache_reset;
    cache_reset.method = "POST";
    cache_reset.url = "http://127.0.0.1:" + std::to_string(app_port) +
                      "/panel/api/cache-reset";
    cache_reset.http2 = false;
    cache_reset.verify_tls = false;
    cache_reset.headers = {"Cookie: " + cookie, "Content-Type: application/json",
                           "X-CSRF-Token: " + csrf};
    cache_reset.body = R"({"scope":"session","value":"price-session"})";
    cache_reset.timeouts.total_ms = 3000;
    CurlHttpResponse cache_reset_resp;
    ASSERT_TRUE(client.perform(cache_reset, cache_reset_resp));
    ASSERT_EQ(cache_reset_resp.status, 200);
    EXPECT_NE(cache_reset_resp.body.find("\"ok\":true"), std::string::npos);

    CurlHttpRequest cache_reset_bad;
    cache_reset_bad.method = "POST";
    cache_reset_bad.url = cache_reset.url;
    cache_reset_bad.http2 = false;
    cache_reset_bad.verify_tls = false;
    cache_reset_bad.headers = cache_reset.headers;
    cache_reset_bad.body = R"({"scope":"nope","value":"x"})";
    cache_reset_bad.timeouts.total_ms = 3000;
    CurlHttpResponse cache_reset_bad_resp;
    client.perform(cache_reset_bad, cache_reset_bad_resp); // 400 => perform returns false
    EXPECT_EQ(cache_reset_bad_resp.status, 400);

    // Blacklisted pairs stay visible, marked with the reason, until hidden.
    auto providers_at = [&](const std::string& query, std::string& body) {
        CurlHttpRequest page;
        page.method = "GET";
        page.url = "http://127.0.0.1:" + std::to_string(app_port) +
                   "/panel/providers?model=mock/model-a" + query;
        page.http2 = false;
        page.verify_tls = false;
        page.headers = {"Cookie: " + cookie, "Accept: text/html"};
        page.timeouts.total_ms = 3000;
        CurlHttpResponse resp;
        EXPECT_TRUE(client.perform(page, resp));
        EXPECT_EQ(resp.status, 200);
        body = resp.body;
    };
    std::string blocked_body;
    providers_at("", blocked_body);
    EXPECT_NE(blocked_body.find("name=\"hide_blocked\""), std::string::npos);
    EXPECT_NE(blocked_body.find("data-provider='slow'"), std::string::npos);
    EXPECT_NE(blocked_body.find("data-blocked='1'"), std::string::npos);
    EXPECT_NE(blocked_body.find("row-blocked"), std::string::npos);
    EXPECT_NE(blocked_body.find("медленный"), std::string::npos);
    EXPECT_NE(blocked_body.find("В чёрном списке"), std::string::npos);

    std::string hidden_body;
    providers_at("&hide_blocked=1", hidden_body);
    EXPECT_NE(hidden_body.find("name=\"hide_blocked\" value=\"1\" checked"),
              std::string::npos);
    EXPECT_NE(hidden_body.find("data-provider='fast'"), std::string::npos);
    EXPECT_EQ(hidden_body.find("data-provider='slow'"), std::string::npos);
    EXPECT_EQ(hidden_body.find("медленный"), std::string::npos);

    app.reset();
}

TEST(AppIntegration, MainSessionsLimitAndStatsPageFilters) {
    const int app_port = 19090 + (getpid() % 100);
    test::TempDir dir;
    const auto stats_path = (dir.path() / "stats.db").string();
    {
        StatsDb seed(stats_path);
        ASSERT_TRUE(seed.ok()) << seed.lastError();
        const auto now = std::chrono::system_clock::now();
        for (int i = 1; i <= 7; ++i) {
            RequestRecord r;
            r.request_id = "req-s" + std::to_string(i);
            r.session_id = "s" + std::to_string(i);
            r.model = "mock/model-a";
            r.provider_tag = (i % 2 == 0) ? "fast" : "slow";
            r.status = 200;
            r.tokens_in = 100;
            r.tokens_out = 10;
            r.cache_read_tokens = 50;
            r.cost_actual_nano = 10000;
            r.cost_estimated = false;
            r.cost_input_nano = 4000;
            r.cost_cache_read_nano = 5000;
            r.cost_output_nano = 1000;
            r.latency_ms = 1000;
            r.ts = now - std::chrono::seconds(7 - i);
            if (i == 5) r.error_code = kCacheMissStreakCode;
            ASSERT_TRUE(seed.record(r));
        }
        // Прошлые сутки: база для процента изменения стоимости.
        RequestRecord previous;
        previous.request_id = "req-prev";
        previous.session_id = "s-prev";
        previous.model = "mock/model-a";
        previous.provider_tag = "fast";
        previous.status = 200;
        previous.tokens_in = 100;
        previous.tokens_out = 10;
        previous.cache_read_tokens = 50;
        previous.cost_actual_nano = 10000;
        previous.cost_estimated = false;
        previous.cost_input_nano = 4000;
        previous.cost_cache_read_nano = 5000;
        previous.cost_output_nano = 1000;
        previous.latency_ms = 1000;
        previous.ts = now - std::chrono::hours(24);
        ASSERT_TRUE(seed.record(previous));
    }
    const auto config_path = dir.path() / "config.yaml";
    std::ostringstream cfg;
    cfg << R"YAML(
proxy:
  name: "sessions-page"
  data_dir: ")YAML" << dir.str() << R"YAML("
  listen: "127.0.0.1:)YAML" << app_port << R"YAML("
  threads: 2

panel:
  path: "/panel"
  allowed_networks: ["127.0.0.1/32", "::1/128"]

portal:
  users:
    - username: "admin"
      password_hash: ")YAML" << adminPasswordHash() << R"YAML("
      role: "admin"

models: ["mock/model-a"]
default_model: "mock/model-a"

providers:
  local:
    - id: "local"
      url: "http://127.0.0.1:1/v1"
      models: ["mock/model-a"]
      price_per_million: { input: 1.0, output: 2.0, cache_read: 0.25 }

consumers:
  - id: "dev"
    token_hash: ")YAML" << tokenHash("dev-token") << R"YAML("
    models: ["*"]

stats:
  sqlite_path: ")YAML" << stats_path << R"YAML("
  ring_buffer_size: 100
  prometheus: { enabled: false }
)YAML";
    test::writeFile(config_path, cfg.str());
    const auto log = dir.path() / "process.log";
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()},
                             log.string());
    ASSERT_GT(app, 0);
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready) << readTextFile(log);

    CurlClient client;
    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    login.timeouts.total_ms = 3000;
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    ASSERT_EQ(login_resp.status, 302);
    std::string cookie = login_resp.headers.count("set-cookie")
                             ? login_resp.headers.at("set-cookie")
                             : "";
    const auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    const auto get_page = [&](const std::string& path, std::string& body) {
        CurlHttpRequest req;
        req.method = "GET";
        req.url = "http://127.0.0.1:" + std::to_string(app_port) + path;
        req.http2 = false;
        req.verify_tls = false;
        req.headers = {"Cookie: " + cookie, "Accept: text/html"};
        req.timeouts.total_ms = 5000;
        CurlHttpResponse resp;
        EXPECT_TRUE(client.perform(req, resp));
        EXPECT_EQ(resp.status, 200);
        body = resp.body;
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        std::size_t found = 0, pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            ++found;
            pos += needle.size();
        }
        return found;
    };

    std::string main_body;
    get_page("/", main_body);
    EXPECT_NE(main_body.find("Запросов сегодня"), std::string::npos);
    // Ошибки и предупреждения — за сутки и с переходом в журнал под фильтром.
    const std::string today_day = utcDayString(std::chrono::system_clock::now());
    EXPECT_NE(main_body.find("Ошибки сегодня"), std::string::npos);
    EXPECT_NE(main_body.find("Предупреждения сегодня"), std::string::npos);
    EXPECT_NE(main_body.find("severity 1 · " + today_day), std::string::npos);
    EXPECT_NE(main_body.find("/panel/stats?day=" + today_day + "&amp;issue=warning"),
              std::string::npos);
    EXPECT_NE(main_body.find("Стоимость, сегодня"), std::string::npos);
    EXPECT_NE(main_body.find("Стоимость: вход, ₽"), std::string::npos);
    EXPECT_NE(main_body.find("Стоимость: кеш, ₽"), std::string::npos);
    EXPECT_NE(main_body.find("Стоимость: выход, ₽"), std::string::npos);
    EXPECT_NE(main_body.find("Cache hit сегодня"), std::string::npos);
    // Стоимость за текущие сутки: 7 запросов по 10000 nanoRUB.
    EXPECT_NE(main_body.find(">0.00007 ₽</div>"), std::string::npos);
    EXPECT_NE(main_body.find("40% распределения"), std::string::npos);
    EXPECT_NE(main_body.find("50% распределения"), std::string::npos);
    EXPECT_NE(main_body.find("10% распределения"), std::string::npos);
    // Прошлые сутки — отдельная карточка полного дня, без сравнения с неполными.
    EXPECT_NE(main_body.find("Стоимость, прошлые сутки"), std::string::npos);
    EXPECT_EQ(main_body.find("Δ стоимости"), std::string::npos);
    EXPECT_EQ(main_body.find("delta-up"), std::string::npos);
    EXPECT_EQ(main_body.find("Стримов"), std::string::npos);
    EXPECT_EQ(main_body.find("Cache saved, RUB"), std::string::npos);
    EXPECT_EQ(main_body.find("Ср. latency, мс"), std::string::npos);
    EXPECT_EQ(main_body.find("Моделей в каталоге"), std::string::npos);
    // Главная показывает текущий и предыдущий маршрут и время переключения.
    EXPECT_NE(main_body.find("Текущий и предыдущий маршрут"), std::string::npos);
    EXPECT_NE(main_body.find("href=\"/panel/providers\""), std::string::npos);
    EXPECT_NE(main_body.find("href=\"/panel/providers#blacklist\""), std::string::npos);
    EXPECT_EQ(count(main_body, "/panel/stats?session_id="), 5);
    EXPECT_NE(main_body.find("session_id=s7"), std::string::npos);
    EXPECT_NE(main_body.find("session_id=s3"), std::string::npos);
    EXPECT_EQ(main_body.find("session_id=s2"), std::string::npos);
    EXPECT_NE(main_body.find("href=\"/panel/stats\""), std::string::npos);
    EXPECT_EQ(main_body.find("href=\"/panel/stats#prices\""), std::string::npos);
    EXPECT_EQ(main_body.find("id=\"price-table\""), std::string::npos);
    EXPECT_EQ(main_body.find("id=\"session-log-modal\""), std::string::npos);
    EXPECT_EQ(main_body.find("id=\"blacklist-open\""), std::string::npos);
    EXPECT_NE(main_body.find("Время работы"), std::string::npos);
    EXPECT_NE(main_body.find("<h2>Точки подключения агентов</h2>"), std::string::npos);
    // Настройки и алгоритм — на отдельной странице, не на главной.
    EXPECT_EQ(main_body.find("id=\"routing-toggle\""), std::string::npos);
    std::string routing_body;
    get_page("/panel/routing", routing_body);
    EXPECT_NE(routing_body.find("Алгоритм перестройки очереди"), std::string::npos);
    EXPECT_NE(routing_body.find("id=\"routing-toggle\""), std::string::npos);
    EXPECT_NE(routing_body.find("provider.order из 1 — смену провайдера выбирает только прокси"),
              std::string::npos);
    // Eight seeded requests (7 today + 1 previous day) of
    // 100 input / 10 output / 50 cached tokens each.
    EXPECT_NE(routing_body.find("вход 100 · выход 10 · кеш 50 токенов"), std::string::npos);
    EXPECT_NE(routing_body.find("наблюдений: 8"), std::string::npos);
    EXPECT_NE(routing_body.find("local</td><td>—</td><td><code>"), std::string::npos);
    // A local OpenAI-compatible provider gets no routing object: the preview is
    // produced by buildUpstreamBody, so it reports exactly that.
    EXPECT_NE(routing_body.find("не передаётся"), std::string::npos);
    EXPECT_NE(main_body.find("http://127.0.0.1:" + std::to_string(app_port) + "/v1"),
              std::string::npos);
    EXPECT_NE(main_body.find(">dev</td>"), std::string::npos);
    EXPECT_NE(main_body.find("без лимитов"), std::string::npos);
    EXPECT_NE(main_body.find("data-restart=\"dsh\""), std::string::npos);
    EXPECT_NE(main_body.find("data-restart=\"strata\""), std::string::npos);
    EXPECT_NE(main_body.find("data-restart=\"proxy\""), std::string::npos);
    const auto menu_pos = main_body.find("id=\"service-menu-toggle\"");
    const auto user_pos = main_body.find("<span>admin");
    ASSERT_NE(menu_pos, std::string::npos);
    ASSERT_NE(user_pos, std::string::npos);
    EXPECT_LT(menu_pos, user_pos);

    const std::string csrf = csrfFromPage(main_body);
    ASSERT_FALSE(csrf.empty());

    // Routing options toggle: off removes the provider object, on restores it.
    auto routing_options = [&](const std::string& body, long& code, std::string& out) {
        CurlHttpRequest request;
        request.method = "GET";
        request.url = "http://127.0.0.1:" + std::to_string(app_port) +
                      "/panel/api/routing-options";
        request.http2 = false;
        request.verify_tls = false;
        request.headers = {"Cookie: " + cookie};
        if (!body.empty()) {
            request.method = "POST";
            request.headers.push_back("Content-Type: application/json");
            request.headers.push_back("X-CSRF-Token: " + csrf);
            request.body = body;
        }
        request.timeouts.total_ms = 3000;
        CurlHttpResponse resp;
        client.perform(request, resp); // 4xx responses are expected here
        code = resp.status;
        out = resp.body;
    };
    long code = 0;
    std::string options_body;
    routing_options("", code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"inject_provider_options\":true"), std::string::npos);
    EXPECT_NE(options_body.find("\"provider_order_size\":1"), std::string::npos);
    EXPECT_NE(options_body.find("\"order\":[\"local\"]"), std::string::npos);
    EXPECT_NE(options_body.find("\"inject\":null"), std::string::npos);
    // Форма «Правка настроек» читает эти ключи из того же ответа: без них поля
    // показывают 0, а сохранение отвергается валидацией.
    for (const char* key : {"\"session_pin_max\":4096", "\"queue_min_sec\":600",
                            "\"queue_max_sec\":900", "\"queue_stats_sec\":3600"}) {
        EXPECT_NE(options_body.find(key), std::string::npos) << key;
    }
    routing_options(R"({"session_pin_max":2048,"queue_min_sec":300,"queue_max_sec":600,)"
                    R"("queue_stats_sec":1200})",
                    code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"session_pin_max\":2048"), std::string::npos);
    EXPECT_NE(options_body.find("\"queue_stats_sec\":1200"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("session_pin_max: 2048"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("queue_min_sec: 300"), std::string::npos);
    routing_options(R"({"session_pin_max":4096,"queue_min_sec":600,"queue_max_sec":900,)"
                    R"("queue_stats_sec":3600})",
                    code, options_body);
    ASSERT_EQ(code, 200);

    routing_options(R"({"inject_provider_options":false})", code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"inject_provider_options\":false"), std::string::npos);
    EXPECT_NE(options_body.find("\"inject\":null"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("inject_provider_options: false"),
              std::string::npos);

    std::string toggled_body;
    get_page("/panel/routing", toggled_body);
    EXPECT_NE(toggled_body.find("Включить опции маршрутизации"), std::string::npos);
    EXPECT_NE(toggled_body.find("не передаётся"), std::string::npos);

    routing_options(R"({"inject_provider_options":true})", code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"order\":[\"local\"]"), std::string::npos);
    EXPECT_NE(options_body.find("\"inject\":null"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("inject_provider_options: true"),
              std::string::npos);

    // Веса ранжирования: сохраняются в конфиг и сразу применяются к очереди.
    routing_options(R"({"output_weight":1.5,"cache_weight":3.0})", code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"output_weight\":1.5"), std::string::npos);
    EXPECT_NE(options_body.find("\"cache_weight\":3"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("output_weight: 1.5"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("cache_weight: 3.0"), std::string::npos);
    const auto weights_reloaded = AppConfig::load(config_path.string());
    EXPECT_DOUBLE_EQ(weights_reloaded.routing.output_weight, 1.5);
    EXPECT_DOUBLE_EQ(weights_reloaded.routing.cache_weight, 3.0);

    std::string weights_body;
    get_page("/panel/routing", weights_body);
    // Форма настроек рендерится с текущими значениями конфигурации.
    EXPECT_NE(weights_body.find("id=\"set-output-weight\" min=\"0\" max=\"100\" "
                                "step=\"0.1\" value=\"1.5\""),
              std::string::npos);
    EXPECT_NE(weights_body.find("id=\"set-cache-weight\" min=\"0\" max=\"100\" "
                                "step=\"0.1\" value=\"3\""),
              std::string::npos);

    // Профиль весов: выборка последних запросов сохраняется и переживает --check.
    routing_options(R"({"profile_sample_size":12})", code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"profile_sample_size\":12"), std::string::npos);
    EXPECT_NE(readTextFile(config_path).find("profile_sample_size: 12"), std::string::npos);
    EXPECT_EQ(AppConfig::load(config_path.string()).routing.profile_sample_size, 12);
    routing_options(R"({"profile_sample_size":100001})", code, options_body);
    EXPECT_EQ(code, 400);
    routing_options(R"({"profile_sample_size":-1})", code, options_body);
    EXPECT_EQ(code, 400);

    // Правило неработающего провайдера: сохраняется в конфиг и переживает --check.
    routing_options(R"({"health_fail_threshold":4,"health_down_after_sec":30,)"
                    R"("health_probe_interval_sec":15,"health_recover_successes":2})",
                    code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"health_fail_threshold\":4"), std::string::npos);
    EXPECT_NE(options_body.find("\"provider_health\""), std::string::npos);
    const auto health_cfg = AppConfig::load(config_path.string());
    EXPECT_EQ(health_cfg.routing.health.fail_threshold, 4);
    EXPECT_EQ(health_cfg.routing.health.down_after_sec, 30);
    EXPECT_EQ(health_cfg.routing.health.probe_interval_sec, 15);
    EXPECT_EQ(health_cfg.routing.health.recover_successes, 2);
    routing_options(R"({"wait_price_rub_per_sec":0.002,"failover_wait_max_sec":45})",
                    code, options_body);
    ASSERT_EQ(code, 200);
    EXPECT_NE(options_body.find("\"wait_price_rub_per_sec\":0.002"), std::string::npos);
    EXPECT_NE(options_body.find("\"failover_wait_max_sec\":45"), std::string::npos);
    const auto wait_cfg = AppConfig::load(config_path.string());
    EXPECT_DOUBLE_EQ(wait_cfg.routing.wait_price_rub_per_sec, 0.002);
    EXPECT_EQ(wait_cfg.routing.failover_wait_max_sec, 45);
    routing_options(R"({"wait_price_rub_per_sec":101})", code, options_body);
    EXPECT_EQ(code, 400);
    routing_options(R"({"failover_wait_max_sec":-1})", code, options_body);
    EXPECT_EQ(code, 400);
    std::string wait_body;
    get_page("/panel/routing", wait_body);
    EXPECT_NE(wait_body.find("id=\"set-wait-price\""), std::string::npos);
    EXPECT_NE(wait_body.find("id=\"set-failover-wait\""), std::string::npos);

    routing_options(R"({"health_probe_interval_sec":0})", code, options_body);
    EXPECT_EQ(code, 400);
    routing_options(R"({"health_recover_successes":0})", code, options_body);
    EXPECT_EQ(code, 400);
    std::string health_body;
    get_page("/panel/routing", health_body);
    EXPECT_NE(health_body.find("id=\"set-health-threshold\""), std::string::npos);
    EXPECT_NE(health_body.find("неработающий провайдер"), std::string::npos);

    routing_options(R"({"cache_weight":"2"})", code, options_body);
    EXPECT_EQ(code, 400);
    routing_options(R"({"cache_weight":101})", code, options_body);
    EXPECT_EQ(code, 400);
    EXPECT_NE(readTextFile(config_path).find("cache_weight: 3.0"), std::string::npos);

    routing_options(R"({"inject_provider_options":"yes"})", code, options_body);
    EXPECT_EQ(code, 400);

    CurlHttpRequest bad_restart;
    bad_restart.method = "POST";
    bad_restart.url = "http://127.0.0.1:" + std::to_string(app_port) +
                     "/panel/api/service-restart";
    bad_restart.http2 = false;
    bad_restart.verify_tls = false;
    bad_restart.headers = {"Cookie: " + cookie, "Content-Type: application/json",
                           "X-CSRF-Token: " + csrf};
    bad_restart.body = R"({"service":"unknown"})";
    bad_restart.timeouts.total_ms = 3000;
    CurlHttpResponse bad_restart_resp;
    client.perform(bad_restart, bad_restart_resp);
    EXPECT_EQ(bad_restart_resp.status, 400);
    EXPECT_NE(bad_restart_resp.body.find("service must be one of"), std::string::npos);

    std::string stats_body;
    const std::string stats_url = "/panel/stats?day=" + today_day + "&issue=warning";
    get_page(stats_url, stats_body);
    EXPECT_NE(stats_body.find("id=\"requests\""), std::string::npos);
    // Журнал — в самом низу страницы, провайдеры выше.
    const auto providers_pos = stats_body.find("<h2>Провайдеры</h2>");
    const auto journal_pos = stats_body.find("id=\"requests\"");
    ASSERT_NE(providers_pos, std::string::npos);
    ASSERT_NE(journal_pos, std::string::npos);
    EXPECT_LT(providers_pos, journal_pos);
    EXPECT_NE(stats_body.find("Журнал запросов"), std::string::npos);
    EXPECT_NE(stats_body.find("name=\"day\" value=\"" + today_day + "\""),
              std::string::npos);
    EXPECT_EQ(stats_body.find("Оценка веса"), std::string::npos);
    // Провайдеры и их переключение — на странице журнала.
    EXPECT_NE(stats_body.find("in 40% · cache 50% · out 10%"), std::string::npos);
    EXPECT_NE(stats_body.find("Переключение на"), std::string::npos);
    // Относительное время рядом с абсолютным во всех временных колонках.
    EXPECT_NE(stats_body.find(" назад"), std::string::npos);
    EXPECT_NE(stats_body.find("slow"), std::string::npos);
    EXPECT_NE(stats_body.find("fast"), std::string::npos);
    EXPECT_NE(stats_body.find("Страница 1 из 1"), std::string::npos);
    EXPECT_EQ(stats_body.find("id=\"prices\""), std::string::npos);
    EXPECT_EQ(stats_body.find("id=\"blacklist\""), std::string::npos);
    EXPECT_EQ(stats_body.find("id=\"price-table\""), std::string::npos);
    EXPECT_EQ(count(stats_body, "/panel/stats?session_id="), 0);

    std::string page_body;
    get_page("/panel/stats?limit=2&page=2", page_body);
    EXPECT_NE(page_body.find("Страница 2 из 4"), std::string::npos);
    EXPECT_NE(page_body.find("req-s5"), std::string::npos);
    EXPECT_EQ(page_body.find("req-s7"), std::string::npos);
    EXPECT_NE(page_body.find("page=3"), std::string::npos);

    std::string filtered_body;
    get_page("/panel/stats?session_id=s7", filtered_body);
    EXPECT_NE(filtered_body.find("value=\"s7\" selected"), std::string::npos);
    EXPECT_NE(filtered_body.find("req-s7"), std::string::npos);
    EXPECT_EQ(filtered_body.find("req-s6"), std::string::npos);

    // Фильтр важности: req-s5 — единственное предупреждение, ошибок нет.
    std::string warning_body;
    get_page("/panel/stats?issue=warning", warning_body);
    EXPECT_NE(warning_body.find("value=\"warning\" selected"), std::string::npos);
    EXPECT_NE(warning_body.find("req-s5"), std::string::npos);
    EXPECT_EQ(warning_body.find("req-s6"), std::string::npos);
    EXPECT_NE(warning_body.find("предупреждение: нет попаданий в кеш"), std::string::npos);
    EXPECT_NE(warning_body.find("data-total=\"1\""), std::string::npos);

    std::string issue_error_body;
    get_page("/panel/stats?issue=error", issue_error_body);
    EXPECT_NE(issue_error_body.find("value=\"error\" selected"), std::string::npos);
    EXPECT_NE(issue_error_body.find("Запросов по текущим фильтрам нет"), std::string::npos);
    EXPECT_EQ(issue_error_body.find("req-s5"), std::string::npos);

    // Удаление журнала: кнопка на странице, админ+CSRF, фильтр по сессии.
    EXPECT_NE(stats_body.find("id=\"log-delete\""), std::string::npos);
    // Выборка ограничена сегодняшними сутками и предупреждениями: 1 запись.
    EXPECT_NE(stats_body.find("data-day=\"" + today_day + "\""), std::string::npos);
    EXPECT_NE(stats_body.find("data-total=\"1\""), std::string::npos);
    EXPECT_NE(stats_body.find("Сбросить фильтры"), std::string::npos);
    const std::string stats_csrf = csrfFromPage(stats_body);
    ASSERT_FALSE(stats_csrf.empty());

    CurlHttpRequest delete_one;
    delete_one.method = "POST";
    delete_one.url = "http://127.0.0.1:" + std::to_string(app_port) +
                     "/panel/api/requests-delete";
    delete_one.http2 = false;
    delete_one.verify_tls = false;
    delete_one.headers = {"Cookie: " + cookie, "Content-Type: application/json",
                          "X-CSRF-Token: " + stats_csrf};
    delete_one.body = R"({"session_id":"s7"})";
    delete_one.timeouts.total_ms = 3000;
    CurlHttpResponse delete_one_resp;
    ASSERT_TRUE(client.perform(delete_one, delete_one_resp));
    ASSERT_EQ(delete_one_resp.status, 200);
    EXPECT_NE(delete_one_resp.body.find("\"deleted\":1"), std::string::npos);

    CurlHttpRequest after_delete;
    after_delete.method = "GET";
    after_delete.url = "http://127.0.0.1:" + std::to_string(app_port) +
                       "/panel/api/requests?limit=50&session_id=s7";
    after_delete.http2 = false;
    after_delete.verify_tls = false;
    after_delete.headers = {"Cookie: " + cookie};
    after_delete.timeouts.total_ms = 3000;
    CurlHttpResponse after_delete_resp;
    ASSERT_TRUE(client.perform(after_delete, after_delete_resp));
    ASSERT_EQ(after_delete_resp.status, 200);
    EXPECT_NE(after_delete_resp.body.find("\"requests\":[]"), std::string::npos);

    std::string after_stats;
    get_page("/panel/stats", after_stats);
    EXPECT_NE(after_stats.find("Страница 1 из 1"), std::string::npos);
    EXPECT_EQ(after_stats.find("req-s7"), std::string::npos);
    EXPECT_NE(after_stats.find("req-s6"), std::string::npos);

    // Без CSRF и с нечитаемым телом удаление отклоняется.
    CurlHttpRequest delete_no_csrf = delete_one;
    delete_no_csrf.headers = {"Cookie: " + cookie, "Content-Type: application/json"};
    delete_no_csrf.body = R"({"session_id":"s6"})";
    CurlHttpResponse delete_no_csrf_resp;
    client.perform(delete_no_csrf, delete_no_csrf_resp); // 403 => perform returns false
    EXPECT_EQ(delete_no_csrf_resp.status, 403);

    // Весь журнал без "all" не удаляется: пустой фильтр требует явного подтверждения.
    CurlHttpRequest delete_all_implicit = delete_one;
    delete_all_implicit.body = "{}";
    CurlHttpResponse delete_all_implicit_resp;
    client.perform(delete_all_implicit, delete_all_implicit_resp);
    EXPECT_EQ(delete_all_implicit_resp.status, 400);

    CurlHttpRequest delete_bad_body = delete_one;
    delete_bad_body.body = "not json";
    CurlHttpResponse delete_bad_body_resp;
    client.perform(delete_bad_body, delete_bad_body_resp);
    EXPECT_EQ(delete_bad_body_resp.status, 400);
    // Отклонённые запросы ничего не удалили.
    std::string intact_body;
    get_page("/panel/stats?session_id=s6", intact_body);
    EXPECT_NE(intact_body.find("req-s6"), std::string::npos);

    std::string providers_body;
    get_page("/panel/providers", providers_body);
    EXPECT_NE(providers_body.find("id=\"price-table\""), std::string::npos);
    EXPECT_NE(providers_body.find("data-sort=\"speed\""), std::string::npos);
    EXPECT_NE(providers_body.find("id=\"blacklist-table\""), std::string::npos);
    EXPECT_NE(providers_body.find("data-provider='local'"), std::string::npos);

    app.reset();
}

TEST(AppIntegration, PortalWebSockets) {
    const int app_port = 18980 + (getpid() % 100);
    const int mock_port = 19980 + (getpid() % 100);
    test::TempDir dir;
    test::TestWsServer ws;
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path,
                    configFor(app_port, mock_port, dir.str(), false, 0, "", "", true,
                              true, false, ws.port()));
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    CurlClient client;
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready) << readTextFile(log);

    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    login.timeouts.total_ms = 3000;
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    ASSERT_EQ(login_resp.status, 302);
    std::string cookie = login_resp.headers.count("set-cookie")
                             ? login_resp.headers.at("set-cookie")
                             : "";
    const auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);
    ASSERT_FALSE(cookie.empty());

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(app_port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string request =
        "GET /localws/echo HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(app_port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Cookie: " + cookie + "\r\n\r\n";
    ASSERT_EQ(send(fd, request.data(), request.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(request.size()));
    std::string response;
    char buf[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        ASSERT_GT(n, 0);
        response.append(buf, static_cast<std::size_t>(n));
    }
    EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos);

    const std::string payload = "hello through portal";
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(0x80 | payload.size()));
    frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }
    ASSERT_EQ(send(fd, frame.data(), frame.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(frame.size()));

    std::uint8_t opcode = 0;
    std::string echoed;
    ASSERT_TRUE(test::readWsFrame(fd, opcode, echoed));
    EXPECT_EQ(opcode, 0x1);
    EXPECT_EQ(echoed, payload);

    close(fd);
    app.reset();
    mock.reset();
}

TEST(AppIntegration, PanelCanBeDisabled) {
    const int app_port = 18580 + (getpid() % 100);
    const int mock_port = 19480 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path,
                    configFor(app_port, mock_port, dir.str(), false, 0, "", "", false, true));
    const auto log = dir.path() / "process.log";
    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    CurlClient client;
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);
    EXPECT_EQ(readTextFile(log).find("web UI URL:"), std::string::npos) << readTextFile(log);

    long status = 0;
    std::string body;
    httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/panel/login", status, body);
    EXPECT_EQ(status, 404);
    CurlHttpRequest models;
    models.method = "GET";
    models.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/models";
    models.http2 = false;
    models.verify_tls = false;
    models.headers = {"Authorization: Bearer dev-token"};
    CurlHttpResponse models_resp;
    ASSERT_TRUE(client.perform(models, models_resp));
    EXPECT_EQ(models_resp.status, 200);

    app.reset();
    mock.reset();
}


TEST(AppIntegration, MetricsRequireToken) {
    const int app_port = 18680 + (getpid() % 100);
    const int mock_port = 19580 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path,
                    configFor(app_port, mock_port, dir.str(), false, 0, "", "", true, true, true));
    const auto log = dir.path() / "process.log";
    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    CurlHttpRequest metrics;
    metrics.method = "GET";
    metrics.url = "http://127.0.0.1:" + std::to_string(app_port) + "/metrics";
    metrics.http2 = false;
    metrics.verify_tls = false;
    metrics.timeouts.total_ms = 3000;
    CurlHttpResponse no_token;
    client.perform(metrics, no_token);
    EXPECT_EQ(no_token.status, 401);

    metrics.headers = {"Authorization: Bearer dev-token"};
    CurlHttpResponse metrics_only;
    client.perform(metrics, metrics_only);
    EXPECT_EQ(metrics_only.status, 401);

    metrics.headers = {"Authorization: Bearer stats-token"};
    CurlHttpResponse stats_reader;
    ASSERT_TRUE(client.perform(metrics, stats_reader));
    EXPECT_EQ(stats_reader.status, 200);
    EXPECT_NE(stats_reader.body.find("costrelay_up 1"), std::string::npos);

    app.reset();
    mock.reset();
}


TEST(AppIntegration, LocalOpenAiCompatibleProviderGetsMappedModelAndToken) {
    const int app_port = 18780 + (getpid() % 100);
    test::TempDir dir;
    auto seen_request = std::make_shared<std::string>();
    auto seen_mutex = std::make_shared<std::mutex>();
    test::TestHttpServer local([&](const std::string& req) {
        {
            std::lock_guard<std::mutex> lock(*seen_mutex);
            *seen_request = req;
        }
        return test::httpResponse(
            R"({"id":"local-1","object":"chat.completion","model":"upstream-model","choices":[{"index":0,"message":{"role":"assistant","content":"local-ok"},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15},"timings":{"cache_n":4,"prompt_n":6}})");
    });

    const auto token_path = dir.path() / "local.token";
    test::writeFile(token_path, "local-upstream-token\n");
    std::ostringstream cfg;
    cfg << R"YAML(
proxy:
  name: "local-integration"
  data_dir: ")YAML" << dir.str() << R"YAML("
  listen: "127.0.0.1:)YAML" << app_port << R"YAML("
  threads: 2

portal: { enabled: false }

models: ["local/model"]
default_model: "local/model"
aliases: { "default": "local/model" }

providers:
  local:
    - id: "local-llama"
      url: "http://127.0.0.1:)YAML" << local.port() << R"YAML(/v1"
      api_key: "file:)YAML" << token_path.string() << R"YAML("
      models: { "local/model": "upstream-model" }
      price_per_million: { input: 1.0, output: 2.0, cache_read: 0.25 }
      context_length: 131072
      max_completion_tokens: 4096
      verify_tls: false

consumers:
  - id: "local"
    token_hash: ")YAML" << tokenHash("local-client-token") << R"YAML("
    models: ["*"]

routing: { }

stats:
  sqlite_path: ")YAML" << dir.str() << R"YAML(/stats.db"
  ring_buffer_size: 50
  prometheus: { enabled: false }
)YAML";
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path, cfg.str());
    const auto log = dir.path() / "process.log";
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);

    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ready) {
        std::ifstream in(log);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ADD_FAILURE() << "local app did not become ready\n" << contents;
        app.reset();
        return;
    }

    CurlHttpRequest chat;
    chat.method = "POST";
    chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    chat.http2 = false;
    chat.verify_tls = false;
    chat.headers = {"Authorization: Bearer local-client-token", "Content-Type: application/json",
                    "X-Session-Id: sess-local"};
    chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}],"max_completion_tokens":10})";
    chat.timeouts.total_ms = 5000;
    CurlHttpResponse resp;
    CurlClient client;
    ASSERT_TRUE(client.perform(chat, resp)) << resp.error;
    EXPECT_EQ(resp.status, 200);
    EXPECT_NE(resp.body.find("local-ok"), std::string::npos);
    ASSERT_TRUE(resp.headers.count("x-proxy-provider"));
    EXPECT_EQ(resp.headers.at("x-proxy-provider"), "local-llama");

    std::string upstream;
    {
        std::lock_guard<std::mutex> lock(*seen_mutex);
        upstream = *seen_request;
    }
    EXPECT_NE(upstream.find("Authorization: Bearer local-upstream-token"), std::string::npos);
    EXPECT_NE(upstream.find("\"model\":\"upstream-model\""), std::string::npos);
    EXPECT_NE(upstream.find("\"max_tokens\":10"), std::string::npos);
    EXPECT_EQ(upstream.find("max_completion_tokens"), std::string::npos);
    EXPECT_EQ(upstream.find("\"provider\""), std::string::npos);

    app.reset();

    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    auto sessions = db.sessions(10);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["session_id"].asString(), "sess-local");
    EXPECT_EQ(sessions[0]["requests"].asInt64(), 1);
    EXPECT_GT(sessions[0]["bytes_in"].asInt64(), 50);
    EXPECT_GT(sessions[0]["bytes_out"].asInt64(), 50);
    EXPECT_EQ(sessions[0]["cache_read_tokens"].asInt64(), 4);
    EXPECT_EQ(sessions[0]["max_cache_read_tokens"].asInt64(), 4);
    EXPECT_EQ(sessions[0]["cost_nano"].asInt64(), 17000);
    EXPECT_EQ(sessions[0]["cost_input_nano"].asInt64(), 6000);
    EXPECT_EQ(sessions[0]["cost_cache_read_nano"].asInt64(), 1000);
    EXPECT_EQ(sessions[0]["cost_output_nano"].asInt64(), 10000);
    EXPECT_EQ(sessions[0]["cost_rub"].asString(), "0.000017");
    auto requests = db.requests("sess-local", 10);
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_FALSE(requests[0]["request_id"].asString().empty());
    EXPECT_EQ(requests[0]["cache_read_tokens"].asInt64(), 4);
    auto totals = db.totals();
    EXPECT_EQ(totals["cache_read_tokens_total"].asInt64(), 4);
    EXPECT_EQ(totals["cache_saved_nano"].asInt64(), 3000);
}

TEST(AppIntegration, CacheMissStreakWarnsAndBlacklistsProvider) {
    const int app_port = 18700 + (getpid() % 100);
    test::TempDir dir;
    // Апстрим отвечает без попаданий в кеш: timings.cache_n = 0.
    test::TestHttpServer local([&](const std::string&) {
        return test::httpResponse(
            R"({"id":"local-1","object":"chat.completion","model":"upstream-model","choices":[{"index":0,"message":{"role":"assistant","content":"local-ok"},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15},"timings":{"cache_n":0,"prompt_n":10}})");
    });

    std::ostringstream cfg;
    cfg << R"YAML(
proxy:
  name: "streak-integration"
  data_dir: ")YAML" << dir.str() << R"YAML("
  listen: "127.0.0.1:)YAML" << app_port << R"YAML("
  threads: 2

portal: { enabled: false }

models: ["local/model"]
default_model: "local/model"

providers:
  local:
    - id: "local-llama"
      url: "http://127.0.0.1:)YAML" << local.port() << R"YAML(/v1"
      models: { "local/model": "upstream-model" }
      price_per_million: { input: 1.0, output: 2.0, cache_read: 0.25 }
      context_length: 131072
      max_completion_tokens: 4096
      verify_tls: false

consumers:
  - id: "local"
    token_hash: ")YAML" << tokenHash("local-client-token") << R"YAML("
    models: ["*"]

routing: { }

stats:
  sqlite_path: ")YAML" << dir.str() << R"YAML(/stats.db"
  ring_buffer_size: 100
  prometheus: { enabled: false }
)YAML";
    const auto config_path = dir.path() / "config.yaml";
    test::writeFile(config_path, cfg.str());
    const auto log = dir.path() / "process.log";
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready) << readTextFile(log);

    CurlClient client;
    const auto chat = [&](CurlHttpResponse& resp) {
        CurlHttpRequest req;
        req.method = "POST";
        req.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
        req.http2 = false;
        req.verify_tls = false;
        req.headers = {"Authorization: Bearer local-client-token",
                       "Content-Type: application/json",
                       "X-Session-Id: sess-miss"};
        req.body =
            R"({"model":"local/model","messages":[{"role":"user","content":"hi"}],"max_completion_tokens":10})";
        req.timeouts.total_ms = 5000;
        return client.perform(req, resp);
    };

    // Пять запросов подряд без кеша: со второго — предупреждения, с пятого —
    // провайдер в чёрном списке.
    for (int i = 1; i <= 5; ++i) {
        CurlHttpResponse resp;
        ASSERT_TRUE(chat(resp)) << resp.error << " attempt=" << i;
        ASSERT_EQ(resp.status, 200);
    }

    // Серия закрыта: следующий запрос не получает маршрута.
    bool blocked = false;
    for (int attempt = 0; attempt < 20 && !blocked; ++attempt) {
        CurlHttpResponse resp;
        chat(resp); // 503 => perform возвращает false, статус в resp
        if (resp.status == 503) {
            blocked = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    EXPECT_TRUE(blocked) << readTextFile(log);

    app.reset();

    // Причина записана в конфигурацию: провайдер и модель.
    const std::string saved = readTextFile(config_path);
    EXPECT_NE(saved.find("local-llama"), std::string::npos);
    EXPECT_NE(saved.find("local/model"), std::string::npos);
    EXPECT_NE(saved.find("cache_miss_streak"), std::string::npos);

    StatsDb db((dir.path() / "stats.db").string());
    ASSERT_TRUE(db.ok()) << db.lastError();
    // Второй, третий и четвёртый запросы — предупреждения.
    const auto warnings = db.requests("", 50, "", "", 0, IssueFilter::Warning);
    ASSERT_EQ(warnings.size(), 3u);
    for (const auto& x : warnings) {
        EXPECT_EQ(x["error_code"].asString(), kCacheMissStreakCode);
        EXPECT_EQ(x["severity"].asInt(), 1);
    }
    // Пятый (и возможные повторы до блокировки) — ошибка блокировки.
    bool saw_blacklist = false;
    for (const auto& x : db.requests("", 50, "", "", 0, IssueFilter::Error)) {
        if (x["error_code"].asString() != kCacheMissBlacklistCode) continue;
        saw_blacklist = true;
        EXPECT_EQ(x["severity"].asInt(), 2);
        EXPECT_EQ(x["provider"].asString(), "local-llama");
    }
    EXPECT_TRUE(saw_blacklist);
    EXPECT_EQ(db.filteredRequestCount("", "", "", IssueFilter::Warning), 3);
}

TEST(AppIntegration, ProviderLogCsvUploadFromPanel) {
    const int app_port = 18880 + (getpid() % 100);
    test::TempDir dir;
    const auto logs_dir = dir.path() / "provider-data";
    std::filesystem::create_directories(logs_dir);
    const auto cfg_path = dir.path() / "config.yaml";
    std::ostringstream cfg;
    cfg << R"YAML(
proxy:
  name: "analytics-integration"
  data_dir: ")YAML" << dir.str() << R"YAML("
  listen: "127.0.0.1:)YAML" << app_port << R"YAML("
  threads: 2

panel:
  allowed_networks: ["127.0.0.1/32"]

portal:
  users:
    - { username: "admin", password_hash: ")YAML" << adminPasswordHash() << R"YAML(", role: "admin" }

models: ["local/model"]
providers:
  local:
    - id: "local"
      url: "http://127.0.0.1:1/v1"
      models: ["local/*"]
consumers:
  - { id: "dev", token_hash: ")YAML" << tokenHash("dev-token") << R"YAML(", scopes: ["proxy:invoke"], models: ["*"] }

analytics:
  log_dir: ")YAML" << logs_dir.string() << R"YAML("
  snapshot: ")YAML" << (dir.path() / "provider-analytics.json").string() << R"YAML("
  auto_import: false

stats:
  sqlite_path: ")YAML" << dir.str() << R"YAML(/stats.db"
)YAML";
    test::writeFile(cfg_path, cfg.str());
    const auto log = dir.path() / "process.log";
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", cfg_path.string()}, log.string());
    ASSERT_GT(app, 0);

    bool ready = false;
    for (int i = 0; i < 80; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    login.timeouts.total_ms = 3000;
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    EXPECT_EQ(login_resp.status, 302);
    ASSERT_TRUE(login_resp.headers.count("set-cookie"));
    std::string cookie = login_resp.headers.at("set-cookie");
    auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    CurlHttpRequest dashboard;
    dashboard.method = "GET";
    dashboard.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/providers";
    dashboard.http2 = false;
    dashboard.verify_tls = false;
    dashboard.headers = {"Cookie: " + cookie};
    dashboard.timeouts.total_ms = 3000;
    CurlHttpResponse dashboard_resp;
    ASSERT_TRUE(client.perform(dashboard, dashboard_resp));
    EXPECT_EQ(dashboard_resp.status, 200);
    EXPECT_NE(dashboard_resp.body.find("id=\"provider-csv\""), std::string::npos);
    EXPECT_NE(dashboard_resp.body.find("Загрузить CSV"), std::string::npos);
    const std::string csrf = csrfFromPage(dashboard_resp.body);
    ASSERT_FALSE(csrf.empty());

    const std::string csv =
        "created_at (Europe/Moscow);model_name;provider_name;input_tokens;output_tokens;"
        "input_tokens_cost;output_tokens_cost;input_cached_tokens;input_cached_tokens_cost;"
        "amount;latency_sec\n"
        "14-11-2026 13:00:00;mock/model;Fast;100;20;0.001;0.002;40;0.0001;0.0031;2.5\n"
        "14-11-2026 14:00:00;mock/model;Fast;200;30;0.002;0.003;80;0.0002;0.0052;3.5\n";
    CurlHttpRequest upload;
    upload.method = "POST";
    upload.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/provider-logs/import";
    upload.http2 = false;
    upload.verify_tls = false;
    upload.headers = {"Cookie: " + cookie, "Content-Type: text/csv",
                      "X-CSRF-Token: " + csrf};
    upload.body = csv;
    upload.timeouts.total_ms = 5000;
    CurlHttpResponse upload_resp;
    ASSERT_TRUE(client.perform(upload, upload_resp));
    EXPECT_EQ(upload_resp.status, 200);
    EXPECT_NE(upload_resp.body.find("\"rows\":2"), std::string::npos);
    EXPECT_NE(upload_resp.body.find("\"ok\":true"), std::string::npos);

    CurlHttpRequest logs;
    logs.method = "GET";
    logs.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/api/provider-logs";
    logs.http2 = false;
    logs.verify_tls = false;
    logs.headers = {"Cookie: " + cookie};
    logs.timeouts.total_ms = 3000;
    CurlHttpResponse logs_resp;
    ASSERT_TRUE(client.perform(logs, logs_resp));
    EXPECT_EQ(logs_resp.status, 200);
    EXPECT_NE(logs_resp.body.find("\"rows\":2"), std::string::npos);
    EXPECT_NE(logs_resp.body.find("\"models\""), std::string::npos);
    EXPECT_NE(logs_resp.body.find("\"provider\":\"Fast\""), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "provider-analytics.json"));
    // Raw CSV is intentionally not stored.
    EXPECT_TRUE(std::filesystem::is_empty(logs_dir));

    app.reset();
}

TEST(AppIntegration, ProviderDownOnEmptyAnswerAndRecoversByProbe) {
    const int app_port = 18430 + (getpid() % 100);
    const int mock_port = 19430 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    // Пустой ответ провайдера — отказ: серия уводит его из маршрута, а проверка
    // доступности возвращает обратно, не дожидаясь живого запроса.
    std::string cfg_text = configFor(app_port, mock_port, dir.str());
    const std::string base_routing = "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0 }";
    ASSERT_NE(cfg_text.find(base_routing), std::string::npos);
    cfg_text.replace(cfg_text.find(base_routing), base_routing.size(),
                     "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0, retry: { attempts: 0 }, "
                     "health_fail_threshold: 1, health_down_after_sec: 0, "
                     "health_probe_interval_sec: 5, health_recover_successes: 1 }");
    test::writeFile(config_path, cfg_text);
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedEnvVar empty_guard("MOCK_EMPTY_PROVIDERS", "fast:1");
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    auto chat = [&](const char* content) {
        CurlHttpRequest req;
        req.method = "POST";
        req.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
        req.http2 = false;
        req.verify_tls = false;
        req.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
        req.body = R"({"model":"default","messages":[{"role":"user","content":")" +
                   std::string(content) + R"("}]})";
        req.timeouts.total_ms = 5000;
        CurlHttpResponse resp;
        EXPECT_TRUE(client.perform(req, resp));
        return resp;
    };

    const auto first = chat("hi");
    ASSERT_EQ(first.status, 200);
    // Первый провайдер ответил пустым 200: запрос ушёл следующему по очереди.
    EXPECT_NE(first.body.find("hello from slow"), std::string::npos);

    CurlHttpRequest login;
    login.method = "POST";
    login.url = "http://127.0.0.1:" + std::to_string(app_port) + "/panel/login";
    login.http2 = false;
    login.verify_tls = false;
    login.headers = {"Content-Type: application/x-www-form-urlencoded"};
    login.body = "username=admin&password=admin";
    CurlHttpResponse login_resp;
    ASSERT_TRUE(client.perform(login, login_resp));
    ASSERT_TRUE(login_resp.headers.count("set-cookie"));
    std::string cookie = login_resp.headers.at("set-cookie");
    const auto semi = cookie.find(';');
    if (semi != std::string::npos) cookie = cookie.substr(0, semi);

    auto panel_get = [&](const std::string& path) {
        CurlHttpRequest req;
        req.method = "GET";
        req.url = "http://127.0.0.1:" + std::to_string(app_port) + path;
        req.http2 = false;
        req.verify_tls = false;
        req.headers = {"Cookie: " + cookie};
        req.timeouts.total_ms = 3000;
        CurlHttpResponse resp;
        EXPECT_TRUE(client.perform(req, resp));
        return resp;
    };

    const auto overview = panel_get("/panel/api/overview");
    ASSERT_EQ(overview.status, 200);
    EXPECT_NE(overview.body.find("\"provider\":\"fast\""), std::string::npos);
    EXPECT_NE(overview.body.find("\"down\":true"), std::string::npos);

    const auto dashboard = panel_get("/");
    ASSERT_EQ(dashboard.status, 200);
    EXPECT_NE(dashboard.body.find("Провайдер не работает"), std::string::npos);
    EXPECT_NE(dashboard.body.find("причина: пустой ответ"), std::string::npos);

    // Проверка доступности возвращает провайдера; следующий запрос снова дешевле.
    bool recovered = false;
    for (int i = 0; i < 100; ++i) {
        const auto current = panel_get("/panel/api/overview");
        if (current.body.find("\"down\":true") == std::string::npos &&
            current.body.find("\"provider\":\"fast\"") != std::string::npos) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ASSERT_TRUE(recovered);
    const auto second = chat("again");
    ASSERT_EQ(second.status, 200);
    EXPECT_NE(second.body.find("hello from fast"), std::string::npos);

    // Журнал: пустой ответ записан как ошибка провайдера с повтором у другого.
    const auto logs = panel_get("/panel/api/requests?limit=5");
    ASSERT_EQ(logs.status, 200);
    EXPECT_NE(logs.body.find("\"attempts\":2"), std::string::npos);
    EXPECT_NE(logs.body.find("empty completion"), std::string::npos);
    const auto statsPage = panel_get("/panel/stats");
    ASSERT_EQ(statsPage.status, 200);
    EXPECT_NE(statsPage.body.find("пустой ответ"), std::string::npos);

    app.reset();
    mock.reset();
}

TEST(AppIntegration, WaitsForCurrentProviderBeforeFallback) {
    const int app_port = 18450 + (getpid() % 100);
    const int mock_port = 19450 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    // Провайдер ответил пустым 200 один раз: пока цена ожидания ниже разницы
    // стоимости следующего оффера, попытки идут к нему же — fallback не наступает.
    std::string cfg_text = configFor(app_port, mock_port, dir.str());
    const std::string base_routing =
        "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0 }";
    ASSERT_NE(cfg_text.find(base_routing), std::string::npos);
    cfg_text.replace(cfg_text.find(base_routing), base_routing.size(),
                     "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0.001, "
                     "failover_wait_max_sec: 60, retry: { attempts: 0, backoff_ms: 10 } }");
    test::writeFile(config_path, cfg_text);
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedEnvVar empty_guard("MOCK_EMPTY_PROVIDERS", "fast:1");
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    CurlHttpRequest chat;
    chat.method = "POST";
    chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    chat.http2 = false;
    chat.verify_tls = false;
    chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}]})";
    chat.timeouts.total_ms = 5000;
    CurlHttpResponse resp;
    ASSERT_TRUE(client.perform(chat, resp));
    ASSERT_EQ(resp.status, 200);
    EXPECT_NE(resp.body.find("hello from fast"), std::string::npos);

    app.reset();
    mock.reset();

    std::ifstream log_in(log.string());
    const std::string log_text((std::istreambuf_iterator<char>(log_in)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(log_text.find("session rebind on provider switch"), std::string::npos);

    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2((dir.path() / "stats.db").string().c_str(), &db,
                              SQLITE_OPEN_READONLY, nullptr),
              SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
                                 "SELECT provider_tag, attempts, error_code, event_detail "
                                 "FROM requests ORDER BY ts DESC LIMIT 1",
                                 -1, &st, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "fast");
    EXPECT_EQ(sqlite3_column_int(st, 1), 2);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2))), "");
    EXPECT_NE(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3)))
                  .find("empty completion"),
              std::string::npos);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

TEST(AppIntegration, StreamEmptyAnswerSwitchesProvider) {
    const int app_port = 18440 + (getpid() % 100);
    const int mock_port = 19440 + (getpid() % 100);
    test::TempDir dir;
    const auto config_path = dir.path() / "config.yaml";
    std::string cfg_text = configFor(app_port, mock_port, dir.str());
    const std::string base_routing = "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0 }";
    ASSERT_NE(cfg_text.find(base_routing), std::string::npos);
    cfg_text.replace(cfg_text.find(base_routing), base_routing.size(),
                     "routing: { fallback_attempts: 1, wait_price_rub_per_sec: 0, failover_wait_max_sec: 0, retry: { attempts: 0 }, "
                     "health_fail_threshold: 0, health_down_after_sec: 0 }");
    test::writeFile(config_path, cfg_text);
    const auto log = dir.path() / "process.log";

    setenv("MOCK_ROUTERAI_PORT", std::to_string(mock_port).c_str(), 1);
    ScopedEnvVar empty_guard("MOCK_EMPTY_PROVIDERS", "fast:1");
    ScopedProcess mock({"python3", std::string(COSTRELAY_SOURCE_DIR) + "/scripts/mock_routerai.py"},
                              log.string());
    ASSERT_GT(mock, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ScopedProcess app({std::string(COSTRELAY_BINARY), "--config", config_path.string()}, log.string());
    ASSERT_GT(app, 0);
    bool ready = false;
    for (int i = 0; i < 60; ++i) {
        long status = 0;
        std::string body;
        httpGet("http://127.0.0.1:" + std::to_string(app_port) + "/readyz", status, body);
        if (status == 200) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready);

    CurlClient client;
    CurlHttpRequest chat;
    chat.method = "POST";
    chat.url = "http://127.0.0.1:" + std::to_string(app_port) + "/v1/chat/completions";
    chat.http2 = false;
    chat.verify_tls = false;
    chat.headers = {"Authorization: Bearer dev-token", "Content-Type: application/json"};
    chat.body = R"({"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true})";
    chat.timeouts.total_ms = 5000;
    CurlHttpResponse resp;
    ASSERT_TRUE(client.perform(chat, resp, [&](const char* data, std::size_t len) {
        resp.body.append(data, len);
        return true;
    }));
    // Пустой поток первого провайдера не ушёл клиенту: заголовки ждут первый
    // содержательный фрагмент, поэтому запрос ушёл следующему по очереди.
    EXPECT_EQ(resp.status, 200);
    EXPECT_NE(resp.body.find("from slow"), std::string::npos);
    ASSERT_TRUE(resp.headers.count("x-proxy-provider"));
    EXPECT_EQ(resp.headers.at("x-proxy-provider"), "slow");

    app.reset();
    mock.reset();
}
