// client/main.cpp
// Student Record Management - C++ WebSocket Client
//
// Connects to the server, transmits the full student dataset on startup
// (per the spec), then provides an interactive CLI for CRUD / search / sort.
//
// Default behaviour: looks for ./data/students.csv then ./students.csv and
// uploads whichever it finds. Use --no-upload to skip, or --upload PATH for
// an explicit file.
//
// Usage:
//   ./student_client                                # auto-upload then interact
//   ./student_client --upload data/students.csv     # explicit dataset
//   ./student_client --no-upload                    # skip startup transmit
//   ./student_client --host 127.0.0.1 --port 9001

#include "../common/json.h"
#include "../common/net_compat.h"
#include "../common/ws_protocol.h"
#include "../server/csv_handler.h"   // reuse CSV reader
#include "../server/student.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

sock_t connectTcp(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        std::cerr << "DNS resolution failed for " << host << "\n";
        return NET_INVALID_SOCK;
    }
    sock_t fd = NET_INVALID_SOCK;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == NET_INVALID_SOCK) continue;
        if (::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
        net::close(fd);
        fd = NET_INVALID_SOCK;
    }
    ::freeaddrinfo(res);
    return fd;
}

std::string randomKey() {
    std::vector<uint8_t> bytes(16);
    std::random_device rd;
    for (auto& b : bytes) b = static_cast<uint8_t>(rd());
    return ws::base64Encode(bytes);
}

bool performClientHandshake(sock_t fd, const std::string& host, uint16_t port) {
    std::string key = randomKey();
    std::string req =
        "GET / HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: http://" + host + "\r\n\r\n";
    if (!ws::sendAll(fd, req.data(), req.size())) return false;
    std::string resp;
    if (!ws::recvHttpRequest(fd, resp)) return false;
    if (resp.find("101") == std::string::npos) {
        std::cerr << "Handshake failed:\n" << resp << "\n";
        return false;
    }
    std::string expected = ws::acceptKey(key);
    std::string got = ws::headerValue(resp, "Sec-WebSocket-Accept");
    if (got != expected) {
        std::cerr << "Sec-WebSocket-Accept mismatch\n";
        return false;
    }
    return true;
}

void printHelp() {
    std::cout << "\nCommands:\n"
              << "  list                          - request snapshot from server\n"
              << "  create <id> <name> <age> <grade>\n"
              << "  update <id> <name> <age> <grade>\n"
              << "  delete <id>\n"
              << "  search id <id> | search name <substring>\n"
              << "  sort id|name|age|grade [asc|desc]\n"
              << "  upload <path-to-csv>          - send full dataset to server\n"
              << "  stats                         - request server stats\n"
              << "  help                          - this message\n"
              << "  quit                          - exit\n\n";
}

void prettyPrintEvent(const json::Value& v) {
    std::string event = v.getString("event");
    if (event == "snapshot" || event == "sorted" || event == "search_result") {
        const auto& arr = v.at("students").isArray() ?
                          v.at("students").asArray() : json::Array{};
        std::cout << "--- " << event << " (" << arr.size() << " records) ---\n";
        std::cout << " id   name                          age  grade\n";
        std::cout << " ---  ----                          ---  -----\n";
        for (const auto& s : arr) {
            std::printf(" %-4d %-30s %-4d %s\n",
                        s.getInt("id"),
                        s.getString("name").c_str(),
                        s.getInt("age"),
                        s.getString("grade").c_str());
        }
        std::cout << std::endl;
    } else if (event == "created" || event == "updated") {
        const auto& s = v.at("student");
        std::cout << "[" << event << "] id=" << s.getInt("id")
                  << " name=" << s.getString("name")
                  << " age=" << s.getInt("age")
                  << " grade=" << s.getString("grade") << "\n";
    } else if (event == "deleted") {
        std::cout << "[deleted] id=" << v.getInt("id") << "\n";
    } else if (event == "error") {
        std::cout << "[error] " << v.getString("message") << "\n";
    } else if (event == "ack") {
        std::cout << "[ack] " << v.getString("op") << "\n";
    } else if (event == "stats") {
        std::cout << "[stats] count=" << v.getInt("count")
                  << " loadMs=" << v.getNumber("loadMs")
                  << " saveMs=" << v.getNumber("lastSaveMs")
                  << " sortMs=" << v.getNumber("lastSortMs")
                  << " broadcastMs=" << v.getNumber("lastBroadcastMs")
                  << " clients=" << v.getInt("clients") << "\n";
    } else {
        std::cout << "[event:" << event << "] " << v.dump() << "\n";
    }
    std::cout.flush();
}

