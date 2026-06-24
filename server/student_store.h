// server/student_store.h
// Thread-safe in-memory store for student records.
// Persists changes through an IStudentRepository.
#pragma once

#include "csv_handler.h"
#include "logger.h"
#include "student.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class StudentStore {
public:
    explicit StudentStore(std::unique_ptr<csv::IStudentRepository> repo)
        : repo_(std::move(repo)) {}

    // Returns load time in milliseconds (for performance metrics).
    double load() {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Student> loaded = repo_->load();
        std::unique_lock lock(mutex_);
        students_.clear();
        index_.clear();
        students_.reserve(loaded.size());
        for (auto& s : loaded) {
            if (index_.count(s.id)) {
                logger::warn("Duplicate id ignored: " + std::to_string(s.id));
                continue;
            }
            index_[s.id] = students_.size();
            students_.push_back(std::move(s));
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        logger::info("Loaded " + std::to_string(students_.size()) +
                     " students in " + format(ms) + " ms");
        loadMs_ = ms;
        return ms;
    }

    double saveMs() const { return saveMs_; }
    double loadMs() const { return loadMs_; }
    double lastSortMs() const { return lastSortMs_; }

    std::vector<Student> all() const {
        std::shared_lock lock(mutex_);
        return students_;
    }

    size_t size() const {
        std::shared_lock lock(mutex_);
        return students_.size();
    }

    std::optional<Student> find(int id) const {
        std::shared_lock lock(mutex_);
        auto it = index_.find(id);
        if (it == index_.end()) return std::nullopt;
        return students_[it->second];
    }

    // Case-insensitive substring search by name.
    std::vector<Student> searchByName(const std::string& q) const {
        std::shared_lock lock(mutex_);
        std::string ql = lower(q);
        std::vector<Student> out;
        for (const auto& s : students_) {
            if (lower(s.name).find(ql) != std::string::npos) out.push_back(s);
        }
        return out;
    }

    // CRUD ------------------------------------------------------------------

    // Returns true on success. Fails if id already exists.
    bool create(const Student& s, std::string& err) {
        std::unique_lock lock(mutex_);
        if (index_.count(s.id)) {
            err = "id already exists: " + std::to_string(s.id);
            return false;
        }
        index_[s.id] = students_.size();
        students_.push_back(s);
        persistLocked();
        return true;
    }

    // Returns true if updated, false if not found.
    bool update(const Student& s, std::string& err) {
        std::unique_lock lock(mutex_);
        auto it = index_.find(s.id);
        if (it == index_.end()) {
            err = "id not found: " + std::to_string(s.id);
            return false;
        }
        students_[it->second] = s;
        persistLocked();
        return true;
    }

    bool remove(int id, std::string& err) {
        std::unique_lock lock(mutex_);
        auto it = index_.find(id);
        if (it == index_.end()) {
            err = "id not found: " + std::to_string(id);
            return false;
        }
        size_t idx = it->second;
        students_.erase(students_.begin() + idx);
        index_.erase(it);
        // Rebuild index for shifted entries.
        for (auto& kv : index_) {
            if (kv.second > idx) --kv.second;
        }
        persistLocked();
        return true;
    }

    // Bulk replace (used when the C++ client transmits its full dataset).
    void replaceAll(const std::vector<Student>& replacement) {
        std::unique_lock lock(mutex_);
        students_.clear();
        index_.clear();
        students_.reserve(replacement.size());
        for (const auto& s : replacement) {
            if (index_.count(s.id)) continue;
            index_[s.id] = students_.size();
            students_.push_back(s);
        }
        persistLocked();
    }

    // Sort by a field. Returns a sorted copy (does not mutate store).
    std::vector<Student> sorted(const std::string& by, bool asc) {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<Student> copy;
        {
            std::shared_lock lock(mutex_);
            copy = students_;
        }
        auto cmp = [&](const Student& a, const Student& b) {
            if (by == "id")    return asc ? a.id    < b.id    : a.id    > b.id;
            if (by == "age")   return asc ? a.age   < b.age   : a.age   > b.age;
            if (by == "grade") return asc ? a.grade < b.grade : a.grade > b.grade;
            // default: name
            return asc ? a.name < b.name : a.name > b.name;
        };
        std::sort(copy.begin(), copy.end(), cmp);
        auto t1 = std::chrono::steady_clock::now();
        lastSortMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return copy;
    }

private:
    void persistLocked() {
        auto t0 = std::chrono::steady_clock::now();
        try {
            repo_->save(students_);
        } catch (const std::exception& e) {
            logger::error(std::string("persist failed: ") + e.what());
        }
        auto t1 = std::chrono::steady_clock::now();
        saveMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }

    static std::string lower(const std::string& s) {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    static std::string format(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return buf;
    }

    mutable std::shared_mutex mutex_;
    std::vector<Student> students_;
    std::unordered_map<int, size_t> index_;
    std::unique_ptr<csv::IStudentRepository> repo_;
    double loadMs_     = 0.0;
    double saveMs_     = 0.0;
    double lastSortMs_ = 0.0;
};
