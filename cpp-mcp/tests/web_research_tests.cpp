#include "devbox/contract.hpp"
#include "devbox/oauth.hpp"
#include "devbox/research.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using namespace devbox;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using Tcp = asio::ip::tcp;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F&& fn, std::string_view expected) {
    try {
        fn();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(expected) != std::string_view::npos)
            return;
        throw Error("Unexpected rejection: " + std::string(error.what()));
    }
    throw Error("Expected rejection: " + std::string(expected));
}
std::string page(std::string id) {
    return "<!doctype html><html><head><title>Photon research document " + id +
           "</title><meta property='article:published_time' content='2026-09-18T12:00:00Z'></head>"
           "<body><main><h1>Photon research document " +
           id + "</h1><p>" +
           "This independent fixture document records photon detector measurements, instrument calibration, "
           "uncertainty, and experimental methods. The observation identifier is " +
           id +
           ". It contains enough substantive source text to distinguish real evidence from an empty HTML "
           "shell."
           "</p><table><tr><th>Variant</th><th>Price</th></tr><tr><td>Detector " +
           id + "</td><td>&pound;123.45</td></tr></table></main></body></html>";
}
struct HttpFixture {
    asio::io_context io;
    Tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic_bool stop{false};
    std::atomic_size_t requests{0}, connections{0}, active{0}, peak{0}, not_modified{0}, transient_calls{0};
    std::atomic_size_t ddg_queries{0}, bing_queries{0};
    std::atomic_int ddg_mode{1}, bing_mode{0};
    std::mutex search_mutex;
    std::vector<Clock::time_point> bing_starts;
    std::thread listener;
    std::vector<std::thread> workers;
    HttpFixture() {
        acceptor.non_blocking(true);
        listener = std::thread([this] {
            while (!stop) {
                boost::system::error_code error;
                Tcp::socket socket(io);
                acceptor.accept(socket, error);
                if (error) {
                    std::this_thread::sleep_for(Millis(2));
                    continue;
                }
                ++connections;
                workers.emplace_back([this, socket = std::move(socket)]() mutable {
                    boost::beast::flat_buffer buffer;
                    while (!stop) {
                        boost::system::error_code ec;
                        http::request<http::string_body> request;
                        http::read(socket, buffer, request, ec);
                        if (ec)
                            break;
                        ++requests;
                        const auto now = ++active;
                        auto observed = peak.load();
                        while (observed < now && !peak.compare_exchange_weak(observed, now)) {
                        }
                        const auto target = std::string(request.target());
                        http::response<http::string_body> response{http::status::ok, 11};
                        response.keep_alive(true);
                        response.set(http::field::content_type, "text/html; charset=utf-8");
                        response.set(http::field::etag, "\"fixture-v1\"");
                        if (target.starts_with("/search/duckduckgo_html")) {
                            const auto count = ++ddg_queries;
                            if (ddg_mode == 0 || (ddg_mode == 4 && count == 1)) {
                                response.body() =
                                    "<html><a class='result__a' href='https://duckduckgo.com/l/?uddg=" +
                                    url_encode(url("/doc/discovered")) +
                                    "'>Photon detector evidence</a></html>";
                            } else if (ddg_mode == 3) {
                                response.body() = "<html><div class='no-results__message'>No results found "
                                                  "for this query</div></html>";
                            } else {
                                response.result(http::status::accepted);
                                response.body() =
                                    R"(<html><title>DuckDuckGo</title><form id="challenge-form" action="/anomaly.js"><div class="anomaly-modal__mask">Complete this human verification</div></form></html>)";
                            }
                        } else if (target.starts_with("/search/bing_rss")) {
                            ++bing_queries;
                            {
                                std::lock_guard lock(search_mutex);
                                bing_starts.push_back(Clock::now());
                            }
                            if (bing_mode == 5) {
                                response.result(http::status::too_many_requests);
                                response.set(http::field::retry_after, "1800");
                                response.body() = "Retry later";
                            } else if (bing_mode == 1) {
                                response.result(http::status::forbidden);
                                response.body() = "Access denied";
                            } else {
                                response.set(http::field::content_type, "application/rss+xml; charset=utf-8");
                                response.body() = "<rss version='2.0'><channel><title>Fixture search</title>";
                                if (bing_mode != 3)
                                    for (int i = 0; i < 3; ++i)
                                        response.body() += "<item><title>Photon detector</title><link>" +
                                                           url("/doc/search-" + std::to_string(i)) +
                                                           "</link><description>This snippet is not a source "
                                                           "document</description></item>";
                                response.body() += "</channel></rss>";
                            }
                        } else if (target == "/robots.txt") {
                            response.set(http::field::content_type, "text/plain");
                            response.body() = "User-agent: *\nDisallow: /denied\nAllow: /denied/allowed\n";
                        } else if (target == "/rate") {
                            response.result(http::status::too_many_requests);
                            response.set(http::field::retry_after, "60");
                            response.body() = "retry later";
                        } else if (target == "/transient" && ++transient_calls == 1) {
                            response.result(http::status::bad_gateway);
                            response.body() = "temporary upstream failure";
                        } else if (target == "/blocked") {
                            response.result(http::status::forbidden);
                            response.body() = "Access denied";
                        } else if (target == "/empty")
                            response.body() = "<html><body></body></html>";
                        else if (target == "/private") {
                            response.result(http::status::found);
                            response.set(http::field::location, "http://169.254.169.254/latest/meta-data/");
                        } else if (target == "/redirect") {
                            response.result(http::status::found);
                            response.set(http::field::location, "/doc/1");
                        } else if (target == "/oversized")
                            response.body() = std::string(8192, 'x');
                        else if (target == "/alias")
                            response.body() = page("/doc/1");
                        else {
                            if (target.starts_with("/delay/")) {
                                const auto end = Clock::now() + Millis(150);
                                while (!stop && Clock::now() < end)
                                    std::this_thread::sleep_for(Millis(2));
                            }
                            if (target == "/stall") {
                                const auto end = Clock::now() + Millis(5000);
                                while (!stop && Clock::now() < end)
                                    std::this_thread::sleep_for(Millis(2));
                            }
                            if (request[http::field::if_none_match] == "\"fixture-v1\"") {
                                ++not_modified;
                                response.result(http::status::not_modified);
                            } else
                                response.body() = page(target);
                        }
                        response.prepare_payload();
                        http::write(socket, response, ec);
                        --active;
                        if (ec)
                            break;
                    }
                });
            }
        });
    }
    ~HttpFixture() {
        stop = true;
        listener.join();
        for (auto& worker : workers)
            worker.join();
    }
    unsigned short port() const {
        return acceptor.local_endpoint().port();
    }
    std::string url(std::string_view path = "/doc/1") const {
        return "http://127.0.0.1:" + std::to_string(port()) + std::string(path);
    }
    web::TransportLimits limits(std::size_t per_origin = 1) const {
        web::TransportLimits value;
        value.fixture_loopback_port = port();
        value.per_origin = per_origin;
        return value;
    }
};
void extraction_tests() {
    web::Transfer challenge;
    challenge.url = challenge.final_url = "https://html.duckduckgo.com/html/";
    challenge.status = 202;
    challenge.headers = Json{{"content-type", "text/html; charset=utf-8"}};
    challenge.body = R"(<html><head><title>DuckDuckGo</title></head><body>
      <form id="challenge-form" action="/anomaly.js"><div class="anomaly-modal__mask">
      Please complete the following challenge to confirm this search was made by a human.
      </div></form></body></html>)";
    require(web::extract_document(challenge)["status"] == "challenge_required",
            "HTTP 202 DuckDuckGo challenge is identified before generic provider failure");
    web::Transfer response;
    response.url = response.final_url = "https://example.org/catalogue/item";
    response.status = 200;
    response.headers = {{"content-type", "text/html; charset=utf-8"}};
    response.body = R"(<html><head><title>Detector &amp; sensor</title>
      <meta property="article:published_time" content="2026-09-18">
      <link rel="canonical" href="/detector">
      <script type="application/ld+json">{"@type":"Product","name":"Café sensor","offers":{"@type":"Offer","price":"123.45","priceCurrency":"GBP"}}</script>
      <script>STEAL_SECRET_INSTRUCTION()</script></head><body><nav>MENU_NOISE</nav><main>
      <p>Photon detector café research measurements and calibration methods are documented with uncertainty and reproducible observations, including instrument context and independent verification.</p>
      <p hidden>HIDDEN_INSTRUCTION</p><table><tr><th>Model<th>Price<tr><td>Café<td>&pound;123.45</table>
      <a href="../specification#details">Photon specification</a></main></body></html>)";
    response.bytes = response.body.size();
    const auto doc = web::extract_document(response);
    require(doc["status"] == "ok", "HTML extracted");
    require(doc["title"] == "Detector & sensor", "entity decoding");
    const auto text = json_string(doc, "text");
    require(text.find("Café") != text.npos && text.find("£123.45") != text.npos,
            "Unicode and currency preserved");
    require(text.find("STEAL_SECRET") == text.npos && text.find("MENU_NOISE") == text.npos &&
                text.find("HIDDEN_INSTRUCTION") == text.npos,
            "executable and hidden content excluded");
    require(doc["tables"][0][1][1] == "£123.45", "malformed HTML table recovery");
    require(doc["structured_metadata"].size() == 2, "bounded product and offer metadata");
    require(doc["structured_metadata"][1]["context_name"] == "Café sensor",
            "offer retains its product identity");
    require(doc["structured_metadata"][1]["schema_path"] == "$.offers",
            "structured evidence retains JSON location");
    require(web::exact_term_matches(doc, {"Photon detector"}).size() == 1, "exact entity evidence");
    require(web::exact_term_matches(Json{{"title", "华为麒麟9020处理器"}}, {"麒麟9020"}).size() == 1,
            "CJK exact entities do not require Latin word boundaries");
    require(doc["published_at_reported"] == "2026-09-18", "reported date kept separate");
    require(doc["links"][0]["url"] == "https://example.org/specification", "relative link resolution");
    response.body =
        R"(<html><head><title>Catalogue</title><script type="application/ld+json">{"@type":"ItemList","itemListElement":[{"@type":"WebPage","name":"Child product","url":"https://example.org/child","datePublished":"1999-01-01"}]}</script></head><body><p>This catalogue describes the available measurement instruments and their calibration methods, with pricing and independent specification details for research customers.</p></body></html>)";
    require(web::extract_document(response)["published_at_reported"] == "",
            "child product date is not the catalogue publication date");
    response.body =
        R"(<html><title>Different detector</title><body><form><select><option>Needle Entity</option></select></form><table><tr><td><script>Needle Entity</script>Actual sensor</td></tr></table><p>This source describes another measurement instrument and its calibration methods, with substantive specifications and independently reproducible experiments.</p></body></html>)";
    require(web::exact_term_matches(web::extract_document(response), {"Needle Entity"}).empty(),
            "dropdowns and scripts do not establish entity relevance");
    response.body = "<title>Just a moment...</title><p>Verify you are human</p>";
    require(web::extract_document(response)["status"] == "challenge_required", "challenge not evidence");
    response.headers["content-type"] = "application/pdf";
    response.body = "%PDF-1.7";
    require(web::extract_document(response)["status"] == "unsupported_pdf", "unsupported format explicit");
    response.headers["content-type"] = "text/plain; charset=windows-1252";
    response.body = std::string(150, 'a') + " price " + char(0x80) + "20 " + char(0x95) + " item";
    require(json_string(web::extract_document(response), "text").find("€20 • item") != std::string::npos,
            "native CP1252 conversion");
    response.headers["content-type"] = "text/html; charset=gbk";
    require(web::extract_document(response)["status"] == "unsupported_encoding",
            "unsupported encoding does not become corrupted usable evidence");
    response.headers["content-type"] = "text/html; charset=utf-8";
    response.body = "<p>" + std::string(65533, 'a') + "éééé</p>";
    const auto bounded = web::extract_document(response);
    require(bounded["content_truncated"] == true, "text byte bound explicit");
    (void)bounded.dump();
    require(web::evidence_excerpt("first unrelated section. photon observation matches here. last section.",
                                  "photon", 30)
                    .find("photon") != std::string::npos,
            "query-focused actual excerpt");
}
void policy_tests() {
    for (const auto* ip : {"127.0.0.1", "10.2.3.4", "169.254.169.254", "100.64.0.1", "192.168.1.1", "::1",
                           "::ffff:127.0.0.1", "fc00::1", "fe80::1", "2002:7f00:1::"})
        require(!web::public_address(ip), "special address denied");
    require(web::public_address("8.8.8.8") && web::public_address("2606:4700:4700::1111"),
            "global addresses accepted");
    for (const auto* url : {"file:///etc/passwd", "http://localhost/", "http://2130706433/",
                            "https://user:pass@example.org/", "http://[::1]/", "http://example.org:22/"})
        rejects([&] { (void)web::normalize_url(url); }, "WEB_");
    require(web::normalize_url("../b#x", "https://example.org/a/c") == "https://example.org/b",
            "standard URL normalization");
    const std::string robots =
        "User-agent: *\nDisallow: /private\nAllow: /private/public\nDisallow: /*.pdf$\n\nUser-agent: "
        "DevboxResearch\nDisallow: /secret\nAllow: /secret/open\n";
    require(!web::robots_allowed(robots, "/secret/data") && web::robots_allowed(robots, "/secret/open"),
            "specific robots group and longest allow");
    require(web::robots_allowed(robots, "/private/data"), "specific group supersedes wildcard");
    require(!web::robots_allowed("User-agent: *\nDisallow: /*.pdf$\n", "/paper.pdf"),
            "robots wildcard match");
    const auto standard = web::validate_plan(Json{{"topic", "photon detector research"}});
    const auto fast = web::validate_plan(Json{{"topic", "photon detector research"}, {"mode", "fast"}});
    require(standard["target_sources"] == 100 && fast["target_sources"] == 50, "100 and 50 source budgets");
    rejects([&] { web::validate_plan(Json{{"topic", "x"}, {"urls", {"http://127.0.0.1/"}}}); },
            "WEB_PRIVATE");
    Config config;
    config.runtime_mode = RuntimeMode::host;
    config.platform = Platform::detect();
    ToolContract contract(config);
    require(contract.all().size() == 50, "complete additive contract");
    require(required_tool_scope("devbox_web_fetch") == "mcp:devbox:read", "fetch scope");
    require(required_tool_scope("devbox_web_research") == "mcp:devbox:exec", "research admission scope");
    rejects([&] { contract.arguments("devbox_web_fetch", Json{{"urls", Json::array()}}); }, "arguments");
}
void transport_tests(HttpFixture& server) {
    {
        web::Transport transport(server.limits());
        const auto before = server.connections.load();
        auto first = transport.get({{server.url("/doc/1"), Json::object()}}, Clock::now() + Millis(3000));
        auto second = transport.get({{server.url("/doc/2"), Json::object()}}, Clock::now() + Millis(3000));
        require(first[0].status == 200 && second[0].status == 200, "native GET");
        require(server.connections - before == 1, "connection reused across batches");
        auto redirect =
            transport.get({{server.url("/redirect"), Json::object()}}, Clock::now() + Millis(3000));
        require(redirect[0].final_url == server.url("/doc/1") && redirect[0].status == 200,
                "bounded relative redirect");
        auto denied = transport.get({{server.url("/private"), Json::object()}}, Clock::now() + Millis(3000));
        require(denied[0].error.find("WEB_PRIVATE") != std::string::npos, "redirect to metadata denied");
        auto recovered =
            transport.get({{server.url("/transient"), Json::object()}}, Clock::now() + Millis(3000));
        require(recovered[0].status == 200 && recovered[0].attempts == 2 && server.transient_calls == 2,
                "one bounded retry recovers a transient GET failure");
    }
    {
        auto limits = server.limits(4);
        web::Transport transport(limits);
        server.peak = 0;
        std::vector<web::Request> requests;
        for (int i = 0; i < 8; ++i)
            requests.push_back({server.url("/delay/" + std::to_string(i)), Json::object()});
        auto results = transport.get(requests, Clock::now() + Millis(4000));
        require(results.size() == 8 && server.peak > 1 && server.peak <= 4, "bounded parallel transfers");
    }
    {
        web::Transport transport(server.limits());
        server.peak = 0;
        auto results = transport.get(
            {{server.url("/delay/1"), Json::object()}, {server.url("/delay/2"), Json::object()}},
            Clock::now() + Millis(3000));
        require(results[0].status == 200 && server.peak == 1, "production per-origin bound");
    }
    {
        auto limits = server.limits();
        limits.max_body_bytes = 1024;
        web::Transport transport(limits);
        auto results = transport.get(
            {{server.url("/oversized"), Json::object()}, {server.url("/doc/3"), Json::object()}},
            Clock::now() + Millis(3000));
        require(results[0].error == "WEB_RESPONSE_TOO_LARGE" && results[1].status == 200,
                "oversize is isolated");
    }
    {
        web::Transport transport(server.limits());
        auto result = transport.get({{server.url("/rate"), Json::object()}}, Clock::now() + Millis(3000));
        const auto before = server.requests.load();
        auto next = transport.get({{server.url("/doc/1"), Json::object()}}, Clock::now() + Millis(3000));
        require(result[0].status == 429 && next[0].error.find("WEB_RATE_LIMITED") != std::string::npos &&
                    server.requests == before,
                "Retry-After respected across batches");
    }
    {
        web::Transport transport(server.limits());
        auto cancel = std::make_shared<Cancellation>();
        const auto start = Clock::now();
        auto pending = std::async(std::launch::async, [&] {
            return transport.get({{server.url("/stall"), Json::object()}}, Clock::now() + Millis(6000),
                                 cancel);
        });
        std::this_thread::sleep_for(Millis(40));
        cancel->cancel();
        rejects([&] { pending.get(); }, "cancelled");
        require(Clock::now() - start < Millis(1000), "prompt native cancellation");
    }
}
void research_tests(HttpFixture& server, const fs::path& root) {
    auto config = std::make_shared<Config>();
    config->project_root = root;
    config->jobs_root = root / "jobs";
    config->runtime_mode = RuntimeMode::host;
    web::ResearchService service(config, server.limits());
    const auto first = service.fetch(Json{{"urls", {server.url("/doc/7"), server.url("/doc/7")}}});
    require(first["unique_urls"] == 1 && first["documents"][0]["status"] == "ok",
            "duplicate request suppressed");
    const auto reused = service.fetch(Json{{"urls", {server.url("/doc/7")}}, {"max_age_seconds", 3600}});
    require(reused["documents"][0]["cache_status"] == "fresh_cache", "fresh cache reuse");
    const auto revalidated = service.fetch(Json{{"urls", {server.url("/doc/7")}}, {"max_age_seconds", 0}});
    require(revalidated["documents"][0]["cache_status"] == "revalidated" && server.not_modified > 0,
            "conditional revalidation");
    const auto small = service.fetch(
        Json{{"urls",
              {server.url("/doc/11"), server.url("/doc/12"), server.url("/doc/13"), server.url("/doc/14")}},
             {"max_chars", 4000}});
    require(small.dump().size() <= 4000, "complete fetch response respects output budget");
    require(small["documents"].size() + small["remaining_url_indexes"].size() == 4,
            "unreturned documents are explicit");
    Json partial{{"topic", "photon research"},
                 {"mode", "fast"},
                 {"discovery", "none"},
                 {"urls",
                  {server.url("/doc/1"), server.url("/doc/2"), server.url("/alias"), server.url("/blocked"),
                   server.url("/empty"), server.url("/denied/hidden")}}};
    const auto result = service.run(partial, root / "partial", Millis(10000), {});
    require(result["usable_sources"] == 2 && result["shortfall"] == 48 &&
                result["coverage_status"] == "partial",
            "honest deduplication and shortfall");
    Json urls = Json::array();
    for (int i = 0; i < 110; ++i)
        urls.push_back(server.url("/doc/" + std::to_string(i + 100)));
    Json standard{{"topic", "photon research"}, {"mode", "standard"}, {"discovery", "none"}, {"urls", urls}};
    JobStore store(config);
    const std::string evidence_id = "job-research-evidence-fixture";
    const auto evidence_paths = store.create_job(
        evidence_id,
        Json{{"id", evidence_id}, {"mode", "research"}, {"runtimeMode", "host"}, {"research", standard}},
        Json{{"id", evidence_id},
             {"mode", "research"},
             {"runtimeMode", "host"},
             {"status", "succeeded"},
             {"exitCode", 0}});
    const auto hundred = service.run(standard, evidence_paths.dir / "research", Millis(25000), {});
    require(hundred["usable_sources"] == 100 && hundred["target_met"] == true &&
                hundred["distinct_domains"] == 1,
            "100 distinct documents, one honest domain count");
    std::size_t read_count = 0;
    std::uint64_t offset = 0;
    for (;;) {
        const auto evidence = web::research_evidence(
            store, Json{{"job_id", evidence_id}, {"offset", offset}, {"max_chars", 16000}});
        require(evidence.dump().size() <= 16000, "source ledger output is bounded");
        read_count += evidence["sources"].size();
        if (evidence["next_offset"].is_null())
            break;
        const auto next = evidence["next_offset"].get<std::uint64_t>();
        require(next > offset, "source ledger pagination advances");
        offset = next;
    }
    require(read_count == 100, "all 100 source briefs can be read without hidden truncation");
    const auto detail = web::research_evidence(
        store, Json{{"job_id", evidence_id}, {"source_id", "s1"}, {"max_chars", 16000}});
    require(detail.dump().size() <= 16000 && detail.contains("document"), "source drill-down is bounded");
    standard["mode"] = "fast";
    standard["max_age_seconds"] = 3600;
    const auto fifty = service.run(standard, root / "fast", Millis(20000), {});
    require(fifty["usable_sources"] == 50 && fifty["target_met"] == true, "Fast stops at 50 usable sources");
    std::cout << Json{{"standard", hundred}, {"fast", fifty}, {"partial", result}}.dump() << '\n';
}
void discovery_tests(HttpFixture& server, const fs::path& root) {
    const auto config_for = [&](std::string_view name) {
        auto config = std::make_shared<Config>();
        config->project_root = root / name;
        config->jobs_root = config->project_root / "jobs";
        config->runtime_mode = RuntimeMode::host;
        return config;
    };
    Json plan{{"topic", "Photon detector research"},
              {"mode", "fast"},
              {"discovery", "web"},
              {"queries", {"Photon detector UK", "Photon detector prices"}},
              {"exact_terms", {"Photon"}}};
    auto config = config_for("discovery-recovery");
    server.ddg_queries = 0;
    server.bing_queries = 0;
    web::ResearchService first(config, server.limits());
    const auto recovered = first.run(plan, root / "recovery-result", Millis(20000), {});
    require(recovered["usable_sources"] == 3 && recovered["discovery"]["queries_completed"] == 2 &&
                recovered["discovery"]["fallback_queries"] == 2,
            "independent fallback retrieves real documents after HTTP 202 challenge");
    require(server.ddg_queries == 1 && server.bing_queries == 2 &&
                recovered["providers"][0]["status"] == "challenge_required" &&
                !recovered["providers"][0]["error"].get<std::string>().empty(),
            "blocked provider is stopped after one request with a useful diagnostic");
    web::ResearchService next(config, server.limits());
    plan["queries"] = {"Photon detector current prices"};
    const auto resumed = next.run(plan, root / "recovery-next-job", Millis(10000), {});
    require(server.ddg_queries == 1 && server.bing_queries == 3 &&
                resumed["providers"][0]["status"] == "cooldown" && resumed["usable_sources"] == 3,
            "provider cooldown survives a new research service and job");
    {
        std::lock_guard lock(server.search_mutex);
        require(server.bing_starts.size() >= 3 &&
                    server.bing_starts[1] - server.bing_starts[0] >= Millis(900) &&
                    server.bing_starts[2] - server.bing_starts[1] >= Millis(900),
                "provider pacing applies across queries and jobs");
    }
    const auto health_root = config->project_root / "run" / "web-research-cache" / "discovery";
    auto state = read_json(health_root / "health.json");
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<Millis>(std::chrono::system_clock::now().time_since_epoch()).count());
    state["providers"]["bing_rss"]["next_request_ms"] = now_ms + 1000;
    write_json_atomic(health_root / "health.json", state);
    web::Transport paced_transport(server.limits());
    const auto paced = web::discover_sources(paced_transport, plan, health_root, server.limits(),
                                             Clock::now() + Millis(5), {});
    require(paced["summary"]["status"] == "budget_exhausted" && paced["summary"]["provider_attempts"] == 0,
            "pacing cannot overrun a short discovery budget");
    auto cancel = std::make_shared<Cancellation>();
    const auto began = Clock::now();
    auto pending = std::async(std::launch::async, [&] {
        return web::discover_sources(paced_transport, plan, health_root, server.limits(),
                                     Clock::now() + Millis(3000), cancel);
    });
    std::this_thread::sleep_for(Millis(40));
    cancel->cancel();
    rejects([&] { pending.get(); }, "cancelled");
    require(Clock::now() - began < Millis(700), "provider pacing remains promptly cancellable");

    server.bing_mode = 1;
    config = config_for("all-providers-blocked");
    web::ResearchService blocked(config, server.limits());
    plan["queries"] = {"Photon detector alpha", "Photon detector beta", "Photon detector gamma"};
    plan["urls"] = {server.url("/doc/explicit-seed")};
    const auto seeded = blocked.run(plan, root / "blocked-seeded", Millis(10000), {});
    require(seeded["usable_sources"] == 1 && seeded["stop_reason"] == "discovery_blocked" &&
                seeded["discovery"]["queries_failed"] == 3 && seeded["discovery"]["provider_attempts"] == 2,
            "explicit seeds remain usable and blocked discovery is not source exhaustion");
    plan["urls"] = Json::array();
    web::ResearchService still_blocked(config, server.limits());
    const auto empty = still_blocked.run(plan, root / "blocked-empty", Millis(10000), {});
    require(empty["usable_sources"] == 0 && empty["attempted_urls"] == 0 &&
                empty["stop_reason"] == "discovery_blocked" && empty["discovery"]["provider_attempts"] == 0,
            "repeated blocked jobs make no network attempts and report zero coverage honestly");
    server.ddg_mode = 4;
    server.ddg_queries = 0;
    config = config_for("partial-discovery");
    web::ResearchService partial(config, server.limits());
    const auto mixed = partial.run(plan, root / "partial-discovery-result", Millis(12000), {});
    require(mixed["usable_sources"] == 1 && mixed["discovery"]["queries_completed"] == 1 &&
                mixed["discovery"]["status"] == "partial" && mixed["stop_reason"] == "discovery_incomplete",
            "a few successful queries do not imply complete discovery");
    server.ddg_mode = 3;
    server.bing_mode = 3;
    config = config_for("genuine-no-results");
    web::ResearchService no_results(config, server.limits());
    plan["queries"] = {"Photon detector absent"};
    const auto none = no_results.run(plan, root / "no-results", Millis(10000), {});
    require(none["stop_reason"] == "candidate_exhaustion" && none["discovery"]["status"] == "complete" &&
                none["discovery"]["queries_completed"] == 1,
            "genuine empty searches remain distinct from blocked searches");
    server.ddg_mode = 1;
    server.bing_mode = 5;
    config = config_for("retry-after-discovery");
    web::ResearchService throttled(config, server.limits());
    const auto rate_started = static_cast<std::uint64_t>(
        std::chrono::duration_cast<Millis>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto limited = throttled.run(plan, root / "retry-after-result", Millis(10000), {});
    const auto health =
        read_json(config->project_root / "run" / "web-research-cache" / "discovery" / "health.json");
    require(limited["stop_reason"] == "discovery_blocked" &&
                health["providers"]["bing_rss"]["cooldown_until_ms"].get<std::uint64_t>() >=
                    rate_started + 1800000,
            "longer Retry-After survives in shared provider state");
}
void search_parser_tests() {
    web::Transfer response;
    response.url = response.final_url = "https://www.bing.com/search?format=rss";
    response.status = 200;
    response.body = R"(<rss><channel><title>Search</title>
      <item><link>https://example.org/one?a=1&amp;b=2</link><pubDate>Fri, 1 Jan 2000</pubDate></item>
      <item><link><![CDATA[https://example.org/two]]></link></item>
      <item><link>https://example.org/two</link></item>
      <item><link>javascript:alert(1)</link></item></channel></rss>)";
    const auto parsed = web::parse_search_response("bing_rss", response);
    require(parsed["status"] == "ok" && parsed["urls"].size() == 2 &&
                parsed["urls"][0] == "https://example.org/one?a=1&b=2" && !parsed.contains("pubDate") &&
                parsed.contains("use_notice"),
            "RSS extracts bounded unique URLs and preserves notice without promoting feed dates");
    for (const auto* body : {"<rss><channel>", "<html>Unexpected search markup</html>",
                             "<!DOCTYPE rss [<!ENTITY e SYSTEM 'file:///private'>]><rss><channel/></rss>",
                             "<rss><channel><item><link>javascript:alert(1)</link></item></channel></rss>"}) {
        response.body = body;
        require(web::parse_search_response("bing_rss", response)["status"] == "parse_error",
                "malformed or unsafe RSS is explicit, not a no-results response");
    }
    response.status = 202;
    response.body = "Accepted, processing later";
    const auto accepted = web::parse_search_response("duckduckgo_html", response);
    require(accepted["status"] == "unavailable" && !accepted["error"].get<std::string>().empty(),
            "non-challenge HTTP 202 still has a useful unavailable diagnostic");
    response.status = 200;
    response.headers = Json{{"content-type", "text/html"}};
    response.body = "<html><body>Unexpected search layout</body></html>";
    require(web::parse_search_response("duckduckgo_html", response)["status"] == "parse_error",
            "unrecognized search markup cannot silently become no results");
}
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-web-tests-" + uuid());
    try {
        ensure_directory(root);
        extraction_tests();
        search_parser_tests();
        policy_tests();
        {
            HttpFixture server;
            transport_tests(server);
            research_tests(server, root);
            discovery_tests(server, root);
        }
        if (root.parent_path() == fs::temp_directory_path() &&
            root.filename().string().starts_with("devbox-web-tests-"))
            fs::remove_all(root);
        std::cout << "native web research checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nEvidence: " << root << '\n';
        return 1;
    }
}
