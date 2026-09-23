#include "devbox/state_store.hpp"
#include <iostream>
using namespace devbox;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F&& work, std::string_view part) {
    try {
        work();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(part) != std::string_view::npos)
            return;
        throw;
    }
    throw Error("Expected snapshot rejection: " + std::string(part));
}
} // namespace
int main() {
    const auto root = fs::temp_directory_path() / path_from_utf8("devbox-state-snapshot-" + uuid());
    fs::create_directory(root);
    ScopeExit cleanup([&] {
        std::error_code error;
        fs::remove_all(root, error);
    });
    try {
        auto store = open_state_store(root / "source");
        const std::string canary = "SYNTHETIC-CREDENTIAL-NEVER-IN-DEFAULT-EXPORT";
        StateMutation admitted{
            {"job_operation", "operation-1", "owner", "task-1", "admitted", 0, Json{{"request", canary}}}, 0};
        StateEvent event{"run-1", "admitted", 0, Json{{"payload", canary}}};
        require(!store->apply_once("critical-transaction-1", {&admitted, 1}, {&event, 1}),
                "original admitted transaction");
        const auto report = export_state_summary(*store);
        require(report.dump().find(canary) == std::string::npos &&
                    report.dump().find("operation-1") == std::string::npos &&
                    report["contains_private_payloads"] == false,
                "default diagnostic export has no payload, identifiers, or canary");
        const auto snapshot = root / "snapshot";
        const auto manifest = store->private_snapshot(snapshot);
        require(manifest["contains_private_payloads"] == true &&
                    manifest["external_artifacts_included"] == false &&
                    manifest["database_sha256"] == sha256_file(snapshot / "metadata.sqlite3"),
                "explicit private snapshot integrity and scope");
        require(read_file(snapshot / "metadata.sqlite3", 1024 * 1024).find(canary) != std::string::npos,
                "private snapshot preserves explicitly included data");
        rejects([&] { store->private_snapshot(snapshot); }, "REQUIRES_NEW_ABSOLUTE_DIRECTORY");
        rejects([&] { (void)open_state_store(snapshot); }, "RECOVERY_RECONCILIATION_REQUIRED");
        StateMutation later{
            {"job_operation", "operation-after-snapshot", "owner", "task-1", "completed", 0, Json::object()},
            0};
        store->apply({&later, 1});
        const auto restored = root / "restored";
        const auto restored_manifest = restore_state_snapshot(snapshot, restored);
        require(restored_manifest["recovery_fenced"] == true,
                "restored snapshot is always fenced against automatic replay");
        StateStoreOptions readonly;
        readonly.writable = false;
        auto view = open_state_store(restored, readonly);
        require(view->get("job_operation", "operation-1")->data["request"] == canary &&
                    view->events("run-1", 0, 10).size() == 1 &&
                    !view->get("job_operation", "operation-after-snapshot"),
                "metadata and ordered events restore at the snapshot boundary");
        rejects([&] { (void)open_state_store(restored); }, "RECOVERY_RECONCILIATION_REQUIRED");
        rejects([&] { view->apply_once("critical-transaction-1", {&admitted, 1}); }, "WRITER_FENCED");
        rejects([&] { restore_state_snapshot(snapshot, root / "source"); },
                "REQUIRES_NEW_ABSOLUTE_DIRECTORY");
        require(store->get("job_operation", "operation-after-snapshot").has_value(),
                "restore cannot overwrite later admitted effects");
        view.reset();
        auto broken = read_json(snapshot / "manifest.json");
        broken["database_sha256"] = std::string(64, '0');
        write_json_atomic(snapshot / "manifest.json", broken);
        rejects([&] { restore_state_snapshot(snapshot, root / "tampered"); }, "SNAPSHOT_INTEGRITY");
        std::cout << "Payload-safe export, private snapshot, receipt/event preservation and fenced recovery "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
