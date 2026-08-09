use std::{
    collections::BTreeMap,
    fmt,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use time::{OffsetDateTime, format_description::well_known::Rfc3339};
use tokio::{
    fs::{self, OpenOptions},
    io::AsyncWriteExt,
};
use tokio_util::sync::CancellationToken;

const POLL_INTERVAL: Duration = Duration::from_millis(50);
const CLAIM_RETRY_INTERVAL: Duration = Duration::from_millis(20);
const CORRUPT_SLOT_STALE: Duration = Duration::from_secs(5 * 60);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ResourceClass {
    Watch,
    Light,
    Heavy,
}

impl ResourceClass {
    #[must_use]
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "watch" => Self::Watch,
            "heavy" => Self::Heavy,
            _ => Self::Light,
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Watch => "watch",
            Self::Light => "light",
            Self::Heavy => "heavy",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExecutionKind {
    Interactive,
    Background,
}

impl ExecutionKind {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Interactive => "interactive",
            Self::Background => "background",
        }
    }
}

#[derive(Debug, Clone)]
pub struct SchedulerConfig {
    pub root: PathBuf,
    pub max_concurrent: usize,
    pub reserved_interactive: usize,
    pub watch_max_concurrent: usize,
    pub queue_timeout: Duration,
    pub heavy_weight: usize,
}

impl SchedulerConfig {
    #[must_use]
    pub fn normalized(mut self) -> Self {
        self.max_concurrent = self.max_concurrent.max(1);
        self.reserved_interactive = self
            .reserved_interactive
            .min(self.max_concurrent.saturating_sub(1));
        self.watch_max_concurrent = self.watch_max_concurrent.max(1);
        self.queue_timeout = self.queue_timeout.max(Duration::from_millis(1));
        self.heavy_weight = self.heavy_weight.max(1);
        self
    }
}

#[derive(Debug, Clone)]
pub struct AcquireRequest {
    pub kind: ExecutionKind,
    pub resource_class: ResourceClass,
    pub weight: usize,
    pub label: String,
    pub queue_timeout: Option<Duration>,
}

impl AcquireRequest {
    #[must_use]
    pub fn interactive(label: impl Into<String>) -> Self {
        Self {
            kind: ExecutionKind::Interactive,
            resource_class: ResourceClass::Light,
            weight: 1,
            label: label.into(),
            queue_timeout: None,
        }
    }

