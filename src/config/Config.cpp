#include "config/Config.h"
#include "util/Cidr.h"
#include "util/NanoRub.h"
#include "util/Paths.h"
#include "util/Strings.h"
#include "util/Time.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace proxy {
namespace {

std::string nodeKind(const YAML::Node& n) {
    if (!n) return "missing";
    if (n.IsNull()) return "null";
    if (n.IsScalar()) return "scalar";
    if (n.IsSequence()) return "sequence";
    if (n.IsMap()) return "mapping";
    return "unknown";
}

// Узел, у которого ожидается скалярное значение: тип проверяется, а не
// подменяется дефолтом — опечатка в структуре обязана быть видимой.
void requireScalar(const YAML::Node& n, const char* what) {
    if (!n || n.IsNull()) return;
    if (!n.IsScalar()) {
        throw ConfigError(std::string(what) + " must be a scalar, got " + nodeKind(n));
    }
}

int durationToSeconds(const YAML::Node& n, int def) {
    if (!n || n.IsNull()) return def;
    requireScalar(n, "duration");
    std::string s = trim(n.as<std::string>());
    if (s.empty()) return def;
    char suffix = s.back();
    std::string num = s;
    long long mult = 1;
    if (suffix == 's' || suffix == 'S') { num = s.substr(0, s.size() - 1); mult = 1; }
    else if (suffix == 'm' || suffix == 'M') { num = s.substr(0, s.size() - 1); mult = 60; }
    else if (suffix == 'h' || suffix == 'H') { num = s.substr(0, s.size() - 1); mult = 3600; }
    else if (suffix == 'd' || suffix == 'D') { num = s.substr(0, s.size() - 1); mult = 86400; }
    try {
        std::size_t used = 0;
        const long long amount = std::stoll(num, &used);
        if (used != num.size()) throw std::invalid_argument("trailing characters");
        return static_cast<int>(amount * mult);
    } catch (...) {
        throw ConfigError("invalid duration: '" + s + "' (use s, m, h or d)");
    }
}

std::string scalar(const YAML::Node& n, const std::string& def = {}) {
    if (!n || n.IsNull()) return def;
    requireScalar(n, "value");
    return n.as<std::string>();
}

// Неизвестный ключ — ошибка, а не молчаливое игнорирование: опечатка в имени
// настройки иначе оставляет конфигурацию без заданного правила, а защита
// (список сетей, лимит) тихо не включается.
void requireKnownKeys(const YAML::Node& node, const std::string& block,
                      std::initializer_list<const char*> known) {
    if (!node || !node.IsMap()) return;
    std::set<std::string> allowed;
    for (const char* key : known) allowed.insert(key);
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) {
            throw ConfigError(block + " has a non-scalar key");
        }
        const std::string key = entry.first.as<std::string>();
        if (!allowed.count(key)) {
            throw ConfigError(block + " has unknown key '" + key + "'");
        }
    }
}

bool boolean(const YAML::Node& n, bool def) {
    if (!n || n.IsNull()) return def;
    requireScalar(n, "boolean");
    const std::string s = toLower(trim(n.as<std::string>()));
    if (s.empty()) return def;
    if (s == "true" || s == "yes" || s == "1" || s == "on") return true;
    if (s == "false" || s == "no" || s == "0" || s == "off") return false;
    throw ConfigError("invalid boolean: '" + n.as<std::string>() + "'");
}

long double decimal(const YAML::Node& n, long double def) {
    if (!n || n.IsNull()) return def;
    requireScalar(n, "number");
    long double value = 0;
    try {
        std::size_t used = 0;
        value = std::stold(n.as<std::string>(), &used);
        if (used != n.as<std::string>().size()) throw std::invalid_argument("trailing characters");
    } catch (...) {
        throw ConfigError("invalid number: '" + n.as<std::string>() + "'");
    }
    if (!std::isfinite(static_cast<double>(value))) {
        throw ConfigError("number must be finite: '" + n.as<std::string>() + "'");
    }
    return value;
}

std::int64_t int64(const YAML::Node& n, std::int64_t def) {
    if (!n || n.IsNull()) return def;
    requireScalar(n, "integer");
    try {
        std::size_t used = 0;
        const auto value = std::stoll(n.as<std::string>(), &used);
        if (used != n.as<std::string>().size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (...) {
        throw ConfigError("invalid integer: '" + n.as<std::string>() + "'");
    }
}

// Размер в байтах: число или число с двоичным суффиксом k/kib, m/mib, g/gib.
// Неоднозначные kb/mb/gb отвергаются: ожидание «1000» и факт «1024» расходятся
// молча, а такое расхождение — ошибка конфигурации.
std::size_t byteSize(const YAML::Node& n, std::size_t def) {
    if (!n || n.IsNull()) return def;
    requireScalar(n, "size");
    std::string s = trim(n.as<std::string>());
    if (s.empty()) return def;
    unsigned long long mult = 1;
    auto stripSuffix = [&](const char* suffix) {
        const std::string u = suffix;
        if (s.size() <= u.size()) return false;
        if (toLower(s.substr(s.size() - u.size())) != u) return false;
        s = trim(s.substr(0, s.size() - u.size()));
        return true;
    };
    if (stripSuffix("kib") || stripSuffix("k")) {
        mult = 1024ULL;
    } else if (stripSuffix("mib") || stripSuffix("m")) {
        mult = 1024ULL * 1024ULL;
    } else if (stripSuffix("gib") || stripSuffix("g")) {
        mult = 1024ULL * 1024ULL * 1024ULL;
    }
    try {
        std::size_t used = 0;
        const auto value = std::stoull(s, &used);
        if (used != s.size()) throw std::invalid_argument("trailing characters");
        return static_cast<std::size_t>(value * mult);
    } catch (...) {
        throw ConfigError("invalid size: '" + n.as<std::string>() +
                          "' (use k, m, g, kib, mib or gib)");
    }
}

std::vector<std::string> stringList(const YAML::Node& n, std::vector<std::string> def = {}) {
    if (!n || n.IsNull()) return def;
    if (n.IsScalar()) return {n.as<std::string>()};
    if (!n.IsSequence()) {
        throw ConfigError(std::string("list value must be a sequence, got ") + nodeKind(n));
    }
    std::vector<std::string> out;
    for (const auto& x : n) {
        requireScalar(x, "list item");
        out.push_back(x.as<std::string>());
    }
    return out;
}

YAML::Node child(const YAML::Node& n, const std::string& key) {
    if (!n || n.IsNull()) return YAML::Node();
    if (!n.IsMap()) {
        throw ConfigError("block '" + key + "' expects a mapping, got " + nodeKind(n));
    }
    return n[key];
}

bool isLoopbackHost(const std::string& host) {
    const std::string h = toLower(host);
    return h == "localhost" || h == "::1" || h == "[::1]" ||
           h.rfind("127.", 0) == 0;
}

PanelUserConfig parseUser(const YAML::Node& n, const std::string& base_dir) {
    requireKnownKeys(n, "portal user", {"username", "password", "password_hash", "role"});
    PanelUserConfig u;
    u.username = scalar(n["username"]);
    if (n["password"] && !n["password"].IsNull()) {
        throw ConfigError("panel user '" + u.username +
                          "': plaintext 'password' is not supported; use password_hash");
    }
    u.password_hash = scalar(n["password_hash"]);
    if (!u.password_hash.empty()) u.password_hash = expandSecret(u.password_hash, base_dir);
    u.role = scalar(n["role"], "viewer");
    return u;
}

std::string expandHeaderValue(const std::string& value, const std::string& base_dir) {
    if (startsWith(value, "file:") || startsWith(value, "env:")) {
        return expandSecret(value, base_dir);
    }
    const auto space = value.find(' ');
    if (space != std::string::npos) {
        const std::string ref = value.substr(space + 1);
        if (startsWith(ref, "file:") || startsWith(ref, "env:")) {
            return value.substr(0, space + 1) + expandSecret(ref, base_dir);
        }
    }
    return value;
}

// Ключи времени заданы в секундах, структура — в миллисекундах: дефолт остаётся
// в структуре, а перевод единицы живёт здесь.
int defaultSeconds(int milliseconds) { return std::max(1, milliseconds / 1000); }

PortalAppConfig parsePortalApp(const YAML::Node& n, const std::string& base_dir) {
    requireKnownKeys(n, "portal service",
                     {"id", "title", "url", "roles", "headers", "verify_tls", "timeout",
                      "max_response_bytes"});
    PortalAppConfig a;
    a.id = scalar(n["id"]);
    a.title = scalar(n["title"], a.id);
    a.url = scalar(n["url"]);
    a.roles = stringList(n["roles"], {"admin", "viewer"});
    a.verify_tls = boolean(n["verify_tls"], a.verify_tls);
    a.timeout_ms = durationToSeconds(child(n, "timeout"), defaultSeconds(a.timeout_ms)) * 1000;
    a.max_response_bytes = static_cast<std::size_t>(int64(
        child(n, "max_response_bytes"), static_cast<std::int64_t>(a.max_response_bytes)));
    if (auto h = child(n, "headers"); h && h.IsMap()) {
        for (const auto& it : h) {
            if (!it.first.IsScalar() || !it.second.IsScalar()) {
                throw ConfigError("portal app '" + a.id +
                                  "' headers must map scalars to scalars");
            }
            a.headers[it.first.as<std::string>()] =
                expandHeaderValue(it.second.as<std::string>(), base_dir);
        }
    }
    return a;
}

TimeoutConfig parseTimeouts(const YAML::Node& n) {
    TimeoutConfig t;
    t.connect_ms = durationToSeconds(child(n, "connect"), defaultSeconds(t.connect_ms)) * 1000;
    t.total_ms = durationToSeconds(child(n, "total"), defaultSeconds(t.total_ms)) * 1000;
    return t;
}

std::pair<std::string, int> parseListen(const std::string& value,
                                        const std::string& default_host,
                                        int default_port) {
    if (value.empty()) return {default_host, default_port};
    std::string host = value;
    int port = default_port;
    const auto parsePort = [&](const std::string& text) {
        try {
            std::size_t used = 0;
            const int parsed = std::stoi(text, &used);
            if (used != text.size()) throw std::invalid_argument("trailing characters");
            return parsed;
        } catch (...) {
            // Неразобранный порт — ошибка конфигурации, а не тихая подмена дефолтом:
            // иначе сервис слушает не тот адрес, который записан в файле.
            throw ConfigError("invalid listen port: '" + text + "' in '" + value + "'");
        }
    };
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string::npos) {
            throw ConfigError("invalid listen address: '" + value + "'");
        }
        host = host.substr(1, close - 1);
        if (close + 1 < value.size() && value[close + 1] == ':') {
            port = parsePort(value.substr(close + 2));
        }
    } else {
        const auto colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon) {
            port = parsePort(host.substr(colon + 1));
            host.resize(colon);
        }
    }
    if (host.empty()) throw ConfigError("invalid listen address: '" + value + "'");
    return {host, port};
}

