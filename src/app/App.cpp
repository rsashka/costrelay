#include "app/App.h"
#include "app/Endpoints.h"
#include "util/Http.h"
#include "util/Strings.h"

#include <drogon/drogon.h>
#include <drogon/WebSocketClient.h>
#include <drogon/WebSocketController.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>

namespace proxy {
namespace {

// Буфер кадров WebSocket до готовности апстрима, кадров.
constexpr std::size_t kWsPendingLimit = 1024;
// Периодическая уборка захваченных тел, сессий и журнала — раз в час.
constexpr double kRetentionTickSec = 3600.0;

bool isPanelPath(std::string_view path, const std::string& base) {
    return path == base || startsWith(path, base + "/");
}

// Первый сегмент пути: ссылка на локальное приложение портала.
std::string firstPathSegment(std::string_view path) {
    const auto start = path.find_first_not_of('/');
    if (start == std::string_view::npos) return {};
    const auto end = path.find('/', start);
    return std::string(path.substr(start, end == std::string_view::npos ? end : end - start));
}

bool wsHopByHop(const std::string& name) {
    const std::string n = toLower(name);
    if (isHopByHopHeader(n)) return true;
    return n == "origin" || n == "referer" || n == "cookie" ||
           n == "authorization" || n == "proxy-authorization" ||
           startsWith(n, "sec-websocket-");
}

struct WsBridge {
    std::weak_ptr<drogon::WebSocketConnection> client;
    drogon::WebSocketClientPtr upstream;
    std::mutex mutex;
    bool ready = false;
    bool closed = false;
    std::vector<std::pair<std::string, drogon::WebSocketMessageType>> pending;
};

void closeWs(const std::shared_ptr<WsBridge>& bridge,
             drogon::CloseCode code,
             const std::string& reason) {
    if (auto client = bridge->client.lock(); client && client->connected()) {
        client->shutdown(code, reason);
    }
}

class PortalWsController final
    : public drogon::WebSocketController<PortalWsController, false> {
  public:
    explicit PortalWsController(ProxyApp* app) : app_(app) {}

    static void initPathRouting() {
        WS_ADD_PATH_VIA_REGEX(R"(^/([a-z0-9_-]+)(/.*)$)", drogon::Get);
    }

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override {
        const std::string& path = req->path();
        const auto slash = path.find('/', 1);
        if (path.size() < 2 || path.front() != '/' || slash == std::string::npos) {
            conn->shutdown(drogon::CloseCode::kViolation, "invalid websocket path");
            return;
        }
        const std::string app_id = path.substr(1, slash - 1);
        const std::string rest = path.substr(slash);
        auto target = app_->portalWsTarget(req, app_id, rest);
        if (!target) {
            conn->shutdown(drogon::CloseCode::kViolation, "forbidden");
            return;
        }

        auto bridge = std::make_shared<WsBridge>();
        bridge->client = conn;
        conn->setContext(bridge);

        auto upstream = drogon::WebSocketClient::newWebSocketClient(
            target->host, drogon::app().getLoop(), false, target->verify_tls);
        if (!upstream) {
            conn->shutdown(drogon::CloseCode::kUnexpectedCondition,
                           "cannot create upstream websocket");
            return;
        }
        bridge->upstream = upstream;

        upstream->setMessageHandler(
            [bridge](std::string&& message,
                     const drogon::WebSocketClientPtr&,
                     const drogon::WebSocketMessageType& type) {
                auto client = bridge->client.lock();
                if (!client || client->disconnected()) return;
                client->send(std::move(message), type);
            });
        upstream->setConnectionClosedHandler(
            [bridge](const drogon::WebSocketClientPtr&) {
                {
                    std::lock_guard<std::mutex> lock(bridge->mutex);
                    bridge->ready = false;
                    bridge->closed = true;
                    bridge->pending.clear();
                }
                closeWs(bridge, drogon::CloseCode::kNormalClosure, "upstream closed");
            });

        auto out = drogon::HttpRequest::newHttpRequest();
        out->setMethod(drogon::Get);
        out->setPath(target->path);
        out->setPathEncode(false);
        for (const auto& [name, value] : target->headers) {
            out->addHeader(name, value);
        }

        upstream->connectToServer(
            out,
            [bridge](drogon::ReqResult result,
                     const drogon::HttpResponsePtr&,
                     const drogon::WebSocketClientPtr&) {
                std::vector<std::pair<std::string, drogon::WebSocketMessageType>> pending;
                drogon::WebSocketClientPtr upstream;
                bool failed = result != drogon::ReqResult::Ok;
                {
                    std::lock_guard<std::mutex> lock(bridge->mutex);
                    if (failed || bridge->closed) {
                        failed = true;
                        bridge->closed = true;
                        bridge->ready = false;
                        bridge->pending.clear();
                    } else {
                        bridge->ready = true;
                        upstream = bridge->upstream;
                        pending.swap(bridge->pending);
                    }
                }
                if (failed) {
                    closeWs(bridge, drogon::CloseCode::kUnexpectedCondition,
                            "upstream websocket failed");
                    return;
                }
                auto up = upstream ? upstream->getConnection() : nullptr;
                if (!up || !up->connected()) {
                    closeWs(bridge, drogon::CloseCode::kUnexpectedCondition,
                            "upstream websocket unavailable");
                    return;
                }
                for (auto& item : pending) {
                    up->send(std::move(item.first), item.second);
                }
            });
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override {
        auto bridge = conn->getContext<WsBridge>();
        if (!bridge) return;
        drogon::WebSocketConnectionPtr upstream;
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(bridge->mutex);
            if (bridge->closed) return;
            if (!bridge->ready || !bridge->upstream) {
                if (bridge->pending.size() >= kWsPendingLimit) {
                    overflow = true;
                } else {
                    bridge->pending.emplace_back(std::move(message), type);
                }
            } else {
                upstream = bridge->upstream->getConnection();
            }
        }
        if (overflow) {
            conn->shutdown(drogon::CloseCode::kMessageTooBig, "websocket queue overflow");
            return;
        }
        if (upstream && upstream->connected()) {
            upstream->send(std::move(message), type);
        }
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        auto bridge = conn->getContext<WsBridge>();
        if (!bridge) return;
        drogon::WebSocketClientPtr upstream;
        {
            std::lock_guard<std::mutex> lock(bridge->mutex);
            bridge->closed = true;
            bridge->ready = false;
            bridge->pending.clear();
            upstream = std::move(bridge->upstream);
        }
        if (upstream) upstream->stop();
    }

  private:
    ProxyApp* app_;
};

} // namespace

ProxyApp::ProxyApp(AppConfig config)
    : config_(std::move(config)),
      tokens_(config_.tokens),
      sessions_(config_.panel.absolute_ttl_sec, config_.panel.idle_ttl_sec),
      login_limiter_(config_.panel.login_attempts, config_.panel.login_window_sec,
                     config_.panel.login_lockout_sec),
      analytics_(config_.analytics),
      router_(config_, catalog_),
      stats_(config_.stats.ring_size, config_.routing.cache_loss_spike, StatsLimits{},
             config_.routing.cache_miss),
      queue_(config_, router_, catalog_, stats_),
      bodies_(config_.capture),
      fetcher_(config_, catalog_),
      proxy_(config_, catalog_, queue_, stats_, bodies_, &quotas_),
      portal_proxy_(config_.panel.cookie_name) {
    std::filesystem::create_directories(config_.data_dir);
    if (analytics_.enabled()) {
        if (!config_.analytics.snapshot_file.empty()) {
            if (!analytics_.loadSnapshot()) {
                LOG_WARN << "provider analytics snapshot not loaded: "
                         << analytics_.lastError();
            }
            if (!analytics_.saveSnapshot()) {
                LOG_WARN << "provider analytics snapshot not saved: " << analytics_.lastError();
            }
        }
        if (config_.analytics.auto_import) {
            auto imported = analytics_.importDirectory();
            if (imported.ok) {
                LOG_INFO << "provider CSV import: files=" << imported.files
                         << " rows=" << imported.rows << " skipped=" << imported.skipped;
            } else {
                LOG_WARN << "provider CSV import failed: " << imported.error;
            }
        }
        if (!config_.analytics.snapshot_file.empty() && !analytics_.saveSnapshot()) {
            LOG_WARN << "provider analytics snapshot not saved: " << analytics_.lastError();
        }
    }
    if (config_.stats.enabled) {
        const std::string path = config_.stats.sqlite_path.empty()
                                     ? (config_.data_dir + "/stats.db")
                                     : config_.stats.sqlite_path;
        stats_db_ = std::make_unique<StatsDb>(path);
        if (stats_db_->ok()) {
            stats_.attach(stats_db_.get());
            quotas_.attach(stats_db_.get());
            const int removed = stats_db_->purge(config_.stats.requests_retention_days,
                                                config_.stats.aggregates_retention_days);
            if (removed > 0) LOG_INFO << "stats retention removed " << removed << " rows";
        } else {
            LOG_WARN << "stats sqlite disabled: " << stats_db_->lastError();
        }
    }
    std::string error;
    if (!bodies_.init(error)) throw std::runtime_error("body store init failed: " + error);
    for (const auto& u : config_.panel.users) {
        users_[u.username] = UserRecord{u.password_hash, u.role};
    }
    dummy_hash_ = PasswordHash::hash("dummy-" + randomHex(16));
    proxy_.setOnCacheMissBlacklist(
        [this](const std::string& provider, const std::string& model, const std::string& reason) {
            blacklistProviderFromStats(provider, model, reason);
        });
}

std::string ProxyApp::panelBase() const { return config_.panel.base_path; }

bool ProxyApp::panelNetworkAllowed(const drogon::HttpRequestPtr& req) const {
    if (config_.panel.allowed_networks.empty()) return true;
    const std::string ip = clientIp(req);
    for (const auto& net : config_.panel.allowed_networks) {
        if (ipInCidr(ip, net)) return true;
    }
    return false;
}

drogon::HttpResponsePtr ProxyApp::redirect(const std::string& location) const {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k302Found);
    resp->addHeader("Location", location);
    return resp;
}

