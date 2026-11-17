#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace proxy {

struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct PanelUserConfig {
    std::string username;
    std::string password_hash;   // Argon2id-хеш или ссылка env:/file:
    std::string role = "viewer";
};

struct TokenConfig {
    std::string id;
    std::string token_hash;      // sha256:<hex>, 64-hex, or env:/file: reference
    std::vector<std::string> scopes{"proxy:invoke"};
    std::vector<std::string> models{"*"};
    int rpm = 0;                 // 0 = unlimited
    int tpm = 0;
    int max_concurrent = 0;
    std::string expires_at;      // RFC3339 or empty
    std::vector<std::string> allowed_networks;
    long double day_budget_rub = 0; // 0 = unlimited
};

struct TimeoutConfig {
    int connect_ms = 5000;
    int total_ms = 900000;
};

// Предел параллельных запросов к апстриму по умолчанию: сетевая модель
// обслуживает много клиентов, локальная — одна машина.
constexpr int kDefaultRemoteConcurrency = 32;
constexpr int kDefaultLocalConcurrency = 2;

struct RouterConfig {
    std::string id;
    std::string kind = "routerai";
    bool enabled = true;
    std::string base_url;
    std::string api_key;
    std::vector<std::string> models{"*"};
    long double price_multiplier = 1.0L;
    std::int64_t fixed_fee_nano = 0;
    // Локальные OpenAI-compatible апстримы (llama.cpp/Ollama/LM Studio/vLLM):
    // не получают объект `provider` и могут переименовать модель.
    std::string provider_tag;
    std::map<std::string, std::string> model_map;
    long double prompt_per_million_rub = 0;
    long double completion_per_million_rub = 0;
    long double cache_read_per_million_rub = 0;
    int context_length = 0;
    int cache_size_tokens = 0;
    int max_completion_tokens = 0;
    std::string auth_header = "Authorization";
    std::string auth_scheme = "Bearer";
    bool http2 = true;
    bool verify_tls = true;
    std::string ca_file;
    std::string client_cert_file;
    std::string client_key_file;
    int max_concurrent = kDefaultRemoteConcurrency;
    TimeoutConfig timeouts;
};

// Тег провайдера роутера: явный provider_tag, иначе id. Единственное правило
// имени — здесь; Router, панель и API вызывают его.
inline std::string providerTagOf(const RouterConfig& router) {
    return router.provider_tag.empty() ? router.id : router.provider_tag;
}

struct PricingRefreshConfig {
    int hot_interval_sec = 300;
    int max_endpoints_per_model = 500;
};

struct PricingConfig {
    PricingRefreshConfig refresh;
    int stale_after_sec = 900;
    int max_stale_sec = 3600;
    bool require_fresh = false;
    int timeout_ms = 5000;
    std::size_t max_body_bytes = 2 * 1024 * 1024;
    std::string snapshot_file;
    std::string threshold_mode = "replace";
};

struct ProviderBlockConfig {
    std::string provider; // glob, case-insensitive
    std::vector<std::string> models{"*"};
    std::string reason;
};

// Причина блокировки по умолчанию: её же подставляет API, если поле не задано.
constexpr const char* kDefaultBlockReason = "blocked by policy";

// Признак инцидента «смена провайдера сбросила кеш»: текущий запрос серии дороже
// предыдущего в ratio раз при нулевом попадании в кеш. Абсолютных цен в предикате
// нет: предыдущая стоимость должна быть просто больше нуля.
struct CacheLossSpikeConfig {
    double ratio = 3.0;
    int window_sec = 900; // разрыв больше — не одна серия
};

// Петля гистерезиса приоритетного провайдера: лидер модели не меняется на
// минимальных различиях и не дёргается на каждом перестроении очереди.
struct LeaderConfig {
    double margin_percent = 15.0; // относительный выигрыш challenger'а, %
    int min_dwell_sec = 1800;     // минимальное удержание лидера, сек
    int confirm_builds = 2;       // перестроений подряд у challenger'а
    std::string state_file;       // пусто = <data_dir>/routing-leader.json
};

// Серия успешных запросов подряд без попаданий в кеш: со `warn` — предупреждение
// в журнале, с `blacklist` — пара (провайдер, модель) уходит в чёрный список.
struct CacheMissConfig {
    int warn = 2;
    int blacklist = 5;
};

// Повтор запроса у того же провайдера после отклоняемой ошибки: сеть, таймаут,
// 429, 5xx. Тело запроса остаётся в памяти процесса и на диск не пишется.
struct RetryConfig {
    int attempts = 1;     // повторов после первой попытки; 0 — выключено
    int backoff_ms = 500; // пауза между повторами
};

// Правило работы с неработающим провайдером: серия отказов (включая пустой
// ответ) уводит его из маршрута; живые запросы сразу идут к следующему по
// очереди, а недоступный провайдер периодически проверяется и возвращается.
struct ProviderHealthConfig {
    int fail_threshold = 3;      // отказов подряд до ухода из маршрута; 0 — выключено
    int down_after_sec = 60;     // серия отказов дольше, сек; 0 — выключено
    int probe_interval_sec = 60; // период проверки доступности, сек
    int recover_successes = 1;   // успешных проверок подряд для возврата
};