void parseSimpleProviderModels(RouterConfig& r, const YAML::Node& n) {
    const auto models = n["models"];
    if (!models) {
        r.models = {"*"};
        return;
    }
    if (models.IsMap()) {
        r.models.clear();
        for (const auto& it : models) {
            if (!it.first.IsScalar() || !it.second.IsScalar()) {
                throw ConfigError("upstream '" + r.id +
                                  "' models map must map public names to scalars");
            }
            const std::string public_name = it.first.as<std::string>();
            r.models.push_back(public_name);
            r.model_map[public_name] = it.second.as<std::string>();
        }
        return;
    }
    r.models = stringList(models);
}

RouterConfig parseSimpleProvider(const YAML::Node& n, bool local, const std::string& base_dir) {
    requireKnownKeys(n, "providers entry",
                     {"id", "type", "url", "enabled", "api_key", "models", "max_concurrent",
                      "verify_tls", "ca_file", "client_cert_file", "client_key_file",
                      "auth_header", "auth_scheme", "context_length", "cache_size_tokens",
                      "max_completion_tokens", "price_multiplier", "fixed_fee_rub",
                      "price_per_million", "timeouts"});
    requireKnownKeys(child(n, "price_per_million"), "price_per_million",
                     {"input", "output", "cache_read"});
    requireKnownKeys(child(n, "timeouts"), "timeouts", {"connect", "total"});
    RouterConfig r;
    r.id = scalar(n["id"]);
    const std::string type = scalar(n["type"], local ? "openai" : "routerai");
    r.kind = (!local && type == "routerai") ? "routerai" : "openai_compatible";
    r.base_url = scalar(n["url"]);
    r.enabled = boolean(n["enabled"], true);
    r.api_key = scalar(n["api_key"]);
    if (!r.api_key.empty()) {
        if (!startsWith(r.api_key, "file:") && !startsWith(r.api_key, "env:")) {
            throw ConfigError("upstream '" + r.id +
                              "': api_key must be referenced as file: or env:");
        }
        r.api_key = expandSecret(r.api_key, base_dir);
    }
    parseSimpleProviderModels(r, n);
    r.verify_tls = boolean(n["verify_tls"], true);
    r.http2 = !local && r.kind == "routerai";
    r.max_concurrent = static_cast<int>(int64(
        n["max_concurrent"], local ? kDefaultLocalConcurrency : kDefaultRemoteConcurrency));
    r.context_length = static_cast<int>(int64(n["context_length"], 0));
    r.cache_size_tokens = static_cast<int>(int64(n["cache_size_tokens"], 0));
    r.max_completion_tokens = static_cast<int>(int64(n["max_completion_tokens"], 0));
    r.price_multiplier = decimal(n["price_multiplier"], 1.0);
    r.fixed_fee_nano = static_cast<std::int64_t>(
        rubToNano(decimal(n["fixed_fee_rub"], 0)));
    r.auth_header = scalar(n["auth_header"], "Authorization");
    r.auth_scheme = scalar(n["auth_scheme"], "Bearer");
    r.ca_file = anchorPath(scalar(n["ca_file"]), base_dir);
    r.client_cert_file = anchorPath(scalar(n["client_cert_file"]), base_dir);
    r.client_key_file = anchorPath(scalar(n["client_key_file"]), base_dir);
    if (auto t = child(n, "timeouts"); t && t.IsMap()) r.timeouts = parseTimeouts(t);
    if (r.kind == "openai_compatible") {
        auto price = child(n, "price_per_million");
        if (price && price.IsMap()) {
            r.prompt_per_million_rub = decimal(child(price, "input"), 0);
            r.completion_per_million_rub = decimal(child(price, "output"), 0);
            r.cache_read_per_million_rub = decimal(child(price, "cache_read"),
                                                   r.prompt_per_million_rub);
        } else if (!local) {
            throw ConfigError("external provider '" + r.id +
                              "' requires price_per_million for openai-compatible upstream");
        }
    }
    return r;
}

TokenConfig parseSimpleConsumer(const YAML::Node& n, const std::string& base_dir) {
    requireKnownKeys(n, "consumers entry",
                     {"id", "token", "token_hash", "models", "scopes", "rpm", "tpm",
                      "max_concurrent", "budget_rub_day", "expires_at", "allowed_networks"});
    TokenConfig t;
    t.id = scalar(n["id"]);
    const bool has_token = n["token"] && !n["token"].IsNull();
    const bool has_hash = n["token_hash"] && !n["token_hash"].IsNull();
    if (has_token && has_hash) {
        throw ConfigError("consumer '" + t.id + "': use token or token_hash, not both");
    }
    if (has_token) {
        const std::string source = scalar(n["token"]);
        if (!startsWith(source, "file:") && !startsWith(source, "env:")) {
            throw ConfigError("consumer '" + t.id +
                              "': raw token must be referenced as file: or env:; use token_hash for a hash");
        }
        const std::string raw = expandSecret(source, base_dir);
        if (raw.empty()) throw ConfigError("consumer '" + t.id + "' token is empty");
        t.token_hash = "sha256:" + sha256Hex(raw);
    } else if (has_hash) {
        const std::string source = scalar(n["token_hash"]);
        const bool reference = startsWith(source, "file:") || startsWith(source, "env:");
        std::string value = expandSecret(source, base_dir);
        if (reference && !isSha256Hash(value)) {
            if (value.empty()) throw ConfigError("consumer '" + t.id + "' token is empty");
            value = "sha256:" + sha256Hex(value);
        }
        t.token_hash = value;
    }
    t.models = stringList(n["models"], {"*"});
    t.scopes = stringList(n["scopes"], {"proxy:invoke"});
    t.rpm = static_cast<int>(int64(n["rpm"], 0));
    t.tpm = static_cast<int>(int64(n["tpm"], 0));
    t.max_concurrent = static_cast<int>(int64(n["max_concurrent"], 0));
    t.expires_at = scalar(n["expires_at"]);
    t.allowed_networks = stringList(n["allowed_networks"]);
    t.day_budget_rub = decimal(n["budget_rub_day"], 0);
    return t;
}