std::optional<Session> ProxyApp::sessionFor(const drogon::HttpRequestPtr& req) {
    const auto& cookie = req->getCookie(config_.panel.cookie_name);
    if (cookie.empty()) return std::nullopt;
    return sessions_.touch(cookie);
}

std::optional<Session> ProxyApp::requireSession(
    const drogon::HttpRequestPtr& req,
    const std::function<void(const drogon::HttpResponsePtr&)>& cb, bool page) {
    auto session = sessionFor(req);
    if (!session) {
        if (page) cb(redirect(panelBase() + "/login"));
        else cb(errorResponse(drogon::k401Unauthorized, "unauthorized", "panel session required"));
    }
    return session;
}

std::string ProxyApp::portalHttpsUrl(const drogon::HttpRequestPtr& req) const {
    // Адрес HTTPS берётся из конфигурации, а не из заголовка Host: иначе клиент
    // подставляет свой хост и получает редирект на чужой адрес.
    std::string base = config_.server.public_url;
    if (base.rfind("https://", 0) != 0) {
        const std::string port = config_.server.remote_https_port == 443
                                     ? std::string()
                                     : ":" + std::to_string(config_.server.remote_https_port);
        base = "https://" + httpsUrlHost(config_.server.remote_https_listen) + port;
    }
    std::string url = base + req->path();
    if (!req->query().empty()) url += "?" + req->query();
    return url;
}

