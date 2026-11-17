#include "portal/PortalProxy.h"

#include "upstream/CurlClient.h"
#include "util/Http.h"
#include "util/StreamBridge.h"
#include "util/Strings.h"

#include <drogon/drogon.h>

#include <atomic>
#include <ctime>
#include <string>
#include <thread>

namespace proxy {
namespace {

// Соединение до локального приложения не должно висеть дольше этого предела.
constexpr int kPortalConnectCapMs = 5000;
// Ответ локального приложения может быть крупным: мост держит больше байт, чем
// по умолчанию.
constexpr std::size_t kPortalBridgeLimit = 1024 * 1024;

std::string headerValue(const std::string& line) {
    const auto colon = line.find(':');
    return colon == std::string::npos ? std::string() : trim(line.substr(colon + 1));
}

std::string headerName(const std::string& line) {
    const auto colon = line.find(':');
    return colon == std::string::npos ? std::string() : line.substr(0, colon);
}

std::string joinUrl(std::string base, std::string suffix, const std::string& query) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (suffix.empty()) suffix = "/";
    if (suffix.front() != '/') suffix.insert(suffix.begin(), '/');
    std::string url = base + suffix;
    if (!query.empty()) url += "?" + query;
    return url;
}

std::string cookieName(const std::string& set_cookie) {
    const auto eq = set_cookie.find('=');
    return trim(eq == std::string::npos ? std::string() : set_cookie.substr(0, eq));
}

bool underPrefix(const std::string& value, const std::string& prefix) {
    return value == prefix || value.rfind(prefix + "/", 0) == 0;
}

std::string rewriteCookiePath(const std::string& path, const std::string& prefix) {
    if (underPrefix(path, prefix)) return path;
    if (path.empty()) return prefix;
    if (path.front() != '/') return prefix + "/" + path;
    if (path == "/") return prefix;
    return prefix + path;
}

bool parseSetCookie(const std::string& value, const std::string& prefix, bool secure,
                    drogon::Cookie& cookie) {
    auto parts = split(value, ';');
    for (auto& p : parts) p = trim(p);
    if (parts.empty()) return false;
    const auto eq = parts[0].find('=');
    if (eq == std::string::npos || eq == 0) return false;
    cookie.setKey(trim(parts[0].substr(0, eq)));
    cookie.setValue(trim(parts[0].substr(eq + 1)));

    bool has_path = false;
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (p.empty()) continue;
        const auto attr_eq = p.find('=');
        const std::string attr = toLower(trim(p.substr(0, attr_eq)));
        const std::string attr_value = attr_eq == std::string::npos
                                           ? std::string()
                                           : trim(p.substr(attr_eq + 1));
        if (attr == "path") {
            cookie.setPath(rewriteCookiePath(attr_value, prefix));
            has_path = true;
        } else if (attr == "max-age") {
            try {
                cookie.setMaxAge(std::stoi(attr_value));
            } catch (const std::exception&) {
                LOG_WARN << "portal cookie max-age ignored: '" << attr_value << "'";
            }
        } else if (attr == "expires") {
            std::tm tm{};
            if (strptime(attr_value.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) != nullptr) {
                const std::time_t t = timegm(&tm);
                if (t >= 0) {
                    cookie.setExpiresDate(trantor::Date(static_cast<std::int64_t>(t) * 1000000));
                }
            } else {
                LOG_WARN << "portal cookie expires ignored: '" << attr_value << "'";
            }
        } else if (attr == "secure") {
            cookie.setSecure(true);
        } else if (attr == "httponly") {
            cookie.setHttpOnly(true);
        } else if (attr == "samesite") {
            const std::string v = toLower(attr_value);
            if (v == "lax") cookie.setSameSite(drogon::Cookie::SameSite::kLax);
            else if (v == "strict") cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
            else if (v == "none") cookie.setSameSite(drogon::Cookie::SameSite::kNone);
        } else if (attr == "partitioned") {
            cookie.setPartitioned(true);
        }
        // Domain отбрасывается намеренно: источник cookie — сам портал.
    }
    if (!has_path) cookie.setPath(prefix);
    if (secure) cookie.setSecure(true);
    return true;
}

std::string rewriteLocationValue(const std::string& value, const std::string& app_origin,
                                 const std::string& prefix) {
    std::string location = trim(value);
    if (location.rfind(app_origin, 0) == 0) {
        location = location.substr(app_origin.size());
        if (location.empty()) location = "/";
    }
    if (!location.empty() && location.front() == '/' && location.rfind("//", 0) != 0 &&
        !underPrefix(location, prefix)) {
        location = prefix + location;
    }
    return location;
}

void queueResponse(std::function<void(const drogon::HttpResponsePtr&)> cb,
                   drogon::HttpResponsePtr resp) {
    auto loop = drogon::app().getLoop();
    if (loop) loop->queueInLoop([cb = std::move(cb), resp]() mutable { cb(resp); });
}

} // namespace

PortalProxy::PortalProxy(std::string panel_cookie_name)
    : panel_cookie_name_(std::move(panel_cookie_name)) {}

std::string portalRequestCookies(const drogon::HttpRequestPtr& req,
                                 const std::string& panel_cookie) {
    std::string out;
    for (const auto& [name, value] : req->cookies()) {
        if (name == panel_cookie) continue;
        if (!out.empty()) out += "; ";
        out += name + "=" + value;
    }
    return out;
}

