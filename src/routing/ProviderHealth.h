#pragma once

#include "config/Config.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace proxy {

// Доступность провайдера по фактам живых запросов и проверок. Провайдер, который
// отказывает `health_fail_threshold` раз подряд или дольше `health_down_after_sec`,
// уходит из маршрута: живой запрос сразу идёт к следующему по очереди. Пока он
// недоступен, его периодически проверяют (`health_probe_interval_sec`), и после
// `health_recover_successes` успешных проверок подряд он возвращается в очередь.
struct ProviderHealthSnapshot {
    std::string provider;
    std::string router_id;
    std::string model;              // модель последнего отказа
    bool down = false;
    int fail_streak = 0;
    std::int64_t failing_since = 0; // epoch секунд, начало серии отказов
    std::int64_t down_since = 0;    // epoch секунд, когда провайдер ушёл из маршрута
    long last_status = 0;
    std::string last_error;
    std::int64_t next_probe_at = 0; // epoch секунд, когда пора проверить
};

class ProviderHealth {
  public:
    explicit ProviderHealth(const AppConfig& config) : config_(config) {}

    // Отказ провайдера в запросе: серия считается по запросам, а не по попыткам —
    // `failing_since` передаёт время первой неудачной попытки этого запроса, чтобы
    // ожидание восстановления внутри запроса не обходило правило по длительности.
    // Каждый отказ уже записан предупреждением вызывающим кодом.
    void noteFailure(const std::string& provider, const std::string& router_id,
                     const std::string& model, long status, const std::string& error,
                     std::int64_t failing_since = 0);
    // Успешный живой запрос: серия отказов сбрасывается.
    void noteSuccess(const std::string& provider);
    // Итог проверки доступности. ok=false — проверить снова через интервал;
    // ok=true — после recover_successes успехов подряд провайдер возвращается.
    void noteProbeResult(const std::string& provider, bool ok, long status,
                         const std::string& error);

    // Провайдер перестал быть оффером модели: отметка доступности больше не нужна,
    // но это не «успешная проверка» — счётчик восстановлений не накручивается.
    void forget(const std::string& provider);

    bool isDown(const std::string& provider) const;
    std::vector<ProviderHealthSnapshot> snapshot() const;
    // Провайдеры, которым пора проверить доступность.
    std::vector<ProviderHealthSnapshot> dueProbes() const;

  private:
    struct State {
        ProviderHealthSnapshot view;
        int recoveries = 0;
    };

    const AppConfig& config_;
    mutable std::mutex mu_;
    std::map<std::string, State> states_;
};

} // namespace proxy