const PortalAppConfig* ProxyApp::findPortalApp(const std::string& id) const {
    for (const auto& app : config_.portal.apps) {
        if (app.id == id) return &app;
    }
    return nullptr;
}

std::optional<ProxyApp::PortalWsTarget> ProxyApp::portalWsTarget(
    const drogon::HttpRequestPtr& req, const std::string& app_id,
    const std::string& rest) {
    if (!config_.portal.enabled) return std::nullopt;
    if (config_.server.remote_https_enabled && !req->isOnSecureConnection()) return std::nullopt;

    if (!config_.panel.allowed_networks.empty()) {
        if (!panelNetworkAllowed(req)) return std::nullopt;
    }

    const PortalAppConfig* app = findPortalApp(app_id);
    if (!app) return std::nullopt;
    auto session = sessionFor(req);
    if (!session) return std::nullopt;
    if (!portalRoleAllowed(*app, session->role)) {
        return std::nullopt;
    }
    if (rest.empty() || rest.front() != '/') return std::nullopt;

    const UrlBase app_url = splitUrlBase(app->url);
    const std::string origin = app_url.origin;
    const std::string base = app_url.base_path;
    const std::string hostport = urlHostPort(app->url);
    if (origin.empty() || hostport.empty()) return std::nullopt;

    PortalWsTarget target;
    const bool tls = startsWith(app->url, "https://");
    target.host = (tls ? "wss://" : "ws://") + hostport;
    target.origin = origin;
    target.verify_tls = app->verify_tls;
    target.path = base + rest;
    if (!req->query().empty()) target.path += "?" + req->query();

    for (const auto& [name, value] : req->headers()) {
        if (wsHopByHop(name)) continue;
        target.headers.emplace_back(name, value);
    }

    const std::string cookies = portalRequestCookies(req, config_.panel.cookie_name);
    if (!cookies.empty()) target.headers.emplace_back("Cookie", cookies);

    target.headers.emplace_back("Origin", origin);
    target.headers.emplace_back("Referer", origin + base + rest);
    target.headers.emplace_back("X-Forwarded-For", clientIp(req));
    target.headers.emplace_back("X-Forwarded-Proto",
                                req->isOnSecureConnection() ? "https" : "http");
    target.headers.emplace_back("X-Forwarded-Host", req->getHeader("host"));
    target.headers.emplace_back("X-Forwarded-Prefix", "/" + app->id);
    for (const auto& [name, value] : app->headers) {
        target.headers.emplace_back(name, value);
    }
    return target;
}

