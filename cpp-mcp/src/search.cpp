#include "devbox/search.hpp"
#include <algorithm>
#include <re2/re2.h>
#include <set>
namespace devbox {
namespace {
struct Pattern {
    std::unique_ptr<re2::RE2> expression;
    bool literal = false;
    Pattern(const std::string& pattern, bool case_sensitive) {
        re2::RE2::Options options;
        options.set_case_sensitive(case_sensitive);
        options.set_log_errors(false);
        options.set_max_mem(16 * 1024 * 1024);
        expression = std::make_unique<re2::RE2>(pattern, options);
        if (!expression->ok()) {
            literal = true;
            options.set_literal(true);
            expression = std::make_unique<re2::RE2>(pattern, options);
        }
        if (!expression->ok())
            throw Error("Search pattern exceeds the native regex memory bound.");
    }
    bool matches(std::string_view text) const {
        return re2::RE2::PartialMatch(text, *expression);
    }
};
bool skippable(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::permission_denied;
}
std::string glob_pattern(std::string glob) {
    if (trim(glob).empty())
        glob = "*";
    std::string result = "^";
    for (const auto c : glob) {
        if (c == '*')
            result += ".*";
        else if (c == '?')
            result += '.';
        else {
            if (std::string_view(".+()^$|{}[]\\").find(c) != std::string_view::npos)
                result += '\\';
            result += c;
        }
    }
    return result + "$";
}
ProcessOutput fallback(const Config& config, const SearchRequest& request, const Pattern& pattern,
                       const Cancel& cancel) {
    auto root = path_from_utf8(request.path);
    if (!root.is_absolute())
        root = config.devbox_workspace_path / root;
    std::error_code ec;
    const auto root_is_file = fs::is_regular_file(root, ec);
    Pattern glob(glob_pattern(request.glob), true);
    std::set<std::string> excluded;
    for (const auto& name : request.exclude_directories)
        if (!trim(name).empty())
            excluded.insert(lower(trim(name)));
    std::vector<std::pair<fs::path, std::size_t>> stack{{root, 0}};
    std::size_t matches = 0, skipped = 0, skipped_large = 0, pruned = 0;
    bool timed_out = false;
    CaptureAccumulator output(std::clamp<std::size_t>(config.max_mcp_transfer_chars, 1, 65536));
    const auto started = Clock::now();
    while (!stack.empty()) {
        if (cancel && cancel->cancelled())
            throw Cancelled();
        if (Clock::now() - started >= request.timeout) {
            timed_out = true;
            break;
        }
        auto [path, depth] = std::move(stack.back());
        stack.pop_back();
        const auto metadata = fs::symlink_status(path, ec);
        if (ec) {
            if (skippable(ec)) {
                ++skipped;
                continue;
            }
            throw Error("inspect " + path_text(path) + ": " + ec.message());
        }
        if (fs::is_directory(metadata)) {
            if (depth >= request.max_depth)
                continue;
            fs::directory_iterator iterator(path, ec);
            if (ec) {
                if (skippable(ec)) {
                    ++skipped;
                    continue;
                }
                throw Error("list " + path_text(path) + ": " + ec.message());
            }
            std::vector<fs::path> children;
            for (; iterator != fs::directory_iterator(); iterator.increment(ec)) {
                if (ec)
                    throw Error("list " + path_text(path) + ": " + ec.message());
                if (cancel)
                    cancel->check();
                if (Clock::now() - started >= request.timeout) {
                    timed_out = true;
                    break;
                }
                if (children.size() >= 65536)
                    throw Error("Native search directory exceeds the 65536-entry memory bound.");
                children.push_back(iterator->path());
            }
            if (timed_out)
                break;
            std::sort(children.begin(), children.end(),
                      [](const auto& a, const auto& b) { return path_text(a) < path_text(b); });
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (excluded.contains(lower(path_text(it->filename()))))
                    ++pruned;
                else {
                    if (stack.size() >= 65536)
                        throw Error("Native search traversal exceeds the 65536-path memory bound.");
                    stack.emplace_back(*it, depth + 1);
                }
            }
        } else if (fs::is_regular_file(metadata)) {
            const auto relative = root_is_file ? path.filename() : path.lexically_relative(root);
            if (!glob.matches(replace_all(path_text(relative.empty() ? path : relative), "\\", "/")))
                continue;
            const auto size = fs::file_size(path, ec);
            if (ec) {
                ++skipped;
                continue;
            }
            if (size > request.max_file_bytes) {
                ++skipped_large;
                continue;
            }
            std::string bytes;
            try {
                bytes = read_file(path, static_cast<std::size_t>(request.max_file_bytes));
            } catch (const Error&) {
                std::error_code status_ec;
                const auto exists = fs::exists(path, status_ec);
                if (!exists || status_ec == std::errc::permission_denied) {
                    ++skipped;
                    continue;
                }
                throw;
            }
            if (std::string_view(bytes).substr(0, 8192).find('\0') != std::string_view::npos)
                continue;
            const auto text = sanitize_utf8(bytes);
            std::size_t offset = 0, line_number = 0;
            while (offset < text.size()) {
                if (cancel)
                    cancel->check();
                if (Clock::now() - started >= request.timeout) {
                    timed_out = true;
                    break;
                }
                const auto end = text.find('\n', offset);
                auto line = std::string_view(text).substr(offset, end == text.npos ? text.size() - offset
                                                                                   : end - offset);
                if (end != text.npos && line.ends_with('\r'))
                    line.remove_suffix(1);
                ++line_number;
                if (pattern.matches(line)) {
                    output.push(path_text(path) + ":" + std::to_string(line_number) + ":" +
                                std::string(line) + "\n");
                    if (++matches >= request.max_matches)
                        break;
                }
                if (end == text.npos)
                    break;
                offset = end + 1;
            }
            if (timed_out || matches >= request.max_matches)
                break;
        }
    }
    std::vector<std::string> notices;
    if (timed_out)
        notices.push_back("search stopped after " + std::to_string(request.timeout.count()) + " ms");
    if (matches >= request.max_matches)
        notices.push_back("match limit " + std::to_string(request.max_matches) + " reached");
    if (pruned)
        notices.push_back("pruned " + std::to_string(pruned) + " excluded directories");
    if (skipped)
        notices.push_back("skipped " + std::to_string(skipped) + " inaccessible or vanished paths");
    if (skipped_large)
        notices.push_back("skipped " + std::to_string(skipped_large) + " oversized files");
    output.finish();
    const auto captured = output.snapshot();
    ProcessOutput result;
    result.stdout_text = captured.text;
    result.stdout_original_chars = captured.original_chars;
    result.stdout_capture_truncated = captured.truncated;
    if (!notices.empty())
        result.stderr_text = join(notices, "; ") + "\n";
    return result;
}
struct RgParser {
    std::string pending;
    std::size_t matches = 0, files = 0, max_matches;
    bool reached = false, oversized = false;
    CaptureAccumulator output;
    RgParser(std::size_t maximum, std::size_t capture) : max_matches(maximum), output(capture) {}
    void line(std::string_view line) {
        Json event;
        try {
            event = Json::parse(line);
        } catch (...) {
            return;
        }
        const auto type = json_string(event, "type");
        if (type == "begin")
            ++files;
        else if (type == "match" && !reached && event.contains("data") && event["data"].is_object()) {
            const auto& data = event["data"];
            auto text = json_string(data.value("lines", Json::object()), "text");
            while (text.ends_with('\n') || text.ends_with('\r'))
                text.pop_back();
            output.push(json_string(data.value("path", Json::object()), "text") + ":" +
                        std::to_string(json_uint(data, "line_number")) + ":" + text + "\n");
            reached = ++matches >= max_matches;
        }
    }
    void push(std::string_view bytes) {
        if (reached || oversized)
            return;
        if (pending.size() + bytes.size() > 16 * 1024 * 1024) {
            oversized = true;
            return;
        }
        pending.append(bytes);
        std::size_t consumed = 0;
        while (!reached) {
            const auto end = pending.find('\n', consumed);
            if (end == pending.npos)
                break;
            line(std::string_view(pending).substr(consumed, end - consumed));
            consumed = end + 1;
        }
        pending.erase(0, consumed);
    }
    void finish() {
        if (!reached && !oversized && !pending.empty())
            line(pending);
        pending.clear();
        output.finish();
    }
};
std::vector<std::string> rg_arguments(const SearchRequest& request, bool fixed) {
    std::vector<std::string> args{"--json",         "--color",
                                  "never",          "--no-messages",
                                  "--max-depth",    std::to_string(request.max_depth),
                                  "--max-filesize", std::to_string(request.max_file_bytes)};
    if (!request.case_sensitive)
        args.emplace_back("-i");
    if (fixed)
        args.emplace_back("-F");
    if (request.include_ignored)
        args.insert(args.end(), {"--hidden", "--no-ignore"});
    if (!trim(request.glob).empty())
        args.insert(args.end(), {"--glob", request.glob});
    for (const auto& name : request.exclude_directories)
        if (!trim(name).empty())
            args.insert(args.end(), {"--glob", "!**/" + trim(name) + "/**"});
    args.insert(args.end(), {"--", request.pattern, request.path});
    return args;
}
bool missing_program(const std::exception& error) {
    auto text = std::string(error.what());
    if (const auto* process = dynamic_cast<const ProcessError*>(&error))
        text += process->stderr_text;
    text = lower(text);
    return text.find("not found") != text.npos || text.find("cannot find") != text.npos ||
           text.find("no such file") != text.npos;
}
} // namespace
ProcessOutput SearchService::search(SearchRequest request, const Cancel& cancel) const {
    request.max_matches = std::clamp<std::size_t>(request.max_matches, 1, 5000);
    request.max_depth = std::clamp<std::size_t>(request.max_depth, 1, 50);
    request.max_file_bytes = std::clamp<std::uint64_t>(request.max_file_bytes, 1, 67108864);
    request.timeout = std::max(Millis(1), request.timeout);
    const Pattern pattern(request.pattern, request.case_sensitive);
    if (config_->runtime_mode == RuntimeMode::host &&
        (config_->host_search_backend == "js" || config_->host_search_backend == "rust" ||
         config_->host_search_backend == "cpp" || config_->host_search_backend == "native"))
        return fallback(*config_, request, pattern, cancel);
    auto args = rg_arguments(request, pattern.literal);
    std::string program = "rg";
    ProcessOptions options;
    options.timeout = request.timeout;
    const auto cap = std::clamp<std::size_t>(config_->max_mcp_transfer_chars, 1, 65536);
    options.max_capture_chars = cap;
    if (config_->runtime_mode == RuntimeMode::host)
        options.cwd = config_->devbox_workspace_path;
    else {
        std::vector<std::string> command{"exec"};
        if (!trim(config_->devbox_default_user).empty())
            command.insert(command.end(), {"-u", config_->devbox_default_user});
        command.insert(command.end(), {"-w", path_text(config_->devbox_workspace_path),
                                       config_->devbox_container_name, "rg"});
        command.insert(command.end(), args.begin(), args.end());
        args = std::move(command);
        program = "docker";
    }
    const auto child_cancel = std::make_shared<Cancellation>(cancel);
    RgParser parser(request.max_matches, cap);
    options.on_output = [&](OutputStream stream, std::string_view bytes) {
        if (stream == OutputStream::stdout_stream) {
            parser.push(bytes);
            if (parser.reached || parser.oversized)
                child_cancel->cancel();
        }
    };
    ProcessOutput result;
    try {
        result = spawn_process(program, args, options, child_cancel);
    } catch (const ProcessError& e) {
        if (cancel && cancel->cancelled())
            throw Cancelled();
        if (parser.oversized)
            throw Error("Ripgrep result line exceeds the 16 MiB parsing memory bound.");
        if ((parser.reached && e.aborted) || (e.exit_code == 1 && !e.timed_out && !e.aborted))
            result.stderr_text = e.stderr_text;
        else if (!e.exit_code && missing_program(e) && config_->runtime_mode == RuntimeMode::host)
            return fallback(*config_, request, pattern, cancel);
        else
            throw;
    } catch (const Error& e) {
        if (cancel && cancel->cancelled())
            throw Cancelled();
        if (missing_program(e) && config_->runtime_mode == RuntimeMode::host)
            return fallback(*config_, request, pattern, cancel);
        throw;
    }
    if (cancel && cancel->cancelled())
        throw Cancelled();
    if (parser.oversized)
        throw Error("Ripgrep result line exceeds the 16 MiB parsing memory bound.");
    parser.finish();
    std::vector<std::string> notices{"search backend ripgrep"};
    if (pattern.literal)
        notices.emplace_back("invalid regex treated as literal text");
    if (parser.reached)
        notices.push_back("match limit " + std::to_string(request.max_matches) + " reached");
    if (!request.exclude_directories.empty())
        notices.push_back("excluded " + std::to_string(request.exclude_directories.size()) +
                          " directory names");
    notices.push_back("candidate files " + std::to_string(parser.files));
    if (!trim(result.stderr_text).empty())
        notices.push_back(trim(result.stderr_text));
    const auto captured = parser.output.snapshot();
    result.stdout_text = captured.text;
    result.stdout_original_chars = captured.original_chars;
    result.stdout_capture_truncated = captured.truncated;
    result.stderr_text = join(notices, "; ") + "\n";
    result.exit_code = 0;
    return result;
}
} // namespace devbox
