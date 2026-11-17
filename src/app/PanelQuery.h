#pragma once

#include <drogon/HttpRequest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace proxy {

// Разбор limit/offset панели: одно правило для API и страниц. Битое значение —
// отказ (nullopt), а не тихая подмена дефолтом: иначе панель показывает не те
// данные, о которых просил клиент. Запрошенный предел зажимается сверху: предел
// списка принадлежит вызывающему, а не клиенту.
inline std::optional<std::size_t> queryLimit(const drogon::HttpRequestPtr& req,
                                             std::size_t def, std::size_t max) {
    const std::string raw = req->getParameter("limit");
    if (raw.empty()) return def;
    try {
        std::size_t used = 0;
        const auto value = std::stoull(raw, &used);
        if (used != raw.size()) return std::nullopt;
        return std::clamp<std::size_t>(value, 1, max);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<int> queryOffset(const drogon::HttpRequestPtr& req) {
    const std::string raw = req->getParameter("offset");
    if (raw.empty()) return 0;
    try {
        std::size_t used = 0;
        const int value = std::stoi(raw, &used);
        if (used != raw.size() || value < 0) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace proxy
