#include "build_identity.hpp"
#include "devbox/contract.hpp"
namespace devbox {
std::string build_version() {
    return build_identity::version;
}
Json build_snapshot() {
    static const Json value = [] {
        auto path = executable_path();
        std::string binary = "unavailable";
        try {
            binary = sha256_file(path);
        } catch (...) {
        }
        Json dirty = nullptr;
        if (std::string_view(build_identity::dirty) == "true")
            dirty = true;
        if (std::string_view(build_identity::dirty) == "false")
            dirty = false;
        const auto generation = environment("DEVBOX_DEPLOYMENT_GENERATION");
        return Json{{"gitSha", build_identity::sha},
                    {"gitRef", build_identity::ref},
                    {"sourceTree", build_identity::tree},
                    {"sourceDirty", dirty},
                    {"sourceFingerprint", build_identity::fingerprint},
                    {"buildUnixSeconds", build_identity::built},
                    {"compiler", build_identity::compiler},
                    {"implementation", "cpp"},
                    {"sanitizers",
#ifdef DEVBOX_SANITIZERS
                     true
#else
                     false
#endif
                    },
                    {"binarySha256", binary},
                    {"executableName", path_text(path.filename())},
                    {"deploymentGeneration", generation ? Json(*generation) : Json()}};
    }();
    return value;
}
} // namespace devbox
