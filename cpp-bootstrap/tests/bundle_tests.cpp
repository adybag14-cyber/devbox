#include "devbox/bundle.hpp"
#include "devbox/native.hpp"
#include "devbox/state_store.hpp"
#include <fstream>
#include <iostream>
using namespace devbox;
using namespace std::string_literals;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> void rejects(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "unsafe offline package was accepted");
}
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-bundle-test-" + uuid());
    try {
        ensure_private_state_directory(root);
        ScopeExit cleanup([&] {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        });
        ensure_private_state_directory(root / "exclusive", true);
        write_file(root / "exclusive" / "keep", "owned");
        rejects([&] { ensure_private_state_directory(root / "exclusive", true); });
        require(read_file(root / "exclusive" / "keep", 10) == "owned",
                "exclusive private creation preserves existing directories");
        const auto package = root / "package";
        ensure_private_state_directory(package);
        ensure_private_state_directory(package / "bin");
        const std::string bytes = std::string(150000, 'x') + "binary\0tail"s;
        write_file(package / "bin" / "devbox-mcp", bytes);
        const Json record{{"name", "devbox-mcp"},
                          {"file", "bin/devbox-mcp"},
                          {"bytes", bytes.size()},
                          {"sha256", sha256(bytes)}};
        const Json build{{"schema", 1},
                         {"implementation", "cpp"},
                         {"releaseReady", true},
                         {"gitSha", std::string(40, 'a')},
                         {"sourceTree", std::string(40, 'b')},
                         {"target", "linux-x86_64"},
                         {"setupVersion", "1.0.0"},
                         {"build", {{"sourceDirty", false}, {"sanitizers", false}}},
                         {"binaries", Json::array({record})}};
        write_json_atomic(package / "build-manifest.json", build);
        for (const auto* name :
             {"THIRD_PARTY_NOTICES.txt", "dependency-inventory.json", "sbom.spdx.json", "OFFLINE.md"})
            write_file(package / name, "{}\n");
        const auto payload = root / "payload";
        const auto manifest = bundle::pack(package, payload, "123");
        bundle::extract(payload, 0, manifest, root / "extracted");
        require(read_file(root / "extracted" / "devbox-mcp", 1024 * 1024) == bytes,
                "native streaming extraction preserves binary bytes");
        rejects([&] { bundle::extract(payload, 0, manifest, root / "extracted"); });
        require(read_file(root / "extracted" / "devbox-mcp", 1024 * 1024) == bytes,
                "existing installation stays unchanged");
        for (const auto* name : {"../escape", "x/../../escape", "x\\escape", "/absolute", "C:drive", "CON",
                                 "aux.txt", "COM1.exe", "NUL", "name.", ".hidden"})
            require(!bundle::safe_name(name), "cross-platform path/device rejection");
        for (const auto* field : {"bytes", "compressedBytes", "offset"}) {
            auto invalid = manifest;
            invalid["files"][0][field] = UINT64_MAX;
            rejects([&] { bundle::extract(payload, 0, invalid, root / "invalid"); });
            require(!fs::exists(root / "invalid"), "invalid metadata must not create files");
        }
        auto invalid = manifest;
        invalid["files"].push_back(invalid["files"][0]);
        rejects([&] { bundle::validate(invalid); });
        invalid = manifest;
        invalid["files"][0]["name"] = "..";
        rejects([&] { bundle::extract(payload, 0, invalid, root / "escape"); });
        const auto stub = root / "stub";
        write_file(stub, "native-stub");
        bundle::attach(stub, payload, root / "combined");
        const auto offset = bundle::payload_offset(root / "combined", json_uint(manifest, "payloadBytes"));
        require(offset == 11, "self-extractor locates the exact payload after its executable");
        bundle::extract(root / "combined", offset, manifest, root / "from-executable");
        require(read_file(root / "from-executable" / "devbox-mcp", 1024 * 1024) == bytes,
                "self-extracting format preserves bytes");
        fs::resize_file(root / "combined", fs::file_size(root / "combined") - 1);
        rejects([&] { bundle::payload_offset(root / "combined", json_uint(manifest, "payloadBytes")); });
        const auto original = read_file(payload, 1024 * 1024);
        auto changed = original;
        changed.at(changed.size() / 2) ^= 1;
        write_file(payload, changed);
        rejects([&] { bundle::extract(payload, 0, manifest, root / "tampered"); });
        require(!fs::exists(root / "tampered"), "tampered payload rejected before extraction");
        write_file(payload, original);
        invalid = manifest;
        invalid["files"][0]["sha256"] = std::string(64, '0');
        rejects([&] { bundle::extract(payload, 0, invalid, root / "wrong-file-hash"); });
        require(!fs::exists(root / "wrong-file-hash"), "failed extraction rolls back its own files");
        invalid = manifest;
        invalid["files"][0]["bytes"] = std::uint64_t{1};
        invalid["expandedBytes"] = json_uint(manifest, "expandedBytes") - bytes.size() + 1;
        rejects([&] { bundle::extract(payload, 0, invalid, root / "bomb"); });
        require(!fs::exists(root / "bomb"), "decompression bound rejects expansion beyond declared size");
        fs::resize_file(payload, fs::file_size(payload) - 1);
        rejects([&] { bundle::extract(payload, 0, manifest, root / "truncated"); });
        require(!fs::exists(root / "truncated"), "truncated input cannot publish a partial install");
        std::cout << "offline streaming, identities, bounds, paths and atomic rollback passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
