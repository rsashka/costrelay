#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace proxy {

std::string trim(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
std::string toLower(std::string_view s);
std::vector<std::string> split(std::string_view s, char delim);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
bool globMatch(std::string_view pattern, std::string_view value);
std::string htmlEscape(std::string_view s);
std::string urlEncode(std::string_view s);
std::string hexEncode(const unsigned char* data, std::size_t len);
std::string base64UrlEncode(const unsigned char* data, std::size_t len);
bool constantTimeEqual(std::string_view a, std::string_view b);
std::string randomHex(std::size_t bytes);
std::string randomBase64Url(std::size_t bytes);
std::string uuidV7();
std::string sha256Hex(std::string_view data);
// Контракт токен-хеша: 64 hex-символа с необязательным префиксом sha256:.
bool isSha256Hash(std::string_view value);
// Нормализованная форма хеша: без префикса `sha256:` и в нижнем регистре.
std::string normalizeSha256Hash(std::string_view value);
// Идентификатор запроса: 8..64 символов [A-Za-z0-9_-]. Одно правило для
// клиентского заголовка и для имени файла захваченного тела.
bool isRequestId(std::string_view value);
std::string expandSecret(std::string_view value, const std::string& base_dir = {}); // env: / file: / literal

} // namespace proxy
