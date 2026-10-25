#pragma once
#include "native.hpp"
#include "process.hpp"
namespace devbox {
struct BundledExecutable {
    fs::path path;
    NativeHandle verification_handle;
};
Json bundled_ripgrep_identity();
// Only the compiled-in build identity is trusted; no model-supplied path,
// external manifest or environment variable can authorize a different binary.
std::optional<BundledExecutable> verified_bundled_ripgrep(const fs::path& directory,
                                                          const Cancel& cancel = {});
std::optional<fs::path> host_ripgrep(const Environment& environment);
} // namespace devbox
