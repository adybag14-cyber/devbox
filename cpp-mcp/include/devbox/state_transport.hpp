#pragma once
#include "common.hpp"
namespace devbox {
// Native-only transport for the encrypted, authenticated state protocol. It
// accepts no hostname, path, redirect, proxy or caller-supplied HTTP headers.
class StateHttpTransport {
    struct Pool;
    struct Attempt;
    std::shared_ptr<Pool> pool_;

  public:
    struct Peer {
        std::uint16_t port = 0;
        std::uint64_t generation = 0, instance = 0;
        std::uint32_t pid = 0;
        bool operator==(const Peer&) const = default;
    };
    struct Statistics {
        std::uint64_t transfers = 0, connections = 0;
        std::size_t idle = 0;
    };
    class Transfer {
        friend class StateHttpTransport;
        std::unique_ptr<Attempt> attempt_;
        explicit Transfer(std::unique_ptr<Attempt> attempt);

      public:
        ~Transfer();
        Transfer(Transfer&&) noexcept;
        Transfer& operator=(Transfer&&) noexcept;
        Transfer(const Transfer&) = delete;
        Transfer& operator=(const Transfer&) = delete;
        const HttpResult& response() const;
        // The state protocol must verify nonce, generation, MAC and ciphertext
        // before permitting the connection to return to the bounded idle pool.
        void verified();
    };
    explicit StateHttpTransport(bool reuse = true);
    ~StateHttpTransport();
    StateHttpTransport(const StateHttpTransport&) = delete;
    StateHttpTransport& operator=(const StateHttpTransport&) = delete;
    Transfer post(Peer peer, std::string_view body, Millis timeout, std::size_t max_bytes,
                  const Cancel& cancel = {}) const;
    void discard_idle() const;
    Statistics statistics() const;
};
} // namespace devbox
