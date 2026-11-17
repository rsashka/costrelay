#include "proxy/ProxyService.h"
#include "proxy/UpstreamFault.h"
#include "util/Http.h"
#include "util/StreamBridge.h"
#include "util/Strings.h"
#include "util/Time.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <thread>

namespace proxy {

namespace {

// Идентификатор сессии из заголовка: длиннее — обрезаем, чтобы ключ таблиц не рос.
constexpr std::size_t kMaxSessionIdLen = 128;
// Накопитель разбора потока: больше не нужно — usage приходит в последних чанках.
constexpr std::size_t kStreamTapLimit = 1024 * 1024;
// Проверка доступности: минимальный запрос через того же провайдера.
constexpr int kProbeMaxTokens = 1;
constexpr int kProbeTickSec = 1;
constexpr std::size_t kSsePendingLimit = 256 * 1024;

struct UsageStats {
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_write_tokens = 0;
};

std::int64_t jsonInt64(const Json::Value& v, const char* key) {
    if (!v.isObject() || !v.isMember(key)) return 0;
    const auto& x = v[key];
    if (x.isInt64()) return x.asInt64();
    if (x.isUInt64()) return static_cast<std::int64_t>(x.asUInt64());
    if (x.isDouble()) return static_cast<std::int64_t>(x.asDouble());
    return 0;
}

void applyTimingsCache(UsageStats& u, const Json::Value& payload) {
    if (u.cache_read_tokens > 0 || !payload.isObject()) return;
    const auto& timings = payload["timings"];
    if (!timings.isObject()) return;
    const auto cached = jsonInt64(timings, "cache_n");
    if (cached > 0) u.cache_read_tokens = cached;
}

UsageStats parseUsage(const Json::Value& usage) {
    UsageStats u;
    if (!usage.isObject()) return u;
    u.prompt_tokens = jsonInt64(usage, "prompt_tokens");
    if (u.prompt_tokens == 0) u.prompt_tokens = jsonInt64(usage, "input_tokens");
    u.completion_tokens = jsonInt64(usage, "completion_tokens");
    if (u.completion_tokens == 0) u.completion_tokens = jsonInt64(usage, "output_tokens");
    u.cache_read_tokens = jsonInt64(usage, "cache_read_input_tokens");
    if (u.cache_read_tokens == 0) u.cache_read_tokens = jsonInt64(usage, "prompt_cache_hit_tokens");
    if (u.cache_read_tokens == 0) u.cache_read_tokens = jsonInt64(usage, "cached_tokens");
    const auto& details = usage["prompt_tokens_details"];
    if (u.cache_read_tokens == 0 && details.isObject()) {
        u.cache_read_tokens = jsonInt64(details, "cached_tokens");
    }
    u.cache_write_tokens = jsonInt64(usage, "cache_creation_input_tokens");
    if (u.cache_write_tokens == 0) u.cache_write_tokens = jsonInt64(usage, "cache_write_input_tokens");
    if (u.cache_write_tokens == 0 && details.isObject()) {
        u.cache_write_tokens = jsonInt64(details, "cache_creation_tokens");
    }
    if (u.cache_read_tokens < 0) u.cache_read_tokens = 0;
    if (u.cache_write_tokens < 0) u.cache_write_tokens = 0;
    if (u.prompt_tokens > 0) u.cache_read_tokens = std::min(u.cache_read_tokens, u.prompt_tokens);
    return u;
}

std::string requestSessionId(const drogon::HttpRequestPtr& req, const Principal& principal,
                             const Json::Value& body) {
    static const char* headers[] = {"x-session-id", "x-proxy-session", "x-request-session"};
    for (const auto* name : headers) {
        std::string value = trim(req->getHeader(name));
        if (value.empty()) continue;
        value.erase(std::remove_if(value.begin(), value.end(),
                                   [](unsigned char c) { return c < 0x20 || c == 0x7f; }),
                    value.end());
        if (value.size() > kMaxSessionIdLen) value.resize(kMaxSessionIdLen);
        if (!value.empty()) return value;
    }
    if (body.isObject() && body["user"].isString()) {
        std::string user = trim(body["user"].asString());
        user.erase(std::remove_if(user.begin(), user.end(),
                                  [](unsigned char c) { return c < 0x20 || c == 0x7f; }),
                   user.end());
        if (user.size() > kMaxSessionIdLen) user.resize(kMaxSessionIdLen);
        if (!user.empty()) return "user:" + user;
    }
    return principal.token_id.empty() ? "anonymous" : "token:" + principal.token_id;
}

// Единственная классификация ответа апстрима — в UpstreamFault.h: одна на
// ретраи, терминальный ответ, журнал и проверку доступности.
bool isRetryableUpstreamStatus(long status) { return classifyUpstream(status).retryable; }

// Неудачная попытка у провайдера: попадает в журнал всегда, включая
// последнюю (повтора после неё может и не быть).
struct RetryNote {
    std::string provider;
    long status = 0;
    std::string error;
    int attempt = 0;
};

// Журнал запроса: провайдер, ответ и номер попытки — то, что видно в панели.
std::string retriesJson(const std::vector<RetryNote>& notes) {
    if (notes.empty()) return {};
    Json::Value root(Json::objectValue);
    Json::Value retries(Json::arrayValue);
    for (const auto& note : notes) {
        Json::Value item(Json::objectValue);
        item["provider"] = note.provider;
        item["status"] = static_cast<Json::Int64>(note.status);
        item["error"] = note.error;
        item["attempt"] = note.attempt;
        retries.append(item);
    }
    root["retries"] = retries;
    return serializeJson(root);
}

bool isUpstreamProblem(long status) { return classifyUpstream(status).problem; }

// Разбор ответа модели: «есть вывод» — текст, reasoning или вызов инструмента.
// Проверка доступности принимает и живой ответ без вывода: важен факт ответа
// провайдера, а не содержание проверочного запроса. Это разные правила: живой
// путь ответ без вывода считает отказом провайдера.
struct CompletionInspection {
    bool meaningful = false;
    bool has_choices = false;
    std::string finish_reason;
    std::string error;
};

bool completionAlive(const CompletionInspection& inspection) {
    return inspection.has_choices || inspection.meaningful;
}

CompletionInspection inspectCompletion(const Json::Value& payload) {
    CompletionInspection out;
    if (!payload.isObject()) return out;
    if (payload.isMember("error") && !payload["error"].isNull()) {
        const auto& error = payload["error"];
        out.error = error.isString() ? error.asString() : std::string();
        if (error.isObject() && error["message"].isString()) out.error = error["message"].asString();
        if (out.error.empty()) out.error = "upstream error object";
        return out;
    }
    const auto& choices = payload["choices"];
    if (!choices.isArray()) return out;
    out.has_choices = !choices.empty();
    for (const auto& choice : choices) {
        if (!choice.isObject()) continue;
        const auto& fr = choice["finish_reason"];
        if (fr.isString() && !fr.asString().empty()) out.finish_reason = fr.asString();
        for (const char* key : {"delta", "message"}) {
            if (!choice.isMember(key) || !choice[key].isObject()) continue;
            const auto& part = choice[key];
            for (const char* text : {"content", "reasoning_content", "reasoning"}) {
                if (part[text].isString() && !part[text].asString().empty()) out.meaningful = true;
            }
            if (part.isMember("tool_calls") && part["tool_calls"].isArray() &&
                !part["tool_calls"].empty()) {
                out.meaningful = true;
            }
            if (part.isMember("function_call") && part["function_call"].isObject() &&
                !part["function_call"].empty()) {
                out.meaningful = true;
            }
        }
    }
    return out;
}

std::string describeEmpty(const CompletionInspection& inspection) {
    std::string text = "empty completion";
    if (!inspection.finish_reason.empty()) text += " finish_reason=" + inspection.finish_reason;
    if (!inspection.error.empty()) text += " error='" + inspection.error + "'";
    return text;
}

// Инкрементальный разбор SSE: решает, начинать ли ответ клиенту, и объясняет,
// почему ответ оказался пустым.
class SseInspector {
  public:
    void feed(const char* data, std::size_t len) {
        pending_.append(data, len);
        std::size_t start = 0;
        while (true) {
            const auto pos = pending_.find('\n', start);
            if (pos == std::string::npos) break;
            consume(pending_.substr(start, pos - start));
            start = pos + 1;
        }
        pending_.erase(0, start);
        // Не SSE и без переводов строк: не копим бесконечно, разбираем как есть.
        if (pending_.size() > kSsePendingLimit) {
            consume(pending_);
            pending_.clear();
        }
    }