void ProxyApp::handlePortalApp(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                               const std::string& app_id, const std::string& rest) {
    // Сеть панели ограничивает и локальные приложения: путь портала — тот же
    // периметр, что у панели, а не отдельная дверь.
    if (!panelNetworkAllowed(req)) {
        cb(errorResponse(drogon::k403Forbidden, "forbidden", "panel network is not allowed"));
        return;
    }
    if (config_.server.remote_https_enabled && !req->isOnSecureConnection()) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k308PermanentRedirect);
        resp->addHeader("Location", portalHttpsUrl(req));
        cb(resp);
        return;
    }
    const PortalAppConfig* app = findPortalApp(app_id);
    if (!app) {
        cb(errorResponse(drogon::k404NotFound, "not_found", "portal app not found"));
        return;
    }
    const std::string accept = req->getHeader("accept");
    const bool page = accept.find("text/html") != std::string::npos ||
                      req->getHeader("sec-fetch-mode") == "navigate";
    auto session = requireSession(req, cb, page);
    if (!session) return;
    if (!portalRoleAllowed(*app, session->role)) {
        cb(errorResponse(drogon::k403Forbidden, "forbidden",
                         "role is not allowed for this portal app"));
        return;
    }
    if (!rest.empty() && rest.front() != '/') {
        cb(errorResponse(drogon::k404NotFound, "not_found", "portal app not found"));
        return;
    }
    const std::string prefix = "/" + app->id;
    if (rest.empty()) {
        std::string location = prefix + "/";
        if (!req->query().empty()) location += "?" + req->query();
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k302Found);
        resp->addHeader("Location", location);
        cb(resp);
        return;
    }
    portal_proxy_.forward(*app, prefix, req, std::move(cb));
}

void ProxyApp::handleLoginPage(const drogon::HttpRequestPtr&,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    cb(loginPage());
}

