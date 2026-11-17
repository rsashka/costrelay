#include "auth/Auth.h"
#include "util/Cidr.h"
#include "util/Strings.h"
#include "util/NanoRub.h"

#include <sodium.h>
#include <openssl/rand.h>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace proxy {

std::string PasswordHash::hash(const std::string& password) {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    });
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out, password.data(), password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        throw std::runtime_error("argon2id hash failed (out of memory?)");
    }
    return std::string(out);
}

bool PasswordHash::verify(const std::string& password, const std::string& encoded) {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    });
    if (encoded.rfind("$argon2", 0) != 0) return false;
    return crypto_pwhash_str_verify(encoded.c_str(), password.data(), password.size()) == 0;
}

bool Principal::hasScope(std::string_view scope) const {
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

bool Principal::canUseModel(std::string_view model) const {
    for (const auto& pattern : models) {
        if (pattern == "*" || globMatch(pattern, model)) return true;
    }
    return false;
}

std::string TokenStore::normalizeHash(const TokenConfig& cfg) {
    if (cfg.token_hash.empty()) {
        throw std::runtime_error("token '" + cfg.id + "' has no valid hash");
    }
    if (!isSha256Hash(cfg.token_hash)) {
        throw std::runtime_error("token '" + cfg.id + "' hash must be 64 hex chars");
    }
    return normalizeSha256Hash(cfg.token_hash);
}

TokenStore::TokenStore(const std::vector<TokenConfig>& configs) {
    for (const auto& c : configs) {
        if (c.id.empty()) throw std::runtime_error("token id is empty");
        TokenEntry e;
        e.id = c.id;
        e.digest_hex = normalizeHash(c);
        e.scopes = c.scopes;
        e.models = c.models;
        e.allowed_networks = c.allowed_networks;
        e.rpm = c.rpm;
        e.tpm = c.tpm;
        e.max_concurrent = c.max_concurrent;
        e.day_budget_nano = static_cast<std::int64_t>(c.day_budget_rub * kNanoPerRub);
        if (!c.expires_at.empty()) {
            if (!parseRfc3339(c.expires_at, e.expires_at)) {
                throw std::runtime_error("token '" + c.id + "' has invalid expires_at");
            }
            e.expires = true;
        }
        entries_.push_back(std::move(e));
    }
}

std::optional<Principal> TokenStore::authenticate(std::string_view header,
                                                  std::string_view client_ip) const {
    const auto token = bearerToken(header);
    if (token.empty()) return std::nullopt;
    const auto digest = sha256Hex(token);
    const auto now = std::chrono::system_clock::now();
    for (const auto& e : entries_) {
        if (constantTimeEqual(digest, e.digest_hex)) {
            if (e.expires && now >= e.expires_at) return std::nullopt;
            if (!e.allowed_networks.empty() && !client_ip.empty()) {
                bool allowed = false;
                for (const auto& cidr : e.allowed_networks) {
                    if (ipInCidr(client_ip, cidr)) { allowed = true; break; }
                }
                if (!allowed) return std::nullopt;
            }
            Principal p;
            p.token_id = e.id;
            p.scopes = e.scopes;
            p.models = e.models;
            p.rpm = e.rpm;
            p.tpm = e.tpm;
            p.max_concurrent = e.max_concurrent;
            p.day_budget_nano = e.day_budget_nano;
            return p;
        }
    }
    return std::nullopt;
}

SessionStore::SessionStore(int absolute_ttl_sec, int idle_ttl_sec)
    : absolute_ttl_sec_(absolute_ttl_sec), idle_ttl_sec_(idle_ttl_sec) {}

std::string SessionStore::key(const std::string& id) const { return sha256Hex(id); }

std::optional<Session> SessionStore::create(std::string username, std::string role) {
    std::lock_guard<std::mutex> lock(mu_);
    Session s;
    s.id = randomBase64Url(32);
    s.username = std::move(username);
    s.role = std::move(role);
    s.csrf = randomBase64Url(24);
    s.created = std::chrono::system_clock::now();
    s.last_seen = s.created;
    sessions_[key(s.id)] = s;
    return s;
}

std::optional<Session> SessionStore::touch(const std::string& session_id) {
    if (session_id.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(key(session_id));
    if (it == sessions_.end()) return std::nullopt;
    const auto now = std::chrono::system_clock::now();
    auto& s = it->second;
    if (now - s.created > std::chrono::seconds(absolute_ttl_sec_) ||
        now - s.last_seen > std::chrono::seconds(idle_ttl_sec_)) {
        sessions_.erase(it);
        return std::nullopt;
    }
    s.last_seen = now;
    return s;
}

void SessionStore::destroy(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.erase(key(session_id));
}

bool SessionStore::validateCsrf(const std::string& session_id, std::string_view csrf) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(key(session_id));
    if (it == sessions_.end()) return false;
    return constantTimeEqual(it->second.csrf, csrf);
}

void SessionStore::gc() {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::system_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second.created > std::chrono::seconds(absolute_ttl_sec_) ||
            now - it->second.last_seen > std::chrono::seconds(idle_ttl_sec_)) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

LoginLimiter::LoginLimiter(int attempts, int window_sec, int lockout_sec)
    : attempts_(attempts), window_sec_(window_sec), lockout_sec_(lockout_sec) {}

bool LoginLimiter::allow(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = states_.find(key);
    if (it == states_.end()) return true;
    const auto now = std::chrono::system_clock::now();
    if (now < it->second.locked_until) return false;
    if (now - it->second.first > std::chrono::seconds(window_sec_)) return true;
    return it->second.failures < attempts_;
}

void LoginLimiter::failure(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::system_clock::now();
    auto& st = states_[key];
    if (st.failures == 0 || now - st.first > std::chrono::seconds(window_sec_)) {
        st.failures = 0;
        st.first = now;
    }
    ++st.failures;
    if (st.failures >= attempts_) st.locked_until = now + std::chrono::seconds(lockout_sec_);
}

void LoginLimiter::success(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    states_.erase(key);
}

std::string bearerToken(std::string_view header) {
    std::string h = trim(header);
    if (h.size() < 7) return {};
    std::string prefix = toLower(h.substr(0, 7));
    if (prefix != "bearer ") return {};
    return trim(std::string_view(h).substr(7));
}

bool isLoopbackIp(std::string_view ip) {
    if (ip == "::1") return true;
    if (startsWith(ip, "::ffff:")) ip.remove_prefix(7);
    return ipInCidr(ip, "127.0.0.0/8");
}

} // namespace proxy
