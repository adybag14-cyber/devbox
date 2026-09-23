import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import os from "node:os";
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { runCheckedProcess, resolveMcpImplementation } from "../src/mcp-implementation.js";
import { getCppMcpBinaryPath, readCppSourceIdentity, prepareCppImplementation, promoteCppImplementation } from "../src/cpp-implementation.js";

const fixture = async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-cpp-preflight-"));
  const git = async (...args) => runCheckedProcess("git", args, { cwd: root, label: "C++ fixture Git" });
  await git("init", "--quiet");
  await git("config", "user.name", "Devbox C++ fixture");
  await git("config", "user.email", "fixture@example.invalid");
  await writeFile(path.join(root, ".gitignore"), "run/\nbin/native/\n.cpp-build/\n");
  const registry = { contract_version: 19, tools: Array.from({ length: 52 }, (_, i) => ({ name: `fixture_${i}` })) };
  await mkdir(path.join(root, "cpp-mcp", "contract"), { recursive: true });
  await writeFile(path.join(root, "cpp-mcp", "contract", "tool-registry.json"), JSON.stringify(registry));
  await git("add", ".gitignore", "cpp-mcp/contract/tool-registry.json");
  await git("commit", "--quiet", "-m", "Fixture source");
  const source = await readCppSourceIdentity(root, { runProcess: runCheckedProcess });
  const binary = getCppMcpBinaryPath(root);
  await mkdir(path.dirname(binary), { recursive: true });
  await writeFile(binary, "native candidate fixture");
  if (process.platform !== "win32") await chmod(binary, 0o755);
  const hash = createHash("sha256").update(await readFile(binary)).digest("hex");
  const info = { implementation: "cpp", sanitizers: false, sourceDirty: false, gitSha: source.GitSha, sourceTree: source.SourceTree, sourceFingerprint: "a".repeat(64), binarySha256: hash };
  const report = { implementation: "cpp", contract_version: registry.contract_version, complete: true, cutover_allowed: true, implemented_tools: registry.tools.length, target_tools: registry.tools.length };
  const calls = [];
  const runner = async (file, args, options) => {
    calls.push({ file, args });
    if (args[0] === "--build-info") return { stdout: JSON.stringify(info), exitCode: 0 };
    if (args[0] === "--parity-report") return { stdout: JSON.stringify(report), exitCode: 0 };
    return runCheckedProcess(file, args, options);
  };
  return { root, git, source, binary, hash, info, report, calls, runner, close: () => rm(root, { recursive: true, force: true, maxRetries: 20, retryDelay: 50 }) };
};

test("C++ preflight stages a verified immutable executable and promotes only after startup", async () => {
  const f = await fixture();
  try {
    assert.equal(resolveMcpImplementation({ DEVBOX_MCP_IMPLEMENTATION: " Cpp " }), "cpp");
    const spec = await prepareCppImplementation(f.root, { runProcess: f.runner, build: async () => { throw new Error("Unexpected rebuild"); } });
    assert.equal(spec.implementation, "cpp");
    assert.match(path.basename(spec.file), /^devbox-cpp-mcp-[a-f0-9]{12}-[a-f0-9]{16}(?:\.exe)?$/u);
    assert.equal(await readFile(spec.file, "utf8"), "native candidate fixture");
    assert.equal(spec.env.DEVBOX_PROJECT_ROOT, f.root);
    assert.equal(spec.env.DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE, "1");
    await assert.rejects(readFile(spec.candidate.CandidateManifestPath), { code: "ENOENT" });
    await promoteCppImplementation(spec, 12345);
    const current = JSON.parse(await readFile(spec.candidate.CandidateManifestPath, "utf8"));
    assert.equal(current.Sha256, f.hash);
    assert.equal(current.ProcessId, 12345);
    const second = await prepareCppImplementation(f.root, { runProcess: f.runner, build: async () => { throw new Error("Unexpected rebuild"); } });
    assert.equal(second.file, spec.file);
    assert.equal(second.candidate.Reused, true);
    assert.equal(second.candidate.Generation, spec.candidate.Generation);
    assert.equal(second.env.DEVBOX_DEPLOYMENT_GENERATION, spec.env.DEVBOX_DEPLOYMENT_GENERATION);
    await promoteCppImplementation(second, 12346);
    const restarted = JSON.parse(await readFile(spec.candidate.CandidateManifestPath, "utf8"));
    assert.equal(restarted.FirstPromotedAtUtc, current.FirstPromotedAtUtc);
    assert.equal(restarted.ProcessId, 12346);
    const explicit = await prepareCppImplementation(f.root, { env: { ...process.env, CPP_MCP_EXE: f.binary }, runProcess: f.runner });
    assert.equal(explicit.candidate.Generation, current.Generation);
    assert.equal(explicit.candidate.Reused, true);
    assert(!f.calls.some((call) => /cargo|rustc/u.test(call.file)));
  } finally { await f.close(); }
});

