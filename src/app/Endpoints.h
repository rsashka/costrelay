#pragma once

#include "config/Config.h"

#include <string>
#include <vector>

namespace proxy {

// Хост HTTPS-листенера для ссылок, которые видит браузер: wildcard-bind
// заменяется на loopback, IPv6-литерал берётся в скобки.
std::string httpsUrlHost(const std::string& listen);

// Browser-facing panel URLs for the effective listeners. Empty when the panel
// is disabled. A wildcard bind is rendered as loopback because it is not a
// usable destination address in a browser.
std::vector<std::string> panelUrls(const AppConfig& config);

// Базовые URL агентов: сначала loopback по HTTP, затем HTTPS для локальной
// сети (из `proxy.public_url`, иначе из TLS-листенера).
std::vector<std::string> agentApiUrls(const AppConfig& config);

} // namespace proxy
