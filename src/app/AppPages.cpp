#include "app/App.h"
#include "app/Endpoints.h"
#include "app/PanelQuery.h"
#include "app/ServiceControl.h"
#include "PageAssets.h"
#include "proxy/UpstreamFault.h"
#include "util/Http.h"
#include "util/Ratio.h"
#include "util/Strings.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace proxy {
namespace {

// Строк в таблицах CSV-агрегатов панели.
constexpr std::size_t kCsvTableRows = 12;
// Провайдеров в сводке на странице логов.
constexpr int kTopProviders = 10;
// Сессий в сводке на главной странице.
constexpr std::size_t kDashboardSessions = 5;
// Символов идентификатора сессии, видимых в таблицах.
constexpr std::size_t kSessionIdDisplayChars = 24;
// Ещё один предлагаемый размер страницы журнала, строк.
constexpr int kLogPageMid = 100;

std::string localDateTime(std::int64_t epoch) {
    if (epoch <= 0) return "—";
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32]{};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) == 0) return "—";
    return buf;
}

std::int64_t nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Форма русского существительного: 1 минута, 2 минуты, 5 минут.
const char* pluralRu(std::int64_t n, const char* one, const char* few, const char* many) {
    const std::int64_t mod100 = n % 100;
    const std::int64_t mod10 = n % 10;
    if (mod100 >= 11 && mod100 <= 14) return many;
    if (mod10 == 1) return one;
    if (mod10 >= 2 && mod10 <= 4) return few;
    return many;
}

// Относительный отрезок времени в человеческом виде: «23 минуты назад»,
// «1.5 дня назад». Дробное значение — форма родительного падежа (минуты, дня).
std::string relativeTime(std::int64_t epoch, std::int64_t now) {
    if (epoch <= 0) return "—";
    std::int64_t delta = now - epoch;
    if (delta < 0) delta = 0;
    if (delta < 60) {
        return std::to_string(delta) + " " +
               pluralRu(delta, "секунду", "секунды", "секунд") + " назад";
    }
    double value = 0.0;
    const char* one = nullptr;
    const char* few = nullptr;
    const char* many = nullptr;
    if (delta < 3600) {
        value = static_cast<double>(delta) / 60.0;
        one = "минуту"; few = "минуты"; many = "минут";
    } else if (delta < 86400) {
        value = static_cast<double>(delta) / 3600.0;
        one = "час"; few = "часа"; many = "часов";
    } else if (delta < 30 * 86400) {
        value = static_cast<double>(delta) / 86400.0;
        one = "день"; few = "дня"; many = "дней";
    } else {
        value = static_cast<double>(delta) / (30.0 * 86400.0);
        one = "месяц"; few = "месяца"; many = "месяцев";
    }
    const long long tenths = std::llround(value * 10.0);
    std::ostringstream os;
    if (tenths % 10 == 0) os << (tenths / 10);
    else os << (tenths / 10) << "." << (tenths % 10);
    const bool fractional = tenths % 10 != 0;
    const auto whole = static_cast<std::int64_t>(tenths / 10);
    return os.str() + " " + (fractional ? few : pluralRu(whole, one, few, many)) + " назад";
}

// Абсолютное время и относительный отрезок в одном формате: все места панели,
// где показывается момент времени, читаются одинаково.
std::string timeWithAge(std::int64_t epoch, std::int64_t now) {
    const std::string absolute = localDateTime(epoch);
    if (absolute == "—") return absolute;
    return absolute + " · " + relativeTime(epoch, now);
}

std::string formatSpeed(double tokens_per_second) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << std::setprecision(2) << tokens_per_second;
    return os.str();
}

std::string formatUptime(std::int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const std::int64_t days = seconds / 86400;
    seconds %= 86400;
    const std::int64_t hours = seconds / 3600;
    seconds %= 3600;
    const std::int64_t minutes = seconds / 60;
    const std::int64_t secs = seconds % 60;
    std::ostringstream os;
    if (days > 0) {
        os << days << "д " << hours << "ч " << minutes << "м";
    } else if (hours > 0) {
        os << hours << "ч " << minutes << "м";
    } else if (minutes > 0) {
        os << minutes << "м " << secs << "с";
    } else {
        os << secs << "с";
    }
    return os.str();
}

// JSON is rendered inside an element, so only markup-breaking characters need
// escaping; quotes stay readable.
std::string escapeJsonForHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else if (c == '&') {
            out += "&amp;";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string pageParam(const drogon::HttpRequestPtr& req, const char* name) {
    std::string value = trim(req->getParameter(name));
    if (value.size() > 128) value.resize(128);
    return value;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Доля 0..1 в целые проценты: единая точка для всех «percent» в панели.
int percent(double ratio) { return static_cast<int>(std::llround(ratio * 100.0)); }

// Ширина полосы в процентах: доля значения от максимума серии.
int barPercent(std::int64_t value, std::int64_t max) {
    if (max <= 0) return 0;
    return static_cast<int>(100.0 * static_cast<double>(value) / static_cast<double>(max));
}

// Причина отказа провайдера: журнал хранит технический текст, панель — понятный.
std::string providerReason(const std::string& error) {
    if (error.rfind("empty completion", 0) == 0) return "пустой ответ";
    if (error == "stream truncated") return "поток прерван";
    if (error == "transport error") return "ошибка сети";
    return error;
}

// Понятный текст для кода предупреждения/ошибки: панель показывает его в журнале.
std::string issueText(const std::string& code) {
    if (code == kCacheMissStreakCode) return "предупреждение: нет попаданий в кеш";
    if (code == kCacheMissBlacklistCode) {
        return "ошибка: провайдер добавлен в чёрный список (серия без кеша)";
    }
    if (code == kProviderSwitchCacheLossCode) {
        return "предупреждение: смена провайдера обнулила кеш";
    }
    if (code == kProviderAffinityRebindCode) {
        return "сессия переключена на другого провайдера";
    }
    if (code == kProviderDown) {
        return "ошибка: провайдер не работает";
    }
    // Коды отказа провайдера: текст обязан быть у каждого кода, который попадает
    // в журнал, иначе панель покажет техническую строку.
    if (code == kUpstreamEmptyCompletionCode) {
        return "ошибка: провайдер вернул пустой ответ";
    }
    if (code == kRouteNotBuildableCode) return "ошибка: запрос к провайдеру не собран";
    if (code == kUpstreamBusyCode) return "ошибка: локальный провайдер занят";
    if (code == upstreamFaultCode(UpstreamFault::RateLimited)) {
        return "ошибка: провайдер ограничил частоту запросов";
    }
    if (code == upstreamFaultCode(UpstreamFault::Timeout)) {
        return "ошибка: таймаут провайдера";
    }
    if (code == upstreamFaultCode(UpstreamFault::Auth)) {
        return "ошибка: провайдер отклонил авторизацию";
    }
    if (code == upstreamFaultCode(UpstreamFault::Unavailable)) {
        return "ошибка: провайдер недоступен";
    }
    if (code == upstreamFaultCode(UpstreamFault::Server)) {
        return "ошибка: отказ провайдера (5xx)";
    }
    return code;
}

void appendCard(std::ostringstream& html, const std::string& label, const std::string& value,
                const std::string& note = {}, const std::string& value_class = {},
                const std::string& href = {}) {
    html << "<div class=\"card\"><div class=\"muted\">" << htmlEscape(label)
         << "</div><div class=\"num" << (value_class.empty() ? "" : " " + value_class) << "\">"
         << htmlEscape(value) << "</div>";
    if (!note.empty()) html << "<div class=\"muted\">" << htmlEscape(note) << "</div>";
    if (!href.empty()) {
        html << "<a class=\"more\" href=\"" << htmlEscape(href) << "\">открыть в логах →</a>";
    }
    html << "</div>";
}

void appendSelect(std::ostringstream& html, const std::string& name,
                  const std::string& label,
                  const std::vector<std::pair<std::string, std::string>>& options,
                  const std::string& selected) {
    html << "<label>" << htmlEscape(label) << "<select name=\"" << htmlEscape(name)
         << "\">";
    bool found = false;
    for (const auto& [value, text] : options) {
        if (value == selected) found = true;
        html << "<option value=\"" << htmlEscape(value) << "\""
             << (value == selected ? " selected" : "") << ">" << htmlEscape(text)
             << "</option>";
    }
    if (!selected.empty() && !found) {
        html << "<option value=\"" << htmlEscape(selected) << "\" selected>"
             << htmlEscape(selected) << "</option>";
    }
    html << "</select></label>";
}

// Пагинатор журнала: те же фильтры, меняется только номер страницы.
void appendPaginator(std::ostringstream& html, const std::string& base, int page, int pages,
                     std::int64_t total,
                     const std::vector<std::pair<std::string, std::string>>& filters) {
    std::string query;
    for (const auto& [name, value] : filters) {
        if (value.empty()) continue;
        if (!query.empty()) query += "&";
        query += name + "=" + urlEncode(value);
    }
    const auto page_url = [&](int number) {
        return base + "/stats?" + query + "&page=" + std::to_string(number);
    };
    html << "<p class=\"muted\">Страница " << page << " из " << pages
         << " · всего запросов: " << total << "</p>";
    if (pages <= 1) return;
    const auto link = [&](int number, const std::string& label) {
        html << "<a class=\"btn btn-small\" href=\"" << htmlEscape(page_url(number))
             << "\">" << label << "</a>";
    };
    html << "<div class=\"links\">";
    if (page > 1) {
        link(1, "« первая");
        link(page - 1, "‹ назад");
    }
    const int from = std::max(1, page - 3);
    const int to = std::min(pages, page + 3);
    if (from > 1) {
        link(1, "1");
        html << "<span class=\"muted\">…</span>";
    }
    for (int number = from; number <= to; ++number) {
        if (number == page) {
            html << "<strong class=\"btn btn-small\">" << number << "</strong>";
        } else {
            link(number, std::to_string(number));
        }
    }
    if (to < pages) {
        html << "<span class=\"muted\">…</span>";
        link(pages, std::to_string(pages));
    }
    if (page < pages) {
        link(page + 1, "вперёд ›");
        link(pages, "последняя »");
    }
    html << "</div>";
}

// One configured consumer (agent) rendered for the dashboard and the startup log.
struct AgentEndpointView {
    std::string id;
    std::vector<std::string> urls;
    std::string models;
    std::string scopes;
    std::string limits;
    std::string networks;
    std::string expires;
    std::string budget;
};

std::string formatBudget(long double rub) {
    if (rub <= 0) return "без лимита";
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << std::setprecision(6) << static_cast<double>(rub);
    std::string text = os.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text.empty()) text = "0";
    return text + " ₽";
}

std::string agentModels(const std::vector<std::string>& models) {
    if (models.empty() || (models.size() == 1 && models.front() == "*")) return "все";
    return join(models, ", ");
}

std::vector<AgentEndpointView> agentEndpointViews(const AppConfig& config) {
    const auto urls = agentApiUrls(config);
    std::vector<AgentEndpointView> views;
    views.reserve(config.tokens.size());
    for (const auto& token : config.tokens) {
        AgentEndpointView v;
        v.id = token.id.empty() ? "(без id)" : token.id;
        v.urls = urls;
        v.models = agentModels(token.models);
        v.scopes = token.scopes.empty() ? "—" : join(token.scopes, ", ");
        std::vector<std::string> limits;
        if (token.rpm > 0) limits.push_back("rpm " + std::to_string(token.rpm));
        if (token.tpm > 0) limits.push_back("tpm " + std::to_string(token.tpm));
        if (token.max_concurrent > 0) {
            limits.push_back("параллельно " + std::to_string(token.max_concurrent));
        }
        v.limits = limits.empty() ? "без лимитов" : join(limits, " · ");
        v.networks = token.allowed_networks.empty() ? "любые"
                                                    : join(token.allowed_networks, ", ");
        v.expires = token.expires_at.empty() ? "бессрочно" : token.expires_at;
        v.budget = formatBudget(token.day_budget_rub);
        views.push_back(std::move(v));
    }
    return views;
}

} // namespace