struct RoutingConfig {
    std::vector<std::string> models;
    std::map<std::string, std::string> aliases;
    std::string default_model;
    int fallback_attempts = 2;
    int candidate_cap = 32;
    std::string client_provider_policy = "strip";
    std::string country;
    std::vector<std::string> quantization;
    bool inject_provider_options = true; // добавлять объект provider в запрос
    // Единственная очередь провайдеров на модель: столько верхних позиций
    // уходит в запрос (provider.order). Один провайдер — внешний обработчик не
    // может сменить провайдера внутри запроса.
    int provider_order_size = 1;
    // Перестроение очереди: тик случайной длины [min; max] секунд; изменение
    // цен перестраивает очередь на ближайшем тике, изменение статистики — не
    // чаще, чем раз в queue_stats_sec.
    int queue_min_sec = 600;
    int queue_max_sec = 900;
    int queue_stats_sec = 3600;
    // Обязательная запись инцидента потери кеша при смене провайдера.
    CacheLossSpikeConfig cache_loss_spike;
    // Серия промахов кеша: предупреждение и автоматический чёрный список.
    CacheMissConfig cache_miss;
    // Гистерезис лидера: удержание провайдера, который обслуживал серию.
    LeaderConfig leader;
    // Привязка (сессия, модель) к провайдеру: кеш живёт в беседе, поэтому
    // провайдер обязан быть стабильным на время сессии. 0 — привязка выключена.
    int session_pin_sec = 1800;
    int session_pin_max = 4096;
    // Цена секунды ожидания клиента, RUB/с. Входит в вес RUB/M каждого оффера
    // (стоимость токенов + цена наблюдаемой задержки) и задаёт, сколько выгодно
    // ещё ждать текущего провайдера, прежде чем уйти к следующему. 0 — время
    // в весе и в решении о смене не учитывается.
    double wait_price_rub_per_sec = 0.001;
    // Предел ожидания текущего провайдера перед fallback, сек; 0 — переходить
    // сразу после бюджета попыток `routing.retry.attempts`.
    int failover_wait_max_sec = 60;
    // Поправочные веса при ранжировании (weight RUB/M): 1 — элемент счёта входит
    // как в формуле стоимости, больше — сильнее влияет на порядок очереди.
    // Выход обычно дороже и меньше по объёму, кеш — основной элемент счёта.
    double output_weight = 1.0;
    double cache_weight = 2.0;
    RetryConfig retry;
    ProviderHealthConfig health;
    // Профиль токенов модели для весов: 0 — средняя за всё время процесса,
    // N — последние N успешных запросов модели (скользящее окно).
    int profile_sample_size = 30;
    std::vector<ProviderBlockConfig> provider_blacklist;
    std::vector<ProviderBlockConfig> provider_whitelist;
};

// Границы числовых настроек маршрутизации: один источник для AppConfig::validate,
// API панели и полей ввода. Панель и API не вводят собственных диапазонов.
namespace routing_bounds {
constexpr int kFallbackAttemptsMin = 0;
constexpr int kFallbackAttemptsMax = 10;
constexpr int kProviderOrderMin = 1;
constexpr int kProviderOrderMax = 32;
constexpr int kCandidateCapMin = 1;
constexpr int kCandidateCapMax = 256;
constexpr int kProfileSampleMin = 0;
constexpr int kProfileSampleMax = 100000;
constexpr int kSessionPinSecMin = 0;
constexpr int kSessionPinSecMax = 604800;
constexpr int kSessionPinMaxMin = 1;
constexpr int kSessionPinMaxMax = 1000000;
constexpr int kFailoverWaitMinSec = 0;
constexpr int kFailoverWaitMaxSec = 86400;
constexpr int kQueueSecMin = 1;
constexpr int kQueueSecMax = 86400;
constexpr int kHealthFailThresholdMax = 1000;
constexpr int kHealthDownAfterSecMax = 86400;
constexpr int kHealthProbeSecMin = 1;
constexpr int kHealthProbeSecMax = 86400;
constexpr int kHealthRecoverMin = 1;
constexpr int kHealthRecoverMax = 100;
constexpr int kRetryAttemptsMax = 10;
constexpr int kRetryBackoffMaxMs = 60000;
constexpr double kWeightMin = 0.0;
constexpr double kWeightMax = 100.0;
constexpr double kWaitPriceMin = 0.0;
constexpr double kWaitPriceMax = 100.0;
constexpr int kCacheMissStreakMin = 1;
constexpr int kCacheMissStreakMax = 1000;
} // namespace routing_bounds

// Границы настроек панели: один источник для validate и полей ввода.
namespace panel_bounds {
constexpr int kSessionTtlMin = 60;
constexpr int kSessionTtlMax = 30 * 24 * 3600;
constexpr int kLoginAttemptsMin = 1;
constexpr int kLoginAttemptsMax = 1000;
constexpr int kLoginWindowMin = 1;
constexpr int kLoginWindowMax = 86400;
constexpr int kCookieNameMax = 64;
} // namespace panel_bounds

