#include "routing/ProviderHealth.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>

namespace proxy {
namespace {

std::int64_t epochSeconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

} // namespace

void ProviderHealth::noteFailure(const std::string& provider, const std::string& router_id,
                                 const std::string& model, long status,
                                 const std::string& error, std::int64_t failing_since) {
    if (provider.empty()) return;
    const auto& cfg = config_.routing.health;
    const std::int64_t now = epochSeconds(std::chrono::system_clock::now());
    std::lock_guard<std::mutex> lock(mu_);
    State& state = states_[provider];
    ProviderHealthSnapshot& view = state.view;
    view.provider = provider;
    if (!router_id.empty()) view.router_id = router_id;
    if (!model.empty()) view.model = model;
    view.last_status = status;
    view.last_error = error;
    if (view.down) return;
    ++view.fail_streak;
    if (failing_since > 0 && (view.failing_since == 0 || failing_since < view.failing_since)) {
        view.failing_since = failing_since;
    }
    if (view.failing_since == 0) view.failing_since = now;
    const bool by_threshold = cfg.fail_threshold > 0 && view.fail_streak >= cfg.fail_threshold;
    const bool by_duration =
        cfg.down_after_sec > 0 && now - view.failing_since >= cfg.down_after_sec;
    if (!by_threshold && !by_duration) return;
    view.down = true;
    view.down_since = now;
    view.next_probe_at =
        now + std::max(routing_bounds::kHealthProbeSecMin, cfg.probe_interval_sec);
    state.recoveries = 0;
    LOG_ERROR << "provider down: provider=" << provider << " model=" << view.model
              << " router=" << view.router_id << " failures=" << view.fail_streak
              << " failing_sec=" << (now - view.failing_since) << " status=" << status
              << " error='" << error << "'";
}

void ProviderHealth::noteSuccess(const std::string& provider) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = states_.find(provider);
    if (it == states_.end() || it->second.view.down) return;
    it->second.view.fail_streak = 0;
    it->second.view.failing_since = 0;
}

void ProviderHealth::noteProbeResult(const std::string& provider, bool ok, long status,
                                     const std::string& error) {
    const auto& cfg = config_.routing.health;
    const std::int64_t now = epochSeconds(std::chrono::system_clock::now());
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = states_.find(provider);
    if (it == states_.end() || !it->second.view.down) return;
    State& state = it->second;
    ProviderHealthSnapshot& view = state.view;
    view.next_probe_at =
        now + std::max(routing_bounds::kHealthProbeSecMin, cfg.probe_interval_sec);
    if (!ok) {
        state.recoveries = 0;
        view.last_status = status;
        view.last_error = error;
        return;
    }
    ++state.recoveries;
    if (state.recoveries < std::max(routing_bounds::kHealthRecoverMin, cfg.recover_successes))
        return;
    view.down = false;
    view.down_since = 0;
    view.failing_since = 0;
    view.fail_streak = 0;
    view.next_probe_at = 0;
    view.last_status = status;
    view.last_error.clear();
    LOG_INFO << "provider recovered: provider=" << provider << " model=" << view.model
             << " checks=" << state.recoveries;
    state.recoveries = 0;
}

void ProviderHealth::forget(const std::string& provider) {
    LOG_INFO << "provider no longer routed: provider=" << provider;
    std::lock_guard<std::mutex> lock(mu_);
    states_.erase(provider);
}

bool ProviderHealth::isDown(const std::string& provider) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = states_.find(provider);
    return it != states_.end() && it->second.view.down;
}

std::vector<ProviderHealthSnapshot> ProviderHealth::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ProviderHealthSnapshot> out;
    out.reserve(states_.size());
    for (const auto& [provider, state] : states_) {
        ProviderHealthSnapshot view = state.view;
        view.provider = provider;
        out.push_back(std::move(view));
    }
    return out;
}

std::vector<ProviderHealthSnapshot> ProviderHealth::dueProbes() const {
    const std::int64_t now = epochSeconds(std::chrono::system_clock::now());
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ProviderHealthSnapshot> out;
    for (const auto& [provider, state] : states_) {
        if (!state.view.down) continue;
        if (state.view.next_probe_at > now) continue;
        ProviderHealthSnapshot view = state.view;
        view.provider = provider;
        out.push_back(std::move(view));
    }
    return out;
}

} // namespace proxy