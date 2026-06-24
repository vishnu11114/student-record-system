// server/ws_server.h
// A small thread-per-connection WebSocket server.
// Not designed for tens of thousands of clients, but very simple to reason
// about and adequate for this project. Broadcasts are protected by a mutex.
#pragma once

#include "../common/ws_protocol.h"
#include "logger.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

class WsServer {
public:
    using MessageHandler = std::function<void(sock_t clientFd, const std::string&)>;

    explicit WsServer(uint16_t port) : port_(port) {}

    ~WsServer() { stop(); }

    void onMessage(MessageHandler h) { handler_ = std::move(h); }

    bool start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ == NET_INVALID_SOCK) {
            logger::error("socket() failed");
            return false;
        }
        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            logger::error("bind() failed on port " + std::to_string(port_));
            net::close(listenFd_);
            listenFd_ = NET_INVALID_SOCK;
            return false;
        }
        if (::listen(listenFd_, 32) < 0) {
            logger::error("listen() failed");
            net::close(listenFd_);
            listenFd_ = NET_INVALID_SOCK;
            return false;
        }
        running_ = true;
        acceptThread_ = std::thread(&WsServer::acceptLoop, this);
        logger::info("WebSocket server listening on 0.0.0.0:" + std::to_string(port_));
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (listenFd_ != NET_INVALID_SOCK) {
            ::shutdown(listenFd_, SHUT_RDWR);
            net::close(listenFd_);
            listenFd_ = NET_INVALID_SOCK;
        }
        if (acceptThread_.joinable()) acceptThread_.join();
        // Close all client sockets so their threads exit.
        std::set<sock_t> snap;
        {
            std::lock_guard<std::mutex> lk(clientsMutex_);
            snap = clients_;
        }
        for (sock_t fd : snap) ::shutdown(fd, SHUT_RDWR);
        // Detached worker threads will exit on their own.
    }

    // Broadcast `text` to every connected client. Returns ms taken.
    double broadcast(const std::string& text) {
        auto t0 = std::chrono::steady_clock::now();
        std::set<sock_t> snap;
        {
            std::lock_guard<std::mutex> lk(clientsMutex_);
            snap = clients_;
        }
        for (sock_t fd : snap) {
            if (!ws::sendText(fd, text, /*mask=*/false)) {
                std::lock_guard<std::mutex> lk(clientsMutex_);
                clients_.erase(fd);
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        lastBroadcastMs_ = ms;
        return ms;
    }

    // Send to a single client only.
    bool sendTo(sock_t fd, const std::string& text) {
        return ws::sendText(fd, text, /*mask=*/false);
    }

    size_t clientCount() const {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        return clients_.size();
    }

    double lastBroadcastMs() const { return lastBroadcastMs_; }

private:
    void acceptLoop() {
        while (running_) {
            sockaddr_in cliAddr{};
            socklen_t cliLen = sizeof(cliAddr);
            sock_t fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&cliAddr), &cliLen);
            if (fd == NET_INVALID_SOCK) {
                if (!running_) break;
                logger::warn("accept() returned error, continuing");
                continue;
            }
            // Hand off to a detached worker thread.
            std::thread(&WsServer::handleClient, this, fd).detach();
        }
    }

    void handleClient(sock_t fd) {
        if (!performHandshake(fd)) {
            net::close(fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(clientsMutex_);
            clients_.insert(fd);
        }
        logger::info("Client connected fd=" + std::to_string(static_cast<long long>(fd)) +
                     " (total=" + std::to_string(clientCount()) + ")");

        // Read loop
        std::string assembled;
        ws::Opcode assemblingOp = ws::Opcode::Text;
        while (running_) {
            ws::Frame frame;
            if (!ws::readFrame(fd, frame)) break;
            switch (frame.opcode) {
                case ws::Opcode::Ping: {
                    ws::Frame pong;
                    pong.opcode = ws::Opcode::Pong;
                    pong.payload = frame.payload;
                    ws::writeFrame(fd, pong, false);
                    break;
                }
                case ws::Opcode::Pong:
                    break;
                case ws::Opcode::Close: {
                    ws::sendClose(fd, false);
                    goto done;
                }
                case ws::Opcode::Text:
                case ws::Opcode::Binary: {
                    assembled = std::move(frame.payload);
                    assemblingOp = frame.opcode;
                    if (frame.fin) {
                        if (handler_) handler_(fd, assembled);
                        assembled.clear();
                    }
                    break;
                }
                case ws::Opcode::Continuation: {
                    assembled.append(frame.payload);
                    if (frame.fin) {
                        if (handler_ && assemblingOp == ws::Opcode::Text)
                            handler_(fd, assembled);
                        assembled.clear();
                    }
                    break;
                }
            }
        }
done:
        {
            std::lock_guard<std::mutex> lk(clientsMutex_);
            clients_.erase(fd);
        }
        net::close(fd);
        logger::info("Client disconnected fd=" + std::to_string(static_cast<long long>(fd)) +
                     " (total=" + std::to_string(clientCount()) + ")");
    }

    bool performHandshake(sock_t fd) {
        std::string req;
        if (!ws::recvHttpRequest(fd, req)) return false;
        std::string key = ws::headerValue(req, "Sec-WebSocket-Key");
        std::string upgrade = ws::headerValue(req, "Upgrade");
        if (key.empty() || upgrade.find("websocket") == std::string::npos) {
            std::string body = "WebSocket endpoint. Use ws:// to connect.";
            std::string resp =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            net::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
            return false;
        }
        std::string accept = ws::acceptKey(key);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        return net::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(resp.size());
    }

    uint16_t port_;
    sock_t listenFd_ = NET_INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    mutable std::mutex clientsMutex_;
    std::set<sock_t> clients_;
    MessageHandler handler_;
    std::atomic<double> lastBroadcastMs_{0.0};
};
