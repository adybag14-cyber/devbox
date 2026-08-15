import test from "node:test";
import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile } from "node:fs/promises";

import { DemoClientStore, PersistentOAuthState } from "../src/oauth.js";

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


test("OAuth client capacity evicts oldest unreferenced clients but preserves active ones", async () => {
  const state = new PersistentOAuthState();
  state.loaded = true;
  state.clients.set("active", { client_id: "active", client_id_issued_at: 1 });
  state.clients.set("old", { client_id: "old", client_id_issued_at: 2 });
  state.clients.set("new", { client_id: "new", client_id_issued_at: 3 });
  state.accessTokens.set("token", { clientId: "active", scopes: [], expiresAt: Date.now() + 60_000 });
  const removed = state.pruneClientsForCapacity(2);
  assert.equal(removed, 2);
  assert.equal(state.clients.has("active"), true);
  assert.equal(state.clients.size, 1);
});


test("OAuth client capacity clamps an explicit zero to one", () => {
  const state = new PersistentOAuthState();
  const store = new DemoClientStore(state, { maxClients: 0 });
  assert.equal(store.maxClients, 1);
});


test("persistent OAuth state recovers after a prior serialized write failure", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "devbox-oauth-recover-"));
  const filePath = path.join(dir, "oauth-state.json");
  const state = new PersistentOAuthState(filePath);
  await state.ensureLoaded();
  state.clients.set("client", { client_id: "client", client_id_issued_at: 1 });
  state.writePromise = Promise.reject(new Error("synthetic prior write failure"));
  await state.persist();
  const persisted = JSON.parse(await readFile(filePath, "utf8"));
  assert.equal(persisted.clients.length, 1);
  assert.equal(persisted.clients[0][0], "client");
});
