#include "limits/QuotaManager.h"
#include "stats/StatsDb.h"

#include <algorithm>
#include <ctime>
#include <limits>

namespace proxy {
namespace {
constexpr auto kWindow = std::chrono::seconds(60);
}

void QuotaManager::prune(State& state, std::chrono::system_clock::time_point now) {
    while (!state.requests.empty() && now - state.requests.front().at > kWindow) {
        state.requests.pop_front();
    }
    while (!state.tokens.empty() && now - state.tokens.front().at > kWindow) {
        state.tokens.pop_front();
    }
    // Расход храним за несколько суток: ключ — дата UTC (YYYY-MM-DD), поэтому
    // лексикографически первый ключ и есть самый старый день.
    constexpr std::size_t kSpentDaysKept = 8;
    while (state.spent.size() > kSpentDaysKept) state.spent.erase(state.spent.begin());
}

std::int64_t QuotaManager::windowTokens(const State& state) {
    std::int64_t sum = 0;
    for (const auto& e : state.tokens) {
        if (sum > std::numeric_limits<std::int64_t>::max() - e.tokens) {
            return std::numeric_limits<std::int64_t>::max();
        }
        sum += e.tokens;
    }
    return sum;
}

int QuotaManager::secondsUntilUtcMidnight(std::chrono::system_clock::time_point now) {
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday += 1;
    const std::time_t next = timegm(&tm);
    const auto seconds = static_cast<int>(next - t);
    return seconds > 0 ? seconds : static_cast<int>(kWindow.count());
}

QuotaDecision QuotaManager::acquire(std::string token_id, const QuotaLimits& limits,
                                    std::int64_t estimated_tokens, NanoRub estimated_cost,
                                    std::chrono::system_clock::time_point now) {
    QuotaDecision d;
    if (limits.rpm <= 0 && limits.tpm <= 0 && limits.max_concurrent <= 0 &&
        limits.day_budget_nano <= 0) {
        d.allowed = true;
        return d;
    }
    estimated_tokens = std::max<std::int64_t>(0, estimated_tokens);
    estimated_cost = std::max<NanoRub>(0, estimated_cost);
    const std::string day = utcDayString(now);

    std::lock_guard<std::mutex> lock(mu_);
    auto& st = states_[token_id];
    prune(st, now);
    if (st.spent.find(day) == st.spent.end()) {
        st.spent[day] = db_ ? db_->tokenDayCost(token_id, day) : 0;
    }
    const NanoRub spent = st.spent[day];

    auto reject = [&](const std::string& code, const std::string& message, int retry) {
        d.allowed = false;
        d.status = 429;
        d.code = code;
        d.message = message;
        d.retry_after_sec = std::max(1, retry);
        return d;
    };

    if (limits.rpm > 0 && static_cast<int>(st.requests.size()) >= limits.rpm) {
        const auto wait = std::chrono::duration_cast<std::chrono::seconds>(
            st.requests.front().at + kWindow - now).count();
        return reject("token_rpm_exceeded", "token requests-per-minute limit exceeded",
                      static_cast<int>(wait) + 1);
    }
    if (limits.tpm > 0) {
        const auto used = windowTokens(st);
        if (used >= limits.tpm || estimated_tokens > limits.tpm - used) {
            int retry = static_cast<int>(kWindow.count());
            if (!st.tokens.empty()) {
                retry = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                    st.tokens.front().at + kWindow - now).count()) + 1;
            }
            return reject("token_tpm_exceeded", "token tokens-per-minute limit exceeded", retry);
        }
    }
    if (limits.max_concurrent > 0 && st.concurrent >= limits.max_concurrent) {
        return reject("token_concurrency_exceeded",
                      "too many concurrent requests for this token", 1);
    }
    if (limits.day_budget_nano > 0) {
        const NanoRub remaining = limits.day_budget_nano - spent;
        if (remaining <= 0 || estimated_cost > remaining) {
            return reject("budget_exceeded", "daily token budget exceeded",
                          secondsUntilUtcMidnight(now));
        }
    }

    QuotaTicket ticket;
    ticket.active = true;
    ticket.token_id = token_id;
    ticket.day = day;
    ticket.reserved_tokens = estimated_tokens;
    ticket.reserved_cost = estimated_cost;
    ticket.seq = ++seq_;

    st.requests.push_back({now, ticket.seq});
    st.tokens.push_back({now, estimated_tokens, ticket.seq});
    ++st.concurrent;
    st.spent[day] = spent + estimated_cost;
    d.allowed = true;
    d.ticket = ticket;
    return d;
}

void QuotaManager::settle(QuotaTicket& ticket, std::int64_t tokens_used, NanoRub cost) {
    if (!ticket.active) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = states_.find(ticket.token_id);
    if (it == states_.end()) {
        ticket.active = false;
        return;
    }
    auto& st = it->second;
    if (st.concurrent > 0) --st.concurrent;
    for (auto& entry : st.tokens) {
        if (entry.seq == ticket.seq) {
            entry.tokens = tokens_used > 0 ? tokens_used : ticket.reserved_tokens;
            break;
        }
    }
    cost = std::max<NanoRub>(0, cost);
    auto& spent = st.spent[ticket.day];
    spent += cost - ticket.reserved_cost;
    if (spent < 0) spent = 0;
    ticket.active = false;
}

void QuotaManager::release(QuotaTicket& ticket) {
    if (!ticket.active) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = states_.find(ticket.token_id);
    if (it != states_.end()) {
        auto& st = it->second;
        if (st.concurrent > 0) --st.concurrent;
        st.requests.erase(std::remove_if(st.requests.begin(), st.requests.end(),
                                         [&](const RequestWindowEntry& e) {
                                             return e.seq == ticket.seq;
                                         }),
                          st.requests.end());
        st.tokens.erase(std::remove_if(st.tokens.begin(), st.tokens.end(),
                                       [&](const TokenWindowEntry& e) {
                                           return e.seq == ticket.seq;
                                       }),
                        st.tokens.end());
        auto sit = st.spent.find(ticket.day);
        if (sit != st.spent.end()) {
            sit->second -= ticket.reserved_cost;
            if (sit->second < 0) sit->second = 0;
        }
    }
    ticket.active = false;
}

int QuotaManager::concurrent(const std::string& token_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = states_.find(token_id);
    return it == states_.end() ? 0 : it->second.concurrent;
}

NanoRub QuotaManager::spentToday(const std::string& token_id,
                                 std::chrono::system_clock::time_point now) const {
    const std::string day = utcDayString(now);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = states_.find(token_id);
    if (it != states_.end()) {
        auto sit = it->second.spent.find(day);
        if (sit != it->second.spent.end()) return sit->second;
    }
    return db_ ? db_->tokenDayCost(token_id, day) : 0;
}

} // namespace proxy