// server/csv_handler.h
// RFC-4180-ish CSV reader / writer for Student rows.
// Handles quoted fields, embedded commas, and CRLF.
#pragma once

#include "student.h"
#include "logger.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace csv {

// Interface so the data layer can be swapped (e.g. for an SQLite-backed
// store) without changing higher-level code.
class IStudentRepository {
public:
    virtual ~IStudentRepository() = default;
    virtual std::vector<Student> load() = 0;
    virtual void save(const std::vector<Student>& students) = 0;
};

class CsvStudentRepository : public IStudentRepository {
public:
    explicit CsvStudentRepository(std::string path) : path_(std::move(path)) {}

    std::vector<Student> load() override {
        std::ifstream in(path_);
        if (!in.is_open()) {
            logger::warn("CSV file not found, starting empty: " + path_);
            return {};
        }
        std::vector<Student> out;
        std::string line;
        bool first = true;
        size_t lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::vector<std::string> fields = splitRow(line);
            if (first) {
                first = false;
                if (looksLikeHeader(fields)) continue;
            }
            if (fields.size() < 4) {
                logger::warn("Skipping malformed CSV row " +
                             std::to_string(lineNo));
                continue;
            }
            try {
                Student s;
                s.id    = std::stoi(fields[0]);
                s.name  = fields[1];
                s.age   = std::stoi(fields[2]);
                s.grade = fields[3];
                out.push_back(std::move(s));
            } catch (const std::exception& e) {
                logger::warn(std::string("Skipping row ") +
                             std::to_string(lineNo) + ": " + e.what());
            }
        }
        return out;
    }

    void save(const std::vector<Student>& students) override {
        // Atomic-ish write: write to .tmp then rename.
        std::string tmp = path_ + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                throw std::runtime_error("cannot open " + tmp + " for writing");
            out << "id,name,age,grade\n";
            for (const auto& s : students) {
                out << s.id << ','
                    << quote(s.name) << ','
                    << s.age << ','
                    << quote(s.grade) << '\n';
            }
            out.flush();
            if (!out.good())
                throw std::runtime_error("write failed for " + tmp);
        }
        if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
            // Fallback for cross-filesystem moves: copy contents.
            std::ifstream src(tmp, std::ios::binary);
            std::ofstream dst(path_, std::ios::binary | std::ios::trunc);
            dst << src.rdbuf();
            std::remove(tmp.c_str());
        }
    }

    const std::string& path() const { return path_; }

private:
    static bool looksLikeHeader(const std::vector<std::string>& fields) {
        if (fields.empty()) return false;
        // If first field can't be parsed as int, treat as header.
        try {
            (void)std::stoi(fields[0]);
            return false;
        } catch (...) {
            return true;
        }
    }

    static std::vector<std::string> splitRow(const std::string& line) {
        std::vector<std::string> out;
        std::string cur;
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (inQuotes) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i+1] == '"') {
                        cur.push_back('"');
                        ++i;
                    } else {
                        inQuotes = false;
                    }
                } else {
                    cur.push_back(c);
                }
            } else {
                if (c == ',') {
                    out.push_back(std::move(cur));
                    cur.clear();
                } else if (c == '"' && cur.empty()) {
                    inQuotes = true;
                } else {
                    cur.push_back(c);
                }
            }
        }
        out.push_back(std::move(cur));
        return out;
    }

    static std::string quote(const std::string& s) {
        bool needs = s.find_first_of(",\"\r\n") != std::string::npos;
        if (!needs) return s;
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s) {
            if (c == '"') out.push_back('"');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    std::string path_;
};

} // namespace csv