void ProxyApp::handleLoginPost(const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    const std::string ip = clientIp(req);
    const std::string username = req->getParameter("username");
    const std::string password = req->getParameter("password");
    const std::string key = ip + "|" + username;
    if (!login_limiter_.allow(key)) {
        auto resp = loginPage("too many attempts; try later");
        resp->setStatusCode(drogon::k429TooManyRequests);
        resp->addHeader("Retry-After", std::to_string(config_.panel.login_lockout_sec));
        cb(resp);
        return;
    }
    auto it = users_.find(username);
    bool ok = false;
    if (it != users_.end()) {
        ok = PasswordHash::verify(password, it->second.password_hash);
    } else {
        PasswordHash::verify(password, dummy_hash_);
    }
    if (!ok) {
        login_limiter_.failure(key);
        auto resp = loginPage("invalid username or password");
        resp->setStatusCode(drogon::k401Unauthorized);
        cb(resp);
        return;
    }
    login_limiter_.success(key);
    const std::string role = users_.at(username).role;
    auto session = sessions_.create(username, role);
    if (!session) {
        cb(loginPage("session store unavailable"));
        return;
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k302Found);
    resp->addHeader("Location", "/");
    drogon::Cookie cookie(config_.panel.cookie_name, session->id);
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
    cookie.setSecure(config_.server.remote_https_enabled);
    cookie.setMaxAge(config_.panel.absolute_ttl_sec);
    resp->addCookie(cookie);
    cb(resp);
}

void ProxyApp::handleLogout(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    auto session = sessionFor(req);
    if (!session) {
        cb(redirect(panelBase() + "/login"));
        return;
    }
    const std::string csrf = !req->getParameter("csrf").empty()
                                 ? req->getParameter("csrf")
                                 : std::string(req->getHeader("x-csrf-token"));
    if (!sessions_.validateCsrf(session->id, csrf)) {
        cb(errorResponse(drogon::k403Forbidden, "csrf_failed", "CSRF token mismatch"));
        return;
    }
    sessions_.destroy(session->id);
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k302Found);
    resp->addHeader("Location", panelBase() + "/login");
    drogon::Cookie cookie(config_.panel.cookie_name, "");
    cookie.setPath("/");
    cookie.setHttpOnly(true);
    cookie.setMaxAge(0);
    resp->addCookie(cookie);
    cb(resp);
}

void ProxyApp::handleHome(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
    if (config_.server.remote_https_enabled && !req->isOnSecureConnection()) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k308PermanentRedirect);
        resp->addHeader("Location", portalHttpsUrl(req));
        cb(resp);
        return;
    }
    auto session = requireSession(req, cb, true);
    if (!session) return;
    cb(dashboardPage(*session));
}

