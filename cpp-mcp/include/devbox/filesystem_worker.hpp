#pragma once
#include "storage.hpp"
namespace devbox {
struct FilesystemDeadline : Error {
    FilesystemDeadline()
        : Error("FILESYSTEM_WORKER_DEADLINE: owned worker termination requested; inspect destination version "
                "before retrying a write") {}
};
Json filesystem_operation(std::string_view operation, const Json& arguments);
using FilesystemDispatch = std::function<Json(std::string_view, const Json&)>;
int run_filesystem_worker(const FilesystemDispatch& dispatch = {});
Json isolated_filesystem(std::string_view operation, const Json& arguments, Millis timeout,
                         const Cancel& cancel = {}, const fs::path& worker = {});
} // namespace devbox
