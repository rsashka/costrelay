#pragma once

#include "analytics/ProviderAnalytics.h"
#include "auth/Auth.h"
#include "capture/BodyStore.h"
#include "config/Config.h"
#include "pricing/PriceCatalog.h"
#include "limits/QuotaManager.h"
#include "portal/PortalProxy.h"
#include "pricing/PriceFetcher.h"
#include "proxy/ProxyService.h"
#include "routing/ProviderQueue.h"
#include "routing/Router.h"
#include "stats/Stats.h"
#include "stats/StatsDb.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace proxy {

// Пределы выборок панели и её API: одно место для обработчиков и страниц.
namespace panel_limits {
constexpr std::size_t kLogDefault = 200;
constexpr std::size_t kLogMax = 500;
constexpr std::size_t kListDefault = 50;
constexpr std::size_t kListMax = 500;
constexpr std::size_t kSessionListMax = 1000;
constexpr int kDailyDaysDefault = 30;
constexpr int kDailyDaysMax = 365;
} // namespace panel_limits

class ProxyApp {
  public:
    explicit ProxyApp(AppConfig config);
    int run();

    struct PortalWsTarget {
        std::string host;       // ws://host:port or wss://host:port
        std::string path;       // upstream path, including query
        std::string origin;     // upstream Origin header
        bool verify_tls = true;
        std::vector<std::pair<std::string, std::string>> headers;
    };
    std::optional<PortalWsTarget> portalWsTarget(const drogon::HttpRequestPtr& req,
                                                 const std::string& app_id,
                                                 const std::string& rest);

  private:
    void setupRoutes();
    void handleHome(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handlePortalApp(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                         const std::string& app_id, const std::string& rest);
    void handleLoginPage(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleLoginPost(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleLogout(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleOverview(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleRouters(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleExplain(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleRequests(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleRequestsDelete(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleStatsPage(const drogon::HttpRequestPtr& req,
                         std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleProvidersPage(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleRoutingPage(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleServiceRestart(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleRoutingOptions(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleRoutingPreview(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleCacheReset(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleDailyStats(const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleSessions(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleMetrics(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleProviderLogs(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleProviderLogImport(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleProviderLogReload(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleBlacklistGet(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleBlacklistSave(const drogon::HttpRequestPtr& req,
                             std::function<void(const drogon::HttpResponsePtr&)>&& cb);
    void handleBody(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    struct UserRecord {
        std::string password_hash;
        std::string role;
    };

    std::optional<Session> sessionFor(const drogon::HttpRequestPtr& req);
    std::optional<Session> requireSession(const drogon::HttpRequestPtr& req,
                                          const std::function<void(const drogon::HttpResponsePtr&)>& cb,
                                          bool page);
    // Администратор с проверенным CSRF; отказ уже отправлен в cb.
    std::optional<Session> requireAdmin(
        const drogon::HttpRequestPtr& req,
        const std::function<void(const drogon::HttpResponsePtr&)>& cb,
        const std::string& action, bool require_csrf = true);
    drogon::HttpResponsePtr loginPage(const std::string& error = {}) const;
    drogon::HttpResponsePtr dashboardPage(const Session& session) const;
    drogon::HttpResponsePtr statsPage(const Session& session,
                                      const drogon::HttpRequestPtr& req) const;
    drogon::HttpResponsePtr providersPage(const Session& session,
                                          const drogon::HttpRequestPtr& req) const;
    drogon::HttpResponsePtr routingPage(const Session& session) const;
    drogon::HttpResponsePtr pageResponse(std::string body) const;
    void appendPageStart(std::ostringstream& html, const std::string& title,
                         const Session& session) const;
    void appendPageEnd(std::ostringstream& html) const;
    std::string headerHtml(const Session& session) const;
    void appendSessionsTable(std::ostringstream& html, const Json::Value& sessions,
                             const std::string& link_extra) const;
    void appendLogTable(std::ostringstream& html, const Json::Value& requests,
                        const std::string& base, bool can_view_bodies) const;
    void appendRecentProviders(std::ostringstream& html) const;
    // Текущий и предыдущий маршрут модели и время переключения: единственная
    // сводка, которую показывает главная страница.
    void appendRouteSummary(std::ostringstream& html) const;
    void appendProviderHealth(std::ostringstream& html) const;
    void appendAgentEndpoints(std::ostringstream& html) const;
    void appendRoutingOptions(std::ostringstream& html, const Session& session) const;
    Json::Value routingOptionsJson() const;
    void logAgentEndpoints() const;
    void appendPriceReport(std::ostringstream& html,
                           const std::string& model_filter,
                           const std::string& provider_filter,
                           bool hide_blocked) const;
    void appendProviderAnalytics(std::ostringstream& html, const Session& session) const;
    void appendCsvProviderTable(std::ostringstream& html) const;
    void appendCsvModelTable(std::ostringstream& html) const;
    void appendCsvTimeTable(std::ostringstream& html) const;
    void appendBlacklistManager(std::ostringstream& html, bool can_edit) const;
    std::string portalHttpsUrl(const drogon::HttpRequestPtr& req) const;
    const PortalAppConfig* findPortalApp(const std::string& id) const;
    Json::Value overviewJson() const;
    // Теги провайдеров, известные конфигу, каталогу цен и статистике.
    std::set<std::string> providerTags() const;
    std::string panelBase() const;
    // Единственная проверка сети панели: панель, её API и локальные приложения
    // портала — один периметр.
    bool panelNetworkAllowed(const drogon::HttpRequestPtr& req) const;
    drogon::HttpResponsePtr redirect(const std::string& location) const;
    // Добавляет провайдера в чёрный список по серии запросов без кеша: пишет
    // конфигурацию, обновляет очередь и логирует причину.
    void blacklistProviderFromStats(const std::string& provider, const std::string& model,
                                    const std::string& reason);

    AppConfig config_;
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
    TokenStore tokens_;
    SessionStore sessions_;
    LoginLimiter login_limiter_;
    PriceCatalog catalog_;
    ProviderAnalytics analytics_;
    Router router_;
    std::unique_ptr<StatsDb> stats_db_;
    Stats stats_;
    ProviderQueue queue_;
    BodyStore bodies_;
    PriceFetcher fetcher_;
    QuotaManager quotas_;
    ProxyService proxy_;
    PortalProxy portal_proxy_;
    std::string dummy_hash_;
    std::map<std::string, UserRecord> users_;
    // Сериализует записи чёрного списка: панель и авто-блокировка.
    std::mutex blacklist_mu_;
};

} // namespace proxy