void PortalProxy::forward(const PortalAppConfig& app,
                          const std::string& prefix,
                          const drogon::HttpRequestPtr& req,
                          std::function<void(const drogon::HttpResponsePtr&)>&& cb) const {
    const std::string path = req->path();
    std::string suffix = path.size() >= prefix.size() ? path.substr(prefix.size()) : "/";
    if (suffix.empty()) suffix = "/";

    CurlHttpRequest out;
    out.method = req->methodString();
    out.url = joinUrl(app.url, suffix, req->query());
    out.body = std::string(req->body());
    out.http2 = false;
    out.verify_tls = app.verify_tls;
    out.accept_gzip = false; // preserve the upstream encoding and the client's Accept-Encoding
    out.timeouts.connect_ms = std::min(kPortalConnectCapMs, app.timeout_ms);
    out.timeouts.total_ms = app.timeout_ms;

    const UrlBase app_url = splitUrlBase(app.url);
    const std::string& app_origin = app_url.origin;
    const std::string& app_base = app_url.base_path;
    out.headers.push_back("Host: " + urlHostPort(app.url));

    for (const auto& [name, value] : req->headers()) {
        if (isHopByHopHeader(name)) continue;
        if (name == "authorization" || name == "proxy-authorization") continue;
        if (name == "origin") {
            out.headers.push_back("Origin: " + app_origin);
            continue;
        }
        if (name == "referer") {
            const auto scheme = value.find("://");
            const auto slash = scheme == std::string::npos
                                   ? std::string::npos
                                   : value.find('/', scheme + 3);
            std::string ref = slash == std::string::npos ? "/" : value.substr(slash);
            if (ref.rfind(prefix, 0) == 0) ref = ref.substr(prefix.size());
            if (ref.empty()) ref = "/";
            out.headers.push_back("Referer: " + app_origin + app_base + ref);
            continue;
        }
        out.headers.push_back(name + ": " + value);
    }
    const std::string cookies = portalRequestCookies(req, panel_cookie_name_);
    if (!cookies.empty()) out.headers.push_back("Cookie: " + cookies);
    out.headers.push_back("X-Forwarded-For: " + clientIp(req));
    out.headers.push_back("X-Forwarded-Proto: " +
                          std::string(req->isOnSecureConnection() ? "https" : "http"));
    out.headers.push_back("X-Forwarded-Host: " + req->getHeader("host"));
    out.headers.push_back("X-Forwarded-Prefix: " + prefix);
    for (const auto& [name, value] : app.headers) {
        out.headers.push_back(name + ": " + value);
    }

    auto bridge = std::make_shared<StreamBridge>(kPortalBridgeLimit);
    auto started = std::make_shared<std::atomic<bool>>(false);
    auto bytes = std::make_shared<std::atomic<std::size_t>>(0);
    const bool secure_client = req->isOnSecureConnection();
    const std::size_t max_bytes = app.max_response_bytes;

    std::thread([out = std::move(out), prefix, app_origin, bridge, started, bytes,
                 secure_client, max_bytes, panel_cookie = panel_cookie_name_,
                 callback = std::move(cb)]() mutable {
        CurlClient client;
        CurlHttpResponse response;
        const bool curl_ok = client.perform(
            out, response,
            [&](const char* data, std::size_t len) {
                if (bytes->fetch_add(len) + len > max_bytes) return false;
                return bridge->onData(data, len);
            },
            [&](const CurlHttpResponse& h) {
                if (h.status < 200 || started->exchange(true)) return;
                auto loop = drogon::app().getLoop();
                if (!loop) return;
                loop->queueInLoop([h, prefix, app_origin, bridge, secure_client, panel_cookie,
                                   callback = callback]() mutable {
                    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
                        [bridge](drogon::ResponseStreamPtr stream) {
                            bridge->attach(std::move(stream));
                        },
                        true);
                    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(h.status));
                    for (const auto& line : h.header_lines) {
                        const std::string name = headerName(line);
                        const std::string lower_name = toLower(name);
                        if (name.empty() || isHopByHopHeader(lower_name)) continue;
                        if (lower_name == "set-cookie") {
                            // Cookie панели не проходит через портал: локальный
                            // сервис не переписывает сессию панели.
                            if (cookieName(headerValue(line)) == panel_cookie) continue;
                            drogon::Cookie cookie;
                            if (parseSetCookie(headerValue(line), prefix, secure_client,
                                               cookie)) {
                                resp->addCookie(std::move(cookie));
                            } else {
                                LOG_WARN << "portal set-cookie ignored: " << headerValue(line);
                            }
                        } else if (lower_name == "location" ||
                                   lower_name == "content-location") {
                            resp->addHeader(name, rewriteLocationValue(headerValue(line),
                                                                       app_origin, prefix));
                        } else if (lower_name == "content-type") {
                            resp->setContentTypeString(headerValue(line));
                        } else {
                            resp->addHeader(name, headerValue(line));
                        }
                    }
                    callback(resp);
                });
            });
        bridge->close();
        if (!started->load()) {
            const bool too_large = bytes->load() > max_bytes;
            auto resp = too_large
                            ? errorResponse(drogon::k502BadGateway, "portal_too_large",
                                            "local application response is too large")
                            : errorResponse(drogon::k502BadGateway, "portal_upstream_error",
                                            !curl_ok && !response.error.empty()
                                                ? response.error
                                                : "local application did not respond");
            queueResponse(std::move(callback), resp);
        }
    }).detach();
}

} // namespace proxy
