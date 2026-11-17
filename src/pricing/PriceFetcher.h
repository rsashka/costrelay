#pragma once

#include "config/Config.h"
#include "pricing/PriceCatalog.h"
#include "upstream/CurlClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace proxy {

class PriceFetcher {
  public:
    PriceFetcher(const AppConfig& config, PriceCatalog& catalog);
    ~PriceFetcher();

    void start();
    void stop();
    bool fetchModel(const std::string& model);
    bool refreshNow();
    std::string lastError() const;
    // Вызывается после каждого цикла обновления цен (в т.ч. без изменений).
    void setOnRefresh(std::function<void()> callback) { on_refresh_ = std::move(callback); }

  private:
    void loop();
    std::string endpointsUrl(const std::string& model) const;
    // Общая точка отказа: текст ошибки виден в панели и /readyz.
    bool fail(const std::string& model, const std::string& message);

    const AppConfig& config_;
    PriceCatalog& catalog_;
    // Адрес прайса: базовый URL RouterAI-апстрима без завершающих «/».
    std::string price_base_;
    CurlClient curl_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string last_error_;
    std::mutex refresh_mu_;
    std::function<void()> on_refresh_;
};

} // namespace proxy
