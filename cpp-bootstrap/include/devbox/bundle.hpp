#pragma once
#include "devbox/common.hpp"

namespace devbox::bundle {
inline constexpr std::uint64_t maximum_payload = 1024ULL * 1024 * 1024;
inline constexpr std::uint64_t maximum_file = 256ULL * 1024 * 1024;
// Only flat release files are supported. No links, paths or executable hooks
// are accepted from archive metadata.
bool safe_name(std::string_view name);
Json pack(const fs::path& package, const fs::path& payload, std::string workflow);
void validate(const Json& manifest);
void extract(const fs::path& payload, std::uint64_t offset, const Json& manifest,
             const fs::path& new_directory);
void attach(const fs::path& executable, const fs::path& payload, const fs::path& output);
std::uint64_t payload_offset(const fs::path& executable, std::uint64_t expected_size);
std::string region_hash(const fs::path& file, std::uint64_t offset, std::uint64_t size);
} // namespace devbox::bundle
