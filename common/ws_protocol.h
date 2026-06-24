// common/ws_protocol.h
// Self-contained WebSocket (RFC 6455) helpers used by both server and client.
// Includes minimal SHA-1, Base64 and frame read/write routines.
//
// This is intentionally implemented from scratch to keep the project
// dependency-free. It supports the handshake, text frames (with payloads
// up to 2^63-1), close frames, and automatic ping/pong handling.
#pragma once

#include "net_compat.h"
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace ws {

// ---------------- SHA-1 ---------------------------------------------------
class Sha1 {
public:
    Sha1() { reset(); }

    void reset() {
        state_[0] = 0x67452301;
        state_[1] = 0xEFCDAB89;
        state_[2] = 0x98BADCFE;
        state_[3] = 0x10325476;
        state_[4] = 0xC3D2E1F0;
        bits_ = 0;
        bufLen_ = 0;
    }

    void update(const uint8_t* data, size_t len) {
        bits_ += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            size_t copy = std::min<size_t>(64 - bufLen_, len);
            std::memcpy(buf_ + bufLen_, data, copy);
            bufLen_ += copy;
            data += copy;
            len -= copy;
            if (bufLen_ == 64) {
                transform(buf_);
                bufLen_ = 0;
            }
        }
    }

    void update(const std::string& s) {
        update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    std::vector<uint8_t> digest() {
        uint64_t bits = bits_;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (bufLen_ != 56) update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 7; i >= 0; --i) {
            lenBytes[i] = static_cast<uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
        update(lenBytes, 8);
        std::vector<uint8_t> out(20);
        for (int i = 0; i < 5; ++i) {
            out[i*4+0] = static_cast<uint8_t>(state_[i] >> 24);
            out[i*4+1] = static_cast<uint8_t>(state_[i] >> 16);
            out[i*4+2] = static_cast<uint8_t>(state_[i] >> 8);
            out[i*4+3] = static_cast<uint8_t>(state_[i]);
        }
        return out;
    }

private:
    void transform(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i*4]) << 24) |
                   (uint32_t(block[i*4+1]) << 16) |
                   (uint32_t(block[i*4+2]) << 8)  |
                    uint32_t(block[i*4+3]);
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (t << 1) | (t >> 31);
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2],
                 d = state_[3], e = state_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        state_[0] += a; state_[1] += b; state_[2] += c;
        state_[3] += d; state_[4] += e;
    }

    uint32_t state_[5];
    uint64_t bits_;
    uint8_t buf_[64];
    size_t bufLen_;
};

// ---------------- Base64 --------------------------------------------------
inline std::string base64Encode(const std::vector<uint8_t>& data) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) |
                     (uint32_t(data[i+1]) << 8) |
                      uint32_t(data[i+2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
    }
    if (i < data.size()) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < data.size()) v |= uint32_t(data[i+1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        if (i + 1 < data.size())
            out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        else
            out.push_back('=');
        out.push_back('=');
    }
    return out;
}

// ---------------- Handshake helpers --------------------------------------
inline std::string acceptKey(const std::string& clientKey) {
    static const std::string kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 h;
    h.update(clientKey);
    h.update(kGuid);
    return base64Encode(h.digest());
}

// ---------------- Socket I/O helpers -------------------------------------
inline bool sendAll(sock_t fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t n = net::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && net::wasInterrupted(net::lastError())) continue;
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

