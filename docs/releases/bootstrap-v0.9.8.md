# Devbox C++23 0.9.8

This release includes the native dependency and performance improvements from
[0.9.7](bootstrap-v0.9.7.md), and corrects native HTTP metadata privacy behind
local reverse proxies and tunnels.

A loopback proxy connection could be classified as a local operator when a
client supplied a loopback value at the beginning of `X-Forwarded-For`. That
classification exposed internal operational metadata through the public root
endpoint. MCP tool authentication remained enforced.

Native local metadata and the optional local browser bridge now require a
loopback peer, a loopback Host authority, and no forwarding headers. Public
hostnames always receive the public metadata view. Direct loopback supervision,
readiness and operator probes remain available. Forwarded requests retain normal
authenticated MCP access; forwarding headers do not grant local privileges.
This metadata boundary also applies when authentication is explicitly disabled;
it does not add authentication to an intentionally unauthenticated MCP server.

Wire regression coverage includes forged loopback addresses, mixed proxy chains,
localhost and IPv4-mapped IPv6 forms, empty forwarding headers, and the Forwarded,
CF-Connecting-IP, True-Client-IP, X-Real-IP, Via and other X-Forwarded headers.