drogon::HttpResponsePtr ProxyApp::loginPage(const std::string& error) const {
    std::ostringstream html;
    html << "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
         << "<title>CostRelay — login</title><style>body{font-family:system-ui,sans-serif;"
         << "max-width:420px;margin:8vh auto;padding:24px;background:#f7f9fc;color:#172033}"
         << "input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 14px;"
         << "border:1px solid #cbd5e1;border-radius:8px}button{padding:10px 18px;border:0;"
         << "border-radius:8px;background:#1f6feb;color:#fff;font-weight:600}"
         << ".err{color:#b42318;background:#fef3f2;padding:10px;border-radius:8px}</style>"
         << "</head><body><h1>CostRelay</h1>";
    if (!error.empty()) html << "<p class=\"err\">" << htmlEscape(error) << "</p>";
    html << "<form method=\"post\" action=\"" << htmlEscape(panelBase() + "/login") << "\">"
         << "<label>Имя пользователя</label><input name=\"username\" autocomplete=\"username\" autofocus>"
         << "<label>Пароль</label><input name=\"password\" type=\"password\" autocomplete=\"current-password\">"
         << "<button type=\"submit\">Войти</button></form></body></html>";
    auto resp = textResponse(html.str(), drogon::k200OK, "text/html; charset=utf-8");
    resp->addHeader("Content-Security-Policy",
                    "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                    "form-action 'self'; frame-ancestors 'none'");
    return resp;
}

Json::Value ProxyApp::overviewJson() const {
    Json::Value v = stats_.overview();
    if (stats_db_ && stats_db_->ok()) {
        // Суточные агрегаты — единственный источник «сегодня»: те же числа, что
        // уменьшаются при удалении записей журнала. Форма строки — у владельца,
        // в StatsDb::emptyDailyRow.
        const auto now = std::chrono::system_clock::now();
        const std::string today_day = utcDayString(now);
        const std::string previous_day = utcDayString(now - std::chrono::hours(24));
        Json::Value today = StatsDb::emptyDailyRow(today_day);
        Json::Value previous = StatsDb::emptyDailyRow(previous_day);
        for (const auto& day : stats_db_->daily(2)) {
            const std::string name = day["day"].asString();
            if (name == today_day) {
                today = day;
            } else if (name == previous_day) {
                previous = day;
            }
        }
        v["today"] = today;
        v["previous"] = previous;
    }
    v["provider_health"] = proxy_.healthJson();
    return v;
}

std::string ProxyApp::headerHtml(const Session& session) const {
    std::ostringstream html;
    html << "<header><strong>CostRelay</strong><nav>"
         << "<a href=\"/\">Главная</a>"
         << "<a href=\"" << htmlEscape(panelBase() + "/stats") << "\">Логи</a>"
         << "<a href=\"" << htmlEscape(panelBase() + "/routing") << "\">Маршрутизация</a>";
    for (const auto& app : config_.portal.apps) {
        if (!portalRoleAllowed(app, session.role)) {
            continue;
        }
        html << "<a href=\"" << htmlEscape("/" + app.id + "/") << "\">"
             << htmlEscape(app.title.empty() ? app.id : app.title) << "</a>";
    }
    html << "</nav><div class=\"header-right\">";
    if (session.role == "admin") {
        html << "<div class=\"menu\"><button class=\"btn btn-small\" id=\"service-menu-toggle\" "
                "type=\"button\" aria-haspopup=\"true\" aria-expanded=\"false\">Сервисы ▾</button>"
                "<div class=\"menu-panel\" id=\"service-menu\" hidden>";
        for (const auto& target : serviceTargets()) {
            html << "<button type=\"button\" data-restart=\"" << target.key
                 << "\">Перезапустить " << target.label << "</button>";
        }
        html << "<div id=\"service-restart-status\" class=\"muted\"></div></div></div>";
    }
    html << "<span>" << htmlEscape(session.username)
         << " · <form method=\"post\" action=\"" << htmlEscape(panelBase() + "/logout")
         << "\" style=\"display:inline;margin:0\"><input type=\"hidden\" name=\"csrf\" value=\""
         << htmlEscape(session.csrf)
         << "\"><button style=\"background:none;border:0;color:#c7d2fe;cursor:pointer;font:inherit\">"
            "logout</button></form></span></div></header>";
    if (session.role == "admin") {
        html << "<script>(()=>{const toggle=document.getElementById('service-menu-toggle');"
                "const menu=document.getElementById('service-menu');if(!toggle||!menu)return;"
                "const close=()=>{menu.hidden=true;toggle.setAttribute('aria-expanded','false')};"
                "toggle.addEventListener('click',e=>{e.stopPropagation();menu.hidden=!menu.hidden;"
                "toggle.setAttribute('aria-expanded',String(!menu.hidden))});"
                "document.addEventListener('click',e=>{if(!menu.hidden&&!menu.contains(e.target)&&e.target!==toggle)close()});"
                "document.addEventListener('keydown',e=>{if(e.key==='Escape')close()});"
                "const csrfMeta=document.querySelector('meta[name=\"csrf-token\"]');const csrf=csrfMeta?csrfMeta.content:'';"
                "const status=document.getElementById('service-restart-status');"
                "const api='"
             << htmlEscape(panelBase() + "/api/service-restart")
             << "';document.querySelectorAll('[data-restart]').forEach(btn=>{btn.addEventListener('click',async()=>{"
                "const service=btn.dataset.restart;const was=btn.textContent;btn.disabled=true;"
                "status.textContent='перезапуск: '+was+'…';try{"
                "const r=await fetch(api,{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},"
                "body:JSON.stringify({service})});const j=await r.json().catch(()=>({}));"
                "if(!r.ok)throw new Error((j.error&&j.error.message)||((j.message||('HTTP '+r.status))+(j.output?(': '+j.output):'')));"
                "status.textContent=j.message||('перезапуск '+service+' запущен');"
                "if(service==='proxy'){setTimeout(()=>location.reload(),6000)}}"
                "catch(e){status.textContent='ошибка: '+e.message}finally{btn.disabled=false}});})})();</script>";
    }
    return html.str();
}

drogon::HttpResponsePtr ProxyApp::pageResponse(std::string body) const {
    auto resp = textResponse(std::move(body), drogon::k200OK, "text/html; charset=utf-8");
    resp->addHeader("Content-Security-Policy",
                    "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                    "form-action 'self'; frame-ancestors 'none'");
    resp->addHeader("Cache-Control", "no-store");
    return resp;
}

void ProxyApp::appendPageStart(std::ostringstream& html, const std::string& title,
                               const Session& session) const {
    std::string page = kPageTemplate;
    replaceAll(page, "{{TITLE}}", htmlEscape(title));
    replaceAll(page, "{{CSRF}}", htmlEscape(session.csrf));
    replaceAll(page, "{{HEADER}}", headerHtml(session));
    html << page;
}

void ProxyApp::appendPageEnd(std::ostringstream& html) const {
    html << "</main></body></html>";
}

void ProxyApp::appendSessionsTable(std::ostringstream& html, const Json::Value& sessions,
                                   const std::string& link_extra) const {
    if (!sessions.isArray() || sessions.empty()) return;
    std::int64_t max_requests = 1;
    for (const auto& x : sessions) max_requests = std::max(max_requests, x["requests"].asInt64());
    html << "<table><tr><th>сессия</th><th>запросы</th><th>in/out tok</th>"
            "<th>bytes in/out</th><th>cache read/size / hit</th><th>₽</th><th>avg ms</th>"
            "<th>model</th><th>логи</th></tr>";
    for (const auto& x : sessions) {
        const auto ratio = x["cache_hit_ratio"].asDouble();
        const int width = barPercent(x["requests"].asInt64(), max_requests);
        const std::string full_sid = x["session_id"].asString();
        std::string sid = full_sid;
        if (sid.size() > kSessionIdDisplayChars) {
            sid = sid.substr(0, kSessionIdDisplayChars) + "…";
        }
        html << "<tr><td>" << htmlEscape(sid) << "<div class=\"bar\"><span style=\"width:"
             << std::clamp(width, 1, 100) << "%\"></span></div></td><td>"
             << x["requests"].asInt64() << "</td><td>" << x["tokens_in"].asInt64() << " / "
             << x["tokens_out"].asInt64() << "</td><td>" << x["bytes_in"].asInt64() << " / "
             << x["bytes_out"].asInt64() << "</td><td>" << x["cache_read_tokens"].asInt64()
             << " / " << x["max_cache_read_tokens"].asInt64() << " / "
             << percent(ratio) << "%</td><td>"
             << formatRubDisplay(x["cost_nano"].asInt64()) << " ₽<div class=\"muted\">in "
             << formatRubDisplay(x["cost_input_nano"].asInt64()) << " · cache "
             << formatRubDisplay(x["cost_cache_read_nano"].asInt64()) << " · out "
             << formatRubDisplay(x["cost_output_nano"].asInt64()) << "</div>";
        if (x["estimated_requests"].asInt64() > 0) {
            html << "<div class=\"muted\">оценка " << formatRubDisplay(x["cost_estimated_nano"].asInt64())
                 << " ₽ (" << x["estimated_requests"].asInt64() << " запр.)</div>";
        }
        html << "</td><td>"
             << x["avg_latency_ms"].asInt64() << "</td><td>"
             << htmlEscape(x["model"].asString()) << "</td><td>";
        if (full_sid.empty()) {
            html << "<span class=\"muted\">—</span>";
        } else {
            html << "<a class=\"btn btn-small\" href=\""
                 << htmlEscape(panelBase() + "/stats?session_id=" + urlEncode(full_sid) +
                               link_extra)
                 << "\">логи</a>";
        }
        html << "</td></tr>";
    }
    html << "</table>";
}

void ProxyApp::appendRecentProviders(std::ostringstream& html) const {
    if (!stats_db_ || !stats_db_->ok()) return;
    const auto rows = stats_db_->recentProviders(kTopProviders);
    if (!rows.isArray() || rows.empty()) return;
    const std::int64_t now = nowEpoch();
    const std::string latest = rows[0]["provider"].asString();
    std::int64_t switch_ts = 0;
    if (rows.size() > 1) {
        switch_ts = stats_db_->providerSwitchTs(latest, rows[1]["provider"].asString());
    }
    if (switch_ts <= 0) switch_ts = rows[0]["first_ts"].asInt64();
    html << "<h2>Провайдеры</h2><table><tr><th>provider</th><th>запросов</th>"
            "<th>₽ (всего)</th><th>in/cache/out</th><th>последний запрос</th></tr>";
    for (const auto& x : rows) {
        const auto cost_input = x["cost_input_nano"].asInt64();
        const auto cost_cache = x["cost_cache_read_nano"].asInt64();
        const auto cost_output = x["cost_output_nano"].asInt64();
        const auto cost_parts = cost_input + cost_cache + cost_output;
        html << "<tr><td>" << htmlEscape(x["provider"].asString()) << "</td><td>"
             << x["requests"].asInt64() << "</td><td>"
             << formatRubDisplay(x["cost_nano"].asInt64()) << " ₽</td><td>";
        if (cost_parts > 0) {
            html << "in " << percent(ratioOf(cost_input, cost_parts)) << "% · cache "
                 << percent(ratioOf(cost_cache, cost_parts)) << "% · out "
                 << percent(ratioOf(cost_output, cost_parts))
                 << "%<div class=\"muted\">" << formatRubDisplay(cost_input) << " · "
                 << formatRubDisplay(cost_cache) << " · " << formatRubDisplay(cost_output)
                 << " ₽</div>";
        } else {
            html << "<span class=\"muted\">—</span>";
        }
        html << "</td><td>" << timeWithAge(x["last_ts"].asInt64(), now) << "</td></tr>";
    }
    html << "</table><p class=\"muted\">";
    if (rows.size() > 1) {
        html << "Предыдущий: <b>" << htmlEscape(rows[1]["provider"].asString())
             << "</b>. Переключение на <b>" << htmlEscape(latest) << "</b>: "
             << timeWithAge(switch_ts, now);
    } else {
        html << "Первый провайдер: <b>" << htmlEscape(latest) << "</b>, с "
             << timeWithAge(switch_ts, now);
    }
    html << "</p>";
}

void ProxyApp::appendPriceReport(std::ostringstream& html,
                                 const std::string& model_filter,
                                 const std::string& provider_filter,
                                 bool hide_blocked) const {
    struct Usage {
        std::int64_t requests = 0;
        std::int64_t tokens_in = 0;
        std::int64_t tokens_out = 0;
        double cache_ratio = 0.0;
        double tokens_per_second = 0.0;
    };
    std::map<std::string, Usage> provider_usage; // model \x1f provider
    if (stats_db_ && stats_db_->ok()) {
        for (const auto& item : stats_db_->usageByModelProvider("")) {
            const std::string model = item["model"].asString();
            const std::string provider = item["provider"].asString();
            Usage u;
            u.requests = item["requests"].asInt64();
            u.tokens_in = item["tokens_in"].asInt64();
            u.tokens_out = item["tokens_out"].asInt64();
            u.cache_ratio = item["cache_hit_ratio"].asDouble();
            u.tokens_per_second = item["tokens_per_second"].asDouble();
            provider_usage[model + "\x1f" + provider] = u;
        }
    }

    struct Row {
        std::string model;
        RouteCandidate quote;
        bool nominal = false;
        bool has_usage = false;
        std::int64_t requests = 0;
        double cache_ratio = 0.0;
        std::optional<double> speed;
        std::optional<NanoRub> saved_per_million;
        // Доли компонентов в стоимости профиля по ценам этого оффера.
        int mix_in_percent = 0;
        int mix_cache_percent = 0;
        int mix_out_percent = 0;
        std::string mix_per_million;
    };
    std::vector<Row> rows;
    std::int64_t blocked_total = 0;
    std::int64_t rejected_total = 0;
    const auto snapshot = queue_.snapshot();
    for (const auto& model : config_.routing.models) {
        if (!model_filter.empty() && model != model_filter) continue;
        // Порядок строк — весовой порядок снимка (ниже — дешевле); живой запрос
        // идёт к лидеру очереди, который при гистерезисе может быть не первым.
        const QueueModelEntry* entry = snapshot ? snapshot->find(model) : nullptr;
        if (!entry) continue;
        const CostMix& mix = entry->profile;
        if (mix.billedTokens() <= 0) continue;
        const std::int64_t billed = mix.billedTokens();
        std::size_t model_rejected = 0;
        for (const auto& quote : entry->quotes) {
            if (!provider_filter.empty() && quote.provider_tag != provider_filter) continue;
            const bool blocked = quote.excluded == kProviderBlacklisted;
            if (blocked) {
                ++blocked_total;
                if (hide_blocked) continue;
            } else if (!quote.excluded.empty()) {
                ++model_rejected;
                continue;
            }
            Row row;
            row.model = model;
            row.quote = quote;
            row.nominal = entry->nominal();
            const auto pit = provider_usage.find(model + "\x1f" + quote.provider_tag);
            if (pit != provider_usage.end() && pit->second.tokens_in + pit->second.tokens_out > 0) {
                row.has_usage = true;
                row.requests = pit->second.requests;
                row.cache_ratio = pit->second.cache_ratio;
                if (pit->second.tokens_per_second > 0) row.speed = pit->second.tokens_per_second;
            }
            row.saved_per_million = perMillionFromTotal(quote.cache_saved_nano, billed);
            // Цены и профиль те же, что у weight, но без поправочных весов: доли
            // стоимости берутся из снимка очереди, а не считаются заново.
            const NanoRub parts =
                quote.input_cost_nano + quote.cache_cost_nano + quote.output_cost_nano;
            row.mix_in_percent = percent(ratioOf(quote.input_cost_nano, parts));
            row.mix_cache_percent = percent(ratioOf(quote.cache_cost_nano, parts));
            row.mix_out_percent = percent(ratioOf(quote.output_cost_nano, parts));
            row.mix_per_million =
                "in " +
                formatRubPerMillionTotal(perMillionFromTotal(quote.input_cost_nano, billed)) +
                " · cache " +
                formatRubPerMillionTotal(perMillionFromTotal(quote.cache_cost_nano, billed)) +
                " · out " +
                formatRubPerMillionTotal(perMillionFromTotal(quote.output_cost_nano, billed)) +
                " RUB/M";
            rows.push_back(std::move(row));
        }
        rejected_total += static_cast<std::int64_t>(model_rejected);
    }

    html << "<h2 id=\"prices\">Провайдеры и цены: сравнение</h2>";
    if (blocked_total > 0 || rejected_total > 0) {
        html << "<p class=\"muted\">";
        if (blocked_total > 0) {
            html << "В чёрном списке: <b>" << blocked_total << "</b> строк"
                 << (hide_blocked ? " (скрыты)" : "") << ". ";
        }
        if (rejected_total > 0) {
            html << "Исключено по правилам маршрутизации: <b>" << rejected_total << "</b>. ";
        }
        html << "</p>";
    }
    if (rows.empty()) {
        html << "<p class=\"muted\">Нет строк по текущим фильтрам или каталог цен не загружен.</p>";
        return;
    }
    html << R"HTML(<p class="muted">Сортировка — по заголовку колонки. <b>weight</b> — вес 1M токенов у провайдера: стоимость токенов по формуле с поправочными весами выхода и кеша (<code>computeTokenCost</code> по наблюдаемому профилю модели) плюс цена наблюдаемой задержки провайдера по <code>wait_price_rub_per_sec</code>, RUB/M; ниже — дешевле. Живой запрос ничего не пересчитывает: он идёт к лидеру очереди, а лидер удерживается гистерезисом и может быть не первым по весу. <b>lat</b> — наблюдаемая средняя задержка провайдера, мс; в подсказке weight — раздельно токены и время. <b>req</b> и <b>cache %</b> — фактические успешные запросы и доля кешированных входных токенов; <b>tok/s</b> — скорость генерации; <b>saved RUB/M</b> — экономия на кеше. <b>in/cache/out cost %</b> — примерное распределение стоимости профиля между входными, кешированными и выходными токенами по ценам этого провайдера (без поправочных весов); в подсказке — RUB/M каждого компонента.</p>
<div class="table-wrap"><table id="price-table"><thead><tr>
<th data-sort="model">model</th>
<th data-sort="provider">provider</th>
<th data-sort="in" data-numeric="1">in RUB/M</th>
<th data-sort="cache" data-numeric="1">cache RUB/M</th>
<th data-sort="out" data-numeric="1">out RUB/M</th>
<th data-sort="context" data-numeric="1">context</th>
<th data-sort="maxout" data-numeric="1">max out</th>
<th data-sort="threshold" data-numeric="1">threshold</th>
<th data-sort="quant">quant</th>
<th data-sort="status" data-numeric="1">status</th>
<th data-sort="req" data-numeric="1">req</th>
<th data-sort="cachepct" data-numeric="1">cache %</th>
<th data-sort="lat" data-numeric="1">lat ms</th>
<th data-sort="speed" data-numeric="1">tok/s</th>
<th data-sort="saved" data-numeric="1">saved RUB/M</th>
<th data-sort="weight" data-numeric="1">weight RUB/M</th>
<th data-sort="mixin" data-numeric="1">in cost %</th>
<th data-sort="mixcache" data-numeric="1">cache cost %</th>
<th data-sort="mixout" data-numeric="1">out cost %</th>
</tr></thead><tbody>)HTML";
    for (const auto& row : rows) {
        const auto& q = row.quote;
        const auto& o = q.offer;
        const bool blocked = q.excluded == kProviderBlacklisted;
        html << "<tr" << (blocked ? " class=\"row-blocked\"" : "")
             << " data-model='" << htmlEscape(row.model)
             << "' data-provider='" << htmlEscape(q.provider_tag)
             << "' data-blocked='" << (blocked ? "1" : "")
             << "' data-in='" << q.prompt_price_nano
             << "' data-cache='" << q.cache_read_price_nano
             << "' data-out='" << q.completion_price_nano
             << "' data-context='" << o.context_length
             << "' data-maxout='" << o.max_completion_tokens
             << "' data-threshold='" << o.threshold_tokens
             << "' data-quant='" << htmlEscape(o.quantization)
             << "' data-status='" << o.status
             << "' data-req='" << (row.has_usage ? std::to_string(row.requests) : "")
             << "' data-cachepct='"
             << (row.has_usage
                     ? std::to_string(percent(row.cache_ratio))
                     : "")
             << "' data-speed='" << (row.speed ? std::to_string(*row.speed) : "")
             << "' data-saved='" << (row.saved_per_million ? std::to_string(*row.saved_per_million) : "")
             << "' data-lat='" << q.latency_ms
             << "' data-weight='" << q.weight_per_million_nano
             << "' data-mixin='" << row.mix_in_percent
             << "' data-mixcache='" << row.mix_cache_percent
             << "' data-mixout='" << row.mix_out_percent
             << "'><td>" << htmlEscape(row.model) << "</td><td>"
             << htmlEscape(q.provider_tag);
        if (blocked) {
            html << "<span class=\"blocked-tag\">⛔ " << htmlEscape(q.excluded_detail)
                 << "</span>";
        }
        html << "</td><td>"
             << formatRubPerMillion(q.prompt_price_nano) << "</td><td>"
             << formatRubPerMillion(q.cache_read_price_nano) << "</td><td>"
             << formatRubPerMillion(q.completion_price_nano) << "</td><td>"
             << (o.context_length > 0 ? std::to_string(o.context_length) : "—")
             << "</td><td>"
             << (o.max_completion_tokens > 0 ? std::to_string(o.max_completion_tokens) : "—")
             << "</td><td>"
             << (o.threshold_tokens > 0 ? std::to_string(o.threshold_tokens) : "—")
             << "</td><td>"
             << (o.quantization.empty() ? "—" : htmlEscape(o.quantization)) << "</td><td>"
             << o.status << "</td><td>"
             << (row.has_usage ? std::to_string(row.requests) : "—") << "</td><td>"
             << (row.has_usage
                     ? std::to_string(percent(row.cache_ratio)) + "%"
                     : "—")
             << "</td><td>" << (q.latency_ms > 0 ? std::to_string(q.latency_ms) : "—")
             << "</td><td>" << (row.speed ? formatSpeed(*row.speed) : "—")
             << "</td><td>"
             << (row.saved_per_million ? formatRubPerMillionTotal(*row.saved_per_million) : "—")
             << "</td><td title='токены "
             << formatRubPerMillionTotal(q.cost_per_million_nano) << " RUB/M · время "
             << formatRubPerMillionTotal(q.latency_cost_per_million_nano) << " RUB/M'>"
             << formatRubPerMillionTotal(q.weight_per_million_nano)
             << (row.nominal ? " <span class=\"muted\">оценка</span>" : "")
             << "</td><td title='" << htmlEscape(row.mix_per_million) << "'>"
             << row.mix_in_percent << "%</td><td title='" << htmlEscape(row.mix_per_million)
             << "'>" << row.mix_cache_percent << "%</td><td title='"
             << htmlEscape(row.mix_per_million) << "'>" << row.mix_out_percent
             << "%</td></tr>";
    }
    html << R"HTML(</tbody></table>
</div><script>(()=>{const t=document.getElementById('price-table');if(!t)return;t.querySelectorAll('th[data-sort]').forEach(th=>{let dir=1;th.addEventListener('click',()=>{const k=th.dataset.sort,num=th.dataset.numeric==='1',rows=Array.from(t.tBodies[0].rows);rows.sort((a,b)=>{const av=a.dataset[k]||'',bv=b.dataset[k]||'';if(num){if(av===''&&bv==='')return 0;if(av==='')return 1;if(bv==='')return -1;return dir*(Number(av)-Number(bv))}return dir*av.localeCompare(bv,'ru',{numeric:true})});rows.forEach(r=>t.tBodies[0].appendChild(r));t.querySelectorAll('th').forEach(x=>x.removeAttribute('aria-sort'));th.setAttribute('aria-sort',dir===1?'ascending':'descending');dir=-dir})})})();</script>)HTML";
}

