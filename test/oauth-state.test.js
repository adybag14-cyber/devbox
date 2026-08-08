import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile } from "node:fs/promises";

import { PersistentOAuthState } from "../src/oauth.js";

test("persistent OAuth state prunes expired transient records on load and persistence", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "devbox-oauth-prune-"));
  const filePath = path.join(dir, "oauth-state.json");
  const now = Date.now();
  const seed = {
    clients: [["client", { client_id: "client" }]],
    authorizationCodes: [
      ["expired-code", { clientId: "client", expiresAt: now - 1, params: { scopes: [] } }],
      ["live-code", { clientId: "client", expiresAt: now + 60000, params: { scopes: [] } }],
    ],
    accessTokens: [
      ["expired-access", { clientId: "client", scopes: [], expiresAt: now - 1 }],
      ["live-access", { clientId: "client", scopes: [], expiresAt: now + 60000 }],
    ],
    refreshTokens: [
      ["expired-refresh", { clientId: "client", scopes: [], expiresAt: now - 1 }],
      ["live-refresh", { clientId: "client", scopes: [], expiresAt: now + 60000 }],
    ],
  };
  await import("node:fs/promises").then(({ writeFile }) => writeFile(filePath, JSON.stringify(seed), "utf8"));

  const state = new PersistentOAuthState(filePath);
  await state.ensureLoaded();
  assert.equal(state.authorizationCodes.has("expired-code"), false);
  assert.equal(state.accessTokens.has("expired-access"), false);
  assert.equal(state.refreshTokens.has("expired-refresh"), false);
  assert.equal(state.authorizationCodes.has("live-code"), true);
  assert.equal(state.accessTokens.has("live-access"), true);
  assert.equal(state.refreshTokens.has("live-refresh"), true);

  const persisted = JSON.parse(await readFile(filePath, "utf8"));
  assert.equal(persisted.authorizationCodes.length, 1);
  assert.equal(persisted.accessTokens.length, 1);
  assert.equal(persisted.refreshTokens.length, 1);
});
