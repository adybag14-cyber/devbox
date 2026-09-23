#pragma once
#include "state_store.hpp"
namespace devbox {
struct ArtifactLimits {
    std::uint64_t file_bytes = 1024ULL * 1024 * 1024;
    std::uint64_t reserved_bytes = 2ULL * 1024 * 1024 * 1024;
    std::size_t chunk_bytes = 1024 * 1024, active_uploads = 16, chunks = 4096;
    // Native fault injection only; never exposed as tool input or environment configuration.
    std::function<void(std::string_view)> transition_hook;
};
class ArtifactUploads {
    std::shared_ptr<StateStore> store_;
    fs::path root_, workspace_;
    ArtifactLimits limits_;
    StateRecord read(std::string_view principal, std::string_view id) const;
    Json finish(StateRecord record, std::string_view status, const Json& receipt = {});

  public:
    ArtifactUploads(std::shared_ptr<StateStore> store, fs::path private_root, fs::path workspace,
                    ArtifactLimits limits = {});
    Json begin(std::string_view principal, std::string_view id, const fs::path& destination,
               std::uint64_t bytes, std::string_view whole_sha256, std::string_view expected_sha256);
    Json status(std::string_view principal, std::string_view id) const;
    Json chunk(std::string_view principal, std::string_view id, std::uint64_t offset, std::string_view bytes,
               std::string_view chunk_sha256);
    Json finalize(std::string_view principal, std::string_view id, const Cancel& cancel = {});
    Json cancel(std::string_view principal, std::string_view id);
};
} // namespace devbox
