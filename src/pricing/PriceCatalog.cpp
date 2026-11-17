#include "pricing/PriceCatalog.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace proxy {
namespace {

NanoRub priceToNano(const Json::Value& v) {
    if (!v || v.isNull()) return 0;
    if (v.isNumeric()) return rubToNano(static_cast<long double>(v.asDouble()));
    if (v.isString()) {
        try {
            return rubToNano(std::stold(v.asString()));
        } catch (...) {
            LOG_WARN << "price value is not a number: '" << v.asString() << "'";
            return 0;
        }
    }
    LOG_WARN << "price value has unsupported type: " << v.toStyledString();
    return 0;
}

std::string asString(const Json::Value& v, const std::string& def = {}) {
    if (!v || !v.isString()) return def;
    return v.asString();
}

std::vector<std::string> asStringList(const Json::Value& v) {
    std::vector<std::string> out;
    if (!v || !v.isArray()) return out;
    for (const auto& x : v) if (x.isString()) out.push_back(x.asString());
    return out;
}

std::int64_t asInt64(const Json::Value& v, std::int64_t def = 0) {
    if (!v) return def;
    if (v.isInt64()) return v.asInt64();
    if (v.isInt()) return v.asInt();
    if (v.isUInt64()) return static_cast<std::int64_t>(v.asUInt64());
    if (v.isDouble()) return static_cast<std::int64_t>(v.asDouble());
    if (v.isString()) {
        try { return std::stoll(v.asString()); } catch (...) { return def; }
    }
    return def;
}

} // namespace

bool PriceCatalog::replace(ModelPriceSnapshot snapshot) {
    if (snapshot.model_id.empty()) return false;
    if (snapshot.endpoints.empty()) return false;
    auto ptr = std::make_shared<const ModelPriceSnapshot>(std::move(snapshot));
    {
        std::lock_guard<std::mutex> lock(mu_);
        models_[ptr->model_id] = ptr;
    }
    revision_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::shared_ptr<const ModelPriceSnapshot> PriceCatalog::get(const std::string& model) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = models_.find(model);
    return it == models_.end() ? nullptr : it->second;
}

std::vector<std::string> PriceCatalog::models() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    out.reserve(models_.size());
    for (const auto& [k, _] : models_) out.push_back(k);
    return out;
}

std::size_t PriceCatalog::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return models_.size();
}