std::vector<ProviderBlockConfig> parseProviderBlockList(const YAML::Node& n) {
    std::vector<ProviderBlockConfig> out;
    if (!n || n.IsNull()) return out;
    if (!n.IsSequence()) {
        // Пустой whitelist означает «маршрутизируются все»: молча принять
        // неверный тип нельзя — это расширение доступа вместо ограничения.
        throw ConfigError("blacklist/whitelist must be a sequence, got " + nodeKind(n));
    }
    for (const auto& item : n) {
        requireKnownKeys(item, "blacklist/whitelist entry", {"provider", "models", "reason"});
        ProviderBlockConfig b;
        if (item.IsScalar()) {
            b.provider = item.as<std::string>();
            b.reason = kDefaultBlockReason;
        } else if (item.IsMap()) {
            b.provider = scalar(item["provider"]);
            b.models = stringList(item["models"], {"*"});
            b.reason = scalar(item["reason"], kDefaultBlockReason);
        } else {
            throw ConfigError("blacklist/whitelist entry must be a string or mapping, got " +
                              nodeKind(item));
        }
        if (b.provider.empty()) {
            throw ConfigError("blacklist/whitelist entry has empty provider");
        }
        out.push_back(std::move(b));
    }
    return out;
}

AppConfig parseConfig(const YAML::Node& root, const std::string& base_dir) {
    AppConfig c;
    requireKnownKeys(root, "config",
                     {"proxy", "panel", "portal", "models", "default_model", "aliases",
                      "providers", "consumers", "analytics", "capture", "blacklist", "whitelist",
                      "stats", "pricing", "routing"});
    const auto proxy = child(root, "proxy");
    requireKnownKeys(proxy, "proxy",
                     {"name", "data_dir", "max_body_bytes", "listen", "threads", "public_url",
                      "require_https", "tls"});
    requireKnownKeys(child(proxy, "tls"), "proxy.tls", {"listen", "cert_file", "key_file"});
    requireKnownKeys(child(root, "panel"), "panel",
                     {"path", "allowed_networks", "session_absolute_ttl_sec",
                      "session_idle_ttl_sec", "cookie_name", "login_attempts", "login_window_sec",
                      "login_lockout_sec"});
    requireKnownKeys(child(root, "portal"), "portal", {"enabled", "users", "services"});
    requireKnownKeys(child(root, "providers"), "providers", {"external", "local"});
    requireKnownKeys(child(root, "analytics"), "analytics",
                     {"enabled", "log_dir", "snapshot", "auto_import", "max_file_bytes",
                      "max_samples_per_group"});
    requireKnownKeys(child(root, "capture"), "capture",
                     {"enabled", "key", "compression", "retention_days", "max_total_bytes",
                      "max_request_bytes", "max_response_bytes", "max_stream_bytes",
                      "capture_stream", "capture_errors", "sample_rate"});
    requireKnownKeys(child(root, "stats"), "stats",
                     {"enabled", "sqlite_path", "ring_buffer_size", "requests_retention_days",
                      "aggregates_retention_days", "prometheus"});
    requireKnownKeys(child(child(root, "stats"), "prometheus"), "stats.prometheus",
                     {"enabled", "require_token"});
    requireKnownKeys(child(root, "pricing"), "pricing",
                     {"threshold_mode", "stale_after_sec", "max_stale_sec", "require_fresh",
                      "timeout", "max_body_bytes", "refresh"});
    requireKnownKeys(child(child(root, "pricing"), "refresh"), "pricing.refresh",
                     {"hot_interval", "max_endpoints_per_model"});
    const auto routing_keys = child(root, "routing");
    requireKnownKeys(routing_keys, "routing",
                     {"client_provider_policy", "country", "quantization",
                      "inject_provider_options", "fallback_attempts", "candidate_cap",
                      "provider_order_size", "queue_min_sec", "queue_max_sec", "queue_stats_sec",
                      "cache_loss_spike", "cache_miss", "leader", "session_pin_sec",
                      "session_pin_max", "wait_price_rub_per_sec", "failover_wait_max_sec",
                      "output_weight", "cache_weight", "profile_sample_size", "retry",
                      "health_fail_threshold", "health_down_after_sec",
                      "health_probe_interval_sec", "health_recover_successes"});
    requireKnownKeys(child(routing_keys, "cache_loss_spike"), "routing.cache_loss_spike",
                     {"ratio", "window_sec"});
    requireKnownKeys(child(routing_keys, "cache_miss"), "routing.cache_miss",
                     {"warn", "blacklist"});
    requireKnownKeys(child(routing_keys, "leader"), "routing.leader",
                     {"margin_percent", "min_dwell_sec", "confirm_builds", "state_file"});
    requireKnownKeys(child(routing_keys, "retry"), "routing.retry", {"attempts", "backoff_ms"});
    c.instance_name = scalar(child(proxy, "name"), "costrelay");
    c.data_dir = anchorPath(scalar(child(proxy, "data_dir"), "./state"), base_dir);
    c.server.max_body_bytes = byteSize(child(proxy, "max_body_bytes"), c.server.max_body_bytes);

    auto [listen_host, listen_port] =
        parseListen(scalar(child(proxy, "listen"), "127.0.0.1:8080"), "127.0.0.1", 8080);
    c.server.listen = listen_host;
    c.server.port = listen_port;
    c.server.threads = static_cast<int>(int64(child(proxy, "threads"), 2));
    c.server.public_url = scalar(child(proxy, "public_url"));
    while (!c.server.public_url.empty() && c.server.public_url.back() == '/') {
        c.server.public_url.pop_back();
    }

    const auto tls = child(proxy, "tls");
    const bool has_tls = tls && tls.IsMap();
    std::string https_host;
    if (has_tls) {
        auto [tls_host, https_port] =
            parseListen(scalar(child(tls, "listen"), "0.0.0.0:8443"), "0.0.0.0", 8443);
        https_host = tls_host;
        c.server.remote_https_enabled = true;
        c.server.remote_https_listen = https_host;
        c.server.remote_https_port = https_port;
        c.server.tls_cert_file = anchorPath(scalar(child(tls, "cert_file")), base_dir);
        c.server.tls_key_file = anchorPath(scalar(child(tls, "key_file")), base_dir);
    }
    c.server.require_https = boolean(child(proxy, "require_https"), has_tls);

    const auto panel = child(root, "panel");
    c.panel.base_path = scalar(child(panel, "path"), "/panel");
    while (!c.panel.base_path.empty() && c.panel.base_path.back() == '/') {
        c.panel.base_path.pop_back();
    }
    if (c.panel.base_path.empty()) c.panel.base_path = "/panel";
    const bool network_listener = !isLoopbackHost(listen_host) ||
                                  (has_tls && !isLoopbackHost(https_host));
    if (auto nets = child(panel, "allowed_networks"); nets && !nets.IsNull()) {
        c.panel.allowed_networks = stringList(nets);
        if (c.panel.allowed_networks.empty()) {
            throw ConfigError("panel.allowed_networks is empty: list the allowed networks");
        }
    } else if (network_listener) {
        c.panel.allowed_networks = {"0.0.0.0/0", "::/0"};
    } else {
        c.panel.allowed_networks = {"127.0.0.1/32", "::1/128"};
    }
    c.panel.absolute_ttl_sec = static_cast<int>(
        int64(child(panel, "session_absolute_ttl_sec"), c.panel.absolute_ttl_sec));
    c.panel.idle_ttl_sec = static_cast<int>(
        int64(child(panel, "session_idle_ttl_sec"), c.panel.idle_ttl_sec));
    c.panel.cookie_name = scalar(child(panel, "cookie_name"), c.panel.cookie_name);
    c.panel.login_attempts = static_cast<int>(
        int64(child(panel, "login_attempts"), c.panel.login_attempts));
    c.panel.login_window_sec = static_cast<int>(
        int64(child(panel, "login_window_sec"), c.panel.login_window_sec));
    c.panel.login_lockout_sec = static_cast<int>(
        int64(child(panel, "login_lockout_sec"), c.panel.login_lockout_sec));


    const auto portal = child(root, "portal");
    c.portal.enabled = (portal && !portal.IsNull()) && boolean(child(portal, "enabled"), true);
    if (auto users = child(portal, "users"); users && users.IsSequence()) {
        for (const auto& u : users) c.panel.users.push_back(parseUser(u, base_dir));
    }
    if (auto services = child(portal, "services"); services && services.IsSequence()) {
        for (const auto& a : services) c.portal.apps.push_back(parsePortalApp(a, base_dir));
    }

    c.routing.models = stringList(root["models"]);
    if (auto aliases = child(root, "aliases"); aliases && aliases.IsMap()) {
        for (const auto& it : aliases) {
            if (!it.first.IsScalar() || !it.second.IsScalar()) {
                throw ConfigError("aliases must map model names to model names, got " +
                                  nodeKind(it.second));
            }
            c.routing.aliases[it.first.as<std::string>()] = it.second.as<std::string>();
        }
    }
    c.routing.default_model = scalar(root["default_model"]);
    if (c.routing.default_model.empty() && !c.routing.models.empty()) {
        c.routing.default_model = c.routing.models.front();
    }
    // Значения по умолчанию берутся из структуры: правка Config.h меняет и
    // фактический дефолт, копий чисел по месту разбора нет.
    const auto routing = child(root, "routing");
    const RoutingConfig rd;
    c.routing.fallback_attempts = static_cast<int>(
        int64(child(routing, "fallback_attempts"), rd.fallback_attempts));
    c.routing.provider_order_size = static_cast<int>(
        int64(child(routing, "provider_order_size"), rd.provider_order_size));
    c.routing.candidate_cap =
        static_cast<int>(int64(child(routing, "candidate_cap"), rd.candidate_cap));
    c.routing.queue_min_sec =
        static_cast<int>(int64(child(routing, "queue_min_sec"), rd.queue_min_sec));
    c.routing.queue_max_sec =
        static_cast<int>(int64(child(routing, "queue_max_sec"), rd.queue_max_sec));
    c.routing.queue_stats_sec =
        static_cast<int>(int64(child(routing, "queue_stats_sec"), rd.queue_stats_sec));
    if (child(routing, "cache_loss_spike")) {
        c.routing.cache_loss_spike.ratio = static_cast<double>(decimal(
            child(routing, "cache_loss_spike")["ratio"], rd.cache_loss_spike.ratio));
        c.routing.cache_loss_spike.window_sec = static_cast<int>(int64(
            child(routing, "cache_loss_spike")["window_sec"], rd.cache_loss_spike.window_sec));
    }
    if (child(routing, "cache_miss")) {
        const auto cache_miss = child(routing, "cache_miss");
        c.routing.cache_miss.warn = static_cast<int>(
            int64(child(cache_miss, "warn"), rd.cache_miss.warn));
        c.routing.cache_miss.blacklist = static_cast<int>(
            int64(child(cache_miss, "blacklist"), rd.cache_miss.blacklist));
    }
    if (child(routing, "leader")) {
        const auto leader = child(routing, "leader");
        c.routing.leader.margin_percent =
            static_cast<double>(decimal(child(leader, "margin_percent"),
                                        rd.leader.margin_percent));
        c.routing.leader.min_dwell_sec =
            static_cast<int>(int64(child(leader, "min_dwell_sec"), rd.leader.min_dwell_sec));
        c.routing.leader.confirm_builds =
            static_cast<int>(int64(child(leader, "confirm_builds"), rd.leader.confirm_builds));
        c.routing.leader.state_file = anchorPath(scalar(child(leader, "state_file")), base_dir);
    }
    if (c.routing.leader.state_file.empty()) {
        c.routing.leader.state_file =
            (std::filesystem::path(c.data_dir) / "routing-leader.json").string();
    }
    c.routing.session_pin_sec =
        static_cast<int>(int64(child(routing, "session_pin_sec"), rd.session_pin_sec));
    c.routing.session_pin_max =
        static_cast<int>(int64(child(routing, "session_pin_max"), rd.session_pin_max));
    c.routing.wait_price_rub_per_sec = static_cast<double>(
        decimal(child(routing, "wait_price_rub_per_sec"), rd.wait_price_rub_per_sec));
    c.routing.failover_wait_max_sec = static_cast<int>(
        int64(child(routing, "failover_wait_max_sec"), rd.failover_wait_max_sec));
    c.routing.output_weight = static_cast<double>(
        decimal(child(routing, "output_weight"), rd.output_weight));
    c.routing.cache_weight = static_cast<double>(
        decimal(child(routing, "cache_weight"), rd.cache_weight));
    c.routing.profile_sample_size = static_cast<int>(
        int64(child(routing, "profile_sample_size"), rd.profile_sample_size));
    if (child(routing, "retry")) {
        const auto retry = child(routing, "retry");
        c.routing.retry.attempts =
            static_cast<int>(int64(child(retry, "attempts"), rd.retry.attempts));
        c.routing.retry.backoff_ms =
            static_cast<int>(int64(child(retry, "backoff_ms"), rd.retry.backoff_ms));
    }
    c.routing.health.fail_threshold = static_cast<int>(
        int64(child(routing, "health_fail_threshold"), rd.health.fail_threshold));
    c.routing.health.down_after_sec = static_cast<int>(
        int64(child(routing, "health_down_after_sec"), rd.health.down_after_sec));
    c.routing.health.probe_interval_sec = static_cast<int>(
        int64(child(routing, "health_probe_interval_sec"), rd.health.probe_interval_sec));
    c.routing.health.recover_successes = static_cast<int>(
        int64(child(routing, "health_recover_successes"), rd.health.recover_successes));
    c.routing.client_provider_policy =
        scalar(child(routing, "client_provider_policy"), rd.client_provider_policy);
    c.routing.country = scalar(child(routing, "country"));
    c.routing.quantization = stringList(child(routing, "quantization"));
    c.routing.inject_provider_options = boolean(child(routing, "inject_provider_options"),
                                                rd.inject_provider_options);
    c.routing.provider_blacklist = parseProviderBlockList(child(root, "blacklist"));
    c.routing.provider_whitelist = parseProviderBlockList(child(root, "whitelist"));

    bool has_routerai = false;
    const auto providers = child(root, "providers");
    auto add_providers = [&](const YAML::Node& list, bool local) {
        if (!list || !list.IsSequence()) return;
        for (const auto& item : list) {
            RouterConfig r = parseSimpleProvider(item, local, base_dir);
            if (r.kind == "routerai") has_routerai = true;
            c.routers.push_back(std::move(r));
        }
    };
    add_providers(child(providers, "external"), false);
    add_providers(child(providers, "local"), true);

    if (has_routerai) {
        c.pricing.snapshot_file = (std::filesystem::path(c.data_dir) / "prices.json").string();
    }

    if (auto pr = child(root, "pricing"); pr && !pr.IsNull()) {
        const PricingConfig pd;
        c.pricing.threshold_mode = scalar(child(pr, "threshold_mode"), pd.threshold_mode);
        c.pricing.stale_after_sec =
            static_cast<int>(int64(child(pr, "stale_after_sec"), pd.stale_after_sec));
        c.pricing.max_stale_sec =
            static_cast<int>(int64(child(pr, "max_stale_sec"), pd.max_stale_sec));
        c.pricing.require_fresh = boolean(child(pr, "require_fresh"), pd.require_fresh);
        c.pricing.timeout_ms =
            durationToSeconds(child(pr, "timeout"), defaultSeconds(pd.timeout_ms)) * 1000;
        c.pricing.max_body_bytes =
            byteSize(child(pr, "max_body_bytes"), pd.max_body_bytes);
        if (child(pr, "refresh")) {
            const auto rf = child(pr, "refresh");
            c.pricing.refresh.hot_interval_sec = durationToSeconds(
                child(rf, "hot_interval"), c.pricing.refresh.hot_interval_sec);
            c.pricing.refresh.max_endpoints_per_model = static_cast<int>(
                int64(child(rf, "max_endpoints_per_model"),
                      c.pricing.refresh.max_endpoints_per_model));
        }
    }

    if (auto consumers = child(root, "consumers"); consumers && !consumers.IsNull()) {
        if (!consumers.IsSequence()) {
            throw ConfigError("consumers must be a sequence, got " + nodeKind(consumers));
        }
        for (const auto& item : consumers) c.tokens.push_back(parseSimpleConsumer(item, base_dir));
    }

    if (auto an = child(root, "analytics"); an && !an.IsNull()) {
        c.analytics.enabled = boolean(child(an, "enabled"), true);
        c.analytics.provider_log_dir = anchorPath(scalar(child(an, "log_dir"), "./data"), base_dir);
        c.analytics.snapshot_file = anchorPath(
            scalar(child(an, "snapshot"),
                   (std::filesystem::path(c.data_dir) / "provider-analytics.json").string()),
            base_dir);
        c.analytics.auto_import = boolean(child(an, "auto_import"), true);
        c.analytics.max_file_bytes = byteSize(child(an, "max_file_bytes"),
                                              c.analytics.max_file_bytes);
        c.analytics.max_samples_per_group =
            static_cast<int>(int64(child(an, "max_samples_per_group"),
                                   c.analytics.max_samples_per_group));
    }

    if (auto cap = child(root, "capture"); cap && !cap.IsNull()) {
        c.capture.enabled = boolean(child(cap, "enabled"), true);
        c.capture.store_dir = (std::filesystem::path(c.data_dir) / "bodies").string();
        c.capture.encryption_key = scalar(child(cap, "key"));
        if (!c.capture.encryption_key.empty()) {
            if (!startsWith(c.capture.encryption_key, "file:") &&
                !startsWith(c.capture.encryption_key, "env:")) {
                throw ConfigError("capture.key must be referenced as file: or env:");
            }
            c.capture.encryption_key = expandSecret(c.capture.encryption_key, base_dir);
        }
        c.capture.compression = scalar(child(cap, "compression"), c.capture.compression);
        c.capture.retention_days =
            static_cast<int>(int64(child(cap, "retention_days"), c.capture.retention_days));
        c.capture.max_total_bytes =
            byteSize(child(cap, "max_total_bytes"), c.capture.max_total_bytes);
        c.capture.max_request_bytes =
            byteSize(child(cap, "max_request_bytes"), c.capture.max_request_bytes);
        c.capture.max_response_bytes =
            byteSize(child(cap, "max_response_bytes"), c.capture.max_response_bytes);
        c.capture.max_stream_bytes =
            byteSize(child(cap, "max_stream_bytes"), c.capture.max_stream_bytes);
        c.capture.capture_stream = boolean(child(cap, "capture_stream"), c.capture.capture_stream);
        c.capture.capture_errors = boolean(child(cap, "capture_errors"), c.capture.capture_errors);
        c.capture.sample_rate = static_cast<double>(
            decimal(child(cap, "sample_rate"), static_cast<long double>(c.capture.sample_rate)));
    }

    const auto st = child(root, "stats");
    const StatsConfig sd;
    if (st && !st.IsNull()) {
        c.stats.enabled = boolean(child(st, "enabled"), sd.enabled);
        c.stats.sqlite_path = anchorPath(scalar(child(st, "sqlite_path")), base_dir);
        c.stats.ring_size =
            static_cast<int>(int64(child(st, "ring_buffer_size"), sd.ring_size));
        c.stats.prometheus = boolean(child(child(st, "prometheus"), "enabled"), sd.prometheus);
        c.stats.require_token =
            boolean(child(child(st, "prometheus"), "require_token"), sd.require_token);
        c.stats.requests_retention_days = static_cast<int>(
            int64(child(st, "requests_retention_days"), sd.requests_retention_days));
        c.stats.aggregates_retention_days = static_cast<int>(
            int64(child(st, "aggregates_retention_days"), sd.aggregates_retention_days));
    }

    c.validate();
    return c;
}

} // namespace

