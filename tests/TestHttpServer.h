#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

namespace test {

class TestHttpServer {
  public:
    using Responder = std::function<std::string(const std::string&)>;
    explicit TestHttpServer(Responder responder) : responder_(std::move(responder)) {
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
    ~TestHttpServer() { stop(); }
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
            int client = accept(fd_, nullptr, nullptr);
            if (client < 0) return;
            serveClient(client);
            close(client);
            if (stopped_.load()) return;
        }
    }

    void serveClient(int client) {
        timeval tv{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::string data;
        char buf[4096];
        while (data.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, static_cast<size_t>(n));
        }
        size_t head_end = data.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            size_t content_length = 0;
            auto pos = data.find("Content-Length:");
            if (pos != std::string::npos && pos < head_end) {
                content_length = static_cast<size_t>(std::stoul(data.substr(pos + 15)));
            }
            size_t body_start = head_end + 4;
            while (data.size() < body_start + content_length) {
                ssize_t n = recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                data.append(buf, static_cast<size_t>(n));
            }
        }
        std::string response = responder_(data);
        send(client, response.data(), response.size(), MSG_NOSIGNAL);
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    Responder responder_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

inline std::string httpResponse(const std::string& body, int status = 200) {
    std::ostringstream os;
    os << "HTTP/1.1 " << status << " OK\r\n"
       << "Content-Type: application/json\r\n"
       << "X-Test: yes\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return os.str();
}


} // namespace test