    void finish() {
        if (pending_.empty()) return;
        Json::Value whole;
        std::string error;
        const std::string text = trim(pending_);
        const bool looks_json = !text.empty() && (text.front() == '{' || text.front() == '[');
        if (looks_json && parseJson(text, whole, error)) {
            merge(inspectCompletion(whole));
        } else {
            consume(pending_);
        }
        pending_.clear();
    }

    bool meaningful() const { return meaningful_; }
    const std::string& error() const { return error_; }
    CompletionInspection inspection() const {
        CompletionInspection out;
        out.meaningful = meaningful_;
        out.finish_reason = finish_reason_;
        out.error = error_;
        return out;
    }

  private:
    void consume(const std::string& line) {
        std::string text = trim(line);
        if (!startsWith(text, "data:")) return;
        text = trim(std::string_view(text).substr(5));
        if (text.empty() || text == "[DONE]") return;
        Json::Value chunk;
        std::string error;
        if (!parseJson(text, chunk, error)) return;
        merge(inspectCompletion(chunk));
    }

    void merge(const CompletionInspection& inspection) {
        if (inspection.meaningful) meaningful_ = true;
        if (!inspection.finish_reason.empty()) finish_reason_ = inspection.finish_reason;
        if (!inspection.error.empty() && error_.empty()) error_ = inspection.error;
    }

    std::string pending_;
    bool meaningful_ = false;
    std::string finish_reason_;
    std::string error_;
};

drogon::HttpResponsePtr upstreamErrorResponse(const CurlHttpResponse& last,
                                              const std::string& request_id) {
    switch (classifyUpstream(last.status).fault) {
        case UpstreamFault::RateLimited: {
            auto resp = errorResponse(drogon::k429TooManyRequests, "rate_limited",
                                      "upstream rate limited", request_id);
            if (last.headers.count("retry-after")) {
                resp->addHeader("Retry-After", last.headers.at("retry-after"));
            }
            return resp;
        }
        case UpstreamFault::Timeout:
            return errorResponse(drogon::k504GatewayTimeout, "upstream_timeout",
                                 last.error.empty()
                                     ? "upstream request timed out"
                                     : "upstream request failed: " + last.error,
                                 request_id);
        case UpstreamFault::Auth:
            return errorResponse(drogon::k502BadGateway, "upstream_auth_failed",
                                 "upstream authentication failed", request_id);
        case UpstreamFault::Unavailable:
            return errorResponse(drogon::k502BadGateway, "provider_unavailable",
                                 "requested provider is unavailable", request_id);
        case UpstreamFault::Server:
        case UpstreamFault::None:
            break;
    }
    return errorResponse(drogon::k502BadGateway, "upstream_error",
                         "upstream returned an error", request_id);
}

// Единственный владелец терминального ответа запроса: и потоковая, и обычная
// ветки отдают клиенту и в журнал одно и то же для одного и того же отказа.
struct TerminalResponse {
    drogon::HttpResponsePtr resp;
    int status = 0;
    std::string journal_error;
};

TerminalResponse terminalResponse(const CurlHttpResponse& last, bool local_busy, bool empty,
                                  const std::string& empty_detail,
                                  const std::string& request_id, bool no_attempt) {
    TerminalResponse out;
    if (no_attempt) {
        // Ни одного обращения к провайдеру не было: это дефект конфигурации, а не
        // отказ апстрима — 504/502 «upstream» здесь вводил бы в заблуждение.
        out.journal_error = kRouteNotBuildableCode;
        out.resp = errorResponse(drogon::k502BadGateway, "route_not_buildable",
                                 "no candidate could be built into an upstream request",
                                 request_id);
    } else if (local_busy) {
        out.journal_error = kUpstreamBusyCode;
        out.resp = errorResponse(drogon::k503ServiceUnavailable, "upstream_busy",
                                 "upstream concurrency limit reached", request_id);
    } else if (empty) {
        out.journal_error = kUpstreamEmptyCompletionCode;
        out.resp = errorResponse(drogon::k502BadGateway, "upstream_empty_completion",
                                 "upstream returned no output: " + empty_detail, request_id);
    } else if (isUpstreamProblem(last.status)) {
        out.resp = upstreamErrorResponse(last, request_id);
        out.journal_error = upstreamFaultCode(classifyUpstream(last.status).fault);
    } else {
        // 400/422 — ошибки клиента: тело и статус уходят как есть.
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(last.status));
        resp->setContentTypeString("application/json");
        resp->setBody(last.body);
        out.resp = std::move(resp);
    }
    out.status = static_cast<int>(out.resp->getStatusCode());
    return out;
}

// Состояние попыток одного кандидата: одно на потоковую и обычную ветки.
struct CandidateAttempt {
    bool provider_failed = false;
    long failed_status = 0;
    std::string failed_detail;
    std::int64_t first_failure_epoch = 0;
    std::chrono::steady_clock::time_point patience_deadline{};
    int retries = 0;
    int backoff_ms = 0;
};

struct AttemptResult {
    bool empty = false;
    long status = 0;
    std::string detail;
};

// Неудачная попытка: серия отказов, решение о повторе и запись в журнал — один
// владелец политики. Возвращает true, если попытку нужно повторить.
bool handleAttemptFailure(CandidateAttempt& attempt, const std::string& model,
                          const std::string& provider, const std::string& router_id,
                          int provider_attempt, const AttemptResult& outcome,
                          std::vector<RetryNote>& notes, bool& next_candidate) {
    if (outcome.empty || isUpstreamProblem(outcome.status)) {
        if (!attempt.provider_failed) {
            attempt.provider_failed = true;
            attempt.first_failure_epoch =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
        }
        attempt.failed_status = outcome.status;
        attempt.failed_detail = outcome.detail;
    }
    const bool retryable = outcome.empty || isRetryableUpstreamStatus(outcome.status);
    const bool patience_left = std::chrono::steady_clock::now() < attempt.patience_deadline;
    const bool will_retry = retryable && (provider_attempt < attempt.retries || patience_left);
    LOG_WARN << "provider error: model=" << model << " provider=" << provider
             << " router=" << router_id << " status=" << outcome.status << " error='"
             << outcome.detail << "' attempt=" << (provider_attempt + 1)
             << (will_retry ? " retry=1" : "");
    // Ошибка провайдера попадает в журнал независимо от того, будет ли повтор:
    // панель показывает каждую неудачную попытку.
    notes.push_back({provider, outcome.status, outcome.detail, provider_attempt + 1});
    if (will_retry) {
        if (attempt.backoff_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt.backoff_ms));
        }
        return true;
    }
    // Неретраиваемая ошибка клиента fallback не запускает; исчерпанное ожидание —
    // повод перейти к следующему провайдеру.
    next_candidate = retryable;
    return false;
}

} // namespace

