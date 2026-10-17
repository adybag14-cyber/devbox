#include "devbox/browser.hpp"
#include "devbox/browser_protocol.hpp"
#include "devbox/browser_proxy.hpp"
#include "devbox/process.hpp"
#include "devbox/research.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace devbox {
std::uint64_t browser_cgroup_headroom(std::uint64_t limit, std::uint64_t usage,
                                      std::uint64_t inactive_file_cache) {
    // Match Linux working-set accounting: inactive file cache can be reclaimed.
    // Inconsistent/non-atomic counters receive no cache credit; swap is never credited.
    const auto working = inactive_file_cache < usage ? usage - inactive_file_cache : usage;
    return limit > working ? limit - working : 0;
}
bool browser_process_arguments_match(std::string_view command, std::string_view executable,
                                     std::string_view profile, bool require_compatibility) {
    if (command.empty() || command.size() >= 16384)
        return false;
    auto args = split(std::string(command), '\0');
    while (!args.empty() && args.back().empty())
        args.pop_back();
    if (args.size() == 1 && args.front() != executable) {
        // Alpine Chromium replaces argv with a single, space-separated process
        // title. Only accept complete switch tokens for the fixed managed profile;
        // the caller separately verifies /proc/exe and the boot-bound PID identity.
        if (profile.find_first_of(" \t\r\n") != profile.npos ||
            args.front().find_first_of("\"'\t\r\n") != std::string::npos ||
            !args.front().starts_with(std::string(executable) + " "))
            return false;
        std::istringstream words(args.front());
        args.clear();
        std::string word;
        while (words >> word)
            args.push_back(std::move(word));
    }
    if (args.empty() || args.front() != executable)
        return false;
    const auto option = [](std::string_view arg) { return arg.substr(0, arg.find('=')); };
    for (const auto& arg : args)
        if (option(arg) == "--no-sandbox")
            return false;
    const auto unique = [&](std::string_view name, std::string_view value) {
        unsigned count = 0;
        bool matches = false;
        for (const auto& arg : args)
            if (option(arg) == name) {
                ++count;
                matches = arg == std::string(name) + "=" + std::string(value);
            }
        return count == 1 && matches;
    };
    return (!require_compatibility || unique("--disable-blink-features", "AutomationControlled")) &&
           unique("--user-data-dir", profile) && unique("--remote-debugging-address", "127.0.0.1") &&
           unique("--remote-debugging-port", "0");
}
std::string browser_normalize_url(std::string_view supplied) {
    auto result = web::normalize_url(supplied);
    // Fragment routes matter to a rendered application even though they are not
    // part of an HTTP fetch. Apply the public-network policy before preserving it.
    const auto fragment = supplied.find('#');
    if (fragment != supplied.npos)
        result.append(supplied.substr(fragment));
    if (result.size() > 4096)
        throw Error("WEB_INVALID_URL: normalized browser URL exceeds 4096 bytes");
    return result;
}
std::string browser_page_status(const Json& observation, int http_status) {
    const auto title = lower(json_string(observation, "title"));
    const auto text = lower(json_string(observation, "text").substr(0, 8000));
    const auto url = lower(json_string(observation, "url"));
    if (url.starts_with("chrome-error:") || url == "about:blank")
        return "unavailable";
    if (title.find("attention required") != title.npos || title.find("access denied") != title.npos ||
        text.find("sorry, you have been blocked") != text.npos ||
        text.find("you don't have permission to access") != text.npos)
        return "access_denied";
    if (title.find("just a moment") != title.npos || text.find("verify you are human") != text.npos ||
        text.find("checking your browser") != text.npos ||
        title.find("pardon our interruption") != title.npos)
        return "challenge_required";
    if (http_status == 403)
        return "access_denied";
    if (http_status == 404 || http_status == 410 ||
        (title.starts_with("something went wrong") &&
         text.find("we can't seem to find that page") != text.npos))
        return "not_found";
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
void connect_browser(BrowserProtocol& protocol, const fs::path& profile, Clock::time_point deadline,
                     const Cancel& cancel) {
    const auto endpoint = split(read_file(profile / "DevToolsActivePort", 512), '\n');
    const auto port_text = endpoint.empty() ? std::string() : trim(endpoint[0]);
    if (endpoint.size() < 2 || port_text.empty() || port_text.size() > 5 ||
        port_text.find_first_not_of("0123456789") != std::string::npos)
        throw Error("BROWSER_ENDPOINT_INVALID");
    const auto port = std::stoul(port_text);
    if (!port || port > 65535)
        throw Error("BROWSER_ENDPOINT_INVALID");
    protocol.connect(static_cast<std::uint16_t>(port), trim(endpoint[1]), deadline, cancel);
}
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
    for (const auto path : {"/opt/google/chrome/chrome", "/usr/lib/chromium/chromium",
                            "/usr/lib/chromium-browser/chromium-browser"})
        if (fs::is_regular_file(path))
            return fs::path(path);
#endif
#ifdef _WIN32
    // Rich developer/CI PATHs can require thousands of filesystem probes before
    // finding a browser. Prefer installed native binaries, then retain PATH as
    // the fallback for portable installations. Explicit overrides still win.
    for (const auto suffix :
         {"Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe"}) {
        for (const auto env : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
            const auto base = environment(env);
            if (!base)
                continue;
            auto path = path_from_utf8(*base) / suffix;
            if (fs::is_regular_file(path))
                return path;
        }
    }
#endif
    for (const auto name : {"google-chrome", "chromium", "chromium-browser", "chrome", "msedge"})
        if (auto found = find_program(name))
            return found;
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
            std::ifstream stats(fs::path(pair.first).parent_path() / "memory.stat");
            const std::string_view field = std::string_view(pair.first).ends_with("memory.max")
                                               ? "inactive_file"
                                               : "total_inactive_file";
            std::string key;
            std::uint64_t count = 0, inactive = 0;
            while (stats >> key >> count)
                if (key == field)
                    inactive = count;
            const auto left = browser_cgroup_headroom(limit, current, inactive);
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
        std::string context;
        bool shared = false;
        bool text_only = false;
        std::deque<std::pair<std::string, bool>> intercepted_resources;
        std::size_t omitted_resources = 0;
        std::optional<std::uint64_t> instance;
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
        std::function<void(std::string_view)> diagnostics;
        void diagnose(std::string_view message) const {
            if (diagnostics) {
                try {
                    diagnostics(message);
                } catch (...) {
                }
            }
        }
        ~Session() {
            // Let Chromium close its own helpers and profile before enforcing the
            // supervisor's process-group/Job Object deadline.
            if (protocol) {
                try {
                    if (shared) {
                        if (!context.empty())
                            protocol->call("Target.disposeBrowserContext", {{"browserContextId", context}},
                                           Clock::now() + Millis(2000));
                    } else
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
                // Windows can retain a Chromium profile handle briefly after the owned
                // Job Object closes. Retry only this exact profile, within a fixed bound.
                const auto cleanup_deadline = Clock::now() + Millis(2000);
                for (unsigned attempt = 0;; ++attempt) {
                    error.clear();
                    const auto removed = fs::remove_all(profile, error);
                    if (diagnostics)
                        diagnose("cleanup profile=" + profile.filename().string() +
                                 " attempt=" + std::to_string(attempt) +
                                 " removed=" + (error ? std::string("unknown") : std::to_string(removed)) +
                                 " error=" + std::to_string(error.value()));
                    if (!error || Clock::now() >= cleanup_deadline)
                        break;
                    std::this_thread::sleep_for(Millis(50));
                }
            }
        }
    };
    std::unique_ptr<Session> session;
    struct EndedSession {
        std::string id, owner, reason, ended_at;
    };
    // Keep one small diagnostic only; never retain a page, URL, image or HTML after expiry.
    std::optional<EndedSession> last_ended;
    void end_session(std::string reason) {
        if (session) {
            last_ended = EndedSession{session->id, session->owner, std::move(reason), utc_now()};
            session.reset();
        }
    }
    void expire_session() {
        if (!session)
            return;
        const auto now = Clock::now();
        if (now - session->created >= options.lifetime)
            end_session("hard_lifetime");
        else if (now - session->touched >= options.idle_timeout)
            end_session("idle_timeout");
        else if (session->exited)
            end_session("browser_exited");
        else if (session->shared && !process_matches_instance(session->pid.load(), session->instance))
            end_session("managed_browser_changed");
        else if (available_memory().value_or(UINT64_MAX) < 64 * 1024 * 1024)
            end_session("resource_pressure");
    }
    Json session_budget() const {
        if (!session)
            return Json();
        const auto now = Clock::now();
        const auto remaining = [now](Clock::time_point start, Millis allowance) {
            return std::max<std::int64_t>(
                0, std::chrono::duration_cast<Millis>(start + allowance - now).count());
        };
        return {{"idle_timeout_ms", options.idle_timeout.count()},
                {"hard_lifetime_ms", options.lifetime.count()},
                {"idle_remaining_ms", remaining(session->touched, options.idle_timeout)},
                {"hard_remaining_ms", remaining(session->created, options.lifetime)}};
    }
    Json ended_session(std::string_view principal) const {
        if (!last_ended || last_ended->owner != principal)
            return Json();
        return {{"session_id", last_ended->id},
                {"reason", last_ended->reason},
                {"ended_at", last_ended->ended_at}};
    }
    State(std::shared_ptr<const Config> c, BrowserOptions o) : config(std::move(c)), options(std::move(o)) {
        options.headed = env_bool("DEVBOX_BROWSER_HEADED", options.headed);
        options.text_only = env_bool("DEVBOX_BROWSER_TEXT_ONLY", options.text_only);
        options.compatibility = env_bool("DEVBOX_BROWSER_COMPATIBILITY", options.compatibility);
        if (env_bool("DEVBOX_BROWSER_SHARED", false)) {
            if (!config->platform.is_linux)
                throw Error("BROWSER_SHARED_UNSUPPORTED: managed shared mode requires Linux");
            options.headed = true;
            options.shared_profile = fs::path("/tmp/devbox-desktop/browser");
            options.shared_status_file = config->project_root / "run/desktop/status.json";
        }
        if (options.shared_profile.has_value() != options.shared_status_file.has_value())
            throw Error("BROWSER_SHARED_CONFIGURATION");
    }
    Json shared_owner() const {
        if (!options.shared_profile)
            return Json::object();
        if (fs::is_symlink(fs::symlink_status(*options.shared_profile)) ||
            fs::is_symlink(fs::symlink_status(*options.shared_profile / "DevToolsActivePort")))
            throw Error("BROWSER_SHARED_IDENTITY: symbolic profile or endpoint rejected");
        const auto status = read_json_optional(*options.shared_status_file, 16384);
        if (!status || !json_bool(*status, "ready") ||
            json_string(*status, "browser_mode") != "shared_native")
            throw Error("BROWSER_WARMING: managed Chromium is not ready; check status before retrying");
        if (json_string(*status, "browser_profile") != path_text(*options.shared_profile))
            throw Error("BROWSER_SHARED_IDENTITY: profile mismatch");
        for (const auto& child : status->at("children")) {
            if (json_string(child, "name") != "chromium" || !json_bool(child, "running"))
                continue;
            const auto pid = json_uint(child, "pid");
            if (!pid || pid > UINT32_MAX)
                break;
            const auto text = json_string(child, "startTicks");
            if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != text.npos)
                break;
            const auto instance = process_instance(static_cast<std::uint32_t>(pid));
            if (!instance)
                throw Error("BROWSER_SHARED_IDENTITY: managed process identity is unavailable");
#ifdef __linux__
            // The desktop supervisor publishes Linux start ticks. Native process
            // identities also bind the boot ID; they are not the raw tick count.
            const auto stat = read_file(fs::path("/proc") / std::to_string(pid) / "stat", 8192);
            const auto close = stat.rfind(')');
            if (close == std::string::npos)
                break;
            std::istringstream fields(stat.substr(close + 1));
            std::string ticks;
            bool valid_ticks = true;
            for (unsigned i = 0; i < 20; ++i)
                if (!(fields >> ticks)) {
                    valid_ticks = false;
                    break;
                }
            if (!valid_ticks || ticks != text)
                throw Error("BROWSER_SHARED_IDENTITY: supervisor start ticks do not match the process");
            std::ifstream stream(fs::path("/proc") / std::to_string(pid) / "cmdline", std::ios::binary);
            std::string command(16384, '\0');
            stream.read(command.data(), static_cast<std::streamsize>(command.size()));
            command.resize(static_cast<std::size_t>(stream.gcount()));
            const auto executable = chromium_path(options);
            const auto proc_exe = fs::path("/proc") / std::to_string(pid) / "exe";
            if (!executable || !fs::equivalent(proc_exe, *executable))
                throw Error("BROWSER_SHARED_IDENTITY: executable is not the configured Chromium binary");
            if (!browser_process_arguments_match(command, path_text(fs::read_symlink(proc_exe)),
                                                 path_text(*options.shared_profile), options.compatibility))
                throw Error("BROWSER_SHARED_IDENTITY: process arguments do not match the managed profile");
#else
            if (std::stoull(text) != *instance)
                break;
#endif
            if (!fs::is_regular_file(*options.shared_profile / "DevToolsActivePort") ||
                !process_matches_instance(static_cast<std::uint32_t>(pid), instance))
                break;
            return {{"pid", pid}, {"instance", std::to_string(*instance)}};
        }
        throw Error("BROWSER_SHARED_IDENTITY: managed browser process changed");
    }
    void check_shared_health(const Cancel& cancel) const {
        const auto owner = shared_owner();
        try {
            // A live PID and an old endpoint file survive some Chromium crashes.
            // Probe the browser process, without opening a page, changing inputs,
            // or attaching to any caller's context. One deadline bounds the whole probe.
            BrowserProtocol probe;
            const auto deadline = Clock::now() + Millis(2000);
            connect_browser(probe, *options.shared_profile, deadline, cancel);
            const auto version = probe.call("Browser.getVersion", Json::object(), deadline, cancel);
            if (json_string(version, "product").empty() || json_string(version, "protocolVersion").empty())
                throw Error("invalid browser version response");
        } catch (const Cancelled&) {
            throw;
        } catch (const std::exception& error) {
            throw Error("BROWSER_SHARED_UNRESPONSIVE: managed Chromium did not answer its health probe; "
                        "operator recovery is required (" +
                        std::string(error.what()).substr(0, 200) + ")");
        }
        if (shared_owner() != owner)
            throw Error("BROWSER_SHARED_IDENTITY: managed browser changed during health probe");
    }
    void start_reaper() {
        if (janitor.joinable())
            return;
        janitor = std::thread([this] {
            while (!stopping->wait_for(Millis(500))) {
                std::unique_lock lock(mutex, std::try_to_lock);
                if (lock)
                    expire_session();
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
        return browser_normalize_url(supplied);
    }
    Json command(std::string_view method, Json args, Clock::time_point deadline, const Cancel& cancel,
                 bool browser = false) {
        try {
            auto result = session->protocol->call(method, std::move(args), deadline, cancel,
                                                  browser ? "" : session->cdp_session);
            // Drain after receiving the requested reply. Calling back recursively
            // from each Fetch event would exhaust the protocol nesting budget on
            // an image-heavy page. Each drain shares the caller's deadline.
            unsigned processed = 0;
            while (!session->intercepted_resources.empty()) {
                if (++processed > 512)
                    throw Error("BROWSER_RESOURCE_EVENT_LIMIT");
                auto [request, omit] = std::move(session->intercepted_resources.front());
                session->intercepted_resources.pop_front();
                Json params{{"requestId", request}};
                if (omit)
                    params["errorReason"] = "BlockedByClient";
                try {
                    session->protocol->call(omit ? "Fetch.failRequest" : "Fetch.continueRequest",
                                            std::move(params), deadline, cancel, session->cdp_session);
                    if (omit)
                        ++session->omitted_resources;
                } catch (const Error& error) {
                    // A navigation can cancel an intercepted resource before the
                    // queued rejection is sent. Other protocol errors remain fatal.
                    if (std::string_view(error.what()).find("Invalid InterceptionId") ==
                        std::string_view::npos)
                        throw;
                }
            }
            return result;
        } catch (const Cancelled&) {
            throw;
        } catch (const std::exception& error) {
            // Identify the stalled protocol phase without disclosing page data or arguments.
            throw Error(std::string(error.what()) + " [CDP " + std::string(method) + "]");
        }
    }
    Json frame(Clock::time_point deadline, const Cancel& cancel) {
        return command("Page.getFrameTree", Json::object(), deadline, cancel).at("frameTree").at("frame");
    }
    void settle(const Json& navigation, std::string_view previous_loader, Clock::time_point deadline,
                const Cancel& cancel) {
        const auto loader = json_string(navigation, "loaderId");
        // Slow renderers can still produce a valid observation within the
        // existing 40-second action budget. Do not truncate them at 15 seconds.
        const auto ready_by = std::min(deadline, Clock::now() + Millis(30000));
        std::string candidate_loader, candidate_url;
        std::optional<Clock::time_point> readable_since;
        while (Clock::now() < ready_by) {
            cancel->check();
            try {
                const auto root = frame(ready_by, cancel);
                const auto current_loader = json_string(root, "loaderId");
                // Client redirects commit a different loader from Page.navigate's
                // reply. Accept that new document, but never the preceding page.
                const bool committed = loader.empty() || current_loader == loader ||
                                       (!current_loader.empty() && current_loader != previous_loader);
                if (committed && evaluate("document.readyState", ready_by, cancel) != "loading") {
                    const auto after = frame(ready_by, cancel);
                    const auto current_url = json_string(root, "url") + json_string(root, "urlFragment");
                    if (json_string(after, "loaderId") == current_loader &&
                        json_string(after, "url") + json_string(after, "urlFragment") == current_url) {
                        if (readable_since && candidate_loader == current_loader &&
                            candidate_url == current_url && Clock::now() - *readable_since >= Millis(600))
                            return;
                        candidate_loader = current_loader;
                        candidate_url = current_url;
                        readable_since = Clock::now();
                        cancel->wait_for(std::min(
                            Millis(600), std::chrono::duration_cast<Millis>(ready_by - Clock::now())));
                        continue;
                    }
                }
            } catch (const Error& error) {
                // A redirect can destroy the isolated observation world between
                // its creation and evaluation. Only that transient error retries.
                const std::string_view message(error.what());
                if (!message.starts_with("BROWSER_PROTOCOL_REMOTE:") ||
                    (message.find("Cannot find context with specified id") == message.npos &&
                     message.find("Execution context was destroyed") == message.npos))
                    throw;
            }
            readable_since.reset();
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
    void open(const std::string& owner, bool text_only, Clock::time_point deadline, const Cancel& cancel) {
        const auto began = Clock::now();
        const auto diagnose = [&](std::string_view phase) {
            if (options.launch_diagnostics) {
                const auto elapsed = std::chrono::duration_cast<Millis>(Clock::now() - began).count();
                try {
                    options.launch_diagnostics("phase=" + std::string(phase) + " elapsed_ms=" +
                                               std::to_string(elapsed));
                } catch (...) {
                }
            }
        };
        diagnose("resolve-browser");
        if (session)
            throw Error("BROWSER_BUSY: close the existing session or wait for its idle expiry");
        if (stopping->cancelled())
            throw Error("BROWSER_STOPPED");
        auto executable = chromium_path(options);
        if (!executable)
            throw Error("BROWSER_UNAVAILABLE: install Chromium or configure DEVBOX_BROWSER_EXECUTABLE");
        if (options.launch_diagnostics)
            diagnose("executable=" + path_text(*executable));
#ifndef _WIN32
        if (geteuid() == 0)
            throw Error("BROWSER_SANDBOX_REQUIRED: launch Devbox as a non-root user");
#endif
        const auto memory = available_memory();
        if (memory && *memory < 192 * 1024 * 1024)
            throw Error("BROWSER_RESOURCE_PRESSURE: less than 192 MiB host memory available");
        start_reaper();
        auto owned = std::make_unique<Session>();
        owned->diagnostics = options.launch_diagnostics;
        owned->owner = owner;
        owned->text_only = text_only;
        owned->shared = options.shared_profile.has_value();
        owned->proxy = std::make_unique<BrowserProxy>(options.fixture_port);
        diagnose("proxy-ready");
        fs::path endpoint_profile;
        if (owned->shared) {
            const auto daemon = shared_owner();
            owned->pid = static_cast<std::uint32_t>(json_uint(daemon, "pid"));
            owned->instance = std::stoull(json_string(daemon, "instance"));
            endpoint_profile = *options.shared_profile;
            session = std::move(owned);
        } else {
            owned->profile = fs::temp_directory_path() / ("devbox-browser-" + owned->id);
            if (!fs::create_directory(owned->profile))
                throw Error("BROWSER_PROFILE_CREATE_FAILED");
#ifndef _WIN32
            if (chmod(owned->profile.c_str(), 0700) != 0)
                throw Error("BROWSER_PROFILE_PERMISSIONS");
#endif
            std::vector<std::string> args{"--remote-debugging-address=127.0.0.1",
                                          "--remote-debugging-port=0",
                                          "--user-data-dir=" + path_text(owned->profile),
                                          "--no-first-run",
                                          "--no-default-browser-check",
                                          "--disable-background-networking",
                                          "--disable-background-timer-throttling",
                                          "--disable-backgrounding-occluded-windows",
                                          "--disable-renderer-backgrounding",
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
            if (!options.headed)
                args.insert(args.begin(), "--headless=new");
            else if (config->platform.is_linux)
                args.insert(args.begin(), "--ozone-platform=x11");
            if (options.compatibility)
                args.insert(args.begin(), "--disable-blink-features=AutomationControlled");
#ifdef _WIN32
            // Chrome otherwise may relaunch through the interactive shell with
            // a different token and exit this owned process before DevTools is
            // ready. Its explicit automation mode retains process/job ownership.
            // Renderer sandboxing and the existing network gate remain enabled.
            args.insert(args.end() - 1, "--enable-automation");
            // The controlled window is intentionally hidden in headless mode.
            // Native occlusion must not suspend the compositor needed for capture.
            args.insert(args.end() - 1, "--disable-features=CalculateNativeWinOcclusion");
#endif
            auto* pointer = owned.get();
            ProcessOptions process;
            process.env = worker_environment();
#ifndef _WIN32
            if (options.headed) {
                if (!config->platform.is_linux || !environment("DISPLAY") || !environment("XAUTHORITY"))
                    throw Error("BROWSER_HEADED_UNAVAILABLE: an authorized Linux X11 display is required");
                for (const auto key :
                     {"DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"})
                    if (const auto value = environment(key))
                        (*process.env)[key] = *value;
            }
#endif
            process.timeout = options.lifetime;
            process.termination_grace = Millis(1000);
            process.max_capture_chars = 4096;
            process.windows_detached_console = true;
            process.on_pid = [pointer, observer = options.pid_observer](std::uint32_t pid) {
                pointer->pid = pid;
                if (observer)
                    observer(pid);
            };
            owned->runner = std::thread([pointer, executable = *executable, args = std::move(args), process,
                                         diagnostics = options.launch_diagnostics] {
                const auto report = [&](std::string_view message) noexcept {
                    if (diagnostics && !message.empty()) {
                        try {
                            diagnostics(message);
                        } catch (...) {
                        }
                    }
                };
                try {
                    const auto output =
                        spawn_process(path_text(executable), args, process, pointer->lifetime);
                    pointer->launch_error = "Chromium exited before DevTools readiness (exit_code=" +
                                            std::to_string(output.exit_code) + ")";
                    report(output.stderr_text);
                } catch (const ProcessError& error) {
                    pointer->launch_error = std::string(error.what()).substr(0, 1024);
                    report(error.stderr_text);
                } catch (const std::exception& error) {
                    pointer->launch_error = std::string(error.what()).substr(0, 1024);
                }
                pointer->exited.store(true, std::memory_order_release);
            });
            session = std::move(owned);
            diagnose("process-dispatched");
            while (!fs::is_regular_file(session->profile / "DevToolsActivePort")) {
                cancel->check();
                if (session->exited.load(std::memory_order_acquire))
                    throw Error("BROWSER_LAUNCH_FAILED: " + session->launch_error);
                if (Clock::now() >= deadline)
                    throw Error("BROWSER_STARTUP_DEADLINE");
                cancel->wait_for(Millis(25));
            }
            endpoint_profile = session->profile;
            diagnose("endpoint-file-ready");
        }
        session->protocol = std::make_unique<BrowserProtocol>();
        // A stale endpoint must not consume the entire navigation allowance.
        const auto connection_deadline = std::min(deadline, Clock::now() + Millis(3000));
        try {
            connect_browser(*session->protocol, endpoint_profile, connection_deadline, cancel);
        } catch (const Cancelled&) {
            throw;
        } catch (const std::exception& error) {
            throw Error(std::string(error.what()) + " [CDP connect]");
        }
        diagnose("cdp-connected");
        const auto version = command("Browser.getVersion", Json::object(), connection_deadline, cancel, true);
        session->browser_version = {{"product", json_string(version, "product")},
                                    {"protocol_version", json_string(version, "protocolVersion")}};
        diagnose("version=" + json_string(version, "product"));
        if (session->shared) {
            session->context = json_string(
                command("Target.createBrowserContext",
                        {{"disposeOnDetach", true},
                         {"proxyServer", "http://127.0.0.1:" + std::to_string(session->proxy->port())},
                         {"proxyBypassList", "<-loopback>"}},
                        deadline, cancel, true),
                "browserContextId");
            if (session->context.empty())
                throw Error("BROWSER_CONTEXT_MISSING");
            command("Browser.setDownloadBehavior",
                    {{"behavior", "deny"}, {"browserContextId", session->context}}, deadline, cancel, true);
            Json target_args{{"url", "about:blank"}, {"browserContextId", session->context}};
            if (options.headed) {
                target_args["newWindow"] = true;
                target_args["width"] = 1280;
                target_args["height"] = 800;
            }
            session->target =
                json_string(command("Target.createTarget", target_args, deadline, cancel, true), "targetId");
        } else {
            command("Browser.setDownloadBehavior", {{"behavior", "deny"}}, deadline, cancel, true);
            const auto targets = command("Target.getTargets", Json::object(), deadline, cancel, true);
            for (const auto& target : targets.at("targetInfos"))
                if (json_string(target, "type") == "page") {
                    session->target = json_string(target, "targetId");
                    break;
                }
        }
        if (session->target.empty())
            throw Error("BROWSER_PAGE_MISSING");
        session->cdp_session =
            json_string(command("Target.attachToTarget", {{"targetId", session->target}, {"flatten", true}},
                                deadline, cancel, true),
                        "sessionId");
        command("Page.enable", Json::object(), deadline, cancel);
        diagnose("page-enabled");
        session->main_frame = json_string(frame(deadline, cancel), "id");
        session->protocol->on_event(
            [owned = session.get()](const Json& event, Clock::time_point, const Cancel&) {
                if (json_string(event, "sessionId") != owned->cdp_session)
                    return;
                if (owned->text_only && json_string(event, "method") == "Fetch.requestPaused" &&
                    event.contains("params")) {
                    const auto& params = event["params"];
                    auto request = json_string(params, "requestId");
                    if (request.empty() || request.size() > 256 || owned->intercepted_resources.size() >= 256)
                        throw Error("BROWSER_RESOURCE_EVENT_LIMIT");
                    const auto type = json_string(params, "resourceType");
                    owned->intercepted_resources.emplace_back(std::move(request),
                                                              type == "Image" || type == "Media");
                }
                if (json_string(event, "method") == "Network.responseReceived" && event.contains("params")) {
                    const auto& p = event["params"];
                    if (json_string(p, "type") == "Document" &&
                        json_string(p, "frameId") == owned->main_frame && p.contains("response"))
                        owned->http_status = static_cast<int>(json_number(p["response"], "status"));
                }
            });
        if (session->text_only)
            command("Fetch.enable",
                    {{"patterns",
                      Json::array(
                          {{{"urlPattern", "*"}, {"resourceType", "Image"}, {"requestStage", "Request"}},
                           {{"urlPattern", "*"}, {"resourceType", "Media"}, {"requestStage", "Request"}}})}},
                    deadline, cancel);
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
        // Activate only the controlled headless target, not a user's desktop
        // window, before reading its DOM and requesting the next captured frame.
        if (json_bool(args, "screenshot", true) && !options.headed)
            command("Page.bringToFront", Json::object(), deadline, cancel);
        auto before = frame(deadline, cancel);
        auto data = evaluate(observe_script, deadline, cancel);
        std::string image;
        if (json_bool(args, "screenshot", true)) {
            image =
                json_string(command("Page.captureScreenshot",
                                    {{"format", "jpeg"}, {"quality", 65}, {"captureBeyondViewport", false}},
                                    deadline, cancel),
                            "data");
        }
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
        data["browser"]["mode"] = session->shared ? "managed_process_isolated_context" : "owned_process";
        data["browser"]["compatibility_requested"] = options.compatibility;
        data["render_mode"] = session->text_only ? "text" : "full";
        data["omitted_network_resources"] = session->omitted_resources;
        if (session->text_only)
            data["render_note"] = "Network images and media are omitted to reduce memory and bandwidth. "
                                  "Layout, scripts, text and controls remain enabled; inline graphics may "
                                  "remain. This is a reduced visual rendering, not a complete page image.";
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
        // Slow navigation/rendering must not consume the caller's idle allowance.
        session->touched = Clock::now();
        data["session_budget"] = session_budget();
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
    // A request arriving between janitor ticks must not revive an expired session.
    state.expire_session();
    const auto action = json_string(args, "action");
    if (action == "status") {
        bool managed_ready = true;
        std::string reason;
        if (state.options.shared_profile) {
            try {
                state.check_shared_health(cancel);
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception& e) {
                managed_ready = false;
                reason = e.what();
            }
        }
        return {
            {{"available", managed_ready && chromium_path(state.options).has_value() &&
                               state.config->runtime_mode == RuntimeMode::host &&
                               state.config->host_exec_enabled},
             {"mode", state.options.shared_profile ? "managed_process_isolated_context" : "owned_process"},
             {"managed_ready", managed_ready},
             {"reason", reason},
             {"active_session", static_cast<bool>(state.session)},
             {"session_id",
              state.session && state.session->owner == principal ? Json(state.session->id) : Json()},
             {"session_limit", 1},
             {"headed", state.options.headed},
             {"compatibility_requested", state.options.compatibility},
             {"default_render_mode", state.options.text_only ? "text" : "full"},
             {"idle_timeout_ms", state.options.idle_timeout.count()},
             {"hard_lifetime_ms", state.options.lifetime.count()},
             {"session_budget",
              state.session && state.session->owner == principal ? state.session_budget() : Json()},
             {"last_session_end", state.ended_session(principal)},
             {"available_host_memory_bytes", available_memory().value_or(0)}},
            {}};
    }
    if (state.config->runtime_mode != RuntimeMode::host || !state.config->host_exec_enabled)
        throw Error("BROWSER_UNAVAILABLE: enabled host runtime is required");
    const auto deadline = Clock::now() + Millis(40000);
    if (action == "open") {
        if (state.session)
            throw Error("BROWSER_BUSY: close the existing session or wait for its idle expiry");
        const auto target = state.url(json_string(args, "url"));
        const auto render_mode = json_string(args, "render_mode", "auto");
        if (render_mode != "auto" && render_mode != "full" && render_mode != "text")
            throw Error("BROWSER_RENDER_MODE: expected auto, full or text");
        const bool text_only = render_mode == "text" || (render_mode == "auto" && state.options.text_only);
        try {
            state.open(std::string(principal), text_only, deadline, cancel);
            const auto previous_loader = json_string(state.frame(deadline, cancel), "loaderId");
            auto nav = state.command("Page.navigate", {{"url", target}}, deadline, cancel);
            if (nav.contains("errorText"))
                throw Error("BROWSER_NAVIGATION: " + json_string(nav, "errorText"));
            state.settle(nav, previous_loader, deadline, cancel);
            return state.observe(args, deadline, cancel);
        } catch (const Cancelled&) {
            if (state.session && state.session->owner == principal)
                state.end_session("open_cancelled");
            throw;
        } catch (...) {
            if (state.session && state.session->owner == principal)
                state.end_session("open_failed");
            throw;
        }
    }
    if (!state.session || state.session->owner != principal ||
        json_string(args, "session_id") != state.session->id) {
        if (state.last_ended && state.last_ended->owner == principal &&
            json_string(args, "session_id") == state.last_ended->id)
            throw Error("BROWSER_SESSION_ENDED: " + state.last_ended->reason +
                        "; open a new session and obtain a fresh observation");
        throw Error(
            "BROWSER_SESSION_NOT_FOUND: session expired, was closed, or belongs to another principal");
    }
    auto& session = *state.session;
    if (action == "close") {
        state.end_session("closed");
        return {{{"closed", true}}, {}};
    }
    if (session.exited ||
        (session.shared && !process_matches_instance(session.pid.load(), session.instance))) {
        state.end_session("browser_exited");
        throw Error(
            "BROWSER_SESSION_ENDED: browser_exited; open a new session and obtain a fresh observation");
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
        // Reading valid retained evidence is activity; failed reads and input validation are not.
        session.touched = Clock::now();
        return {{{"sha256", session.html_hash},
                 {"offset", offset},
                 {"bytes", count},
                 {"total_bytes", session.html.size()},
                 {"text", session.html.substr(offset, count)},
                 {"next_offset", offset + count},
                 {"session_budget", state.session_budget()}},
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
        const auto previous_loader = json_string(state.frame(deadline, cancel), "loaderId");
        auto nav = state.command("Page.navigate", {{"url", target}}, deadline, cancel);
        session.html.clear();
        session.html_hash.clear();
        session.observation.clear();
        if (nav.contains("errorText"))
            throw Error("BROWSER_NAVIGATION: " + json_string(nav, "errorText"));
        state.settle(nav, previous_loader, deadline, cancel);
        return state.observe(args, deadline, cancel);
    }
    if (action != "scroll" && action != "click" && action != "type")
        throw Error("BROWSER_INVALID_ACTION");
    if (session.observation.empty() || json_string(args, "observation_id") != session.observation ||
        !json_bool(session.last, "navigation_stable"))
        throw Error("BROWSER_STALE_OBSERVATION: observe the page again before input");
    if (action == "click" && (!args.contains("x") || !args.contains("y")))
        throw Error("BROWSER_INPUT_REQUIRED: click requires explicit x and y coordinates");
    if (action == "type") {
        const auto text = json_string(args, "text");
        if (text.empty() || text.size() > 2000)
            throw Error("BROWSER_TEXT_BOUNDS");
    }
    const auto dy = json_number(args, "delta_y", 600);
    if (action == "scroll" && (!std::isfinite(dy) || dy < -2000 || dy > 2000 || dy == 0.0))
        throw Error("BROWSER_SCROLL_BOUNDS");
    const auto current = state.evaluate(observe_script, deadline, cancel);
    const auto x = json_number(args, "x", 640), y = json_number(args, "y", 400);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x >= 1280 || y < 0 || y >= 800)
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
        // Wait for Chromium's bounded mouse-scroll gesture to finish. A raw
        // wheel dispatch acknowledgement does not establish compositor movement.
        state.command("Input.synthesizeScrollGesture",
                      {{"x", x},
                       {"y", y},
                       {"yDistance", -dy},
                       {"gestureSourceType", "mouse"},
                       {"preventFling", true},
                       {"speed", 8000}},
                      deadline, cancel);
    } else if (action == "click") {
        state.command("Input.dispatchMouseEvent",
                      {{"type", "mousePressed"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}},
                      deadline, cancel);
        state.command("Input.dispatchMouseEvent",
                      {{"type", "mouseReleased"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}},
                      deadline, cancel);
    } else {
        const auto text = json_string(args, "text");
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
    cancel->wait_for(Millis(action == "scroll" ? 50 : 200));
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
