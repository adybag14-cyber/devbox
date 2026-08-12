use std::{
    collections::BTreeMap,
    future::Future,
    sync::{Arc, Mutex, PoisonError},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use serde::Serialize;
use tokio_util::sync::CancellationToken;

#[derive(Debug, Clone, Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct BackgroundTaskState {
    pub running: bool,
    pub starts: u64,
    pub restarts: u64,
    pub last_tick_unix_ms: Option<u64>,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Default)]
pub struct BackgroundTaskRegistry {
    inner: Arc<Mutex<BTreeMap<String, BackgroundTaskState>>>,
}

#[derive(Debug, Clone)]
pub struct TaskHeartbeat {
    registry: BackgroundTaskRegistry,
    name: Arc<str>,
}

impl TaskHeartbeat {
    pub fn tick(&self) {
        self.registry.tick(&self.name);
    }
}

impl BackgroundTaskRegistry {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn spawn_supervised<F, Fut>(
        &self,
        name: &'static str,
        cancellation: CancellationToken,
        factory: F,
    ) where
        F: Fn(CancellationToken, TaskHeartbeat) -> Fut + Send + Sync + 'static,
        Fut: Future<Output = Result<(), String>> + Send + 'static,
    {
        let registry = self.clone();
        let factory = Arc::new(factory);
        tokio::spawn(async move {
            loop {
                if cancellation.is_cancelled() {
                    registry.mark_stopped(name, None);
                    return;
                }
                registry.mark_started(name);
                let heartbeat = TaskHeartbeat {
                    registry: registry.clone(),
                    name: Arc::from(name),
                };
                heartbeat.tick();
                let child_cancel = cancellation.child_token();
                let mut task = tokio::spawn(factory(child_cancel.clone(), heartbeat));
                let failure = tokio::select! {
                    () = cancellation.cancelled() => {
                        child_cancel.cancel();
                        task.abort();
                        let _ = task.await;
                        registry.mark_stopped(name, None);
                        return;
                    }
                    result = &mut task => match result {
                        Ok(Ok(())) => Some("background task exited unexpectedly".to_owned()),
                        Ok(Err(error)) => Some(error),
                        Err(error) => Some(format!("background task panicked or was cancelled: {error}")),
                    },
                };
                registry.mark_stopped(name, failure);
                tokio::select! {
                    () = cancellation.cancelled() => return,
                    () = tokio::time::sleep(Duration::from_millis(500)) => {},
                }
            }
        });
    }

    #[must_use]
    pub fn snapshot(&self) -> serde_json::Value {
        let now = unix_ms();
        let states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let mapped = states
            .iter()
            .map(|(name, state)| {
                let age = state.last_tick_unix_ms.map(|tick| now.saturating_sub(tick));
                (
                    name.clone(),
                    serde_json::json!({
                        "running": state.running,
                        "starts": state.starts,
                        "restarts": state.restarts,
                        "lastTickUnixMs": state.last_tick_unix_ms,
                        "lastTickAgeMs": age,
                        "lastError": state.last_error,
                    }),
                )
            })
            .collect::<serde_json::Map<_, _>>();
        serde_json::Value::Object(mapped)
    }

    fn mark_started(&self, name: &str) {
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        if state.starts > 0 {
            state.restarts = state.restarts.saturating_add(1);
        }
        state.starts = state.starts.saturating_add(1);
        state.running = true;
        state.last_error = None;
        state.last_tick_unix_ms = Some(unix_ms());
    }

    fn mark_stopped(&self, name: &str, error: Option<String>) {
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        state.running = false;
        if let Some(error) = error {
            state.last_error = Some(error);
        }
    }

    fn tick(&self, name: &str) {
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        state.running = true;
        state.last_tick_unix_ms = Some(unix_ms());
    }
}

fn unix_ms() -> u64 {
    u64::try_from(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis(),
    )
    .unwrap_or(u64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    #[tokio::test]
    async fn supervisor_restarts_failed_background_task() {
        let registry = BackgroundTaskRegistry::new();
        let cancellation = CancellationToken::new();
        let starts = Arc::new(AtomicUsize::new(0));
        let starts_factory = starts.clone();
        registry.spawn_supervised("fixture", cancellation.clone(), move |cancel, heartbeat| {
            let starts = starts_factory.clone();
            async move {
                let attempt = starts.fetch_add(1, Ordering::SeqCst);
                heartbeat.tick();
                if attempt == 0 {
                    return Err("synthetic failure".to_owned());
                }
                cancel.cancelled().await;
                Ok(())
            }
        });
        let deadline = tokio::time::Instant::now() + Duration::from_secs(3);
        while starts.load(Ordering::SeqCst) < 2 && tokio::time::Instant::now() < deadline {
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        let snapshot = registry.snapshot();
        assert!(starts.load(Ordering::SeqCst) >= 2);
        assert!(snapshot["fixture"]["restarts"].as_u64().unwrap_or(0) >= 1);
        assert_eq!(snapshot["fixture"]["running"], true);
        cancellation.cancel();
    }
}