void ProxyApp::setupRoutes() {
    auto& app = drogon::app();
    const std::string base = panelBase();

    if (config_.portal.enabled) {
        using Handler = void (ProxyApp::*)(const drogon::HttpRequestPtr&,
                                           std::function<void(const drogon::HttpResponsePtr&)>&&);
        auto add = [&](const std::string& path, drogon::HttpMethod method, Handler handler) {
            app.registerHandler(path,
                                [this, handler](const drogon::HttpRequestPtr& req,
                                                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                                    (this->*handler)(req, std::move(cb));
                                },
                                {method});
        };
        add(base + "/login", drogon::Get, &ProxyApp::handleLoginPage);
        add(base + "/login", drogon::Post, &ProxyApp::handleLoginPost);
        add(base + "/logout", drogon::Post, &ProxyApp::handleLogout);
        add(base + "/stats", drogon::Get, &ProxyApp::handleStatsPage);
        add(base + "/providers", drogon::Get, &ProxyApp::handleProvidersPage);
        add(base + "/routing", drogon::Get, &ProxyApp::handleRoutingPage);
        add(base + "/api/service-restart", drogon::Post, &ProxyApp::handleServiceRestart);
        add(base + "/api/routing-options", drogon::Get, &ProxyApp::handleRoutingOptions);
        add(base + "/api/routing-options", drogon::Post, &ProxyApp::handleRoutingOptions);
        add(base + "/api/cache-reset", drogon::Post, &ProxyApp::handleCacheReset);
        add(base + "/api/overview", drogon::Get, &ProxyApp::handleOverview);
        add(base + "/api/routers", drogon::Get, &ProxyApp::handleRouters);
        add(base + "/api/explain", drogon::Get, &ProxyApp::handleExplain);
        add(base + "/api/routing-preview", drogon::Get, &ProxyApp::handleRoutingPreview);
        add(base + "/api/requests", drogon::Get, &ProxyApp::handleRequests);
        add(base + "/api/requests-delete", drogon::Post, &ProxyApp::handleRequestsDelete);
        add(base + "/api/stats/daily", drogon::Get, &ProxyApp::handleDailyStats);
        add(base + "/api/sessions", drogon::Get, &ProxyApp::handleSessions);
        add(base + "/api/provider-logs", drogon::Get, &ProxyApp::handleProviderLogs);
        add(base + "/api/provider-logs/import", drogon::Post, &ProxyApp::handleProviderLogImport);
        add(base + "/api/provider-logs/reload", drogon::Post, &ProxyApp::handleProviderLogReload);
        add(base + "/api/blacklist", drogon::Get, &ProxyApp::handleBlacklistGet);
        add(base + "/api/blacklist", drogon::Post, &ProxyApp::handleBlacklistSave);
        add(base + "/api/request-body", drogon::Get, &ProxyApp::handleBody);
    }
    if (config_.portal.enabled) {
        app.registerHandler("/",
                            [this](const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                                handleHome(req, std::move(cb));
                            },
                            {drogon::Get});
        if (!config_.portal.apps.empty()) {
            std::string ids;
            for (const auto& app : config_.portal.apps) {
                if (!ids.empty()) ids += "|";
                ids += app.id;
            }
            app.registerHandlerViaRegex(
                "^/(" + ids + ")(?=/|$)(.*)$",
                [this](const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       const std::string& app_id, const std::string& rest) {
                    handlePortalApp(req, std::move(cb), app_id, rest);
                });
            app.registerController(std::make_shared<PortalWsController>(this));
        }
    }

    app.registerHandler("/healthz",
                        [](const drogon::HttpRequestPtr&,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                            Json::Value v(Json::objectValue);
                            v["status"] = "ok";
                            cb(jsonResponse(v));
                        },
                        {drogon::Get});
    if (config_.stats.prometheus) {
        app.registerHandler("/metrics",
                            [this](const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                                handleMetrics(req, std::move(cb));
                            },
                            {drogon::Get});
    }
    app.registerHandler("/readyz",
                        [this](const drogon::HttpRequestPtr&,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                            Json::Value v(Json::objectValue);
                            // Тот же критерий пригодности цен, что у очереди:
                            // Router::priceState (свежесть, require_fresh, прямой роутер).
                            std::size_t routable = 0;
                            for (const auto& model : config_.routing.models) {
                                const PriceState state = router_.priceState(model);
                                if (state.usable || state.has_direct) ++routable;
                            }
                            const bool ready = !config_.routers.empty() && routable > 0;
                            v["ready"] = ready;
                            v["routers"] = static_cast<Json::UInt64>(config_.routers.size());
                            v["routable_models"] = static_cast<Json::UInt64>(routable);
                            v["catalog_models"] = static_cast<Json::UInt64>(catalog_.size());
                            v["price_error"] = fetcher_.lastError();
                            cb(jsonResponse(v, ready ? drogon::k200OK : drogon::k503ServiceUnavailable));
                        },
                        {drogon::Get});

    app.registerHandler("/v1/models",
                        [this](const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                            auto principal = tokens_.authenticate(req->getHeader("authorization"), clientIp(req));
                            if (!principal) {
                                cb(errorResponse(drogon::k401Unauthorized, "unauthorized",
                                                 "valid Bearer token required"));
                                return;
                            }
                            cb(jsonResponse(proxy_.modelsJson()));
                        },
                        {drogon::Get});
    app.registerHandler("/v1/chat/completions",
                        [this](const drogon::HttpRequestPtr& req,
                               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                            auto principal = tokens_.authenticate(req->getHeader("authorization"), clientIp(req));
                            if (!principal) {
                                cb(errorResponse(drogon::k401Unauthorized, "unauthorized",
                                                 "valid Bearer token required"));
                                return;
                            }
                            proxy_.handleChat(req, *principal, std::move(cb));
                        },
                        {drogon::Post});

    if (config_.portal.enabled) {
        app.registerPreRoutingAdvice([this, base](const drogon::HttpRequestPtr& req,
                                                  drogon::AdviceCallback&& ac,
                                                  drogon::AdviceChainCallback&& acc) {
            // Корень — главная страница панели после входа: тот же периметр, что
            // и остальные пути панели.
            const bool panel_path = isPanelPath(req->path(), base) || req->path() == "/";
            const bool portal_path =
                !panel_path && findPortalApp(firstPathSegment(req->path())) != nullptr;
            if (panel_path || portal_path) {
                if (!panelNetworkAllowed(req)) {
                    ac(errorResponse(drogon::k403Forbidden, "forbidden",
                                     "panel network is not allowed"));
                    return;
                }
                if (panel_path && config_.server.remote_https_enabled &&
                    !req->isOnSecureConnection()) {
                    ac(errorResponse(drogon::k403Forbidden, "insecure_panel",
                                     "panel requires HTTPS"));
                    return;
                }
            }
            acc();
        });
    }
    if (config_.server.require_https) {
        app.registerPreRoutingAdvice([](const drogon::HttpRequestPtr& req,
                                        drogon::AdviceCallback&& ac,
                                        drogon::AdviceChainCallback&& acc) {
            // Loopback-трафик не покидает хост: локальные агенты ходят по HTTP,
            // всё сетевое — только по HTTPS.
            if (startsWith(req->path(), "/v1") && !req->isOnSecureConnection() &&
                !isLoopbackIp(clientIp(req))) {
                ac(errorResponse(drogon::k403Forbidden, "insecure_api",
                                 "network clients must use HTTPS"));
                return;
            }
            acc();
        });
    }
}

