#include "pricing/PriceFetcher.h"
#include "util/Strings.h"

#include <drogon/drogon.h>

#include <json/json.h>

#include <sstream>

namespace proxy {

PriceFetcher::PriceFetcher(const AppConfig& config, PriceCatalog& catalog)
    : config_(config), catalog_(catalog) {
    if (const RouterConfig* source = config_.priceSourceRouter()) {
        price_base_ = source->base_url;
        while (!price_base_.empty() && price_base_.back() == '/') price_base_.pop_back();
    }
}

PriceFetcher::~PriceFetcher() { stop(); }

std::string PriceFetcher::endpointsUrl(const std::string& model) const {
    if (price_base_.empty()) {
        throw std::runtime_error("no RouterAI upstream provides prices");
    }
    auto slash = model.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= model.size()) {
        throw std::runtime_error("model id must be author/model: " + model);
    }
    const std::string author = model.substr(0, slash);
    const std::string name = model.substr(slash + 1);
    return price_base_ + "/models/" + urlEncode(author) + "/" + urlEncode(name) + "/endpoints";
}

bool PriceFetcher::fetchModel(const std::string& model) {
    std::lock_guard<std::mutex> lock(refresh_mu_);
    CurlHttpRequest req;
    req.url = endpointsUrl(model);
    req.method = "GET";
    req.timeouts = config_.pricing.timeout_ms > 0
                       ? TimeoutConfig{config_.pricing.timeout_ms, config_.pricing.timeout_ms}
                       : TimeoutConfig{};
    req.http2 = true;
    req.verify_tls = true;
    req.accept_gzip = true;
    req.headers = {"Accept: application/json"};
    if (auto existing = catalog_.get(model); existing && !existing->etag.empty()) {
        req.headers.push_back("If-None-Match: " + existing->etag);
    }
    CurlHttpResponse resp;
    if (!curl_.perform(req, resp)) {
        return fail(model, resp.error.empty() ? "price fetch failed" : resp.error);
    }
    if (resp.status == 304) {
        return true;
    }
    if (resp.body.size() > config_.pricing.max_body_bytes) {
        return fail(model, "price body too large");
    }
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    std::istringstream in(resp.body);
    if (!Json::parseFromStream(b, in, &root, &errs)) {
        return fail(model, "invalid price JSON: " + errs);
    }
    try {
        auto snap = PriceCatalog::parseEndpoints(root, model);
        snap.etag = resp.headers.count("etag") ? resp.headers.at("etag") : "";
        if (snap.endpoints.size() > static_cast<std::size_t>(config_.pricing.refresh.max_endpoints_per_model)) {
            snap.endpoints.resize(config_.pricing.refresh.max_endpoints_per_model);
        }
        if (!catalog_.replace(std::move(snap))) {
            return fail(model, "price catalog rejected empty snapshot");
        }
        return true;
    } catch (const std::exception& e) {
        return fail(model, e.what());
    }
}

bool PriceFetcher::fail(const std::string& model, const std::string& message) {
    LOG_WARN << "price refresh failed: model=" << model << " error='" << message << "'";
    std::lock_guard<std::mutex> l(mu_);
    last_error_ = model + ": " + message;
    return false;
}

bool PriceFetcher::refreshNow() {
    bool all_ok = true;
    for (const auto& model : config_.routing.models) {
        bool needs_price = false;
        for (const auto& router : config_.routers) {
            if (router.enabled && router.kind == "routerai" &&
                routerServesModel(router, model)) {
                needs_price = true;
                break;
            }
        }
        if (!needs_price) continue; // direct OpenAI-compatible models have no RouterAI price
        if (!fetchModel(model)) all_ok = false;
    }
    if (all_ok) {
        // Ошибка любой модели остаётся видимой: частичный отказ прайса не должен
        // выглядеть как «ошибок нет».
        std::lock_guard<std::mutex> l(mu_);
        last_error_.clear();
    }
    if (!config_.pricing.snapshot_file.empty() &&
        !catalog_.saveSnapshot(config_.pricing.snapshot_file)) {
        LOG_WARN << "price snapshot not saved: " << config_.pricing.snapshot_file;
    }
    if (on_refresh_) on_refresh_();
    return all_ok;
}

void PriceFetcher::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
}

void PriceFetcher::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PriceFetcher::loop() {
    while (running_.load()) {
        refreshNow();
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::seconds(config_.pricing.refresh.hot_interval_sec),
                     [this] { return !running_.load(); });
    }
}

std::string PriceFetcher::lastError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

} // namespace proxy
