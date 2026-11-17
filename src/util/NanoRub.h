#pragma once

#include <cstdint>
#include <limits>
#include <cmath>
#include <string>
#include <iomanip>
#include <sstream>

namespace proxy {

// 1 NanoRub == 1e-9 RUB; целочисленная арифметика держит сравнение цен точным.
using NanoRub = std::int64_t;

constexpr NanoRub kNanoPerRub = 1'000'000'000LL;

// Токенов в миллионе: в этих единицах каталог задаёт цену (RUB за 1M токенов).
constexpr long double kTokensPerMillion = 1'000'000.0L;

inline NanoRub rubToNano(long double rub) {
    return static_cast<NanoRub>(std::llround(rub * static_cast<long double>(kNanoPerRub)));
}

inline NanoRub perMillionRubToPerTokenNano(long double rubPerMillion) {
    return rubToNano(rubPerMillion / kTokensPerMillion);
}

inline std::string formatRub(NanoRub nano, int precision = 6) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << std::setprecision(precision)
       << (static_cast<long double>(nano) / static_cast<long double>(kNanoPerRub));
    return os.str();
}

inline std::string formatRubPerMillion(NanoRub nanoPerToken, int precision = 4) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << std::setprecision(precision)
       << (static_cast<long double>(nanoPerToken) * kTokensPerMillion /
           static_cast<long double>(kNanoPerRub));
    return os.str();
}

// Единственное преобразование «сумма → RUB за 1M токенов» (weight, saved).
inline NanoRub perMillionFromTotal(NanoRub total, std::int64_t tokens) {
    if (tokens <= 0) return 0;
    return static_cast<NanoRub>(std::llround(static_cast<long double>(total) * kTokensPerMillion /
                                             static_cast<long double>(tokens)));
}

// Обратное преобразование: цена за 1M токенов → сумма на профиле запроса.
inline NanoRub totalFromPerMillion(NanoRub nanoPerMillion, std::int64_t tokens) {
    if (tokens <= 0) return 0;
    return static_cast<NanoRub>(std::llround(static_cast<long double>(nanoPerMillion) *
                                             static_cast<long double>(tokens) / kTokensPerMillion));
}

// Форматирует результат perMillionFromTotal; formatRubPerMillion принимает
// цену за токен — это разные величины.
inline std::string formatRubPerMillionTotal(NanoRub nanoPerMillion, int precision = 4) {
    return formatRub(nanoPerMillion, precision);
}

// Сумма для панели: не меньше двух знаков после запятой, дальше — сколько нужно,
// чтобы не обнулить мелкие суммы; группы разрядов через пробел.
inline std::string formatRubDisplay(NanoRub nano) {
    const long double rub =
        static_cast<long double>(nano) / static_cast<long double>(kNanoPerRub);
    const long double magnitude = rub < 0 ? -rub : rub;
    const int precision = magnitude >= 1.0L ? 2 : (magnitude >= 0.01L ? 4 : 6);
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << std::setprecision(precision) << rub;
    std::string text = os.str();
    const auto dot = text.find('.');
    if (dot != std::string::npos) {
        while (text.size() > dot + 3 && text.back() == '0') text.pop_back();
    }
    const auto sign = text.rfind('-');
    const std::size_t begin = sign == 0 ? 1 : 0;
    for (std::size_t pos = (dot == std::string::npos ? text.size() : dot); pos > begin + 3;
         pos -= 3) {
        text.insert(pos - 3, " ");
    }
    return text;
}

// Сложение с защитой от переполнения: при переполнении max.
inline NanoRub safeAdd(NanoRub a, NanoRub b) {
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    const auto max = std::numeric_limits<NanoRub>::max();
    return a > max - b ? max : a + b;
}

// Умножение токенов на цену с защитой от переполнения: при переполнении max.
inline NanoRub safeMulTokens(std::int64_t tokens, NanoRub price) {
    if (tokens <= 0 || price <= 0) return 0;
    const auto max = std::numeric_limits<NanoRub>::max();
    if (tokens > max / price) return max;
    return tokens * price;
}

} // namespace proxy
