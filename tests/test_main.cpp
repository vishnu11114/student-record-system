// tests/test_main.cpp
// Lightweight unit tests for the data layer. Run via `make test`.
// No framework: just assertions and a tiny harness so the project stays
// dependency-free.

#include "../common/json.h"
#include "../common/ws_protocol.h"
#include "../server/csv_handler.h"
#include "../server/student_store.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond) do {                                                     \
    if (cond) { ++g_passed; }                                                \
    else { ++g_failed; std::cerr << "  FAIL: " #cond                         \
                  << " (" << __FILE__ << ":" << __LINE__ << ")\n"; }         \
} while (0)

#define TEST(name) static void name(); static struct Reg_##name {            \
    Reg_##name() { tests().push_back({#name, name}); }                       \
} reg_##name; static void name()

struct TestEntry { const char* name; void (*fn)(); };

std::vector<TestEntry>& tests() {
    static std::vector<TestEntry> v;
    return v;
}

// Cross-platform temp file path. Hardcoding "/tmp/..." only works on
// POSIX; on Windows that resolves to a drive-relative "\tmp\..." path
// that usually doesn't exist, so writes silently fail. This resolves
// the OS temp directory on every platform (TEMP/TMP on Windows, /tmp
// on Linux/macOS).
std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// ---------- JSON ---------------------------------------------------------
TEST(json_roundtrip_basic) {
    json::Object o;
    o["id"] = json::Value(7);
    o["name"] = json::Value(std::string("Hello, \"world\"\n"));
    o["nested"] = json::Value(json::Array{json::Value(1), json::Value(true), json::Value(nullptr)});
    std::string s = json::Value(std::move(o)).dump();
    json::Value v = json::parse(s);
    CHECK(v.getInt("id") == 7);
    CHECK(v.getString("name") == "Hello, \"world\"\n");
    CHECK(v.at("nested").asArray().size() == 3);
    CHECK(v.at("nested").asArray()[0].asInt() == 1);
    CHECK(v.at("nested").asArray()[1].asBool() == true);
    CHECK(v.at("nested").asArray()[2].isNull());
}

TEST(json_unicode_escape) {
    json::Value v = json::parse("\"\\u00e9\"");
    CHECK(v.isString());
    // U+00E9 = é = 0xC3 0xA9 in UTF-8
    CHECK(v.asString().size() == 2);
    CHECK(static_cast<unsigned char>(v.asString()[0]) == 0xC3);
    CHECK(static_cast<unsigned char>(v.asString()[1]) == 0xA9);
}

TEST(json_malformed_throws) {
    bool threw = false;
    try { json::parse("{ bad }"); } catch (const json::ParseError&) { threw = true; }
    CHECK(threw);
}

// ---------- CSV ----------------------------------------------------------
TEST(csv_round_trip) {
    const std::string path = tempPath("__test_students.csv");
    {
        std::ofstream f(path);
        f << "id,name,age,grade\n";
        f << "1,Alice,20,A\n";
        f << "2,\"Bob, Jr\",21,B+\n";
        f << "3,\"Has \"\"quote\"\"\",22,C\n";
    }
    csv::CsvStudentRepository repo(path);
    auto students = repo.load();
    CHECK(students.size() == 3);
    CHECK(students[0].id == 1 && students[0].name == "Alice");
    CHECK(students[1].name == "Bob, Jr");
    CHECK(students[2].name == "Has \"quote\"");

    // mutate + save
    students[0].grade = "A+";
    students.push_back({4, "Dave", 23, "B"});
    repo.save(students);

    auto reloaded = csv::CsvStudentRepository(path).load();
    CHECK(reloaded.size() == 4);
    CHECK(reloaded[0].grade == "A+");
    CHECK(reloaded[3].name == "Dave");
    std::remove(path.c_str());
}

TEST(csv_skips_malformed_rows) {
    const std::string path = tempPath("__test_bad.csv");
    {
        std::ofstream f(path);
        f << "id,name,age,grade\n";
        f << "1,Alice,20,A\n";
        f << "notanumber,Bob,21,B\n";
        f << "3,Carol,xx,C\n";
        f << "4,Dave,22,D\n";
    }
    csv::CsvStudentRepository repo(path);
    auto students = repo.load();
    CHECK(students.size() == 2);   // rows 2 and 4 valid
    CHECK(students[0].id == 1);
    CHECK(students[1].id == 4);
    std::remove(path.c_str());
}

// ---------- Store --------------------------------------------------------
TEST(store_crud) {
    const std::string path = tempPath("__test_store.csv");
    std::remove(path.c_str());
    auto repo = std::make_unique<csv::CsvStudentRepository>(path);
    StudentStore store(std::move(repo));
    store.load();

    std::string err;
    CHECK(store.create({1, "Alice", 20, "A"}, err));
    CHECK(store.create({2, "Bob",   21, "B"}, err));
    CHECK(!store.create({1, "Dup",  99, "X"}, err)); // duplicate
    CHECK(store.size() == 2);

    auto found = store.find(1);
    CHECK(found.has_value() && found->name == "Alice");

    CHECK(store.update({1, "Alice S.", 21, "A+"}, err));
    CHECK(store.find(1)->name == "Alice S.");

    auto byName = store.searchByName("alic");
    CHECK(byName.size() == 1);

    auto sorted = store.sorted("name", true);
    CHECK(sorted.size() == 2);
    CHECK(sorted[0].name < sorted[1].name);

    CHECK(store.remove(2, err));
    CHECK(store.size() == 1);
    CHECK(!store.remove(999, err));

    // Reload from disk to ensure persistence worked
    auto repo2 = std::make_unique<csv::CsvStudentRepository>(path);
    StudentStore store2(std::move(repo2));
    store2.load();
    CHECK(store2.size() == 1);
    CHECK(store2.find(1)->name == "Alice S.");
    std::remove(path.c_str());
}

TEST(store_replace_all) {
    const std::string path = tempPath("__test_bulk.csv");
    std::remove(path.c_str());
    auto repo = std::make_unique<csv::CsvStudentRepository>(path);
    StudentStore store(std::move(repo));
    store.load();
    store.replaceAll({{10, "X", 1, "A"}, {11, "Y", 2, "B"}, {12, "Z", 3, "C"}});
    CHECK(store.size() == 3);
    CHECK(store.find(11)->name == "Y");
    std::remove(path.c_str());
}

// ---------- WebSocket helpers -------------------------------------------
TEST(ws_handshake_key) {
    // RFC 6455 §1.3 reference example:
    // key "dGhlIHNhbXBsZSBub25jZQ==" -> accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    std::string accept = ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(ws_base64_basic) {
    CHECK(ws::base64Encode({'M','a','n'}) == "TWFu");
    CHECK(ws::base64Encode({'M','a'})     == "TWE=");
    CHECK(ws::base64Encode({'M'})         == "TQ==");
    CHECK(ws::base64Encode({})            == "");
}

TEST(ws_header_parsing) {
    std::string req =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    CHECK(ws::headerValue(req, "Sec-WebSocket-Key") == "dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(ws::headerValue(req, "upgrade") == "websocket");
    CHECK(ws::headerValue(req, "Missing").empty());
}

} // namespace

int main() {
    std::cout << "Running " << tests().size() << " test(s)...\n";
    for (auto& t : tests()) {
        std::cout << " - " << t.name << "...";
        int before = g_failed;
        t.fn();
        std::cout << (g_failed > before ? "  ✗\n" : "  ✓\n");
    }
    std::cout << "\nPassed: " << g_passed << "   Failed: " << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
