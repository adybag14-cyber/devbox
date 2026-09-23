#pragma once
#include "common.hpp"
namespace devbox {
struct ShapedOutput {
    std::string text;
    bool truncated = false;
    std::size_t original_chars = 0;
    std::optional<std::size_t> original_lines;
    std::string mode = "tail";
};
ShapedOutput shape_output(std::string_view text, std::string mode = "tail", std::size_t max_chars = 65536,
                          std::size_t max_lines = 0);
Json result_success(std::string summary, std::optional<Json> data = std::nullopt, bool compact = false);
Json result_error(std::string summary, std::optional<Json> data = std::nullopt);
Json result_process(std::string summary, std::optional<Json> data, std::string out, std::string err,
                    std::optional<int> code, bool success, bool truncated = false);
Json result_explicit(std::string summary, std::optional<Json> data, std::string text);
Json result_image(std::string summary, Json data, std::string base64, std::string mime);
// Server-only lifecycle classification. Remove before exposing a result on any wire/model boundary.
enum class ToolOutcome { Cancelled, TimedOut, PolicyDenied, InvalidArguments, ProcessFailure };
Json with_outcome(Json result, ToolOutcome outcome);
Json with_child_timing(Json result, std::uint64_t elapsed_ms);
std::string result_outcome(const Json& result);
void strip_internal_result_metadata(Json& result);
} // namespace devbox
