#include "Http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

namespace lumen {

namespace {

std::string statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

// Reads until the headers are complete, then the declared body. A browser can
// split a request across packets, so a single read() is not enough.
bool readRequest(int fd, std::string& out) {
    char buf[4096];
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;

    while (true) {
        if (headerEnd == std::string::npos) {
            headerEnd = out.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                const std::string headers = out.substr(0, headerEnd);
                // Header names are case-insensitive, and browsers do vary.
                std::string lower = headers;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                const size_t at = lower.find("content-length:");
                if (at != std::string::npos) {
                    try {
                        contentLength = static_cast<size_t>(
                            std::stoul(headers.substr(at + 15)));
                    } catch (...) {
                        contentLength = 0;
                    }
                }
            }
        }
        if (headerEnd != std::string::npos &&
            out.size() >= headerEnd + 4 + contentLength)
            return true;

        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return headerEnd != std::string::npos;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 8u * 1024u * 1024u) return false;   // not a real request
    }
}

// A client that hangs up mid-write must not take the process with it.
// Linux asks for the flag on every send; macOS and the BSDs set it once on
// the socket instead and have no such flag, so a bare MSG_NOSIGNAL does not
// compile there at all.
#ifndef MSG_NOSIGNAL
#define LUMEN_MSG_NOSIGNAL 0
#else
#define LUMEN_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

void suppressSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

void sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n =
            ::send(fd, data.data() + sent, data.size() - sent, LUMEN_MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

} // namespace

HttpServer::~HttpServer() {
    if (fd_ >= 0) ::close(fd_);
}

bool HttpServer::listen(int port, std::string* err) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        if (err) *err = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // LOOPBACK ONLY. A lighting console that answers to the network by
    // default is a hazard (§18.9).
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (err)
            *err = "cannot bind port " + std::to_string(port) + ": " +
                   std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (::listen(fd_, 16) < 0) {
        if (err) *err = std::string("listen: ") + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Port 0 means "pick one", which is what a test wants.
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        port_ = ntohs(addr.sin_port);
    else
        port_ = port;

    running_ = true;
    return true;
}

void HttpServer::route(const std::string& method, const std::string& path, Handler h) {
    routes_[method + " " + path] = std::move(h);
}

HttpResponse HttpServer::dispatch(const HttpRequest& req) const {
    const auto it = routes_.find(req.method + " " + req.path);
    if (it != routes_.end()) return it->second(req);
    if (fallback_) return fallback_(req);
    HttpResponse r;
    r.status = 404;
    r.contentType = "text/plain";
    r.body = "not found: " + req.path;
    return r;
}

bool HttpServer::poll(int timeoutMs) {
    if (fd_ < 0) return false;
    pollfd p{};
    p.fd = fd_;
    p.events = POLLIN;
    const int n = ::poll(&p, 1, timeoutMs);
    if (n <= 0) return false;

    const int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) return false;
    suppressSigpipe(client);

    std::string raw;
    if (!readRequest(client, raw)) {
        ::close(client);
        return false;
    }

    HttpRequest req;
    {
        const size_t lineEnd = raw.find("\r\n");
        const std::string line = raw.substr(0, lineEnd == std::string::npos
                                                   ? raw.size()
                                                   : lineEnd);
        const size_t sp1 = line.find(' ');
        const size_t sp2 = line.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            req.method = line.substr(0, sp1);
            std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            const size_t q = target.find('?');
            if (q != std::string::npos) {
                req.query = target.substr(q + 1);
                target = target.substr(0, q);
            }
            req.path = target;
        }
        const size_t bodyAt = raw.find("\r\n\r\n");
        if (bodyAt != std::string::npos) req.body = raw.substr(bodyAt + 4);
    }

    HttpResponse res = req.method.empty() ? HttpResponse{400, "text/plain",
                                                         "bad request", true}
                                          : dispatch(req);

    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " +
                      statusText(res.status) + "\r\n";
    out += "Content-Type: " + res.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    if (res.noCache) out += "Cache-Control: no-store\r\n";
    // The UI is served from the same origin, so no CORS is needed; this is
    // only here for a separately-hosted UI during development.
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Connection: close\r\n\r\n";
    out += res.body;

    sendAll(client, out);
    ::close(client);
    ++served_;
    return true;
}

void HttpServer::run() {
    while (running_) poll(200);
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
            out += in[i];
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string queryParam(const std::string& query, const std::string& key) {
    size_t at = 0;
    while (at < query.size()) {
        size_t amp = query.find('&', at);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(at, amp - at);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return urlDecode(pair.substr(eq + 1));
        at = amp + 1;
    }
    return {};
}

} // namespace lumen
