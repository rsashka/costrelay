#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>
#include <thread>

namespace test {

inline std::string wsBase64(const unsigned char* data, std::size_t len) {
    std::string out(4 * ((len + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data,
                                  static_cast<int>(len));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

inline bool readExact(int fd, void* data, std::size_t len) {
    auto* p = static_cast<char*>(data);
    while (len > 0) {
        const ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

inline bool readWsFrame(int fd, std::uint8_t& opcode, std::string& payload) {
    unsigned char head[2];
    if (!readExact(fd, head, sizeof(head))) return false;
    opcode = static_cast<std::uint8_t>(head[0] & 0x0f);
    const bool masked = (head[1] & 0x80) != 0;
    std::uint64_t len = head[1] & 0x7f;
    if (len == 126) {
        unsigned char ext[2];
        if (!readExact(fd, ext, sizeof(ext))) return false;
        len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (!readExact(fd, ext, sizeof(ext))) return false;
        len = 0;
        for (unsigned char c : ext) len = (len << 8) | c;
    }
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && !readExact(fd, mask, sizeof(mask))) return false;
    payload.assign(static_cast<std::size_t>(len), '\0');
    if (len > 0 && !readExact(fd, payload.data(), payload.size())) return false;
    if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
    }
    return true;
}

inline bool sendWsFrame(int fd, std::uint8_t opcode, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xffff) {
        out.push_back(126);
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        out.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((payload.size() >> (i * 8)) & 0xff));
        }
    }
    out += payload;
    return send(fd, out.data(), out.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(out.size());
}

// Minimal WebSocket echo server used to exercise the portal WS proxy.
class TestWsServer {
  public:
    TestWsServer() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(fd_, 4);
        socklen_t len = sizeof(addr);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serveLoop(); });
    }
    ~TestWsServer() { stop(); }
    uint16_t port() const { return port_; }
    void stop() {
        if (!stopped_.exchange(true)) {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            if (thread_.joinable()) thread_.join();
        }
    }

  private:
    void serveLoop() {
        while (!stopped_.load()) {
            const int client = accept(fd_, nullptr, nullptr);
            if (client < 0) return;
            serveClient(client);
            close(client);
        }
    }

    void serveClient(int client) {
        timeval tv{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return;
            request.append(buf, static_cast<std::size_t>(n));
        }
        std::string lower = request;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string marker = "sec-websocket-key:";
        std::string key;
        const auto pos = lower.find(marker);
        if (pos != std::string::npos) {
            const auto end = request.find("\r\n", pos);
            key = request.substr(pos + marker.size(), end - pos - marker.size());
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(0, 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        }
        if (key.empty()) return;
        unsigned char digest[SHA_DIGEST_LENGTH];
        const std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        SHA1(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest);
        const std::string accept = wsBase64(digest, sizeof(digest));
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (send(client, response.data(), response.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(response.size())) return;
        while (!stopped_.load()) {
            std::uint8_t opcode = 0;
            std::string payload;
            if (!readWsFrame(client, opcode, payload)) return;
            if (opcode == 0x8) {
                sendWsFrame(client, 0x8, {});
                return;
            }
            if (opcode == 0x9) {
                sendWsFrame(client, 0xA, payload);
                continue;
            }
            if (!sendWsFrame(client, opcode, payload)) return;
        }
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

} // namespace test
