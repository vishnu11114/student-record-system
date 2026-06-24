// common/net_compat.h
// Thin compatibility layer over POSIX sockets and Winsock2.
// All other source files include this instead of <sys/socket.h> & friends.
//
// Why: BSD-style sockets and Winsock are 90% the same and 10% maddening.
// This header papers over: header names, closesocket() vs close(), SOCKET vs
// int, ssize_t, errno vs WSAGetLastError, MSG_NOSIGNAL (missing on Win + mac),
// EINTR (not really a thing on Windows), and WSAStartup/WSACleanup lifecycle.
//
// Public surface:
//   - sock_t                : socket handle type (int on POSIX, SOCKET on Win)
//   - NET_INVALID_SOCK      : invalid-socket sentinel
//   - net::Init             : RAII helper that calls WSAStartup on Win, no-op
//                             on POSIX. Construct once in main() before any
//                             socket calls.
//   - net::close(s)         : close a socket portably
//   - net::send(s, b, l, f) : signed-return send wrapper
//   - net::recv(s, b, l, f) : signed-return recv wrapper
//   - net::lastError()      : platform error code (WSAGetLastError or errno)
//   - net::wasInterrupted(e): true if e indicates a signal interruption
//   - MSG_NOSIGNAL          : guaranteed to be defined (0 if platform lacks it)

#ifndef SRMS_NET_COMPAT_H
#define SRMS_NET_COMPAT_H

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <basetsd.h>
    // Windows doesn't ship ssize_t; <basetsd.h> gives us SSIZE_T (signed pointer-size).
    #ifndef _SSIZE_T_DEFINED
        typedef SSIZE_T ssize_t;
        #define _SSIZE_T_DEFINED
    #endif
    using sock_t = SOCKET;
    #define NET_INVALID_SOCK INVALID_SOCKET
    // Windows uses SD_BOTH; POSIX uses SHUT_RDWR. Normalize.
    #ifndef SHUT_RDWR
        #define SHUT_RDWR SD_BOTH
    #endif
    // Windows has no SIGPIPE, so MSG_NOSIGNAL has nothing to suppress.
    #ifndef MSG_NOSIGNAL
        #define MSG_NOSIGNAL 0
    #endif
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
    #include <cerrno>
    using sock_t = int;
    #define NET_INVALID_SOCK (-1)
    // macOS lacks MSG_NOSIGNAL; we suppress SIGPIPE process-wide in main() instead.
    #ifndef MSG_NOSIGNAL
        #define MSG_NOSIGNAL 0
    #endif
#endif

#include <cstddef>

namespace net {

// RAII: call WSAStartup/WSACleanup on Windows, do nothing on POSIX. Construct
// one instance in main() before opening any sockets.
class Init {
public:
    Init() {
#ifdef _WIN32
        WSADATA wsa{};
        ok_ = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    }
    ~Init() {
#ifdef _WIN32
        if (ok_) ::WSACleanup();
#endif
    }
    Init(const Init&) = delete;
    Init& operator=(const Init&) = delete;
    bool ok() const {
#ifdef _WIN32
        return ok_;
#else
        return true;
#endif
    }
#ifdef _WIN32
private:
    bool ok_ = false;
#endif
};

inline int close(sock_t s) {
#ifdef _WIN32
    return ::closesocket(s);
#else
    return ::close(s);
#endif
}

inline ssize_t send(sock_t s, const void* buf, std::size_t len, int flags) {
#ifdef _WIN32
    return ::send(s, static_cast<const char*>(buf), static_cast<int>(len), flags);
#else
    return ::send(s, buf, len, flags);
#endif
}

inline ssize_t recv(sock_t s, void* buf, std::size_t len, int flags) {
#ifdef _WIN32
    return ::recv(s, static_cast<char*>(buf), static_cast<int>(len), flags);
#else
    return ::recv(s, buf, len, flags);
#endif
}

inline int lastError() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool wasInterrupted(int e) {
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

}  // namespace net

#endif  // SRMS_NET_COMPAT_H
