# Reproduce This Devbox On Another Device

## 1. Install prerequisites

- Docker Desktop
- Node.js
- Git
- PowerShell
- Optional:
  - GitHub CLI
  - Cloudflare Tunnel
  - Cloudflare Access

## 2. Clone the repo

```powershell
git clone https://github.com/adybag14-cyber/devbox.git
cd .\devbox
Copy-Item .env.example .env
```

## 3. Fill `.env`

Minimum local-only setup:

- `PUBLIC_BASE_URL=` leave blank if you are not exposing the server remotely
- `MCP_AUTH_MODE=demo-oauth` or `none`

For a stable remote ChatGPT app:

- `PUBLIC_BASE_URL=https://<your-host>`
- `CLOUDFLARED_PUBLIC_HOSTNAME=<your-host>`
- `CLOUDFLARED_TUNNEL_TOKEN=<your-named-tunnel-token>`
- `MCP_AUTH_MODE=cloudflare-access`
- `CLOUDFLARE_ACCESS_TEAM_DOMAIN=https://<your-team>.cloudflareaccess.com`
- `CLOUDFLARE_ACCESS_AUD=<your-access-app-aud>`
- `CLOUDFLARE_ACCESS_JWKS_URL=https://<your-team>.cloudflareaccess.com/cdn-cgi/access/certs`

You can leave these blank and the repo will derive them:

- `HOST_WORKSPACE_PATH`
- `HOST_DEFAULT_WORKDIR`
- `NODE_EXE`
- `OAUTH_STATE_FILE_PATH`

## 4. Start the stack

Local only:

```powershell
.\scripts\Start-ChatGptDevboxMcp.ps1
```

Public ChatGPT connector:

```powershell
.\scripts\Start-ChatGptDevboxMcp.ps1 -Public -OAuth
```

## 5. Get the ChatGPT form values

```powershell
.\scripts\Get-ChatGptSetup.ps1
```

## 6. Optional GitHub auth sync

If the Windows host is already logged into GitHub with `gh`, ChatGPT can use the `devbox_sync_github_auth_from_host` tool to copy that auth into the Docker devbox.

## 7. Move secrets carefully

Do not sync these files between devices:

- `.env`
- `.env.runtime`
- `run/`
- `workspace/`

Each device should get its own `.env` with its own tunnel token, Access app values, and host-specific paths if you override the defaults.
