// common/json.h
// Minimal single-header JSON parser & serializer.
// Supports: null, bool, number (double), string, array, object.
// Designed for clarity, not maximum performance. Sufficient for the
// small messages exchanged in this project.
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace json {

class Value; // forward
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(double n) : type_(Type::Number), num_(n) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(const std::string& s) : type_(Type::String), str_(s) {}
    Value(std::string&& s) : type_(Type::String), str_(std::move(s)) {}
    Value(const Array& a) : type_(Type::Array), arr_(std::make_shared<Array>(a)) {}
    Value(Array&& a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Value(const Object& o) : type_(Type::Object), obj_(std::make_shared<Object>(o)) {}
    Value(Object&& o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool          asBool()   const { return bool_; }
    double        asNumber() const { return num_; }
    int           asInt()    const { return static_cast<int>(num_); }
    const std::string& asString() const { return str_; }
    const Array&  asArray()  const { return *arr_; }
    const Object& asObject() const { return *obj_; }
    Array&  asArray()  { if (!arr_) arr_ = std::make_shared<Array>(); return *arr_; }
    Object& asObject() { if (!obj_) obj_ = std::make_shared<Object>(); return *obj_; }

    // Safe accessors that return defaults if missing / wrong type.
    bool        getBool(const std::string& key, bool def = false) const {
        if (!isObject()) return def;
        auto it = obj_->find(key);
        if (it == obj_->end() || !it->second.isBool()) return def;
        return it->second.asBool();
    }
    double      getNumber(const std::string& key, double def = 0.0) const {
        if (!isObject()) return def;
        auto it = obj_->find(key);
        if (it == obj_->end() || !it->second.isNumber()) return def;
        return it->second.asNumber();
    }
    int         getInt(const std::string& key, int def = 0) const {
        return static_cast<int>(getNumber(key, static_cast<double>(def)));
    }
    std::string getString(const std::string& key, const std::string& def = "") const {
        if (!isObject()) return def;
        auto it = obj_->find(key);
        if (it == obj_->end() || !it->second.isString()) return def;
        return it->second.asString();
    }
    bool has(const std::string& key) const {
        return isObject() && obj_->find(key) != obj_->end();
    }
    const Value& at(const std::string& key) const {
        static const Value null_;
        if (!isObject()) return null_;
        auto it = obj_->find(key);
        return (it == obj_->end()) ? null_ : it->second;
    }

    // Serialize to compact JSON string.
    std::string dump() const {
        std::ostringstream os;
        dumpTo(os);
        return os.str();
    }

    void dumpTo(std::ostream& os) const {
        switch (type_) {
            case Type::Null: os << "null"; break;
            case Type::Bool: os << (bool_ ? "true" : "false"); break;
            case Type::Number: {
                if (num_ == static_cast<int64_t>(num_)) {
                    os << static_cast<int64_t>(num_);
                } else {
                    os << num_;
                }
                break;
            }
            case Type::String: dumpString(os, str_); break;
            case Type::Array: {
                os << '[';
                bool first = true;
                for (const auto& v : *arr_) {
                    if (!first) os << ',';
                    v.dumpTo(os);
                    first = false;
                }
                os << ']';
                break;
            }
            case Type::Object: {
                os << '{';
                bool first = true;
                for (const auto& [k, v] : *obj_) {
                    if (!first) os << ',';
                    dumpString(os, k);
                    os << ':';
                    v.dumpTo(os);
                    first = false;
                }
                os << '}';
                break;
            }
        }
    }

private:
    static void dumpString(std::ostream& os, const std::string& s) {
        os << '"';
        for (char c : s) {
            switch (c) {
                case '"':  os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\b': os << "\\b"; break;
                case '\f': os << "\\f"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\t': os << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        os << buf;
                    } else {
                        os << c;
                    }
            }
        }
        os << '"';
    }

    Type type_;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Parser {
public:
    explicit Parser(const std::string& src) : src_(src), pos_(0) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (pos_ != src_.size())
            throw ParseError("trailing characters in JSON input");
        return v;
    }

private:
    const std::string& src_;
    size_t pos_;

    void skipWs() {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\n' || src_[pos_] == '\r'))
            ++pos_;
    }

    char peek() {
        if (pos_ >= src_.size()) throw ParseError("unexpected end of input");
        return src_[pos_];
    }

    char consume() {
        if (pos_ >= src_.size()) throw ParseError("unexpected end of input");
        return src_[pos_++];
    }

    bool match(const std::string& lit) {
        if (src_.compare(pos_, lit.size(), lit) == 0) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    Value parseValue() {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        return parseNumber();
    }

    Value parseObject() {
        consume(); // {
        Object obj;
        skipWs();
        if (peek() == '}') { consume(); return Value(std::move(obj)); }
        while (true) {
            skipWs();
            if (peek() != '"') throw ParseError("expected string key");
            std::string key = parseString();
            skipWs();
            if (consume() != ':') throw ParseError("expected ':'");
            Value v = parseValue();
            obj.emplace(std::move(key), std::move(v));
            skipWs();
            char c = consume();
            if (c == ',') continue;
            if (c == '}') break;
            throw ParseError("expected ',' or '}'");
        }
        return Value(std::move(obj));
    }

    Value parseArray() {
        consume(); // [
        Array arr;
        skipWs();
        if (peek() == ']') { consume(); return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parseValue());
            skipWs();
            char c = consume();
            if (c == ',') continue;
            if (c == ']') break;
            throw ParseError("expected ',' or ']'");
        }
        return Value(std::move(arr));
    }

    std::string parseString() {
        if (consume() != '"') throw ParseError("expected '\"'");
        std::string out;
        while (true) {
            if (pos_ >= src_.size()) throw ParseError("unterminated string");
            char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) throw ParseError("bad escape");
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) throw ParseError("bad unicode escape");
                        unsigned code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src_[pos_++];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= h - '0';
                            else if (h >= 'a' && h <= 'f') code |= 10 + h - 'a';
                            else if (h >= 'A' && h <= 'F') code |= 10 + h - 'A';
                            else throw ParseError("bad unicode hex");
                        }
                        // Encode as UTF-8 (BMP only)
                        if (code < 0x80) out.push_back(static_cast<char>(code));
                        else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: throw ParseError("bad escape character");
                }
            } else {
                out.push_back(c);
            }
        }
    }

    Value parseBool() {
        if (match("true")) return Value(true);
        if (match("false")) return Value(false);
        throw ParseError("expected boolean");
    }

    Value parseNull() {
        if (match("null")) return Value(nullptr);
        throw ParseError("expected null");
    }

    Value parseNumber() {
        size_t start = pos_;
        if (peek() == '-') consume();
        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                src_[pos_] == '+' || src_[pos_] == '-'))
            ++pos_;
        if (start == pos_) throw ParseError("expected number");
        try {
            return Value(std::stod(src_.substr(start, pos_ - start)));
        } catch (...) {
            throw ParseError("invalid number");
        }
    }
};

inline Value parse(const std::string& src) {
    Parser p(src);
    return p.parse();
}

} // namespace json