    #[must_use]
    pub fn background(
        label: impl Into<String>,
        resource_class: ResourceClass,
        weight: usize,
    ) -> Self {
        Self {
            kind: ExecutionKind::Background,
            resource_class,
            weight,
            label: label.into(),
            queue_timeout: None,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct QueueTimeoutDetails {
    pub kind: String,
    pub label: String,
    pub pool: String,
    pub resource_class: String,
    pub weight: usize,
    pub queue_wait_ms: u64,
    pub max_concurrent: usize,
    pub reserved_interactive: usize,
}

#[derive(Debug)]
pub struct ExecutionQueueTimeoutError {
    message: String,
    pub details: QueueTimeoutDetails,
}

impl fmt::Display for ExecutionQueueTimeoutError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for ExecutionQueueTimeoutError {}

#[derive(Debug)]
pub struct ExecutionQueueCancelledError;

impl fmt::Display for ExecutionQueueCancelledError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("Execution queue wait cancelled by the MCP client.")
    }
}

impl std::error::Error for ExecutionQueueCancelledError {}

#[derive(Debug, Clone, Default, Serialize)]
pub struct ResourceMetricsSnapshot {
    pub active: u64,
    pub acquired: u64,
    pub average_queue_wait_ms: u64,
    pub max_queue_wait_ms: u64,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct LocalMetricsSnapshot {
    pub queued: u64,
    pub active: u64,
    pub acquired: u64,
    pub timed_out: u64,
    pub cancelled: u64,
    pub average_queue_wait_ms: u64,
    pub max_queue_wait_ms: u64,
    pub by_resource_class: BTreeMap<String, ResourceMetricsSnapshot>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ExecutionSlotSnapshot {
    pub max_concurrent: usize,
    pub reserved_interactive: usize,
    pub background_capacity: usize,
    pub watch_capacity: usize,
    pub occupied: usize,
    pub occupied_slots: Vec<Value>,
    pub watch_occupied: usize,
    pub watch_slots: Vec<Value>,
    pub local_process: LocalMetricsSnapshot,
}

#[derive(Debug, Default)]
struct ResourceMetrics {
    active: u64,
    acquired: u64,
    total_queue_wait_ms: u128,
    max_queue_wait_ms: u64,
}

#[derive(Debug, Default)]
struct Metrics {
    queued: u64,
    active: u64,
    acquired: u64,
    timed_out: u64,
    cancelled: u64,
    total_queue_wait_ms: u128,
    max_queue_wait_ms: u64,
    by_class: BTreeMap<String, ResourceMetrics>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SlotOwner {
    token: String,
    pid: u32,
    kind: String,
    pool: String,
    resource_class: String,
    weight: usize,
    label: String,
    acquired_at_utc: String,
}

#[derive(Debug)]
struct OwnedSlot {
    path: PathBuf,
    token: String,
    index: usize,
}

#[derive(Debug, Clone)]
struct AcquirePlan {
    pool: String,
    resource_class: ResourceClass,
    total: usize,
    reserved: usize,
    usable_slots: usize,
    requested_weight: usize,
}

impl AcquirePlan {
    fn new(config: &SchedulerConfig, request: &AcquireRequest) -> Self {
        let resource_class = request.resource_class;
        let pool = pool_for(request.kind, resource_class);
        let total = if pool == "watch" {
            config.watch_max_concurrent
        } else {
            config.max_concurrent
        };
        let reserved = if pool == "execution" {
            config.reserved_interactive.min(total.saturating_sub(1))
        } else {
            0
        };
        let usable_slots = if request.kind == ExecutionKind::Background && pool == "execution" {
            total.saturating_sub(reserved).max(1)
        } else {
            total
        };
        let requested_weight = if pool == "watch" {
            1
        } else {
            request.weight.max(1).min(usable_slots)
        };
        Self {
            pool,
            resource_class,
            total,
            reserved,
            usable_slots,
            requested_weight,
        }
    }
}

#[derive(Debug)]
pub struct ExecutionLease {
    owned: Vec<OwnedSlot>,
    metrics: Arc<Mutex<Metrics>>,
    class_key: String,
    released: bool,
    pub slot: Option<usize>,
    pub slots: Vec<usize>,
    pub kind: ExecutionKind,
    pub pool: String,
    pub resource_class: ResourceClass,
    pub weight: usize,
    pub queue_wait_ms: u64,
}

impl ExecutionLease {
    /// Release all slot files owned by this lease.
    ///
    /// # Errors
    /// Returns an error if an owned slot cannot be inspected or removed.
    pub async fn release(&mut self) -> Result<()> {
        if self.released {
            return Ok(());
        }
        release_owned_files(&self.owned).await?;
        self.mark_released();
        Ok(())
    }

    fn mark_released(&mut self) {
        if self.released {
            return;
        }
        self.released = true;
        let mut metrics = lock_metrics(&self.metrics);
        metrics.active = metrics.active.saturating_sub(1);
        if let Some(class) = metrics.by_class.get_mut(&self.class_key) {
            class.active = class.active.saturating_sub(1);
        }
    }
}

impl Drop for ExecutionLease {
    fn drop(&mut self) {
        if self.released {
            return;
        }
        for owned in &self.owned {
            release_owned_file_sync(owned);
        }
        self.mark_released();
    }
}

#[derive(Debug, Clone)]
pub struct ExecutionScheduler {
    config: SchedulerConfig,
    metrics: Arc<Mutex<Metrics>>,
}

impl ExecutionScheduler {
    #[must_use]
    pub fn new(config: SchedulerConfig) -> Self {
        Self {
            config: config.normalized(),
            metrics: Arc::new(Mutex::new(Metrics::default())),
        }
    }

    #[must_use]
    pub fn config(&self) -> &SchedulerConfig {
        &self.config
    }

    /// Acquire an execution lease using weighted capacity and the separate watch pool.
    ///
    /// # Errors
    /// Returns queue-cancelled, queue-timeout, or filesystem/process-inspection errors.
    pub async fn acquire(
        &self,
        request: AcquireRequest,
        cancellation: &CancellationToken,
    ) -> Result<ExecutionLease> {
        fs::create_dir_all(&self.config.root)
            .await
            .with_context(|| {
                format!("create execution slot root {}", self.config.root.display())
            })?;
        {
            let mut metrics = lock_metrics(&self.metrics);
            metrics.queued = metrics.queued.saturating_add(1);
        }

        let started = Instant::now();
        let timeout = request
            .queue_timeout
            .unwrap_or(self.config.queue_timeout)
            .max(Duration::from_millis(1));
        let outcome = self
            .acquire_inner(&request, cancellation, started, timeout)
            .await;

        match outcome {
            Ok((owned, pool, resource_class, weight)) => {
                let queue_wait_ms = duration_ms(started.elapsed());
                let class_key = resource_class.as_str().to_owned();
                {
                    let mut metrics = lock_metrics(&self.metrics);
                    metrics.queued = metrics.queued.saturating_sub(1);
                    metrics.active = metrics.active.saturating_add(1);
                    metrics.acquired = metrics.acquired.saturating_add(1);
                    metrics.total_queue_wait_ms = metrics
                        .total_queue_wait_ms
                        .saturating_add(u128::from(queue_wait_ms));
                    metrics.max_queue_wait_ms = metrics.max_queue_wait_ms.max(queue_wait_ms);
                    let class = metrics.by_class.entry(class_key.clone()).or_default();
                    class.active = class.active.saturating_add(1);
                    class.acquired = class.acquired.saturating_add(1);
                    class.total_queue_wait_ms = class
                        .total_queue_wait_ms
                        .saturating_add(u128::from(queue_wait_ms));
                    class.max_queue_wait_ms = class.max_queue_wait_ms.max(queue_wait_ms);
                }
                let slots = owned.iter().map(|entry| entry.index).collect::<Vec<_>>();
                Ok(ExecutionLease {
                    slot: slots.first().copied(),
                    slots,
                    kind: request.kind,
                    pool,
                    resource_class,
                    weight,
                    queue_wait_ms,
                    owned,
                    metrics: self.metrics.clone(),
                    class_key,
                    released: false,
                })
            }
            Err(error) => {
                let mut metrics = lock_metrics(&self.metrics);
                metrics.queued = metrics.queued.saturating_sub(1);
                if error.downcast_ref::<ExecutionQueueTimeoutError>().is_some() {
                    metrics.timed_out = metrics.timed_out.saturating_add(1);
                }
                if error
                    .downcast_ref::<ExecutionQueueCancelledError>()
                    .is_some()
                {
                    metrics.cancelled = metrics.cancelled.saturating_add(1);
                }
                Err(error)
            }
        }
    }

    async fn acquire_inner(
        &self,
        request: &AcquireRequest,
        cancellation: &CancellationToken,
        started: Instant,
        timeout: Duration,
    ) -> Result<(Vec<OwnedSlot>, String, ResourceClass, usize)> {
        let plan = AcquirePlan::new(&self.config, request);
        loop {
            if cancellation.is_cancelled() {
                return Err(ExecutionQueueCancelledError.into());
            }
            let elapsed = started.elapsed();
            if elapsed >= timeout {
                return Err(timeout_error(request, &plan, elapsed, false).into());
            }
            if let Some(owned) = self
                .claim_slots_once(request, &plan, cancellation, started, timeout)
                .await?
            {
                return Ok((
                    owned,
                    plan.pool.clone(),
                    plan.resource_class,
                    plan.requested_weight,
                ));
            }
            let elapsed = started.elapsed();
            if elapsed >= timeout {
                return Err(timeout_error(request, &plan, elapsed, false).into());
            }
            cancellable_sleep(
                POLL_INTERVAL.min(timeout.saturating_sub(elapsed)),
                cancellation,
            )
            .await?;
        }
    }

    async fn claim_slots_once(
        &self,
        request: &AcquireRequest,
        plan: &AcquirePlan,
        cancellation: &CancellationToken,
        started: Instant,
        timeout: Duration,
    ) -> Result<Option<Vec<OwnedSlot>>> {
        let mut claim = if plan.requested_weight > 1 {
            match self
                .acquire_claim_lock(&plan.pool, cancellation, started, timeout)
                .await?
            {
                Some(lock) => Some(lock),
                None => {
                    return Err(timeout_error(request, plan, started.elapsed(), true).into());
                }
            }
        } else {
            None
        };

        let mut owned = Vec::with_capacity(plan.requested_weight);
        let mut index = 0_usize;
        while index < plan.usable_slots && owned.len() < plan.requested_weight {
            let path = slot_path(&self.config.root, &plan.pool, index);
            let token = unique_token();
            let slot_owner = SlotOwner {
                token: token.clone(),
                pid: std::process::id(),
                kind: request.kind.as_str().to_owned(),
                pool: plan.pool.clone(),
                resource_class: plan.resource_class.as_str().to_owned(),
                weight: plan.requested_weight,
                label: request.label.clone(),
                acquired_at_utc: utc_now(),
            };
            match create_owner_file(&path, slot_owner).await {
                Ok(()) => owned.push(OwnedSlot { path, token, index }),
                Err(error) if is_already_exists(&error) => {
                    if remove_stale_slot(&path).await? {
                        continue;
                    }
                }
                Err(error) => {
                    release_owned_files(&owned).await.ok();
                    return Err(error);
                }
            }
            index = index.saturating_add(1);
        }

        if let Some(lock) = claim.as_mut()
            && let Err(error) = lock.release().await
        {
            release_owned_files(&owned).await.ok();
            return Err(error);
        }
        if owned.len() == plan.requested_weight {
            Ok(Some(owned))
        } else {
            release_owned_files(&owned).await?;
            Ok(None)
        }
    }

    async fn acquire_claim_lock(
        &self,
        pool: &str,
        cancellation: &CancellationToken,
        started: Instant,
        timeout: Duration,
    ) -> Result<Option<ClaimLock>> {
        let path = self.config.root.join(format!("{pool}-weighted-claim.json"));
        loop {
            if cancellation.is_cancelled() {
                return Err(ExecutionQueueCancelledError.into());
            }
            let elapsed = started.elapsed();
            if elapsed >= timeout {
                return Ok(None);
            }
            let token = unique_token();
            let owner = json!({
                "token": token,
                "pid": std::process::id(),
                "pool": pool,
                "acquiredAtUtc": utc_now(),
            });
            match create_json_new(&path, &owner).await {
                Ok(()) => {
                    return Ok(Some(ClaimLock {
                        path,
                        token: owner["token"].as_str().unwrap_or_default().to_owned(),
                        released: false,
                    }));
                }
                Err(error) if is_already_exists(&error) => {
                    if remove_stale_slot(&path).await? {
                        continue;
                    }
                }
                Err(error) => return Err(error),
            }
            cancellable_sleep(
                CLAIM_RETRY_INTERVAL.min(timeout.saturating_sub(elapsed)),
                cancellation,
            )
            .await?;
        }
    }

    /// Inspect live execution/watch slot occupancy and local scheduler metrics.
    ///
    /// # Errors
    /// Returns an error when the slot directory cannot be read or an owner cannot be inspected.
    pub async fn snapshot(&self) -> Result<ExecutionSlotSnapshot> {
        fs::create_dir_all(&self.config.root).await?;
        let (occupied_slots, watch_slots) = tokio::try_join!(
            read_pool_entries(&self.config.root, "execution"),
            read_pool_entries(&self.config.root, "watch")
        )?;
        Ok(ExecutionSlotSnapshot {
            max_concurrent: self.config.max_concurrent,
            reserved_interactive: self.config.reserved_interactive,
            background_capacity: self
                .config
                .max_concurrent
                .saturating_sub(self.config.reserved_interactive)
                .max(1),
            watch_capacity: self.config.watch_max_concurrent,
            occupied: occupied_slots.len(),
            occupied_slots,
            watch_occupied: watch_slots.len(),
            watch_slots,
            local_process: metrics_snapshot(&self.metrics),
        })
    }
}

#[derive(Debug)]
struct ClaimLock {
    path: PathBuf,
    token: String,
    released: bool,
}

impl ClaimLock {
    async fn release(&mut self) -> Result<()> {
        if self.released {
            return Ok(());
        }
        release_owned_file(&OwnedSlot {
            path: self.path.clone(),
            token: self.token.clone(),
            index: 0,
        })
        .await?;
        self.released = true;
        Ok(())
    }
}

impl Drop for ClaimLock {
    fn drop(&mut self) {
        if !self.released {
            release_owned_file_sync(&OwnedSlot {
                path: self.path.clone(),
                token: self.token.clone(),
                index: 0,
            });
        }
    }
}

fn timeout_error(
    request: &AcquireRequest,
    plan: &AcquirePlan,
    elapsed: Duration,
    weighted_claim: bool,
) -> ExecutionQueueTimeoutError {
    let queue_wait_ms = duration_ms(elapsed);
    let message = if weighted_claim {
        format!(
            "Execution queue remained saturated for {queue_wait_ms} ms while reserving weighted capacity."
        )
    } else {
        format!(
            "Execution queue remained saturated for {queue_wait_ms} ms. Retry shortly or use a detached job for long work."
        )
    };
    ExecutionQueueTimeoutError {
        message,
        details: QueueTimeoutDetails {
            kind: request.kind.as_str().to_owned(),
            label: request.label.clone(),
            pool: plan.pool.clone(),
            resource_class: plan.resource_class.as_str().to_owned(),
            weight: plan.requested_weight,
            queue_wait_ms,
            max_concurrent: plan.total,
            reserved_interactive: plan.reserved,
        },
    }
}

fn pool_for(kind: ExecutionKind, resource_class: ResourceClass) -> String {
    if kind == ExecutionKind::Background && resource_class == ResourceClass::Watch {
        "watch".to_owned()
    } else {
        "execution".to_owned()
    }
}

fn slot_path(root: &Path, pool: &str, index: usize) -> PathBuf {
    let prefix = if pool == "watch" {
        "watch-slot"
    } else {
        "slot"
    };
    root.join(format!("{prefix}-{index:02}.json"))
}

async fn create_owner_file(path: &Path, owner: SlotOwner) -> Result<()> {
    create_json_new(path, &serde_json::to_value(owner)?).await
}

async fn create_json_new(path: &Path, value: &Value) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .await
        .with_context(|| format!("claim execution slot {}", path.display()))?;
    let mut bytes = serde_json::to_vec(value)?;
    bytes.push(b'\n');
    file.write_all(&bytes).await?;
    file.flush().await?;
    Ok(())
}

fn is_already_exists(error: &anyhow::Error) -> bool {
    error.chain().any(|source| {
        source
            .downcast_ref::<std::io::Error>()
            .is_some_and(|io| io.kind() == std::io::ErrorKind::AlreadyExists)
    })
}

async fn remove_stale_slot(path: &Path) -> Result<bool> {
    let owner = match fs::read(path).await {
        Ok(bytes) => serde_json::from_slice::<Value>(&bytes).ok(),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(true),
        Err(error) => return Err(error.into()),
    };
    if let Some(pid) = owner
        .as_ref()
        .and_then(|value| value.get("pid"))
        .and_then(Value::as_u64)
        .and_then(|value| u32::try_from(value).ok())
        && process_alive(pid).await
    {
        return Ok(false);
    }
    if owner.is_none() {
        match fs::metadata(path).await {
            Ok(metadata) => {
                if let Ok(modified) = metadata.modified()
                    && modified.elapsed().unwrap_or_default() < CORRUPT_SLOT_STALE
                {
                    return Ok(false);
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(true),
            Err(error) => return Err(error.into()),
        }
    }
    match fs::remove_file(path).await {
        Ok(()) => Ok(true),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(true),
        Err(error) => Err(error.into()),
    }
}

async fn release_owned_files(owned: &[OwnedSlot]) -> Result<()> {
    let mut errors = Vec::new();
    for entry in owned {
        if let Err(error) = release_owned_file(entry).await {
            errors.push(error.to_string());
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        anyhow::bail!(
            "Failed to release execution-slot file(s): {}",
            errors.join("; ")
        )
    }
}

async fn release_owned_file(owned: &OwnedSlot) -> Result<()> {
    let bytes = match fs::read(&owned.path).await {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };
    let current: Value = serde_json::from_slice(&bytes)?;
    if current.get("token").and_then(Value::as_str) == Some(owned.token.as_str()) {
        match fs::remove_file(&owned.path).await {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(())
}

fn release_owned_file_sync(owned: &OwnedSlot) {
    let Ok(bytes) = std::fs::read(&owned.path) else {
        return;
    };
    let Ok(current) = serde_json::from_slice::<Value>(&bytes) else {
        return;
    };
    if current.get("token").and_then(Value::as_str) == Some(owned.token.as_str()) {
        let _ = std::fs::remove_file(&owned.path);
    }
}

async fn read_pool_entries(root: &Path, pool: &str) -> Result<Vec<Value>> {
    let prefix = if pool == "watch" {
        "watch-slot-"
    } else {
        "slot-"
    };
    let mut reader = match fs::read_dir(root).await {
        Ok(reader) => reader,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(error) => return Err(error.into()),
    };
    let mut entries = Vec::new();
    while let Some(entry) = reader.next_entry().await? {
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(index) = slot_index(&name, prefix) else {
            continue;
        };
        let path = entry.path();
        let Some(mut value) = fs::read(&path)
            .await
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        else {
            continue;
        };
        let alive = match value
            .get("pid")
            .and_then(Value::as_u64)
            .and_then(|pid| u32::try_from(pid).ok())
        {
            Some(pid) => process_alive(pid).await,
            None => false,
        };
        if !alive {
            fs::remove_file(path).await.ok();
            continue;
        }
        if let Some(object) = value.as_object_mut() {
            object.insert("slot".to_owned(), json!(index));
        }
        entries.push((index, value));
    }
    entries.sort_by_key(|(index, _)| *index);
    Ok(entries.into_iter().map(|(_, value)| value).collect())
}

fn slot_index(name: &str, prefix: &str) -> Option<usize> {
    let value = name.strip_prefix(prefix)?.strip_suffix(".json")?;
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    value.parse().ok()
}

async fn cancellable_sleep(duration: Duration, cancellation: &CancellationToken) -> Result<()> {
    tokio::select! {
        () = tokio::time::sleep(duration) => Ok(()),
        () = cancellation.cancelled() => Err(ExecutionQueueCancelledError.into()),
    }
}

fn metrics_snapshot(metrics: &Arc<Mutex<Metrics>>) -> LocalMetricsSnapshot {
    let metrics = lock_metrics(metrics);
    let by_resource_class = metrics
        .by_class
        .iter()
        .map(|(name, value)| {
            (
                name.clone(),
                ResourceMetricsSnapshot {
                    active: value.active,
                    acquired: value.acquired,
                    average_queue_wait_ms: average(value.total_queue_wait_ms, value.acquired),
                    max_queue_wait_ms: value.max_queue_wait_ms,
                },
            )
        })
        .collect();
    LocalMetricsSnapshot {
        queued: metrics.queued,
        active: metrics.active,
        acquired: metrics.acquired,
        timed_out: metrics.timed_out,
        cancelled: metrics.cancelled,
        average_queue_wait_ms: average(metrics.total_queue_wait_ms, metrics.acquired),
        max_queue_wait_ms: metrics.max_queue_wait_ms,
        by_resource_class,
    }
}

fn average(total: u128, count: u64) -> u64 {
    if count == 0 {
        0
    } else {
        u64::try_from(total / u128::from(count)).unwrap_or(u64::MAX)
    }
}

fn lock_metrics(metrics: &Arc<Mutex<Metrics>>) -> std::sync::MutexGuard<'_, Metrics> {
    metrics
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn duration_ms(duration: Duration) -> u64 {
    u64::try_from(duration.as_millis()).unwrap_or(u64::MAX)
}

fn unique_token() -> String {
    static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let counter = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    format!("{:x}-{nanos:x}-{counter:x}", std::process::id())
}

fn utc_now() -> String {
    OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .unwrap_or_else(|_| {
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis()
                .to_string()
        })
}

#[cfg(unix)]
pub(crate) async fn process_alive(pid: u32) -> bool {
    use nix::{errno::Errno, sys::signal, unistd::Pid};
    let Ok(pid) = i32::try_from(pid) else {
        return false;
    };
    match signal::kill(Pid::from_raw(pid), None) {
        Ok(()) | Err(Errno::EPERM) => true,
        Err(_) => false,
    }
}

#[cfg(windows)]
pub(crate) async fn process_alive(pid: u32) -> bool {
    if pid == std::process::id() {
        return true;
    }
    let output = tokio::process::Command::new("tasklist.exe")
        .args(["/fi", &format!("PID eq {pid}"), "/fo", "csv", "/nh"])
        .output()
        .await;
    let Ok(output) = output else {
        return false;
    };
    if !output.status.success() {
        return false;
    }
    String::from_utf8_lossy(&output.stdout).contains(&format!(",\"{pid}\","))
}

#[cfg(not(any(unix, windows)))]
pub(crate) async fn process_alive(_: u32) -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scheduler(root: &Path, max: usize, reserved: usize, watch: usize) -> ExecutionScheduler {
        ExecutionScheduler::new(SchedulerConfig {
            root: root.to_path_buf(),
            max_concurrent: max,
            reserved_interactive: reserved,
            watch_max_concurrent: watch,
            queue_timeout: Duration::from_millis(250),
            heavy_weight: 2,
        })
    }

    #[tokio::test]
    async fn background_respects_reserved_interactive_capacity() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 3, 1, 2);
        let cancellation = CancellationToken::new();
        let mut one = scheduler
            .acquire(
                AcquireRequest::background("one", ResourceClass::Light, 1),
                &cancellation,
            )
            .await
            .unwrap();
        let mut two = scheduler
            .acquire(
                AcquireRequest::background("two", ResourceClass::Light, 1),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(one.slots, vec![0]);
        assert_eq!(two.slots, vec![1]);
        let mut interactive = scheduler
            .acquire(AcquireRequest::interactive("interactive"), &cancellation)
            .await
            .unwrap();
        assert_eq!(interactive.slots, vec![2]);
        let error = scheduler
            .acquire(
                AcquireRequest {
                    queue_timeout: Some(Duration::from_millis(80)),
                    ..AcquireRequest::background("blocked", ResourceClass::Light, 1)
                },
                &cancellation,
            )
            .await
            .unwrap_err();
        assert!(error.downcast_ref::<ExecutionQueueTimeoutError>().is_some());
        interactive.release().await.unwrap();
        two.release().await.unwrap();
        one.release().await.unwrap();
    }

    #[tokio::test]
    async fn heavy_claim_is_atomic_and_uses_multiple_slots() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 4, 1, 2);
        let cancellation = CancellationToken::new();
        let mut heavy = scheduler
            .acquire(
                AcquireRequest::background("heavy", ResourceClass::Heavy, 2),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(heavy.weight, 2);
        assert_eq!(heavy.slots, vec![0, 1]);
        assert_eq!(scheduler.snapshot().await.unwrap().occupied, 2);
        heavy.release().await.unwrap();
        assert_eq!(scheduler.snapshot().await.unwrap().occupied, 0);
    }

    #[tokio::test]
    async fn watch_jobs_use_separate_pool() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 1, 0, 1);
        let cancellation = CancellationToken::new();
        let mut normal = scheduler
            .acquire(
                AcquireRequest::background("normal", ResourceClass::Light, 1),
                &cancellation,
            )
            .await
            .unwrap();
        let mut watch = scheduler
            .acquire(
                AcquireRequest::background("watch", ResourceClass::Watch, 3),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(normal.pool, "execution");
        assert_eq!(watch.pool, "watch");
        assert_eq!(watch.weight, 1);
        let snapshot = scheduler.snapshot().await.unwrap();
        assert_eq!(snapshot.occupied, 1);
        assert_eq!(snapshot.watch_occupied, 1);
        watch.release().await.unwrap();
        normal.release().await.unwrap();
    }

    #[tokio::test]
    async fn queue_wait_is_cancellation_aware() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 1, 0, 1);
        let cancellation = CancellationToken::new();
        let _held = scheduler
            .acquire(AcquireRequest::interactive("held"), &cancellation)
            .await
            .unwrap();
        let waiting = cancellation.child_token();
        let trigger = waiting.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(30)).await;
            trigger.cancel();
        });
        let request = AcquireRequest {
            queue_timeout: Some(Duration::from_secs(5)),
            ..AcquireRequest::interactive("cancelled")
        };
        let error =
            tokio::time::timeout(Duration::from_secs(1), scheduler.acquire(request, &waiting))
                .await
                .expect("cancelled queue wait should finish well before its queue timeout")
                .unwrap_err();
        assert!(
            error
                .downcast_ref::<ExecutionQueueCancelledError>()
                .is_some()
        );
        assert_eq!(
            scheduler.snapshot().await.unwrap().local_process.cancelled,
            1
        );
    }

    #[tokio::test]
    async fn stale_dead_owner_slot_is_reclaimed() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 1, 0, 1);
        let path = slot_path(temp.path(), "execution", 0);
        create_json_new(
            &path,
            &json!({
                "token": "dead",
                "pid": u32::MAX,
                "kind": "interactive",
                "pool": "execution",
                "resourceClass": "light",
                "weight": 1,
                "label": "stale",
                "acquiredAtUtc": utc_now(),
            }),
        )
        .await
        .unwrap();
        let cancellation = CancellationToken::new();
        let mut lease = scheduler
            .acquire(AcquireRequest::interactive("replacement"), &cancellation)
            .await
            .unwrap();
        assert_eq!(lease.slots, vec![0]);
        lease.release().await.unwrap();
    }

    #[tokio::test]
    async fn drop_releases_owned_slot_using_token_guard() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 1, 0, 1);
        let cancellation = CancellationToken::new();
        {
            let _lease = scheduler
                .acquire(AcquireRequest::interactive("drop"), &cancellation)
                .await
                .unwrap();
            assert_eq!(scheduler.snapshot().await.unwrap().occupied, 1);
        }
        assert_eq!(scheduler.snapshot().await.unwrap().occupied, 0);
    }

    #[tokio::test]
    async fn concurrent_heavy_claims_never_exceed_weighted_capacity() {
        use std::sync::atomic::{AtomicUsize, Ordering};

        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 4, 0, 1);
        let active_weight = Arc::new(AtomicUsize::new(0));
        let max_weight = Arc::new(AtomicUsize::new(0));
        let mut handles = Vec::new();
        for index in 0..8 {
            let scheduler = scheduler.clone();
            let active_weight = active_weight.clone();
            let max_weight = max_weight.clone();
            handles.push(tokio::spawn(async move {
                let cancellation = CancellationToken::new();
                let mut lease = scheduler
                    .acquire(
                        AcquireRequest {
                            queue_timeout: Some(Duration::from_secs(2)),
                            ..AcquireRequest::background(
                                format!("heavy-{index}"),
                                ResourceClass::Heavy,
                                2,
                            )
                        },
                        &cancellation,
                    )
                    .await
                    .unwrap();
                let current =
                    active_weight.fetch_add(lease.weight, Ordering::SeqCst) + lease.weight;
                max_weight.fetch_max(current, Ordering::SeqCst);
                tokio::time::sleep(Duration::from_millis(15)).await;
                active_weight.fetch_sub(lease.weight, Ordering::SeqCst);
                lease.release().await.unwrap();
            }));
        }
        for handle in handles {
            handle.await.unwrap();
        }
        assert!(max_weight.load(Ordering::SeqCst) <= 4);
        assert_eq!(scheduler.snapshot().await.unwrap().occupied, 0);
    }
}
