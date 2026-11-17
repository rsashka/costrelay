#pragma once

#include <algorithm>
#include <cstdint>

namespace proxy {

// Доля части в целом: 0 при пустом или отрицательном знаменателе, не больше 1.
inline double ratioOf(std::int64_t part, std::int64_t total) {
    if (total <= 0) return 0.0;
    const double value =
        static_cast<double>(std::max<std::int64_t>(0, part)) / static_cast<double>(total);
    return std::clamp(value, 0.0, 1.0);
}

} // namespace proxy
