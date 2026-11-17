#include "util/Cidr.h"

#include <arpa/inet.h>

#include <string>

namespace proxy {
namespace {

// Совпадение адреса с сетью по префиксу: побитово, без арифметики над целым.
bool prefixMatches(const unsigned char* address, const unsigned char* network, int bits) {
    const int full = bits / 8;
    for (int i = 0; i < full; ++i) {
        if (address[i] != network[i]) return false;
    }
    const int rest = bits % 8;
    if (rest == 0) return true;
    const unsigned char mask = static_cast<unsigned char>(0xFFu << (8 - rest));
    return (address[full] & mask) == (network[full] & mask);
}

// Разбор префикса: пустая строка, мусор и выход за границы — недействительно.
bool parsePrefix(std::string_view text, int max_bits, int& out) {
    if (text.empty()) return false;
    try {
        std::size_t used = 0;
        const int value = std::stoi(std::string(text), &used);
        if (used != text.size() || value < 0 || value > max_bits) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool cidrValid(std::string_view cidr) {
    if (cidr.empty()) return false;
    const auto slash = cidr.find('/');
    if (slash != std::string_view::npos && cidr.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const std::string network(cidr.substr(0, slash));
    if (network.empty()) return false;
    unsigned char base[16]{};
    const bool v4 = inet_pton(AF_INET, network.c_str(), base) == 1;
    const bool v6 = !v4 && inet_pton(AF_INET6, network.c_str(), base) == 1;
    if (!v4 && !v6) return false;
    if (slash == std::string_view::npos) return true;
    int prefix = 0;
    return parsePrefix(cidr.substr(slash + 1), v4 ? 32 : 128, prefix);
}

bool ipInCidr(std::string_view ip, std::string_view cidr) {
    const auto slash = cidr.find('/');
    const std::string network(cidr.substr(0, slash));
    const bool has_prefix = slash != std::string_view::npos;
    const std::string source(ip);
    unsigned char address[16]{}, base[16]{};
    if (inet_pton(AF_INET, source.c_str(), address) == 1 &&
        inet_pton(AF_INET, network.c_str(), base) == 1) {
        int bits = 32;
        if (has_prefix && !parsePrefix(cidr.substr(slash + 1), 32, bits)) return false;
        return prefixMatches(address, base, bits);
    }
    if (inet_pton(AF_INET6, source.c_str(), address) == 1 &&
        inet_pton(AF_INET6, network.c_str(), base) == 1) {
        int bits = 128;
        if (has_prefix && !parsePrefix(cidr.substr(slash + 1), 128, bits)) return false;
        return prefixMatches(address, base, bits);
    }
    // Не IP-адрес: сравнение как есть, префикс не имеет смысла.
    return !has_prefix && source == network;
}

} // namespace proxy
