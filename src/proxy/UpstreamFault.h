#pragma once

namespace proxy {

// Единственная классификация ответа апстрима: считается ли он отказом
// провайдера, повторяется ли попытка у того же провайдера и какой код уходит в
// журнал. HTTP-ответ клиенту строится из той же классификации.
enum class UpstreamFault { None, RateLimited, Timeout, Auth, Unavailable, Server };

struct UpstreamVerdict {
    bool problem = false;   // ответ считается отказом провайдера
    bool retryable = false; // попытку имеет смысл повторить у того же провайдера
    UpstreamFault fault = UpstreamFault::None;
};

inline UpstreamVerdict classifyUpstream(long status) {
    if (status == 429) return {true, true, UpstreamFault::RateLimited};
    if (status == 0 || status == 408) return {true, true, UpstreamFault::Timeout};
    // Отказ авторизации — проблема провайдера, но повтор её не исправит.
    if (status == 401 || status == 403) return {true, false, UpstreamFault::Auth};
    if (status == 404) return {true, true, UpstreamFault::Unavailable};
    if (status >= 500) return {true, true, UpstreamFault::Server};
    return {};
}

inline const char* upstreamFaultCode(UpstreamFault fault) {
    switch (fault) {
        case UpstreamFault::RateLimited: return "rate_limited";
        case UpstreamFault::Timeout: return "upstream_timeout";
        case UpstreamFault::Auth: return "upstream_auth_failed";
        case UpstreamFault::Unavailable: return "provider_unavailable";
        case UpstreamFault::Server: return "upstream_error";
        case UpstreamFault::None: break;
    }
    return "upstream_error";
}

} // namespace proxy
