#pragma once

#include "config/Config.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <string>

namespace proxy {

// Заголовок Cookie для локального приложения: cookie панели не передаётся —
// правило одно для HTTP-портала и WebSocket-моста.
std::string portalRequestCookies(const drogon::HttpRequestPtr& req,
                                 const std::string& panel_cookie);

// Authenticated reverse proxy for configured local HTTP(S) applications.
class PortalProxy {
  public:
    explicit PortalProxy(std::string panel_cookie_name);

    void forward(const PortalAppConfig& app,
                 const std::string& prefix,
                 const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) const;

  private:
    std::string panel_cookie_name_;
};

} // namespace proxy
