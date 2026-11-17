#pragma once

#include "config/Config.h"
#include "util/Cidr.h"
#include "util/NanoRub.h"
#include "util/Time.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace proxy {

struct PasswordHash {
    static std::string hash(const std::string& password);
    static bool verify(const std::string& password, const std::string& encoded);
};

struct Principal {
    std::string token_id;
    std::vector<std::string> scopes;
    std::vector<std::string> models;
    int rpm = 0;
    int tpm = 0;
    int max_concurrent = 0;
    NanoRub day_budget_nano = 0;

    bool hasScope(std::string_view scope) const;
    bool canUseModel(std::string_view model) const;
};

struct TokenEntry {
    std::string id;
    std::string digest_hex; // sha256 hex of the secret
    std::vector<std::string> scopes;
    std::vector<std::string> models;
    std::chrono::system_clock::time_point expires_at{};
    bool expires = false;
    std::vector<std::string> allowed_networks;
    int rpm = 0;
    int tpm = 0;
    int max_concurrent = 0;
    std::int64_t day_budget_nano = 0;
};

class TokenStore {
  public:
    explicit TokenStore(const std::vector<TokenConfig>& configs);
    std::optional<Principal> authenticate(std::string_view authorization_header,
                                          std::string_view client_ip = {}) const;
    static std::string normalizeHash(const TokenConfig& cfg);
  private:
    std::vector<TokenEntry> entries_;
};

struct Session {
    std::string id;
    std::string username;
    std::string role;
    std::string csrf;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point last_seen;
};

class SessionStore {
  public:
    SessionStore(int absolute_ttl_sec, int idle_ttl_sec);
    std::optional<Session> create(std::string username, std::string role);
    // Возвращает сессию и продлевает её простой: скользящий idle TTL — часть
    // операции, а не побочный эффект чтения.
    std::optional<Session> touch(const std::string& session_id);
    void destroy(const std::string& session_id);
    bool validateCsrf(const std::string& session_id, std::string_view csrf) const;
    void gc();

  private:
    std::string key(const std::string& id) const;
    mutable std::mutex mu_;
    std::map<std::string, Session> sessions_;
    int absolute_ttl_sec_;
    int idle_ttl_sec_;
};

class LoginLimiter {
  public:
    LoginLimiter(int attempts, int window_sec, int lockout_sec);
    bool allow(const std::string& key) const;
    void failure(const std::string& key);
    void success(const std::string& key);

  private:
    struct State { int failures = 0; std::chrono::system_clock::time_point first; std::chrono::system_clock::time_point locked_until; };
    mutable std::mutex mu_;
    std::map<std::string, State> states_;
    int attempts_, window_sec_, lockout_sec_;
};

std::string bearerToken(std::string_view header);
// True for 127.0.0.0/8, ::1 and IPv4-mapped loopback (::ffff:127.x.x.x).
bool isLoopbackIp(std::string_view ip);

} // namespace proxy