AppConfig AppConfig::load(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        throw ConfigError("cannot load config '" + path + "': " + e.what());
    }
    if (!root || !root.IsMap()) throw ConfigError("config root must be a mapping");
    // Relative paths belong to the configuration, not to the shell that started
    // the process: the same file must describe the same state from any cwd.
    const std::string base_dir = directoryOf(path);
    AppConfig c = parseConfig(root, base_dir);
    c.config_path = path;
    return c;
}

void AppConfig::validate() const {
    if (server.listen.empty()) throw ConfigError("server.listen is empty");
    if (server.port <= 0 || server.port > 65535) throw ConfigError("server.port is out of range");
    if (!server.public_url.empty() && server.public_url.rfind("http://", 0) != 0 &&
        server.public_url.rfind("https://", 0) != 0) {
        throw ConfigError("proxy.public_url must be http(s)");
    }
    if (routers.empty()) throw ConfigError("at least one upstream is required");
    if (server.max_body_bytes == 0) throw ConfigError("proxy.max_body_bytes must be positive");
    std::set<std::string> ids;
    bool any_enabled_router = false;
    for (const auto& r : routers) {
        if (r.id.empty()) throw ConfigError("upstream id is empty");
        if (!ids.insert(r.id).second) throw ConfigError("duplicate upstream id: " + r.id);
        if (r.base_url.rfind("http://", 0) != 0 && r.base_url.rfind("https://", 0) != 0) {
            throw ConfigError("upstream '" + r.id + "' base_url must be http(s)");
        }
        if (r.max_concurrent <= 0) throw ConfigError("upstream max_concurrent must be positive");
        // Нулевой таймаут libcurl означает «ждать вечно»: тихая потеря защиты.
        if (r.timeouts.connect_ms <= 0 || r.timeouts.total_ms < r.timeouts.connect_ms) {
            throw ConfigError("upstream '" + r.id +
                              "' timeouts must be 0 < connect <= total");
        }
        if (r.price_multiplier < 0) {
            throw ConfigError("upstream '" + r.id + "' price_multiplier must be >= 0");
        }
        if (r.fixed_fee_nano < 0) {
            throw ConfigError("upstream '" + r.id + "' fixed_fee_rub must be >= 0");
        }
        if (r.client_cert_file.empty() != r.client_key_file.empty()) {
            throw ConfigError("upstream '" + r.id +
                              "' requires both client_cert_file and client_key_file");
        }
        for (const auto* path : {&r.ca_file, &r.client_cert_file, &r.client_key_file}) {
            if (!path->empty() && !std::filesystem::exists(*path)) {
                throw ConfigError("upstream '" + r.id + "' file not found: " + *path);
            }
        }
        if (r.enabled) any_enabled_router = true;
    }
    if (!any_enabled_router) throw ConfigError("all upstreams are disabled");
    if (routing.models.empty()) throw ConfigError("models is empty");
    std::set<std::string> models(routing.models.begin(), routing.models.end());
    for (const auto& [alias, target] : routing.aliases) {
        if (!models.count(target)) throw ConfigError("alias '" + alias + "' points to unknown model '" + target + "'");
    }
    if (!routing.default_model.empty() && !models.count(routing.default_model)) {
        throw ConfigError("default_model is not in models");
    }
    if (portal.enabled) {
        if (panel.users.empty()) throw ConfigError("portal.enabled requires portal.users");
        for (const auto& u : panel.users) {
            if (u.username.empty()) throw ConfigError("panel user has empty username");
            if (u.password_hash.empty()) {
                throw ConfigError("panel user '" + u.username + "' has no password_hash");
            }
        }
    }
    if (portal.enabled) {
        std::set<std::string> ids;
        for (const auto& a : portal.apps) {
            if (a.id.empty()) throw ConfigError("portal app id is empty");
            if (a.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos) {
                throw ConfigError("portal app id must match [a-z0-9_-]+: " + a.id);
            }
            if (!ids.insert(a.id).second) {
                throw ConfigError("duplicate portal service id: " + a.id);
            }
            std::set<std::string> reserved = {"v1", "healthz", "readyz", "metrics"};
            const auto panel_slash = panel.base_path.find('/', 1);
            reserved.insert(panel.base_path.substr(
                1, panel_slash == std::string::npos ? std::string::npos : panel_slash - 1));
            if (reserved.count(a.id)) {
                throw ConfigError("portal service id conflicts with reserved path: " + a.id);
            }
            if (!startsWith(a.url, "http://") && !startsWith(a.url, "https://")) {
                throw ConfigError("portal app '" + a.id + "' url must be http(s)");
            }
            const std::size_t authority = startsWith(a.url, "https://") ? 8 : 7;
            if (a.url.size() <= authority || a.url[authority] == '/' ||
                a.url[authority] == ':') {
                throw ConfigError("portal app '" + a.id + "' url has no host");
            }
            if (a.url.find_first_of("@?# \r\n") != std::string::npos) {
                throw ConfigError("portal app '" + a.id +
                                  "' url must not contain credentials, query, fragment or spaces");
            }
            if (a.roles.empty()) {
                throw ConfigError("portal app '" + a.id + "' roles must not be empty");
            }
            for (const auto& role : a.roles) {
                if (role != "admin" && role != "viewer") {
                    throw ConfigError("portal app '" + a.id + "' has unknown role: " + role);
                }
            }
            if (a.timeout_ms <= 0) {
                throw ConfigError("portal app '" + a.id + "' timeout must be positive");
            }
            if (a.max_response_bytes == 0) {
                throw ConfigError("portal app '" + a.id + "' max_response_bytes must be positive");
            }
            for (const auto& [name, value] : a.headers) {
                if (name.empty() || name.find_first_of(":\r\n ") != std::string::npos) {
                    throw ConfigError("portal app '" + a.id + "' has invalid header name");
                }
                const std::string lname = toLower(name);
                if (lname == "host" || lname == "cookie" || lname == "content-length" ||
                    lname == "connection" || lname == "transfer-encoding" ||
                    lname == "upgrade") {
                    throw ConfigError("portal app '" + a.id + "' must not set header " + name);
                }
                if (value.find_first_of("\r\n") != std::string::npos) {
                    throw ConfigError("portal app '" + a.id + "' header '" + name +
                                      "' contains CR/LF");
                }
            }
        }
    }
    if (server.threads <= 0) throw ConfigError("proxy.threads must be positive");
    if (panel.base_path.empty() || panel.base_path.front() != '/') {
        throw ConfigError("panel.path must start with '/'");
    }
    if (panel.cookie_name.empty() ||
        panel.cookie_name.size() > static_cast<std::size_t>(panel_bounds::kCookieNameMax) ||
        panel.cookie_name.find_first_of(" \t\r\n;=") != std::string::npos) {
        throw ConfigError("panel.cookie_name must be a token of at most " +
                          std::to_string(panel_bounds::kCookieNameMax) + " characters");
    }
    if (panel.absolute_ttl_sec < panel_bounds::kSessionTtlMin ||
        panel.absolute_ttl_sec > panel_bounds::kSessionTtlMax) {
        throw ConfigError("panel.session_absolute_ttl_sec must be " +
                          std::to_string(panel_bounds::kSessionTtlMin) + ".." +
                          std::to_string(panel_bounds::kSessionTtlMax));
    }
    if (panel.idle_ttl_sec < 1 || panel.idle_ttl_sec > panel.absolute_ttl_sec) {
        throw ConfigError("panel.session_idle_ttl_sec must be in 1..session_absolute_ttl_sec");
    }
    if (panel.login_attempts < panel_bounds::kLoginAttemptsMin ||
        panel.login_attempts > panel_bounds::kLoginAttemptsMax) {
        throw ConfigError("panel.login_attempts must be " +
                          std::to_string(panel_bounds::kLoginAttemptsMin) + ".." +
                          std::to_string(panel_bounds::kLoginAttemptsMax));
    }
    for (const auto* value : {&panel.login_window_sec, &panel.login_lockout_sec}) {
        if (*value < panel_bounds::kLoginWindowMin || *value > panel_bounds::kLoginWindowMax) {
            throw ConfigError("panel.login_window_sec and panel.login_lockout_sec must be " +
                              std::to_string(panel_bounds::kLoginWindowMin) + ".." +
                              std::to_string(panel_bounds::kLoginWindowMax));
        }
    }
    for (const auto& net : panel.allowed_networks) {
        if (!cidrValid(net)) {
            throw ConfigError("panel.allowed_networks: invalid CIDR '" + net + "'");
        }
    }
    for (const auto& t : tokens) {
        for (const auto& net : t.allowed_networks) {
            if (!cidrValid(net)) {
                throw ConfigError("consumer '" + t.id + "': invalid CIDR '" + net + "'");
            }
        }
    }
    if (portal.enabled) {
        std::set<std::string> users;
        for (const auto& u : panel.users) {
            if (!users.insert(u.username).second) {
                throw ConfigError("duplicate panel user: " + u.username);
            }
            if (!u.password_hash.empty() && !startsWith(u.password_hash, "$argon2")) {
                throw ConfigError("panel user '" + u.username + "' has unsupported password_hash");
            }
        }
    }
    if (server.remote_https_enabled &&
        (server.tls_cert_file.empty() || server.tls_key_file.empty())) {
        throw ConfigError("proxy.tls requires cert_file and key_file");
    }
    if (server.remote_https_enabled && server.remote_https_listen.empty()) {
        throw ConfigError("proxy.tls.listen is empty");
    }
    if (server.remote_https_enabled &&
        (server.remote_https_port <= 0 || server.remote_https_port > 65535)) {
        throw ConfigError("proxy.tls.listen port is out of range");
    }
    if (server.remote_https_enabled) {
        auto readable = [](const std::string& path) {
            if (!std::filesystem::is_regular_file(path)) return false;
            std::ifstream in(path, std::ios::binary);
            return in.good();
        };
        if (!readable(server.tls_cert_file)) {
            throw ConfigError("TLS cert_file is not readable: " + server.tls_cert_file);
        }
        if (!readable(server.tls_key_file)) {
            throw ConfigError("TLS key_file is not readable: " + server.tls_key_file);
        }
    }
    if (routing.client_provider_policy != "strip" && routing.client_provider_policy != "reject") {
        throw ConfigError("routing.client_provider_policy must be strip or reject");
    }
    if (routing.fallback_attempts < routing_bounds::kFallbackAttemptsMin ||
        routing.fallback_attempts > routing_bounds::kFallbackAttemptsMax) {
        throw ConfigError("routing.fallback_attempts must be " +
                          std::to_string(routing_bounds::kFallbackAttemptsMin) + ".." +
                          std::to_string(routing_bounds::kFallbackAttemptsMax));
    }
    if (routing.provider_order_size < routing_bounds::kProviderOrderMin ||
        routing.provider_order_size > routing_bounds::kProviderOrderMax) {
        throw ConfigError("routing.provider_order_size must be " +
                          std::to_string(routing_bounds::kProviderOrderMin) + ".." +
                          std::to_string(routing_bounds::kProviderOrderMax));
    }
    if (routing.candidate_cap < routing_bounds::kCandidateCapMin ||
        routing.candidate_cap > routing_bounds::kCandidateCapMax) {
        throw ConfigError("routing.candidate_cap must be " +
                          std::to_string(routing_bounds::kCandidateCapMin) + ".." +
                          std::to_string(routing_bounds::kCandidateCapMax));
    }
    if (!(routing.cache_loss_spike.ratio > 1.0)) {
        throw ConfigError("routing.cache_loss_spike.ratio must be greater than 1");
    }
    if (routing.cache_loss_spike.window_sec < 1) {
        throw ConfigError("routing.cache_loss_spike.window_sec must be positive");
    }
    if (!(routing.leader.margin_percent >= 0.0) || routing.leader.margin_percent > 100.0) {
        throw ConfigError("routing.leader.margin_percent must be in [0, 100]");
    }
    if (routing.leader.min_dwell_sec < 0) {
        throw ConfigError("routing.leader.min_dwell_sec must be >= 0");
    }
    if (routing.leader.confirm_builds < 1) {
        throw ConfigError("routing.leader.confirm_builds must be positive");
    }
    if (routing.session_pin_sec < routing_bounds::kSessionPinSecMin ||
        routing.session_pin_sec > routing_bounds::kSessionPinSecMax) {
        throw ConfigError("routing.session_pin_sec must be " +
                          std::to_string(routing_bounds::kSessionPinSecMin) + ".." +
                          std::to_string(routing_bounds::kSessionPinSecMax));
    }
    if (routing.session_pin_max < routing_bounds::kSessionPinMaxMin ||
        routing.session_pin_max > routing_bounds::kSessionPinMaxMax) {
        throw ConfigError("routing.session_pin_max must be " +
                          std::to_string(routing_bounds::kSessionPinMaxMin) + ".." +
                          std::to_string(routing_bounds::kSessionPinMaxMax));
    }
    if (routing.queue_min_sec < routing_bounds::kQueueSecMin ||
        routing.queue_min_sec > routing.queue_max_sec) {
        throw ConfigError("routing.queue_min_sec must be 1..queue_max_sec");
    }
    if (routing.queue_max_sec > routing_bounds::kQueueSecMax) {
        throw ConfigError("routing.queue_max_sec must be at most " +
                          std::to_string(routing_bounds::kQueueSecMax));
    }
    if (routing.queue_stats_sec < routing_bounds::kQueueSecMin ||
        routing.queue_stats_sec > routing_bounds::kQueueSecMax) {
        throw ConfigError("routing.queue_stats_sec must be " +
                          std::to_string(routing_bounds::kQueueSecMin) + ".." +
                          std::to_string(routing_bounds::kQueueSecMax));
    }
    if (!(routing.wait_price_rub_per_sec >= routing_bounds::kWaitPriceMin) ||
        !(routing.wait_price_rub_per_sec <= routing_bounds::kWaitPriceMax)) {
        throw ConfigError("routing.wait_price_rub_per_sec must be in [" +
                          std::to_string(routing_bounds::kWaitPriceMin) + ", " +
                          std::to_string(routing_bounds::kWaitPriceMax) + "]");
    }
    if (routing.failover_wait_max_sec < 0 ||
        routing.failover_wait_max_sec > routing_bounds::kFailoverWaitMaxSec) {
        throw ConfigError("routing.failover_wait_max_sec must be 0.." +
                          std::to_string(routing_bounds::kFailoverWaitMaxSec));
    }
    if (!(routing.output_weight >= routing_bounds::kWeightMin) ||
        !(routing.output_weight <= routing_bounds::kWeightMax)) {
        throw ConfigError("routing.output_weight must be in [" +
                          std::to_string(routing_bounds::kWeightMin) + ", " +
                          std::to_string(routing_bounds::kWeightMax) + "]");
    }
    if (!(routing.cache_weight >= routing_bounds::kWeightMin) ||
        !(routing.cache_weight <= routing_bounds::kWeightMax)) {
        throw ConfigError("routing.cache_weight must be in [" +
                          std::to_string(routing_bounds::kWeightMin) + ", " +
                          std::to_string(routing_bounds::kWeightMax) + "]");
    }
    if (routing.profile_sample_size < routing_bounds::kProfileSampleMin ||
        routing.profile_sample_size > routing_bounds::kProfileSampleMax) {
        throw ConfigError("routing.profile_sample_size must be " +
                          std::to_string(routing_bounds::kProfileSampleMin) + ".." +
                          std::to_string(routing_bounds::kProfileSampleMax));
    }
    if (routing.cache_miss.warn < routing_bounds::kCacheMissStreakMin ||
        routing.cache_miss.warn > routing_bounds::kCacheMissStreakMax ||
        routing.cache_miss.blacklist < routing.cache_miss.warn ||
        routing.cache_miss.blacklist > routing_bounds::kCacheMissStreakMax) {
        throw ConfigError("routing.cache_miss must be warn..blacklist within " +
                          std::to_string(routing_bounds::kCacheMissStreakMin) + ".." +
                          std::to_string(routing_bounds::kCacheMissStreakMax) +
                          " and warn <= blacklist");
    }
    if (routing.retry.attempts < 0 ||
        routing.retry.attempts > routing_bounds::kRetryAttemptsMax) {
        throw ConfigError("routing.retry.attempts must be 0.." +
                          std::to_string(routing_bounds::kRetryAttemptsMax));
    }
    if (routing.retry.backoff_ms < 0 ||
        routing.retry.backoff_ms > routing_bounds::kRetryBackoffMaxMs) {
        throw ConfigError("routing.retry.backoff_ms must be 0.." +
                          std::to_string(routing_bounds::kRetryBackoffMaxMs));
    }
    if (routing.health.fail_threshold < 0 ||
        routing.health.fail_threshold > routing_bounds::kHealthFailThresholdMax) {
        throw ConfigError("routing.health_fail_threshold must be 0.." +
                          std::to_string(routing_bounds::kHealthFailThresholdMax));
    }
    if (routing.health.down_after_sec < 0 ||
        routing.health.down_after_sec > routing_bounds::kHealthDownAfterSecMax) {
        throw ConfigError("routing.health_down_after_sec must be 0.." +
                          std::to_string(routing_bounds::kHealthDownAfterSecMax));
    }
    if (routing.health.probe_interval_sec < routing_bounds::kHealthProbeSecMin ||
        routing.health.probe_interval_sec > routing_bounds::kHealthProbeSecMax) {
        throw ConfigError("routing.health_probe_interval_sec must be " +
                          std::to_string(routing_bounds::kHealthProbeSecMin) + ".." +
                          std::to_string(routing_bounds::kHealthProbeSecMax));
    }
    if (routing.health.recover_successes < routing_bounds::kHealthRecoverMin ||
        routing.health.recover_successes > routing_bounds::kHealthRecoverMax) {
        throw ConfigError("routing.health_recover_successes must be " +
                          std::to_string(routing_bounds::kHealthRecoverMin) + ".." +
                          std::to_string(routing_bounds::kHealthRecoverMax));
    }
    for (const auto& b : routing.provider_blacklist) {
        if (b.provider.empty()) throw ConfigError("provider_blacklist entry has empty provider");
        if (b.models.empty()) throw ConfigError("provider_blacklist entry has empty models");
    }
    for (const auto& b : routing.provider_whitelist) {
        if (b.provider.empty()) throw ConfigError("provider_whitelist entry has empty provider");
        if (b.models.empty()) throw ConfigError("provider_whitelist entry has empty models");
    }
    if (pricing.threshold_mode != "replace" && pricing.threshold_mode != "excess") {
        throw ConfigError("pricing.threshold_mode must be replace or excess");
    }
    if (pricing.stale_after_sec <= 0 || pricing.max_stale_sec < pricing.stale_after_sec) {
        throw ConfigError("pricing freshness requires 0 < stale_after <= max_stale");
    }
    if (pricing.max_body_bytes == 0) throw ConfigError("pricing.max_body_bytes must be positive");
    if (pricing.timeout_ms <= 0) throw ConfigError("pricing.timeout must be positive");
    if (pricing.refresh.hot_interval_sec < 1) {
        throw ConfigError("pricing.refresh.hot_interval must be positive");
    }
    if (pricing.refresh.max_endpoints_per_model < 1) {
        throw ConfigError("pricing.refresh.max_endpoints_per_model must be positive");
    }
    if (stats.aggregates_retention_days < stats.requests_retention_days) {
        throw ConfigError(
            "stats.aggregates_retention_days must be at least stats.requests_retention_days");
    }
    if (capture.enabled &&
        (capture.max_request_bytes == 0 || capture.max_response_bytes == 0 ||
         capture.max_stream_bytes == 0 || capture.max_total_bytes == 0)) {
        throw ConfigError("capture limits must be positive when capture is enabled");
    }
    if (capture.enabled && capture.encryption_key.empty()) {
        throw ConfigError("capture.enabled requires capture.encryption_key");
    }
    if (capture.compression != "zstd" && capture.compression != "gzip" && capture.compression != "none") {
        throw ConfigError("capture.compression must be zstd, gzip or none");
    }
    if (capture.retention_days < 0) throw ConfigError("capture.retention_days must be >= 0");
    if (stats.ring_size <= 0) throw ConfigError("stats.ring_buffer_size must be positive");
    if (stats.requests_retention_days < 0 || stats.aggregates_retention_days < 0) {
        throw ConfigError("stats retention must be >= 0");
    }
    if (analytics.max_samples_per_group < 1 || analytics.max_file_bytes == 0) {
        throw ConfigError("analytics limits must be positive");
    }
    if (analytics.enabled && analytics.provider_log_dir.empty()) {
        throw ConfigError("analytics.provider_log_dir must not be empty");
    }
    if (capture.sample_rate < 0.0 || capture.sample_rate > 1.0) {
        throw ConfigError("capture.sample_rate must be between 0 and 1");
    }
    std::set<std::string> model_set(routing.models.begin(), routing.models.end());
    if (model_set.size() != routing.models.size()) {
        throw ConfigError("routing.models contains duplicates");
    }
    std::set<std::string> token_ids;
    for (const auto& t : tokens) {
        if (t.id.empty()) throw ConfigError("token id is empty");
        if (!token_ids.insert(t.id).second) throw ConfigError("duplicate token id: " + t.id);
        if (t.token_hash.empty()) {
            throw ConfigError("token '" + t.id + "' has no token_hash");
        }
        if (!isSha256Hash(t.token_hash)) {
            throw ConfigError("token '" + t.id +
                              "' must be sha256:<64-hex>, 64-hex, or file:/env: "
                              "containing a hash or raw token");
        }
        if (t.rpm < 0 || t.tpm < 0 || t.max_concurrent < 0) {
            throw ConfigError("token '" + t.id + "' limits must be non-negative");
        }
        if (t.day_budget_rub < 0) {
            throw ConfigError("token '" + t.id + "' budget must be non-negative");
        }
        std::chrono::system_clock::time_point expires;
        if (!t.expires_at.empty() && !parseRfc3339(t.expires_at, expires)) {
            throw ConfigError("token '" + t.id + "' expires_at must be RFC3339");
        }
    }
    std::set<std::string> direct_tags;
    for (const auto& r : routers) {
        if (r.price_multiplier < 0) throw ConfigError("upstream '" + r.id + "' price_multiplier < 0");
        if (r.fixed_fee_nano < 0) throw ConfigError("upstream '" + r.id + "' fixed_fee_rub < 0");
        if (r.kind != "routerai" && r.kind != "openai_compatible") {
            throw ConfigError("upstream '" + r.id + "' kind must be routerai or openai_compatible");
        }
        if (r.id.find_first_of("\r\n") != std::string::npos) {
            throw ConfigError("upstream id contains CR/LF");
        }
        if (r.provider_tag.find_first_of("\r\n") != std::string::npos) {
            throw ConfigError("upstream '" + r.id + "' provider_tag contains CR/LF");
        }
        if (r.api_key.find_first_of("\r\n") != std::string::npos) {
            throw ConfigError("upstream '" + r.id + "' api_key contains CR/LF");
        }
        if (r.auth_header.empty() || r.auth_header.find_first_of(":\r\n") != std::string::npos) {
            throw ConfigError("upstream '" + r.id + "' has invalid auth_header");
        }
        if (r.auth_scheme.find_first_of("\r\n") != std::string::npos) {
            throw ConfigError("upstream '" + r.id + "' has invalid auth_scheme");
        }
        if (!std::isfinite(static_cast<double>(r.prompt_per_million_rub)) ||
            !std::isfinite(static_cast<double>(r.completion_per_million_rub)) ||
            !std::isfinite(static_cast<double>(r.cache_read_per_million_rub)) ||
            r.prompt_per_million_rub < 0 || r.completion_per_million_rub < 0 ||
            r.cache_read_per_million_rub < 0) {
            throw ConfigError("upstream '" + r.id + "' price_per_million must be non-negative");
        }
        if (r.cache_size_tokens < 0) {
            throw ConfigError("upstream '" + r.id + "' cache_size_tokens is invalid");
        }
        if (r.context_length < 0 || r.max_completion_tokens < 0) {
            throw ConfigError("upstream '" + r.id + "' context/limit must be non-negative");
        }
        for (const auto& [from, to] : r.model_map) {
            if (from.empty() || to.empty()) {
                throw ConfigError("upstream '" + r.id + "' model_map has empty entry");
            }
        }
        if (r.kind == "openai_compatible") {
            const std::string tag = providerTagOf(r);
            if (!direct_tags.insert(tag).second) {
                throw ConfigError("duplicate direct provider_tag: " + tag);
            }
        }
    }
}

