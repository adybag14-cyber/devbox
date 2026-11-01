#include "devbox/allocator.hpp"
#include "devbox/contract.hpp"
#include "devbox/research.hpp"
#include <iostream>
using namespace devbox;
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw Error("Supply a new absolute private fixture directory");
        const auto root = path_from_utf8(argv[1]);
        if (!root.is_absolute() || fs::exists(root) || !fs::is_directory(root.parent_path()))
            throw Error("Fixture must be new with an existing parent");
        ensure_directory(root);
        auto config = std::make_shared<Config>();
        config->project_root = root;
        config->jobs_root = root / "jobs";
        config->runtime_mode = RuntimeMode::host;
        JobStore jobs(config);
        const std::string id = "job-evidence-bench", timestamp = "2026-11-01T18:00:00.000Z";
        const auto paths = jobs.create_job(id, Json{{"id", id}, {"mode", "research"}},
                                           Json{{"id", id},
                                                {"mode", "research"},
                                                {"status", "succeeded"},
                                                {"createdAtUtc", timestamp},
                                                {"startedAtUtc", timestamp},
                                                {"completedAtUtc", timestamp},
                                                {"exitCode", 0}});
        const auto evidence_root = paths.dir / "research";
        ensure_directory(evidence_root / "documents");
        Json ledger{{"version", 2},
                    {"topic", "measurement"},
                    {"mode", "standard"},
                    {"target_sources", 100},
                    {"usable_sources", 100},
                    {"target_met", true},
                    {"queries", {"measurement"}},
                    {"providers", Json::array()},
                    {"sources", Json::array()},
                    {"excluded", Json::array()},
                    {"candidates", Json::array()}};
        std::string text;
        while (text.size() < 64000)
            text += "Measurement evidence with uncertainty and independent instrument calibration. ";
        for (unsigned n = 1; n <= 100; ++n) {
            const auto source = "s" + std::to_string(n);
            const auto url = "https://fixture.example/report/" + std::to_string(n);
            ledger["sources"].push_back(Json{{"source_id", source},
                                             {"url", url},
                                             {"status", "ok"},
                                             {"extraction_version", 2},
                                             {"excerpt", text.substr(0, 500)}});
            ledger["excluded"].push_back(
                Json{{"reason", "near_duplicate"}, {"url", url + "/copy"}, {"error", std::string(400, 'e')}});
            if (n <= 10)
                write_json_atomic(evidence_root / "documents" / (source + ".json"),
                                  Json{{"source_id", source},
                                       {"url", url},
                                       {"final_url", url},
                                       {"title", "Measurement report"},
                                       {"status", "ok"},
                                       {"retrieved_at", timestamp},
                                       {"validated_at", timestamp},
                                       {"text", text},
                                       {"extraction_version", 2},
                                       {"tables", Json::array()},
                                       {"structured_metadata", Json::array()},
                                       {"offers", Json::array()}});
        }
        for (unsigned n = 0; n < 256; ++n)
            ledger["candidates"].push_back(
                Json{{"url", "https://fixture.example/candidate/" + std::to_string(n)},
                     {"title", std::string(300, 'c')},
                     {"status", "retained"}});
        write_json_atomic(evidence_root / "ledger.json", ledger);
        std::cout << Json{{"build", build_snapshot()},
                          {"scope",
                           "Real research evidence API, private 100-source ledger and stored documents"}}
                         .dump()
                  << '\n';
        for (const auto* mode : {"sources", "detail", "reexcerpt", "candidates", "exclusions"}) {
            Json args{{"job_id", id}, {"max_chars", 64000}, {"limit", 10}};
            if (std::string_view(mode) == "detail")
                args["source_id"] = "s1";
            else if (std::string_view(mode) == "reexcerpt")
                args["excerpt_chars"] = 500;
            else
                args["section"] = mode;
            const auto expected = web::research_evidence(jobs, args).dump();
            for (unsigned trial = 0; trial < 5; ++trial) {
                std::uint64_t calls = 0, allocated = 0;
                double milliseconds = 0;
                for (unsigned iteration = 0; iteration < 20; ++iteration) {
                    const auto before = allocator_counters();
                    const auto began = Clock::now();
                    auto result = web::research_evidence(jobs, args);
                    milliseconds += std::chrono::duration<double, std::milli>(Clock::now() - began).count();
                    const auto after = allocator_counters();
                    calls += after.calls - before.calls;
                    allocated += after.allocated - before.allocated;
                    if (result.dump() != expected)
                        throw Error("Evidence output changed during measurement");
                }
                std::cout << Json{{"mode", mode},
                                  {"trial", trial},
                                  {"iterations", 20},
                                  {"milliseconds", milliseconds},
                                  {"allocation_calls", calls},
                                  {"allocated_bytes", allocated},
                                  {"output_sha256", sha256(expected)}}
                                 .dump()
                          << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
