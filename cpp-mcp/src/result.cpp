#include "devbox/result.hpp"
#include <algorithm>
namespace devbox {
namespace {
Json render(Json envelope, std::optional<std::string> text = std::nullopt) {
    if (!text) {
        std::vector<std::string> parts{envelope["summary"].get<std::string>()};
        if (envelope.contains("data"))
            parts.push_back(envelope["data"].dump(2));
        for (const auto* stream : {"stdout", "stderr"})
            if (envelope.contains(stream) && !envelope[stream].get<std::string>().empty())
                parts.push_back(std::string(stream) + ":\n" + envelope[stream].get<std::string>());
        text = join(parts, "\n\n");
    }
    const auto failed = !envelope["ok"].get<bool>();
    Json part{{"type", "text"}};
    part["text"] = std::move(*text);
    Json result{{"content", Json::array()}};
    result["content"].push_back(std::move(part));
    result["structuredContent"] = std::move(envelope);
    result["isError"] = failed;
    return result;
}
Json envelope(std::string summary, bool success, std::optional<Json> data) {
    Json out = {{"ok", success}, {"summary", std::move(summary)}};
    if (data)
        out["data"] = std::move(*data);
    return out;
}
std::string marker(std::size_t omitted, std::string_view mode) {
    return "\n... " + std::string(mode) + " output omitted " + std::to_string(omitted) + " characters ...\n";
}
} // namespace
Json result_success(std::string summary, std::optional<Json> data, bool compact) {
    auto out = envelope(std::move(summary), true, std::move(data));
    out["exitCode"] = nullptr;
    out["truncated"] = false;
    std::optional<std::string> text;
    if (compact) {
        text = out["summary"].get<std::string>();
        if (out.contains("data"))
            *text += "\n\n" + out["data"].dump();
    }
    return render(std::move(out), std::move(text));
}
Json result_error(std::string summary, std::optional<Json> data) {
    auto out = envelope(std::move(summary), false, std::move(data));
    out["exitCode"] = nullptr;
    out["truncated"] = false;
    return render(std::move(out));
}
Json result_explicit(std::string summary, std::optional<Json> data, std::string text) {
    auto out = envelope(std::move(summary), true, std::move(data));
    out["exitCode"] = nullptr;
    out["truncated"] = false;
    return render(std::move(out), std::move(text));
}
Json result_process(std::string summary, std::optional<Json> data, std::string stdout_text,
                    std::string stderr_text, std::optional<int> code, bool success, bool truncated) {
    auto out = envelope(std::move(summary), success, std::move(data));
    if (!success || !stdout_text.empty())
        out["stdout"] = std::move(stdout_text);
    if (!success || !stderr_text.empty())
        out["stderr"] = std::move(stderr_text);
    out["exitCode"] = code ? Json(*code) : Json(nullptr);
    out["truncated"] = truncated;
    return render(std::move(out));
}
Json result_image(std::string summary, Json data, std::string base64, std::string mime) {
    auto out = result_success(std::move(summary), std::move(data));
    out["content"].push_back(
        Json{{"type", "image"}, {"data", std::move(base64)}, {"mimeType", std::move(mime)}});
    return out;
}
ShapedOutput shape_output(std::string_view text, std::string mode, std::size_t max_chars,
                          std::size_t max_lines) {
    mode = lower(trim(mode));
    if (mode != "head" && mode != "summary")
        mode = "tail";
    ShapedOutput out{std::string(text), false, js_length(text), std::nullopt, mode};
    if (max_lines) {
        std::string content(text);
        bool trailing = false;
        if (content.ends_with("\r\n")) {
            content.resize(content.size() - 2);
            trailing = true;
        } else if (content.ends_with('\n')) {
            content.pop_back();
            trailing = true;
        }
        auto lines = split(content, '\n');
        for (auto& line : lines)
            if (line.ends_with('\r'))
                line.pop_back();
        out.original_lines = lines.size();
        if (lines.size() > max_lines) {
            out.truncated = true;
            const auto suffix = trailing ? "\n" : "";
            if (mode == "head")
                out.text =
                    join({lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(max_lines)}, "\n") +
                    "\n... tail lines omitted ..." + suffix;
            else if (mode == "summary") {
                const auto head = max_lines / 2, tail = max_lines - head;
                out.text = join({lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(head)}, "\n") +
                           "\n... middle lines omitted ...\n" +
                           join({lines.end() - static_cast<std::ptrdiff_t>(tail), lines.end()}, "\n") +
                           suffix;
            } else
                out.text = "... head lines omitted ...\n" +
                           join({lines.end() - static_cast<std::ptrdiff_t>(max_lines), lines.end()}, "\n") +
                           suffix;
        }
    }
    const auto limit = std::max<std::size_t>(100, max_chars), length = js_length(out.text);
    if (length <= limit)
        return out;
    out.truncated = true;
    const auto note = marker(length - limit, mode == "head" ? "tail" : mode == "summary" ? "middle" : "head");
    const auto note_size = js_length(note), keep = limit > note_size ? limit - note_size : 0;
    if (mode == "head")
        out.text = js_slice(out.text, 0, keep) + note;
    else if (mode == "summary") {
        const auto head = keep / 2, tail = keep - head;
        out.text = js_slice(out.text, 0, head) + note + js_slice(out.text, length - tail, length);
    } else
        out.text = note + js_slice(out.text, length - keep, length);
    return out;
}
} // namespace devbox
