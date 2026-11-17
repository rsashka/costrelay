#pragma once

#include "auth/Auth.h"
#include "capture/BodyStore.h"
#include "config/Config.h"
#include "limits/ConcurrencyLimiter.h"
#include "limits/QuotaManager.h"
#include "pricing/PriceCatalog.h"
#include "routing/ProviderHealth.h"
#include "routing/ProviderQueue.h"
#include "stats/Stats.h"
#include "upstream/CurlClient.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace proxy {

class StreamBridge;

class ProxyService {
  public:
    ProxyService(const AppConfig& config, PriceCatalog& catalog, ProviderQueue& queue,
                 Stats& stats, BodyStore& bodies, QuotaManager* quotas = nullptr);
    ~ProxyService();

    void handleChat(const drogon::HttpRequestPtr& req, Principal principal,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);
    // Фоновая проверка доступности провайдеров, ушедших из маршрута.
    void start();
    void stop();
    // Состояние доступности — для правила маршрута, панели и API.
    const ProviderHealth& health() const { return health_; }
    std::set<std::string> downProviders() const;
    Json::Value healthJson() const;
    Json::Value modelsJson() const;
    // Ответ — текущий снимок очереди: никакого пересчёта под конкретный запрос.
    Json::Value explain(const std::string& model) const;
    // Вызывается, когда серия запросов без кеша достигла порога блокировки.
    void setOnCacheMissBlacklist(
        std::function<void(const std::string& provider, const std::string& model,
                           const std::string& reason)> callback) {
        on_cache_miss_blacklist_ = std::move(callback);
    }

  private:
    void executeNonStream(std::string request_id, Json::Value body, RoutePlan plan,
                          Principal principal, std::string client_ip,
                          std::function<void(const drogon::HttpResponsePtr&)> callback,
                          std::chrono::steady_clock::time_point started, bool capture_requested,
                          QuotaTicket ticket, std::string session_id,
                          std::int64_t request_bytes);
    void executeStream(std::string request_id, Json::Value body, RoutePlan plan,
                       Principal principal, std::string client_ip,
                       std::function<void(const drogon::HttpResponsePtr&)> callback,
                       std::chrono::steady_clock::time_point started, bool capture_requested,
                       QuotaTicket ticket, std::string session_id,
                       std::int64_t request_bytes);

    std::string upstreamUrl(const RouterConfig& router) const;
    CurlHttpRequest makeRequest(const RouterConfig& router, const std::string& upstream_body,
                                bool stream) const;
    // Проверка доступности провайдера минимальным запросом через тот же router.
    void probeLoop();
    bool probeProvider(const ProviderHealthSnapshot& status);
    // Повтор запроса у того же провайдера: политика из routing.retry.
    int providerRetryAttempts() const;
    int providerRetryBackoffMs() const;
    // Бюджет fallback в провайдерах: 1 + routing.fallback_attempts.
    int providerBudget() const;
    // Сколько выгодно ждать текущего провайдера до fallback: цена уже потерянного
    // времени сравнивается с разницей стоимости следующего оффера. Ограничено
    // `routing.failover_wait_max_sec`; 0 — ждать невыгодно, переходить сразу.
    std::int64_t failoverWaitSec(const RoutePlan& plan, std::size_t candidate_index) const;
    // Привязка сессии следует за фактом: запрос ушёл на другого провайдера или не
    // удался. Возвращает причину смены для журнала (пусто — смены не было).
    std::string applyRouteSwitch(const RoutePlan& plan, const RouteCandidate& chosen, bool ok,
                                 const std::string& session_id);
    void recordAtCompletion(const RoutePlan& plan, const RouteCandidate& candidate,
                            const std::string& request_id, const Principal& principal,
                            const std::string& client_ip, int status, bool stream,
                            int latency_ms, int ttfb_ms, std::int64_t bytes_in,
                            std::int64_t bytes_out, std::int64_t tokens_in,
                            std::int64_t tokens_out, std::int64_t cache_read_tokens,
                            std::int64_t cache_write_tokens, int attempts,
                            const std::string& switch_cause, const std::string& error,
                            const std::string& retry_detail, bool captured, QuotaTicket& ticket,
                            const std::string& session_id);

    const AppConfig& config_;
    PriceCatalog& catalog_;
    ProviderQueue& queue_;
    Stats& stats_;
    BodyStore& bodies_;
    QuotaManager* quotas_ = nullptr;
    ConcurrencyLimiter upstream_limiter_;
    mutable CurlClient curl_;
    ProviderHealth health_;
    std::atomic<bool> probing_{false};
    std::thread prober_;
    std::mutex probe_mu_;
    std::condition_variable probe_cv_;
    std::function<void(const std::string& provider, const std::string& model,
                       const std::string& reason)> on_cache_miss_blacklist_;
};

} // namespace proxy
