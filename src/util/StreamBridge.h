#pragma once

#include <drogon/HttpResponse.h>

#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace proxy {

// Предел очереди по умолчанию: сколько байт незабранных чанков держит мост,
// прежде чем считается, что потребитель отстал. Именован, а не числом по месту.
constexpr std::size_t kStreamBridgeDefaultLimit = 256 * 1024;

// Bridges chunks produced on a worker thread to a Drogon response stream.
class StreamBridge {
  public:
    explicit StreamBridge(std::size_t limit = kStreamBridgeDefaultLimit) : limit_(limit) {}

    void attach(drogon::ResponseStreamPtr stream) {
        std::lock_guard<std::mutex> lock(mu_);
        stream_ = std::shared_ptr<drogon::ResponseStream>(std::move(stream));
        attached_ = true;
        while (!queue_.empty() && stream_) {
            auto chunk = std::move(queue_.front());
            queue_.pop_front();
            if (!stream_->send(chunk)) {
                aborted_ = true;
                break;
            }
        }
        if (closed_ && stream_) {
            stream_->close();
            stream_.reset();
        }
    }

    bool onData(const char* data, std::size_t len) {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_ || aborted_) return false;
        std::string chunk(data, len);
        if (attached_ && stream_) {
            if (!stream_->send(chunk)) {
                aborted_ = true;
                return false;
            }
            return true;
        }
        queued_ += chunk.size();
        if (queued_ > limit_) {
            aborted_ = true;
            return false;
        }
        queue_.push_back(std::move(chunk));
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        if (attached_ && stream_) {
            stream_->close();
            stream_.reset();
        }
    }

    bool aborted() const {
        std::lock_guard<std::mutex> lock(mu_);
        return aborted_;
    }

  private:
    std::size_t limit_;
    mutable std::mutex mu_;
    std::deque<std::string> queue_;
    std::shared_ptr<drogon::ResponseStream> stream_;
    std::size_t queued_ = 0;
    bool attached_ = false;
    bool closed_ = false;
    bool aborted_ = false;
};

} // namespace proxy
