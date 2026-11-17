#pragma once

#include "util/NanoRub.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace proxy {

class StatsDb;

struct QuotaLimits {
    int rpm = 0;                 // 0 = unlimited
    int tpm = 0;                 // 0 = unlimited
    int max_concurrent = 0;      // 0 = unlimited
    NanoRub day_budget_nano = 0; // 0 = unlimited
};

struct QuotaTicket {
    bool active = false;
    std::string token_id;
    std::string day;
    std::int64_t reserved_tokens = 0;
    NanoRub reserved_cost = 0;
    std::uint64_t seq = 0;
};

struct QuotaDecision {
    bool allowed = true;
    int status = 0;   // заполняется только при отказе (reject)
    std::string code;
    std::string message;
    int retry_after_sec = 0;
    QuotaTicket ticket;
};

// Per-token sliding-window limits (RPM/TPM), concurrency and a UTC daily
// budget. The budget is reserved at admission and settled with the actual or
// estimated cost at completion; persisted history seeds it after a restart.
class QuotaManager {
  public:
    QuotaManager() = default;
    void attach(StatsDb* db) { db_ = db; }

    QuotaDecision acquire(std::string token_id, const QuotaLimits& limits,
                          std::int64_t estimated_tokens, NanoRub estimated_cost,
                          std::chrono::system_clock::time_point now =
                              std::chrono::system_clock::now());
    void settle(QuotaTicket& ticket, std::int64_t tokens_used, NanoRub cost);
    void release(QuotaTicket& ticket);

    int concurrent(const std::string& token_id) const;
    NanoRub spentToday(const std::string& token_id,
                       std::chrono::system_clock::time_point now =
                           std::chrono::system_clock::now()) const;

  private:
    struct RequestWindowEntry {
        std::chrono::system_clock::time_point at;
        std::uint64_t seq = 0;
    };
    struct TokenWindowEntry {
        std::chrono::system_clock::time_point at;
        std::int64_t tokens = 0;
        std::uint64_t seq = 0;
    };
    struct State {
        std::deque<RequestWindowEntry> requests;
        std::deque<TokenWindowEntry> tokens;
        int concurrent = 0;
        std::map<std::string, NanoRub> spent; // UTC day -> reserved + settled
    };

    static void prune(State& state, std::chrono::system_clock::time_point now);
    static std::int64_t windowTokens(const State& state);
    static int secondsUntilUtcMidnight(std::chrono::system_clock::time_point now);

    mutable std::mutex mu_;
    std::map<std::string, State> states_;
    StatsDb* db_ = nullptr;
    std::uint64_t seq_ = 0;
};

} // namespace proxy