std::string AppConfig::resolveModel(const std::string& requested) const {
    auto it = routing.aliases.find(requested);
    if (it != routing.aliases.end()) return it->second;
    return requested;
}

const RouterConfig* AppConfig::findRouter(const std::string& id) const {
    for (const auto& r : routers) if (r.id == id) return &r;
    return nullptr;
}

const RouterConfig* AppConfig::priceSourceRouter() const {
    for (const auto& r : routers) {
        if (r.kind == "routerai") return &r;
    }
    return nullptr;
}

bool AppConfig::modelConfigured(const std::string& model) const {
    return std::find(routing.models.begin(), routing.models.end(), model) != routing.models.end();
}

bool portalRoleAllowed(const PortalAppConfig& app, const std::string& role) {
    return std::find(app.roles.begin(), app.roles.end(), role) != app.roles.end();
}

bool routerServesModel(const RouterConfig& router, std::string_view model) {
    for (const auto& pattern : router.models) {
        if (pattern == "*" || globMatch(pattern, model)) return true;
    }
    return false;
}

bool routerVisible(const AppConfig& config, const RouterConfig& router) {
    if (config.routing.provider_whitelist.empty()) return true;
    const std::string tag = providerTagOf(router);
    for (const auto& model : config.routing.models) {
        if (routerServesModel(router, model) &&
            providerAllowedByList(config.routing.provider_whitelist, model, tag)) {
            return true;
        }
    }
    return false;
}

