#pragma once

#include <json/json.h>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <string>
#include <string_view>

namespace proxy {

drogon::HttpResponsePtr jsonResponse(const Json::Value& value, drogon::HttpStatusCode status = drogon::k200OK);
drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string& code,
                                      const std::string& message, const std::string& request_id = {});
drogon::HttpResponsePtr textResponse(const std::string& body, drogon::HttpStatusCode status = drogon::k200OK,
                                     const std::string& content_type = "text/plain; charset=utf-8");
std::string requestId(const drogon::HttpRequestPtr& req);
bool parseJson(std::string_view body, Json::Value& out, std::string& error);
std::string serializeJson(const Json::Value& value);
std::string clientIp(const drogon::HttpRequestPtr& req);
std::string urlOrigin(std::string_view url);
std::string urlHostPort(std::string_view url);
// Базовый адрес локального приложения: схема+хост и путь без хвостовых «/».
// Портал и WebSocket-мост берут origin и base только отсюда.
struct UrlBase {
    std::string origin;
    std::string base_path;
};
UrlBase splitUrlBase(std::string_view url);
bool isHopByHopHeader(std::string_view name);

} // namespace proxy
