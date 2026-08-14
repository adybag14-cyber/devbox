use std::{
    collections::BTreeMap,
    future::Future,
    sync::{Arc, Mutex, PoisonError},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use serde::Serialize;
use tokio_util::sync::CancellationToken;

const RESTART_BACKOFF_INITIAL: Duration = Duration::from_millis(500);
const RESTART_BACKOFF_MAX: Duration = Duration::from_secs(30);
const RESTART_BACKOFF_RESET_AFTER: Duration = Duration::from_secs(30);

#[derive(Debug, Clone, Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct BackgroundTaskState {
    pub kind: String,
    pub running: bool,
    pub starts: u64,
    pub restarts: u64,
    pub last_tick_unix_ms: Option<u64>,
    pub last_attempt_unix_ms: Option<u64>,
    pub last_success_unix_ms: Option<u64>,
    pub last_failure_unix_ms: Option<u64>,
    pub consecutive_failures: u64,
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
    /// Record that one iteration is about to attempt its real work.
    pub fn attempt(&self) {
        self.registry.attempt(&self.name);
    }

    /// Record a successful iteration and clear any prior degraded state.
    pub fn tick(&self) {
        self.registry.success(&self.name);
    }

    /// Record a failed iteration without killing the loop.
    pub fn fail(&self, error: impl Into<String>) {
        self.registry.failure(&self.name, error.into());
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
        self.spawn_supervised_kind(name, "periodic", cancellation, factory);
    }

    pub fn spawn_event_driven<F, Fut>(
        &self,
        name: &'static str,
        cancellation: CancellationToken,
        factory: F,
    ) where
        F: Fn(CancellationToken, TaskHeartbeat) -> Fut + Send + Sync + 'static,
        Fut: Future<Output = Result<(), String>> + Send + 'static,
    {
        self.spawn_supervised_kind(name, "event-driven", cancellation, factory);
    }

    fn spawn_supervised_kind<F, Fut>(
        &self,
        name: &'static str,
        kind: &'static str,
        cancellation: CancellationToken,
        factory: F,
    ) where
        F: Fn(CancellationToken, TaskHeartbeat) -> Fut + Send + Sync + 'static,
        Fut: Future<Output = Result<(), String>> + Send + 'static,
    {
        let registry = self.clone();
        let factory = Arc::new(factory);
        tokio::spawn(async move {
            let mut restart_delay = RESTART_BACKOFF_INITIAL;
            loop {
                if cancellation.is_cancelled() {
                    registry.mark_stopped(name, None);
                    return;
                }
                registry.mark_started(name, kind);
                let heartbeat = TaskHeartbeat {
                    registry: registry.clone(),
                    name: Arc::from(name),
                };
                let child_cancel = cancellation.child_token();
                let started = Instant::now();
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
                let delay = restart_delay;
                restart_delay = if started.elapsed() >= RESTART_BACKOFF_RESET_AFTER {
                    RESTART_BACKOFF_INITIAL
                } else {
                    restart_delay.saturating_mul(2).min(RESTART_BACKOFF_MAX)
                };
                tokio::select! {
                    () = cancellation.cancelled() => return,
                    () = tokio::time::sleep(delay) => {},
                }
            }
        });
    }

    pub fn spawn_once_tracked<Fut>(&self, name: &'static str, future: Fut)
    where
        Fut: Future<Output = Result<(), String>> + Send + 'static,
    {
        let registry = self.clone();
        registry.mark_started(name, "one-shot");
        tokio::spawn(async move {
            match future.await {
                Ok(()) => {
                    registry.success(name);
                    registry.mark_stopped(name, None);
                }
                Err(error) => registry.mark_stopped(name, Some(error)),
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
                let success_age = state
                    .last_success_unix_ms
                    .map(|tick| now.saturating_sub(tick));
                let failure_age = state
                    .last_failure_unix_ms
                    .map(|tick| now.saturating_sub(tick));
                let idle_for = (state.kind == "event-driven").then(|| {
                    state
                        .last_attempt_unix_ms
                        .map_or(0, |at| now.saturating_sub(at))
                });
                (
                    name.clone(),
                    serde_json::json!({
                        "kind": state.kind,
                        "idleForMs": idle_for,
                        "running": state.running,
                        "starts": state.starts,
                        "restarts": state.restarts,
                        "lastTickUnixMs": state.last_tick_unix_ms,
                        "lastTickAgeMs": age,
                        "lastAttemptUnixMs": state.last_attempt_unix_ms,
                        "lastSuccessUnixMs": state.last_success_unix_ms,
                        "lastSuccessAgeMs": success_age,
                        "lastFailureUnixMs": state.last_failure_unix_ms,
                        "lastFailureAgeMs": failure_age,
                        "consecutiveFailures": state.consecutive_failures,
                        "lastError": state.last_error,
                    }),
                )
            })
            .collect::<serde_json::Map<_, _>>();
        serde_json::Value::Object(mapped)
    }

    fn mark_started(&self, name: &str, kind: &str) {
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        kind.clone_into(&mut state.kind);
        if state.starts > 0 {
            state.restarts = state.restarts.saturating_add(1);
        }
        state.starts = state.starts.saturating_add(1);
        state.running = true;
        state.last_attempt_unix_ms = Some(unix_ms());
    }

    fn mark_stopped(&self, name: &str, error: Option<String>) {
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        state.running = false;
        if let Some(error) = error {
            let now = unix_ms();
            state.last_failure_unix_ms = Some(now);
            state.consecutive_failures = state.consecutive_failures.saturating_add(1);
            state.last_error = Some(error);
        }
    }

    fn attempt(&self, name: &str) {
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        state.running = true;
        state.last_attempt_unix_ms = Some(unix_ms());
    }

    fn success(&self, name: &str) {
        let now = unix_ms();
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        state.running = true;
        state.last_tick_unix_ms = Some(now);
        state.last_success_unix_ms = Some(now);
        state.consecutive_failures = 0;
        state.last_error = None;
    }

    fn failure(&self, name: &str, error: String) {
        let now = unix_ms();
        let mut states = self.inner.lock().unwrap_or_else(PoisonError::into_inner);
        let state = states.entry(name.to_owned()).or_default();
        state.running = true;
        state.last_failure_unix_ms = Some(now);
        state.consecutive_failures = state.consecutive_failures.saturating_add(1);
        state.last_error = Some(error);
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
    async fn heartbeat_distinguishes_attempt_failure_and_success() {
        let registry = BackgroundTaskRegistry::new();
        registry.mark_started("semantic-fixture", "periodic");
        let heartbeat = TaskHeartbeat {
            registry: registry.clone(),
            name: Arc::from("semantic-fixture"),
        };
        heartbeat.attempt();
        heartbeat.fail("disk unavailable");
        let failed = registry.snapshot();
        assert_eq!(failed["semantic-fixture"]["consecutiveFailures"], 1);
        assert_eq!(failed["semantic-fixture"]["lastError"], "disk unavailable");
        assert!(failed["semantic-fixture"]["lastSuccessUnixMs"].is_null());
        heartbeat.tick();
        let recovered = registry.snapshot();
        assert_eq!(recovered["semantic-fixture"]["consecutiveFailures"], 0);
        assert!(recovered["semantic-fixture"]["lastError"].is_null());
        assert!(
            recovered["semantic-fixture"]["lastSuccessUnixMs"]
                .as_u64()
                .is_some()
        );
    }

    #[tokio::test]
    async fn tracked_one_shot_records_terminal_success_without_appearing_degraded() {
        let registry = BackgroundTaskRegistry::new();
        registry.spawn_once_tracked("one-shot", async { Ok(()) });
        let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
        loop {
            let snapshot = registry.snapshot();
            if snapshot["one-shot"]["lastSuccessUnixMs"].as_u64().is_some()
                && snapshot["one-shot"]["running"] == false
            {
                assert_eq!(snapshot["one-shot"]["consecutiveFailures"], 0);
                break;
            }
            assert!(
                tokio::time::Instant::now() < deadline,
                "tracked one-shot never completed"
            );
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    }
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
