// server/main.cpp
// Student Record Management System - Server
//
// Responsibilities:
//   1. Load students.csv into memory on startup.
//   2. Accept WebSocket connections (browser + C++ clients) on port 9001.
//   3. Translate JSON CRUD messages into store operations.
//   4. Broadcast change events to all connected clients.
//   5. Persist changes back to CSV after every mutation.
//   6. Serve the HTML view on port 8080 so the browser can pick it up
//      without needing a separate web server.

#include "../common/json.h"
#include "csv_handler.h"
#include "../common/net_compat.h"
#include "logger.h"
#include "student.h"
#include "student_store.h"
#include "ws_server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

std::string snapshotJson(const std::vector<Student>& list) {
    json::Object o;
    o["event"] = json::Value(std::string("snapshot"));
    json::Array arr;
    arr.reserve(list.size());
    for (const auto& s : list) arr.push_back(s.toJson());
    o["students"] = json::Value(std::move(arr));
    o["count"] = json::Value(static_cast<int>(list.size()));
    return json::Value(std::move(o)).dump();
}

std::string eventJson(const std::string& name, const Student& s) {
    json::Object o;
    o["event"] = json::Value(name);
    o["student"] = s.toJson();
    return json::Value(std::move(o)).dump();
}

std::string deletedJson(int id) {
    json::Object o;
    o["event"] = json::Value(std::string("deleted"));
    o["id"] = json::Value(id);
    return json::Value(std::move(o)).dump();
}

std::string errorJson(const std::string& message, const std::string& reqId = "") {
    json::Object o;
    o["event"] = json::Value(std::string("error"));
    o["message"] = json::Value(message);
    if (!reqId.empty()) o["requestId"] = json::Value(reqId);
    return json::Value(std::move(o)).dump();
}

std::string okJson(const std::string& op, const std::string& reqId = "") {
    json::Object o;
    o["event"] = json::Value(std::string("ack"));
    o["op"] = json::Value(op);
    if (!reqId.empty()) o["requestId"] = json::Value(reqId);
    return json::Value(std::move(o)).dump();
}

// -------- Tiny HTTP file server for the HTML view ------------------------
// Single-purpose: serve view/index.html on GET / so the browser can load
// the UI without needing nginx or python -m http.server.
void httpServerLoop(uint16_t port, const std::string& htmlPath) {
    sock_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == NET_INVALID_SOCK) {
        logger::error("HTTP socket() failed");
        return;
    }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger::error("HTTP bind() failed on port " + std::to_string(port));
        net::close(fd);
        return;
    }
    if (::listen(fd, 16) < 0) {
        logger::error("HTTP listen() failed");
        net::close(fd);
        return;
    }
    logger::info("HTTP view server listening on http://localhost:" +
                 std::to_string(port));

    while (g_running) {
        sockaddr_in cli{};
        socklen_t cl = sizeof(cli);
        sock_t c = ::accept(fd, reinterpret_cast<sockaddr*>(&cli), &cl);
        if (c == NET_INVALID_SOCK) {
            if (!g_running) break;
            continue;
        }
        std::thread([c, htmlPath]() {
            std::string req;
            char buf[2048];
            ssize_t n = net::recv(c, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                req.assign(buf, n);
            }
            std::ifstream f(htmlPath, std::ios::binary);
            std::string body;
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                body = ss.str();
            } else {
                body = "<h1>view/index.html not found</h1>";
            }
            std::string resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body;
            net::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
            net::close(c);
        }).detach();
    }
    net::close(fd);
}

} // namespace

