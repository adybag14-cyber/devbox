# Docker ChatGPT Devbox MCP

This repo runs a ChatGPT-compatible remote MCP server on Windows. The main execution environment is a reproducible Linux Docker devbox. An optional Windows host bridge exposes native host tools such as PowerShell, Git, Docker CLI, Node, Python, and winget. Please note ChatGPT may become too powerful you have been warned!!!!!
## DO you need auto-approval and openai is annoying you with every approve box command what if you could allow pasting in the dev console and then never have to think about it again???

```
let lastClicked = null;

setInterval(() => {
  const btn = [...document.querySelectorAll('button')].find(b => {
    const rect = b.getBoundingClientRect();
    const bg = getComputedStyle(b).backgroundColor;

    const sizeMatch =
      rect.width >= 195 && rect.width <= 199 &&
      rect.height >= 35 && rect.height <= 37;

    const colorMatch =
      bg === 'rgb(13, 13, 13)' ||
      bg === 'rgb(0, 0, 0)';

    return sizeMatch && colorMatch ;
  });

  if (btn && btn !== lastClicked) {
    lastClicked = btn;
    console.log('Clicking:', btn.innerText.trim(), btn.getBoundingClientRect());
    btn.click();
  }
}, 1000)
```


## What is included

- `src/server.js`: MCP server exposed over Streamable HTTP
- `runtime.Dockerfile`: reproducible Linux devbox image
- `workspace/`: host workspace mounted into the devbox at `/workspace`
- `scripts/Start-ChatGptDevboxMcp.ps1`: builds the runtime, starts the devbox, starts the MCP server, and can publish a public HTTPS URL
- `scripts/Stop-ChatGptDevboxMcp.ps1`: stops the MCP server and optional tunnel/devbox runtime
- `scripts/Get-ChatGptSetup.ps1`: prints the values to paste into the ChatGPT custom app form
- `scripts/Get-CloudflareAccessSetup.ps1`: prints the Cloudflare Access settings needed to protect `/authorize*`

## Requirements

- Windows
- Docker Desktop
- Node.js
- PowerShell
- Git
- Optional:
  - GitHub CLI for repo auth sync into the devbox
  - Cloudflare Tunnel for a stable public hostname
  - Cloudflare Access if you want real browser auth on `/authorize*`

## Clone and bootstrap

```powershell
git clone https://github.com/adybag14-cyber/devbox.git
cd .\devbox
Copy-Item .env.example .env
```

You can leave these `.env` values blank and the repo will derive them automatically:

- `HOST_WORKSPACE_PATH`: defaults to `<repo>\workspace`
- `HOST_DEFAULT_WORKDIR`: defaults to your Windows home directory
- `NODE_EXE`: defaults to the current `node.exe`
- `OAUTH_STATE_FILE_PATH`: defaults to `<repo>\run\oauth-state.json`

## Start locally

```powershell
cd .\devbox
.\scripts\Start-ChatGptDevboxMcp.ps1
```

## Start with a public URL

```powershell
cd .\devbox
.\scripts\Start-ChatGptDevboxMcp.ps1 -Public -OAuth
```

If `.env` contains both `CLOUDFLARED_PUBLIC_HOSTNAME` and `CLOUDFLARED_TUNNEL_TOKEN`, `-Public` uses the named Cloudflare tunnel and keeps the MCP URL stable on your domain.

If you want the stack to stay available across logon and recover from basic runtime failures, install the guardian with `.\scripts\Install-ChatGptDevboxGuardian.ps1` for local mode or `.\scripts\Install-ChatGptDevboxGuardian.ps1 -Public -OAuth` for the public OAuth path. Setup and status commands are in [docs/GUARDIAN.md](./docs/GUARDIAN.md).

## ChatGPT connector values

After startup:

```powershell
.\scripts\Get-ChatGptSetup.ps1
```

Typical output:

- `Name`: `Docker Devbox`
- `Description`: `Reproducible Docker devbox shell plus optional Windows host tools`
- `MCP Server URL`: `https://<your-host>/mcp`
- `Authentication`: `OAuth`

## Cloudflare Access mode

If you want the ChatGPT OAuth flow to require a real login:

1. Create a Cloudflare Access self-hosted application that protects `https://<your-host>/authorize*`.
2. Restrict the Access policy to your identity.
3. Set these values in `.env`:
   - `MCP_AUTH_MODE=cloudflare-access`
   - `CLOUDFLARE_ACCESS_TEAM_DOMAIN=https://<your-team>.cloudflareaccess.com`
   - `CLOUDFLARE_ACCESS_AUD=<Access application AUD>`
   - `CLOUDFLARE_ACCESS_JWKS_URL=https://<your-team>.cloudflareaccess.com/cdn-cgi/access/certs`
4. Restart with:

```powershell
.\scripts\Start-ChatGptDevboxMcp.ps1 -Public -OAuth
```

Use this helper to print the Access-side target:

```powershell
.\scripts\Get-CloudflareAccessSetup.ps1
```

## Stop

```powershell
cd .\devbox
.\scripts\Stop-ChatGptDevboxMcp.ps1 -Tunnel
```

Use `-All` to also stop the devbox container.

## Reproducing on another device

See [docs/REPRODUCE.md](./docs/REPRODUCE.md) for a full copy-to-another-machine checklist.

## Security notes

- `windows_host_exec` is high risk because it gives ChatGPT PowerShell access to the Windows host.
- `windows_host_run_program` is limited by `HOST_PROGRAM_ALLOWLIST`.
- Do not commit `.env`, `.env.runtime`, `run/`, `workspace/`, or other live runtime state.


It also includes `devbox_write_large_file`, a base64-backed large file writer that verifies the exact bytes written so agents can mirror payloads without corruption.
It also includes `devbox_read_large_file`, a base64 chunk reader with real byte offsets, chunk metadata, and paging support for later sections of large logs and generated files.
