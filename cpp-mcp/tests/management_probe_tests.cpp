#include "devbox/management.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <thread>
using namespace devbox;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using Tcp = asio::ip::tcp;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
struct Fixture {
    asio::io_context io;
    Tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::vector<std::pair<int, std::string>> replies;
    std::size_t served = 0;
    std::exception_ptr failure;
    std::jthread worker;
    explicit Fixture(std::vector<std::pair<int, std::string>> values) : replies(std::move(values)) {
        acceptor.non_blocking(true);
        worker = std::jthread([this](std::stop_token stop) {
            try {
                while (!stop.stop_requested() && served < replies.size()) {
                    boost::system::error_code ec;
                    Tcp::socket socket(io);
                    acceptor.accept(socket, ec);
                    if (ec) {
                        std::this_thread::sleep_for(Millis(1));
                        continue;
                    }
                    boost::beast::flat_buffer buffer;
                    http::request<http::string_body> request;
                    http::read(socket, buffer, request);
                    require(request.target() == (served ? "/readyz" : "/"), "Expected native probe route");
                    const auto& [status, body] = replies[served];
                    if (status) {
                        http::response<http::string_body> response{static_cast<http::status>(status), 11};
                        response.keep_alive(false);
                        response.body() = body;
                        response.prepare_payload();
                        http::write(socket, response);
                    }
                    ++served;
                }
            } catch (...) {
                failure = std::current_exception();
            }
        });
    }
    ~Fixture() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
    }
    Environment environment() const {
        return {{"HOST", "127.0.0.1"}, {"PORT", std::to_string(acceptor.local_endpoint().port())}};
    }
    void finish() {
        worker.request_stop();
        worker.join();
        if (failure)
            std::rethrow_exception(failure);
        require(served == replies.size(), "Probe consumed the expected response sequence");
    }
};
int main() {
    try {
        const std::string hash(64, 'a'), generation = "owned-fixture";
        const Json candidate{{"sha256", hash}};
        const auto good =
            Json{{"build", {{"binarySha256", hash}, {"deploymentGeneration", generation}}}}.dump();
        const auto check = [&](std::vector<std::pair<int, std::string>> replies, std::string reason,
                               std::string phase, std::optional<int> status) {
            Fixture fixture(std::move(replies));
            Json failure;
            require(!probe_managed_readiness(fixture.environment(), candidate, generation, &failure),
                    "Failed probe must reject readiness");
            fixture.finish();
            require(failure["reason"] == reason && failure["phase"] == phase &&
                        failure["httpStatus"] == (status ? Json(*status) : Json()),
                    "Probe phase and received HTTP status must survive the failure");
            require(failure.dump().find("PRIVATE_RESPONSE") == std::string::npos,
                    "Diagnostic output must not copy response bodies");
        };
        check({{503, "PRIVATE_RESPONSE"}}, "http_status", "metadata", 503);
        check({{200, "PRIVATE_RESPONSE"}}, "invalid_json", "metadata", 200);
        check({{200, "{}"}}, "identity_mismatch", "metadata", 200);
        check({{0, ""}}, "probe_exception", "metadata", std::nullopt);
        check({{200, good}, {503, "PRIVATE_RESPONSE"}}, "http_status", "readiness", 503);
        check({{200, good}, {200, "{\"ok\":false}"}}, "not_ready", "readiness", 200);
        check({{200, good}, {0, ""}}, "probe_exception", "readiness", std::nullopt);
        Fixture healthy({{200, good}, {200, "{\"ok\":true}"}});
        Json failure;
        require(probe_managed_readiness(healthy.environment(), candidate, generation, &failure) &&
                    failure.is_null(),
                "Healthy identity and readiness remain accepted");
        healthy.finish();
        std::cout << "Native metadata/readiness status, identity and response privacy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
