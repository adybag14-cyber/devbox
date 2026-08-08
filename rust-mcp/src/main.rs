use std::sync::Arc;

use anyhow::Result;
use devbox_mcp::{AuthMode, Config, contract::ParityReport};
use tokio_util::sync::CancellationToken;
use tracing_subscriber::{EnvFilter, layer::SubscriberExt, util::SubscriberInitExt};

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::registry()
        .with(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("devbox_mcp=info,tower_http=warn")),
        )
        .with(tracing_subscriber::fmt::layer())
        .init();

    if std::env::args().any(|arg| arg == "--parity-report") {
        println!(
            "{}",
            serde_json::to_string_pretty(&ParityReport::current())?
        );
        return Ok(());
    }

    let config = Arc::new(Config::load()?);
    if config.auth_mode != AuthMode::None {
        anyhow::bail!(
            "Rust MCP authentication parity is not implemented yet; refusing to start with MCP_AUTH_MODE={}",
            config.auth_mode.as_str()
        );
    }

    let cancellation = CancellationToken::new();
    let address = devbox_mcp::server::serve(config.clone(), cancellation.clone()).await?;
    tracing::info!(%address, name = %config.server_name(), "Rust Devbox MCP listening");

    tokio::select! {
        result = tokio::signal::ctrl_c() => result?,
        () = cancellation.cancelled() => {},
    }
    cancellation.cancel();
    Ok(())
}