bool providerAllowedByList(const std::vector<ProviderBlockConfig>& list,
                           std::string_view model, std::string_view provider) {
    if (list.empty()) return true;
    const std::string tag = toLower(provider);
    for (const auto& entry : list) {
        if (!globMatch(toLower(entry.provider), tag)) continue;
        for (const auto& pattern : entry.models) {
            if (pattern == "*" || globMatch(pattern, model)) return true;
        }
    }
    return false;
}

namespace {

std::string yamlScalar(const std::string& value) {
    return YAML::Dump(YAML::Node(value));
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    lines.push_back(line);
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out.push_back('\n');
    }
    return out;
}

std::string blockYaml(const std::string& key, const std::vector<ProviderBlockConfig>& entries) {
    if (entries.empty()) return key + ": []\n";
    std::ostringstream out;
    out << key << ":\n";
    for (const auto& b : entries) {
        out << "  - provider: " << yamlScalar(b.provider) << "\n";
        out << "    models:\n";
        for (const auto& model : b.models) {
            out << "      - " << yamlScalar(model) << "\n";
        }
        if (!b.reason.empty()) out << "    reason: " << yamlScalar(b.reason) << "\n";
    }
    return out.str();
}

void replaceTopLevelBlock(std::string& content, const std::string& key,
                          const std::string& block) {
    auto lines = splitLines(content);
    std::size_t start = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty() && lines[i][0] != ' ' && lines[i][0] != '\t' &&
            lines[i].rfind(key + ":", 0) == 0) {
            start = i;
            break;
        }
    }
    if (start == lines.size()) {
        if (!content.empty() && content.back() != '\n') content += '\n';
        content += block;
        return;
    }
    std::size_t end = start + 1;
    while (end < lines.size()) {
        const std::string& line = lines[end];
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') break;
        ++end;
    }
    std::vector<std::string> replacement = splitLines(block);
    if (!replacement.empty() && replacement.back().empty()) replacement.pop_back();
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start),
                lines.begin() + static_cast<std::ptrdiff_t>(end));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start),
                 replacement.begin(), replacement.end());
    content = joinLines(lines);
}

