#include "devbox/browser.hpp"
#include "devbox/browser_protocol.hpp"
#include "devbox/browser_proxy.hpp"
#include "devbox/process.hpp"
#include "devbox/research.hpp"
#include <algorithm>
#include <fstream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace devbox {
std::string browser_page_status(const Json& observation, int http_status) {
    const auto title = lower(json_string(observation, "title"));
    const auto text = lower(json_string(observation, "text").substr(0, 8000));
    const auto url = lower(json_string(observation, "url"));
    if (url.starts_with("chrome-error:") || url == "about:blank")
        return "unavailable";
    if (title.find("attention required") != title.npos || title.find("access denied") != title.npos ||
        text.find("sorry, you have been blocked") != text.npos ||
        text.find("you don't have permission to access") != text.npos || http_status == 403)
        return "access_denied";
    if (title.find("just a moment") != title.npos || text.find("verify you are human") != text.npos ||
        text.find("checking your browser") != text.npos ||
        title.find("pardon our interruption") != title.npos)
        return "challenge_required";
    if (http_status == 429)
        return "rate_limited";
    if (http_status == 401)
        return "authentication_required";
    if (http_status >= 400)
        return "http_error";
    if (title.find("before you continue") != title.npos || url.find("//consent.google.") != url.npos)
        return "consent_required";
    if (!json_bool(observation, "navigation_stable"))
        return "navigation_unstable";
    if (json_string(observation, "ready_state") == "loading")
        return "loading";
    if (text.size() < 80)
        return "insufficient_content";
    return "content_available";
}
namespace {
constexpr std::size_t html_limit = 2 * 1024 * 1024;
// Runs only in a fresh isolated world. Page scripts cannot replace these globals.
// Reading live DOM properties avoids confusing serialized attributes with UI state.
constexpr auto observe_script = R"JS((() => {
 const clip=(v,n)=>String(v||'').slice(0,n), clean=v=>clip(v,240).replace(/\s+/g,' ').trim();
 const visible=e=>{const r=e.getBoundingClientRect(),s=getComputedStyle(e);return r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none';};
 const all=document.querySelectorAll('a[href],button,input,select,textarea,[role="button"],[role="checkbox"],[role="tab"],[contenteditable="true"]');
 const controls=[];let examined=0,hidden=0;
 for(const e of all){if(++examined>4000)break;if(!visible(e)){hidden++;continue;}
  const r=e.getBoundingClientRect();if(r.bottom<0||r.right<0||r.top>innerHeight||r.left>innerWidth)continue;
  if(controls.length>=80)break;
  const type=(e.type||'').toLowerCase(); const privateValue=type==='password'||type==='file'||e.autocomplete==='one-time-code';
  controls.push({tag:e.tagName.toLowerCase(),role:e.getAttribute('role')||'',type,
   name:clean(e.getAttribute('aria-label')||e.labels?.[0]?.innerText||e.innerText||e.getAttribute('placeholder')||e.getAttribute('title')),
   href:e.tagName==='A'?clip(e.href,1000):undefined,disabled:!!e.disabled,checked:typeof e.checked==='boolean'?e.checked:undefined,
   value:privateValue?'[redacted]':clip(e.value,160),x:Math.round(Math.max(0,r.left)),y:Math.round(Math.max(0,r.top)),
   width:Math.round(Math.min(innerWidth,r.right)-Math.max(0,r.left)),height:Math.round(Math.min(innerHeight,r.bottom)-Math.max(0,r.top))});
 }
 const scroll_regions=[];let scanned=0;
 for(const e of document.querySelectorAll('body *')){if(++scanned>4000||scroll_regions.length>=12)break;
  if(e.scrollHeight<=e.clientHeight+2||e.clientHeight<40||!visible(e))continue;
  const r=e.getBoundingClientRect(),s=getComputedStyle(e);if(!['auto','scroll'].includes(s.overflowY)||r.bottom<=0||r.top>=innerHeight)continue;
  scroll_regions.push({x:Math.round(r.left),y:Math.round(r.top),width:Math.round(r.width),height:Math.round(r.height),scroll_top:e.scrollTop,scroll_height:e.scrollHeight});
 }
 const viewport_parts=[];let visited=0,viewport_chars=0;
 const walker=document.createTreeWalker(document.body||document.documentElement,NodeFilter.SHOW_TEXT);
 while(walker.nextNode()) {if(++visited>6000||viewport_chars>=4000)break;const n=walker.currentNode,e=n.parentElement;
  if(!e||['SCRIPT','STYLE','NOSCRIPT'].includes(e.tagName)||!n.textContent.trim())continue;
  const range=document.createRange();range.selectNodeContents(n);const r=range.getBoundingClientRect();
  if(r.width<=0||r.height<=0||r.bottom<0||r.top>innerHeight||r.right<0||r.left>innerWidth)continue;
  const s=getComputedStyle(e);if(s.visibility==='hidden'||s.display==='none')continue;
  const t=n.textContent.replace(/\s+/g,' ').trim().slice(0,4000-viewport_chars);viewport_parts.push(t);viewport_chars+=t.length;
 }
 const text=document.body?.innerText||'';
 return {url:location.href,title:clip(document.title,300),ready_state:document.readyState,
  text:clip(text,8000),text_truncated:text.length>8000,viewport_text:viewport_parts.join('\n'),viewport_text_truncated:visited>6000||viewport_chars>=4000,
  controls,controls_truncated:examined>4000||controls.length===80,scroll_regions,
  viewport:{width:innerWidth,height:innerHeight,scroll_x:scrollX,scroll_y:scrollY,document_width:document.documentElement.scrollWidth,document_height:document.documentElement.scrollHeight},
  frames:Array.from(document.querySelectorAll('iframe')).slice(0,20).map(e=>({src:clip(e.src,500),title:clip(e.title,120)})),
  frames_note:'Text and controls describe the top frame; cross-origin frame pixels are present in the screenshot.'};
})())JS";
constexpr auto html_script =
    R"JS((() => {const s=document.documentElement?.outerHTML||'';return {html:s.slice(0,524288),original_utf16_units:s.length,truncated:s.length>524288};})())JS";
