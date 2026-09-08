#include "devbox/async.hpp"
#include "devbox/native.hpp"
#include <future>
#include <iostream>
using namespace devbox;
namespace {
asio::awaitable<void> exercise(WorkPool& pool, const std::shared_ptr<std::string>& payload) {
    std::function<std::string()> callback = [text = *payload] { return text; };
    for (int i = 0; i < 20; ++i) {
        auto pending = pool.run([callback] { return callback(); });
        auto result = co_await std::move(pending);
        if (result != *payload)
            throw Error("Worker lost its owned callback payload");
    }
    bool rejected = false;
    try {
        auto pending = pool.run([copy = callback] {
            (void)copy();
            throw Error("worker fixture exception");
        });
        co_await std::move(pending);
    } catch (const Error& e) {
        rejected = std::string(e.what()) == "worker fixture exception";
    }
    if (!rejected)
        throw Error("Worker exception did not return through its coroutine");
    if (callback() != *payload)
        throw Error("Worker cleanup invalidated caller state");
}
} // namespace
int main() {
    try {
        WorkPool pool(2, 8);
        asio::io_context io;
        auto payload = std::make_shared<std::string>(16384, 'x');
        const std::weak_ptr<std::string> observed = payload;
        auto result = asio::co_spawn(io, exercise(pool, payload), asio::use_future);
        io.run();
        result.get();
        payload.reset();
        if (!observed.expired())
            throw Error("Completed worker retained caller state");
        std::cout << "async ownership tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