ProxyService::ProxyService(const AppConfig& config, PriceCatalog& catalog, ProviderQueue& queue,
                           Stats& stats, BodyStore& bodies, QuotaManager* quotas)
    : config_(config), catalog_(catalog), queue_(queue), stats_(stats), bodies_(bodies),
      quotas_(quotas), health_(config) {}

ProxyService::~ProxyService() { stop(); }

void ProxyService::start() {
    if (probing_.exchange(true)) return;
    prober_ = std::thread([this] { probeLoop(); });
}

void ProxyService::stop() {
    if (!probing_.exchange(false)) {
        if (prober_.joinable()) prober_.join();
        return;
    }
    probe_cv_.notify_all();
    if (prober_.joinable()) prober_.join();
}

std::set<std::string> ProxyService::downProviders() const {
    std::set<std::string> out;
    for (const auto& status : health_.snapshot()) {
        if (status.down) out.insert(status.provider);
    }
    return out;
}

Json::Value ProxyService::healthJson() const {
    Json::Value arr(Json::arrayValue);
    for (const auto& status : health_.snapshot()) {
        Json::Value item(Json::objectValue);
        item["provider"] = status.provider;
        item["router"] = status.router_id;
        item["model"] = status.model;
        item["down"] = status.down;
        item["fail_streak"] = status.fail_streak;
        item["failing_since"] = static_cast<Json::Int64>(status.failing_since);
        item["down_since"] = static_cast<Json::Int64>(status.down_since);
        item["last_status"] = static_cast<Json::Int64>(status.last_status);
        item["last_error"] = status.last_error;
        item["next_probe_at"] = static_cast<Json::Int64>(status.next_probe_at);
        arr.append(item);
    }
    Json::Value root(Json::objectValue);
    root["providers"] = arr;
    root["down"] = static_cast<Json::UInt64>(downProviders().size());
    return root;
}

void ProxyService::probeLoop() {
    while (probing_.load()) {
        for (const auto& status : health_.dueProbes()) {
            if (!probing_.load()) return;
            probeProvider(status);
        }
        std::unique_lock<std::mutex> lock(probe_mu_);
        probe_cv_.wait_for(lock, std::chrono::seconds(kProbeTickSec),
                           [this] { return !probing_.load(); });
    }
}

bool ProxyService::probeProvider(const ProviderHealthSnapshot& status) {
    const auto quote = queue_.quoteFor(status.model, status.provider);
    if (!quote || !quote->excluded.empty()) {
        // Провайдер перестал быть оффером модели: отметку снимаем, но успешной
        // проверкой это не считается — реального запроса не было.
        health_.forget(status.provider);
        return true;
    }
    const auto* router = config_.findRouter(quote->router_id);
    if (!router) {
        health_.noteProbeResult(status.provider, false, 0, "router_not_found");
        return false;
    }
    const RoutePlan plan = queue_.planForProbe(status.model, status.provider);
    Json::Value body(Json::objectValue);
    body["model"] = status.model;
    Json::Value messages(Json::arrayValue);
    Json::Value message(Json::objectValue);
    message["role"] = "user";
    message["content"] = "ping";
    messages.append(message);
    body["messages"] = messages;
    body["max_tokens"] = kProbeMaxTokens;
    body["stream"] = false;
    std::string error;
    const Json::Value upstream = buildUpstreamBody(body, plan, *quote, config_, error);
    if (!error.empty()) {
        health_.noteProbeResult(status.provider, false, 0, error);
        return false;
    }
    CurlHttpResponse response;
    const bool transport = curl_.perform(makeRequest(*router, serializeJson(upstream), false),
                                         response, {}, nullptr);
    CompletionInspection inspection;
    Json::Value parsed;
    std::string parse_error;
    const bool http_ok = transport && response.status >= 200 && response.status < 300;
    if (http_ok) {
        if (parseJson(response.body, parsed, parse_error)) inspection = inspectCompletion(parsed);
    }
    // Проверка принимает и пустой текст с finish_reason: важен живой ответ
    // провайдера, а не содержание проверочного запроса.
    const bool good = http_ok && completionAlive(inspection);
    const std::string reason =
        !transport ? (response.error.empty() ? "transport error" : response.error)
                   : (http_ok ? describeEmpty(inspection)
                              : "status " + std::to_string(response.status));
    if (!good) {
        LOG_WARN << "provider probe failed: provider=" << status.provider
                 << " router=" << quote->router_id << " model=" << status.model
                 << " status=" << response.status << " error='" << reason << "'";
    }
    health_.noteProbeResult(status.provider, good, response.status, reason);
    return good;
}

std::string ProxyService::upstreamUrl(const RouterConfig& router) const {
    std::string base = router.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/chat/completions";
}

