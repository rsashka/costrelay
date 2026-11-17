#include "app/Endpoints.h"

namespace proxy {
namespace {

std::string urlHost(const std::string& listen) {
    if (listen.empty() || listen == "0.0.0.0" || listen == "*") return "127.0.0.1";
    if (listen == "::" || listen == "[::]") return "[::1]";
    if (listen.front() == '[') return listen;
    if (listen.find(':') != std::string::npos) return "[" + listen + "]";
    return listen;
}

bool httpsEnabled(const AppConfig& config) {
    return config.server.remote_https_enabled && !config.server.tls_cert_file.empty() &&
           !config.server.tls_key_file.empty();
}

} // namespace

// The HTTPS listener is bound to 0.0.0.0 in ProxyApp::run(), so an IPv6 HTTP
// bind must not be reused in the browser-facing HTTPS URL.
std::string httpsUrlHost(const std::string& listen) {
    const std::string host = urlHost(listen);
    return host.front() == '[' ? "127.0.0.1" : host;
}

std::vector<std::string> panelUrls(const AppConfig& config) {
    std::vector<std::string> urls;
    if (!config.portal.enabled) return urls;

    const std::string base = config.panel.base_path;
    if (!config.server.public_url.empty()) {
        urls.push_back(config.server.public_url + base);
        return urls;
    }
    const std::string host = urlHost(config.server.listen);
    if (!config.server.remote_https_enabled) {
        urls.push_back("http://" + host + ":" + std::to_string(config.server.port) + base);
    }
    // Совпадает с листенером из ProxyApp::run().
    if (httpsEnabled(config)) {
        urls.push_back("https://" + httpsUrlHost(config.server.remote_https_listen) + ":" +
                       std::to_string(config.server.remote_https_port) + base);
    }
    return urls;
}

std::vector<std::string> agentApiUrls(const AppConfig& config) {
    // Локальные агенты всегда ходят по HTTP на loopback-листенер.
    std::vector<std::string> urls;
    urls.push_back("http://" + urlHost(config.server.listen) + ":" +
                   std::to_string(config.server.port) + "/v1");
    if (!config.server.public_url.empty()) {
        urls.push_back(config.server.public_url + "/v1");
        return urls;
    }
    if (httpsEnabled(config)) {
        urls.push_back("https://" + httpsUrlHost(config.server.remote_https_listen) + ":" +
                       std::to_string(config.server.remote_https_port) + "/v1");
    }
    return urls;
}

} // namespace proxy
