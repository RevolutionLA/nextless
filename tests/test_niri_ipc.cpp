#include "desktop_strategy.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class FakeNiriServer {
public:
    FakeNiriServer(std::string response, std::chrono::milliseconds delay = {})
        : FakeNiriServer({{"\"FocusedWindow\"\n", std::move(response)}},
                         delay) {}

    FakeNiriServer(
        std::vector<std::pair<std::string, std::string>> exchanges,
        std::chrono::milliseconds delay = {})
        : exchanges_(std::move(exchanges)), delay_(delay) {
        path_ = "/tmp/vinput_niri_" + std::to_string(getpid()) + "_" +
                std::to_string(nextId_++) + ".sock";
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(fd_ >= 0);

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        assert(path_.size() < sizeof(address.sun_path));
        memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        unlink(path_.c_str());
        assert(bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        assert(listen(fd_, 1) == 0);

        worker_ = std::thread([this] {
            int client = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
            assert(client >= 0);
            for (const auto &[expected, response] : exchanges_) {
                std::string request;
                while (request.find('\n') == std::string::npos) {
                    char buffer[64];
                    auto count = recv(client, buffer, sizeof(buffer), 0);
                    assert(count > 0);
                    request.append(buffer, static_cast<size_t>(count));
                }
                assert(request == expected);
                if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
                if (!response.empty()) {
                    auto split = response.size() / 2;
                    assert(send(client, response.data(), split, MSG_NOSIGNAL) ==
                           static_cast<ssize_t>(split));
                    assert(send(client, response.data() + split,
                                response.size() - split, MSG_NOSIGNAL) ==
                           static_cast<ssize_t>(response.size() - split));
                }
            }
            close(client);
        });
    }

    ~FakeNiriServer() {
        worker_.join();
        close(fd_);
        unlink(path_.c_str());
    }

    const std::string &path() const { return path_; }

private:
    inline static unsigned nextId_ = 0;
    int fd_ = -1;
    std::string path_;
    std::vector<std::pair<std::string, std::string>> exchanges_;
    std::chrono::milliseconds delay_;
    std::thread worker_;
};

void setSocketPath(const std::string &path) {
    assert(setenv("NIRI_SOCKET", path.c_str(), 1) == 0);
}

} // namespace

int main() {
    vinput::NiriStrategy strategy;

    {
        FakeNiriServer server({
            {"\"FocusedWindow\"\n",
             R"({"Ok":{"FocusedWindow":{"id":41}}})" "\n"},
            {R"({"Action":{"FocusWindow":{"id":42}}})" "\n",
             R"({"Ok":"Handled"})" "\n"},
            {"\"FocusedWindow\"\n",
             R"({"Ok":{"FocusedWindow":{"id":42}}})" "\n"},
        });
        setSocketPath(server.path());
        assert(strategy.getFocusedWindowId() == "41");
        strategy.focusWindow("42");
        assert(strategy.getFocusedWindowId() == "42");
    }

    {
        FakeNiriServer server(
            R"({"Ok":{"FocusedWindow":{"id":184467,"title":"test"}}})"
            "\n");
        setSocketPath(server.path());
        assert(strategy.getFocusedWindowId() == "184467");
    }

    {
        FakeNiriServer server(R"({"Err":"not available"})" "\n");
        setSocketPath(server.path());
        assert(strategy.getFocusedWindowId().empty());
    }

    {
        FakeNiriServer server("", std::chrono::milliseconds(100));
        setSocketPath(server.path());
        auto started = std::chrono::steady_clock::now();
        assert(strategy.getFocusedWindowId().empty());
        auto elapsed = std::chrono::steady_clock::now() - started;
        assert(elapsed < std::chrono::milliseconds(80));
    }

    return 0;
}