void ProxyApp::appendBlacklistManager(std::ostringstream& html, bool can_edit) const {
    html << "<h2 id=\"blacklist\">Чёрный список провайдеров</h2>";
    if (!can_edit) {
        if (config_.routing.provider_blacklist.empty()) {
            html << "<p class=\"muted\">Список пуст.</p>";
        } else {
            html << "<table><tr><th>provider</th><th>models</th><th>reason</th></tr>";
            for (const auto& block : config_.routing.provider_blacklist) {
                std::ostringstream models;
                for (std::size_t i = 0; i < block.models.size(); ++i) {
                    if (i) models << ", ";
                    models << block.models[i];
                }
                html << "<tr><td>" << htmlEscape(block.provider) << "</td><td>"
                     << htmlEscape(models.str()) << "</td><td>"
                     << htmlEscape(block.reason) << "</td></tr>";
            }
            html << "</table>";
        }
        return;
    }
    html << R"HTML(<p class="muted">Провайдеры с этими моделями исключаются из маршрутизации. Сохранение меняет файл конфигурации и сразу перестраивает очередь.</p>
<table id="blacklist-table"><thead><tr><th>provider</th><th>models (через запятую)</th><th>reason</th><th></th></tr></thead><tbody></tbody></table>
<div class="actions"><button class="btn" id="blacklist-add" type="button">Добавить</button><button class="btn" id="blacklist-save" type="button">Сохранить в конфиг</button><span id="blacklist-status" class="muted"></span></div>
<script>(()=>{const table=document.getElementById('blacklist-table');if(!table)return;const body=table.tBodies[0],status=document.getElementById('blacklist-status');const api=')HTML"
         << htmlEscape(panelBase()) << R"HTML(/api/blacklist';const addRow=(e={})=>{const tr=document.createElement('tr');const models=(e.models||[]).join(', ');for(const [field,value] of [['provider',e.provider||''],['models',models],['reason',e.reason||'blocked by policy']]){const td=document.createElement('td'),input=document.createElement('input');input.dataset.field=field;input.value=value;td.appendChild(input);tr.appendChild(td)}const td=document.createElement('td'),del=document.createElement('button');del.type='button';del.className='btn btn-small';del.textContent='×';del.onclick=()=>tr.remove();td.appendChild(del);tr.appendChild(td);body.appendChild(tr)};const load=async()=>{status.textContent='загрузка…';try{const r=await fetch(api,{headers:{Accept:'application/json'}});const j=await r.json();if(!r.ok)throw new Error(j.error&&j.error.message||('HTTP '+r.status));body.replaceChildren();(j.entries||[]).forEach(addRow);status.textContent=''}catch(e){status.textContent='ошибка: '+e.message}};document.getElementById('blacklist-add').addEventListener('click',()=>addRow());document.getElementById('blacklist-save').addEventListener('click',async()=>{const entries=Array.from(body.rows).map(tr=>{const get=f=>tr.querySelector('[data-field="'+f+'"]').value.trim();const models=get('models').split(',').map(x=>x.trim()).filter(Boolean);return {provider:get('provider'),models:models.length?models:['*'],reason:get('reason')}}).filter(e=>e.provider);status.textContent='сохранение…';try{const j=await panelPost(api,{entries});await load();status.textContent='сохранено: '+entries.length+(j.applied?'':' — не применено')}catch(e){status.textContent='ошибка: '+e.message}});load()})();</script>)HTML";
}

void ProxyApp::appendProviderAnalytics(std::ostringstream& html,
                                       const Session& session) const {
    const Json::Value analytics = analytics_.summaryJson();
    html << "<h2 id=\"provider-csv\">Статистика провайдера (CSV)</h2>";
    if (!analytics["enabled"].asBool()) {
        html << "<p class=\"muted\">Загрузка CSV отключена: <code>analytics.enabled: false</code>. "
                "Включите блок <code>analytics</code> в <code>config/costrelay.yaml</code> и "
                "перезапустите costrelayd.</p>";
        return;
    }
    html << "<div class=\"cards\">";
    appendCard(html, "CSV-строк", std::to_string(analytics["rows"].asInt64()));
    appendCard(html, "Моделей в логах", std::to_string(analytics["models"].asInt64()));
    appendCard(html, "Провайдеров в логах", std::to_string(analytics["providers"].asInt64()));
    appendCard(html, "Cache hit (логи)",
               std::to_string(percent(analytics["cache_hit_ratio"].asDouble())) + "%");
    appendCard(html, "Стоимость (логи), RUB", analytics["cost_rub"].asString());
    appendCard(html, "Avg latency (логи), мс",
               std::to_string(static_cast<int>(analytics["avg_latency_ms"].asDouble())));
    html << "</div>";
    html << "<p class=\"muted\">Сырые CSV не сохраняются: импортируются только "
            "агрегаты в память. Автозагрузка из <code>"
         << htmlEscape(analytics["log_dir"].asString())
         << "</code>; ниже — фактические цены и latency по провайдерам.</p>";

    appendCsvProviderTable(html);
    appendCsvModelTable(html);
    appendCsvTimeTable(html);

    if (session.role == "admin") {
        html << "<div style=\"margin-top:14px\"><input type=\"file\" id=\"provider-log-file\" "
                "accept=\".csv,text/csv\"> <button type=\"button\" onclick=\"uploadProviderLog()\">"
                "Загрузить CSV</button> <button type=\"button\" onclick=\"reloadProviderLogs()\">"
                "Перечитать data/</button><div id=\"provider-log-status\" class=\"muted\"></div></div>"
                "<script>async function uploadProviderLog(){"
                "const f=document.getElementById('provider-log-file').files[0];if(!f)return;"
                "const s=document.getElementById('provider-log-status');s.textContent='import...';"
                "const j=await panelRaw('"
             << htmlEscape(panelBase() + "/api/provider-logs/import")
             << "','text/csv',await f.text());"
                "s.textContent='rows='+j.rows+' skipped='+j.skipped+(j.duplicate?' duplicate':'')+(j.error?(' error='+j.error):'');"
                "if(j.ok)setTimeout(()=>location.reload(),600)}"
                "async function reloadProviderLogs(){const s=document.getElementById('provider-log-status');s.textContent='reload...';"
                "const j=await panelRaw('"
             << htmlEscape(panelBase() + "/api/provider-logs/reload")
             << "');s.textContent='files='+j.files+' rows='+j.rows+(j.error?(' error='+j.error):'');"
                "if(j.ok)setTimeout(()=>location.reload(),600)}</script>";
    } else {
        html << "<p class=\"muted\">Загрузка и перечитывание CSV доступны администратору.</p>";
    }
}

void ProxyApp::appendCsvProviderTable(std::ostringstream& html) const {
    const auto rows = analytics_.providersJson(kCsvTableRows);
    if (rows.empty()) return;
    html << "<table><tr><th>provider</th><th>model</th><th>req</th><th>in/out tok</th>"
            "<th>cache hit</th><th>in RUB/M</th><th>cache RUB/M</th><th>out RUB/M</th>"
            "<th>p50/p90 ms</th><th>₽</th></tr>";
    for (const auto& p : rows) {
        html << "<tr><td>" << htmlEscape(p["provider"].asString()) << "</td><td>"
             << htmlEscape(p["model"].asString()) << "</td><td>"
             << p["requests"].asInt64() << "</td><td>" << p["input_tokens"].asInt64()
             << " / " << p["output_tokens"].asInt64() << "</td><td>"
             << percent(p["cache_hit_ratio"].asDouble()) << "%</td><td>"
             << htmlEscape(p["input_per_million_rub"].asString()) << "</td><td>"
             << htmlEscape(p["cache_read_per_million_rub"].asString()) << "</td><td>"
             << htmlEscape(p["output_per_million_rub"].asString()) << "</td><td>"
             << static_cast<int>(p["p50_latency_ms"].asDouble()) << " / "
             << static_cast<int>(p["p90_latency_ms"].asDouble()) << "</td><td>"
             << formatRubDisplay(p["total_cost_nano"].asInt64()) << " ₽</td></tr>";
    }
    html << "</table>";
}

void ProxyApp::appendCsvModelTable(std::ostringstream& html) const {
    const auto rows = analytics_.modelsJson(kCsvTableRows);
    if (rows.empty()) return;
    html << "<h3>Сравнение моделей</h3><table><tr><th>model</th><th>providers</th>"
            "<th>req</th><th>cache hit</th><th>RUB/M tok</th><th>avg RUB</th>"
            "<th>p50/p90 ms</th><th>tok/s</th><th>reasoning %</th></tr>";
    for (const auto& m : rows) {
        html << "<tr><td>" << htmlEscape(m["model"].asString()) << "</td><td>"
             << m["providers"].asInt64() << "</td><td>" << m["requests"].asInt64()
             << "</td><td>" << percent(m["cache_hit_ratio"].asDouble())
             << "%</td><td>" << htmlEscape(m["cost_per_million_tokens_rub"].asString())
             << "</td><td>" << htmlEscape(m["avg_cost_rub"].asString()) << "</td><td>"
             << static_cast<int>(m["p50_latency_ms"].asDouble()) << " / "
             << static_cast<int>(m["p90_latency_ms"].asDouble()) << "</td><td>"
             << static_cast<int>(m["tokens_per_second"].asDouble()) << "</td><td>"
             << percent(m["reasoning_ratio"].asDouble()) << "%</td></tr>";
    }
    html << "</table>";
}

void ProxyApp::appendCsvTimeTable(std::ostringstream& html) const {
    const auto rows = analytics_.timesJson(kCsvTableRows);
    if (rows.empty()) return;
    html << "<h3>По времени работы</h3><table><tr><th>date</th><th>hour</th>"
            "<th>provider</th><th>req</th><th>in/cache/out RUB/M</th><th>cache hit</th>"
            "<th>avg ms</th><th>tok/s</th><th>₽</th></tr>";
    for (const auto& t : rows) {
        html << "<tr><td>" << htmlEscape(t["date"].asString()) << "</td><td>"
             << t["hour"].asInt() << "</td><td>" << htmlEscape(t["provider"].asString())
             << "</td><td>" << t["requests"].asInt64() << "</td><td>"
             << htmlEscape(t["input_per_million_rub"].asString()) << " / "
             << htmlEscape(t["cache_read_per_million_rub"].asString()) << " / "
             << htmlEscape(t["output_per_million_rub"].asString()) << "</td><td>"
             << percent(t["cache_hit_ratio"].asDouble()) << "%</td><td>"
             << static_cast<int>(t["avg_latency_ms"].asDouble()) << "</td><td>"
             << static_cast<int>(t["tokens_per_second"].asDouble()) << "</td><td>"
             << formatRubDisplay(t["total_cost_nano"].asInt64()) << " ₽</td></tr>";
    }
    html << "</table>";
}

void ProxyApp::appendAgentEndpoints(std::ostringstream& html) const {
    const auto views = agentEndpointViews(config_);
    html << "<h2>Точки подключения агентов</h2>";
    if (views.empty()) {
        html << "<p class=\"muted\">Потребители (consumers) не настроены.</p>";
        return;
    }
    html << "<table><tr><th>агент</th><th>URL</th><th>модели</th><th>scopes</th>"
            "<th>лимиты</th><th>сети</th><th>срок</th><th>бюджет/сутки</th></tr>";
    for (const auto& v : views) {
        html << "<tr><td>" << htmlEscape(v.id) << "</td><td><code>";
        for (std::size_t i = 0; i < v.urls.size(); ++i) {
            if (i > 0) html << "<br>";
            html << "<span class=\"muted\">"
                 << (startsWith(v.urls[i], "https://") ? "сеть: " : "локально: ")
                 << "</span>" << htmlEscape(v.urls[i]);
        }
        html << "</code></td><td>" << htmlEscape(v.models) << "</td><td>"
             << htmlEscape(v.scopes) << "</td><td>" << htmlEscape(v.limits) << "</td><td>"
             << htmlEscape(v.networks) << "</td><td>" << htmlEscape(v.expires) << "</td><td>"
             << htmlEscape(v.budget) << "</td></tr>";
    }
    html << "</table>";
}

void ProxyApp::appendProviderHealth(std::ostringstream& html) const {
    const auto statuses = proxy_.health().snapshot();
    std::vector<const ProviderHealthSnapshot*> down;
    for (const auto& status : statuses) {
        if (status.down) down.push_back(&status);
    }
    if (down.empty()) return;
    const std::int64_t now = nowEpoch();
    html << "<div class=\"alert\" id=\"provider-down\"><b>Провайдер не работает</b>";
    for (const auto* status : down) {
        html << "<div>" << htmlEscape(status->provider) << " — с "
             << (status->down_since > 0 ? timeWithAge(status->down_since, now) : std::string("—"))
             << ", отказов подряд " << status->fail_streak;
        if (status->last_status >= 400) {
            html << ", последний ответ " << status->last_status;
        }
        if (!status->last_error.empty()) {
            html << ", причина: " << htmlEscape(providerReason(status->last_error));
        }
        html << "</div>";
    }
    html << "<div class=\"muted\">Живые запросы идут к следующему провайдеру очереди. "
            "Доступность проверяется каждые "
         << config_.routing.health.probe_interval_sec << " с; после "
         << config_.routing.health.recover_successes
         << " успешных проверок подряд провайдер возвращается в маршрут.</div></div>";
}

void ProxyApp::appendRouteSummary(std::ostringstream& html) const {
    const auto snapshot = queue_.snapshot();
    const std::string base = panelBase();
    const std::int64_t now = nowEpoch();
    const auto down = proxy_.downProviders();
    appendProviderHealth(html);
    html << "<h2 id=\"routes\">Текущий и предыдущий маршрут</h2>";
    if (!snapshot) {
        html << "<p class=\"muted\">Очередь ещё не собрана.</p>";
        return;
    }
    html << "<table><tr><th>модель</th><th>сейчас</th><th>переключён</th>"
            "<th>было</th></tr>";
    for (const auto& entry : snapshot->models) {
        html << "<tr><td>" << htmlEscape(entry.model) << "</td><td>";
        if (entry.leader.empty()) {
            html << "<span class=\"muted\">—</span>";
        } else {
            html << "<b>" << htmlEscape(entry.leader) << "</b>";
            if (down.count(entry.leader) > 0) {
                html << " <span class=\"err\">не работает</span>";
            }
        }
        html << "</td><td>"
             << (entry.leader_since > 0 ? timeWithAge(entry.leader_since, now) : std::string("—"))
             << "</td><td>"
             << (entry.previous_leader.empty() ? std::string("—")
                                               : htmlEscape(entry.previous_leader))
             << "</td></tr>";
    }
    html << "</table><div class=\"links\"><a class=\"btn\" href=\""
         << htmlEscape(base + "/routing")
         << "\">Настройки и алгоритм маршрутизации →</a></div>";
}

void ProxyApp::appendRoutingOptions(std::ostringstream& html, const Session& session) const {
    const Json::Value v = routingOptionsJson();
    const bool enabled = v["inject_provider_options"].asBool();
    const std::string base = panelBase();
    const bool admin = session.role == "admin";

    auto row = [&](const std::string& name, const std::string& value) {
        html << "<tr><td>" << htmlEscape(name) << "</td><td>" << htmlEscape(value) << "</td></tr>";
    };
    auto num = [](double value) {
        std::ostringstream out;
        out << std::setprecision(4) << value;
        return out.str();
    };

    html << "<h2 id=\"algorithm\">Алгоритм перестройки очереди</h2>"
            "<p class=\"muted\">weight RUB/M — цена 1M токенов профиля: вход и запись в кеш "
            "плюс кеш с весом <code>routing.cache_weight</code>, выход с весом "
            "<code>routing.output_weight</code>, фиксированная плата оффера и цена "
            "наблюдаемой задержки по <code>routing.wait_price_rub_per_sec</code>. Профиль — "
            "наблюдаемые средние токены модели: последние N запросов или средняя по "
            "журналу. Единственный порядок провайдеров публикуется снимком: панель, "
            "explain и живой запрос читают его, запрос маршрут не пересчитывает. "
            "Перестроение — сразу после обновления прайса и по случайному тику; статистика "
            "перестраивает не чаще заданного интервала. Лидер удерживается: challenger "
            "становится им при выигрыше ≥ маржи, подтверждении нескольких перестроений и "
            "простое. Привязка сессии держит провайдера только пока сброс кеша дороже "
            "выигрыша: выше порога сессия перепривязывается сразу. Смена провайдера с "
            "потерей кеша пишется в журнал как инцидент.</p>";

    html << "<h2 id=\"settings\">Настройки</h2>";
    if (admin) {
        html << "<div class=\"actions\"><button class=\"btn\" id=\"routing-toggle\" type=\"button\" "
                "data-enabled=\""
             << (enabled ? "1" : "0") << "\">"
             << (enabled ? "Выключить опции маршрутизации" : "Включить опции маршрутизации")
             << "</button><span id=\"routing-status\" class=\"muted\">"
             << (enabled ? "провайдеру передаётся provider из выбора ниже"
                         : "provider не передаётся: провайдер выбирает endpoint сам")
             << "</span></div>";
    } else {
        html << "<p class=\"muted\">"
             << (enabled ? "Опции маршрутизации передаются провайдеру."
                         : "Опции маршрутизации выключены: провайдер выбирает endpoint сам.")
             << "</p>";
    }

    html << "<table><tr><th>параметр</th><th>значение</th></tr>";
    row("контроль провайдера",
        "provider.order из " + std::to_string(v["provider_order_size"].asInt()) +
            " — смену провайдера выбирает только прокси");
    row("привязка сессии",
        v["session_pin_sec"].asInt() > 0
            ? "провайдер удерживается " + std::to_string(v["session_pin_sec"].asInt()) +
                  " с бездействия, пока выигрыш другого оффера ниже маржи"
            : "выключена");
    row("цена ожидания",
        num(v["wait_price_rub_per_sec"].asDouble()) +
            " RUB/с: входит в вес RUB/M и решает, сколько ждать провайдера до fallback");
    row("ожидание до fallback",
        v["failover_wait_max_sec"].asInt() > 0
            ? "до " + std::to_string(v["failover_wait_max_sec"].asInt()) + " с, если следующий оффер дороже"
            : "выключено: переход сразу после попыток повтора");
    row("гистерезис лидера",
        "маржа " + num(v["leader_margin_percent"].asDouble()) + "%, удержание " +
            std::to_string(v["leader_min_dwell_sec"].asInt()) + " с");
    row("профиль весов",
        v["profile_sample_size"].asInt() > 0
            ? "последние " + std::to_string(v["profile_sample_size"].asInt()) +
                  " успешных запросов модели"
            : "средняя по журналу запросов (переживает перезапуск)");
    row("инцидент потери кеша",
        "дороже в " + num(v["cache_loss_spike_ratio"].asDouble()) + " раз в пределах " +
            std::to_string(v["cache_loss_spike_window_sec"].asInt()) + " с");
    row("вес цены выхода",
        num(v["output_weight"].asDouble()) +
            " (1 = как в формуле стоимости, больше — сильнее влияет на порядок)");
    row("вес цены кеша",
        num(v["cache_weight"].asDouble()) +
            " (1 = как в формуле стоимости, больше — сильнее влияет на порядок)");
    row("очередь собрана",
        v["queue_built_at"].asInt64() > 0
            ? timeWithAge(v["queue_built_at"].asInt64(), nowEpoch())
            : "ещё не собрана");
    row("allow_fallbacks", v["allow_fallbacks"].asBool() ? "true" : "false");
    row("country", v["country"].asString().empty() ? "не задан" : v["country"].asString());
    std::vector<std::string> quant;
    for (const auto& q : v["quantization"]) quant.push_back(q.asString());
    row("quantization", quant.empty() ? "не задана" : join(quant, ", "));
    row("provider от клиента",
        v["client_provider_policy"].asString() == "reject" ? "reject (400)" : "strip");
    row("повтор запроса у провайдера",
        v["retry_attempts"].asInt() > 0
            ? std::to_string(v["retry_attempts"].asInt()) + " повторов, пауза " +
                  std::to_string(v["retry_backoff_ms"].asInt()) + " мс (тело запроса в памяти)"
            : "выключен");
    row("неработающий провайдер",
        "уходит из маршрута после " + std::to_string(v["health_fail_threshold"].asInt()) +
            " отказов подряд или " + std::to_string(v["health_down_after_sec"].asInt()) +
            " с непрерывных отказов; проверка каждые " +
            std::to_string(v["health_probe_interval_sec"].asInt()) + " с, возврат после " +
            std::to_string(v["health_recover_successes"].asInt()) + " успехов");
    row("попыток fallback", std::to_string(v["fallback_attempts"].asInt()));
    row("лимит кандидатов", std::to_string(v["candidate_cap"].asInt()));
    html << "</table>";

    html << "<table><tr><th>модель</th><th>профиль запросов</th><th>очередь провайдеров</th>"
            "<th>в запросе (order)</th><th>добавляется в запрос</th>"
            "<th>кандидатов / отклонено</th></tr>";
    for (const auto& m : v["models"]) {
        std::vector<std::string> order;
        for (const auto& tag : m["order"]) order.push_back(tag.asString());
        const bool observed = m["profile"].asString() == "observed";
        html << "<tr><td>" << htmlEscape(m["model"].asString()) << "</td><td>";
        if (observed) {
            html << "вход " << m["input_tokens"].asInt64() << " · выход "
                 << m["output_tokens"].asInt64() << " · кеш " << m["cached_tokens"].asInt64()
                 << " токенов<div class=\"muted\">наблюдений: " << m["observations"].asInt64()
                 << "</div>";
        } else {
            html << "нет статистики<div class=\"muted\">оценка " << kNominalInputTokens << " / "
                 << kNominalOutputTokens << " токенов</div>";
        }
        std::vector<std::string> in_request;
        for (const auto& tag : m["in_request"]) in_request.push_back(tag.asString());
        html << "</td><td>" << (order.empty() ? "—" : htmlEscape(join(order, " → ")))
             << "</td><td>" << (in_request.empty() ? "—" : htmlEscape(join(in_request, ", ")))
             << "</td><td><code>"
             << (m["inject"].isNull() ? "не передаётся"
                                      : escapeJsonForHtml(serializeJson(m["inject"])))
             << "</code></td><td>" << m["candidates"].asUInt64() << " / "
             << m["rejected"].asUInt64() << "</td></tr>";
    }
    html << "</table>";

    if (admin) {
        auto int_input = [&](const char* id, const char* label, int value, int min, int max) {
            html << "<label>" << label << "<input type=\"number\" id=\"" << id << "\" min=\""
                 << min << "\" max=\"" << max << "\" step=\"1\" value=\"" << value
                 << "\"></label>";
        };
        auto num_input = [&](const char* id, const char* label, const std::string& value) {
            html << "<label>" << label << "<input type=\"number\" id=\"" << id
                 << "\" min=\"" << routing_bounds::kWeightMin << "\" max=\""
                 << routing_bounds::kWeightMax << "\" step=\"0.1\" value=\"" << value
                 << "\"></label>";
        };
        html << "<h3>Правка настроек</h3>"
                "<p class=\"muted\">Изменения применяются к рабочему алгоритму только "
                "после сохранения: пишутся в <code>config/costrelay.yaml</code> и сразу "
                "перестраивают очередь. Ниже — предпросмотр на выбранной выборке.</p>"
                "<div class=\"filters\" id=\"routing-settings\">";
        num_input("set-output-weight", "вес цены выхода", num(v["output_weight"].asDouble()));
        num_input("set-cache-weight", "вес цены кеша", num(v["cache_weight"].asDouble()));
        int_input("set-profile-sample", "профиль: последних запросов", v["profile_sample_size"].asInt(),
                  routing_bounds::kProfileSampleMin, routing_bounds::kProfileSampleMax);
        int_input("set-fallback-attempts", "попыток fallback", v["fallback_attempts"].asInt(),
                  routing_bounds::kFallbackAttemptsMin, routing_bounds::kFallbackAttemptsMax);
        int_input("set-provider-order", "провайдеров в provider.order",
                  v["provider_order_size"].asInt(), routing_bounds::kProviderOrderMin,
                  routing_bounds::kProviderOrderMax);
        int_input("set-candidate-cap", "лимит кандидатов", v["candidate_cap"].asInt(),
                  routing_bounds::kCandidateCapMin, routing_bounds::kCandidateCapMax);
        int_input("set-pin-sec", "привязка сессии, с", v["session_pin_sec"].asInt(),
                  routing_bounds::kSessionPinSecMin, routing_bounds::kSessionPinSecMax);
        int_input("set-pin-max", "предел таблицы привязок", v["session_pin_max"].asInt(),
                  routing_bounds::kSessionPinMaxMin, routing_bounds::kSessionPinMaxMax);
        int_input("set-queue-min", "тик очереди: минимум, с", v["queue_min_sec"].asInt(),
                  routing_bounds::kQueueSecMin, routing_bounds::kQueueSecMax);
        int_input("set-queue-max", "тик очереди: максимум, с", v["queue_max_sec"].asInt(),
                  routing_bounds::kQueueSecMin, routing_bounds::kQueueSecMax);
        int_input("set-queue-stats", "статистика перестраивает не чаще, с",
                  v["queue_stats_sec"].asInt(), routing_bounds::kQueueSecMin,
                  routing_bounds::kQueueSecMax);
        html << "<label>цена секунды ожидания, RUB/с<input type=\"number\" "
                "id=\"set-wait-price\" min=\""
             << routing_bounds::kWaitPriceMin << "\" max=\"" << routing_bounds::kWaitPriceMax
             << "\" step=\"0.0001\" value=\""
             << num(v["wait_price_rub_per_sec"].asDouble()) << "\"></label>";
        int_input("set-failover-wait", "ожидание провайдера до fallback, с",
                  v["failover_wait_max_sec"].asInt(), 0, routing_bounds::kFailoverWaitMaxSec);
        int_input("set-health-threshold", "отказов подряд до ухода из маршрута",
                  v["health_fail_threshold"].asInt(), 0, routing_bounds::kHealthFailThresholdMax);
        int_input("set-health-down", "непрерывных отказов до ухода, с",
                  v["health_down_after_sec"].asInt(), 0, routing_bounds::kHealthDownAfterSecMax);
        int_input("set-health-probe", "проверка доступности, с",
                  v["health_probe_interval_sec"].asInt(), routing_bounds::kHealthProbeSecMin,
                  routing_bounds::kHealthProbeSecMax);
        int_input("set-health-recover", "успешных проверок для возврата",
                  v["health_recover_successes"].asInt(), routing_bounds::kHealthRecoverMin,
                  routing_bounds::kHealthRecoverMax);
        html << "</div><div class=\"actions\"><button class=\"btn\" id=\"routing-save\" "
                "type=\"button\">Сохранить настройки</button>"
                "<span id=\"routing-save-status\" class=\"muted\"></span></div>";
    }

    if (admin) {
        html << "<h3>Сброс статистики кеша</h3>"
                "<p class=\"muted\">Попадания в кеш до сброса перестают учитываться в ценах "
                "маршрутизации; новые запросы считаются заново.</p>"
                "<form id=\"cache-reset\" class=\"actions\">"
                "<select id=\"cache-reset-scope\">"
                "<option value=\"" << kCacheScopeModel << "\">модель</option>"
                "<option value=\"" << kCacheScopeProvider << "\">провайдер</option>"
                "<option value=\"" << kCacheScopeSession << "\">сессия</option>"
                "</select>"
                "<input id=\"cache-reset-value\" list=\"cache-reset-values\" placeholder=\"значение\">"
                "<datalist id=\"cache-reset-values\">";
        std::set<std::string> values = providerTags();
        for (const auto& model : config_.routing.models) values.insert(model);
        for (const auto& item : stats_.sessionsJson(panel_limits::kListDefault)) {
            const std::string id = item["session_id"].asString();
            if (!id.empty()) values.insert(id);
        }
        for (const auto& value : values) {
            html << "<option value=\"" << htmlEscape(value) << "\">";
        }
        html << "</datalist>"
                "<button class=\"btn\" type=\"submit\">Сбросить кеш</button>"
                "<span id=\"cache-reset-status\" class=\"muted\"></span></form>"
                "<script>(()=>{const f=document.getElementById('cache-reset');if(!f)return;"
                "const st=document.getElementById('cache-reset-status');"
                "f.addEventListener('submit',async(e)=>{e.preventDefault();"
                "const scope=document.getElementById('cache-reset-scope').value;"
                "const value=document.getElementById('cache-reset-value').value.trim();"
                "if(!value){st.textContent='укажите значение';return;}"
                "st.textContent='сброс…';try{"
                "await panelPost('";
        html << htmlEscape(base + "/api/cache-reset")
             << "',{scope:scope,value:value});st.textContent='сброшено: '+scope+' '+value"
                "}catch(err){st.textContent='ошибка: '+err.message}})})();</script>";
    }

    // Предпросмотр — последний блок страницы: он пересчитывается на выбранной
    // выборке и ещё не сохранённых значениях, рабочий маршрут не меняет.
    html << "<h3 id=\"preview\">Предпросмотр весов на последних запросах</h3>"
            "<p class=\"muted\">Тот же расчёт, что и в рабочей очереди, но на выбранной "
            "выборке. Значения полей настроек выше ещё не сохранены: рабочий маршрут не "
            "меняется. Карточки — средний запрос: токены и вклад компонента в weight "
            "(цена с поправочными весами) по сохранённым значениям и по значениям полей "
            "выше; сумма вкладов и задержка дают weight RUB/M. Ниже — очередь по новым "
            "значениям.</p>"
            "<div class=\"filters\"><label>модель<select id=\"preview-model\">";
    for (const auto& model : config_.routing.models) {
        html << "<option value=\"" << htmlEscape(model) << "\">" << htmlEscape(model)
             << "</option>";
    }
    html << "</select></label><label>последних запросов (0 — средняя по журналу)"
            "<input type=\"number\" id=\"preview-sample\" min=\""
         << routing_bounds::kProfileSampleMin << "\" max=\"" << routing_bounds::kProfileSampleMax
         << "\" step=\"1\" value=\"" << v["profile_sample_size"].asInt()
         << "\"></label><span id=\"preview-status\" class=\"muted\"></span></div>"
            "<div id=\"preview-cards\" class=\"cards\"></div>"
            "<div class=\"table-wrap\"><table id=\"preview-table\"><thead><tr>"
            "<th>провайдер</th><th>weight RUB/M</th><th>lat ms</th>"
            "<th>стоимость запроса, ₽</th><th>cache hit</th><th>кеш RUB/M</th>"
            "<th>статус</th></tr></thead><tbody></tbody></table></div>"
            "<script>(()=>{const base='";
    html << htmlEscape(base)
         << "';const modelSel=document.getElementById('preview-model');"
            "const sample=document.getElementById('preview-sample');"
            "const table=document.getElementById('preview-table');"
            "const cardsBox=document.getElementById('preview-cards');"
            "const status=document.getElementById('preview-status');"
            "const ow=document.getElementById('set-output-weight');"
            "const cw=document.getElementById('set-cache-weight');"
            "const wp=document.getElementById('set-wait-price');"
            "const fmt=n=>Number(n).toLocaleString('ru-RU');"
            "const pct=r=>Math.round(Number(r)*100)+'%';"
            "const cell=(tr,text,cls)=>{const td=document.createElement('td');"
            "if(cls)td.className=cls;td.textContent=text;tr.appendChild(td);return td;};"
            "const costCard=(label,value,note,rows)=>{"
            "const d=document.createElement('div');d.className='card';"
            "const l=document.createElement('div');l.className='muted';l.textContent=label;d.appendChild(l);"
            "const v=document.createElement('div');v.className='num';v.textContent=value;d.appendChild(v);"
            "if(note){const n=document.createElement('div');n.className='muted';n.textContent=note;d.appendChild(n);}"
            "for(const [k,t] of rows){const r=document.createElement('div');r.className='kv';"
            "const kk=document.createElement('span');kk.className='muted';kk.textContent=k;"
            "const vv=document.createElement('b');vv.textContent=t;r.append(kk,vv);d.appendChild(r);}"
            "return d;};"
            "async function refresh(){if(!modelSel)return;"
            "const p=new URLSearchParams({model:modelSel.value,sample:sample.value||'0'});"
            "if(ow)p.set('output_weight',ow.value);if(cw)p.set('cache_weight',cw.value);"
            "if(wp)p.set('wait_price_rub_per_sec',wp.value);"
            "status.textContent='счёт…';"
            "try{const r=await fetch(base+'/api/routing-preview?'+p.toString(),"
            "{headers:{'Accept':'application/json'}});"
            "const j=await r.json();if(!r.ok)throw new Error((j.error&&j.error.message)||('HTTP '+r.status));"
            "while(table.tBodies[0].rows.length)table.tBodies[0].deleteRow(0);"
            "for(const row of j.rows){const tr=table.tBodies[0].insertRow(-1);"
            "cell(tr,row.provider,row.provider===j.leader?'':'muted');"
            "cell(tr,row.weight_rub);cell(tr,row.latency_ms>0?row.latency_ms:'—');"
            "cell(tr,row.cost_rub+' ₽');"
            "cell(tr,pct(row.cache_hit_ratio));cell(tr,row.cache_read_rub);"
            "cell(tr,row.excluded?row.excluded:'допущен',row.excluded?'warn':'');}"
            "const cur=j.current||{},nw=j.preview||{};"
            "const billed=Math.max(1,(j.input_tokens||0)+(j.output_tokens||0));"
            "const curName='сейчас'+(cur.provider?' · '+cur.provider:'');"
            "const newName='с новыми'+(nw.provider?' · '+nw.provider:'');"
            "const curMoney=k=>cur.provider?((cur[k]||'')+' ₽'):'—';"
            "const newMoney=k=>nw.provider?((nw[k]||'')+' ₽'):'—';"
            "const src=j.profile_source||'none';"
            "const sampleText=src==='window'?('окно: '+fmt(j.observations)+' запросов')"
            ":src==='journal'?((j.sample>0?'окно пусто — средняя по журналу: ':'средняя по журналу: ')+fmt(j.observations)+' запросов')"
            ":src==='memory'?('средняя за время процесса: '+fmt(j.observations)+' запросов')"
            ":('оценка " +
                std::to_string(kNominalInputTokens) + "/" +
                std::to_string(kNominalOutputTokens) + " токенов');"
            "const leaderNote=(nw.provider&&nw.provider!==j.leader?(' · новый лидер: '+nw.provider):'');"
            "cardsBox.replaceChildren("
            "costCard('Выборка',src==='none'?'оценка':(fmt(j.observations)+' запросов'),"
            "sampleText+' · cache hit '+pct(j.cache_hit_ratio)+' · лидер: '+(j.leader||'—')+leaderNote,[]),"
            "costCard('Вход',fmt(j.input_tokens),Math.round((j.input_tokens||0)/billed*100)+'% входа+выхода',"
            "[[curName,curMoney('input_rub')],[newName,newMoney('input_rub')]]),"
            "costCard('Кеш',fmt(j.cached_tokens),"
            "(j.input_tokens?Math.round((j.cached_tokens||0)/j.input_tokens*100):0)+'% входа',"
            "[[curName,curMoney('cache_rub')],[newName,newMoney('cache_rub')]]),"
            "costCard('Выход',fmt(j.output_tokens),Math.round((j.output_tokens||0)/billed*100)+'% входа+выхода',"
            "[[curName,curMoney('output_rub')],[newName,newMoney('output_rub')]])"
            ");"
            "status.textContent='';}catch(e){status.textContent='ошибка: '+e.message}}"
            "if(modelSel){modelSel.addEventListener('change',refresh);"
            "sample.addEventListener('change',refresh);"
            "if(ow)ow.addEventListener('change',refresh);if(cw)cw.addEventListener('change',refresh);"
            "if(wp)wp.addEventListener('change',refresh);"
            "refresh();}})();</script>";

    if (admin) {
        html << "<script>(()=>{const btn=document.getElementById('routing-save');"
                "if(!btn)return;const status=document.getElementById('routing-save-status');"
                "const val=id=>{const el=document.getElementById(id);"
                "return el?el.value.trim():''};"
                "const ival=id=>{const t=val(id);if(t==='')return null;const n=Number(t);"
                "return Number.isFinite(n)?Math.round(n):NaN;};"
                "const fval=id=>{const t=val(id);if(t==='')return null;const n=Number(t);"
                "return Number.isFinite(n)?n:NaN;};"
                "btn.addEventListener('click',async()=>{"
                "const body={};const si=[['profile_sample_size','set-profile-sample'],"
                "['fallback_attempts','set-fallback-attempts'],['provider_order_size','set-provider-order'],"
                "['candidate_cap','set-candidate-cap'],['session_pin_sec','set-pin-sec'],"
                "['session_pin_max','set-pin-max'],['failover_wait_max_sec','set-failover-wait'],"
                "['queue_min_sec','set-queue-min'],"
                "['queue_max_sec','set-queue-max'],['queue_stats_sec','set-queue-stats'],"
                "['health_fail_threshold','set-health-threshold'],"
                "['health_down_after_sec','set-health-down'],"
                "['health_probe_interval_sec','set-health-probe'],"
                "['health_recover_successes','set-health-recover']];"
                "const sf=[['output_weight','set-output-weight'],['cache_weight','set-cache-weight'],"
                "['wait_price_rub_per_sec','set-wait-price']];"
                "for(const [k,id] of si){const n=ival(id);if(n===null)continue;"
                "if(!Number.isFinite(n)){status.textContent='числовое значение: '+id;return}body[k]=n;}"
                "for(const [k,id] of sf){const n=fval(id);if(n===null)continue;"
                "if(!Number.isFinite(n)){status.textContent='числовое значение: '+id;return}body[k]=n;}"
                "status.textContent='сохранение…';try{await panelPost('";
        html << htmlEscape(base + "/api/routing-options")
             << "',body);status.textContent='сохранено; очередь перестроена';location.reload()}"
                "catch(e){status.textContent='ошибка: '+e.message}})})();</script>";
    }
    if (admin) {
        html << "<script>(()=>{const btn=document.getElementById('routing-toggle');"
                "if(!btn)return;const status=document.getElementById('routing-status');"
                "btn.addEventListener('click',async()=>{"
                "const next=btn.dataset.enabled!=='1';btn.disabled=true;"
                "status.textContent='сохранение…';try{"
                "await panelPost('";
        html << htmlEscape(base + "/api/routing-options")
             << "',{inject_provider_options:next});location.reload()"
                "}catch(e){status.textContent='ошибка: '+e.message;"
                "btn.disabled=false}})})();</script>";
    }

}

drogon::HttpResponsePtr ProxyApp::routingPage(const Session& session) const {
    std::ostringstream html;
    appendPageStart(html, "Маршрутизация", session);
    appendRouteSummary(html);
    appendRoutingOptions(html, session);
    appendPageEnd(html);
    return pageResponse(html.str());
}

void ProxyApp::logAgentEndpoints() const {
    const auto views = agentEndpointViews(config_);
    if (views.empty()) {
        LOG_INFO << "agent endpoints: consumers are not configured";
        return;
    }
    for (const auto& v : views) {
        std::vector<std::string> urls;
        for (const auto& url : v.urls) {
            urls.push_back(std::string(startsWith(url, "https://") ? "network " : "local ") + url);
        }
        LOG_INFO << "agent endpoint " << v.id << ": " << join(urls, " | ")
                 << " | models=" << v.models << " | scopes=" << v.scopes
                 << " | limits=" << v.limits << " | networks=" << v.networks
                 << " | expires=" << v.expires << " | budget/day=" << v.budget;
    }
}

drogon::HttpResponsePtr ProxyApp::dashboardPage(const Session& session) const {
    const auto ov = overviewJson();
    const std::string base = panelBase();
    std::ostringstream html;
    appendPageStart(html, "CostRelay", session);

    auto card = [&](const std::string& label, const std::string& value,
                    const std::string& note = {}, const std::string& value_class = {},
                    const std::string& href = {}) {
        appendCard(html, label, value, note, value_class, href);
    };
    html << "<div class=\"cards\">";
    const auto& today = ov["today"];
    const auto& previous = ov["previous"];
    const bool has_today = today.isObject() && today.isMember("requests");
    const std::string today_day =
        has_today ? today["day"].asString()
                  : utcDayString(std::chrono::system_clock::now());
    const std::string previous_day =
        previous.isObject() && previous.isMember("day") ? previous["day"].asString()
                                                        : std::string();
    const auto requests_today = has_today ? today["requests"].asInt64() : 0;
    const auto errors_today = has_today ? today["errors"].asInt64() : 0;
    const auto warnings_today = has_today ? today["warnings"].asInt64() : 0;
    const auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_at_).count();
    card("Время работы", formatUptime(uptime_seconds));
    card("Запросов сегодня", has_today ? std::to_string(requests_today) : "—",
         has_today ? today_day + " UTC" : "нет суточной статистики");
    // Ошибки и предупреждения — из тех же суточных агрегатов, что и журнал:
    // фильтр в переходе показывает ровно эти записи.
    const std::string logs_link = base + "/stats?day=" + urlEncode(today_day) + "&issue=";
    card("Ошибки сегодня", has_today ? std::to_string(errors_today) : "—",
         has_today ? (errors_today > 0 ? "severity ≥ 2 · " + today_day
                                       : "нет ошибок; отказы с повтором — в предупреждениях")
                   : "нет суточной статистики",
         errors_today > 0 ? "err" : "", errors_today > 0 ? logs_link + "error" : "");
    card("Предупреждения сегодня", has_today ? std::to_string(warnings_today) : "—",
         has_today ? (warnings_today > 0 ? "severity 1 · " + today_day
                                         : "нет предупреждений за сутки")
                   : "нет суточной статистики",
         warnings_today > 0 ? "warn" : "",
         warnings_today > 0 ? logs_link + "warning" : "");
    card("Cache hit сегодня", has_today ? std::to_string(percent(today["cache_hit_ratio"].asDouble())) + "%" : "—",
         has_today ? "доля кешированного входа за сутки" : "нет суточной статистики");

    // Стоимость и её распределение — за текущие календарные сутки (UTC, как бюджеты).
    const auto day_cost_input = has_today ? today["cost_input_nano"].asInt64() : 0;
    const auto day_cost_cache = has_today ? today["cost_cache_read_nano"].asInt64() : 0;
    const auto day_cost_output = has_today ? today["cost_output_nano"].asInt64() : 0;
    const auto day_cost_parts = day_cost_input + day_cost_cache + day_cost_output;
    // Стоимость — только факт по отчёту провайдера. Остаток сверх частей —
    // фиксированная плата роутера, которая не относится к токенам.
    const auto day_cost_total = has_today ? today["cost_nano"].asInt64() : 0;
    const auto day_cost_estimated = has_today ? today["cost_estimated_nano"].asInt64() : 0;
    const auto day_estimated_requests = has_today ? today["estimated_requests"].asInt64() : 0;
    if (!has_today) {
        card("Стоимость, сегодня", "—", "SQLite-статистика отключена");
    } else {
        card("Стоимость, сегодня", formatRubDisplay(day_cost_total) + " ₽",
             today_day + " UTC · факт по отчёту провайдера");
        const auto cost_card = [&](const std::string& label, NanoRub part) {
            card(label, formatRubDisplay(part) + " ₽",
                 day_cost_parts > 0
                     ? std::to_string(percent(ratioOf(part, day_cost_parts))) + "% распределения"
                     : "нет фактов за сутки");
        };
        cost_card("Стоимость: вход, ₽", day_cost_input);
        cost_card("Стоимость: кеш, ₽", day_cost_cache);
        cost_card("Стоимость: выход, ₽", day_cost_output);
        const auto fixed = std::max<NanoRub>(0, day_cost_total - day_cost_parts);
        if (fixed > 0) {
            card("Фиксированная плата, ₽", formatRubDisplay(fixed) + " ₽",
                 "не относится к токенам");
        }
        if (day_cost_estimated > 0 || day_estimated_requests > 0) {
            card("Оценка без отчёта, ₽", formatRubDisplay(day_cost_estimated) + " ₽",
                 std::to_string(day_estimated_requests) + " запр. · в итог не входит");
        }
    }
    const auto& previous_cost = previous.isObject() ? previous["cost_nano"] : Json::Value();
    card("Стоимость, прошлые сутки",
         previous_cost.isInt64() ? formatRubDisplay(previous_cost.asInt64()) + " ₽" : "—",
         previous_day.empty() ? "нет данных за прошлые сутки"
                              : previous_day + " UTC · полные сутки");
    card("Стоимость, всего", formatRubDisplay(ov["cost_nano_total"].asInt64()) + " ₽",
         has_today ? "суточные агрегаты за " +
                         std::to_string(config_.stats.aggregates_retention_days) +
                         " дн., без оценок"
                   : "память процесса, без оценок");
    html << "</div>";
    html << "<div class=\"links\"><a class=\"btn\" href=\"" << htmlEscape(base + "/stats")
         << "\">Логи →</a> <a class=\"btn\" href=\"" << htmlEscape(base + "/stats?day=" + today_day)
         << "\">Записи за сегодня →</a></div>";

    appendRouteSummary(html);
    html << "<div class=\"links\"><a class=\"btn\" href=\""
         << htmlEscape(base + "/providers")
         << "\">Провайдеры и цены: сравнение →</a> <a class=\"btn\" href=\""
         << htmlEscape(base + "/providers#blacklist")
         << "\">Чёрный список →</a> <a class=\"btn\" href=\""
         << htmlEscape(base + "/providers#provider-csv")
         << "\">Статистика провайдера (CSV) →</a></div>";

    appendAgentEndpoints(html);

    const Json::Value sessions = stats_.sessionsJson(kDashboardSessions);
    html << "<h2>Сессии <a class=\"btn btn-small\" href=\"" << htmlEscape(base + "/stats")
         << "\">Логи →</a></h2>";
    if (sessions.isArray() && !sessions.empty()) {
        appendSessionsTable(html, sessions, "");
    } else {
        html << "<p class=\"muted\">Запросов пока нет.</p>";
    }

    if (stats_db_ && stats_db_->ok()) {
        const auto daily = stats_db_->daily(14);
        if (daily.isArray() && !daily.empty()) {
            std::int64_t max_requests = 1;
            for (const auto& d : daily) {
                max_requests = std::max(max_requests, d["requests"].asInt64());
            }
            html << "<h2>Запросы по дням</h2><table><tr><th>день</th><th>запросы</th>"
                    "<th>cache hit</th><th>₽</th></tr>";
            for (const auto& d : daily) {
                const int width = barPercent(d["requests"].asInt64(), max_requests);
                html << "<tr><td>" << htmlEscape(d["day"].asString())
                     << "<div class=\"bar\"><span style=\"width:" << std::clamp(width, 1, 100)
                     << "%\"></span></div></td><td>" << d["requests"].asInt64() << "</td><td>"
                     << percent(d["cache_hit_ratio"].asDouble()) << "%</td><td>"
                     << formatRubDisplay(d["cost_nano"].asInt64()) << " ₽</td></tr>";
            }
            html << "</table>";
        }
    }

    html << "<h2>Маршрутизаторы</h2><table><tr><th>ID</th><th>kind</th><th>Base URL</th>"
            "<th>Модели</th><th>множитель</th></tr>";
    for (const auto& r : config_.routers) {
        if (!routerVisible(config_, r)) continue;
        html << "<tr><td>" << htmlEscape(r.id) << "</td><td>" << htmlEscape(r.kind)
             << "</td><td>" << htmlEscape(r.base_url) << "</td><td>" << r.models.size()
             << "</td><td>" << r.price_multiplier
             << "</td></tr>";
    }
    html << "</table>";

    appendPageEnd(html);
    return pageResponse(html.str());
}

drogon::HttpResponsePtr ProxyApp::statsPage(const Session& session,
                                           const drogon::HttpRequestPtr& req) const {
    const std::string session_filter = pageParam(req, "session_id");
    const std::string model_filter = pageParam(req, "model");
    const std::string provider_filter = pageParam(req, "provider");
    const std::string issue_param = pageParam(req, "issue");
    std::string day_filter = pageParam(req, "day");
    if (!utcDayValid(day_filter)) day_filter.clear();
    const IssueFilter issue = issueFilterFromString(issue_param);
    const int limit = static_cast<int>(
        queryLimit(req, panel_limits::kLogDefault, panel_limits::kLogMax)
            .value_or(panel_limits::kLogDefault));
    int page = 1;
    try {
        if (!req->getParameter("page").empty()) page = std::stoi(req->getParameter("page"));
    } catch (...) {
        page = 1;
    }
    const std::string base = panelBase();
    const bool persistent = stats_db_ && stats_db_->ok();

    const Json::Value sessions = stats_.sessionsJson(panel_limits::kSessionListMax);
    std::set<std::string> providers = providerTags();
    if (!provider_filter.empty()) providers.insert(provider_filter);

    const std::int64_t total = stats_.requestCount(session_filter, model_filter, provider_filter,
                                                   issue, day_filter);
    const int pages = std::max<int>(1, static_cast<int>((total + limit - 1) / limit));
    page = std::clamp(page, 1, pages);
    const int offset = (page - 1) * limit;

    const Json::Value requests = stats_.requestsJson(limit, session_filter, model_filter,
                                                     provider_filter, static_cast<std::size_t>(offset),
                                                     issue, day_filter);

    std::ostringstream html;
    appendPageStart(html, "Логи", session);
    appendRecentProviders(html);
    html << "<h2 id=\"requests\">Журнал запросов</h2>";
    html << "<div class=\"links\"><a class=\"btn btn-small\" href=\"/\">На главную</a>"
            "<a class=\"btn btn-small\" href=\""
         << htmlEscape(base + "/providers")
         << "\">Провайдеры и цены →</a></div>";

    using SelectOptions = std::vector<std::pair<std::string, std::string>>;
    SelectOptions session_options{{"", "все"}};
    for (const auto& item : sessions) {
        const auto sid = item["session_id"].asString();
        if (sid.empty()) continue;
        session_options.emplace_back(
            sid, sid + " · " + std::to_string(item["requests"].asInt64()) + " запр.");
    }
    SelectOptions model_options{{"", "все"}};
    for (const auto& model : config_.routing.models) model_options.emplace_back(model, model);
    SelectOptions provider_options{{"", "все"}};
    for (const auto& provider : providers) provider_options.emplace_back(provider, provider);
    SelectOptions issue_options{{"", "все"},
                                {"warning", "предупреждения"},
                                {"error", "ошибки"},
                                {"issue", "ошибки и предупреждения"}};
    SelectOptions limit_options;
    for (const int value : {static_cast<int>(panel_limits::kListDefault), kLogPageMid,
                            static_cast<int>(panel_limits::kLogDefault),
                            static_cast<int>(panel_limits::kLogMax)}) {
        limit_options.emplace_back(std::to_string(value), std::to_string(value));
    }

    html << "<form class=\"filters\" method=\"get\" action=\"" << htmlEscape(base + "/stats")
         << "\">";
    html << "<label>день (UTC)<input type=\"date\" name=\"day\" value=\""
         << htmlEscape(day_filter) << "\"></label>";
    appendSelect(html, "session_id", "сессия", session_options, session_filter);
    appendSelect(html, "model", "model", model_options, model_filter);
    appendSelect(html, "provider", "provider", provider_options, provider_filter);
    appendSelect(html, "issue", "важность", issue_options, issue_param);
    appendSelect(html, "limit", "запросов", limit_options, std::to_string(limit));
    html << "<button class=\"btn\" type=\"submit\">Показать</button>"
            "<a class=\"btn\" href=\""
         << htmlEscape(base + "/stats") << "\">Сбросить фильтры</a>";
    if (session.role == "admin" && persistent) {
        // Удаляет именно то, что видно по текущему фильтру; второй клик — подтверждение.
        html << "<button class=\"btn\" type=\"button\" id=\"log-delete\" data-total=\""
             << total << "\" data-day=\"" << htmlEscape(day_filter)
             << "\" data-session=\"" << htmlEscape(session_filter)
             << "\" data-model=\"" << htmlEscape(model_filter) << "\" data-provider=\""
             << htmlEscape(provider_filter) << "\" data-issue=\"" << htmlEscape(issue_param)
             << "\">Удалить отфильтрованные</button>"
                "<span id=\"log-delete-status\" class=\"muted\"></span>";
    }
    html << "</form>";
    if (!persistent) {
        html << "<p class=\"muted\">SQLite-статистика отключена: журнал ограничен "
                "кольцевым буфером в памяти, удаление записей недоступно.</p>";
    }

    appendLogTable(html, requests, base, session.role == "admin");
    if (session.role == "admin" && persistent) {
        html << "<script>(()=>{const b=document.getElementById('log-delete');if(!b)return;"
                "const st=document.getElementById('log-delete-status');let armed=false;"
                "const total=Number(b.dataset.total||0);"
                "const reset=()=>{armed=false;b.disabled=false;"
                "b.textContent='Удалить отфильтрованные'};"
                "b.addEventListener('click',async()=>{"
                "if(!armed){if(total===0){st.textContent='по фильтру нет записей';return;}"
                "armed=true;b.textContent='Подтвердить удаление';"
                "const scope=[];"
                "if(b.dataset.day)scope.push('день '+b.dataset.day);"
                "if(b.dataset.session)scope.push('сессия '+b.dataset.session);"
                "if(b.dataset.model)scope.push('модель '+b.dataset.model);"
                "if(b.dataset.provider)scope.push('провайдер '+b.dataset.provider);"
                "if(b.dataset.issue)scope.push('важность '+b.dataset.issue);"
                "st.textContent='будет удалено записей: '+total+' ('+(scope.length?scope.join(', '):'все записи')+') — нажмите ещё раз';"
                "return}b.disabled=true;st.textContent='удаление…';"
                "try{const j=await panelPost('"
             << htmlEscape(panelBase() + "/api/requests-delete")
             << "',{day:b.dataset.day,session_id:b.dataset.session,model:b.dataset.model,provider:b.dataset.provider,"
                "issue:b.dataset.issue,"
                "all:!(b.dataset.day||b.dataset.session||b.dataset.model||b.dataset.provider||b.dataset.issue)});"
                "st.textContent='удалено записей: '+j.deleted;"
                "setTimeout(()=>location.reload(),800)"
                "}catch(e){st.textContent='ошибка: '+e.message;reset()}})})();</script>";
    }
    appendPaginator(html, base, page, pages, total,
                    {{"day", day_filter},
                     {"session_id", session_filter},
                     {"model", model_filter},
                     {"provider", provider_filter},
                     {"issue", issue_param},
                     {"limit", std::to_string(limit)}});

    appendPageEnd(html);
    return pageResponse(html.str());
}

void ProxyApp::appendLogTable(std::ostringstream& html, const Json::Value& requests,
                              const std::string& base, bool can_view_bodies) const {
    if (!requests.isArray() || requests.empty()) {
        html << "<p class=\"muted\">Запросов по текущим фильтрам нет.</p>";
        return;
    }
    const std::int64_t now = nowEpoch();
    html << "<div class=\"table-wrap\"><table><tr><th>время</th><th>сессия</th>"
            "<th>model</th><th>router/provider</th><th>status</th><th>ms</th>"
            "<th>bytes in/out</th><th>in/out/cache tok</th><th>₽</th><th>body</th></tr>";
    for (const auto& x : requests) {
        std::string sid = x["session_id"].asString();
        if (sid.size() > kSessionIdDisplayChars) {
            sid = sid.substr(0, kSessionIdDisplayChars) + "…";
        }
        // Строка уже содержит долю, по которой считалась стоимость.
        const int ratio = percent(x["cache_hit_ratio"].asDouble());
        html << "<tr><td title=\"" << htmlEscape(x["request_id"].asString()) << "\">"
             << timeWithAge(x["ts"].asInt64(), now) << "</td><td>" << htmlEscape(sid)
             << "</td><td>" << htmlEscape(x["model"].asString())
             << "</td><td>" << htmlEscape(x["router"].asString()) << " / "
             << htmlEscape(x["provider"].asString()) << "</td><td>"
             << x["status"].asInt();
        const std::string error_code = x["error_code"].asString();
        if (!error_code.empty()) {
            const bool error = x["severity"].asInt() >= kIssueError;
            html << "<div class=\"" << (error ? "err" : "warn") << "\" title=\""
                 << htmlEscape(error_code) << "\">" << htmlEscape(issueText(error_code))
                 << "</div>";
        }
        const std::string event_detail = x["event_detail"].asString();
        if (!event_detail.empty()) {
            Json::Value detail;
            std::string detail_error;
            if (parseJson(event_detail, detail, detail_error) && detail.isObject()) {
                if (detail["retries"].isArray()) {
                    for (const auto& retry : detail["retries"]) {
                        html << "<div class=\"warn\" title=\"" << htmlEscape(event_detail)
                             << "\">отказ: " << htmlEscape(retry["provider"].asString())
                             << " " << retry["status"].asInt();
                        if (retry["status"].asInt() < 400) {
                            html << ", " << htmlEscape(providerReason(retry["error"].asString()));
                        }
                        html << " (попытка " << retry["attempt"].asInt() << ")</div>";
                    }
                } else {
                    std::ostringstream note;
                    note << std::setprecision(3) << detail["from"].asString() << " → "
                         << detail["to"].asString() << " ×" << detail["ratio"].asDouble();
                    html << "<div class=\"warn\" title=\"" << htmlEscape(event_detail)
                         << "\">потеря кеша: " << htmlEscape(note.str()) << " ("
                         << htmlEscape(detail["cause"].asString()) << ")</div>";
                }
            } else {
                // Разбор не удался — событие всё равно показывается сырым текстом,
                // а не исчезает из журнала.
                html << "<div class=\"warn\" title=\"" << htmlEscape(detail_error)
                     << "\">событие: " << htmlEscape(event_detail) << "</div>";
            }
        }
        html << "</td><td>" << x["latency_ms"].asInt() << "</td><td>"
             << x["bytes_in"].asInt64() << " / " << x["bytes_out"].asInt64()
             << "</td><td>" << x["tokens_in"].asInt64() << " / "
             << x["tokens_out"].asInt64() << " / " << x["cache_read_tokens"].asInt64();
        if (x["tokens_in"].asInt64() > 0) html << " (" << ratio << "%)";
        html << "</td><td>";
        if (x["cost_estimated"].asBool()) {
            // Оценка по профилю модели не выдаётся за счёт провайдера.
            html << "<span class=\"muted\" title=\"нет отчёта провайдера о токенах · оценка по"
                    " профилю модели: "
                 << htmlEscape(formatRubDisplay(x["cost_estimated_nano"].asInt64()))
                 << " ₽ (в итог не входит)\">—</span>";
        } else {
            html << formatRubDisplay(x["cost_nano"].asInt64()) << " ₽";
        }
        html << "</td><td>";
        if (can_view_bodies && x["captured"].asBool()) {
            const std::string id = urlEncode(x["request_id"].asString());
            const auto body_link = [&](const std::string& kind, const std::string& label) {
                html << "<a target=\"_blank\" rel=\"noopener\" href=\""
                     << htmlEscape(base + "/api/request-body?request_id=" + id + "&kind=" + kind)
                     << "\">" << label << "</a>";
            };
            body_link("request", "request");
            html << " · ";
            body_link("response", "response");
        } else {
            html << "<span class=\"muted\">—</span>";
        }
        html << "</td></tr>";
    }
    html << "</table></div>";
}

drogon::HttpResponsePtr ProxyApp::providersPage(const Session& session,
                                                const drogon::HttpRequestPtr& req) const {
    const std::string model_filter = pageParam(req, "model");
    const std::string provider_filter = pageParam(req, "provider");
    const bool hide_blocked = !pageParam(req, "hide_blocked").empty();
    const std::string base = panelBase();

    std::set<std::string> providers = providerTags();
    if (!provider_filter.empty()) providers.insert(provider_filter);

    std::ostringstream html;
    appendPageStart(html, "Провайдеры и цены", session);
    html << "<div class=\"links\"><a class=\"btn btn-small\" href=\"/\">На главную</a>"
            "<a class=\"btn btn-small\" href=\""
         << htmlEscape(base + "/stats") << "\">Логи →</a></div>";

    std::vector<std::pair<std::string, std::string>> model_options{{"", "все"}};
    for (const auto& model : config_.routing.models) model_options.emplace_back(model, model);
    std::vector<std::pair<std::string, std::string>> provider_options{{"", "все"}};
    for (const auto& provider : providers) provider_options.emplace_back(provider, provider);

    html << "<form class=\"filters\" method=\"get\" action=\""
         << htmlEscape(base + "/providers") << "\">";
    appendSelect(html, "model", "model", model_options, model_filter);
    appendSelect(html, "provider", "provider", provider_options, provider_filter);
    html << "<label class=\"check\"><input type=\"checkbox\" name=\"hide_blocked\" value=\"1\""
         << (hide_blocked ? " checked" : "") << "> скрыть чёрный список</label>"
         << "<button class=\"btn\" type=\"submit\">Показать</button>"
            "<a class=\"btn\" href=\""
         << htmlEscape(base + "/providers") << "\">Сбросить</a></form>";

    appendPriceReport(html, model_filter, provider_filter, hide_blocked);
    appendProviderAnalytics(html, session);
    appendBlacklistManager(html, session.role == "admin");
    appendPageEnd(html);
    return pageResponse(html.str());
}


} // namespace proxy