// Split a CLI input line on whitespace, but keep quoted strings together
// so names with spaces work: e.g. `create 1 "Alice Smith" 21 A`.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQ = false;
    for (char c : line) {
        if (inQ) {
            if (c == '"') { inQ = false; out.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        } else {
            if (c == '"') inQ = true;
            else if (c == ' ' || c == '\t') {
                if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
            } else cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 9001;
    std::string uploadCsv;
    bool noUpload = false;
    bool explicitUpload = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--upload" && i + 1 < argc) { uploadCsv = argv[++i]; explicitUpload = true; }
        else if (a == "--no-upload") noUpload = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--host H] [--port N] [--upload FILE.csv] [--no-upload]\n";
            return 0;
        }
    }

    // Per the spec: client transmits the full dataset to the server on startup.
    // If --upload wasn't given, auto-discover a CSV in the conventional locations.
    if (!noUpload && !explicitUpload) {
        for (const char* candidate : {"data/students.csv", "students.csv"}) {
            std::ifstream f(candidate);
            if (f.good()) { uploadCsv = candidate; break; }
        }
        if (uploadCsv.empty()) {
            std::cerr << "[client] No CSV found at data/students.csv or ./students.csv — "
                         "starting without upload. Use --upload PATH or --no-upload to silence.\n";
        }
    }

    std::signal(SIGINT, SIG_DFL);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);  // not a thing on Windows