int ProxyApp::run() {
    auto& app = drogon::app();
    app.setThreadNum(static_cast<size_t>(std::max(1, config_.server.threads)));
    app.setClientMaxBodySize(config_.server.max_body_bytes);
    app.setClientMaxMemoryBodySize(config_.server.max_body_bytes);
    // Drogon defaults the upload root to "uploads" relative to the process cwd
    // and creates its temp shards at startup. Point it at the state directory so
    // the listener starts from any working directory.
    app.setUploadPath((std::filesystem::path(config_.data_dir) / "uploads").string());
    app.addListener(config_.server.listen, static_cast<uint16_t>(config_.server.port));
    if (config_.server.remote_https_enabled && !config_.server.tls_cert_file.empty() &&
        !config_.server.tls_key_file.empty()) {
        app.addListener(config_.server.remote_https_listen,
                        static_cast<uint16_t>(config_.server.remote_https_port), true,
                        config_.server.tls_cert_file, config_.server.tls_key_file);
    }
    if (!config_.pricing.snapshot_file.empty() &&
        !catalog_.loadFromFile(config_.pricing.snapshot_file)) {
        LOG_WARN << "price snapshot not loaded: " << config_.pricing.snapshot_file;
    }
    // Очередь строится до первого запроса и дальше живёт по расписанию.
    queue_.start();
    proxy_.start();
    setupRoutes();
    bodies_.gc();
    app.getLoop()->runEvery(kRetentionTickSec, [this] {
        bodies_.gc();
        sessions_.gc();
        if (stats_db_) {
            stats_db_->purge(config_.stats.requests_retention_days,
                             config_.stats.aggregates_retention_days);
            if (!stats_db_->ok()) {
                LOG_WARN << "stats retention failed: " << stats_db_->lastError();
            }
        }
    });
    std::signal(SIGINT, [](int) { drogon::app().quit(); });
    std::signal(SIGTERM, [](int) { drogon::app().quit(); });
    if (config_.priceSourceRouter()) {
        fetcher_.setOnRefresh([this] { queue_.notifyExternalChange(); });
        fetcher_.start();
    }
    LOG_INFO << "costrelay listening on " << config_.server.listen << ":" << config_.server.port;
    const std::string base = panelBase();
    for (const auto& url : panelUrls(config_)) {
        std::string web = url;
        if (web.size() >= base.size() &&
            web.compare(web.size() - base.size(), base.size(), base) == 0) {
            web.resize(web.size() - base.size());
        }
        if (web.empty() || web.back() != '/') web += '/';
        LOG_INFO << "web UI URL: " << web;
    }
    logAgentEndpoints();
    // Логгер буферизует INFO построчно: сбрасываем поток, чтобы URL панели
    // был виден сразу и при перенаправлении stdout в файл службы.
    std::fflush(stdout);
    app.run();
    proxy_.stop();
    queue_.stop();
    fetcher_.stop();
    return 0;
}

} // namespace proxy
