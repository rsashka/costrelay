#include "capture/BodyStore.h"
#include "util/Strings.h"

#include <drogon/drogon.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <zlib.h>
#include <zstd.h>

#include <filesystem>
#include <memory>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace proxy {
namespace {

// AES-GCM: nonce 96 бит и тег 128 бит — требование режима; от них зависит формат файла.
constexpr std::size_t kGcmNonceBytes = 12;
constexpr std::size_t kGcmTagBytes = 16;

// Маркер формата файла захваченного тела.
constexpr char kBodyMagic[] = "LPB1";

// Владелец контекста OpenSSL: освобождает его на любом пути выхода.
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

CipherCtx newCipherCtx() { return CipherCtx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free); }

bool compressGzip(std::string_view in, std::string& out) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.resize(compressBound(in.size()) + 32);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&zs); return false; }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return true;
}

bool decompressGzip(std::string_view in, std::string& out) {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;
    out.resize(in.size() * 4 + 1024);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    int rc = Z_OK;
    while (true) {
        if (out.size() - zs.total_out < 4096) out.resize(out.size() * 2 + 4096);
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + zs.total_out);
        zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) { inflateEnd(&zs); return false; }
    }
    out.resize(zs.total_out);
    inflateEnd(&zs);
    return true;
}


// Уровень zstd: ZSTD_minCLevel()..ZSTD_maxCLevel(); 3 — скорость важнее степени сжатия.
constexpr int kZstdLevel = 3;

bool compressZstd(std::string_view in, std::string& out) {
    const std::size_t bound = ZSTD_compressBound(in.size());
    out.resize(bound);
    const std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), kZstdLevel);
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    return true;
}

bool decompressZstd(std::string_view in, std::string& out) {
    const unsigned long long size = ZSTD_getFrameContentSize(in.data(), in.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) return false;
    out.resize(static_cast<std::size_t>(size));
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    return true;
}

} // namespace

BodyStore::BodyStore(const CaptureConfig& cfg) : cfg_(cfg) {}

void BodyStore::deriveKey() {
    key_.resize(32);
    SHA256(reinterpret_cast<const unsigned char*>(cfg_.encryption_key.data()),
           cfg_.encryption_key.size(), key_.data());
}

bool BodyStore::init(std::string& error) {
    if (!cfg_.enabled) return true;
    if (cfg_.encryption_key.empty()) {
        error = "capture.encryption_key is empty";
        return false;
    }
    deriveKey();
    try {
        std::filesystem::create_directories(cfg_.store_dir);
    } catch (const std::exception& e) {
        error = std::string("cannot create body store: ") + e.what();
        return false;
    }
    return true;
}

std::string BodyStore::makePath(const std::string& request_id, const std::string& kind) const {
    // request_id формирует сам прокси; правило имени — то же, что у заголовка
    // (util/Strings.h), отказ виден вызывающему, а не бросается исключением.
    if (!isRequestId(request_id)) return {};
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char date[32];
    std::snprintf(date, sizeof(date), "%04d/%02d/%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return cfg_.store_dir + "/" + date + "/" + request_id + "-" + kind + ".bin";
}

bool BodyStore::encryptAndWrite(const std::string& path, std::string_view plain, std::string& error) {
    std::string compressed;
    if (cfg_.compression == "zstd") {
        if (!compressZstd(plain, compressed)) { error = "zstd compression failed"; return false; }
    } else if (cfg_.compression == "gzip") {
        if (!compressGzip(plain, compressed)) { error = "gzip compression failed"; return false; }
    } else {
        compressed.assign(plain.data(), plain.size());
    }
    std::vector<unsigned char> nonce(kGcmNonceBytes);
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        error = "RAND_bytes failed";
        return false;
    }
    auto ctx = newCipherCtx();
    if (!ctx) { error = "EVP_CIPHER_CTX_new failed"; return false; }
    std::string cipher(compressed.size() + 32, '\0');
    int len = 0;
    int total = 0;
    unsigned char tag[kGcmTagBytes];
    bool ok = EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key_.data(),
                                 nonce.data()) == 1;
    if (ok) {
        ok = EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(cipher.data()), &len,
                               reinterpret_cast<const unsigned char*>(compressed.data()),
                               static_cast<int>(compressed.size())) == 1;
        total += len;
    }
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx.get(),
                                 reinterpret_cast<unsigned char*>(cipher.data()) + total,
                                 &len) == 1;
        total += len;
    }
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                                     static_cast<int>(kGcmTagBytes), tag) == 1;
    if (!ok) { error = "AES-GCM encryption failed"; return false; }
    cipher.resize(total);
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "cannot open body file"; return false; }
    out.write(kBodyMagic, 4);
    out.write(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    out.write(reinterpret_cast<const char*>(tag), sizeof(tag));
    out.write(cipher.data(), cipher.size());
    out.close();
    if (!out) { error = "cannot write body file"; return false; }
    return true;
}