test("uncertified C++ replacement preserves the existing promotion manifest", async () => {
  const f = await fixture();
  try {
    const manifest = path.join(f.root, "run", "bin", "current-cpp.json");
    await mkdir(path.dirname(manifest), { recursive: true });
    const previous = '{"priorCandidate":"unchanged"}\n';
    await writeFile(manifest, previous);
    f.report.complete = false;
    f.report.cutover_allowed = false;
    await assert.rejects(prepareCppImplementation(f.root, { env: { ...process.env, CPP_MCP_EXE: f.binary }, runProcess: f.runner }), /incomplete or uncertified/u);
    assert.equal(await readFile(manifest, "utf8"), previous);
  } finally { await f.close(); }
});

test("C++ preflight derives version and inventory from the committed canonical registry", async () => {
  const f = await fixture();
  try {
    const env = { ...process.env, CPP_MCP_EXE: f.binary };
    f.report.contract_version -= 1;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /incomplete or uncertified/u);
    f.report.contract_version += 1;
    f.report.target_tools -= 1;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /incomplete or uncertified/u);
    f.report.target_tools += 1;
    f.report.implemented_tools -= 1;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /incomplete or uncertified/u);
  } finally { await f.close(); }
});

test("C++ preflight rejects dirty source, mismatched hashes, and a source change during validation", async () => {
  const f = await fixture();
  try {
    const env = { ...process.env, CPP_MCP_EXE: f.binary };
    await writeFile(path.join(f.root, "untracked.cpp"), "// not committed\n");
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /clean source checkout/u);
    await rm(path.join(f.root, "untracked.cpp"));
    f.info.binarySha256 = "f".repeat(64);
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /provenance did not match/u);
    f.info.binarySha256 = f.hash;
    f.info.sourceDirty = true;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /provenance did not match/u);
    f.info.sourceDirty = false;
    f.info.sanitizers = true;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /Instrumented C\+\+ test builds/u);
    delete f.info.sanitizers;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /provenance did not match/u);
    f.info.sanitizers = false;
    const runner = async (...args) => {
      const result = await f.runner(...args);
      if (args[1][0] === "--parity-report") {
        await writeFile(path.join(f.root, "new-source.txt"), "changed");
        await f.git("add", "new-source.txt");
        await f.git("commit", "--quiet", "-m", "Concurrent source change");
      }
      return result;
    };
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: runner }), /Source changed during C\+\+ preflight/u);
  } finally { await f.close(); }
});

test("C++ promotion refuses an executable modified after preflight", async () => {
  const f = await fixture();
  try {
    const spec = await prepareCppImplementation(f.root, { runProcess: f.runner });
    await writeFile(spec.file, "tampered staged executable");
    await assert.rejects(promoteCppImplementation(spec, 1), /changed before promotion/u);
    await assert.rejects(readFile(spec.candidate.CandidateManifestPath), { code: "ENOENT" });
  } finally { await f.close(); }
});

test("signed production preflight rejects before candidate execution or fallback build", async () => {
  const f = await fixture();
  try {
    let rebuilt = false, probed = false;
    const env = { ...process.env, CPP_REQUIRE_QUALIFIED_ARTIFACT: "1" };
    await assert.rejects(prepareCppImplementation(f.root, { env: { ...env, CPP_REQUIRE_QUALIFIED_ARTIFACT: "treu" }, runProcess: f.runner }), /ambiguous promotion policy/u);
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /QUALIFICATION_RECEIPT/u);
    env.CPP_QUALIFICATION_RECEIPT = "run/qualification-receipt.json";
    env.CPP_PROVENANCE_BUNDLE = "run/provenance.sigstore.json";
    const runner = async (file, args, options) => {
      if (args[0]?.endsWith("verify-promotion.mjs")) throw new Error("signature rejected");
      if (args[0] === "--build-info") probed = true;
      return f.runner(file, args, options);
    };
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: runner,
      build: async () => { rebuilt = true; return f.binary; } }), /signature rejected/u);
    assert.equal(probed, false); assert.equal(rebuilt, false);
    await assert.rejects(readFile(path.join(f.root, "run/bin/current-cpp.json")), { code: "ENOENT" });
  } finally { await f.close(); }
});

