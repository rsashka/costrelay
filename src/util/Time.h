#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace proxy {

// Локальное время панели и аналитики: Москва без перехода на летнее время.
constexpr int kMoscowUtcOffsetHours = 3;

inline std::pair<std::string, int> localMoscowDateTime() {
    const auto now =
        std::chrono::system_clock::now() + std::chrono::hours(kMoscowUtcOffsetHours);
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char date[16]{};
    if (std::strftime(date, sizeof(date), "%Y-%m-%d", &tm) == 0) return {"", -1};
    return {date, tm.tm_hour};
}

// Секунды в UTC; дробная часть отбрасывается, смещение зоны кроме Z не поддержано.
// Единственный разбор RFC3339: им пользуются проверка конфигурации и токены.
inline bool parseRfc3339(const std::string& s, std::chrono::system_clock::time_point& out) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return false;
    const auto at = ss.tellg();
    std::string rest = at < 0 ? std::string() : s.substr(static_cast<std::size_t>(at));
    if (!rest.empty() && rest.front() == '.') {
        std::size_t i = 1;
        while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') ++i;
        rest = rest.substr(i);
    }
    if (!rest.empty() && rest != "Z") return false;
    out = std::chrono::system_clock::from_time_t(timegm(&tm));
    return true;
}

} // namespace proxy
