use std::{path::PathBuf, sync::Arc};

use anyhow::Result;
use devbox_mcp::{Config, contract::ParityReport};
use tokio_util::sync::CancellationToken;
use tracing_subscriber::{EnvFilter, layer::SubscriberExt, util::SubscriberInitExt};

#[tokio::main]
async fn main() -> Result<()> {
    if let Some(arguments) = capture_worker_arguments() {
        return devbox_mcp::capture::run_capture_worker(&arguments).await;
    }

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
    if let Some(request_path) = job_runner_request()? {
        return devbox_mcp::job_runner::run_job_request(config, &request_path).await;
    }

    let cancellation = CancellationToken::new();
    let (address, mut http_task) =
        devbox_mcp::server::serve(config.clone(), cancellation.clone()).await?;
    tracing::info!(%address, name = %config.server_name(), "Rust Devbox MCP listening");

    let shutdown_requested = tokio::select! {
        result = &mut http_task => {
            match result {
                Ok(Ok(())) => anyhow::bail!("Rust MCP HTTP server exited unexpectedly."),
                Ok(Err(error)) => return Err(error.into()),
                Err(error) => return Err(anyhow::anyhow!("Rust MCP HTTP server task failed: {error}")),
            }
        },
        result = tokio::signal::ctrl_c() => { result?; true },
        result = termination_signal() => { result?; true },
        () = cancellation.cancelled() => true,
    };
    if shutdown_requested {
        cancellation.cancel();
        match tokio::time::timeout(std::time::Duration::from_secs(10), &mut http_task).await {
            Ok(Ok(Ok(()))) => {}
            Ok(Ok(Err(error))) => return Err(error.into()),
            Ok(Err(error)) => {
                return Err(anyhow::anyhow!(
                    "Rust MCP HTTP server task failed during shutdown: {error}"
                ));
            }
            Err(_) => anyhow::bail!("Rust MCP HTTP server did not stop within 10 seconds."),
        }
    }
    Ok(())
}

#[cfg(unix)]
async fn termination_signal() -> std::io::Result<()> {
    use tokio::signal::unix::{SignalKind, signal};

    let mut terminate = signal(SignalKind::terminate())?;
    terminate.recv().await;
    Ok(())
}

#[cfg(not(unix))]
async fn termination_signal() -> std::io::Result<()> {
    std::future::pending::<()>().await;
    Ok(())
}

fn job_runner_request() -> anyhow::Result<Option<PathBuf>> {
    let mut args = std::env::args_os().skip(1);
    let Some(first) = args.next() else {
        return Ok(None);
    };
    if first != "--job-runner" {
        return Ok(None);
    }
    let request = args
        .next()
        .ok_or_else(|| anyhow::anyhow!("--job-runner requires a request.json path"))?;
    if args.next().is_some() {
        anyhow::bail!("--job-runner accepts exactly one request.json path");
    }
    Ok(Some(PathBuf::from(request)))
}

fn capture_worker_arguments() -> Option<Vec<String>> {
    let mut args = std::env::args().skip(1);
    if args.next().as_deref() != Some("--capture-worker") {
        return None;
    }
    Some(args.collect())
}
