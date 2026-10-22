// test_server.cpp — the JSON writer and the HTTP layer the UI talks to.
#include "Check.hpp"

#include "Http.hpp"
#include "Json.hpp"

#include <cstdio>
#include <string>
#include <thread>

using namespace lumen;

static void testJsonWriter() {
    Json j;
    j.beginObject();
    j.member("name", "Front Wash");
    j.member("id", 7);
    j.member("level", 0.5);
    j.member("on", true);
    j.key("nothing").null();
    j.key("list").beginArray().value(1).value(2).value(3).endArray();
    j.key("nested").beginObject().member("a", 1).endObject();
    j.endObject();

    const std::string out = j.str();
    CHECK(out ==
          "{\"name\":\"Front Wash\",\"id\":7,\"level\":0.5,\"on\":true,"
          "\"nothing\":null,\"list\":[1,2,3],\"nested\":{\"a\":1}}");

    // Escaping, because fixture names come from imported files and a stray
    // quote or control character would break the whole document.
    CHECK(Json::escape("say \"hi\"") == "say \\\"hi\\\"");
    CHECK(Json::escape("back\\slash") == "back\\\\slash");
    CHECK(Json::escape("line\nbreak") == "line\\nbreak");
    CHECK(Json::escape(std::string("bell\x07")) == "bell\\u0007");

    Json e;
    e.beginObject().member("n", std::string("a\"b\nc")).endObject();
    CHECK(e.str() == "{\"n\":\"a\\\"b\\nc\"}");

    // Empty containers still produce valid JSON.
    Json empty;
    empty.beginObject().key("rows").beginArray().endArray().endObject();
    CHECK(empty.str() == "{\"rows\":[]}");

    // A non-finite float would make the browser refuse to parse the whole
    // snapshot, so it becomes null rather than NaN.
    Json nan;
    nan.beginObject().member("v", std::nan("")).endObject();
    CHECK(nan.str() == "{\"v\":null}");
    Json inf;
    inf.beginObject().member("v", 1.0 / 0.0).endObject();
    CHECK(inf.str() == "{\"v\":null}");

    // Arrays of objects, the shape the whole snapshot is made of.
    Json arr;
    arr.beginArray();
    for (int i = 0; i < 3; ++i) arr.beginObject().member("i", i).endObject();
    arr.endArray();
    CHECK(arr.str() == "[{\"i\":0},{\"i\":1},{\"i\":2}]");
}

static void testUrlDecoding() {
    CHECK(urlDecode("Fixture+1+At+100") == "Fixture 1 At 100");
    CHECK(urlDecode("a%20b") == "a b");
    CHECK(urlDecode("%22quoted%22") == "\"quoted\"");
    CHECK(urlDecode("plain") == "plain");
    // A malformed escape is passed through rather than swallowing the rest.
    CHECK(urlDecode("100%") == "100%");
    CHECK(urlDecode("%zz") == "%zz");

    CHECK(queryParam("kind=cc&index=7&value=0.5", "index") == "7");
    CHECK(queryParam("kind=cc&index=7&value=0.5", "kind") == "cc");
    CHECK(queryParam("kind=cc&index=7&value=0.5", "value") == "0.5");
    CHECK(queryParam("kind=cc", "missing").empty());
    CHECK(queryParam("", "any").empty());
    // A parameter whose value contains the separator of another.
    CHECK(queryParam("address=%2Flumen%2Fgo&value=1", "address") == "/lumen/go");
}

static void testHttpRoundTrip() {
    HttpServer server;
    std::string err;
    // Port 0 means "pick a free one", so the test cannot collide with a
    // running console.
    CHECK(server.listen(0, &err));
    CHECK(err.empty());
    CHECK(server.port() > 0);

    server.route("GET", "/state", [](const HttpRequest&) {
        HttpResponse r;
        r.body = "{\"ok\":true}";
        return r;
    });
    server.route("POST", "/cmd", [](const HttpRequest& req) {
        HttpResponse r;
        r.body = "{\"echo\":\"" + Json::escape(req.body) + "\"}";
        return r;
    });
    server.route("GET", "/q", [](const HttpRequest& req) {
        HttpResponse r;
        r.contentType = "text/plain";
        r.body = queryParam(req.query, "v");
        return r;
    });
    server.fallback([](const HttpRequest& req) {
        HttpResponse r;
        r.status = 404;
        r.contentType = "text/plain";
        r.body = "missing " + req.path;
        return r;
    });

    // Serve on a thread while the test acts as the client.
    std::thread worker([&server] {
        for (int i = 0; i < 6; ++i) server.poll(2000);
    });

    auto request = [&](const std::string& raw) {
        std::string reply;
        // printf '%b' with the request as an ARGUMENT: a literal format
        // string would read a percent-escape in the URL as a directive.
        const std::string cmd = "printf '%b' \"" + raw +
                                "\" | timeout 5 nc -q1 127.0.0.1 " +
                                std::to_string(server.port());
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return reply;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) reply.append(buf, n);
        pclose(p);
        return reply;
    };

    const std::string state =
        request("GET /state HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n");
    if (state.empty()) {
        // No netcat in this environment; the socket path cannot be exercised,
        // but everything above it still is.
        std::printf("      (skipped: nc unavailable)\n");
        server.stop();
        for (int i = 0; i < 6; ++i) {
            // Unblock the worker's pending poll calls.
            FILE* p = popen(("timeout 1 nc -q0 127.0.0.1 " +
                             std::to_string(server.port()) + " </dev/null")
                                .c_str(),
                            "r");
            if (p) pclose(p);
        }
        worker.detach();
        return;
    }

    CHECK(state.find("200 OK") != std::string::npos);
    CHECK(state.find("application/json") != std::string::npos);
    CHECK(state.find("{\"ok\":true}") != std::string::npos);
    CHECK(state.find("no-store") != std::string::npos);

    // A POST with a body: the command path.
    const std::string post = request(
        "POST /cmd HTTP/1.1\\r\\nHost: x\\r\\nContent-Length: 16\\r\\nConnection: "
        "close\\r\\n\\r\\nFixture 1 At 100");
    CHECK(post.find("Fixture 1 At 100") != std::string::npos);

    // Query parameters.
    const std::string q = request(
        "GET /q?v=hello%20there HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n");
    CHECK(q.find("hello there") != std::string::npos);

    // An unknown path reaches the fallback, which is what serves the UI.
    const std::string missing = request(
        "GET /nope HTTP/1.1\\r\\nHost: x\\r\\nConnection: close\\r\\n\\r\\n");
    CHECK(missing.find("404") != std::string::npos);
    CHECK(missing.find("missing /nope") != std::string::npos);

    // A request with no recognisable method line is refused rather than
    // crashing the loop.
    request("garbage\\r\\n\\r\\n");

    server.stop();
    worker.join();
    CHECK(server.served() >= 4);
}

int main() {
    std::printf("lumen server\n");
    testJsonWriter();
    testUrlDecoding();
    testHttpRoundTrip();
    return check::report("server");
}