CurlHttpRequest ProxyService::makeRequest(const RouterConfig& router,
                                          const std::string& upstream_body,
                                          bool stream) const {
    CurlHttpRequest r;
    r.method = "POST";
    r.url = upstreamUrl(router);
    r.body = upstream_body;
    r.timeouts = router.timeouts;
    r.http2 = router.http2;
    r.verify_tls = router.verify_tls;
    r.ca_file = router.ca_file;
    r.client_cert_file = router.client_cert_file;
    r.client_key_file = router.client_key_file;
    r.accept_gzip = !stream;
    r.headers.push_back("Content-Type: application/json");
    r.headers.push_back(stream ? "Accept: text/event-stream" : "Accept: application/json");
    if (!router.api_key.empty()) {
        const std::string value = router.auth_scheme.empty()
                                      ? router.api_key
                                      : router.auth_scheme + " " + router.api_key;
        r.headers.push_back(router.auth_header + ": " + value);
    }
    return r;
}

void ProxyService::handleChat(const drogon::HttpRequestPtr& req, Principal principal,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string rid = requestId(req);
    const auto started = std::chrono::steady_clock::now();
    const std::string ip = clientIp(req);

    if (!principal.hasScope("proxy:invoke")) {
        callback(errorResponse(drogon::k403Forbidden, "model_not_allowed",
                               "token scope proxy:invoke is required", rid));
        return;
    }
    if (req->body().size() > config_.server.max_body_bytes) {
        callback(errorResponse(drogon::k413RequestEntityTooLarge, "invalid_request",
                               "request body too large", rid));
        return;
    }
    Json::Value body;
    std::string parse_error;
    if (!parseJson(req->body(), body, parse_error) || !body.isObject()) {
        callback(errorResponse(drogon::k400BadRequest, "invalid_request",
                               parse_error.empty() ? "body must be a JSON object" : parse_error, rid));
        return;
    }
    if (!body["model"].isString() || body["model"].asString().empty()) {
        callback(errorResponse(drogon::k400BadRequest, "invalid_request", "model is required", rid));
        return;
    }
    const std::string session_id = requestSessionId(req, principal, body);
    if (body.isMember("provider") && config_.routing.client_provider_policy == "reject") {
        callback(errorResponse(drogon::k400BadRequest, "client_override_not_allowed",
                               "client-provided provider routing is disabled", rid));
        return;
    }
    std::string requested = body["model"].asString();
    if (requested.find('@') != std::string::npos) {
        callback(errorResponse(drogon::k400BadRequest, "invalid_request",
                               "@-syntax in model is not supported by this proxy; use provider routing",
                               rid));
        return;
    }
    const std::string model = config_.resolveModel(requested);
    if (!config_.modelConfigured(model)) {
        callback(errorResponse(drogon::k404NotFound, "model_not_found", "unknown model: " + requested, rid));
        return;
    }
    if (!principal.canUseModel(model)) {
        callback(errorResponse(drogon::k403Forbidden, "model_not_allowed", "token is not allowed to use this model", rid));
        return;
    }
    bool stream = body.isMember("stream") && body["stream"].isBool() && body["stream"].asBool();

    // Маршрут не считается под запрос: берём верх текущей очереди модели.
    // Провайдер, закреплённый за сессией, идёт первым: кеш живёт в беседе.
    RoutePlan plan = queue_.planForSession(model, session_id, downProviders());
    if (!plan.hasRoute()) {
        std::map<std::string, int> reasons;
        for (const auto& candidate : plan.rejected) {
            ++reasons[candidate.excluded.empty() ? "unknown" : candidate.excluded];
        }
        std::string summary;
        int shown = 0;
        for (const auto& [reason, count] : reasons) {
            if (shown++ >= 3) break;
            if (!summary.empty()) summary += ", ";
            summary += reason + "=" + std::to_string(count);
        }
        if (plan.error == kProviderDown) {
            LOG_ERROR << "no provider available model=" << model
                      << " body_bytes=" << req->body().size()
                      << " queue_reason=" << plan.reason
                      << " reasons=" << (summary.empty() ? "none" : summary);
        } else {
            LOG_WARN << "no route model=" << model
                     << " body_bytes=" << req->body().size()
                     << " queue_reason=" << plan.reason
                     << " reasons=" << (summary.empty() ? "none" : summary);
        }
        const std::string detail = summary.empty() ? "" : " (" + summary + ")";
        callback(errorResponse(drogon::k503ServiceUnavailable,
                               plan.error.empty() ? "no_route" : plan.error,
                               "no healthy route for model '" + model + "'" + detail, rid));
        return;
    }

    QuotaTicket quota_ticket;
    if (quotas_) {
        QuotaLimits limits;
        limits.rpm = principal.rpm;
        limits.tpm = principal.tpm;
        limits.max_concurrent = principal.max_concurrent;
        limits.day_budget_nano = principal.day_budget_nano;
        const std::int64_t estimated_tokens = plan.estimated_in + plan.estimated_out;
        const NanoRub estimated_cost = plan.candidates.empty() ? 0 : plan.candidates.front().cost_nano;
        auto decision = quotas_->acquire(principal.token_id, limits, estimated_tokens,
                                         estimated_cost);
        if (!decision.allowed) {
            auto resp = errorResponse(drogon::k429TooManyRequests, decision.code,
                                      decision.message, rid);
            resp->addHeader("Retry-After", std::to_string(decision.retry_after_sec));
            callback(resp);
            return;
        }
        quota_ticket = decision.ticket;
    }

    bool capture_requested = false;
    if (config_.capture.enabled && bodies_.enabled()) {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) < config_.capture.sample_rate) {
            capture_requested = bodies_.put(rid, "request", "application/json", req->body(), false).has_value();
        }
    }

    if (stream) {
        executeStream(rid, body, std::move(plan), std::move(principal), ip,
                      std::move(callback), started, capture_requested, quota_ticket, session_id,
                      static_cast<std::int64_t>(req->body().size()));
    } else {
        executeNonStream(rid, body, std::move(plan), std::move(principal), ip,
                         std::move(callback), started, capture_requested, quota_ticket, session_id,
                         static_cast<std::int64_t>(req->body().size()));
    }
}