ModelPriceSnapshot PriceCatalog::parseEndpoints(const Json::Value& root,
                                                const std::string& expected_model) {
    ModelPriceSnapshot snap;
    snap.model_id = expected_model;
    snap.fetched_at = std::chrono::system_clock::now();

    const Json::Value& data = root["data"];
    if (!data.isObject()) throw std::runtime_error("price body has no data object");
    const std::string id = asString(data["id"]);
    if (!id.empty() && id != expected_model) {
        throw std::runtime_error("price body model mismatch: " + id);
    }
    const Json::Value& endpoints = data["endpoints"];
    if (!endpoints.isArray() || endpoints.empty()) {
        throw std::runtime_error("price body has no endpoints");
    }

    for (const auto& e : endpoints) {
        if (!e.isObject()) continue;
        PriceOffer o;
        o.provider_tag = asString(e["tag"]);
        if (o.provider_tag.empty()) continue;
        o.context_length = asInt64(e["context_length"]);
        o.max_completion_tokens = asInt64(e["max_completion_tokens"]);
        if (e.isMember("max_prompt_tokens") && !e["max_prompt_tokens"].isNull()) {
            o.max_prompt_tokens = asInt64(e["max_prompt_tokens"]);
        }
        o.supported_parameters = asStringList(e["supported_parameters"]);
        o.status = static_cast<int>(asInt64(e["status"], 0));
        o.quantization = asString(e["quantization"]);
        o.countries = asStringList(e["countries"]);
        if (o.countries.empty() && e["country"].isString()) o.countries.push_back(e["country"].asString());

        const Json::Value& units = e["pricing_units"];
        o.pricing_units_prompt = asString(units["prompt"], "token");
        o.pricing_units_completion = asString(units["completion"], "token");
        const std::string cache_units = asString(units["input_cache_read"], "token");
        o.chat_compatible = (o.pricing_units_prompt == "token" &&
                             o.pricing_units_completion == "token");

        const Json::Value& pricing = e["pricing"];
        if (!pricing.isObject()) continue;
        o.prompt_per_token = priceToNano(pricing["prompt"]);
        o.completion_per_token = priceToNano(pricing["completion"]);
        if (o.prompt_per_token < 0 || o.completion_per_token < 0) continue;
        if (pricing.isMember("input_cache_read") && cache_units == "token") {
            o.cache_read_per_token = priceToNano(pricing["input_cache_read"]);
            if (o.cache_read_per_token < 0) o.cache_read_per_token = o.prompt_per_token;
        } else {
            // Отдельной цены кеша нет: кешированный вход не дешевле.
            o.cache_read_per_token = o.prompt_per_token;
        }
        if (pricing.isMember("input_cache_write") && cache_units == "token") {
            const NanoRub write = priceToNano(pricing["input_cache_write"]);
            o.cache_write_per_token = write > 0 ? write : o.prompt_per_token;
        }

        const Json::Value& variable = e["variable_pricings"];
        if (variable.isArray()) {
            for (const auto& v : variable) {
                if (!v.isObject()) continue;
                if (asString(v["type"]) == "prompt-threshold") {
                    o.threshold_tokens = asInt64(v["threshold"]);
                    o.threshold_prompt_per_token = priceToNano(v["prompt"]);
                    if (o.threshold_tokens <= 0) o.threshold_tokens = 0;
                }
            }
        }
        snap.endpoints.push_back(std::move(o));
    }
    if (snap.endpoints.empty()) throw std::runtime_error("no valid endpoints in price body");
    // Отбор по max_endpoints_per_model режет хвост, поэтому дешёвые офферы — первыми.
    std::sort(snap.endpoints.begin(), snap.endpoints.end(),
              [](const PriceOffer& a, const PriceOffer& b) {
                  if (a.prompt_per_token != b.prompt_per_token) {
                      return a.prompt_per_token < b.prompt_per_token;
                  }
                  return a.provider_tag < b.provider_tag;
              });
    return snap;
}

bool PriceCatalog::loadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        LOG_WARN << "price snapshot not readable: " << path;
        return false;
    }
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    if (!Json::parseFromStream(b, in, &root, &errs)) {
        LOG_WARN << "price snapshot ignored: " << path << " " << errs;
        return false;
    }
    if (!root.isObject()) {
        LOG_WARN << "price snapshot ignored: " << path << " (not an object)";
        return false;
    }
    bool loaded = false;
    for (const auto& item : root["models"]) {
        if (!item.isObject()) continue;
        try {
            ModelPriceSnapshot snap;
            snap.model_id = item["model_id"].asString();
            snap.etag = item.get("etag", "").asString();
            if (item.isMember("fetched_at") && item["fetched_at"].isInt64()) {
                snap.fetched_at = std::chrono::system_clock::from_time_t(
                    static_cast<std::time_t>(item["fetched_at"].asInt64()));
            } else {
                snap.fetched_at = std::chrono::system_clock::now();
            }
            for (const auto& e : item["endpoints"]) {
                if (!e.isObject()) continue;
                PriceOffer o;
                o.provider_tag = e.get("tag", "").asString();
                if (o.provider_tag.empty()) continue;
                o.prompt_per_token = rubToNano(e.get("prompt", 0.0).asDouble());
                o.completion_per_token = rubToNano(e.get("completion", 0.0).asDouble());
                o.cache_read_per_token = e.isMember("cache_read")
                                             ? rubToNano(e.get("cache_read", 0.0).asDouble())
                                             : o.prompt_per_token;
                o.cache_write_per_token = e.isMember("cache_write")
                                              ? rubToNano(e.get("cache_write", 0.0).asDouble())
                                              : 0;
                o.cache_capacity_tokens = e.get("cache_capacity_tokens", 0).asInt64();
                o.threshold_tokens = e.get("threshold", 0).asInt64();
                o.threshold_prompt_per_token = rubToNano(e.get("threshold_prompt", 0.0).asDouble());
                o.fixed_request_fee = rubToNano(e.get("fixed_request_fee", 0.0).asDouble());
                o.context_length = e.get("context_length", 0).asInt64();
                o.max_completion_tokens = e.get("max_completion_tokens", 0).asInt64();
                if (e.isMember("max_prompt_tokens") && e["max_prompt_tokens"].isInt64()) {
                    o.max_prompt_tokens = e["max_prompt_tokens"].asInt64();
                }
                for (const auto& p : e["supported_parameters"]) if (p.isString()) o.supported_parameters.push_back(p.asString());
                for (const auto& c : e["countries"]) if (c.isString()) o.countries.push_back(c.asString());
                o.quantization = e.get("quantization", "").asString();
                o.pricing_units_prompt = e.get("pricing_units_prompt", "token").asString();
                o.pricing_units_completion = e.get("pricing_units_completion", "token").asString();
                o.chat_compatible = e.get("chat_compatible", true).asBool();
                o.status = e.get("status", 0).asInt();
                snap.endpoints.push_back(std::move(o));
            }
            if (!snap.endpoints.empty()) {
                replace(std::move(snap));
                loaded = true;
            }
        } catch (const std::exception& e) {
            LOG_WARN << "price snapshot item skipped: " << path << " " << e.what();
        } catch (...) {
            LOG_WARN << "price snapshot item skipped: " << path;
        }
    }
    if (!loaded) LOG_WARN << "price snapshot has no usable models: " << path;
    return loaded;
}

