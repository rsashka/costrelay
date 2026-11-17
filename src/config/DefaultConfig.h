#pragma once

#include <string_view>

namespace proxy {

// Шаблон конфигурации: единственная документация параметров (`--init-config`).
inline constexpr std::string_view kDefaultConfig = R"CONFIG(
# Конфигурация CostRelay. Создана командой: costrelayd --init-config
# Секреты: только file:/путь или env:ИМЯ, пароли и токены в файле не хранятся.
# Проверка: costrelayd --config config/costrelay.yaml --check
# Длительности: "5s", "10m", "2h", "1d"; без суффикса — секунды.
# Размеры: число байт или суффикс k/kib, m/mib, g/gib (двоичные).

proxy:
  name: "costrelay"
  data_dir: "./state"               # относительно каталога этого файла; статистика, цены, тела
  listen: "127.0.0.1:8080"          # host:port; локальные агенты ходят по HTTP
  threads: 2                        # потоки ввода-вывода Drogon
  # max_body_bytes: 8m              # предел тела запроса клиента, байт; суффиксы k/kib, m/mib, g/gib (двоичные)
  # public_url: "https://llm.example.com"   # адрес в локальной сети для панели и агентов, /v1 добавляется
  # require_https: true                     # по умолчанию включён вместе с tls: сетевой /v1 только по HTTPS
  # TLS — листенер для локальной сети; loopback-клиенты остаются на HTTP:
  # tls:
  #   listen: "0.0.0.0:8443"
  #   cert_file: "/etc/costrelay/tls/fullchain.pem"
  #   key_file: "/etc/costrelay/tls/privkey.pem"

panel:
  path: "/panel"                    # вход, API и страницы; сама панель на /
  # allowed_networks: ["10.0.0.0/8"]   # по умолчанию: loopback для loopback-листенера, любые сети для сетевого; список обязателен и проверяется как CIDR
  # session_absolute_ttl_sec: 43200  # срок жизни сессии панели, сек (60..2592000)
  # session_idle_ttl_sec: 1800       # простой, после которого сессия истекает, сек (1..session_absolute_ttl_sec)
  # cookie_name: "proxy_session"     # имя cookie панели, безопасный токен (до 64 символов)
  # login_attempts: 5                # неудачных входов до блокировки (1..1000)
  # login_window_sec: 900            # окно подсчёта попыток, сек (1..86400)
  # login_lockout_sec: 900           # блокировка после исчерпания попыток, сек (1..86400)

portal:
  enabled: true                     # false = только API, без панели и портала
  users:
    - username: "admin"
      password_hash: "file:/run/secrets/panel_admin_hash"   # создать: --hash-password
      role: "admin"                 # admin | viewer
  services:                         # локальные HTTP-сервисы, ссылки в верхнем меню на /<id>/
    - id: "grafana"
      title: "Grafana"
      url: "http://127.0.0.1:3000" # без user:pass, query и fragment
      roles: ["admin"]
      # verify_tls: true
      # timeout: "60s"
      # max_response_bytes: 33554432
      # headers: { "X-Api-Key": "file:/run/secrets/grafana_key" }
      # headers: { "Authorization": "Bearer file:/run/secrets/strata_token" }

models:                             # рабочие модели: маршрутизация и выбор цен
  - "deepseek/deepseek-v4.1-flash"
default_model: "deepseek/deepseek-v4.1-flash"
# aliases: { "default": "deepseek/deepseek-v4.1-flash" }

providers:
  external:                         # RouterAI или удалённый OpenAI-compatible
    - id: "routerai"
      type: "routerai"              # routerai | openai
      url: "https://routerai.ru/api/v1"
      api_key: "env:ROUTERAI_API_KEY"   # только file:/путь или env:ИМЯ
      # enabled: true               # false — апстрим не участвует в маршрутизации
      # models: ["*"]               # список glob или карта { публичное: у провайдера }
      # max_concurrent: 32
      # verify_tls: true
      # ca_file: "/etc/costrelay/upstream-ca.pem"        # свой УЦ апстрима
      # client_cert_file: "/etc/costrelay/client.pem"    # mTLS: только вместе с client_key_file
      # client_key_file: "/etc/costrelay/client.key"
      # auth_header: "Authorization"
      # auth_scheme: "Bearer"
      # context_length: 131072
      # cache_size_tokens: 0
      # max_completion_tokens: 0
      # price_multiplier: 1.0       # множитель к цене провайдера
      # fixed_fee_rub: 0            # фиксированная плата за запрос
      # price_per_million: { input: 0, output: 0, cache_read: 0 }  # обязательна для type: openai
      # timeouts: { connect: "5s", total: "900s" }
  # local:                          # локальные OpenAI-compatible серверы
  #   - id: "llama"
  #     url: "http://127.0.0.1:8080/v1"
  #     api_key: "file:/run/secrets/local_llm_token"
  #     enabled: true
  #     models: { "local/llama": "qwen3" }
  #     max_concurrent: 2
  #     ca_file: "/etc/costrelay/upstream-ca.pem"
  #     client_cert_file: "/etc/costrelay/client.pem"   # mTLS: вместе с client_key_file
  #     client_key_file: "/etc/costrelay/client.key"
  #     price_per_million: { input: 0, output: 0, cache_read: 0 }  # по умолчанию 0
  #     auth_header: "Authorization"
  #     auth_scheme: "Bearer"
  #     context_length: 131072
  #     cache_size_tokens: 0
  #     max_completion_tokens: 0
  #     verify_tls: true
  #     timeouts: { connect: "5s", total: "900s" }

consumers:                          # агенты с токенами доступа
  - id: "agent-server"
    # token_hash или token; оба принимают file:/env:, как api_key провайдера.
    # Файл может содержать sha256-хеш или сырой токен (хешируется при старте).
    token_hash: "file:/run/secrets/agent_server_hash"
    # token: "file:/run/secrets/agent_server_token"   # сырой токен; вместе с token_hash нельзя
    models: ["*"]
    # scopes: ["proxy:invoke"]       # proxy:invoke | stats:read
    # rpm: 0                        # 0 = без ограничения
    # tpm: 0
    # max_concurrent: 0
    # budget_rub_day: 0             # 0 = без ограничения
    # expires_at: null              # RFC3339
    # allowed_networks: []

# Необязательные блоки:
# analytics: { enabled: true, log_dir: "./data", snapshot: "./state/provider-analytics.json", auto_import: true, max_file_bytes: 64m, max_samples_per_group: 20000 }   # тоже от каталога конфига
# capture: { enabled: true, key: "file:/run/secrets/body_key", compression: "gzip", retention_days: 7, max_total_bytes: 50g, max_request_bytes: 8m, max_response_bytes: 32m, max_stream_bytes: 8m, capture_stream: true, capture_errors: true, sample_rate: 1.0 }   # compression: gzip | zstd | none; key — только file:/env:
# blacklist:                        # строка = все модели провайдера и причина по умолчанию
#   - "byteplus"
#   - { provider: "routerai", models: ["deepseek/*"], reason: "quantization" }
whitelist: []   # пусто = все провайдеры; непустой список = только эти теги: [{ provider: "routerai", models: ["deepseek/*"] }]
# stats: { enabled: true, sqlite_path: "./state/stats.db", ring_buffer_size: 1000, requests_retention_days: 7, aggregates_retention_days: 90, prometheus: { enabled: true, require_token: false } }
# pricing: { threshold_mode: "replace", stale_after_sec: 900, max_stale_sec: 3600, require_fresh: false, timeout: "5s", max_body_bytes: 2097152, refresh: { hot_interval: "300s", max_endpoints_per_model: 500 } }   # threshold_mode: replace | excess
# routing:
#   client_provider_policy: "strip"   # strip | reject: что делать с `provider` от клиента
#   country: ""                       # подсказка RouterAI о стране
#   quantization: []                  # допустимые квантования
#   inject_provider_options: true     # переключатель панели: добавлять объект provider в запрос
#   fallback_attempts: 2              # дополнительных попыток после первой (0..10)
#   candidate_cap: 32                 # предел кандидатов на запрос (1..256)
#   provider_order_size: 1            # верхних провайдеров очереди в запрос (provider.order), 1..32; 1 — смену выбирает только прокси
#   queue_min_sec: 600                # тик перестроения очереди: минимум, сек (1..86400; новый прайс перестраивает сразу)
#   queue_max_sec: 900                # тик перестроения очереди: максимум, сек (1..86400; случайный интервал)
#   queue_stats_sec: 3600             # статистика перестраивает очередь не чаще, чем раз в сек (1..86400)
#   cache_loss_spike: { ratio: 3.0, window_sec: 900 }   # инцидент: запрос дороже предыдущего в ratio раз (> 1) без попадания в кеш; window_sec >= 1
#   cache_miss: { warn: 2, blacklist: 5 }   # серия успешных запросов без попаданий в кеш: с warn — предупреждение в журнале, с blacklist — пара (провайдер, модель) уходит в чёрный список (1..1000, warn <= blacklist)
#   leader: { margin_percent: 15, min_dwell_sec: 1800, confirm_builds: 2, state_file: "" }   # min_dwell_sec >= 0, confirm_builds >= 1   # гистерезис лидера; margin_percent (0..100) — выигрыш в %, при котором меняется лидер и снимается привязка сессии; state_file пусто = <data_dir>/routing-leader.json
#   session_pin_sec: 1800             # привязка (сессия, модель): держит провайдера на время бездействия (0..604800), пока выигрыш другого оффера ниже margin_percent; выше порога — перепривязка сразу; 0 — выключена
#   session_pin_max: 4096             # предел таблицы привязок (1..1000000)
#   wait_price_rub_per_sec: 0.001    # цена секунды ожидания клиента, RUB/с (0..100): входит в вес RUB/M (токены + задержка) и решает, сколько ждать провайдера до fallback; 0 — время не учитывается
#   failover_wait_max_sec: 60        # предел ожидания текущего провайдера перед fallback, сек (0..86400); 0 — переходить сразу после попыток retry
#   output_weight: 1.0                # поправочный вес цены выхода, 0..100 (1 = как в формуле стоимости)
#   cache_weight: 2.0                 # поправочный вес цены кеша, 0..100 (1 = как в формуле стоимости)
#   profile_sample_size: 30           # профиль весов (0..100000): N — последние N успешных запросов модели (динамические веса), 0 — средняя по журналу (переживает перезапуск)
#   retry: { attempts: 1, backoff_ms: 500 }   # повтор запроса у того же провайдера при сети/таймауте/429/5xx/пустом ответе; attempts 0..10, backoff_ms 0..60000; тело запроса — в памяти, не на диске
#   health_fail_threshold: 3          # серия отказов провайдера (включая пустой ответ), после которой он уходит из маршрута (0..1000); 0 — правило выключено
#   health_down_after_sec: 60         # отказы непрерывно дольше, сек (0..86400), тоже уводят провайдера из маршрута; 0 — правило выключено
#   health_probe_interval_sec: 60     # как часто проверять доступность ушедшего из маршрута провайдера минимальным запросом (1..86400)
#   health_recover_successes: 1       # успешных проверок подряд, чтобы вернуть провайдера в маршрут (1..100)
)CONFIG";

} // namespace proxy
