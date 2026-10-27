#pragma once
#include "devbox/x11_session.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

// One private, single-client X11 relay. It delays replies from our owned Xvfb,
// without delaying the server itself or touching any desktop outside this test.
class SlowX11Relay {
    struct Fd {
        int value = -1;
        ~Fd() {
            if (value >= 0)
                ::close(value);
        }
    } listener_;
    std::atomic<bool> stopping_{false}, failed_{false};
    std::atomic<unsigned> delay_ms_{0}, next_reply_delay_ms_{0};
    std::thread worker_;
    unsigned display_ = 0;

    void run(unsigned backend, std::uint32_t server_pid) noexcept {
        try {
            Fd client;
            while (!stopping_.load()) {
                client.value = ::accept4(listener_.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client.value >= 0)
                    break;
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                    throw std::runtime_error("Relay accept");
                pollfd descriptor{listener_.value, POLLIN, 0};
                ::poll(&descriptor, 1, 10);
            }
            if (client.value < 0)
                return;
            Fd server{devbox::connect_private_x11(
                backend, std::chrono::steady_clock::now() + std::chrono::seconds(2), {}, server_pid)};
            if (::fcntl(server.value, F_SETFL, ::fcntl(server.value, F_GETFL) | O_NONBLOCK) != 0)
                throw std::runtime_error("Relay nonblocking server");
            struct Queue {
                std::vector<char> data;
                std::size_t offset = 0;
                std::chrono::steady_clock::time_point ready{};
                bool empty() const {
                    return offset == data.size();
                }
            } to_server, to_client;
            const auto receive = [&](int fd, Queue& queue, unsigned delay) {
                char data[8192];
                const auto count = ::recv(fd, data, sizeof(data), 0);
                if (!count)
                    return false;
                if (count < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                        return true;
                    throw std::runtime_error("Relay receive");
                }
                if (queue.empty()) {
                    queue.data.clear();
                    queue.offset = 0;
                    queue.ready = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
                }
                if (queue.data.size() + static_cast<std::size_t>(count) > 1024 * 1024)
                    throw std::runtime_error("Relay bounded queue");
                queue.data.insert(queue.data.end(), data, data + count);
                return true;
            };
            const auto send = [](int fd, Queue& queue) {
                if (queue.empty() || std::chrono::steady_clock::now() < queue.ready)
                    return;
                const auto count = ::send(fd, queue.data.data() + queue.offset,
                                          queue.data.size() - queue.offset, MSG_NOSIGNAL);
                if (count > 0)
                    queue.offset += static_cast<std::size_t>(count);
                else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                    throw std::runtime_error("Relay send");
            };
            while (!stopping_.load()) {
                pollfd descriptors[]{{client.value, POLLIN, 0}, {server.value, POLLIN, 0}};
                if (::poll(descriptors, 2, 5) < 0 && errno != EINTR)
                    throw std::runtime_error("Relay poll");
                if (descriptors[0].revents & POLLIN) {
                    if (!receive(client.value, to_server, 0))
                        break;
                    if (const auto delay = next_reply_delay_ms_.exchange(0))
                        delay_ms_.store(delay);
                }
                if (descriptors[1].revents & POLLIN)
                    if (!receive(server.value, to_client, delay_ms_.load()))
                        break;
                if ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
                    break;
                send(server.value, to_server);
                send(client.value, to_client);
            }
        } catch (...) {
            if (!stopping_.load())
                failed_.store(true);
        }
    }

  public:
    SlowX11Relay(unsigned backend, std::uint32_t server_pid) {
        listener_.value = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener_.value < 0)
            throw std::runtime_error("Relay socket");
        const auto first = 10000u + static_cast<unsigned>(::getpid() % 10000);
        for (unsigned i = 0; i < 100; ++i) {
            const auto candidate = first + i;
            const auto name = "/tmp/.X11-unix/X" + std::to_string(candidate);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path + 1, name.data(), name.size());
            if (::bind(listener_.value, reinterpret_cast<sockaddr*>(&address),
                       static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size())) == 0) {
                display_ = candidate;
                break;
            }
            if (errno != EADDRINUSE)
                throw std::runtime_error("Relay private bind");
        }
        if (!display_ || ::listen(listener_.value, 1) != 0)
            throw std::runtime_error("Relay listen");
        worker_ = std::thread([this, backend, server_pid] { run(backend, server_pid); });
    }
    ~SlowX11Relay() {
        stopping_.store(true);
        if (worker_.joinable())
            worker_.join();
    }
    unsigned display() const {
        return display_;
    }
    void delay(unsigned ms) {
        delay_ms_.store(ms);
    }
    void delay_next_reply(unsigned ms) {
        next_reply_delay_ms_.store(ms);
    }
    bool failed() const {
        return failed_.load();
    }
};
