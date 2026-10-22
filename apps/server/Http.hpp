// Http.hpp — a minimal HTTP/1.1 server, enough to serve one UI and a JSON API.
//
// Dependency-free on purpose: the console should build with a compiler and
// CMake and nothing else. This handles one connection at a time, which is
// right for a local UI and would be wrong for anything else — it is not a web
// server, it is a window that happens to be rendered by a browser.
//
// Bound to loopback only. A lighting console that answers to the network by
// default is a hazard, and §18.9 makes interface binding a requirement rather
// than an option.
#pragma once

#include <functional>
#include <map>
#include <string>

namespace lumen {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    // Served from memory, and never cached: the point of the state endpoint
    // is that it is current.
    bool noCache = true;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer();

    // Loopback only. Returns false with a reason rather than throwing, so a
    // port already in use is a message and not a crash.
    bool listen(int port, std::string* err);
    int port() const { return port_; }

    void route(const std::string& method, const std::string& path, Handler h);
    // Anything unmatched, for serving the UI's files.
    void fallback(Handler h) { fallback_ = std::move(h); }

    // Serves until stop() is called from another thread. One request at a
    // time, in order.
    void run();
    // Serves at most one request, waiting up to `timeoutMs`. Returns false on
    // timeout. This is what makes the server testable without a thread.
    bool poll(int timeoutMs);
    void stop() { running_ = false; }
    bool running() const { return running_; }

    unsigned long long served() const { return served_; }

private:
    HttpResponse dispatch(const HttpRequest& req) const;

    int fd_ = -1;
    int port_ = 0;
    bool running_ = false;
    unsigned long long served_ = 0;
    std::map<std::string, Handler> routes_;   // "METHOD path"
    Handler fallback_;
};

// Percent-decoding, for command strings arriving in a query string.
std::string urlDecode(const std::string& in);
// One parameter out of a query string, or empty.
std::string queryParam(const std::string& query, const std::string& key);

} // namespace lumen
