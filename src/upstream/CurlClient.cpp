#include "upstream/CurlClient.h"
#include "util/Strings.h"

#include <curl/curl.h>

#include <mutex>

namespace proxy {
namespace {

// Предел тела ответа апстрима в памяти: защита от неограниченного роста.
constexpr std::size_t kMaxResponseBodyBytes = 64 * 1024 * 1024;

// Пока скорость ниже предела дольше этого времени, запрос обрывается (секунды).
constexpr long kLowSpeedTimeSec = 120;
// Ниже этой скорости считается, что передача встала (байт/с).
constexpr long kLowSpeedLimitBytesPerSec = 1;

std::once_flag curl_once;

struct CurlCtx {
    CurlHttpResponse* resp;
    const CurlDataCallback* on_data;
    const CurlHeaderCallback* on_headers;
    std::atomic<bool>* cancel;
    bool headers_done = false;
    bool informational = false;
};

void notifyHeaders(CurlCtx* ctx) {
    if (ctx->headers_done) return;
    ctx->headers_done = true;
    if (ctx->on_headers && *ctx->on_headers) (*ctx->on_headers)(*ctx->resp);
}

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<CurlCtx*>(userdata);
    if (ctx->cancel && ctx->cancel->load()) return 0;
    const size_t n = size * nmemb;
    notifyHeaders(ctx);
    if (ctx->on_data && *ctx->on_data) {
        if (!(*ctx->on_data)(ptr, n)) return 0;
    } else {
        if (ctx->resp->body.size() + n > kMaxResponseBodyBytes) return 0;
        ctx->resp->body.append(ptr, n);
    }
    return n;
}

size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<CurlCtx*>(userdata);
    const size_t n = size * nitems;
    std::string line(buffer, n);
    if (line.rfind("HTTP/", 0) == 0) {
        auto sp = line.find(' ');
        if (sp != std::string::npos) {
            try { ctx->resp->status = std::stol(line.substr(sp + 1)); } catch (...) {}
        }
        ctx->informational = ctx->resp->status >= 100 && ctx->resp->status < 200 &&
                              ctx->resp->status != 101;
        return n;
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string raw = line;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) raw.pop_back();
        if (!raw.empty()) ctx->resp->header_lines.push_back(raw);
        std::string name = toLower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
        if (!name.empty()) ctx->resp->headers[name] = value;
    } else if (line == "\r\n" || line == "\n") {
        if (ctx->informational) {
            ctx->informational = false;
            return n;
        }
        notifyHeaders(ctx);
    }
    return n;
}

int xferCb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<CurlCtx*>(userdata);
    return (ctx->cancel && ctx->cancel->load()) ? 1 : 0;
}

} // namespace

bool CurlClient::perform(const CurlHttpRequest& req,
                         CurlHttpResponse& resp,
                         const CurlDataCallback& on_data,
                         const CurlHeaderCallback& on_headers,
                         std::atomic<bool>* cancel) const {
    std::call_once(curl_once, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl_easy_init failed";
        return false;
    }
    struct Cleanup {
        CURL* c;
        ~Cleanup() { curl_easy_cleanup(c); }
    } cleanup{curl};

    CurlCtx ctx{&resp, &on_data, &on_headers, cancel, false};
    curl_slist* slist = nullptr;
    struct SlistCleanup { curl_slist* s; ~SlistCleanup() { if (s) curl_slist_free_all(s); } } sc{slist};

    for (const auto& h : req.headers) slist = curl_slist_append(slist, h.c_str());
    if (!req.body.empty() || (req.method != "GET" && req.method != "HEAD")) {
        slist = curl_slist_append(slist, "Expect:");
    }
    sc.s = slist;

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "costrelay/0.1");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.timeouts.connect_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeouts.total_ms));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedLimitBytesPerSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeSec);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, req.verify_tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, req.verify_tls ? 2L : 0L);
    if (!req.ca_file.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, req.ca_file.c_str());
    if (!req.client_cert_file.empty()) curl_easy_setopt(curl, CURLOPT_SSLCERT, req.client_cert_file.c_str());
    if (!req.client_key_file.empty()) curl_easy_setopt(curl, CURLOPT_SSLKEY, req.client_key_file.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     req.http2 ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);
#else
    (void)req.http2;
#endif
    if (req.accept_gzip) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (slist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

    const bool has_body = !req.body.empty() || req.method == "POST" ||
                          req.method == "PUT" || req.method == "PATCH";
    if (req.method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
    } else if (req.method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (has_body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(req.body.size()));
        }
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.error = curl_easy_strerror(rc);
        return false;
    }
    if (resp.status >= 400) {
        if (resp.error.empty()) resp.error = "HTTP " + std::to_string(resp.status);
        return false;
    }
    return true;
}

} // namespace proxy
