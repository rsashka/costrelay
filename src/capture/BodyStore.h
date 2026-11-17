#pragma once

#include "config/Config.h"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

namespace proxy {

struct BodyMeta {
    std::string id;
    std::string request_id;
    std::string kind;
    std::string path;
    std::string sha256;
    std::size_t size_plain = 0;
    std::size_t size_stored = 0;
    std::string content_type;
    bool truncated = false;
    std::chrono::system_clock::time_point created;
};

class BodyStore {
  public:
    explicit BodyStore(const CaptureConfig& cfg);
    bool init(std::string& error);
    bool enabled() const { return cfg_.enabled; }

    std::optional<BodyMeta> put(const std::string& request_id, const std::string& kind,
                                const std::string& content_type, std::string_view body,
                                bool truncated = false);
    std::optional<std::string> get(const std::string& request_id, const std::string& kind);
    Json::Value metadataJson(const std::string& request_id) const;
    void gc();
    std::size_t storedBytes() const;
    std::size_t count() const;

  private:
    bool encryptAndWrite(const std::string& path, std::string_view plain, std::string& error);
    std::optional<std::string> readAndDecrypt(const std::string& path, std::string& error) const;
    std::string makePath(const std::string& request_id, const std::string& kind) const;
    void deriveKey();

    CaptureConfig cfg_;
    std::vector<unsigned char> key_;
    mutable std::mutex mu_;
    std::map<std::string, std::vector<BodyMeta>> by_request_;
    std::map<std::string, BodyMeta> by_id_;
    std::size_t stored_bytes_ = 0;
};

} // namespace proxy