#endif

    net::Init netInit;
    if (!netInit.ok()) {
        std::cerr << "Network init failed\n";
        return 1;
    }

    sock_t fd = connectTcp(host, port);
    if (fd == NET_INVALID_SOCK) {
        std::cerr << "Failed to connect to " << host << ":" << port << "\n";
        return 1;
    }
    if (!performClientHandshake(fd, host, port)) {
        net::close(fd);
        return 1;
    }
    std::cout << "Connected to ws://" << host << ":" << port << "\n";

    std::atomic<bool> running{true};

    // Receiver thread
    std::thread rx([&]() {
        while (running) {
            ws::Frame f;
            if (!ws::readFrame(fd, f)) break;
            if (f.opcode == ws::Opcode::Close) {
                std::cout << "\nServer closed connection.\n";
                running = false;
                break;
            }
            if (f.opcode == ws::Opcode::Ping) {
                ws::Frame p;
                p.opcode = ws::Opcode::Pong;
                p.payload = f.payload;
                ws::writeFrame(fd, p, true);
                continue;
            }
            if (f.opcode != ws::Opcode::Text) continue;
            try {
                json::Value v = json::parse(f.payload);
                std::cout << "\r";  // overwrite prompt
                prettyPrintEvent(v);
                std::cout << "> " << std::flush;
            } catch (const std::exception& e) {
                std::cerr << "bad message: " << e.what() << "\n";
            }
        }
        running = false;
    });

    auto sendOp = [&](const json::Value& v) -> bool {
        return ws::sendText(fd, v.dump(), /*mask=*/true);
    };

    // Optional upload on startup
    if (!uploadCsv.empty()) {
        using clk = std::chrono::steady_clock;
        auto tRead0 = clk::now();
        csv::CsvStudentRepository repo(uploadCsv);
        auto students = repo.load();
        auto tRead1 = clk::now();

        json::Object o;
        o["op"] = json::Value(std::string("bulk"));
        json::Array arr;
        for (auto& s : students) arr.push_back(s.toJson());
        o["students"] = json::Value(std::move(arr));
        auto payload = json::Value(std::move(o)).dump();
        auto tSerialize = clk::now();

        bool ok = ws::sendText(fd, payload, /*mask=*/true);
        auto tSend = clk::now();

        double readMs      = std::chrono::duration<double, std::milli>(tRead1 - tRead0).count();
        double serializeMs = std::chrono::duration<double, std::milli>(tSerialize - tRead1).count();
        double sendMs      = std::chrono::duration<double, std::milli>(tSend - tSerialize).count();
        double totalMs     = std::chrono::duration<double, std::milli>(tSend - tRead0).count();

        std::cout << "[startup] transmitted " << students.size() << " records to server"
                  << (ok ? "" : " (send FAILED)") << "\n"
                  << "          read CSV       : " << readMs      << " ms\n"
                  << "          serialize JSON : " << serializeMs << " ms\n"
                  << "          send over WS   : " << sendMs      << " ms\n"
                  << "          total transmit : " << totalMs     << " ms ("
                  << payload.size() << " bytes)\n";
    }

    // Ask for the current snapshot
    {
        json::Object o;
        o["op"] = json::Value(std::string("list"));
        sendOp(json::Value(std::move(o)));
    }

    printHelp();
    std::string line;
    std::cout << "> " << std::flush;
    while (running && std::getline(std::cin, line)) {
        auto tokens = tokenize(line);
        if (tokens.empty()) { std::cout << "> " << std::flush; continue; }
        const std::string& cmd = tokens[0];
        try {
            if (cmd == "quit" || cmd == "exit") {
                // Give the receiver thread a brief window to print any
                // still-in-flight responses before we tear down the socket.
                // Without this, commands piped in quickly via stdin (e.g.
                // automated scripts) can hit "quit" before the server's
                // reply to the previous command has been read, so the
                // operation succeeds server-side but its confirmation is
                // never shown here.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                running = false;
                break;
            }
            if (cmd == "help") { printHelp(); }
            else if (cmd == "list") {
                json::Object o;
                o["op"] = json::Value(std::string("list"));
                sendOp(json::Value(std::move(o)));
            }
            else if (cmd == "create" && tokens.size() == 5) {
                json::Object stu;
                stu["id"]    = json::Value(std::stoi(tokens[1]));
                stu["name"]  = json::Value(tokens[2]);
                stu["age"]   = json::Value(std::stoi(tokens[3]));
                stu["grade"] = json::Value(tokens[4]);
                json::Object o;
                o["op"] = json::Value(std::string("create"));
                o["student"] = json::Value(std::move(stu));
                sendOp(json::Value(std::move(o)));
            }
            else if (cmd == "update" && tokens.size() == 5) {
                json::Object stu;
                stu["id"]    = json::Value(std::stoi(tokens[1]));
                stu["name"]  = json::Value(tokens[2]);
                stu["age"]   = json::Value(std::stoi(tokens[3]));
                stu["grade"] = json::Value(tokens[4]);
                json::Object o;
                o["op"] = json::Value(std::string("update"));
                o["student"] = json::Value(std::move(stu));
                sendOp(json::Value(std::move(o)));
            }
            else if (cmd == "delete" && tokens.size() == 2) {
                json::Object o;
                o["op"] = json::Value(std::string("delete"));
                o["id"] = json::Value(std::stoi(tokens[1]));
                sendOp(json::Value(std::move(o)));
            }
            else if (cmd == "search" && tokens.size() == 3) {
                json::Object o;
                o["op"] = json::Value(std::string("search"));
                o["by"] = json::Value(tokens[1]);
                o["value"] = json::Value(tokens[2]);
                sendOp(json::Value(std::move(o)));
            }
            else if (cmd == "sort" && tokens.size() >= 2) {
                json::Object o;
                o["op"] = json::Value(std::string("sort"));
                o["by"] = json::Value(tokens[1]);
                o["order"] = json::Value(tokens.size() > 2 ? tokens[2] : std::string("asc"));
                sendOp(json::Value(std::move(o)));
            }
            else if (cmd == "upload" && tokens.size() == 2) {
                csv::CsvStudentRepository repo(tokens[1]);
                auto students = repo.load();
                json::Object o;
                o["op"] = json::Value(std::string("bulk"));
                json::Array arr;
                for (auto& s : students) arr.push_back(s.toJson());
                o["students"] = json::Value(std::move(arr));
                auto t0 = std::chrono::steady_clock::now();
                sendOp(json::Value(std::move(o)));
                auto t1 = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                std::cout << "Sent " << students.size() << " records in "
                          << ms << " ms\n";
            }
            else if (cmd == "stats") {
                json::Object o;
                o["op"] = json::Value(std::string("stats"));
                sendOp(json::Value(std::move(o)));
            }
            else {
                std::cout << "Unknown or malformed command. Type 'help'.\n";
            }
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
        std::cout << "> " << std::flush;
    }

    ws::sendClose(fd, true);
    running = false;
    ::shutdown(fd, SHUT_RDWR);
    if (rx.joinable()) rx.join();
    net::close(fd);
    return 0;
}
