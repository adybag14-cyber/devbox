export const MCP_OAUTH_SCOPES = Object.freeze([
  "mcp:tools",
  "mcp:devbox:read",
  "mcp:devbox:exec",
  "mcp:host:read",
  "mcp:host:exec",
  "mcp:admin",
]);

const TOOL_SCOPE_BY_NAME = new Map([
  ...[
    "devbox_status", "devbox_github_auth_status", "devbox_job_logs", "devbox_job_status",
    "devbox_list_files", "devbox_read_file", "devbox_read_large_file", "devbox_search_files",
    "devbox_wait", "devbox_wait_for_file",
  ].map((name) => [name, "mcp:devbox:read"]),
  ...[
    "devbox_exec", "devbox_exec_readonly", "devbox_exec_start", "devbox_job_cancel",
    "devbox_run_program", "devbox_run_program_start", "devbox_write_file", "devbox_write_large_file",
  ].map((name) => [name, "mcp:devbox:exec"]),
  ...[
    "devbox_recreate", "devbox_restart", "devbox_start", "devbox_stop", "devbox_sync_github_auth_from_host",
  ].map((name) => [name, "mcp:admin"]),
  ...[
    "host_capture_display", "host_capture_program", "host_capture_window", "host_status",
    "windows_host_capture_display", "windows_host_capture_program", "windows_host_inspect_file",
    "windows_host_read_large_file", "windows_host_status",
  ].map((name) => [name, "mcp:host:read"]),
  ...[
    "host_exec", "host_run_program", "windows_host_exec", "windows_host_run_program", "windows_host_write_large_file",
  ].map((name) => [name, "mcp:host:exec"]),
]);

export const requiredToolScope = (toolName) => TOOL_SCOPE_BY_NAME.get(String(toolName ?? "")) ?? null;

export const oauthScopeAllows = (scopes, requiredScope) => {
  if (!requiredScope) return true;
  const values = Array.isArray(scopes) ? scopes.map(String) : [];
  return values.includes("mcp:tools") || values.includes(requiredScope);
};

export const missingRequiredToolScope = (toolName, authInfo) => {
  if (!authInfo) return null;
  const required = requiredToolScope(toolName);
  if (!required) return null;
  return oauthScopeAllows(authInfo.scopes, required) ? null : required;
};