std::optional<fs::path> chromium_path(const BrowserOptions& options) {
    if (options.executable && fs::is_regular_file(*options.executable))
        return options.executable;
    if (const auto value = environment("DEVBOX_BROWSER_EXECUTABLE")) {
        const auto path = path_from_utf8(*value);
        if (path.is_absolute() && fs::is_regular_file(path))
            return path;
        return {};
    }
    // Ubuntu's installed Chrome path has the distribution's scoped user-namespace
    // AppArmor policy. Prefer it over an unrelated developer Chromium on PATH;
    // never disable the browser sandbox or the host's namespace restrictions.
#ifdef __linux__
    if (fs::is_regular_file("/opt/google/chrome/chrome"))
        return fs::path("/opt/google/chrome/chrome");
#endif
    for (const auto name : {"google-chrome", "chromium", "chromium-browser", "chrome", "msedge"})
        if (auto found = find_program(name))
            return found;
#ifdef _WIN32
    for (const auto env : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const auto base = environment(env);
        if (!base)
            continue;
        for (const auto suffix :
             {"Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe"}) {
            auto path = path_from_utf8(*base) / suffix;
            if (fs::is_regular_file(path))
                return path;
        }
    }
#endif
#ifdef __APPLE__
    for (const auto path : {"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                            "/Applications/Chromium.app/Contents/MacOS/Chromium"})
        if (fs::is_regular_file(path))
            return fs::path(path);
#endif
    return {};
}
std::optional<std::uint64_t> available_memory() {
#ifndef _WIN32
    std::ifstream stream("/proc/meminfo");
    std::string name, unit;
    std::uint64_t value;
    std::optional<std::uint64_t> available;
    while (stream >> name >> value >> unit)
        if (name == "MemAvailable:") {
            available = value * 1024;
            break;
        }
    // Container namespace mount roots expose the applicable hard memory budget.
    for (const auto& pair : {std::pair{"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory.current"},
                             std::pair{"/sys/fs/cgroup/memory/memory.limit_in_bytes",
                                       "/sys/fs/cgroup/memory/memory.usage_in_bytes"}}) {
        std::ifstream cap(pair.first), used(pair.second);
        std::uint64_t limit = 0, current = 0;
        if ((cap >> limit) && (used >> current) && limit < (1ULL << 60)) {
            const auto left = limit > current ? limit - current : 0;
            available = available ? std::min(*available, left) : left;
        }
    }
    return available;
#endif
    return {};
}
} // namespace
struct BrowserService::State {
    std::shared_ptr<const Config> config;
    BrowserOptions options;
    std::mutex mutex;
    Cancel stopping = std::make_shared<Cancellation>();
    std::thread janitor;
    struct Session {
        std::string id = uuid(), owner, target, cdp_session, observation, html, html_hash, main_frame;
        int http_status = 0;
        Json browser_version = Json::object();
        fs::path profile;
        Clock::time_point created = Clock::now(), touched = created;
        Cancel lifetime = std::make_shared<Cancellation>();
        std::atomic_bool exited{false};
        std::atomic_uint32_t pid{0};
        std::string launch_error;
        std::unique_ptr<BrowserProxy> proxy;
        std::unique_ptr<BrowserProtocol> protocol;
        Json last = Json::object();
        std::thread runner;
        ~Session() {
            // Let Chromium close its own helpers and profile before enforcing the
            // supervisor's process-group/Job Object deadline.
            if (protocol) {
                try {
                    protocol->call("Browser.close", Json::object(), Clock::now() + Millis(1000));
                } catch (...) {
                }
            }
            lifetime->cancel();
            if (runner.joinable())
                runner.join();
            protocol.reset();
            proxy.reset();
            // Only the freshly created, exact profile owned by this session.
            std::error_code error;
            if (!profile.empty() && profile.filename() == "devbox-browser-" + id &&
                fs::is_directory(profile, error) && !fs::is_symlink(fs::symlink_status(profile, error))) {
                for (unsigned attempt = 0; attempt < 5; ++attempt) {
                    error.clear();
                    fs::remove_all(profile, error);
                    if (!error)
                        break;
                    std::this_thread::sleep_for(Millis(50));
                }
            }
        }
    };
    std::unique_ptr<Session> session;
    State(std::shared_ptr<const Config> c, BrowserOptions o) : config(std::move(c)), options(std::move(o)) {
        options.headed = env_bool("DEVBOX_BROWSER_HEADED", options.headed);
    }
    void start_reaper() {
        if (janitor.joinable())
            return;
        janitor = std::thread([this] {
            while (!stopping->wait_for(Millis(500))) {
                std::unique_lock lock(mutex, std::try_to_lock);
                if (lock && session &&
                    (session->exited || Clock::now() - session->touched >= options.idle_timeout ||
                     Clock::now() - session->created >= options.lifetime ||
                     available_memory().value_or(UINT64_MAX) < 64 * 1024 * 1024))
                    session.reset();
            }
        });
    }
    ~State() {
        stop();
    }
    void stop() {
        stopping->cancel();
        if (janitor.joinable())
            janitor.join();
        std::lock_guard lock(mutex);
        session.reset();
    }
    std::string url(std::string_view supplied) const {
        if (options.fixture_port) {
            auto parsed = Url::parse(supplied);
            if (parsed.scheme == "http" && parsed.host == "127.0.0.1" &&
                parsed.port == std::to_string(*options.fixture_port) && parsed.userinfo.empty())
                return std::string(supplied);
        }
        return web::normalize_url(supplied);
    }
    Json command(std::string_view method, Json args, Clock::time_point deadline, const Cancel& cancel,
                 bool browser = false) {
        return session->protocol->call(method, std::move(args), deadline, cancel,
                                       browser ? "" : session->cdp_session);
    }
    Json frame(Clock::time_point deadline, const Cancel& cancel) {
        return command("Page.getFrameTree", Json::object(), deadline, cancel).at("frameTree").at("frame");
    }
    void settle(const Json& navigation, Clock::time_point deadline, const Cancel& cancel) {
        const auto loader = json_string(navigation, "loaderId");
        const auto ready_by = std::min(deadline, Clock::now() + Millis(15000));
        while (Clock::now() < ready_by) {
            cancel->check();
            const auto root = frame(deadline, cancel);
            if ((loader.empty() || json_string(root, "loaderId") == loader) &&
                evaluate("document.readyState", deadline, cancel) != "loading") {
                cancel->wait_for(Millis(600));
                cancel->check();
                return;
            }
            cancel->wait_for(Millis(100));
        }
        throw Error("BROWSER_NAVIGATION_DEADLINE: page did not reach a readable state");
    }
    Json evaluate(std::string_view script, Clock::time_point deadline, const Cancel& cancel) {
        const auto root = frame(deadline, cancel);
        const auto world =
            command("Page.createIsolatedWorld",
                    {{"frameId", root.at("id")}, {"worldName", "devbox-observation"}}, deadline, cancel);
        const auto result = command("Runtime.evaluate",
                                    {{"expression", script},
                                     {"contextId", world.at("executionContextId")},
                                     {"returnByValue", true},
                                     {"timeout", 3000},
                                     {"awaitPromise", false}},
                                    deadline, cancel);
        if (result.contains("exceptionDetails") || !result.contains("result") ||
            !result["result"].contains("value"))
            throw Error("BROWSER_EVALUATION_FAILED: page observation is unavailable");
        return result["result"]["value"];
    }
    void open(const std::string& owner, Clock::time_point deadline, const Cancel& cancel) {
        if (session)
            throw Error("BROWSER_BUSY: close the existing session or wait for its idle expiry");
        if (stopping->cancelled())
            throw Error("BROWSER_STOPPED");
        auto executable = chromium_path(options);
        if (!executable)
            throw Error("BROWSER_UNAVAILABLE: install Chromium or configure DEVBOX_BROWSER_EXECUTABLE");
#ifndef _WIN32
        if (geteuid() == 0)
            throw Error("BROWSER_SANDBOX_REQUIRED: launch Devbox as a non-root user");
#endif
        const auto memory = available_memory();
        if (memory && *memory < 192 * 1024 * 1024)
            throw Error("BROWSER_RESOURCE_PRESSURE: less than 192 MiB host memory available");
        start_reaper();
        auto owned = std::make_unique<Session>();
        owned->owner = owner;
        owned->profile = fs::temp_directory_path() / ("devbox-browser-" + owned->id);
        if (!fs::create_directory(owned->profile))
            throw Error("BROWSER_PROFILE_CREATE_FAILED");
#ifndef _WIN32
        if (chmod(owned->profile.c_str(), 0700) != 0)
            throw Error("BROWSER_PROFILE_PERMISSIONS");
#endif
        owned->proxy = std::make_unique<BrowserProxy>(options.fixture_port);
        std::vector<std::string> args{options.headed ? "--ozone-platform=x11" : "--headless=new",
                                      "--remote-debugging-address=127.0.0.1",
                                      "--remote-debugging-port=0",
                                      "--user-data-dir=" + path_text(owned->profile),
                                      "--no-first-run",
                                      "--no-default-browser-check",
                                      "--disable-background-networking",
                                      "--disable-component-update",
                                      "--disable-sync",
                                      "--disable-extensions",
                                      "--disable-quic",
                                      "--disable-breakpad",
                                      "--disable-crash-reporter",
                                      "--disable-default-apps",
                                      "--force-webrtc-ip-handling-policy=disable_non_proxied_udp",
                                      "--proxy-server=http://127.0.0.1:" +
                                          std::to_string(owned->proxy->port()),
                                      "--proxy-bypass-list=<-loopback>",
                                      "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1",
                                      "--renderer-process-limit=2",
                                      "--js-flags=--max-old-space-size=128",
                                      "--window-size=1280,800",
                                      "--password-store=basic",
                                      "about:blank"};
        auto* pointer = owned.get();
        ProcessOptions process;
        process.env = worker_environment();
        if (options.headed) {
            if (!config->platform.is_linux || !environment("DISPLAY") || !environment("XAUTHORITY"))
                throw Error("BROWSER_HEADED_UNAVAILABLE: an authorized Linux X11 display is required");
            for (const auto key : {"DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"})
                if (const auto value = environment(key))
                    (*process.env)[key] = *value;
        }
        process.timeout = options.lifetime;
        process.termination_grace = Millis(1000);
        process.max_capture_chars = 4096;
        process.windows_detached_console = true;
        process.on_pid = [pointer](std::uint32_t pid) { pointer->pid = pid; };
        owned->runner = std::thread([pointer, executable = *executable, args = std::move(args), process] {
            try {
                spawn_process(path_text(executable), args, process, pointer->lifetime);
            } catch (const std::exception& error) {
                pointer->launch_error = std::string(error.what()).substr(0, 1024);
            }
            pointer->exited.store(true, std::memory_order_release);
        });
        session = std::move(owned);
        while (!fs::is_regular_file(session->profile / "DevToolsActivePort")) {
            cancel->check();
            if (session->exited.load(std::memory_order_acquire))
                throw Error("BROWSER_LAUNCH_FAILED: " + session->launch_error);
            if (Clock::now() >= deadline)
                throw Error("BROWSER_STARTUP_DEADLINE");
            cancel->wait_for(Millis(25));
        }
        const auto endpoint = split(read_file(session->profile / "DevToolsActivePort", 512), '\n');
        if (endpoint.size() < 2)
            throw Error("BROWSER_ENDPOINT_INVALID");
        unsigned long port = std::stoul(endpoint[0]);
        if (!port || port > 65535)
            throw Error("BROWSER_ENDPOINT_INVALID");
        session->protocol = std::make_unique<BrowserProtocol>();
        session->protocol->connect(static_cast<std::uint16_t>(port), trim(endpoint[1]), deadline, cancel);
        const auto version = command("Browser.getVersion", Json::object(), deadline, cancel, true);
        session->browser_version = {{"product", json_string(version, "product")},
                                    {"protocol_version", json_string(version, "protocolVersion")}};
        command("Browser.setDownloadBehavior", {{"behavior", "deny"}}, deadline, cancel, true);
        const auto targets = command("Target.getTargets", Json::object(), deadline, cancel, true);
        for (const auto& target : targets.at("targetInfos"))
            if (json_string(target, "type") == "page") {
                session->target = json_string(target, "targetId");
                break;
            }
        if (session->target.empty())
            throw Error("BROWSER_PAGE_MISSING");
        session->cdp_session =
            json_string(command("Target.attachToTarget", {{"targetId", session->target}, {"flatten", true}},
                                deadline, cancel, true),
                        "sessionId");
        command("Page.enable", Json::object(), deadline, cancel);
        session->main_frame = json_string(frame(deadline, cancel), "id");
        session->protocol->on_event(
            [owned = session.get()](const Json& event, Clock::time_point, const Cancel&) {
                if (json_string(event, "sessionId") != owned->cdp_session)
                    return;
                if (json_string(event, "method") == "Network.responseReceived" && event.contains("params")) {
                    const auto& p = event["params"];
                    if (json_string(p, "type") == "Document" &&
                        json_string(p, "frameId") == owned->main_frame && p.contains("response"))
                        owned->http_status = static_cast<int>(json_number(p["response"], "status"));
                }
            });
        command("Network.enable",
                {{"maxTotalBufferSize", 1024 * 1024}, {"maxResourceBufferSize", 256 * 1024}}, deadline,
                cancel);
        command("Network.setCacheDisabled", {{"cacheDisabled", true}}, deadline, cancel);
        command("Network.setBypassServiceWorker", {{"bypass", true}}, deadline, cancel);
        command("Emulation.setDeviceMetricsOverride",
                {{"width", 1280}, {"height", 800}, {"deviceScaleFactor", 1}, {"mobile", false}}, deadline,
                cancel);
    }
    BrowserObservation observe(const Json& args, Clock::time_point deadline, const Cancel& cancel) {
        auto before = frame(deadline, cancel);
        auto data = evaluate(observe_script, deadline, cancel);
        std::string image;
        if (json_bool(args, "screenshot", true))
            image =
                json_string(command("Page.captureScreenshot",
                                    {{"format", "jpeg"}, {"quality", 65}, {"captureBeyondViewport", false}},
                                    deadline, cancel),
                            "data");
        if (image.size() > 2 * 1024 * 1024)
            throw Error("BROWSER_IMAGE_LIMIT");
        const auto after = frame(deadline, cancel);
        const auto viewport = evaluate("({x:scrollX,y:scrollY,w:innerWidth,h:innerHeight,url:location.href})",
                                       deadline, cancel);
        const bool stable = before.value("loaderId", "") == after.value("loaderId", "") &&
                            data.value("url", "") == viewport.value("url", "") &&
                            data["viewport"]["scroll_x"] == viewport["x"] &&
                            data["viewport"]["scroll_y"] == viewport["y"];
        data["session_id"] = session->id;
        data["observation_id"] = session->observation = uuid();
        data["observed_at"] = utc_now();
        data["navigation_stable"] = stable;
        data["atomic_snapshot"] = false;
        data["observation_note"] = "Sequential DOM and image capture; live animations may change between "
                                   "them. Refresh before acting on changed content.";
        data["network"] = session->proxy->metrics();
        data["browser"] = session->browser_version;
        data["http_status"] = session->http_status ? Json(session->http_status) : Json();
        data["untrusted_page_content"] = true;
        data["image_mime_type"] = image.empty() ? Json() : Json("image/jpeg");
        const auto page_status = browser_page_status(data, session->http_status);
        data["page_status"] = page_status;
        data["possible_challenge"] = page_status == "access_denied" || page_status == "challenge_required";
        if (json_bool(args, "include_html", false)) {
            auto capture = evaluate(html_script, deadline, cancel);
            session->html = json_string(capture, "html");
            if (session->html.size() > html_limit)
                throw Error("BROWSER_HTML_LIMIT");
            session->html_hash = sha256(session->html);
            data["html_artifact"] = {
                {"sha256", session->html_hash},
                {"bytes", session->html.size()},
                {"original_utf16_units", capture["original_utf16_units"]},
                {"truncated", capture["truncated"]},
                {"retrieval",
                 "read_html with session_id and html_sha256; artifact lasts until next capture or close"}};
            web::Transfer rendered;
            rendered.url = rendered.final_url = json_string(data, "url");
            rendered.status = 200;
            rendered.body = session->html;
            rendered.bytes = rendered.body.size();
            rendered.headers = {{"content-type", "text/html"}};
            rendered.completed_at = json_string(data, "observed_at");
            rendered.completed_unix_ms = unix_millis();
            auto document = web::extract_document(rendered);
            data["offers"] = web::offer_view(document, 0, 10);
            data["offers_origin"] =
                "rendered DOM extraction, not an HTTP status or a verified selected-variant price";
            const auto html_frame = frame(deadline, cancel);
            if (json_string(html_frame, "loaderId") != json_string(after, "loaderId")) {
                data["navigation_stable"] = false;
                data["page_status"] = "navigation_unstable";
                data["offers"] = Json::object();
            }
        }
        session->last = data;
        return {std::move(data), std::move(image)};
    }
};
BrowserService::BrowserService(std::shared_ptr<const Config> config, BrowserOptions options)
    : state_(std::make_unique<State>(std::move(config), std::move(options))) {}
BrowserService::~BrowserService() = default;
void BrowserService::stop() {
    state_->stop();
}
BrowserObservation BrowserService::perform(const Json& args, std::string_view principal,
                                           const Cancel& supplied) {
    const auto cancel = std::make_shared<Cancellation>(state_->stopping);
    auto subscription =
        supplied ? supplied->subscribe([cancel] { cancel->cancel(); }) : Cancellation::Subscription{};
    cancel->check();
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    const auto action = json_string(args, "action");
    if (action == "status")
        return {{{"available", chromium_path(state.options).has_value() &&
                                   state.config->runtime_mode == RuntimeMode::host &&
                                   state.config->host_exec_enabled},
                 {"active_session", static_cast<bool>(state.session)},
                 {"session_limit", 1},
                 {"headed", state.options.headed},
                 {"idle_timeout_ms", state.options.idle_timeout.count()},
                 {"hard_lifetime_ms", state.options.lifetime.count()},
                 {"available_host_memory_bytes", available_memory().value_or(0)}},
                {}};
    if (state.config->runtime_mode != RuntimeMode::host || !state.config->host_exec_enabled)
        throw Error("BROWSER_UNAVAILABLE: enabled host runtime is required");
    const auto deadline = Clock::now() + Millis(40000);
    if (action == "open") {
        if (state.session)
            throw Error("BROWSER_BUSY: close the existing session or wait for its idle expiry");
        const auto target = state.url(json_string(args, "url"));
        try {
            state.open(std::string(principal), deadline, cancel);
            auto nav = state.command("Page.navigate", {{"url", target}}, deadline, cancel);
            if (nav.contains("errorText"))
                throw Error("BROWSER_NAVIGATION: " + json_string(nav, "errorText"));
            state.settle(nav, deadline, cancel);
            return state.observe(args, deadline, cancel);
        } catch (...) {
            if (state.session && state.session->owner == principal)
                state.session.reset();
            throw;
        }
    }
    if (!state.session || state.session->owner != principal ||
        json_string(args, "session_id") != state.session->id)
        throw Error(
            "BROWSER_SESSION_NOT_FOUND: session expired, was closed, or belongs to another principal");
    auto& session = *state.session;
    session.touched = Clock::now();
    if (action == "close") {
        state.session.reset();
        return {{{"closed", true}}, {}};
    }
    if (session.exited) {
        state.session.reset();
        throw Error("BROWSER_EXITED");
    }
    if (action == "read_html") {
        if (session.html_hash.empty() || json_string(args, "html_sha256") != session.html_hash)
            throw Error("BROWSER_HTML_CHANGED: recapture HTML and use its current hash");
        const auto offset = json_uint(args, "offset"), limit = json_uint(args, "max_bytes", 16000);
        if (!limit || limit > 64000 || offset > session.html.size())
            throw Error("BROWSER_HTML_RANGE");
        if (offset < session.html.size() && (static_cast<unsigned char>(session.html[offset]) & 0xc0) == 0x80)
            throw Error("BROWSER_HTML_RANGE: offset must be a UTF-8 character boundary");
        auto count = std::min<std::size_t>(limit, session.html.size() - offset);
        while (count && offset + count < session.html.size() &&
               (static_cast<unsigned char>(session.html[offset + count]) & 0xc0) == 0x80)
            --count;
        if (!count && offset < session.html.size())
            throw Error("BROWSER_HTML_RANGE: max_bytes too small for the next character");
        return {{{"sha256", session.html_hash},
                 {"offset", offset},
                 {"bytes", count},
                 {"total_bytes", session.html.size()},
                 {"text", session.html.substr(offset, count)},
                 {"next_offset", offset + count}},
                {}};
    }
    if (action == "observe") {
        const auto wait = json_uint(args, "wait_ms");
        if (wait > 3000)
            throw Error("BROWSER_WAIT_BOUNDS");
        cancel->wait_for(Millis(wait));
        cancel->check();
        return state.observe(args, deadline, cancel);
    }
    if (action == "navigate") {
        const auto target = state.url(json_string(args, "url"));
        session.http_status = 0;
        auto nav = state.command("Page.navigate", {{"url", target}}, deadline, cancel);
        session.html.clear();
        session.html_hash.clear();
        session.observation.clear();
        if (nav.contains("errorText"))
            throw Error("BROWSER_NAVIGATION: " + json_string(nav, "errorText"));
        state.settle(nav, deadline, cancel);
        return state.observe(args, deadline, cancel);
    }
    if (action != "scroll" && action != "click" && action != "type")
        throw Error("BROWSER_INVALID_ACTION");
    if (json_string(args, "observation_id") != session.observation ||
        !json_bool(session.last, "navigation_stable"))
        throw Error("BROWSER_STALE_OBSERVATION: observe the page again before input");
    const auto current = state.evaluate(observe_script, deadline, cancel);
    const auto x = json_number(args, "x", 640), y = json_number(args, "y", 400);
    if (x < 0 || x >= 1280 || y < 0 || y >= 800)
        throw Error("BROWSER_INPUT_BOUNDS");
    bool stale = current["url"] != session.last["url"];
    for (const auto key : {"width", "height", "scroll_x", "scroll_y"})
        stale = stale || current["viewport"][key] != session.last["viewport"][key];
    // A rotating banner or growing document does not invalidate wheel delivery.
    // Target panes must still match; click/type retain the stricter control guard.
    auto scroll_target = [&](const Json& data) {
        Json found = nullptr;
        double area = 1e30;
        for (const auto& r : data["scroll_regions"]) {
            const double left = json_number(r, "x"), top = json_number(r, "y"), w = json_number(r, "width"),
                         h = json_number(r, "height");
            if (x >= left && x < left + w && y >= top && y < top + h && w * h < area) {
                found = r;
                area = w * h;
            }
        }
        return found;
    };
    stale = stale || scroll_target(current) != scroll_target(session.last);
    if (action != "scroll")
        stale = stale || current["controls"] != session.last["controls"];
    if (stale)
        throw Error("BROWSER_STALE_OBSERVATION: page or targets changed; observe again");
    session.observation.clear();
    if (action == "scroll") {
        const auto dy = json_number(args, "delta_y", 600);
        if (dy < -2000 || dy > 2000 || dy == 0.0)
            throw Error("BROWSER_SCROLL_BOUNDS");
        state.command("Input.dispatchMouseEvent",
                      {{"type", "mouseWheel"}, {"x", x}, {"y", y}, {"deltaX", 0}, {"deltaY", dy}}, deadline,
                      cancel);
    } else if (action == "click") {
        state.command("Input.dispatchMouseEvent",
                      {{"type", "mousePressed"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}},
                      deadline, cancel);
        state.command("Input.dispatchMouseEvent",
                      {{"type", "mouseReleased"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}},
                      deadline, cancel);
    } else {
        const auto text = json_string(args, "text");
        if (text.empty() || text.size() > 2000)
            throw Error("BROWSER_TEXT_BOUNDS");
        const auto focused =
            state.evaluate("({tag:document.activeElement?.tagName,type:document.activeElement?.type,editable:"
                           "document.activeElement?.isContentEditable})",
                           deadline, cancel);
        const auto type = json_string(focused, "type");
        if (type == "password" || type == "file" ||
            (focused.value("tag", "") != "INPUT" && focused.value("tag", "") != "TEXTAREA" &&
             !json_bool(focused, "editable")))
            throw Error("BROWSER_TEXT_TARGET: focus a non-sensitive editable control first");
        state.command("Input.insertText", {{"text", text}}, deadline, cancel);
    }
    cancel->wait_for(Millis(200));
    cancel->check();
    auto observed = state.observe(args, deadline, cancel);
    if (action == "scroll") {
        const auto old_pane = scroll_target(current), new_pane = scroll_target(observed.data);
        const bool moved = observed.data["viewport"]["scroll_y"] != current["viewport"]["scroll_y"] ||
                           (old_pane.is_object() && new_pane.is_object() &&
                            old_pane["scroll_top"] != new_pane["scroll_top"]);
        observed.data["scroll_result"] = {
            {"input_sent", true},
            {"movement_observed", moved},
            {"note",
             "A delivered wheel event may leave content unchanged at a boundary or over a different pane."}};
    }
    return observed;
}
} // namespace devbox
