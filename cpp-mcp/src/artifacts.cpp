#include "devbox/artifacts.hpp"
#include <algorithm>
#include <array>
namespace devbox {
namespace {
std::string digest(std::string_view value, bool missing = false) {
    auto result = lower(std::string(value));
    if (missing && result == "missing")
        return result;
    if (result.size() != 64 || !std::all_of(result.begin(), result.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw Error("UPLOAD_INVALID_SHA256");
    return result;
}
std::string identity(std::string_view principal, std::string_view id) {
    validate_key(principal);
    validate_key(id);
    return sha256(Json::array({principal, id}).dump());
}
bool within(const fs::path& child, const fs::path& parent) {
    auto part = child.begin();
    for (const auto& segment : parent) {
        if (part == child.end())
            return false;
#ifdef _WIN32
        if (lower(path_text(*part)) != lower(path_text(segment)))
#else
        if (*part != segment)
#endif
            return false;
        ++part;
    }
    return part != child.end();
}
std::string chunk_name(std::uint64_t index) {
    return "chunk-" + std::to_string(index);
}
Json view(const StateRecord& record, bool replayed = false) {
    const auto& value = record.data;
    Json result{{"upload_id", value.at("upload_id")},
                {"status", record.status},
                {"next_offset_bytes", value.at("next_offset_bytes")},
                {"total_bytes", value.at("total_bytes")},
                {"sha256", value.at("sha256")},
                {"chunks", value.at("chunks")},
                {"max_chunk_bytes", value.at("max_chunk_bytes")},
                {"replayed", replayed},
                {"reservation_held", json_bool(value, "reservation_held")}};
    if (value.contains("receipt"))
        result["receipt"] = value["receipt"];
    return result;
}
StateRecord quota_record(const std::shared_ptr<StateStore>& store) {
    return store->get("artifact_quota", "global")
        .value_or(StateRecord{"artifact_quota", "global", "system", "", "active", 0,
                              Json{{"active", 0}, {"bytes", 0}}});
}
} // namespace
ArtifactUploads::ArtifactUploads(std::shared_ptr<StateStore> store, fs::path root, fs::path workspace,
                                 ArtifactLimits limits)
    : store_(std::move(store)), root_(std::move(root)), workspace_(canonical_target(workspace)),
      limits_(std::move(limits)) {
    if (!store_)
        throw Error("UPLOAD_REQUIRES_INDEXED_STATE");
    if (!limits_.chunk_bytes || limits_.chunk_bytes > 1024 * 1024 || !limits_.chunks || limits_.chunks > 4096)
        throw Error("UPLOAD_INVALID_LIMITS");
    ensure_directory(root_.parent_path());
    ensure_private_state_directory(root_);
    root_ = fs::canonical(root_);
}
StateRecord ArtifactUploads::read(std::string_view principal, std::string_view id) const {
    const auto record = store_->get("upload", identity(principal, id));
    if (!record || record->principal != principal)
        throw Error("UPLOAD_NOT_FOUND");
    return *record;
}
Json ArtifactUploads::status(std::string_view principal, std::string_view id) const {
    return view(read(principal, id));
}
Json ArtifactUploads::begin(std::string_view principal, std::string_view id, const fs::path& destination,
                            std::uint64_t bytes, std::string_view whole_sha256,
                            std::string_view expected_sha256) {
    const auto key = identity(principal, id);
    const auto target = canonical_target(destination.is_absolute() ? destination : workspace_ / destination);
    if (!within(target, workspace_))
        throw Error("UPLOAD_DESTINATION_OUTSIDE_WORKSPACE");
    if (bytes > limits_.file_bytes)
        throw Error("UPLOAD_FILE_QUOTA");
    Json definition{{"path", path_text(target)},
                    {"total_bytes", bytes},
                    {"sha256", digest(whole_sha256)},
                    {"expected_sha256", digest(expected_sha256, true)}};
    const auto fingerprint = sha256(definition.dump());
    FileLock admission(root_ / ".admission.lock", Millis(5000), {}, true);
    if (const auto existing = store_->get("upload", key)) {
        if (json_string(existing->data, "fingerprint") != fingerprint)
            throw Error("UPLOAD_ID_CONFLICT");
        return view(*existing, true);
    }
    auto quota = quota_record(store_);
    const auto active = json_uint(quota.data, "active"), reserved = json_uint(quota.data, "bytes");
    if (active >= limits_.active_uploads || reserved > limits_.reserved_bytes ||
        bytes > limits_.reserved_bytes - reserved)
        throw Error("UPLOAD_AGGREGATE_QUOTA");
    definition["upload_id"] = id;
    definition["fingerprint"] = fingerprint;
    definition["next_offset_bytes"] = 0;
    definition["chunks"] = 0;
    definition["max_chunk_bytes"] = limits_.chunk_bytes;
    definition["reservation_held"] = true;
    StateRecord upload{"upload", key, std::string(principal), std::string(id), "receiving", 0, definition};
    quota.data["active"] = active + 1;
    quota.data["bytes"] = reserved + bytes;
    std::array<StateMutation, 2> mutations{{{upload, 0}, {quota, quota.revision}}};
    store_->apply(mutations);
    return view(upload);
}
Json ArtifactUploads::chunk(std::string_view principal, std::string_view id, std::uint64_t offset,
                            std::string_view bytes, std::string_view chunk_sha256) {
    const auto hash = digest(chunk_sha256);
    if (bytes.empty() || bytes.size() > limits_.chunk_bytes || sha256(bytes) != hash)
        throw Error("UPLOAD_CHUNK_SIZE_OR_HASH");
    const auto key = identity(principal, id);
    const auto directory = root_ / key;
    (void)read(principal, id);
    ensure_private_state_directory(directory);
    FileLock lock(directory / ".upload.lock", Millis(5000), {}, true);
    auto record = read(principal, id);
    const auto chunk_id = key + "-" + std::to_string(offset);
    if (const auto previous = store_->get("upload_chunk", chunk_id)) {
        if (json_string(previous->data, "sha256") != hash ||
            json_uint(previous->data, "bytes") != bytes.size())
            throw Error("UPLOAD_CHUNK_CONFLICT");
        return view(record, true);
    }
    if (record.status != "receiving")
        throw Error("UPLOAD_NOT_RECEIVING");
    const auto next = json_uint(record.data, "next_offset_bytes"),
               total = json_uint(record.data, "total_bytes"), count = json_uint(record.data, "chunks");
    if (offset != next || offset > total || bytes.size() > total - offset || count >= limits_.chunks)
        throw Error("UPLOAD_OFFSET_OR_CHUNK_LIMIT");
    // One bounded immutable file per chunk. Replaying after a metadata-commit failure never copies a prefix.
    atomic_write(directory / chunk_name(count), bytes, false, false,
                 Preconditions{std::string("missing"), {}});
    if (limits_.transition_hook)
        limits_.transition_hook("chunk_durable");
    StateRecord chunk{"upload_chunk",
                      chunk_id,
                      std::string(principal),
                      key,
                      "durable",
                      0,
                      Json{{"index", count}, {"offset", offset}, {"bytes", bytes.size()}, {"sha256", hash}}};
    record.data["next_offset_bytes"] = offset + bytes.size();
    record.data["chunks"] = count + 1;
    std::array<StateMutation, 2> mutations{{{record, record.revision}, {chunk, 0}}};
    store_->apply(mutations);
    return view(record);
}
Json ArtifactUploads::finish(StateRecord record, std::string_view status, const Json& receipt) {
    const bool replay = record.status == status;
    if (!replay) {
        record.status = status;
        if (!receipt.is_null())
            record.data["receipt"] = receipt;
        const StateMutation update{record, record.revision};
        store_->apply({&update, 1});
        ++record.revision;
    }
    if (!json_bool(record.data, "reservation_held"))
        return view(record, replay);
    // Keep the reservation until cleanup is confirmed. A restart can finish cleanup without republishing.
    const auto directory = root_ / record.id;
    for (std::uint64_t i = 0; i <= json_uint(record.data, "chunks"); ++i) {
        std::error_code error;
        fs::remove(directory / chunk_name(i), error);
        if (error)
            return view(record, replay);
    }
    const auto stage_directory =
        path_from_utf8(json_string(record.data, "path")).parent_path() / (".devbox-upload-" + record.id);
    if (fs::exists(stage_directory)) {
        ensure_private_state_directory(stage_directory);
        std::error_code error;
        fs::remove(stage_directory / "publish.tmp", error);
        if (error)
            return view(record, replay);
        fs::remove(stage_directory, error);
        if (error)
            return view(record, replay);
    }
    FileLock admission(root_ / ".admission.lock", Millis(5000), {}, true);
    auto quota = quota_record(store_);
    const auto active = json_uint(quota.data, "active"), reserved = json_uint(quota.data, "bytes"),
               total = json_uint(record.data, "total_bytes");
    if (!active || reserved < total)
        throw Error("UPLOAD_QUOTA_INTEGRITY");
    quota.data["active"] = active - 1;
    quota.data["bytes"] = reserved - total;
    record.data["reservation_held"] = false;
    std::array<StateMutation, 2> mutations{{{record, record.revision}, {quota, quota.revision}}};
    store_->apply(mutations);
    return view(record, replay);
}
Json ArtifactUploads::finalize(std::string_view principal, std::string_view id, const Cancel& cancel) {
    const auto directory = root_ / identity(principal, id);
    (void)read(principal, id);
    ensure_private_state_directory(directory);
    FileLock lock(directory / ".upload.lock", Millis(5000), cancel, true);
    auto record = read(principal, id);
    if (record.status == "completed")
        return finish(record, "completed");
    if (record.status != "receiving" && record.status != "finalizing")
        throw Error("UPLOAD_NOT_RECEIVING");
    const auto target = path_from_utf8(json_string(record.data, "path"));
    if (canonical_target(target) != target || !within(target, workspace_))
        throw Error("UPLOAD_DESTINATION_CHANGED");
    const auto bytes = json_uint(record.data, "total_bytes"), count = json_uint(record.data, "chunks");
    const auto hash = json_string(record.data, "sha256");
    const bool reconciling = record.status == "finalizing";
    if (json_uint(record.data, "next_offset_bytes") != bytes || count > limits_.chunks)
        throw Error("UPLOAD_INCOMPLETE");
    if (record.status == "finalizing") {
        const auto current = file_state(target);
        if (current.bytes == bytes && current.sha256 == hash)
            return finish(record, "completed",
                          Json{{"path", path_text(target)},
                               {"current", current.json()},
                               {"reconciled_by_destination_digest", true}});
    }
    std::vector<ArtifactChunk> chunks(static_cast<std::size_t>(count));
    std::vector<bool> seen(static_cast<std::size_t>(count), false);
    std::optional<std::string> after;
    std::uint64_t observed = 0, total = 0;
    do {
        if (cancel)
            cancel->check();
        const auto page =
            store_->list(StateQuery{"upload_chunk", std::string(principal), record.id, {}, after, 100});
        for (const auto& entry : page.records) {
            const auto index = json_uint(entry.data, "index"), size = json_uint(entry.data, "bytes");
            if (index >= count || seen[static_cast<std::size_t>(index)] || size > limits_.chunk_bytes ||
                size > bytes - total)
                throw Error("UPLOAD_CHUNK_INDEX_INTEGRITY");
            seen[static_cast<std::size_t>(index)] = true;
            chunks[static_cast<std::size_t>(index)] = {directory / chunk_name(index), size,
                                                       json_string(entry.data, "sha256")};
            total += size;
            ++observed;
        }
        after = page.next;
    } while (after);
    if (total != bytes || observed != count)
        throw Error("UPLOAD_CHUNK_INDEX_INTEGRITY");
    const auto stage_directory = target.parent_path() / (".devbox-upload-" + record.id);
    ensure_private_state_directory(stage_directory);
    if (!reconciling) {
        record.status = "finalizing";
        const StateMutation update{record, record.revision};
        store_->apply({&update, 1});
        ++record.revision;
    }
    bool publication_attempted = false;
    const auto transition = [&](std::string_view stage) {
        if (stage == "before_publish")
            publication_attempted = true;
        if (limits_.transition_hook)
            limits_.transition_hook(stage);
    };
    WriteReceipt receipt;
    try {
        receipt = publish_chunks(target, stage_directory / "publish.tmp", chunks, bytes, hash,
                                 json_string(record.data, "expected_sha256"), cancel, transition);
    } catch (...) {
        if (!publication_attempted && !reconciling) {
            record.status = "receiving";
            const StateMutation update{record, record.revision};
            store_->apply({&update, 1});
        }
        throw;
    }
    return finish(record, "completed", receipt.json());
}
Json ArtifactUploads::cancel(std::string_view principal, std::string_view id) {
    const auto directory = root_ / identity(principal, id);
    (void)read(principal, id);
    ensure_private_state_directory(directory);
    FileLock lock(directory / ".upload.lock", Millis(5000), {}, true);
    auto record = read(principal, id);
    if (record.status == "completed" || record.status == "finalizing")
        throw Error(
            "UPLOAD_FINALIZATION_REQUIRES_RECONCILIATION: inspect or retry finalize with the same ID");
    return finish(record, "cancelled");
}
} // namespace devbox
