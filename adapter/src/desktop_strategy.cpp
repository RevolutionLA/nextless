#include "desktop_strategy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>

namespace vinput {

namespace {

std::string parseNiriFocusedWindowId(const std::string &reply) {
    constexpr char marker[] = "\"FocusedWindow\":{\"id\":";
    auto pos = reply.find(marker);
    if (pos == std::string::npos) return "";
    pos += sizeof(marker) - 1;
    auto end = reply.find_first_not_of("0123456789", pos);
    if (end == pos) return "";
    return reply.substr(pos, end - pos);
}

int remainingMsec(std::chrono::steady_clock::time_point deadline) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return 0;
    return static_cast<int>(remaining.count());
}

bool waitForFd(int fd, short events,
               std::chrono::steady_clock::time_point deadline) {
    while (true) {
        pollfd pfd{fd, events, 0};
        int result = poll(&pfd, 1, remainingMsec(deadline));
        if (result > 0) return (pfd.revents & events) != 0;
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
}

} // namespace

// --- NiriStrategy ---

NiriStrategy::~NiriStrategy() {
    closeSocket();
}

void NiriStrategy::closeSocket() {
    if (socketFd_ >= 0) close(socketFd_);
    socketFd_ = -1;
    socketPath_.clear();
}

bool NiriStrategy::connectSocket(
    const std::string &path,
    std::chrono::steady_clock::time_point deadline) {
    if (socketFd_ >= 0 && socketPath_ == path) return true;
    closeSocket();

    socketFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socketFd_ < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(socketFd_, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            closeSocket();
            return false;
        }
        if (!waitForFd(socketFd_, POLLOUT, deadline)) {
            closeSocket();
            return false;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(socketFd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
            error != 0) {
            closeSocket();
            return false;
        }
    }
    socketPath_ = path;
    return true;
}

std::string NiriStrategy::request(const std::string &payload) {
    const char *pathValue = getenv("NIRI_SOCKET");
    if (!pathValue || !*pathValue ||
        strlen(pathValue) >= sizeof(sockaddr_un::sun_path)) {
        closeSocket();
        return "";
    }
    std::string path(pathValue);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(25);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!connectSocket(path, deadline)) continue;

        size_t sent = 0;
        while (sent < payload.size()) {
            ssize_t count = send(socketFd_, payload.data() + sent,
                                 payload.size() - sent, MSG_NOSIGNAL);
            if (count > 0) {
                sent += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                waitForFd(socketFd_, POLLOUT, deadline)) {
                continue;
            }
            break;
        }
        if (sent != payload.size()) {
            closeSocket();
            continue;
        }

        std::string reply;
        char buffer[1024];
        while (reply.size() < 16384) {
            ssize_t count = recv(socketFd_, buffer, sizeof(buffer), 0);
            if (count > 0) {
                reply.append(buffer, static_cast<size_t>(count));
                auto newline = reply.find('\n');
                if (newline != std::string::npos) {
                    reply.resize(newline + 1);
                    return reply;
                }
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                waitForFd(socketFd_, POLLIN, deadline)) {
                continue;
            }
            break;
        }
        closeSocket();
    }
    return "";
}

std::string NiriStrategy::getFocusedWindowId() {
    return parseNiriFocusedWindowId(request("\"FocusedWindow\"\n"));
}

void NiriStrategy::focusWindow(const std::string &id) {
    if (id.empty()) return;
    request("{\"Action\":{\"FocusWindow\":{\"id\":" + id + "}}}\n");
}

// --- HyprlandStrategy ---

std::string HyprlandStrategy::getFocusedWindowId() {
    FILE *f = popen("hyprctl activewindow -j 2>/dev/null", "r");
    if (!f) return "";
    char buf[512] = {};
    (void)!fread(buf, 1, sizeof(buf) - 1, f);
    pclose(f);
    const char *p = strstr(buf, "\"address\"");
    if (!p) return "";
    p = strchr(p, ':');
    if (!p) return "";
    p++; // skip ':'
    while (*p == ' ' || *p == '"') p++;
    const char *end = strchr(p, '"');
    if (!end) return "";
    return std::string(p, end - p);
}

void HyprlandStrategy::focusWindow(const std::string &id) {
    if (id.empty()) return;
    std::string cmd = "hyprctl dispatch focuswindow address:" + id;
    pid_t pid;
    const char *argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    posix_spawn(&pid, "/bin/sh", nullptr, nullptr,
                (char *const *)argv, environ);
}

// --- Factory ---

std::unique_ptr<DesktopStrategy> DesktopStrategy::create(const std::string &desktop) {
    if (desktop == "niri")     return std::make_unique<NiriStrategy>();
    if (desktop == "hyprland") return std::make_unique<HyprlandStrategy>();
    return std::make_unique<NoopStrategy>();
}

std::unique_ptr<DesktopStrategy> DesktopStrategy::autoDetect() {
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE"))
        return std::make_unique<HyprlandStrategy>();
    if (getenv("NIRI_SOCKET"))
        return std::make_unique<NiriStrategy>();

    const char *xdg = getenv("XDG_CURRENT_DESKTOP");
    if (!xdg) xdg = getenv("XDG_SESSION_DESKTOP");
    if (xdg) {
        std::string d(xdg);
        if (d == "Hyprland" || d == "hyprland") return std::make_unique<HyprlandStrategy>();
        if (d == "niri" || d == "Niri")          return std::make_unique<NiriStrategy>();
    }

    return std::make_unique<NoopStrategy>();
}

} // namespace vinput