void ProxyService::executeNonStream(std::string request_id, Json::Value body, RoutePlan plan,
                                    Principal principal, std::string client_ip,
                                    std::function<void(const drogon::HttpResponsePtr&)> callback,
                                    std::chrono::steady_clock::time_point started,
                                    bool capture_requested, QuotaTicket ticket,
                                    std::string session_id, std::int64_t request_bytes) {
    std::thread([this, request_id = std::move(request_id), body = std::move(body),
                 plan = std::move(plan), principal = std::move(principal),
                 client_ip = std::move(client_ip), callback = std::move(callback), started,
                 capture_requested, ticket, session_id = std::move(session_id),
                 request_bytes]() mutable {
        CurlHttpResponse last;
        int attempts = 0;      // все попытки, включая повторы (журнал)
        int provider_tries = 0; // провайдеры, к которым реально обращались
        RouteCandidate chosen;
        std::int64_t ttfb_ms = 0;
        bool success = false;
        bool last_empty = false;
        std::string empty_detail;
        const int max_providers = providerBudget();
        const int retries = providerRetryAttempts();
        const int backoff_ms = providerRetryBackoffMs();
        std::vector<RetryNote> retry_notes;
        bool local_busy = false;
        for (std::size_t candidate_index = 0; candidate_index < plan.candidates.size();
             ++candidate_index) {
            const auto& candidate = plan.candidates[candidate_index];
            if (provider_tries >= max_providers) break;
            std::string inject_error;
            Json::Value upstream_body = buildUpstreamBody(body, plan, candidate, config_, inject_error);
            if (!inject_error.empty()) {
                LOG_ERROR << "candidate not buildable: model=" << plan.model
                          << " provider=" << candidate.provider_tag
                          << " router=" << candidate.router_id << " error='" << inject_error
                          << "'";
                continue;
            }
            const std::string payload = serializeJson(upstream_body);
            const auto* router = config_.findRouter(candidate.router_id);
            if (!router) {
                LOG_ERROR << "candidate router not found: model=" << plan.model
                          << " provider=" << candidate.provider_tag
                          << " router=" << candidate.router_id;
                continue;
            }
            bool next_candidate = false;
            CandidateAttempt attempt;
            attempt.retries = retries;
            attempt.backoff_ms = backoff_ms;
            attempt.patience_deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(failoverWaitSec(plan, candidate_index));
            for (int provider_attempt = 0;; ++provider_attempt) {
                if (!upstream_limiter_.tryAcquire(router->id, router->max_concurrent)) {
                    last = CurlHttpResponse{};
                    last.error = "upstream concurrency limit reached";
                    last_empty = false;
                    local_busy = true;
                    next_candidate = true;
                    break;
                }
                local_busy = false;
                if (provider_attempt == 0) ++provider_tries;
                ++attempts;
                CurlHttpRequest request = makeRequest(*router, payload, false);
                CurlHttpResponse response;
                const auto t0 = std::chrono::steady_clock::now();
                std::int64_t first_byte = 0;
                bool got_first = false;
                const bool transport_ok =
                    curl_.perform(request, response, {}, [&](const CurlHttpResponse&) {
                        if (!got_first) {
                            got_first = true;
                            first_byte = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - t0).count();
                        }
                    });
                upstream_limiter_.release(router->id);
                ttfb_ms = first_byte;
                const bool http_ok =
                    transport_ok && response.status >= 200 && response.status < 300;
                CompletionInspection inspection;
                if (http_ok) {
                    Json::Value parsed;
                    std::string parse_error;
                    if (parseJson(response.body, parsed, parse_error)) {
                        inspection = inspectCompletion(parsed);
                    } else {
                        inspection.error = parse_error.empty() ? "invalid JSON body" : parse_error;
                    }
                }
                // Пустой ответ провайдера — такой же отказ, как 5xx: журнал, серия
                // отказов и переход к следующему провайдеру.
                const bool empty = http_ok && !inspection.meaningful;
                if (http_ok && !empty) {
                    chosen = candidate;
                    last = std::move(response);
                    health_.noteSuccess(candidate.provider_tag);
                    success = true;
                    break;
                }
                last = std::move(response);
                if (empty) {
                    empty_detail = describeEmpty(inspection);
                    last.status = 200;
                    last.error = empty_detail;
                }
                const long status_code = last.status;
                const std::string detail =
                    last.error.empty()
                        ? (status_code == 0 ? std::string("transport error")
                                            : std::string("upstream error"))
                        : last.error;
                last_empty = empty;
                const AttemptResult outcome{empty, status_code, detail};
                if (!handleAttemptFailure(attempt, plan.model, candidate.provider_tag,
                                          candidate.router_id, provider_attempt, outcome,
                                          retry_notes, next_candidate)) {
                    break;
                }
            }
            // Серия считается по запросам: отказ одной попытки в ожидании не должен
            // уводить провайдера из маршрута раньше правила по длительности.
            if (!success && attempt.provider_failed) {
                health_.noteFailure(candidate.provider_tag, candidate.router_id, plan.model,
                                    attempt.failed_status, attempt.failed_detail,
                                    attempt.first_failure_epoch);
            }
            if (success) break;
            if (!next_candidate) break;
        }
        const std::string retry_detail = retriesJson(retry_notes);

        const auto elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        const std::string switch_cause =
            applyRouteSwitch(plan, chosen, success, session_id);
        drogon::HttpResponsePtr resp;
        int status = success ? static_cast<int>(last.status)
                             : (last_empty || last.status <= 0 ? 502
                                                               : static_cast<int>(last.status));
        std::string journal_error;
        std::int64_t tokens_in = 0, tokens_out = 0;
        std::int64_t cache_read_tokens = 0, cache_write_tokens = 0;
        bool captured = false;

        if (success) {
            Json::Value parsed;
            std::string perr;
            if (parseJson(last.body, parsed, perr) && parsed.isObject()) {
                auto usage = parseUsage(parsed["usage"]);
                applyTimingsCache(usage, parsed);
                tokens_in = usage.prompt_tokens;
                tokens_out = usage.completion_tokens;
                cache_read_tokens = usage.cache_read_tokens;
                cache_write_tokens = usage.cache_write_tokens;
            }
            resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
            const auto ct = last.headers.find("content-type");
            resp->setContentTypeString(ct != last.headers.end() ? ct->second : "application/json");
            resp->setBody(last.body);
        } else {
            const TerminalResponse terminal = terminalResponse(
                last, local_busy, last_empty, empty_detail, request_id, attempts == 0);
            resp = terminal.resp;
            status = terminal.status;
            journal_error = terminal.journal_error;
        }

        // Остановка работы — ошибка: причина уходит в лог и в журнал, главная
        // страница показывает недоступного провайдера отдельным сообщением.
        if (!success) {
            LOG_ERROR << "request failed: model=" << plan.model
                      << " providers=" << provider_tries << " attempts=" << attempts
                      << " status=" << status
                      << " error='" << (last_empty ? empty_detail : last.error) << "'";
        }

        resp->addHeader("X-Proxy-Request-Id", request_id);
        if (!chosen.router_id.empty()) {
            resp->addHeader("X-Proxy-Router", chosen.router_id);
            resp->addHeader("X-Proxy-Provider", chosen.provider_tag);
            resp->addHeader("X-Proxy-Route-Reason", plan.reason);
        }

        if (capture_requested && config_.capture.enabled && bodies_.enabled() &&
            (status < 400 || config_.capture.capture_errors) && !last.body.empty()) {
            const auto ct = last.headers.find("content-type");
            captured = bodies_
                           .put(request_id, "response",
                                ct != last.headers.end() ? ct->second : "application/json",
                                last.body, false)
                           .has_value();
        }
        recordAtCompletion(plan, chosen, request_id, principal, client_ip, status, false,
                           elapsed, static_cast<int>(ttfb_ms), request_bytes,
                           static_cast<std::int64_t>(last.body.size()), tokens_in, tokens_out,
                           cache_read_tokens, cache_write_tokens, attempts, switch_cause,
                           journal_error, retry_detail, captured, ticket, session_id);

        auto loop = drogon::app().getLoop();
        if (loop) loop->queueInLoop([callback = std::move(callback), resp]() { callback(resp); });
    }).detach();
}