struct CaptureConfig {
    // Блок не задан — выключено; заданный блок включает захват, если не сказано
    // обратное (та же схема у analytics и portal).
    bool enabled = false;
    std::string store_dir;
    std::string encryption_key;
    std::string compression = "gzip";
    int retention_days = 7;
    std::int64_t max_total_bytes = 50LL * 1024 * 1024 * 1024;
    std::size_t max_request_bytes = 8 * 1024 * 1024;
    std::size_t max_response_bytes = 32 * 1024 * 1024;
    std::size_t max_stream_bytes = 8 * 1024 * 1024;
    bool capture_stream = true;
    bool capture_errors = true;
    double sample_rate = 1.0;
};

struct StatsConfig {
    bool enabled = true;
    std::string sqlite_path;      // empty -> <data_dir>/stats.db
    int ring_size = 1000;
    int requests_retention_days = 7;
    int aggregates_retention_days = 90;
    bool prometheus = true;
    bool require_token = false;
};

struct ServerConfig {
    std::string listen = "127.0.0.1";
    int port = 8080;
    int threads = 2;
    std::string public_url;      // external base URL for panel and agents; "/v1" is appended
    bool require_https = false;  // reject plaintext /v1; defaults to true when TLS is set
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::string tls_cert_file;
    std::string tls_key_file;
    bool remote_https_enabled = false;
    std::string remote_https_listen = "0.0.0.0";
    int remote_https_port = 8443;
};

struct PanelConfig {
    std::string base_path = "/panel";
    std::vector<std::string> allowed_networks{"127.0.0.1/32", "::1/128"};
    std::vector<PanelUserConfig> users;
    int absolute_ttl_sec = 12 * 3600;
    int idle_ttl_sec = 1800;
    std::string cookie_name = "proxy_session";
    bool secure_cookie = false;
    int login_attempts = 5;
    int login_window_sec = 900;
    int login_lockout_sec = 900;
};

struct AnalyticsConfig {
    bool enabled = false;   // включён заданным блоком `analytics:`, см. CaptureConfig
    std::string provider_log_dir = "./data";
    std::string snapshot_file; // empty = no persistence of aggregates
    bool auto_import = true;
    std::size_t max_file_bytes = 64 * 1024 * 1024;
    int max_samples_per_group = 20000;
};

struct PortalAppConfig {
    std::string id;
    std::string title;
    std::string url;             // локальный HTTP(S) апстрим, без учётных данных
    std::vector<std::string> roles{"admin", "viewer"};
    std::map<std::string, std::string> headers; // optional, env:/file: allowed
    bool verify_tls = true;
    int timeout_ms = 60000;
    std::size_t max_response_bytes = 32 * 1024 * 1024;
};

struct PortalConfig {
    bool enabled = false;    // включён заданным блоком `portal:`, см. CaptureConfig
    std::vector<PortalAppConfig> apps;
};

struct AppConfig {
    std::string instance_name;
    std::string data_dir = "./state";
    std::string config_path; // файл, который перезаписывает панель
    ServerConfig server;
    PanelConfig panel;
    std::vector<TokenConfig> tokens;
    PricingConfig pricing;
    RoutingConfig routing;
    std::vector<RouterConfig> routers;
    CaptureConfig capture;
    StatsConfig stats;
    AnalyticsConfig analytics;
    PortalConfig portal;

    static AppConfig load(const std::string& path);
    void validate() const;

    std::string resolveModel(const std::string& requested) const;
    const RouterConfig* findRouter(const std::string& id) const;
    // Единственный источник цен — первый RouterAI-апстрим: его адрес отдаёт прайс.
    const RouterConfig* priceSourceRouter() const;
    bool modelConfigured(const std::string& model) const;
};

bool routerServesModel(const RouterConfig& router, std::string_view model);
// Разрешена ли роль в приложении портала: одно правило для HTTP, WebSocket и
// меню панели.
bool portalRoleAllowed(const PortalAppConfig& app, const std::string& role);
bool providerAllowedByList(const std::vector<ProviderBlockConfig>& list,
                           std::string_view model, std::string_view provider);
// Виден ли апстрим в панели при текущем whitelist провайдеров.
bool routerVisible(const AppConfig& config, const RouterConfig& router);
void saveProviderBlockList(const std::string& path, const std::string& key,
                           const std::vector<ProviderBlockConfig>& entries);
// Persists `routing.<key>: <value>` into the configuration file, keeping the
// comments of the other lines. saveRoutingNumber writes the value as a plain
// YAML float (1.0, 1.5, 0.25), saveRoutingInt — как целое.
void saveRoutingBool(const std::string& path, const std::string& key, bool value);
void saveRoutingNumber(const std::string& path, const std::string& key, double value);
void saveRoutingInt(const std::string& path, const std::string& key, std::int64_t value);

} // namespace proxy
