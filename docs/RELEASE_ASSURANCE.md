# Release assurance

The release packaging job depends on the complete native, Android, Termux, Linux distribution, Alpine and security qualification graph. Packaging reuses the tested binaries; it does not rebuild them.

Each target carries a resolved vcpkg inventory, SPDX 2.3 SBOM and third-party notices. Installed versions, port revisions and ABI identities must agree with the resolved vcpkg SPDX metadata. Missing direct dependencies or missing runtime notices stop packaging. The `boost-uninstall` build-support package is explicitly identified as not shipping compiled code or headers; that exemption requires its installed file list to contain only build-support share files.

The package includes `qualification-receipt.json`, binding every tested binary digest to the source commit/tree, target, dependency assurance files and required workflow graph. GitHub Actions signs the exact release assets and receipt using the pinned `actions/attest` action. Its Sigstore verification bundle is retained. Fork PRs can exercise qualification but cannot produce promotion-eligible signed artifacts through this workflow.

`node cpp-mcp/scripts/verify-promotion.mjs BINARY TARGET RECEIPT BUNDLE SOURCE_SHA CONTRACT_VERSION` verifies GitHub's signature, repository, signer workflow, source digest and hosted-runner identity before executing the candidate. It then checks the tested/downloaded digest equality, immutable source identity, non-sanitizer build, complete reviewed contract and native extension schemas. Candidate identity/schema probes use an isolated fixture directory and a restricted environment. Signature or schema failure prevents promotion.

The vulnerability report uses keyless OSV OSS-Fuzz project/version queries for declared mappings, preserving findings, query details and unmapped packages. Empty responses are not a claim of comprehensive vulnerability freedom. Any returned vulnerability blocks this gate; remediating the dependency and rerunning is required. Independent scanning can use the complete SBOM/source/ABI inventory.

Native dependency cache keys include a CMake probe of the selected compiler, SDK, generator/platform/toolset, target triplet, standard/runtime, instrumentation/configuration, relevant flags and dependency manifest/baseline. vcpkg's per-package ABI checks remain authoritative inside the restored cache. Android and Alpine additionally retain their platform-specific baseline/toolchain configuration; their cache receipts require separate hosted verification.

Security jobs are separate from production artifacts: Windows MSVC ASan plus static analysis; Linux ThreadSanitizer for concurrent core/coordinator/scheduler paths; and Clang ASan/UBSan libFuzzer coverage of bounded HTML, JSON, product-offer and Retry-After parsing. These instrumented binaries cannot pass the production-artifact verifier. New lanes must pass hosted qualification before this audit is marked complete.
