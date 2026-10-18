#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace joc_mpv {

struct HttpRequest {
    std::string method;
    std::string target;
    bool has_range = false;
    std::uint64_t range_start = 0;
    std::uint64_t range_end = 0;   // inclusive
    bool has_range_end = false;
};

// Writer handed to a streaming response: every write() becomes one HTTP chunk.
class ResponseWriter {
public:
    explicit ResponseWriter(std::uintptr_t socket) : socket_(socket) {}
    bool write(const void* data, std::size_t size);
    bool ok() const { return ok_; }

private:
    std::uintptr_t socket_;
    bool ok_ = true;
};

struct HttpResponse {
    int status = 200;
    std::string headers;  // one "Name: value\r\n" per line
    std::string body;     // used when stream is empty
    // Set for a chunked response: the server sends the headers, then calls this
    // with a writer that frames the data, then terminates the chunked body.
    std::function<void(ResponseWriter&)> stream;
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse*)>;

// Minimal HTTP/1.1 server on 127.0.0.1 serving one resource with byte ranges.
// A seekable, continuously streaming audio source is what lets mpv's own
// demuxer, cache, A/V sync, WASAPI output and seeking keep working unmodified.
class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(HttpHandler handler, unsigned short* port, std::string* error);
    void stop();

private:
    void accept_loop();
    void serve(std::uintptr_t client_socket);
    HttpHandler handler_;
    std::uintptr_t listener_ = ~static_cast<std::uintptr_t>(0);
    std::thread thread_;
    std::vector<std::thread> workers_;
    bool running_ = false;
};

}  // namespace joc_mpv