void ProxyService::executeStream(std::string request_id, Json::Value body, RoutePlan plan,
                                 Principal principal, std::string client_ip,
                                 std::function<void(const drogon::HttpResponsePtr&)> callback,
                                 std::chrono::steady_clock::time_point started,
                                 bool capture_requested, QuotaTicket ticket,
                                 std::string session_id, std::int64_t request_bytes) {
    auto started_response = std::make_shared<std::atomic<bool>>(false);
    auto stream_counted = std::make_shared<std::atomic<bool>>(false);
    const bool capture_this = capture_requested && config_.capture.enabled && bodies_.enabled() &&
                              config_.capture.capture_stream;

    std::thread([this, request_id, body = std::move(body), plan = std::move(plan),
                 principal = std::move(principal), client_ip = std::move(client_ip),
                 callback = std::move(callback), started, started_response, capture_this,
                 stream_counted, ticket, session_id = std::move(session_id),
                 request_bytes]() mutable {
        long response_status = 0;
        std::map<std::string, std::string> response_headers;
        const int max_providers = providerBudget();
        const int retries = providerRetryAttempts();
        const int backoff_ms = providerRetryBackoffMs();
        std::vector<RetryNote> retry_notes;
        int attempts = 0;       // все попытки, включая повторы (журнал)
        int provider_tries = 0; // провайдеры, к которым реально обращались
        CurlHttpResponse last;
        RouteCandidate chosen;
        bool started_ok = false;
        bool last_curl_ok = true;
        bool last_empty = false;
        std::string empty_detail;
        bool local_busy = false;
        std::int64_t ttfb_ms = 0;
        std::int64_t stream_bytes = 0;
        std::string captured_stream;
        std::string stream_tap;
        std::shared_ptr<StreamBridge> stream_bridge;
        for (std::size_t candidate_index = 0; candidate_index < plan.candidates.size();
             ++candidate_index) {
            const auto& candidate = plan.candidates[candidate_index];
            if (provider_tries >= max_providers) break;
            std::string inject_error;
            Json::Value upstream_body = buildUpstreamBody(body, plan, candidate, config_, inject_error);
            if (!inject_error.empty()) {
                LOG_ERROR << "candidate not buildable: model=" << plan.model
                          << " provider=" << candidate.provider_tag
                          << " router=" << candidate.router_id << " error='" << inject_error
                          << "'";
                continue;
            }
            const std::string payload = serializeJson(upstream_body);
            const auto* router = config_.findRouter(candidate.router_id);
            if (!router) {
                LOG_ERROR << "candidate router not found: model=" << plan.model
                          << " provider=" << candidate.provider_tag
                          << " router=" << candidate.router_id;
                continue;
            }
            bool next_candidate = false;
            CandidateAttempt attempt;
            attempt.retries = retries;
            attempt.backoff_ms = backoff_ms;
            attempt.patience_deadline =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(failoverWaitSec(plan, candidate_index));
            for (int provider_attempt = 0;; ++provider_attempt) {
                response_status = 0;
                stream_bytes = 0;
                auto bridge = std::make_shared<StreamBridge>();
                stream_bridge = bridge;
                captured_stream.clear();
                stream_tap.clear();
                response_headers.clear();
                auto inspector = std::make_shared<SseInspector>();
                if (!upstream_limiter_.tryAcquire(router->id, router->max_concurrent)) {
                    last = CurlHttpResponse{};
                    last.error = "upstream concurrency limit reached";
                    last_empty = false;
                    local_busy = true;
                    next_candidate = true;
                    break;
                }
                local_busy = false;
                if (provider_attempt == 0) ++provider_tries;
                ++attempts;
                CurlHttpRequest request = makeRequest(*router, payload, true);
                CurlHttpResponse response;
                const auto t0 = std::chrono::steady_clock::now();
                bool headers_started = false;
                // Заголовки и первый содержательный фрагмент уходят клиенту вместе:
                // пустой ответ провайдера ещё можно повторить у другого. Байты до
                // этого копит StreamBridge и отдаёт их при подключении потока.
                auto begin_response = [&]() {
                    if (started_response->exchange(true)) return;
                    ttfb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0).count();
                    if (!stream_counted->exchange(true)) stats_.incActiveStreams(1);
                    auto loop = drogon::app().getLoop();
                    if (!loop) return;
                    loop->queueInLoop([bridge, status = response_status,
                                       headers = response_headers, callback, request_id,
                                       router_id = candidate.router_id,
                                       provider = candidate.provider_tag,
                                       reason = plan.reason]() {
                        auto resp = drogon::HttpResponse::newAsyncStreamResponse(
                            [bridge](drogon::ResponseStreamPtr stream) {
                                bridge->attach(std::move(stream));
                            }, true);
                        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
                        if (headers.count("content-type")) {
                            resp->setContentTypeString(headers.at("content-type"));
                        } else {
                            resp->setContentTypeString("text/event-stream");
                        }
                        resp->addHeader("X-Proxy-Request-Id", request_id);
                        resp->addHeader("X-Proxy-Router", router_id);
                        resp->addHeader("X-Proxy-Provider", provider);
                        resp->addHeader("X-Proxy-Route-Reason", reason);
                        callback(resp);
                    });
                };
                const bool curl_ok = curl_.perform(
                    request, response,
                    [&](const char* data, std::size_t len) {
                        stream_bytes += static_cast<std::int64_t>(len);
                        if (capture_this && captured_stream.size() < config_.capture.max_stream_bytes) {
                            const auto remain = config_.capture.max_stream_bytes - captured_stream.size();
                            captured_stream.append(data, std::min(len, remain));
                        }
                        if (stream_tap.size() < kStreamTapLimit) {
                            const auto remain = kStreamTapLimit - stream_tap.size();
                            stream_tap.append(data, std::min(len, remain));
                        }
                        inspector->feed(data, len);
                        if (!started_response->load() && inspector->meaningful()) begin_response();
                        return !bridge->aborted() && bridge->onData(data, len);
                    },
                    [&](const CurlHttpResponse& h) {
                        if (headers_started) return;
                        headers_started = true;
                        response_status = h.status;
                        response_headers = h.headers;
                    });
                upstream_limiter_.release(router->id);
                last_curl_ok = curl_ok;
                last = std::move(response);
                inspector->finish();
                if (!started_response->load() && response_status >= 200 &&
                    response_status < 300 && inspector->meaningful()) {
                    begin_response();
                }
                if (response_status >= 200 && response_status < 300 && started_response->load()) {
                    chosen = candidate;
                    started_ok = true;
                    health_.noteSuccess(candidate.provider_tag);
                    break;
                }
                const bool empty =
                    response_status >= 200 && response_status < 300 && !started_response->load();
                if (empty) {
                    empty_detail = describeEmpty(inspector->inspection());
                    last.status = 200;
                    last.error = empty_detail;
                } else if (last.error.empty() && response_status == 0) {
                    last.error = "transport error";
                }
                const long status_code = last.status;
                const std::string detail =
                    last.error.empty() ? std::string("upstream error") : last.error;
                last_empty = empty;
                const AttemptResult outcome{empty, status_code, detail};
                if (!handleAttemptFailure(attempt, plan.model, candidate.provider_tag,
                                          candidate.router_id, provider_attempt, outcome,
                                          retry_notes, next_candidate)) {
                    break;
                }
            }
            if (!started_ok && attempt.provider_failed) {
                health_.noteFailure(candidate.provider_tag, candidate.router_id, plan.model,
                                    attempt.failed_status, attempt.failed_detail,
                                    attempt.first_failure_epoch);
            }
            if (started_ok) break;
            if (!next_candidate) break;
        }

        if (!started_ok) {
            const std::string switch_cause = applyRouteSwitch(plan, chosen, false, session_id);
            const TerminalResponse terminal = terminalResponse(
                last, local_busy, last_empty, empty_detail, request_id, attempts == 0);
            auto resp = terminal.resp;
            const int status = terminal.status;
            if (stream_bridge) stream_bridge->close();
            LOG_ERROR << "request failed: model=" << plan.model << " providers=" << provider_tries
                      << " attempts=" << attempts << " status=" << status << " error='"
                      << (last_empty ? empty_detail : last.error) << "'";
            auto loop = drogon::app().getLoop();
            if (loop) loop->queueInLoop([callback = std::move(callback), resp]() { callback(resp); });
            recordAtCompletion(plan, chosen, request_id, principal, client_ip, status, true,
                               static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started).count()),
                               static_cast<int>(ttfb_ms), request_bytes, 0,
                               0, 0, 0, 0, attempts, switch_cause, terminal.journal_error,
                               retriesJson(retry_notes), false, ticket, session_id);
            if (stream_counted->exchange(false)) stats_.incActiveStreams(-1);
            return;
        }

        std::int64_t tokens_in = 0, tokens_out = 0;
        std::int64_t cache_read_tokens = 0, cache_write_tokens = 0;
        for (const auto& line : split(stream_tap, '\n')) {
            std::string t = trim(line);
            if (!startsWith(t, "data:")) continue;
            t = trim(std::string_view(t).substr(5));
            if (t.empty() || t == "[DONE]") continue;
            Json::Value chunk;
            std::string perr;
            if (parseJson(t, chunk, perr) && chunk.isObject() && chunk["usage"].isObject()) {
                auto usage = parseUsage(chunk["usage"]);
                applyTimingsCache(usage, chunk);
                if (usage.prompt_tokens > 0) tokens_in = usage.prompt_tokens;
                if (usage.completion_tokens > 0) tokens_out = usage.completion_tokens;
                if (usage.cache_read_tokens > 0) cache_read_tokens = usage.cache_read_tokens;
                if (usage.cache_write_tokens > 0) cache_write_tokens = usage.cache_write_tokens;
            }
        }
        if (!last_curl_ok && stream_bridge && !stream_bridge->aborted()) {
            health_.noteFailure(chosen.provider_tag, chosen.router_id, plan.model, 200,
                                "stream truncated");
            LOG_WARN << "provider error: model=" << plan.model
                     << " provider=" << chosen.provider_tag << " router=" << chosen.router_id
                     << " status=200 error='stream truncated'";
        }
        if (stream_bridge) stream_bridge->close();
        bool captured = false;
        if (capture_this && !captured_stream.empty()) {
            captured = bodies_
                           .put(request_id, "sse", "text/event-stream", captured_stream,
                                captured_stream.size() >= config_.capture.max_stream_bytes)
                           .has_value();
            if (!captured) LOG_WARN << "response body not captured: " << request_id;
        }
        if (stream_counted->exchange(false)) stats_.incActiveStreams(-1);
        const std::string switch_cause = applyRouteSwitch(plan, chosen, true, session_id);
        recordAtCompletion(plan, chosen, request_id, principal, client_ip, 200, true,
                           static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started).count()),
                           static_cast<int>(ttfb_ms), request_bytes, stream_bytes,
                           tokens_in, tokens_out,
                           cache_read_tokens, cache_write_tokens, attempts, switch_cause,
                           (!last_curl_ok && stream_bridge && !stream_bridge->aborted())
                               ? "stream_truncated"
                               : "",
                           retriesJson(retry_notes), captured, ticket, session_id);
    }).detach();
}