test("signed production promotion retains the binding qualification receipt", async () => {
  const f = await fixture();
  try {
    const env = { ...process.env, CPP_REQUIRE_QUALIFIED_ARTIFACT: "true", CPP_MCP_EXE: f.binary,
      CPP_QUALIFICATION_RECEIPT: "run/qualification-receipt.json", CPP_PROVENANCE_BUNDLE: "run/provenance.sigstore.json" };
    const qualification = { verified: true, sourceSha: f.source.GitSha, sourceTree: f.source.SourceTree,
      contractVersion: f.report.contract_version, binarySha256: f.hash, workflowRunId: "controlled-fixture" };
    const runner = async (file, args, options) => args[0]?.endsWith("verify-promotion.mjs")
      ? { stdout: JSON.stringify(qualification), exitCode: 0 } : f.runner(file, args, options);
    const spec = await prepareCppImplementation(f.root, { env, runProcess: runner });
    assert.equal(spec.candidate.QualificationRequired, true);
    assert.deepEqual(spec.candidate.ReleaseQualification, qualification);
    await promoteCppImplementation(spec, 123);
    assert.deepEqual(JSON.parse(await readFile(spec.candidate.CandidateManifestPath, "utf8")).ReleaseQualification, qualification);
  } finally { await f.close(); }
});

test("Windows managed startup persists qualified and development promotion records", { skip: process.platform !== "win32" }, async () => {
  // Execute only the real launcher's manifest block with a harmless JSON sink.
  // Never dot-source the startup script: that would control the live service.
  const source = await readFile(new URL("../scripts/Start-ChatGptDevboxMcp.ps1", import.meta.url), "utf8");
  const begin = source.indexOf("    $promotionManifest = @{");
  const end = source.indexOf("    # Keep the current candidate", begin);
  assert(begin >= 0 && end > begin, "Managed promotion block is present");
  const qualification = { verified: true, sourceSha: "a".repeat(40), sourceTree: "b".repeat(40),
    binarySha256: "c".repeat(64), contractVersion: 9, stateSchemaVersion: 2,
    stateCoordinatorProtocol: 1, workflowRunId: "fixture-only" };
  for (const qualified of [true, false]) {
    const candidate = { GitSha: qualification.sourceSha, SourceTree: qualification.sourceTree,
      SourceDirty: false, Sha256: qualification.binarySha256, Generation: "fixture-generation",
      FilePath: "C:\\fixture-only\\candidate.exe",
      ...(qualified ? { QualificationRequired: true, ReleaseQualification: qualification } : {}) };
    const script = `$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$launchSpec = '${JSON.stringify(candidate)}' | ConvertFrom-Json
$manifestPath='fixture-only.json'
$startedAtUtc=$promotedAtUtc=$firstPromotedAtUtc='2026-09-23T00:00:00Z'
function Write-JsonStateFile { param($Path,$Value) $Value | ConvertTo-Json -Depth 8 -Compress }
${source.slice(begin, end)}`;
    const result = await runCheckedProcess("powershell.exe", ["-NoProfile", "-NonInteractive", "-EncodedCommand",
      Buffer.from(script, "utf16le").toString("base64")], { label: "Isolated managed promotion record" });
    const manifest = JSON.parse(result.stdout);
    assert.equal(manifest.Sha256, candidate.Sha256);
    assert.equal(manifest.SourceDirty, false);
    assert.equal(manifest.FirstPromotedAtUtc, "2026-09-23T00:00:00Z");
    if (qualified) {
      assert.equal(manifest.QualificationRequired, true);
      assert.deepEqual(manifest.ReleaseQualification, qualification);
    } else {
      assert.equal(Object.hasOwn(manifest, "QualificationRequired"), false);
      assert.equal(Object.hasOwn(manifest, "ReleaseQualification"), false);
    }
  }
});
