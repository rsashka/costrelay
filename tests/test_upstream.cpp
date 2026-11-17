#include "TestHelpers.h"
#include "TestHttpServer.h"
#include "upstream/CurlClient.h"

#include <gtest/gtest.h>


#include <sstream>

using namespace proxy;
using test::TestHttpServer;
using test::httpResponse;

TEST(Upstream, PostJsonAndHeaders) {
    TestHttpServer server([](const std::string& req) {
        EXPECT_NE(req.find("POST /v1/chat/completions"), std::string::npos);
        EXPECT_NE(req.find("\"model\":\"m/x\""), std::string::npos);
        return httpResponse(R"({"ok":true})");
    });
    CurlHttpRequest req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/chat/completions";
    req.body = R"({"model":"m/x"})";
    req.http2 = false;
    req.verify_tls = false;
    req.headers = {"Content-Type: application/json"};
    req.timeouts.total_ms = 5000;
    req.timeouts.connect_ms = 2000;
    CurlHttpResponse resp;
    CurlClient client;
    bool headers_called = false;
    ASSERT_TRUE(client.perform(req, resp, {}, [&](const CurlHttpResponse&) { headers_called = true; }));
    EXPECT_TRUE(headers_called);
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.body, R"({"ok":true})");
    EXPECT_EQ(resp.headers["x-test"], "yes");
}

TEST(Upstream, StreamingChunkedBody) {
    TestHttpServer server([](const std::string&) {
        std::string body = "5\r\ndata:\r\n7\r\n hello \r\n0\r\n\r\n";
        std::ostringstream os;
        os << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/event-stream\r\n"
           << "Transfer-Encoding: chunked\r\n"
           << "Connection: close\r\n\r\n"
           << body;
        return os.str();
    });
    CurlHttpRequest req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/stream";
    req.body = "{}";
    req.http2 = false;
    req.verify_tls = false;
    req.timeouts.total_ms = 5000;
    req.timeouts.connect_ms = 2000;
    CurlHttpResponse resp;
    CurlClient client;
    std::string got;
    ASSERT_TRUE(client.perform(req, resp, [&](const char* d, std::size_t n) {
        got.append(d, n);
        return true;
    }));
    EXPECT_EQ(resp.status, 200);
    EXPECT_NE(got.find("data:"), std::string::npos);
    EXPECT_NE(got.find("hello"), std::string::npos);
}

TEST(Upstream, CustomMethodWithBody) {
    std::string seen;
    TestHttpServer server([&](const std::string& req) {
        seen = req;
        return std::string("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    });
    CurlHttpRequest req;
    req.method = "DELETE";
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/items/1";
    req.body = "reason=gone";
    req.http2 = false;
    req.verify_tls = false;
    req.headers = {"Content-Type: application/x-www-form-urlencoded"};
    req.timeouts.total_ms = 5000;
    req.timeouts.connect_ms = 2000;
    CurlHttpResponse resp;
    CurlClient client;
    ASSERT_TRUE(client.perform(req, resp));
    EXPECT_EQ(resp.status, 204);
    EXPECT_NE(seen.find("DELETE /items/1 HTTP/1.1"), std::string::npos);
    EXPECT_NE(seen.find("reason=gone"), std::string::npos);
}

TEST(Upstream, HttpErrorReturnsStatusAndBody) {
    TestHttpServer server([](const std::string&) {
        return httpResponse(R"({"error":{"code":"rate_limited"}})", 429);
    });
    CurlHttpRequest req;
    req.method = "POST";
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/err";
    req.body = "{}";
    req.http2 = false;
    req.verify_tls = false;
    req.timeouts.total_ms = 5000;
    req.timeouts.connect_ms = 2000;
    CurlHttpResponse resp;
    CurlClient client;
    EXPECT_FALSE(client.perform(req, resp));
    EXPECT_EQ(resp.status, 429);
    EXPECT_NE(resp.body.find("rate_limited"), std::string::npos);
}

TEST(Upstream, ConnectionFailureIsReported) {
    // Port 1 is almost certainly closed.
    CurlHttpRequest req;
    req.method = "GET";
    req.url = "http://127.0.0.1:1/nope";
    req.http2 = false;
    req.verify_tls = false;
    req.timeouts.total_ms = 1000;
    req.timeouts.connect_ms = 500;
    CurlHttpResponse resp;
    CurlClient client;
    EXPECT_FALSE(client.perform(req, resp));
    EXPECT_FALSE(resp.error.empty());
}

TEST(Upstream, TimeoutIsReported) {
    test::TestHttpServer server([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return test::httpResponse("{}");
    });
    CurlHttpRequest req;
    req.method = "GET";
    req.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/slow";
    req.http2 = false;
    req.verify_tls = false;
    req.timeouts.total_ms = 100;
    req.timeouts.connect_ms = 100;
    CurlHttpResponse resp;
    CurlClient client;
    EXPECT_FALSE(client.perform(req, resp));
    EXPECT_FALSE(resp.error.empty());
}