// Комментарий в конце строки (` #…`) — это документация к ключу, он переживает
// перезапись значения вместе с отступом до него (выравнивание колонок в файле).
std::string trailingComment(const std::string& line) {
    const auto hash = line.find('#');
    if (hash == std::string::npos) return {};
    std::size_t start = hash;
    while (start > 0 && line[start - 1] == ' ') --start;
    if (start == hash) return {}; // нет пробела перед `#` — по YAML это не комментарий
    return line.substr(start);
}

// Вес в YAML: без хвостовых нулей, но всегда как число с точкой (1.0, 1.5, 0.25).
std::string formatRoutingNumber(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << value;
    std::string text = out.str();
    if (text.find('.') != std::string::npos) {
        while (text.size() > 1 && text.back() == '0') text.pop_back();
        if (text.back() == '.') text += "0";
    }
    return text;
}

// Sets `routing.<key>` to a scalar, replacing the existing entry or inserting
// it as the first key of the routing block.
void setRoutingEntry(std::string& content, const std::string& key, const std::string& value) {
    const std::string entry = key + ": " + value;
    auto lines = splitLines(content);
    std::size_t start = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty() && lines[i][0] != ' ' && lines[i][0] != '\t' &&
            lines[i].rfind("routing:", 0) == 0) {
            start = i;
            break;
        }
    }
    if (start == lines.size()) {
        if (!content.empty() && content.back() != '\n') content += '\n';
        content += "routing:\n  " + entry + "\n";
        return;
    }
    std::size_t end = start + 1;
    while (end < lines.size() &&
           (lines[end].empty() || lines[end][0] == ' ' || lines[end][0] == '\t')) {
        ++end;
    }
    std::string indent = "  ";
    if (start + 1 < end) {
        const auto pos = lines[start + 1].find_first_not_of(" \t");
        if (pos != std::string::npos) indent = lines[start + 1].substr(0, pos);
    }
    for (std::size_t i = start + 1; i < end; ++i) {
        // Ключ блока ищется только на его уровне: вложенные блоки (leader, retry,
        // cache_loss_spike, cache_miss) не подменяются одноимённым ключом.
        if (lines[i].compare(0, indent.size(), indent) != 0) continue;
        const auto rest = lines[i].substr(indent.size());
        if (rest.empty() || rest.front() == ' ' || rest.front() == '\t') continue;
        const std::string trimmed = trim(lines[i]);
        if (trimmed.rfind(key + ":", 0) == 0) {
            lines[i] = indent + entry + trailingComment(lines[i]);
            content = joinLines(lines);
            return;
        }
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start) + 1, indent + entry);
    content = joinLines(lines);
}

