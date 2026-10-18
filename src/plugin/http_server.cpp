#include "http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace joc_mpv {
namespace {

bool winsock_ready() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

std::string lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 416: return "Range Not Satisfiable";
        default: return "Internal Server Error";
    }
}

bool send_all(SOCKET socket, const char* data, std::size_t size) {
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size, 1u << 20));
        const int sent = ::send(socket, data, chunk, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

}  // namespace

bool ResponseWriter::write(const void* data, std::size_t size) {
    if (!ok_) return false;
    char header[32];
    const int header_length = std::snprintf(header, sizeof(header), "%zx\r\n", size);
    if (!send_all(static_cast<SOCKET>(socket_), header, static_cast<std::size_t>(header_length)) ||
        !send_all(static_cast<SOCKET>(socket_), static_cast<const char*>(data), size) ||
        !send_all(static_cast<SOCKET>(socket_), "\r\n", 2)) {
        ok_ = false;
        return false;
    }
    return true;
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(HttpHandler handler, unsigned short* port, std::string* error) {
    if (running_) return true;
    if (!winsock_ready()) {
        if (error != nullptr) *error = "Winsock failed to initialize";
        return false;
    }
    handler_ = std::move(handler);
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (error != nullptr) *error = "cannot create the audio socket";
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 8) != 0) {
        closesocket(listener);
        if (error != nullptr) *error = "cannot bind the loopback audio port";
        return false;
    }
    sockaddr_in bound{};
    int length = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) == 0 && port != nullptr) {
        *port = ntohs(bound.sin_port);
    }
    listener_ = static_cast<std::uintptr_t>(listener);
    running_ = true;
    thread_ = std::thread(&HttpServer::accept_loop, this);
    return true;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
    const SOCKET listener = static_cast<SOCKET>(listener_);
    listener_ = ~static_cast<std::uintptr_t>(0);
    closesocket(listener);
    if (thread_.joinable()) thread_.join();
    // Connection threads end when their socket fails or the source is closed;
    // joining them keeps the handler and the source alive until then.
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    handler_ = nullptr;
}

void HttpServer::accept_loop() {
    const SOCKET listener = static_cast<SOCKET>(listener_);
    while (running_) {
        const SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        workers_.emplace_back(&HttpServer::serve, this, static_cast<std::uintptr_t>(client));
    }
}

void HttpServer::serve(std::uintptr_t client_value) {
    const SOCKET client = static_cast<SOCKET>(client_value);
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const int got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            closesocket(client);
            return;
        }
        request.append(buffer, static_cast<std::size_t>(got));
        if (request.size() > 16384) {
            closesocket(client);
            return;
        }
    }

    HttpRequest parsed;
    std::size_t line_end = request.find("\r\n");
    if (line_end != std::string::npos) {
        const std::string line = request.substr(0, line_end);
        const std::size_t first = line.find(' ');
        const std::size_t second = (first == std::string::npos) ? std::string::npos
                                                                : line.find(' ', first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            parsed.method = line.substr(0, first);
            parsed.target = line.substr(first + 1, second - first - 1);
        }
        std::size_t position = line_end + 2;
        while (position < request.size()) {
            const std::size_t end = request.find("\r\n", position);
            if (end == std::string::npos) break;
            const std::string header_line = request.substr(position, end - position);
            position = end + 2;
            if (header_line.empty()) break;
            const std::size_t colon = header_line.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = lower(header_line.substr(0, colon));
            std::string value = header_line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(0, 1);
            }
            if (name != "range" || value.compare(0, 6, "bytes=") != 0) continue;
            value.erase(0, 6);
            const std::size_t dash = value.find('-');
            if (dash == std::string::npos) continue;
            const std::string first_text = value.substr(0, dash);
            const std::string last_text = value.substr(dash + 1);
            if (first_text.empty()) continue;  // suffix ranges are not used by mpv
            parsed.has_range = true;
            parsed.range_start = std::strtoull(first_text.c_str(), nullptr, 10);
            if (!last_text.empty()) {
                parsed.has_range_end = true;
                parsed.range_end = std::strtoull(last_text.c_str(), nullptr, 10);
            }
        }
    }

    HttpResponse response;
    if (handler_) handler_(parsed, &response);
    else response.status = 404;

    std::string head;
    head += "HTTP/1.1 " + std::to_string(response.status) + " " + status_text(response.status) +
            "\r\n";
    head += response.headers;
    if (response.stream) {
        head += "Transfer-Encoding: chunked\r\n";
    } else {
        head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";

    if (send_all(client, head.data(), head.size())) {
        if (response.stream) {
            ResponseWriter writer(static_cast<std::uintptr_t>(client));
            response.stream(writer);
            send_all(client, "0\r\n\r\n", 5);
        } else {
            send_all(client, response.body.data(), response.body.size());
        }
    }
    closesocket(client);
}

}  // namespace joc_mpv
