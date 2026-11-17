#include "util/Paths.h"
#include "util/Strings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace proxy {

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto pos = s.find(delim, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

// Supports '*' and '?' only; enough for model allow-lists.
bool globMatch(std::string_view pattern, std::string_view value) {
    std::size_t p = 0, v = 0, star = std::string::npos, match = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p; ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = v;
        } else if (star != std::string::npos) {
            p = star + 1;
            v = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string htmlEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string urlEncode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string hexEncode(const unsigned char* data, std::size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out += hex[data[i] >> 4];
        out += hex[data[i] & 0xF];
    }
    return out;
}

std::string base64UrlEncode(const unsigned char* data, std::size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        if (i + 1 < len) out += tbl[(v >> 6) & 0x3F];
        if (i + 2 < len) out += tbl[v & 0x3F];
    }
    return out;
}

bool constantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return hexEncode(buf.data(), buf.size());
}

std::string randomBase64Url(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return base64UrlEncode(buf.data(), buf.size());
}

std::string uuidV7() {
    // UUIDv7: упорядочен по времени, хвост случайный.
    std::uint64_t ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::array<unsigned char, 16> b{};
    b[0] = static_cast<unsigned char>((ms >> 40) & 0xFF);
    b[1] = static_cast<unsigned char>((ms >> 32) & 0xFF);
    b[2] = static_cast<unsigned char>((ms >> 24) & 0xFF);
    b[3] = static_cast<unsigned char>((ms >> 16) & 0xFF);
    b[4] = static_cast<unsigned char>((ms >> 8) & 0xFF);
    b[5] = static_cast<unsigned char>(ms & 0xFF);
    if (RAND_bytes(b.data() + 6, 10) != 1) throw std::runtime_error("RAND_bytes failed");
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x70);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    std::string out;
    out.reserve(36);
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += hex[b[i] >> 4];
        out += hex[b[i] & 0xF];
    }
    return out;
}

bool isSha256Hash(std::string_view value) {
    if (startsWith(value, "sha256:")) value.remove_prefix(7);
    if (value.size() != 64) return false;
    return value.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}

std::string normalizeSha256Hash(std::string_view value) {
    if (startsWith(value, "sha256:")) value.remove_prefix(7);
    std::string out(value);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isRequestId(std::string_view value) {
    if (value.size() < 8 || value.size() > 64) return false;
    for (const char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string sha256Hex(std::string_view data) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    return hexEncode(digest, sizeof(digest));
}

std::string expandSecret(std::string_view value, const std::string& base_dir) {
    std::string s(value);
    if (startsWith(s, "env:")) {
        const char* v = std::getenv(s.substr(4).c_str());
        if (!v) throw std::runtime_error("environment variable is not set: " + s.substr(4));
        return v;
    }
    if (startsWith(s, "file:")) {
        const std::string path = anchorPath(s.substr(5), base_dir);
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read secret file: " + path);
        std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }
    return s;
}

} // namespace proxy