int ProxyService::providerRetryAttempts() const {
    return std::max(0, config_.routing.retry.attempts);
}

int ProxyService::providerRetryBackoffMs() const {
    return std::max(0, config_.routing.retry.backoff_ms);
}

// Бюджет fallback считает провайдеров, а не попытки: одна первая попытка плюс
// `routing.fallback_attempts` переходов.
int ProxyService::providerBudget() const {
    return 1 + std::max(0, config_.routing.fallback_attempts);
}

std::int64_t ProxyService::failoverWaitSec(const RoutePlan& plan,
                                           std::size_t candidate_index) const {
    const auto& cfg = config_.routing;
    if (candidate_index != 0 || cfg.failover_wait_max_sec <= 0) return 0;
    if (!(cfg.wait_price_rub_per_sec > 0.0)) return 0;
    if (plan.candidates.size() < 2) return 0;
    const RouteCandidate& current = plan.candidates[candidate_index];
    // Следующий оффер другого провайдера: дубль того же тега на другом роутере
    // кеш не теряет и цену ожидания не меняет.
    const RouteCandidate* next = nullptr;
    for (std::size_t i = candidate_index + 1; i < plan.candidates.size(); ++i) {
        if (plan.candidates[i].provider_tag != current.provider_tag) {
            next = &plan.candidates[i];
            break;
        }
    }
    if (!next) return 0;
    const NanoRub gap_per_million =
        next->weight_per_million_nano - current.weight_per_million_nano;
    if (gap_per_million <= 0) return 0;
    // Разница весов на профиле запроса — та цена, которую готовы заплатить за
    // переход; ожидание стоит `wait_price_rub_per_sec` за секунду.
    const std::int64_t tokens = std::max<std::int64_t>(1, plan.estimated_in + plan.estimated_out);
    const NanoRub gap_nano = totalFromPerMillion(gap_per_million, tokens);
    const NanoRub price_nano = rubToNano(cfg.wait_price_rub_per_sec);
    if (price_nano <= 0) return 0;
    return std::min<std::int64_t>(gap_nano / price_nano, cfg.failover_wait_max_sec);
}

