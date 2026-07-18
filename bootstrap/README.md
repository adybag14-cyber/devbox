# Devbox setup binary

`devbox-setup` is a dependency-free Rust bootstrap program for new Devbox MCP installations.

It can configure an existing checkout or clone the official repository into `./devbox`, then:

- verifies Node.js 18+ and npm
- creates `.env` from `.env.example` without discarding an existing configuration
- creates `workspace/` and `run/`
- runs `npm install`
- attempts `npm link` without blocking setup if the global npm prefix is not writable
- starts the service and checks `/healthz`

Build and test:

```bash
cargo test --manifest-path bootstrap/Cargo.toml
cargo build --release --manifest-path bootstrap/Cargo.toml
```

Run from an existing checkout:

```bash
./bootstrap/target/release/devbox-setup --repo .
```

Run from an empty directory to clone and configure Devbox:

```bash
devbox-setup
```

Use `devbox-setup --help` for runtime, port, workspace, and non-starting setup options.
