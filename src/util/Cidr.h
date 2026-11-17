#pragma once

#include <string_view>

namespace proxy {

// Синтаксис CIDR: адрес и необязательный префикс в границах 0..32 (IPv4) или
// 0..128 (IPv6). Один владелец разбора: конфигурация проверяет этим же
// правилом, которым пользуется доступ.
bool cidrValid(std::string_view cidr);
// Входит ли адрес в CIDR; неразобранное значение не совпадает ни с чем.
bool ipInCidr(std::string_view ip, std::string_view cidr);

} // namespace proxy
