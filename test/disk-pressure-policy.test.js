import test from "node:test";
import assert from "node:assert/strict";

import { diskPressureCleanupOperation, shouldRejectDiskPressure } from "../src/disk-pressure-policy.js";

test("critical pressure rejects heavy mutating work but keeps inspection and cleanup available", () => {
  assert.equal(shouldRejectDiskPressure({ diskPressure: "critical", resourceClass: "heavy", operation: "cargo build --release" }), true);
  assert.equal(shouldRejectDiskPressure({ diskPressure: "critical", resourceClass: "io-heavy", operation: "git clone https://example.invalid/repo" }), true);
  assert.equal(shouldRejectDiskPressure({ diskPressure: "critical", resourceClass: "io-heavy", operation: "find C:\\\\src -type f", readOnly: true }), false);
  assert.equal(shouldRejectDiskPressure({ diskPressure: "warning", resourceClass: "io-heavy", operation: "git clone https://example.invalid/repo" }), false);
  assert.equal(shouldRejectDiskPressure({ diskPressure: "critical", resourceClass: "light", operation: "git status" }), false);
});

test("critical pressure recognizes cleanup-only recovery operations", () => {
  for (const operation of ["Remove-Item C:\\\\temp -Recurse -Force", "del C:\\\\temp\\\\old.bin", "docker system prune -af", "cargo clean", "git clean -xfd"]) {
    assert.equal(diskPressureCleanupOperation(operation), true, operation);
    assert.equal(shouldRejectDiskPressure({ diskPressure: "critical", resourceClass: "io-heavy", operation }), false, operation);
  }
  for (const operation of [
    "cargo build --release; cargo clean",
    "git clone https://example.invalid/repo && rm -rf repo",
    "cargo clean; cargo build --release",
    "echo model-rm - cache",
  ]) {
    assert.equal(diskPressureCleanupOperation(operation), false, operation);
    assert.equal(shouldRejectDiskPressure({ diskPressure: "critical", resourceClass: "io-heavy", operation }), true, operation);
  }
});