std::optional<std::string> BodyStore::readAndDecrypt(const std::string& path, std::string& error) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "body not found"; return std::nullopt; }
    char magic[4];
    unsigned char nonce[kGcmNonceBytes], tag[kGcmTagBytes];
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(nonce), sizeof(nonce));
    in.read(reinterpret_cast<char*>(tag), sizeof(tag));
    if (!in || std::string(magic, 4) != kBodyMagic) {
        error = "bad body header";
        return std::nullopt;
    }
    std::string cipher((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto ctx = newCipherCtx();
    if (!ctx) { error = "EVP_CIPHER_CTX_new failed"; return std::nullopt; }
    std::string plain(cipher.size() + 32, '\0');
    int len = 0;
    int total = 0;
    bool ok = EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key_.data(), nonce) == 1;
    if (ok) {
        ok = EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &len,
                               reinterpret_cast<const unsigned char*>(cipher.data()),
                               static_cast<int>(cipher.size())) == 1;
        total += len;
    }
    if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                                     static_cast<int>(kGcmTagBytes), tag) == 1;
    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx.get(),
                                 reinterpret_cast<unsigned char*>(plain.data()) + total,
                                 &len) == 1;
    }
    if (!ok) { error = "body decryption/auth failed"; return std::nullopt; }
    plain.resize(total + len);
    std::string out;
    if (cfg_.compression == "zstd") {
        if (!decompressZstd(plain, out)) { error = "zstd decompression failed"; return std::nullopt; }
    } else if (cfg_.compression == "gzip") {
        if (!decompressGzip(plain, out)) { error = "body decompression failed"; return std::nullopt; }
    } else {
        out = std::move(plain);
    }
    return out;
}

std::optional<BodyMeta> BodyStore::put(const std::string& request_id, const std::string& kind,
                                       const std::string& content_type, std::string_view body,
                                       bool truncated) {
    if (!cfg_.enabled) return std::nullopt;
    const std::size_t limit = kind == "request" ? cfg_.max_request_bytes
                              : kind == "sse" ? cfg_.max_stream_bytes
                                              : cfg_.max_response_bytes;
    if (body.size() > limit) { body = body.substr(0, limit); truncated = true; }
    BodyMeta meta;
    meta.id = uuidV7();
    meta.request_id = request_id;
    meta.kind = kind;
    meta.path = makePath(request_id, kind);
    if (meta.path.empty()) return std::nullopt;
    meta.sha256 = sha256Hex(body);
    meta.size_plain = body.size();
    meta.content_type = content_type;
    meta.truncated = truncated;
    meta.created = std::chrono::system_clock::now();
    std::string error;
    if (!encryptAndWrite(meta.path, body, error)) {
        LOG_WARN << "body capture failed: request=" << request_id << " kind=" << kind
                 << " reason=" << error;
        return std::nullopt;
    }
    std::error_code ec;
    meta.size_stored = static_cast<std::size_t>(std::filesystem::file_size(meta.path, ec));
    if (ec) meta.size_stored = meta.size_plain;
    std::lock_guard<std::mutex> lock(mu_);
    if (cfg_.max_total_bytes > 0 &&
        stored_bytes_ + meta.size_stored > static_cast<std::size_t>(cfg_.max_total_bytes)) {
        std::error_code ec_remove;
        std::filesystem::remove(meta.path, ec_remove);
        if (ec_remove) {
            LOG_WARN << "captured body not removed after quota: " << meta.path << " "
                     << ec_remove.message();
        }
        return std::nullopt;
    }
    stored_bytes_ += meta.size_stored;
    by_request_[request_id].push_back(meta);
    by_id_[meta.id] = meta;
    return meta;
}

std::optional<std::string> BodyStore::get(const std::string& request_id, const std::string& kind) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = by_request_.find(request_id);
        if (it == by_request_.end()) return std::nullopt;
        for (const auto& m : it->second) if (m.kind == kind) { path = m.path; break; }
    }
    if (path.empty()) return std::nullopt;
    std::string error;
    auto body = readAndDecrypt(path, error);
    if (!body) {
        // Причина отказа не теряется: панель и лог называют её, а не «тело не
        // захвачено или истекло» без различия.
        LOG_WARN << "captured body not read: request=" << request_id << " kind=" << kind
                 << " reason=" << error;
    }
    return body;
}

Json::Value BodyStore::metadataJson(const std::string& request_id) const {
    Json::Value arr(Json::arrayValue);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_request_.find(request_id);
    if (it == by_request_.end()) return arr;
    for (const auto& m : it->second) {
        Json::Value x(Json::objectValue);
        x["id"] = m.id;
        x["kind"] = m.kind;
        x["sha256"] = m.sha256;
        x["size_plain"] = static_cast<Json::UInt64>(m.size_plain);
        x["size_stored"] = static_cast<Json::UInt64>(m.size_stored);
        x["truncated"] = m.truncated;
        x["content_type"] = m.content_type;
        arr.append(x);
    }
    return arr;
}

void BodyStore::gc() {
    if (!cfg_.enabled) return;
    const auto cutoff = std::chrono::system_clock::now() -
                        std::chrono::hours(24 * std::max(0, cfg_.retention_days));
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = by_request_.begin(); it != by_request_.end();) {
        auto& vec = it->second;
        for (auto mit = vec.begin(); mit != vec.end();) {
            if (mit->created < cutoff) {
                std::error_code ec;
                std::filesystem::remove(mit->path, ec);
                if (ec) {
                    // Файл остался на диске: счётчик места не должен считать его
                    // удалённым, иначе квота разрешит лишнюю запись.
                    LOG_WARN << "captured body not removed: " << mit->path << " "
                             << ec.message();
                    ++mit;
                    continue;
                }
                stored_bytes_ = stored_bytes_ > mit->size_stored ? stored_bytes_ - mit->size_stored : 0;
                by_id_.erase(mit->id);
                mit = vec.erase(mit);
            } else {
                ++mit;
            }
        }
        if (vec.empty()) it = by_request_.erase(it);
        else ++it;
    }
}

std::size_t BodyStore::storedBytes() const { std::lock_guard<std::mutex> lock(mu_); return stored_bytes_; }
std::size_t BodyStore::count() const { std::lock_guard<std::mutex> lock(mu_); return by_id_.size(); }

} // namespace proxy