int main(int argc, char** argv) {
    net::Init netInit;  // WSAStartup on Windows, no-op on POSIX
    if (!netInit.ok()) {
        std::cerr << "Network init failed\n";
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);  // not a thing on Windows
#endif

    std::string csvPath  = "data/students.csv";
    std::string htmlPath = "view/index.html";
    uint16_t wsPort   = 9001;
    uint16_t httpPort = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) csvPath = argv[++i];
        else if (a == "--html" && i + 1 < argc) htmlPath = argv[++i];
        else if (a == "--ws-port" && i + 1 < argc) wsPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--http-port" && i + 1 < argc) httpPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--csv FILE] [--html FILE]"
                      << " [--ws-port N] [--http-port N]\n";
            return 0;
        }
    }

    auto repo = std::make_unique<csv::CsvStudentRepository>(csvPath);
    StudentStore store(std::move(repo));
    store.load();

    WsServer wsServer(wsPort);

    auto sendSnapshotTo = [&](sock_t fd) {
        std::string snap = snapshotJson(store.all());
        wsServer.sendTo(fd, snap);
    };

    auto broadcastSnapshot = [&]() {
        std::string snap = snapshotJson(store.all());
        double ms = wsServer.broadcast(snap);
        logger::info("Broadcast snapshot to " +
                     std::to_string(wsServer.clientCount()) +
                     " client(s) in " + std::to_string(ms) + " ms");
    };

    wsServer.onMessage([&](sock_t fd, const std::string& payload) {
        json::Value msg;
        try {
            msg = json::parse(payload);
        } catch (const std::exception& e) {
            wsServer.sendTo(fd, errorJson(std::string("bad JSON: ") + e.what()));
            return;
        }
        std::string op    = msg.getString("op", "");
        std::string reqId = msg.getString("requestId", "");

        if (op == "list") {
            sendSnapshotTo(fd);
            return;
        }

        if (op == "search") {
            std::string by = msg.getString("by", "name");
            std::string val = msg.getString("value", "");
            json::Object o;
            o["event"] = json::Value(std::string("search_result"));
            if (!reqId.empty()) o["requestId"] = json::Value(reqId);
            json::Array results;
            if (by == "id") {
                try {
                    int id = std::stoi(val);
                    auto s = store.find(id);
                    if (s) results.push_back(s->toJson());
                } catch (...) {}
            } else {
                for (auto& s : store.searchByName(val))
                    results.push_back(s.toJson());
            }
            o["students"] = json::Value(std::move(results));
            wsServer.sendTo(fd, json::Value(std::move(o)).dump());
            return;
        }

        if (op == "sort") {
            std::string by = msg.getString("by", "name");
            std::string order = msg.getString("order", "asc");
            auto sorted = store.sorted(by, order == "asc");
            json::Object o;
            o["event"] = json::Value(std::string("sorted"));
            o["by"] = json::Value(by);
            o["order"] = json::Value(order);
            if (!reqId.empty()) o["requestId"] = json::Value(reqId);
            json::Array arr;
            for (auto& s : sorted) arr.push_back(s.toJson());
            o["students"] = json::Value(std::move(arr));
            o["sortMs"] = json::Value(store.lastSortMs());
            wsServer.sendTo(fd, json::Value(std::move(o)).dump());
            return;
        }

        if (op == "create") {
            Student s = Student::fromJson(msg.at("student"));
            std::string err;
            if (!store.create(s, err)) {
                wsServer.sendTo(fd, errorJson(err, reqId));
                return;
            }
            logger::info("CREATE id=" + std::to_string(s.id) + " name=" + s.name);
            wsServer.sendTo(fd, okJson("create", reqId));
            wsServer.broadcast(eventJson("created", s));
            return;
        }

        if (op == "update") {
            Student s = Student::fromJson(msg.at("student"));
            std::string err;
            if (!store.update(s, err)) {
                wsServer.sendTo(fd, errorJson(err, reqId));
                return;
            }
            logger::info("UPDATE id=" + std::to_string(s.id) + " name=" + s.name);
            wsServer.sendTo(fd, okJson("update", reqId));
            wsServer.broadcast(eventJson("updated", s));
            return;
        }

        if (op == "delete") {
            int id = msg.getInt("id", -1);
            std::string err;
            if (!store.remove(id, err)) {
                wsServer.sendTo(fd, errorJson(err, reqId));
                return;
            }
            logger::info("DELETE id=" + std::to_string(id));
            wsServer.sendTo(fd, okJson("delete", reqId));
            wsServer.broadcast(deletedJson(id));
            return;
        }

        if (op == "bulk") {
            std::vector<Student> incoming;
            if (msg.has("students") && msg.at("students").isArray()) {
                for (auto& v : msg.at("students").asArray())
                    incoming.push_back(Student::fromJson(v));
            }
            store.replaceAll(incoming);
            logger::info("BULK loaded " + std::to_string(incoming.size()) +
                         " records from client");
            wsServer.sendTo(fd, okJson("bulk", reqId));
            broadcastSnapshot();
            return;
        }

        if (op == "stats") {
            json::Object o;
            o["event"] = json::Value(std::string("stats"));
            o["count"] = json::Value(static_cast<int>(store.size()));
            o["loadMs"] = json::Value(store.loadMs());
            o["lastSaveMs"] = json::Value(store.saveMs());
            o["lastSortMs"] = json::Value(store.lastSortMs());
            o["lastBroadcastMs"] = json::Value(wsServer.lastBroadcastMs());
            o["clients"] = json::Value(static_cast<int>(wsServer.clientCount()));
            wsServer.sendTo(fd, json::Value(std::move(o)).dump());
            return;
        }

        wsServer.sendTo(fd, errorJson("unknown op: " + op, reqId));
    });

    if (!wsServer.start()) {
        logger::error("Failed to start WebSocket server");
        return 1;
    }

    // When a new client connects we want it to receive the current snapshot
    // immediately so it can render the UI. The simplest way is to broadcast
    // on every connection event - but our WsServer doesn't fire one, so we
    // rely on the client sending an explicit {"op":"list"} on connect.

    std::thread httpThread(httpServerLoop, httpPort, htmlPath);

    logger::info("Server ready. Open http://localhost:" +
                 std::to_string(httpPort) + " in your browser.");
    logger::info("Press Ctrl+C to exit.");

    // Poll in short increments so Ctrl+C/SIGTERM is responsive (a long
    // sleep_for() doesn't wake early just because g_running changed), while
    // still logging a heartbeat roughly every 30s.
    auto lastHeartbeat = std::chrono::steady_clock::now();
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::seconds(30)) {
            logger::info("Heartbeat: " + std::to_string(store.size()) +
                         " student(s) in store, " +
                         std::to_string(wsServer.clientCount()) +
                         " client(s) connected");
            lastHeartbeat = now;
        }
    }

    logger::info("Shutting down...");
    wsServer.stop();
    if (httpThread.joinable()) httpThread.join();
    logger::info("Goodbye.");
    return 0;
}
