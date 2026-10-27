#include "devbox/browser.hpp"
#include "devbox/browser_protocol.hpp"
#include "devbox/browser_proxy.hpp"
#include "devbox/contract.hpp"
#include "devbox/native.hpp"
#include "devbox/process.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace devbox;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using Tcp = asio::ip::tcp;
void browser_diagnostics(std::string_view text) {
    for (const auto& line : split(std::string(text), '\n'))
        if (!line.empty() && !line.starts_with("DevTools listening on "))
            std::cerr << "[Chromium fixture] " << line << '\n';
}
void require(bool v, const char* message) {
    if (!v)
        throw Error(message);
}
void check_startup_timing(const Json& data) {
    const auto& startup = data.at("timing").at("startup");
    std::uint64_t sum = 0;
    for (const auto* key :
         {"resolve_and_proxy_ms", "endpoint_ready_ms", "cdp_connect_ms", "session_setup_ms"}) {
        require(startup.at(key).is_number_integer() && startup.at(key).get<std::int64_t>() >= 0,
                "startup phase durations are nonnegative integers");
        sum += json_uint(startup, key);
    }
    const auto total = json_uint(startup, "total_ms");
    require(startup.size() == 5 && total >= sum && total - sum <= 3,
            "startup phases partition total duration within millisecond rounding");
    require(json_uint(data.at("timing"), "action_ms") >= total,
            "startup duration fits inside its open action");
}
void check_navigation_timing(const Json& data) {
    const auto& navigation = data.at("timing").at("navigation");
    require(navigation.size() == 2, "navigation timing contains only bounded phase durations");
    for (const auto* key : {"submission_ms", "document_wait_ms"})
        require(navigation.at(key).is_number_integer() && navigation.at(key).get<std::int64_t>() >= 0,
                "navigation phase durations are nonnegative integers");
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
    template <class F>
    explicit ProtocolFixture(F f, Millis handshake_delay = Millis(0), bool allow_peer_close = false) {
        worker = std::async(std::launch::async, [this, f, handshake_delay, allow_peer_close] {
            websocket::stream<Tcp::socket> socket(io);
            acceptor.accept(socket.next_layer());
            std::this_thread::sleep_for(handshake_delay);
            boost::system::error_code error;
            socket.accept(error);
            if (error) {
                if (allow_peer_close)
                    return;
                throw boost::system::system_error(error);
            }
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
BrowserObservation initial_observation(BrowserService& browser, const Json& args, std::string_view owner) {
    auto value = browser.perform(args, owner);
    if (json_string(args, "action") == "open")
        check_startup_timing(value.data);
    for (unsigned attempt = 0; json_string(value.data, "next_action") == "observe" && attempt < 4;
         ++attempt) {
        const auto id = json_string(value.data, "session_id");
        value = browser.perform({{"action", "observe"}, {"session_id", id}}, owner);
        require(json_string(value.data, "session_id") == id, "initial observation retains session identity");
    }
    require(json_string(value.data, "next_action") != "observe", "bounded fixture continuation completes");
    check_navigation_timing(value.data);
    return value;
}
void protocol_tests() {
    {
        // A newly started Chromium process can publish its endpoint before its
        // debugger finishes a slow cold start. Exercise an actual delayed
        // WebSocket handshake, then a CDP exchange using the production budget.
        ProtocolFixture cold(
            [](auto& socket) {
                const auto request = receive(socket);
                send(socket, {{"id", request["id"]}, {"result", {{"product", "cold-fixture"}}}});
            },
            Millis(3500), true);
        BrowserProtocol fresh;
        const auto hard = Clock::now() + Millis(12000);
        const auto until = browser_connection_deadline(false, hard);
        fresh.connect(cold.port(), "/devtools/browser/cold-fixture", until);
        require(fresh.call("Browser.getVersion", Json::object(), until)["product"] == "cold-fixture",
                "fresh owned browser survives a slow debugger handshake");
        fresh.close();
        cold.finish();
    }
    {
        ProtocolFixture stale([](auto&) {}, Millis(3500), true);
        BrowserProtocol shared;
        const auto now = Clock::now();
        const auto until = browser_connection_deadline(true, now + Millis(12000), now);
        require(until == now + Millis(3000), "shared endpoint retains its short stale-peer bound");
        rejects([&] { shared.connect(stale.port(), "/devtools/browser/stale-fixture", until); }, "DEADLINE");
        require(!shared.is_connected(), "timed-out shared connection is not reusable");
        stale.finish();
    }
    {
        ProtocolFixture slow([](auto&) {}, Millis(500), true);
        BrowserProtocol bounded;
        const auto now = Clock::now();
        const auto hard = now + Millis(100);
        require(browser_connection_deadline(false, hard, now) == hard,
                "fresh connection never extends the operation deadline");
        rejects(
            [&] {
                bounded.connect(slow.port(), "/devtools/browser/bounded-fixture",
                                browser_connection_deadline(false, hard, now));
            },
            "DEADLINE");
        slow.finish();
    }
    {
        ProtocolFixture slow([](auto&) {}, Millis(500), true);
        BrowserProtocol cancelled;
        auto cancel = std::make_shared<Cancellation>();
        auto abort = std::async(std::launch::async, [cancel] {
            std::this_thread::sleep_for(Millis(40));
            cancel->cancel();
        });
        rejects(
            [&] {
                cancelled.connect(slow.port(), "/devtools/browser/cancel-fixture",
                                  browser_connection_deadline(false, Clock::now() + Millis(12000)), cancel);
            },
            "cancelled");
        require(!cancelled.is_connected(), "cancelled cold connection is not reusable");
        abort.get();
        slow.finish();
    }
    const Json controls = Json::array({{{"tag", "input"},
                                        {"type", "radio"},
                                        {"name", "White"},
                                        {"checked", false},
                                        {"x", 100},
                                        {"y", 100},
                                        {"width", 30},
                                        {"height", 30}},
                                       {{"tag", "a"},
                                        {"name", "Learn more"},
                                        {"href", "https://example.com/old"},
                                        {"x", 10},
                                        {"y", 10},
                                        {"width", 60},
                                        {"height", 20}}});
    auto help_updated = controls;
    help_updated[1]["href"] = "https://example.com/new";
    require(browser_click_controls_match(controls, help_updated, 115, 115),
            "unrelated help-link URL does not invalidate a stable radio click");
    require(!browser_click_controls_match(controls, help_updated, 25, 15),
            "clicked link URL changes still require a fresh observation");
    require(!browser_click_controls_match(controls, help_updated, 500, 500),
            "blank clicks cannot ignore control changes");
    for (const auto& field : {"checked", "name", "x", "disabled"}) {
        auto target_changed = help_updated;
        if (std::string_view(field) == "checked" || std::string_view(field) == "disabled")
            target_changed[0][field] = true;
        else if (std::string_view(field) == "x")
            target_changed[0][field] = 101;
        else
            target_changed[0][field] = "Different variant";
        require(!browser_click_controls_match(controls, target_changed, 115, 115),
                "changed target identity, geometry or state remains stale");
    }
    auto help_renamed = help_updated;
    help_renamed[1]["name"] = "Different help";
    require(!browser_click_controls_match(controls, help_renamed, 115, 115),
            "unrelated visible label changes retain the strict guard");
    auto added_control = help_updated;
    added_control.push_back(controls[0]);
    require(!browser_click_controls_match(controls, added_control, 115, 115),
            "added controls retain the strict guard");
    auto linked_input = controls;
    linked_input[0]["activation_href"] = "https://example.com/wrapped-old";
    auto changed_ancestor = linked_input;
    changed_ancestor[0]["activation_href"] = "https://example.com/wrapped-new";
    require(!browser_click_controls_match(linked_input, changed_ancestor, 115, 115),
            "an input's activating ancestor link remains part of its target identity");
    const auto endpoint_root = fs::temp_directory_path() / ("devbox-endpoint-fixture-" + uuid());
    fs::create_directory(endpoint_root);
    ScopeExit remove_endpoint([&] {
        std::error_code ignored;
        fs::remove_all(endpoint_root, ignored);
    });
    const std::string endpoint_text = "1234\n/devtools/browser/fixture\n";
    const auto endpoint_file = endpoint_root / "DevToolsActivePort";
    write_file(endpoint_file, "");
    auto incomplete_publication = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(Millis(100));
        write_file(endpoint_file, "1234\n");
        std::this_thread::sleep_for(Millis(100));
        write_file(endpoint_file, endpoint_text);
    });
    require(read_browser_endpoint(endpoint_root, Clock::now() + Millis(2000)) == endpoint_text,
            "endpoint read waits for empty and port-only files to finish publication");
    incomplete_publication.get();
    for (const auto* incomplete : {"", "1234", "1234\n", "1234\n/devtools/browser/"}) {
        write_file(endpoint_file, incomplete);
        rejects([&] { (void)read_browser_endpoint(endpoint_root, Clock::now() + Millis(80)); },
                "ENDPOINT_DEADLINE");
    }
    for (const auto* invalid : {"bad-port", "0\n/devtools/browser/test", "65536\n/devtools/browser/test",
                                "1234\n/devtools/page/test", "1234\n/devtools/browser/test?token=bad"}) {
        write_file(endpoint_file, invalid);
        rejects([&] { (void)read_browser_endpoint(endpoint_root, Clock::now() + Millis(2000)); },
                "ENDPOINT_INVALID");
    }
    write_file(endpoint_file, "");
    auto cancelled_publication = std::make_shared<Cancellation>();
    auto abort_publication = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(Millis(40));
        cancelled_publication->cancel();
    });
    rejects(
        [&] {
            (void)read_browser_endpoint(endpoint_root, Clock::now() + Millis(2000), cancelled_publication);
        },
        "cancelled");
    abort_publication.get();
#ifdef _WIN32
    const auto writer = CreateFileW(endpoint_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    require(writer != INVALID_HANDLE_VALUE, "exclusive Chromium endpoint writer fixture");
    DWORD written = 0;
    require(WriteFile(writer, endpoint_text.data(), static_cast<DWORD>(endpoint_text.size()), &written,
                      nullptr) &&
                written == endpoint_text.size(),
            "endpoint fixture written");
    auto publication = std::async(std::launch::async, [writer] {
        std::this_thread::sleep_for(Millis(100));
        CloseHandle(writer);
    });
    require(read_browser_endpoint(endpoint_root, Clock::now() + Millis(2000)) == endpoint_text,
            "endpoint read waits for the actual Windows writer to release its handle");
    publication.get();
    NativeHandle locked(CreateFileW(endpoint_file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
    require(static_cast<bool>(locked), "permanent endpoint lock fixture");
    const auto locked_until = Clock::now() + Millis(80);
    rejects([&] { (void)read_browser_endpoint(endpoint_root, locked_until); }, "ENDPOINT_DEADLINE");
    auto cancelled_endpoint = std::make_shared<Cancellation>();
    cancelled_endpoint->cancel();
    rejects(
        [&] { (void)read_browser_endpoint(endpoint_root, Clock::now() + Millis(2000), cancelled_endpoint); },
        "cancelled");
    locked.reset();
#endif
    require(browser_cgroup_headroom(536870912, 383205376, 182263808) == 335929344,
            "observed Oracle inactive file cache is reclaimable headroom");
    require(browser_cgroup_headroom(512, 400, 0) == 112,
            "missing cache data retains conservative accounting");
    require(browser_cgroup_headroom(512, 400, 500) == 112, "inconsistent cache counters receive no credit");
    require(browser_cgroup_headroom(512, 700, 50) == 0, "over-limit working set has no headroom");
    require(browser_cgroup_headroom(512, 600, 200) == 112,
            "reclaimable cache can restore over-limit headroom");
    const std::string executable = "/usr/lib/chromium/chromium", profile = "/tmp/devbox-desktop/browser";
    const std::vector<std::string> args{executable,
                                        "--ozone-platform=x11",
                                        "--user-data-dir=" + profile,
                                        "--remote-debugging-address=127.0.0.1",
                                        "--remote-debugging-port=0",
                                        "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1",
                                        "about:blank"};
    std::string nul, flat;
    for (const auto& arg : args) {
        nul += arg + '\0';
        if (!flat.empty())
            flat += ' ';
        flat += arg;
    }
    require(browser_process_arguments_match(nul, executable, profile), "structured Chromium argv accepted");
    require(browser_process_arguments_match(flat, executable, profile),
            "Alpine flattened process title accepted");
    require(!browser_process_arguments_match(flat, executable, profile + "-different"),
            "profile prefix is insufficient");
    require(!browser_process_arguments_match(flat + " --no-sandbox=1", executable, profile),
            "valued sandbox-disabling flag rejected");
    require(!browser_process_arguments_match(flat + " --user-data-dir=/another-profile", executable, profile),
            "conflicting profile flag rejected");
    require(!browser_process_arguments_match(flat, "/another/executable", profile),
            "flattened executable prefix must match");
    require(!browser_process_arguments_match(flat, executable, profile, true),
            "requested compatibility cannot silently use an incompatible managed process");
    require(browser_process_arguments_match(flat + " --disable-blink-features=AutomationControlled",
                                            executable, profile, true),
            "managed compatibility requires its exact launch switch");
    require(!browser_process_arguments_match(flat + " --disable-blink-features=AutomationControlled"
                                                    " --disable-blink-features=OtherFeature",
                                             executable, profile, true),
            "conflicting compatibility switches are rejected");
    require(
        !browser_process_arguments_match(flat + " --remote-debugging-address=0.0.0.0", executable, profile),
        "conflicting public debug listener rejected");
    require(browser_normalize_url("HTTPS://Example.COM:443/shop?q=phone#/product/blue") ==
                "https://example.com/shop?q=phone#/product/blue",
            "browser preserves fragment routes");
    rejects([] { browser_normalize_url("http://127.0.0.1/#public"); }, "PRIVATE");
    require(browser_page_status(
                {{"title", "Attention Required! | Cloudflare"}, {"text", "Sorry, you have been blocked"}},
                403) == "access_denied",
            "block page is not useful source content");
    require(browser_page_status({{"title", "Before you continue"}, {"url", "https://consent.google.com/"}},
                                200) == "consent_required",
            "consent is an explicit intermediate state");
    require(browser_page_status({{"title", "Just a moment..."}, {"text", "Checking your browser"}}, 403) ==
                "challenge_required",
            "a challenge using HTTP 403 remains distinguishable from a hard access denial");
    require(browser_page_status({{"title", "Something went wrong - Argos.co.uk"},
                                 {"text", "We can't seem to find that page right now..."}},
                                200) == "not_found",
            "a retailer soft-404 is not usable product evidence");
    require(browser_page_status({{"title", "Shop"}}, 410) == "not_found",
            "a removed resource is not useful source content");
    require(browser_page_status({{"title", "Understanding website errors"},
                                 {"text", std::string(90, 'a') + " we can't seem to find that page"},
                                 {"ready_state", "complete"},
                                 {"navigation_stable", true}},
                                200) == "content_available",
            "mentioning an error inside an ordinary article does not make it a soft-404");
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
    // The peer refuses to answer until all requests have arrived: sequential
    // call-and-wait implementations cannot pass this fixture.
    ProtocolFixture pipelined([](auto& socket) {
        std::vector<Json> requests;
        for (unsigned i = 0; i < 3; ++i)
            requests.push_back(receive(socket));
        send(socket, {{"method", "Test.event"}, {"params", Json::object()}});
        send(socket, {{"id", requests[2]["id"]}, {"result", {{"index", 2}}}});
        send(socket, {{"id", requests[0]["id"]}, {"result", {{"index", 0}}}});
        send(socket, {{"id", requests[1]["id"]},
                      {"error", {{"code", -32000}, {"message", "Invalid InterceptionId"}}}});
        auto next = receive(socket);
        send(socket, {{"id", next["id"]}, {"result", {{"ready", true}}}});
    });
    BrowserProtocol pipeline;
    pipeline.connect(pipelined.port(), "/devtools/browser/test", Clock::now() + Millis(3000));
    unsigned notifications = 0;
    pipeline.on_event([&](const Json&, Clock::time_point, const Cancel&) { ++notifications; });
    const auto replies = pipeline.batch(
        {{"Test.one", Json::object()}, {"Test.two", Json::object()}, {"Test.three", Json::object()}},
        Clock::now() + Millis(3000));
    require(replies.size() == 3 && replies[0]["result"]["index"] == 0 &&
                replies[1]["error"]["message"] == "Invalid InterceptionId" &&
                replies[2]["result"]["index"] == 2 && notifications == 1,
            "batch preserves order, events and per-command errors");
    require(pipeline.call("Test.next", Json::object(), Clock::now() + Millis(3000))["ready"],
            "batch drains all replies after a remote error");
    require(json_uint(pipeline.metrics(), "commands") == 4 && json_uint(pipeline.metrics(), "batches") == 1,
            "protocol metrics count commands separately from batches");
    rejects([&] { pipeline.batch({}, Clock::now() + Millis(1000)); }, "PROTOCOL_CALL");
    rejects(
        [&] {
            pipeline.batch(std::vector<std::pair<std::string, Json>>(33, {"Test.one", Json::object()}),
                           Clock::now() + Millis(1000));
        },
        "PROTOCOL_CALL");
    pipeline.close();
    pipelined.finish();
    ProtocolFixture resource_wait([](auto& socket) {
        const auto outer = receive(socket);
        send(socket, {{"method", "Fetch.requestPaused"}, {"params", {{"requestId", "fixture"}}}});
        const auto first = receive(socket), second = receive(socket);
        // A renderer cannot finish the outer command until its intercepted
        // resources are acknowledged. Deliver the outer result before both
        // nested acknowledgements to exercise reply ownership as well.
        send(socket, {{"id", outer["id"]}, {"result", {{"rendered", true}}}});
        send(socket, {{"id", second["id"]}, {"result", Json::object()}});
        send(socket, {{"id", first["id"]}, {"result", Json::object()}});
    });
    BrowserProtocol renderer;
    renderer.connect(resource_wait.port(), "/devtools/browser/test", Clock::now() + Millis(3000));
    bool resources_drained = false;
    renderer.on_event([&](const Json&, Clock::time_point until, const Cancel& cancel) {
        const auto replies = renderer.batch(
            {{"Fetch.failRequest", Json::object()}, {"Fetch.failRequest", Json::object()}}, until, cancel);
        resources_drained = replies.size() == 2;
    });
    require(
        renderer.call("Page.captureScreenshot", Json::object(), Clock::now() + Millis(3000))["rendered"] &&
            resources_drained,
        "resource acknowledgements can unblock a pending render without losing its early reply");
    renderer.close();
    resource_wait.finish();
    ProtocolFixture cancelled([](auto& socket) {
        receive(socket);
        std::this_thread::sleep_for(Millis(200));
    });
    BrowserProtocol cancellable;
    cancellable.connect(cancelled.port(), "/devtools/browser/test", Clock::now() + Millis(2000));
    auto stop = std::make_shared<Cancellation>();
    auto abort = std::async(std::launch::async, [stop] {
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
    require(!timed.is_connected(), "a transport deadline cannot be reused as a healthy continuation");
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
    std::atomic_bool reject_requests{false};
    std::atomic_uint accepted_connections{0};
    std::atomic_uint requests{0};
    std::atomic_uint picture_requests{0};
    std::atomic_uint continuation_requests{0}, capture_requests{0};
    std::atomic_uint href_revision{0}, href_applied{0};
    std::thread worker;
    explicit PageFixture(bool storage = false) {
        listener.non_blocking(true);
        worker = std::thread([this, storage] {
            while (!stop) {
                Tcp::socket socket(io);
                boost::system::error_code ec;
                listener.accept(socket, ec);
                if (ec) {
                    std::this_thread::sleep_for(Millis(2));
                    continue;
                }
                ++accepted_connections;
                try {
                    beast::flat_buffer b;
                    http::request_parser<http::string_body> parser;
                    parser.header_limit(8192);
                    parser.body_limit(4096);
                    socket.non_blocking(true);
                    const auto read_until = Clock::now() + Millis(2000);
                    while (!stop && !parser.is_done() && Clock::now() < read_until) {
                        boost::system::error_code error;
                        http::read(socket, b, parser, error);
                        if (error == asio::error::would_block || error == asio::error::try_again)
                            std::this_thread::sleep_for(Millis(2));
                        else if (error)
                            break;
                    }
                    if (stop || !parser.is_done())
                        continue;
                    auto request = parser.release();
                    socket.non_blocking(false);
                    ++requests;
                    if (reject_requests)
                        continue;
                    http::response<http::string_body> response{http::status::ok, 11};
                    response.set(http::field::content_type, "text/html; charset=utf-8");
                    response.keep_alive(false);
                    response.body() =
                        R"HTML(<!doctype html><title>Native dynamic phone fixture</title><style>body{font:20px sans-serif}#panel{height:250px;overflow:auto;width:500px}#inside{height:1800px;background:linear-gradient(white,lightblue)}</style><h1>Fixture Phone 256GB Blue</h1><p id="price">Loading price</p><button onclick="this.innerText='Clicked'">Choose variant</button><input aria-label="Search fixture"><div id="panel"><div id="inside">Scrollable panel<div style="padding-top:1400px">Panel bottom</div></div></div><div style="height:2500px">Long page</div><p>Page bottom</p><script>setTimeout(()=>{document.querySelector('#price').innerText='Live fixture price GBP 649.00';let s=document.createElement('script');s.type='application/ld+json';s.textContent=JSON.stringify({'@context':'https://schema.org','@type':'Product',name:'Fixture Phone 256GB Blue',offers:{'@type':'Offer',price:'649.00',priceCurrency:'GBP',availability:'https://schema.org/InStock'}});document.head.appendChild(s);},150)</script>)HTML";
                    if (storage)
                        response.body() += R"HTML(<script>
const state=document.createElement('p');state.id='storage-state';document.body.prepend(state);
function showStorage(){state.innerText='Cookie: '+(document.cookie||'empty')+'; Storage: '+(localStorage.getItem('devbox-fixture')||'empty');}
const save=document.createElement('button');save.innerText='Store fixture state';save.onclick=()=>{document.cookie='devboxFixture=stored; SameSite=Lax; path=/';localStorage.setItem('devbox-fixture','stored');showStorage();};document.querySelector('button').after(save);showStorage();
</script>)HTML";
                    response.body() +=
                        R"HTML(<script>const flag=document.createElement('p');flag.textContent='Fixture webdriver='+navigator.webdriver;document.body.appendChild(flag);</script>)HTML";
                    response.body() += R"HTML(<p id="image-state">Fixture image pending</p><script>
for(let n=0;n<33;n++){const image=new Image(8,8);if(n===0){image.onload=()=>document.querySelector('#image-state').innerText='Fixture image loaded';image.onerror=()=>document.querySelector('#image-state').innerText='Fixture image omitted';}image.src='/fixture-picture?i='+n;document.body.appendChild(image);}
</script>)HTML";
                    if (request.target().starts_with("/fixture-picture")) {
                        ++picture_requests;
                        response.set(http::field::content_type, "image/svg+xml");
                        response.body() =
                            R"(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="8" height="8" fill="blue"/></svg>)";
                    }
                    if (request.target() == "/client-redirect")
                        response.body() =
                            R"HTML(<!doctype html><title>Redirecting fixture</title><script>setTimeout(()=>location.replace('/redirect-target'),25)</script><script src="/hold-script"></script>)HTML";
                    if (request.target() == "/hold-script") {
                        std::this_thread::sleep_for(Millis(800));
                        response.set(http::field::content_type, "application/javascript");
                        response.body() = "/* delayed parser-blocking resource */";
                    }
                    if (request.target() == "/hash-route")
                        response.body() =
                            R"HTML(<!doctype html><title>Hash route fixture</title><p id="route">Initial route</p><script>setTimeout(()=>{location.hash='phone';document.querySelector('#route').innerText='Loading hash route';setTimeout(()=>document.querySelector('#route').innerText='Hash route price GBP 749.00',450)},300)</script>)HTML";
                    if (request.target() == "/slow-readable")
                        response.body() =
                            R"HTML(<!doctype html><title>Slow readable fixture</title><p id="slow">Loading fixture offer</p><script src="/slow-script"></script>)HTML";
                    if (request.target() == "/slow-script") {
                        std::this_thread::sleep_for(Millis(16000));
                        response.set(http::field::content_type, "application/javascript");
                        response.body() =
                            "document.querySelector('#slow').innerText='Slow fixture price GBP 849.00';";
                    }
                    if (request.target() == "/continuation") {
                        const auto visit = ++continuation_requests;
                        response.body() = "<!doctype html><title>Continuation fixture</title><p>Visit " +
                                          std::to_string(visit) +
                                          "</p><p id='price'>Loading the product price</p>"
                                          "<script src='/continuation-script'></script>";
                    }
                    if (request.target() == "/continuation-script") {
                        std::this_thread::sleep_for(Millis(3000));
                        response.set(http::field::content_type, "application/javascript");
                        response.body() = "document.querySelector('#price').innerText='Continuation product "
                                          "price GBP 749.00';";
                    }
                    if (request.target() == "/capture-continuation") {
                        const auto visit = ++capture_requests;
                        response.body() = "<!doctype html><title>Capture continuation</title><h1>Fixture "
                                          "phone GBP 699.00</h1><p>Capture visit " +
                                          std::to_string(visit) +
                                          "</p><p>The requested product observation must retain its original "
                                          "image and HTML choices.</p><img src='/fixture-picture?capture=1'>";
                    }
                    if (request.target() == "/control-change")
                        response.body() = R"HTML(<!doctype html><title>Stable click fixture</title>
<style>button,a{display:block;width:200px;height:40px;margin:15px}</style>
<p>Verify a stable product control while an unrelated help link changes its destination.</p>
<button onclick="this.textContent='Selected White'">Choose White</button>
<a id="help" href="/help-old">Learn more</a>
<a id="wrapper" href="/wrapped-old">Wrapped control<input type="radio" aria-label="Wrapped radio"
style="position:absolute;left:600px;top:200px;width:30px;height:30px;margin:0"></a>
<script>
let applied=0;
async function update(){
 try{
  const revision=Number(await(await fetch('/href-revision')).text());
  if(revision!==applied){
   document.querySelector('#help').href='/help-'+revision;
   if(revision===3)document.querySelector('#wrapper').href='/wrapped-new';
   applied=revision;
   await fetch('/href-applied/'+revision);
  }
 }finally{setTimeout(update,50);}
}
update();
</script>)HTML";
                    if (request.target() == "/href-revision") {
                        response.set(http::field::content_type, "text/plain");
                        response.body() = std::to_string(href_revision.load());
                    }
                    if (request.target().starts_with("/href-applied/")) {
                        href_applied =
                            static_cast<unsigned>(std::stoul(std::string(request.target().substr(14))));
                        response.set(http::field::content_type, "text/plain");
                        response.body() = "ack";
                    }
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
        if (std::uncaught_exceptions())
            std::cerr << "Browser HTTP fixture completed " << requests.load() << " requests and "
                      << picture_requests.load() << " image requests before failure\n";
    }
    std::uint16_t port() {
        return listener.local_endpoint().port();
    }
};
void fixture_transport_tests() {
    PageFixture page;
    asio::io_context io;
    Tcp::socket idle(io);
    idle.connect({asio::ip::make_address("127.0.0.1"), page.port()});
    const auto accepted_by = Clock::now() + Millis(2000);
    while (page.accepted_connections == 0) {
        require(Clock::now() < accepted_by, "fixture accepts the deliberately idle preconnection");
        std::this_thread::sleep_for(Millis(2));
    }
    const auto started = Clock::now();
    try {
        const auto response = http_request("GET", "http://127.0.0.1:" + std::to_string(page.port()) + "/", {},
                                           Json::object(), Millis(1000), 65536, {}, true);
        require(response.status == 200 && response.body.find("Fixture Phone") != std::string::npos,
                "a ready request progresses while another accepted connection remains idle");
    } catch (...) {
        std::cerr << "Fixture idle-peer regression: accepted=" << page.accepted_connections.load()
                  << " completed=" << page.requests.load()
                  << " elapsed_ms=" << std::chrono::duration_cast<Millis>(Clock::now() - started).count()
                  << '\n';
        throw;
    }
    std::cout << "idle preconnection does not block a ready fixture request\n";
}
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
void browser_tests(bool headed = false, bool compatibility = false) {
    const auto saved_software_rendering = environment("DEVBOX_BROWSER_SOFTWARE_RENDERING");
    ScopeExit restore_software_rendering(
        [&] { set_environment("DEVBOX_BROWSER_SOFTWARE_RENDERING", saved_software_rendering); });
    set_environment("DEVBOX_BROWSER_SOFTWARE_RENDERING", std::nullopt);
#ifdef _WIN32
    const auto shadow = fs::temp_directory_path() / ("devbox-browser-path-" + uuid());
    require(fs::create_directory(shadow), "owned discovery fixture directory");
    const auto old_path = environment("PATH");
    ScopeExit restore_path([&] {
        set_environment("PATH", old_path);
        std::error_code ignored;
        fs::remove_all(shadow, ignored);
    });
    {
        std::ofstream marker(shadow / "google-chrome.exe");
        marker << "This PATH sentinel must not replace an installed native browser.";
        marker.close();
        require(marker.good(), "discovery PATH sentinel written");
    }
    set_environment("PATH", path_text(shadow) + ";" + old_path.value_or(""));
#endif
    PageFixture page;
    // Chromium may open a speculative connection before sending any request.
    // Keep one idle connection open while proving the actual page still loads.
    asio::io_context idle_io;
    Tcp::socket idle_connection(idle_io);
    idle_connection.connect({asio::ip::make_address("127.0.0.1"), page.port()});
    auto config = std::make_shared<Config>();
    config->runtime_mode = RuntimeMode::host;
    config->host_exec_enabled = true;
    config->platform = Platform::detect();
    std::atomic_uint32_t launched_pid{0};
    std::atomic_bool cleanup_attempted{false};
    BrowserOptions options;
    options.headed = headed;
    options.compatibility = compatibility;
    // The headed Linux qualification display is Xvfb. Exercise software
    // rendering with the actual price/HTML/screenshot/input fixture below.
    options.software_rendering = headed;
    options.defer_initial_navigation = !headed && !compatibility;
    options.launch_diagnostics = [&](std::string_view message) {
        browser_diagnostics(message);
        if (message.starts_with("cleanup profile="))
            cleanup_attempted = true;
    };
    options.pid_observer = [&](std::uint32_t pid) { launched_pid.store(pid); };
    options.fixture_port = page.port();
    options.lifetime = Millis(60000);
    options.idle_timeout = Millis(30000);
    BrowserService browser(config, options);
    ToolContract contract(*config);
    require(json_bool(browser.perform({{"action", "status"}}, "test").data, "available"),
            "Chromium required for real browser qualification");
    std::cerr << "[browser] opening owned rendered fixture\n" << std::flush;
    auto value =
        browser.perform(contract.arguments("devbox_browser",
                                           {{"action", "open"},
                                            {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/"},
                                            {"screenshot", false},
                                            {"include_html", true}}),
                        "test");
    check_startup_timing(value.data);
    if (json_string(value.data, "pending_phase") == "navigation") {
        const auto pending_id = json_string(value.data, "session_id");
        require(!pending_id.empty() && value.image_base64.empty() && value.data["observation_id"].is_null() &&
                    json_string(value.data, "page_status") == "loading",
                "slow startup returns owned continuation without inventing a page observation");
        rejects([&] { browser.perform({{"action", "observe"}, {"session_id", pending_id}}, "other"); },
                "SESSION_NOT_FOUND");
        rejects(
            [&] {
                browser.perform({{"action", "click"},
                                 {"session_id", pending_id},
                                 {"observation_id", "not-observed"},
                                 {"x", 10},
                                 {"y", 10}},
                                "test");
            },
            "STALE_OBSERVATION");
        if (options.defer_initial_navigation) {
            page.reject_requests = true;
            rejects(
                [&] {
                    browser.perform(contract.arguments("devbox_browser",
                                                       {{"action", "observe"}, {"session_id", pending_id}}),
                                    "test");
                },
                "BROWSER_NAVIGATION");
            page.reject_requests = false;
        }
        value = browser.perform(
            contract.arguments("devbox_browser", {{"action", "observe"}, {"session_id", pending_id}}),
            "test");
        require(json_string(value.data, "session_id") == pending_id,
                "continuation reuses the same owned session");
        require(value.image_base64.empty() && value.data.contains("html_artifact") &&
                    value.data["html_artifact"].is_object(),
                "validated omission inherits capture choices after failed and successful continuation");
    } else {
        require(!options.defer_initial_navigation, "forced continuation regression must execute");
    }
    require(json_string(value.data, "text").find("GBP 649.00") != std::string::npos,
            "JavaScript-rendered live price text");
    check_navigation_timing(value.data);
    require(value.image_base64.empty(), "explicit screenshot=false survives initial navigation");
    value = browser.perform(contract.arguments("devbox_browser", {{"action", "observe"},
                                                                  {"session_id", value.data.at("session_id")},
                                                                  {"screenshot", true},
                                                                  {"include_html", true}}),
                            "test");
    require(!value.image_base64.empty() && base64_decode(value.image_base64).at(0) == 0xff,
            "real JPEG screenshot");
#ifdef __linux__
    auto launch_arguments =
        read_file(fs::path("/proc") / std::to_string(launched_pid.load()) / "cmdline", 16384);
    // Chromium can rewrite Linux argv as one space-separated process title.
    // Both the initial NUL-separated argv and that published title are valid.
    std::replace(launch_arguments.begin(), launch_arguments.end(), '\0', ' ');
    const auto launch_tokens = split(launch_arguments, ' ');
    require((std::find(launch_tokens.begin(), launch_tokens.end(), "--disable-gpu") != launch_tokens.end()) ==
                headed,
            "software rendering is opt-in and reaches the owned Chromium process");
    require(launch_arguments.find("--no-sandbox") == std::string::npos,
            "software rendering retains Chromium sandboxing");
#endif
    std::cerr << "[browser] initial owned screenshot passed\n" << std::flush;
    require(json_string(value.data, "text")
                    .find(compatibility ? "Fixture webdriver=false" : "Fixture webdriver=true") !=
                std::string::npos,
            "operator compatibility choice reaches the real browser without page-script patching");
    require(json_bool(value.data.at("browser"), "compatibility_requested") == compatibility,
            "observations disclose the requested compatibility mode");
    const auto id = json_string(value.data, "session_id"),
               observation = json_string(value.data, "observation_id");
    rejects(
        [&] {
            browser.perform({{"action", "click"}, {"session_id", id}, {"observation_id", observation}},
                            "test");
        },
        "INPUT_REQUIRED");
    const auto profile = fs::temp_directory_path() / ("devbox-browser-" + id);
    require(fs::is_directory(profile), "owned private browser profile exists");
    std::vector<std::pair<std::uint32_t, std::optional<std::uint64_t>>> owned_processes;
    const auto initial_pid = launched_pid.load();
    const auto initial_instance = process_instance(initial_pid);
    require(initial_pid != 0 && initial_instance && process_matches_instance(initial_pid, initial_instance),
            "the originally launched browser remains owned after rendering");
    owned_processes.emplace_back(initial_pid, initial_instance);
#ifdef __linux__
    for (const auto& entry : fs::directory_iterator("/proc")) {
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream stream(entry.path() / "cmdline", std::ios::binary);
        std::string command(8192, '\0');
        stream.read(command.data(), command.size());
        command.resize(static_cast<std::size_t>(stream.gcount()));
        if (command.find(path_text(profile)) != command.npos) {
            const auto pid = static_cast<std::uint32_t>(std::stoul(name));
            owned_processes.emplace_back(pid, process_instance(pid));
        }
    }
    require(!owned_processes.empty(), "observed owned Chromium processes before close");
#endif
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
    if (json_number(nested.data["scroll_regions"][0], "scroll_top") <= 200)
        std::cerr << Json{{"before_panes", click.data["scroll_regions"]},
                          {"after_panes", nested.data["scroll_regions"]},
                          {"before_viewport", click.data["viewport"]},
                          {"after_viewport", nested.data["viewport"]},
                          {"movement", nested.data["scroll_result"]}}
                         .dump()
                  << '\n';
    require(json_number(nested.data["scroll_regions"][0], "scroll_top") > 200,
            "nested pane scrolling moved the intended container");
    rejects(
        [&] {
            browser.perform({{"action", "type"},
                             {"session_id", id},
                             {"observation_id", nested.data["observation_id"]},
                             {"text", "unused"}},
                            "test");
        },
        "TEXT_TARGET");
    rejects(
        [&] { browser.perform({{"action", "scroll"}, {"session_id", id}, {"observation_id", ""}}, "test"); },
        "STALE");
    rejects(
        [&] {
            browser.perform({{"action", "navigate"}, {"session_id", id}, {"url", "http://169.254.169.254/"}},
                            "test");
        },
        "PRIVATE");
    auto stable_click =
        browser.perform({{"action", "navigate"},
                         {"session_id", id},
                         {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/control-change"}},
                        "test");
    const auto selected_control = stable_click.data["controls"][0];
    const auto change_link = [&](unsigned revision) {
        page.href_revision = revision;
        const auto until = Clock::now() + Millis(5000);
        while (page.href_applied.load() != revision && Clock::now() < until)
            std::this_thread::sleep_for(Millis(10));
        require(page.href_applied.load() == revision,
                "real page acknowledged the asynchronous help-link update");
    };
    change_link(1);
    auto selected = browser.perform({{"action", "click"},
                                     {"session_id", id},
                                     {"observation_id", stable_click.data["observation_id"]},
                                     {"x", json_number(selected_control, "x") + 20},
                                     {"y", json_number(selected_control, "y") + 20}},
                                    "test");
    require(json_string(selected.data, "text").find("Selected White") != std::string::npos,
            "unchanged product control can be clicked after an unrelated href update");
    const auto help_control = selected.data["controls"][1];
    change_link(2);
    rejects(
        [&] {
            browser.perform({{"action", "click"},
                             {"session_id", id},
                             {"observation_id", selected.data["observation_id"]},
                             {"x", json_number(help_control, "x") + 20},
                             {"y", json_number(help_control, "y") + 20}},
                            "test");
        },
        "STALE_OBSERVATION");
    auto wrapped_snapshot = browser.perform({{"action", "observe"}, {"session_id", id}}, "test");
    Json wrapped_control;
    for (const auto& control : wrapped_snapshot.data["controls"])
        if (json_string(control, "name") == "Wrapped radio")
            wrapped_control = control;
    require(wrapped_control.is_object() &&
                json_string(wrapped_control, "activation_href").ends_with("/wrapped-old"),
            "linked radio observation retains its activating ancestor URL");
    change_link(3);
    rejects(
        [&] {
            browser.perform({{"action", "click"},
                             {"session_id", id},
                             {"observation_id", wrapped_snapshot.data["observation_id"]},
                             {"x", json_number(wrapped_control, "x") + 15},
                             {"y", json_number(wrapped_control, "y") + 15}},
                            "test");
        },
        "STALE_OBSERVATION");
#ifdef _WIN32
    const auto delayed_file = profile / "fixture-delayed-close";
    const auto delayed_handle =
        CreateFileW(delayed_file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(delayed_handle != INVALID_HANDLE_VALUE, "owned cleanup-delay fixture opened");
    auto release_handle = std::async(std::launch::async, [&, delayed_handle] {
        const auto until = Clock::now() + Millis(5000);
        while (!cleanup_attempted && Clock::now() < until)
            std::this_thread::sleep_for(Millis(5));
        // Reproduce a real Windows sharing violation beyond the old 250ms retry window.
        std::this_thread::sleep_for(Millis(700));
        CloseHandle(delayed_handle);
    });
#endif
    require(json_bool(browser.perform({{"action", "close"}, {"session_id", id}}, "test").data, "closed"),
            "explicit close completed");
#ifdef _WIN32
    release_handle.get();
#endif
    require(!json_bool(browser.perform({{"action", "status"}}, "test").data, "active_session"),
            "owned browser cleared");
    require(!fs::exists(profile), "private browser profile removed after close");
    for (const auto& [pid, instance] : owned_processes) {
        const auto gone_by = Clock::now() + Millis(2000);
        while (process_matches_instance(pid, instance) && Clock::now() < gone_by)
            std::this_thread::sleep_for(Millis(20));
        require(!process_matches_instance(pid, instance), "owned Chromium process terminated after close");
    }
    auto redirected =
        initial_observation(browser,
                            {{"action", "open"},
                             {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/client-redirect"},
                             {"include_html", true}},
                            "test");
    require(json_string(redirected.data, "url").ends_with("/redirect-target") &&
                json_string(redirected.data, "text").find("GBP 649.00") != std::string::npos &&
                json_bool(redirected.data, "navigation_stable"),
            "client redirect settles on its final readable document");
    const auto redirected_id = json_string(redirected.data, "session_id");
    auto navigated =
        browser.perform({{"action", "navigate"},
                         {"session_id", redirected_id},
                         {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/client-redirect"}},
                        "test");
    require(json_string(navigated.data, "url").ends_with("/redirect-target") &&
                json_string(navigated.data, "text").find("GBP 649.00") != std::string::npos &&
                json_bool(navigated.data, "navigation_stable"),
            "existing session settles after a client redirect");
    auto fragment = browser.perform(
        {{"action", "navigate"},
         {"session_id", redirected_id},
         {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/redirect-target#section"}},
        "test");
    require(json_string(fragment.data, "url").ends_with("/redirect-target#section") &&
                json_string(fragment.data, "text").find("GBP 649.00") != std::string::npos &&
                json_bool(fragment.data, "navigation_stable"),
            "same-document navigation does not require a new loader");
    auto routed =
        browser.perform({{"action", "navigate"},
                         {"session_id", redirected_id},
                         {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/hash-route"}},
                        "test");
    require(json_string(routed.data, "url").ends_with("/hash-route#phone") &&
                json_string(routed.data, "text").find("GBP 749.00") != std::string::npos &&
                json_bool(routed.data, "navigation_stable"),
            "hash-route changes restart the document settling interval");
    auto slow =
        browser.perform({{"action", "navigate"},
                         {"session_id", redirected_id},
                         {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/slow-readable"}},
                        "test");
    require(json_string(slow.data, "text").find("GBP 849.00") != std::string::npos &&
                json_bool(slow.data, "navigation_stable"),
            "a page readable after fifteen seconds fits within the overall action budget");
    auto navigation_cancel = std::make_shared<Cancellation>();
    auto abort_navigation = std::async(std::launch::async, [navigation_cancel] {
        std::this_thread::sleep_for(Millis(250));
        navigation_cancel->cancel();
    });
    bool cancelled = false;
    try {
        browser.perform({{"action", "navigate"},
                         {"session_id", redirected_id},
                         {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/client-redirect"}},
                        "test", navigation_cancel);
    } catch (const Cancelled&) {
        cancelled = true;
    }
    abort_navigation.get();
    require(cancelled, "redirect settlement remains cancellable");
    const auto cancelled_status = browser.perform({{"action", "status"}}, "test");
    if (json_bool(cancelled_status.data, "active_session"))
        browser.perform({{"action", "close"}, {"session_id", redirected.data.at("session_id")}}, "test");
    else
        require(cancelled_status.data["last_session_end"]["reason"] == "protocol_closed",
                "cancelled protocol I/O retires the disconnected owned context");
    std::cout << "real Chromium rendering, image, HTML, scroll, click, ownership and bounds passed\n";
}
void continuation_budget_tests(bool compatibility) {
    PageFixture page;
    auto config = std::make_shared<Config>();
    config->runtime_mode = RuntimeMode::host;
    config->host_exec_enabled = true;
    config->platform = Platform::detect();
    ToolContract contract(*config);
    const auto base = "http://127.0.0.1:" + std::to_string(page.port());
    BrowserOptions options;
    options.fixture_port = page.port();
    options.compatibility = compatibility;
    options.navigation_wait = Millis(2000);
    {
        BrowserService browser(config, options);
        const auto first = initial_observation(browser, {{"action", "open"}, {"url", base + "/"}}, "owner");
        const auto id = json_string(first.data, "session_id");
        auto pending = browser.perform(contract.arguments("devbox_browser", {{"action", "navigate"},
                                                                             {"session_id", id},
                                                                             {"url", base + "/continuation"},
                                                                             {"screenshot", false},
                                                                             {"include_html", true}}),
                                       "owner");
        require(pending.data["pending_phase"] == "document" && pending.data["navigation_started"] == true &&
                    pending.data["observation_id"].is_null() && pending.image_base64.empty(),
                "slow document returns owned pending state without invented observation or screenshot");
        check_navigation_timing(pending.data);
        const auto submitted_timing = pending.data["timing"]["navigation"];
        rejects(
            [&] {
                browser.perform({{"action", "click"},
                                 {"session_id", id},
                                 {"observation_id", first.data.at("observation_id")},
                                 {"x", 10},
                                 {"y", 10}},
                                "owner");
            },
            "STALE_OBSERVATION");
        rejects([&] { browser.perform({{"action", "observe"}, {"session_id", id}}, "other"); },
                "SESSION_NOT_FOUND");
        for (unsigned attempt = 0; json_string(pending.data, "next_action") == "observe" && attempt < 4;
             ++attempt)
            pending = browser.perform(
                contract.arguments("devbox_browser", {{"action", "observe"}, {"session_id", id}}), "owner");
        require(json_string(pending.data, "next_action").empty() &&
                    json_bool(pending.data, "navigation_stable") &&
                    json_string(pending.data, "text").find("GBP 749.00") != std::string::npos,
                "continuation reaches the newly readable product document");
        require(page.continuation_requests == 1 &&
                    json_string(pending.data, "text").find("Visit 1") != std::string::npos,
                "observing pending navigation must not submit Page.navigate again");
        check_navigation_timing(pending.data);
        require(pending.data["timing"]["navigation"]["submission_ms"] == submitted_timing["submission_ms"] &&
                    json_uint(pending.data["timing"]["navigation"], "document_wait_ms") >=
                        json_uint(submitted_timing, "document_wait_ms"),
                "continuation retains one submission duration and accumulates document waiting");
        require(pending.image_base64.empty() && pending.data["html_artifact"].is_object(),
                "contract validation preserves original false screenshot and true HTML choices");
        browser.perform({{"action", "close"}, {"session_id", id}}, "owner");
    }
    options.navigation_wait = Millis(30000);
    options.defer_initial_observation = true;
    {
        BrowserService browser(config, options);
        auto pending = browser.perform({{"action", "open"},
                                        {"url", base + "/capture-continuation"},
                                        {"screenshot", true},
                                        {"include_html", true}},
                                       "capture-owner");
        check_startup_timing(pending.data);
        check_navigation_timing(pending.data);
        const auto settled_timing = pending.data["timing"]["navigation"];
        require(pending.data["pending_phase"] == "observation" &&
                    pending.data["navigation_started"] == true && pending.data["observation_id"].is_null() &&
                    pending.image_base64.empty(),
                "snapshot deferral retains a settled document without pretending capture succeeded");
        const auto id = json_string(pending.data, "session_id");
        const auto observed =
            browser.perform(contract.arguments("devbox_browser", {{"action", "observe"}, {"session_id", id}}),
                            "capture-owner");
        require(!observed.image_base64.empty() && observed.data["html_artifact"].is_object() &&
                    json_string(observed.data, "text").find("Capture visit 1") != std::string::npos &&
                    page.capture_requests == 1,
                "deferred snapshot captures JPEG and HTML from the same single navigation");
        require(observed.data["timing"]["navigation"] == settled_timing &&
                    !observed.data["timing"].contains("startup"),
                "capture-only continuation preserves navigation timings without repeating startup metadata");
        browser.perform({{"action", "close"}, {"session_id", id}}, "capture-owner");
    }
    options.defer_initial_observation = false;
    options.text_only = true;
    bool uncertain_acknowledgement = false;
    unsigned expected_capture_requests = 0;
    options.navigation_acknowledgement_probe = [&] {
        if (uncertain_acknowledgement) {
            const auto by = Clock::now() + Millis(5000);
            while (page.capture_requests < expected_capture_requests && Clock::now() < by)
                std::this_thread::sleep_for(Millis(10));
            require(page.capture_requests == expected_capture_requests,
                    "the uncertain navigation was actually submitted to the fixture");
            // Let its image interception arrive while the CDP reply is withheld
            // by this native fault injection. Disposal must not re-enter State.
            std::this_thread::sleep_for(Millis(200));
            throw Error("BROWSER_PROTOCOL_REMOTE: qualification-injected uncertain acknowledgement");
        }
    };
    {
        BrowserService browser(config, options);
        const auto initial =
            initial_observation(browser, {{"action", "open"}, {"url", base + "/"}}, "uncertain-owner");
        const auto id = json_string(initial.data, "session_id");
        uncertain_acknowledgement = true;
        const auto visits_before = page.capture_requests.load();
        expected_capture_requests = visits_before + 1;
        rejects(
            [&] {
                browser.perform(
                    {{"action", "navigate"}, {"session_id", id}, {"url", base + "/capture-continuation"}},
                    "uncertain-owner");
            },
            "BROWSER_PROTOCOL_REMOTE");
        const auto status = browser.perform({{"action", "status"}}, "uncertain-owner");
        require(!json_bool(status.data, "active_session") &&
                    status.data["last_session_end"]["reason"] == "navigation_uncertain",
                "a connected but uncertain navigation cannot retain an old-document continuation");
        rejects([&] { browser.perform({{"action", "observe"}, {"session_id", id}}, "uncertain-owner"); },
                "BROWSER_SESSION_ENDED: navigation_uncertain");
        require(page.capture_requests == visits_before + 1, "an uncertain navigation is never resubmitted");
    }
    std::cout
        << "slow navigation and snapshot continuation retain ownership, capture choices and one navigation\n";
}
void shared_browser_tests(bool headed, bool compatibility) {
    PageFixture page(true);
    auto config = std::make_shared<Config>();
    config->runtime_mode = RuntimeMode::host;
    config->host_exec_enabled = true;
    config->platform = Platform::detect();
    std::atomic_uint32_t pid{0};
    BrowserOptions owned_options;
    owned_options.headed = headed;
    owned_options.compatibility = compatibility;
    owned_options.launch_diagnostics = browser_diagnostics;
    owned_options.fixture_port = page.port();
    owned_options.pid_observer = [&](std::uint32_t value) { pid = value; };
    BrowserService managed(config, owned_options);
    const Json open{{"action", "open"}, {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/"}};
    auto initial = initial_observation(managed, open, "operator");
    const auto managed_id = json_string(initial.data, "session_id");
    const auto images_until = Clock::now() + Millis(10000);
    while (page.picture_requests < 33 ||
           json_string(initial.data, "text").find("Fixture image loaded") == std::string::npos) {
        require(Clock::now() < images_until, "default fixture image requests settle");
        initial = managed.perform(
            {{"action", "observe"}, {"session_id", managed_id}, {"wait_ms", 100}, {"screenshot", false}},
            "operator");
    }
    const auto profile = fs::temp_directory_path() / ("devbox-browser-" + managed_id);
    const auto instance = process_instance(pid.load());
    require(instance.has_value(), "managed process identity is observable");
    std::string supervisor_birth = std::to_string(*instance);
#ifdef __linux__
    // Match the real Node supervisor receipt, rather than accidentally feeding
    // the native boot-bound fingerprint back into the controller as start ticks.
    const auto proc_stat = read_file(fs::path("/proc") / std::to_string(pid.load()) / "stat", 8192);
    std::istringstream proc_fields(proc_stat.substr(proc_stat.rfind(')') + 1));
    for (unsigned i = 0; i < 20; ++i)
        require(static_cast<bool>(proc_fields >> supervisor_birth), "Linux supervisor start ticks");
    require(supervisor_birth != std::to_string(*instance),
            "raw ticks differ from native boot-bound identity");
#endif
    auto store_state = [](BrowserService& browser, const BrowserObservation& observation,
                          std::string_view owner) {
        for (const auto& control : observation.data.at("controls")) {
            if (json_string(control, "name") != "Store fixture state")
                continue;
            auto result =
                browser.perform({{"action", "click"},
                                 {"session_id", observation.data.at("session_id")},
                                 {"observation_id", observation.data.at("observation_id")},
                                 {"x", json_number(control, "x") + json_number(control, "width") / 2},
                                 {"y", json_number(control, "y") + json_number(control, "height") / 2}},
                                owner);
            const auto text = json_string(result.data, "text");
            require(text.find("Cookie: devboxFixture=stored; Storage: stored") != text.npos,
                    "real click stores cookie and local storage");
            return;
        }
        throw Error("Storage fixture control missing");
    };
    store_state(managed, initial, "operator");
    const auto status_path = profile / "supervisor-fixture.json";
    Json status{{"ready", true},
                {"browser_mode", "shared_native"},
                {"browser_profile", path_text(profile)},
                {"children", Json::array({{{"name", "chromium"},
                                           {"pid", pid.load()},
                                           {"startTicks", supervisor_birth},
                                           {"running", true}}})}};
    write_json_atomic(status_path, status);
    const auto endpoint = split(read_file(profile / "DevToolsActivePort", 512), '\n');
    BrowserProtocol probe;
    probe.connect(static_cast<std::uint16_t>(std::stoul(endpoint.at(0))), trim(endpoint.at(1)),
                  Clock::now() + Millis(5000));
    auto contexts = [&] {
        return probe.call("Target.getBrowserContexts", Json::object(), Clock::now() + Millis(5000))
            .at("browserContextIds");
    };
    require(contexts().empty(), "managed default browser has no private contexts");
    BrowserOptions shared_options;
    shared_options.headed = headed;
    shared_options.compatibility = compatibility;
    shared_options.fixture_port = page.port();
    shared_options.shared_profile = profile;
    shared_options.shared_status_file = status_path;
    shared_options.text_only = true;
    shared_options.idle_timeout = Millis(3000);
    {
        BrowserService shared(config, shared_options);
        status["children"][0]["startTicks"] = std::to_string(std::stoull(supervisor_birth) + 1);
        write_json_atomic(status_path, status);
        require(!json_bool(shared.perform({{"action", "status"}}, "first").data, "available"),
                "changed process identity is unavailable");
        rejects([&] { shared.perform(open, "first"); }, "SHARED_IDENTITY");
        require(contexts().empty(), "rejected identity does not create a context");
        status["children"][0]["startTicks"] = supervisor_birth;
        write_json_atomic(status_path, status);
        const auto endpoint_file = profile / "DevToolsActivePort";
        const auto original_endpoint = read_file(endpoint_file, 512);
        {
            ProtocolFixture stalled([](auto& socket) {
                const auto request = receive(socket);
                require(json_string(request, "method") == "Browser.getVersion",
                        "health probe only requests browser version");
                // Wait for the probe's deadline to close the socket, not an arbitrary sleep.
                beast::flat_buffer ignored;
                boost::system::error_code error;
                socket.read(ignored, error);
                require(static_cast<bool>(error), "health probe closes its stalled connection");
            });
            write_file(endpoint_file, std::to_string(stalled.port()) + "\n/devtools/browser/stalled\n");
            const auto before = Clock::now();
            const auto health = shared.perform({{"action", "status"}}, "first").data;
            write_file(endpoint_file, original_endpoint);
            stalled.finish();
            require(!json_bool(health, "available") && !json_bool(health, "managed_ready") &&
                        json_string(health, "reason").find("BROWSER_SHARED_UNRESPONSIVE") == 0,
                    "live process and stale endpoint cannot report a healthy browser");
            require(Clock::now() - before < Millis(6000), "health failure is bounded below action timeout");
            require(process_matches_instance(pid.load(), instance) && contexts().empty(),
                    "readiness failure preserves managed process and creates no context");
        }
        require(json_bool(shared.perform({{"action", "status"}}, "first").data, "available"),
                "readiness recovers after browser answers again");
        const auto pictures_before = page.picture_requests.load();
        auto text_open = open;
        text_open["render_mode"] = "auto";
        auto first = initial_observation(shared, text_open, "first");
        require(first.data.at("render_mode") == "text" &&
                    json_uint(first.data, "omitted_network_resources") >= 33 &&
                    json_string(first.data, "text").find("Fixture image omitted") != std::string::npos &&
                    page.picture_requests == pictures_before,
                "text mode rejects image bursts before network access while rendering live text");
        require(contexts().size() == 1, "one isolated browser context created");
        require(shared.perform({{"action", "status"}}, "first").data.at("session_id") ==
                    first.data.at("session_id"),
                "owner can recover its session after a lost response");
        require(shared.perform({{"action", "status"}}, "another").data.at("session_id").is_null(),
                "status does not disclose another principal's session identifier");
        require(json_string(first.data.at("browser"), "mode") == "managed_process_isolated_context",
                "observation identifies shared process mode");
        require(json_uint(first.data.at("network"), "accepted_connections") > 0,
                "context traffic passes through its own bounded egress proxy");
        require(json_string(first.data, "text").find("Cookie: empty; Storage: empty") != std::string::npos,
                "private context does not inherit default profile state");
        first = shared.perform(
            {{"action", "observe"}, {"session_id", first.data.at("session_id")}, {"wait_ms", 3000}}, "first");
        std::this_thread::sleep_for(Millis(600));
        first = shared.perform({{"action", "observe"}, {"session_id", first.data.at("session_id")}}, "first");
        require(contexts().size() == 1, "idle allowance starts after a slow observation completes");
        store_state(shared, first, "first");
        shared.perform({{"action", "close"}, {"session_id", first.data.at("session_id")}}, "first");
        require(contexts().empty(), "explicit close disposes the private context");
        require(process_matches_instance(pid.load(), instance) && fs::is_directory(profile),
                "closing a session preserves its managed browser and profile");
        auto full_open = open;
        full_open["render_mode"] = "full";
        auto second = initial_observation(shared, full_open, "second");
        const auto full_until = Clock::now() + Millis(10000);
        while (page.picture_requests < pictures_before + 33 ||
               json_string(second.data, "text").find("Fixture image loaded") == std::string::npos) {
            require(Clock::now() < full_until, "full-mode fixture image requests settle");
            second = shared.perform({{"action", "observe"},
                                     {"session_id", second.data.at("session_id")},
                                     {"wait_ms", 100},
                                     {"screenshot", false}},
                                    "second");
        }
        require(second.data.at("render_mode") == "full" &&
                    json_string(second.data, "text").find("Fixture image loaded") != std::string::npos &&
                    page.picture_requests >= pictures_before + 33,
                "full mode in a new context restores image loading without inheriting interception");
        require(json_string(second.data, "text").find("Cookie: empty; Storage: empty") != std::string::npos,
                "new principal receives neither cookies nor local storage from prior context");
    }
    require(contexts().empty(), "service destruction disposes active private context");
    auto retained = managed.perform({{"action", "observe"}, {"session_id", managed_id}}, "operator");
    require(json_string(retained.data, "text").find("Cookie: devboxFixture=stored; Storage: stored") !=
                std::string::npos,
            "managed default page survives session teardown");
    probe.close();
    managed.perform({{"action", "close"}, {"session_id", managed_id}}, "operator");
    require(!fs::exists(profile), "fixture owner removes only its own managed profile");
    std::cout << "managed browser context, proxy, cookie/storage isolation, identity and disposal passed\n";
}
void session_allowance_tests() {
    PageFixture page;
    auto config = std::make_shared<Config>();
    config->runtime_mode = RuntimeMode::host;
    config->host_exec_enabled = true;
    config->platform = Platform::detect();
    const Json open{{"action", "open"},
                    {"url", "http://127.0.0.1:" + std::to_string(page.port()) + "/"},
                    {"include_html", true}};
    BrowserOptions defaults;
    defaults.fixture_port = page.port();
    BrowserService browser(config, defaults);
    auto first = initial_observation(browser, open, "slow-agent");
    const auto id = json_string(first.data, "session_id");
    const auto profile = fs::temp_directory_path() / ("devbox-browser-" + id);
    std::cerr << "[browser] preserving a real session across a 65-second model-thinking pause\n"
              << std::flush;
    std::this_thread::sleep_for(Millis(65000));
    auto resumed = browser.perform({{"action", "observe"}, {"session_id", id}}, "slow-agent");
    require(json_string(resumed.data, "text").find("GBP 649.00") != std::string::npos,
            "a normal model-thinking pause does not discard the live page");
    require(json_uint(resumed.data["session_budget"], "idle_remaining_ms") > 60000 &&
                json_uint(resumed.data["session_budget"], "hard_remaining_ms") > 0,
            "fresh observation reports a usable bounded continuation allowance");
    require(browser.perform({{"action", "status"}}, "other").data["session_budget"].is_null(),
            "another principal cannot read the active session budget");
    browser.perform({{"action", "close"}, {"session_id", id}}, "slow-agent");
    require(!fs::exists(profile), "longer idle allowance still disposes the owned profile on close");

    for (const bool hard : {false, true}) {
        BrowserOptions bounded;
        bounded.fixture_port = page.port();
        bounded.idle_timeout = Millis(hard ? 10000 : 1000);
        bounded.lifetime = Millis(hard ? 8000 : 20000);
        BrowserService expiring(config, bounded);
        auto value = initial_observation(expiring, open, "owner");
        const auto session = json_string(value.data, "session_id");
        const auto html_hash = json_string(value.data["html_artifact"], "sha256");
        if (!hard) {
            std::this_thread::sleep_for(Millis(650));
            const auto html = expiring.perform({{"action", "read_html"},
                                                {"session_id", session},
                                                {"html_sha256", html_hash},
                                                {"max_bytes", 128}},
                                               "owner");
            require(json_uint(html.data["session_budget"], "idle_remaining_ms") > 900,
                    "a successful retained HTML read renews the idle allowance");
            std::this_thread::sleep_for(Millis(650));
            require(json_bool(expiring.perform({{"action", "status"}}, "owner").data, "active_session"),
                    "valid evidence reading preserves the session beyond the prior idle deadline");
        }
        const auto deadline = Clock::now() + Millis(15000);
        Json status;
        do {
            std::this_thread::sleep_for(Millis(100));
            if (hard) {
                try {
                    const auto html = expiring.perform({{"action", "read_html"},
                                                        {"session_id", session},
                                                        {"html_sha256", html_hash},
                                                        {"max_bytes", 128}},
                                                       "owner");
                    require(html.data["session_budget"].is_object(),
                            "HTML reads disclose remaining lifetime");
                } catch (const Error& e) {
                    require(std::string_view(e.what()).find("BROWSER_SESSION_ENDED: hard_lifetime") !=
                                std::string_view::npos,
                            "hard lifetime has an explicit reason despite continuing activity");
                }
            } else {
                try {
                    expiring.perform({{"action", "read_html"},
                                      {"session_id", session},
                                      {"html_sha256", "invalid"},
                                      {"max_bytes", 128}},
                                     "owner");
                    throw Error("invalid HTML read was accepted");
                } catch (const Error& e) {
                    const std::string_view message(e.what());
                    require(message.find("BROWSER_HTML_CHANGED") != message.npos ||
                                message.find("BROWSER_SESSION_ENDED: idle_timeout") != message.npos,
                            "invalid reads are rejected without renewing the idle allowance");
                }
            }
            status = expiring.perform({{"action", "status"}}, "owner").data;
        } while (json_bool(status, "active_session") && Clock::now() < deadline);
        require(!json_bool(status, "active_session"),
                "status checks never renew idle expiry and activity never renews hard expiry");
        const std::string reason = hard ? "hard_lifetime" : "idle_timeout";
        require(status["last_session_end"]["reason"] == reason &&
                    status["last_session_end"]["session_id"] == session && status["session_budget"].is_null(),
                "the owner receives the actual expiry reason without retained page data");
        require(expiring.perform({{"action", "status"}}, "other").data["last_session_end"].is_null(),
                "expiry diagnostics are private to their principal");
        rejects([&] { expiring.perform({{"action", "observe"}, {"session_id", session}}, "other"); },
                "SESSION_NOT_FOUND");
        rejects([&] { expiring.perform({{"action", "observe"}, {"session_id", session}}, "owner"); },
                "BROWSER_SESSION_ENDED: " + reason);
        require(!fs::exists(fs::temp_directory_path() / ("devbox-browser-" + session)),
                "expiry removes the exact owned profile");
    }
    std::cout << "model-thinking allowance, expiry bounds, owner diagnostics and cleanup passed\n";
}
int main(int argc, char** argv) {
    try {
        fixture_transport_tests();
        if (argc == 2 && std::string_view(argv[1]) == "--fixture-transport")
            return 0;
        protocol_tests();
        proxy_tests();
        if (argc > 1) {
            const auto mode = std::string_view(argv[1]);
            require(mode == "--chromium" || mode == "--chromium-headed", "unknown browser fixture mode");
            require(argc <= 3 && (argc == 2 || std::string_view(argv[2]) == "--compatibility"),
                    "unknown browser fixture option");
            const bool headed = mode == "--chromium-headed", compatibility = argc == 3;
            browser_tests(headed, compatibility);
            shared_browser_tests(headed, compatibility);
            if (!headed)
                continuation_budget_tests(compatibility);
            if (!headed && !compatibility)
                session_allowance_tests();
        }
        std::cout << "browser checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
