#include "util/Http.h"
#include "util/Strings.h"

#include <sstream>

namespace proxy {

drogon::HttpResponsePtr jsonResponse(const Json::Value& value, drogon::HttpStatusCode status) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(value);
    resp->setStatusCode(status);
    resp->addHeader("Cache-Control", "no-store");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    return resp;
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status, const std::string& code,
                                      const std::string& message, const std::string& request_id) {
    Json::Value err(Json::objectValue);
    Json::Value inner(Json::objectValue);
    inner["message"] = message;
    inner["type"] = "proxy_error";
    inner["code"] = code;
    if (!request_id.empty()) inner["request_id"] = request_id;
    err["error"] = inner;
    auto resp = jsonResponse(err, status);
    if (status == drogon::k401Unauthorized) {
        resp->addHeader("WWW-Authenticate", "Bearer realm=\"proxy\"");
    }
    return resp;
}

drogon::HttpResponsePtr textResponse(const std::string& body, drogon::HttpStatusCode status,
                                     const std::string& content_type) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setContentTypeString(content_type);
    resp->setBody(body);
    resp->addHeader("X-Content-Type-Options", "nosniff");
    return resp;
}

std::string requestId(const drogon::HttpRequestPtr& req) {
    const std::string id = req->getHeader("x-request-id");
    // Принимается только идентификатор, пригодный и для журнала, и для имени
    // файла захваченного тела: правило одно (util/Strings.h).
    return isRequestId(id) ? id : uuidV7();
}

bool parseJson(std::string_view body, Json::Value& out, std::string& error) {
    Json::CharReaderBuilder b;
    b["collectComments"] = false;
    b["stackLimit"] = 128;
    std::string errs;
    std::istringstream in{std::string(body)};
    if (!Json::parseFromStream(b, in, &out, &errs)) {
        error = "invalid JSON: " + errs;
        return false;
    }
    return true;
}

std::string serializeJson(const Json::Value& value) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["emitUTF8"] = true;
    return Json::writeString(b, value);
}

std::string clientIp(const drogon::HttpRequestPtr& req) {
    return req->peerAddr().toIp();
}

std::string urlHostPort(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    const auto slash = url.find('/', scheme + 3);
    return std::string(slash == std::string_view::npos
                           ? url.substr(scheme + 3)
                           : url.substr(scheme + 3, slash - scheme - 3));
}

std::string urlOrigin(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    return std::string(url.substr(0, scheme + 3)) + urlHostPort(url);
}

UrlBase splitUrlBase(std::string_view url) {
    UrlBase base;
    base.origin = urlOrigin(url);
    if (base.origin.empty()) return base;
    base.base_path = std::string(url.substr(base.origin.size()));
    while (!base.base_path.empty() && base.base_path.back() == '/') base.base_path.pop_back();
    return base;
}

bool isHopByHopHeader(std::string_view name) {
    const std::string n = toLower(name);
    static const char* names[] = {"connection", "keep-alive", "proxy-connection",
                                  "proxy-authenticate", "proxy-authorization", "te",
                                  "trailer", "transfer-encoding", "upgrade",
                                  "content-length", "expect", "host"};
    for (const auto* x : names) {
        if (n == x) return true;
    }
    return false;
}

} // namespace proxy