std::string ProxyService::applyRouteSwitch(const RoutePlan& plan, const RouteCandidate& chosen,
                                           bool ok, const std::string& session_id) {
    std::string cause;
    if (ok && !chosen.provider_tag.empty() && !plan.candidates.empty() &&
        chosen.provider_tag != plan.candidates.front().provider_tag) {
        cause = "local_rebind";
    }
    if (cause.empty() && !plan.session_rebind_from.empty()) {
        // Привязка снята не отказом: порог стоимости (`kPinDropCostMargin`) или
        // исчезнувший провайдер. Причина уходит в журнал и в инцидент.
        cause = plan.session_rebind_reason == kPinDropCostMargin
                ? "session_cost_rebind"
                : (plan.session_rebind_reason == kProviderDown ? kProviderDown
                                                               : "session_rebind");
    }
    if (!cause.empty()) {
        queue_.rebindSession(session_id, plan.model, chosen.provider_tag);
    } else if (!ok) {
        // Ни один кандидат не ответил: привязку снимаем, следующий запрос выберет заново.
        queue_.clearSessionPin(session_id, plan.model);
    }
    return cause;
}

void ProxyService::recordAtCompletion(const RoutePlan& plan, const RouteCandidate& candidate,
                                      const std::string& request_id, const Principal& principal,
                                      const std::string& client_ip, int status, bool stream,
                                      int latency_ms, int ttfb_ms, std::int64_t bytes_in,
                                      std::int64_t bytes_out, std::int64_t tokens_in,
                                      std::int64_t tokens_out,
                                      std::int64_t cache_read_tokens,
                                      std::int64_t cache_write_tokens, int attempts,
                                      const std::string& switch_cause,
                                      const std::string& error,
                                      const std::string& retry_detail, bool captured,
                                      QuotaTicket& ticket, const std::string& session_id) {
    RequestRecord r;
    r.request_id = request_id;
    r.token_id = principal.token_id;
    r.session_id = session_id;
    r.model = plan.model;
    r.router_id = candidate.router_id;
    r.provider_tag = candidate.provider_tag;
    r.route_reason = plan.reason;
    r.status = status;
    r.stream = stream;
    r.latency_ms = latency_ms;
    r.ttfb_ms = ttfb_ms;
    r.bytes_in = bytes_in;
    r.bytes_out = bytes_out;
    r.tokens_in = tokens_in;
    r.tokens_out = tokens_out;
    r.cache_read_tokens = std::clamp<std::int64_t>(cache_read_tokens, 0, std::max<std::int64_t>(0, tokens_in));
    r.cache_write_tokens = std::max<std::int64_t>(0, cache_write_tokens);
    // Факт — только отчёт провайдера о токенах. Запрос без отчёта (оборванный
    // поток) стоимости не даёт; его оценка по профилю модели хранится отдельно и
    // в итоги не входит.
    const bool usage_reported = tokens_in > 0 || tokens_out > 0;
    r.cost_estimated = !usage_reported && status < 400;
    r.cost_estimate_nano = r.cost_estimated ? candidate.cost_nano : 0;
    r.cost_actual_nano = 0;
    if (usage_reported) {
        const CostMix mix{tokens_in, tokens_out, r.cache_read_tokens, r.cache_write_tokens};
        const auto breakdown = candidate.cost(mix, config_.pricing.threshold_mode);
        r.cost_input_nano = breakdown.input_nano;
        r.cost_cache_read_nano = breakdown.cache_read_nano;
        r.cost_output_nano = breakdown.output_nano;
        r.cache_saved_nano = breakdown.saved_nano;
        r.cost_actual_nano = breakdown.total_nano;
    }
    r.attempts = attempts;
    r.switch_cause = switch_cause;
    r.error_code = error;
    // Ошибки провайдера, после которых запрос был повторён: видно в журнале.
    r.event_detail = retry_detail;
    // Серия успешных ответов без попаданий в кеш: пороги `routing.cache_miss`
    // решают, где предупреждение в журнале, а где автоматический чёрный список.
    if (r.status < 400 && r.tokens_in > 0 && r.cache_read_tokens <= 0) {
        const CacheMissOutcome misses =
            stats_.cacheMissOutcome(session_id, plan.model, candidate.provider_tag);
        if (misses.blacklist) {
            LOG_ERROR << "cache miss streak: session=" << session_id << " model=" << plan.model
                      << " provider=" << candidate.provider_tag << " misses=" << misses.misses
                      << " tokens_in=" << r.tokens_in;
            if (r.error_code.empty()) r.error_code = kCacheMissBlacklistCode;
            if (on_cache_miss_blacklist_) {
                std::string reason = "cache_miss_streak: " + std::to_string(misses.misses) +
                                     " запросов подряд без кеша";
                if (!session_id.empty()) reason += ", сессия " + session_id;
                on_cache_miss_blacklist_(candidate.provider_tag, plan.model, reason);
            }
        } else if (misses.warning && r.error_code.empty()) {
            r.error_code = kCacheMissStreakCode;
        }
    }
    r.client_ip = client_ip;
    r.captured = captured;
    r.ts = std::chrono::system_clock::now();
    if (quotas_) {
        // Бюджет потребителя уменьшает тот же факт, что и итоги статистики.
        quotas_->settle(ticket, tokens_in + tokens_out, effectiveCost(r));
    }
    stats_.record(r);
}

Json::Value ProxyService::modelsJson() const {
    Json::Value v(Json::objectValue);
    Json::Value arr(Json::arrayValue);
    for (const auto& m : config_.routing.models) {
        Json::Value x(Json::objectValue);
        x["id"] = m;
        x["object"] = "model";
        arr.append(x);
    }
    v["object"] = "list";
    v["data"] = arr;
    Json::Value aliases(Json::objectValue);
    for (const auto& [a, m] : config_.routing.aliases) aliases[a] = m;
    v["aliases"] = aliases;
    return v;
}

Json::Value ProxyService::explain(const std::string& model) const {
    return queue_.explain(model, downProviders());
}

} // namespace proxy
