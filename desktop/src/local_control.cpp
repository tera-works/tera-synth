#include "local_control.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLen = int;
static void closeSock(intptr_t s) { closesocket((SOCKET)s); }
static const intptr_t kBad = (intptr_t)INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
static void closeSock(intptr_t s) { ::close((int)s); }
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE on each socket instead
#endif
static const intptr_t kBad = -1;
#endif

using namespace hsynth;

namespace {

bool sendAll(intptr_t s, const char* p, size_t n) {
    while (n > 0) {
#ifdef _WIN32
        const int k = ::send((SOCKET)s, p, (int)std::min<size_t>(n, 1 << 20), 0);
#else
        const ssize_t k = ::send((int)s, p, n, MSG_NOSIGNAL);
#endif
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

LocalControl::LocalControl(Engine& engine, RemoteControl::Config config, RemoteControl::Hooks hooks) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    control_ = std::make_unique<RemoteControl>(engine, std::move(config), std::move(hooks));
    control_->setOutput([this](const std::vector<uint8_t>& m) { broadcast(m); });
}

LocalControl::~LocalControl() {
    stop();
    control_.reset();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool LocalControl::start(int port, std::string& error) {
    stop();
    const intptr_t s = (intptr_t)::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) { error = "socket() failed"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only
#ifdef _WIN32
    BOOL excl = TRUE;  // nobody else may bind the same port
    setsockopt((SOCKET)s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
#else
    int one = 1;
    setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (::bind((decltype(::socket(0, 0, 0)))s, (sockaddr*)&a, sizeof a) != 0 ||
        ::listen((decltype(::socket(0, 0, 0)))s, 4) != 0) {
        closeSock(s);
        error = "port " + std::to_string(port) + " is in use (another HSynth running?)";
        return false;
    }
    listen_ = s;
    port_ = port;
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void LocalControl::stop() {
    if (!running_.exchange(false)) return;
    closeSock(listen_);
    listen_ = -1;
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (intptr_t c : socks_) {
#ifdef _WIN32
            shutdown((SOCKET)c, SD_BOTH);
#else
            shutdown((int)c, SHUT_RDWR);
#endif
        }
        threads.swap(threads_);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
}

void LocalControl::acceptLoop() {
    while (running_) {
        sockaddr_in from{};
        SockLen len = sizeof from;
        const intptr_t c = (intptr_t)::accept((decltype(::socket(0, 0, 0)))listen_, (sockaddr*)&from, &len);
        if (c == kBad) {
            if (!running_) break;
            continue;
        }
        int one = 1;
        setsockopt((decltype(::socket(0, 0, 0)))c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt((int)c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);  // a client gone: no SIGPIPE
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        socks_.push_back(c);
        threads_.emplace_back([this, c] { clientLoop(c); });
    }
}

void LocalControl::clientLoop(intptr_t sock) {
    clients_.fetch_add(1);
    std::string line;
    char buf[4096];
    for (;;) {
#ifdef _WIN32
        const int n = ::recv((SOCKET)sock, buf, sizeof buf, 0);
#else
        const ssize_t n = ::recv((int)sock, buf, sizeof buf, 0);
#endif
        if (n <= 0) break;
        for (int i = 0; i < (int)n; ++i) {
            const char ch = buf[i];
            if (ch != '\n') {
                if (line.size() < (1u << 20)) line.push_back(ch);
                continue;
            }
            // one SysEx message as hex bytes
            std::vector<uint8_t> msg;
            int hi = -1;
            for (char x : line) {
                const int v = hexVal(x);
                if (v < 0) continue;
                if (hi < 0) hi = v;
                else { msg.push_back((uint8_t)(hi * 16 + v)); hi = -1; }
            }
            line.clear();
            if (msg.size() >= 2 && msg.front() == 0xF0 && msg.back() == 0xF7) {
                requests_.fetch_add(1);
                control_->onSysEx(msg.data(), msg.size());
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socks_.erase(std::remove(socks_.begin(), socks_.end(), sock), socks_.end());
    }
    closeSock(sock);
    clients_.fetch_sub(1);
}

void LocalControl::broadcast(const std::vector<uint8_t>& msg) {
    std::string text;
    text.reserve(msg.size() * 3 + 1);
    char b[4];
    for (size_t i = 0; i < msg.size(); ++i) {
        std::snprintf(b, sizeof b, i ? " %02X" : "%02X", msg[i]);
        text += b;
    }
    text += '\n';
    std::lock_guard<std::mutex> lock(mutex_);
    for (intptr_t s : socks_) sendAll(s, text.data(), text.size());
}
