import test from "node:test";
import assert from "node:assert/strict";

import {
  MCP_OAUTH_SCOPES,
  missingRequiredToolScope,
  oauthScopeAllows,
  requiredToolScope,
} from "../src/tool-authorization.js";

test("legacy mcp:tools scope remains a full-compatibility capability", () => {
  assert.equal(oauthScopeAllows(["mcp:tools"], "mcp:host:exec"), true);
  assert.equal(missingRequiredToolScope("host_exec", { clientId: "legacy", scopes: ["mcp:tools"] }), null);
});

test("narrow OAuth capabilities gate elevated host and admin tools independently", () => {
  assert.equal(requiredToolScope("host_exec"), "mcp:host:exec");
  assert.equal(requiredToolScope("devbox_status"), "mcp:devbox:read");
  assert.equal(requiredToolScope("devbox_restart"), "mcp:admin");
  assert.equal(missingRequiredToolScope("host_exec", { clientId: "reader", scopes: ["mcp:host:read"] }), "mcp:host:exec");
  assert.equal(missingRequiredToolScope("host_status", { clientId: "reader", scopes: ["mcp:host:read"] }), null);
  assert.equal(missingRequiredToolScope("devbox_restart", { clientId: "exec", scopes: ["mcp:devbox:exec"] }), "mcp:admin");
});

test("non-OAuth/local mode remains unrestricted by capability mapping", () => {
  assert.equal(missingRequiredToolScope("host_exec", null), null);
  assert.ok(MCP_OAUTH_SCOPES.includes("mcp:tools"));
  assert.ok(MCP_OAUTH_SCOPES.includes("mcp:admin"));
});