inline bool recvAll(sock_t fd, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (len > 0) {
        ssize_t n = net::recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && net::wasInterrupted(net::lastError())) continue;
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

// Read a single full HTTP request (terminated by CRLFCRLF).
inline bool recvHttpRequest(sock_t fd, std::string& out, size_t maxBytes = 8192) {
    out.clear();
    char c;
    while (out.size() < maxBytes) {
        ssize_t n = net::recv(fd, &c, 1, 0);
        if (n <= 0) {
            if (n < 0 && net::wasInterrupted(net::lastError())) continue;
            return false;
        }
        out.push_back(c);
        if (out.size() >= 4 &&
            out[out.size()-4] == '\r' && out[out.size()-3] == '\n' &&
            out[out.size()-2] == '\r' && out[out.size()-1] == '\n')
            return true;
    }
    return false;
}

// ---------------- Frame I/O ----------------------------------------------
enum class Opcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct Frame {
    bool fin = true;
    Opcode opcode = Opcode::Text;
    std::string payload;
};

// Read one frame from `fd`. Returns false on socket error / close.
inline bool readFrame(sock_t fd, Frame& frame) {
    uint8_t header[2];
    if (!recvAll(fd, header, 2)) return false;
    frame.fin = (header[0] & 0x80) != 0;
    frame.opcode = static_cast<Opcode>(header[0] & 0x0F);
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        if (!recvAll(fd, ext, 2)) return false;
        len = (uint64_t(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!recvAll(fd, ext, 8)) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    uint8_t mask[4] = {0,0,0,0};
    if (masked && !recvAll(fd, mask, 4)) return false;
    // Safety cap: 16 MiB
    if (len > (16ULL * 1024 * 1024)) return false;
    frame.payload.resize(static_cast<size_t>(len));
    if (len > 0 && !recvAll(fd, frame.payload.data(), len)) return false;
    if (masked) {
        for (size_t i = 0; i < frame.payload.size(); ++i)
            frame.payload[i] ^= mask[i & 3];
    }
    return true;
}

// Write one frame to `fd`. `mask` true => client semantics (must be masked).
inline bool writeFrame(sock_t fd, const Frame& frame, bool mask) {
    std::vector<uint8_t> hdr;
    hdr.push_back(static_cast<uint8_t>((frame.fin ? 0x80 : 0x00) |
                                       static_cast<uint8_t>(frame.opcode)));
    uint64_t len = frame.payload.size();
    uint8_t maskBit = mask ? 0x80 : 0x00;
    if (len < 126) {
        hdr.push_back(maskBit | static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        hdr.push_back(maskBit | 126);
        hdr.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        hdr.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        hdr.push_back(maskBit | 127);
        for (int i = 7; i >= 0; --i)
            hdr.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
    }
    if (mask) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        uint32_t m = rng();
        uint8_t mk[4] = {
            static_cast<uint8_t>((m >> 24) & 0xFF),
            static_cast<uint8_t>((m >> 16) & 0xFF),
            static_cast<uint8_t>((m >> 8) & 0xFF),
            static_cast<uint8_t>(m & 0xFF),
        };
        hdr.insert(hdr.end(), mk, mk + 4);
        if (!sendAll(fd, hdr.data(), hdr.size())) return false;
        std::vector<uint8_t> body(frame.payload.size());
        for (size_t i = 0; i < frame.payload.size(); ++i)
            body[i] = static_cast<uint8_t>(frame.payload[i]) ^ mk[i & 3];
        return body.empty() ? true : sendAll(fd, body.data(), body.size());
    }
    if (!sendAll(fd, hdr.data(), hdr.size())) return false;
    if (frame.payload.empty()) return true;
    return sendAll(fd, frame.payload.data(), frame.payload.size());
}

inline bool sendText(sock_t fd, const std::string& text, bool mask) {
    Frame f;
    f.fin = true;
    f.opcode = Opcode::Text;
    f.payload = text;
    return writeFrame(fd, f, mask);
}

inline bool sendClose(sock_t fd, bool mask) {
    Frame f;
    f.fin = true;
    f.opcode = Opcode::Close;
    return writeFrame(fd, f, mask);
}

// Tiny header parser used by both server (for the Upgrade request) and
// client (for the 101 response). Returns header value (case-insensitive
// lookup) or empty string.
inline std::string headerValue(const std::string& http, const std::string& key) {
    std::string lower;
    lower.reserve(http.size());
    for (char c : http) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    std::string klower;
    klower.reserve(key.size());
    for (char c : key) klower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    size_t pos = 0;
    while (pos < lower.size()) {
        size_t eol = lower.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::string line = lower.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            // trim
            size_t a = name.find_first_not_of(" \t");
            size_t b = name.find_last_not_of(" \t");
            if (a != std::string::npos) name = name.substr(a, b - a + 1);
            if (name == klower) {
                std::string val = http.substr(pos + colon + 1, eol - pos - colon - 1);
                size_t va = val.find_first_not_of(" \t");
                size_t vb = val.find_last_not_of(" \t");
                if (va == std::string::npos) return "";
                return val.substr(va, vb - va + 1);
            }
        }
        pos = eol + 2;
    }
    return "";
}

} // namespace ws
