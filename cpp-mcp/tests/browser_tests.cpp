#include "devbox/browser.hpp"
#include "devbox/browser_protocol.hpp"
#include "devbox/browser_proxy.hpp"
#include "devbox/process.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <future>
#include <iostream>
#include <thread>
using namespace devbox;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using Tcp = asio::ip::tcp;
void require(bool v, const char* message) {
    if (!v)
        throw Error(message);
}
template <class F> void rejects(F f, std::string_view part) {
    try {
        f();
    } catch (const std::exception& e) {
        if (std::string_view(e.what()).find(part) != std::string_view::npos)
            return;
        throw;
    }
    throw Error("Expected rejection: " + std::string(part));
}
struct ProtocolFixture {
    asio::io_context io;
    Tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::future<void> worker;
    template <class F> explicit ProtocolFixture(F f) {
        worker = std::async(std::launch::async, [this, f] {
            websocket::stream<Tcp::socket> socket(io);
            acceptor.accept(socket.next_layer());
            socket.accept();
            f(socket);
        });
    }
    void finish() {
        worker.get();
    }
    std::uint16_t port() {
        return acceptor.local_endpoint().port();
    }
};
Json receive(auto& socket) {
    beast::flat_buffer b;
    socket.read(b);
    return Json::parse(beast::buffers_to_string(b.data()));
}
void send(auto& socket, const Json& j) {
    auto s = j.dump();
    socket.text(true);
    socket.write(asio::buffer(s));
}
void protocol_tests() {
    require(browser_page_status(
                {{"title", "Attention Required! | Cloudflare"}, {"text", "Sorry, you have been blocked"}},
                403) == "access_denied",
            "block page is not useful source content");
    require(browser_page_status({{"title", "Before you continue"}, {"url", "https://consent.google.com/"}},
                                200) == "consent_required",
            "consent is an explicit intermediate state");
    ProtocolFixture fixture([](auto& socket) {
        auto first = receive(socket);
        send(socket, {{"method", "Page.test"}, {"params", Json::object()}});
        auto nested = receive(socket);
        send(socket, {{"id", first["id"]}, {"result", {{"outer", true}}}});
        send(socket, {{"id", nested["id"]}, {"result", {{"inner", true}}}});
    });
    BrowserProtocol protocol;
    auto deadline = Clock::now() + Millis(3000);
    protocol.connect(fixture.port(), "/devtools/browser/test", deadline);
    bool nested = false;
    protocol.on_event([&](const Json&, Clock::time_point d, const Cancel& c) {
        nested = protocol.call("Test.inner", Json::object(), d, c)["inner"];
    });
    require(protocol.call("Test.outer", Json::object(), deadline)["outer"] && nested,
            "nested and out-of-order protocol replies");
    protocol.close();
    fixture.finish();
    ProtocolFixture invalid([](auto& socket) {
        receive(socket);
        send(socket, {{"id", 99}, {"result", Json::object()}});
    });
    BrowserProtocol bad;
    bad.connect(invalid.port(), "/devtools/browser/test", Clock::now() + Millis(2000));
    rejects([&] { bad.call("Test.invalid", Json::object(), Clock::now() + Millis(2000)); }, "unexpected");
    invalid.finish();
    ProtocolFixture recoverable([](auto& socket) {
        auto a = receive(socket);
        send(socket, {{"id", a["id"]},
                      {"error", {{"code", -32000}, {"message", "Cannot find context with specified id"}}}});
        auto b = receive(socket);
        send(socket, {{"id", b["id"]}, {"result", {{"recovered", true}}}});
    });
    BrowserProtocol recovery;
    recovery.connect(recoverable.port(), "/devtools/browser/test", Clock::now() + Millis(2000));
    rejects([&] { recovery.call("Test.context", Json::object(), Clock::now() + Millis(2000)); },
            "PROTOCOL_REMOTE");
    require(recovery.call("Test.next", Json::object(), Clock::now() + Millis(2000))["recovered"],
            "command error retains usable transport");
    recovery.close();
    recoverable.finish();
    ProtocolFixture cancelled([](auto& socket) {
        receive(socket);
        std::this_thread::sleep_for(Millis(200));
    });
    BrowserProtocol cancellable;
    cancellable.connect(cancelled.port(), "/devtools/browser/test", Clock::now() + Millis(2000));
    auto stop = std::make_shared<Cancellation>();
    std::jthread abort([stop] {
        std::this_thread::sleep_for(Millis(40));
        stop->cancel();
    });
    rejects([&] { cancellable.call("Test.cancel", Json::object(), Clock::now() + Millis(2000), stop); },
            "cancelled");
    cancelled.finish();
    ProtocolFixture slow([](auto& socket) {
        receive(socket);
        std::this_thread::sleep_for(Millis(250));
    });
    BrowserProtocol timed;
    timed.connect(slow.port(), "/devtools/browser/test", Clock::now() + Millis(1000));
    rejects([&] { timed.call("Test.slow", Json::object(), Clock::now() + Millis(80)); }, "DEADLINE");
    slow.finish();
    ProtocolFixture oversized([](auto& socket) {
        receive(socket);
        send(socket, {{"id", 1}, {"result", {{"text", std::string(2048, 'a')}}}});
    });
    BrowserProtocol bounded(512);
    bounded.connect(oversized.port(), "/devtools/browser/test", Clock::now() + Millis(1000));
    rejects([&] { bounded.call("Test.large", Json::object(), Clock::now() + Millis(1000)); }, "PROTOCOL_IO");
    oversized.finish();
    rejects(
        [&] {
            BrowserProtocol p;
            p.connect(1234, "/devtools/page/wrong", Clock::now() + Millis(100));
        },
        "ENDPOINT");
}
struct PageFixture {
    asio::io_context io;
    Tcp::acceptor listener{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic_bool stop{false};
    std::atomic_uint requests{0};
    std::jthread worker;
    PageFixture() {
        listener.non_blocking(true);
        worker = std::jthread([this] {
            while (!stop) {
                Tcp::socket socket(io);
                boost::system::error_code ec;
                listener.accept(socket, ec);
                if (ec) {
                    std::this_thread::sleep_for(Millis(2));
                    continue;
                }
                try {
                    beast::flat_buffer b;
                    http::request<http::string_body> request;
                    http::read(socket, b, request);
                    ++requests;
                    http::response<http::string_body> response{http::status::ok, 11};
                    response.set(http::field::content_type, "text/html; charset=utf-8");
                    response.keep_alive(false);
                    response.body() =
                        R"HTML(<!doctype html><title>Native dynamic phone fixture</title><style>body{font:20px sans-serif}#panel{height:250px;overflow:auto;width:500px}#inside{height:1800px;background:linear-gradient(white,lightblue)}</style><h1>Fixture Phone 256GB Blue</h1><p id="price">Loading price</p><button onclick="this.innerText='Clicked'">Choose variant</button><input aria-label="Search fixture"><div id="panel"><div id="inside">Scrollable panel<div style="padding-top:1400px">Panel bottom</div></div></div><div style="height:2500px">Long page</div><p>Page bottom</p><script>setTimeout(()=>{document.querySelector('#price').innerText='Live fixture price GBP 649.00';let s=document.createElement('script');s.type='application/ld+json';s.textContent=JSON.stringify({'@context':'https://schema.org','@type':'Product',name:'Fixture Phone 256GB Blue',offers:{'@type':'Offer',price:'649.00',priceCurrency:'GBP',availability:'https://schema.org/InStock'}});document.head.appendChild(s);},150)</script>)HTML";
                    response.prepare_payload();
                    http::write(socket, response);
                } catch (...) {
                }
            }
        });
    }
    ~PageFixture() {
        stop = true;
        if (worker.joinable())
            worker.join();
    }
    std::uint16_t port() {
        return listener.local_endpoint().port();
    }
};
void proxy_tests() {
    BrowserProxy gate;
    auto request = [&](std::string target) {
        asio::io_context io;
        Tcp::socket socket(io);
        socket.connect({asio::ip::make_address("127.0.0.1"), gate.port()});
        std::string wire = "GET " + target + " HTTP/1.1\r\nHost: ignored\r\nConnection: close\r\n\r\n";
        asio::write(socket, asio::buffer(wire));
        std::array<char, 128> b;
        boost::system::error_code ec;
        const auto n = socket.read_some(asio::buffer(b), ec);
        require(n == 0 && ec, "private target must be denied before connection");
    };
    request("http://127.0.0.1:80/");
    request("http://169.254.169.254/");
    request("http://localhost/");
    require(json_uint(gate.metrics(), "accepted_connections") == 0, "no private connections admitted");
}
void browser_tests(bool headed = false) {
    PageFixture page;
    auto config = std::make_shared<Config>();
    config->runtime_mode = RuntimeMode::host;
    config->host_exec_enabled = true;
    config->platform = Platform::detect();
    BrowserOptions options;
    options.headed = headed;
    options.fixture_port = page.port();
    options.lifetime = Millis(60000);
    options.idle_timeout = Millis(30000);
    BrowserService browser(config, options);
    require(json_bool(browser.perform({{"action", "status"}}, "test").data, "available"),
            "Chromium required for real browser qualification");
    auto value = browser.perform({{"action", "open"},
                                  {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/"},
                                  {"include_html", true}},
                                 "test");
    require(json_string(value.data, "text").find("GBP 649.00") != std::string::npos,
            "JavaScript-rendered live price text");
    require(!value.image_base64.empty() && base64_decode(value.image_base64).at(0) == 0xff,
            "real JPEG screenshot");
    const auto id = json_string(value.data, "session_id"),
               observation = json_string(value.data, "observation_id");
    rejects([&] { browser.perform({{"action", "observe"}, {"session_id", id}}, "other"); },
            "SESSION_NOT_FOUND");
    rejects([&] { browser.perform({{"action", "open"}, {"url", "http://example.com/"}}, "test"); }, "BUSY");
    const auto hash = json_string(value.data["html_artifact"], "sha256");
    auto html =
        browser.perform({{"action", "read_html"}, {"session_id", id}, {"html_sha256", hash}}, "test").data;
    require(json_string(html, "text").find("649.00") != std::string::npos,
            "rendered HTML artifact contains dynamic price");
    require(json_string(html, "sha256") == sha256(json_string(html, "text")),
            "small HTML artifact hash round trip");
    auto scroll = browser.perform({{"action", "scroll"},
                                   {"session_id", id},
                                   {"observation_id", observation},
                                   {"x", 1000},
                                   {"y", 600},
                                   {"delta_y", 650}},
                                  "test");
    require(json_number(scroll.data["viewport"], "scroll_y") > 400,
            "native wheel scroll actually moved page");
    rejects(
        [&] {
            browser.perform({{"action", "scroll"}, {"session_id", id}, {"observation_id", observation}},
                            "test");
        },
        "STALE");
    auto back = browser.perform({{"action", "scroll"},
                                 {"session_id", id},
                                 {"observation_id", scroll.data["observation_id"]},
                                 {"x", 1000},
                                 {"y", 600},
                                 {"delta_y", -650}},
                                "test");
    const auto button = back.data["controls"][0];
    auto click = browser.perform({{"action", "click"},
                                  {"session_id", id},
                                  {"observation_id", back.data["observation_id"]},
                                  {"x", json_number(button, "x") + json_number(button, "width") / 2},
                                  {"y", json_number(button, "y") + json_number(button, "height") / 2}},
                                 "test");
    require(json_string(click.data, "text").find("Clicked") != std::string::npos, "native click changed DOM");
    const auto panel = click.data["scroll_regions"][0];
    auto nested = browser.perform({{"action", "scroll"},
                                   {"session_id", id},
                                   {"observation_id", click.data["observation_id"]},
                                   {"x", json_number(panel, "x") + 100},
                                   {"y", json_number(panel, "y") + 100},
                                   {"delta_y", 500}},
                                  "test");
    require(json_number(nested.data["scroll_regions"][0], "scroll_top") > 200,
            "nested pane scrolling moved the intended container");
    rejects(
        [&] {
            browser.perform({{"action", "navigate"}, {"session_id", id}, {"url", "http://169.254.169.254/"}},
                            "test");
        },
        "PRIVATE");
    require(json_bool(browser.perform({{"action", "close"}, {"session_id", id}}, "test").data, "closed"),
            "explicit close completed");
    require(!json_bool(browser.perform({{"action", "status"}}, "test").data, "active_session"),
            "owned browser cleared");
    std::cout << "real Chromium rendering, image, HTML, scroll, click, ownership and bounds passed\n";
}
int main(int argc, char** argv) {
    try {
        protocol_tests();
        proxy_tests();
        if (argc > 1 &&
            (std::string_view(argv[1]) == "--chromium" || std::string_view(argv[1]) == "--chromium-headed"))
            browser_tests(std::string_view(argv[1]) == "--chromium-headed");
        std::cout << "browser checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