bool PriceCatalog::saveSnapshot(const std::string& path) const {
    Json::Value root(Json::objectValue);
    Json::Value arr(Json::arrayValue);
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [_, snap] : models_) {
        Json::Value item(Json::objectValue);
        item["model_id"] = snap->model_id;
        item["etag"] = snap->etag;
        item["fetched_at"] = static_cast<Json::Int64>(
            std::chrono::duration_cast<std::chrono::seconds>(
                snap->fetched_at.time_since_epoch()).count());
        Json::Value eps(Json::arrayValue);
        for (const auto& e : snap->endpoints) {
            Json::Value x(Json::objectValue);
            x["tag"] = e.provider_tag;
            x["prompt"] = static_cast<double>(e.prompt_per_token) / static_cast<double>(kNanoPerRub);
            x["completion"] = static_cast<double>(e.completion_per_token) / static_cast<double>(kNanoPerRub);
            x["cache_read"] = static_cast<double>(e.cache_read_per_token) / static_cast<double>(kNanoPerRub);
            x["cache_write"] = static_cast<double>(e.cache_write_per_token) / static_cast<double>(kNanoPerRub);
            x["cache_capacity_tokens"] = static_cast<Json::Int64>(e.cache_capacity_tokens);
            x["threshold"] = static_cast<Json::Int64>(e.threshold_tokens);
            x["threshold_prompt"] = static_cast<double>(e.threshold_prompt_per_token) / static_cast<double>(kNanoPerRub);
            x["fixed_request_fee"] = static_cast<double>(e.fixed_request_fee) / static_cast<double>(kNanoPerRub);
            x["context_length"] = static_cast<Json::Int64>(e.context_length);
            x["max_completion_tokens"] = static_cast<Json::Int64>(e.max_completion_tokens);
            if (e.max_prompt_tokens) x["max_prompt_tokens"] = static_cast<Json::Int64>(*e.max_prompt_tokens);
            Json::Value params(Json::arrayValue);
            for (const auto& p : e.supported_parameters) params.append(p);
            x["supported_parameters"] = params;
            Json::Value countries(Json::arrayValue);
            for (const auto& c : e.countries) countries.append(c);
            x["countries"] = countries;
            x["quantization"] = e.quantization;
            x["pricing_units_prompt"] = e.pricing_units_prompt;
            x["pricing_units_completion"] = e.pricing_units_completion;
            x["chat_compatible"] = e.chat_compatible;
            x["status"] = e.status;
            eps.append(x);
        }
        item["endpoints"] = eps;
        arr.append(item);
    }
    root["models"] = arr;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path + ".tmp", std::ios::trunc);
    if (!out) return false;
    out << root;
    out.close();
    if (!out) return false;
    std::filesystem::rename(path + ".tmp", path);
    return true;
}

} // namespace proxy