void writeConfigAtomically(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw ConfigError("cannot write config: " + tmp);
        out << content;
        if (!out.good()) throw ConfigError("cannot write config: " + tmp);
    }
    try {
        YAML::LoadFile(tmp);
    } catch (const std::exception& e) {
        std::filesystem::remove(tmp);
        throw ConfigError("generated config is invalid: " + std::string(e.what()));
    }
    std::error_code ec;
    const auto perms = std::filesystem::status(path, ec).permissions();
    if (!ec) std::filesystem::permissions(tmp, perms, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        throw ConfigError("cannot replace config: " + ec.message());
    }
}

// Перезаписывает один скаляр блока routing, сохраняя остальные строки и
// комментарии конфигурации.
void saveRoutingEntry(const std::string& path, const std::string& key,
                      const std::string& value) {
    if (path.empty()) throw ConfigError("config path is empty");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigError("cannot read config: " + path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    setRoutingEntry(content, key, value);
    writeConfigAtomically(path, content);
}

} // namespace

void saveProviderBlockList(const std::string& path, const std::string& key,
                           const std::vector<ProviderBlockConfig>& entries) {
    if (path.empty()) throw ConfigError("config path is empty");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigError("cannot read config: " + path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    replaceTopLevelBlock(content, key, blockYaml(key, entries));
    writeConfigAtomically(path, content);
}

void saveRoutingBool(const std::string& path, const std::string& key, bool value) {
    saveRoutingEntry(path, key, value ? "true" : "false");
}

void saveRoutingNumber(const std::string& path, const std::string& key, double value) {
    if (!std::isfinite(value)) throw ConfigError(std::string(key) + " must be a finite number");
    saveRoutingEntry(path, key, formatRoutingNumber(value));
}

void saveRoutingInt(const std::string& path, const std::string& key, std::int64_t value) {
    saveRoutingEntry(path, key, std::to_string(value));
}

} // namespace proxy
