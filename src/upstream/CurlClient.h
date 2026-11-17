#pragma once

#include "config/Config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace proxy {

struct CurlHttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    TimeoutConfig timeouts;
    bool http2 = true;
    bool verify_tls = true;
    std::string ca_file;
    std::string client_cert_file;
    std::string client_key_file; // mTLS-сертификат и ключ, если нужны
    bool accept_gzip = true;
};

struct CurlHttpResponse {
    long status = 0;
    std::map<std::string, std::string> headers;
    std::vector<std::string> header_lines; // original "Name: value", duplicates preserved
    std::string body;
    std::string error;
};

using CurlDataCallback = std::function<bool(const char*, std::size_t)>;
using CurlHeaderCallback = std::function<void(const CurlHttpResponse&)>;

class CurlClient {
  public:
    CurlClient() = default;
    CurlClient(const CurlClient&) = delete;
    CurlClient& operator=(const CurlClient&) = delete;

    // Синхронный запрос: on_data возвращает false для отмены, on_headers
    // вызывается один раз при получении полных заголовков ответа.
    bool perform(const CurlHttpRequest& req,
                 CurlHttpResponse& resp,
                 const CurlDataCallback& on_data = {},
                 const CurlHeaderCallback& on_headers = {},
                 std::atomic<bool>* cancel = nullptr) const;
};

} // namespace proxy
