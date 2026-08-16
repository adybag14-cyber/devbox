use std::{
    collections::BTreeMap,
    fmt,
    path::{Path, PathBuf},
    sync::{Arc, Mutex, OnceLock},
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

use crate::lifecycle::replace_file_preserving_previous;

const POLL_INTERVAL: Duration = Duration::from_millis(50);
const MAX_QUEUE_POLL_INTERVAL: Duration = Duration::from_millis(500);
const CLAIM_RETRY_INTERVAL: Duration = Duration::from_millis(20);
const CORRUPT_SLOT_STALE: Duration = Duration::from_secs(5 * 60);
const CORRUPT_QUEUE_TICKET_STALE: Duration = Duration::from_secs(5);
const DISK_PRESSURE_STATE_STALE: Duration = Duration::from_secs(180);
const DISK_PRESSURE_CACHE_TTL: Duration = Duration::from_secs(1);
const AGED_BACKGROUND_CACHE_TTL: Duration = Duration::from_millis(250);
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct AgedBackgroundCacheKey {
    root: PathBuf,
    threshold_ms: u64,
    interactive_pool: String,
    candidate_start: usize,
    candidate_end: usize,
    disk_pressure_constrained: bool,
    max_concurrent: usize,
    reserved_interactive: usize,
    watch_max_concurrent: usize,
    heavy_capacity: usize,
    io_heavy_capacity: usize,
}
type AgedBackgroundCache = BTreeMap<AgedBackgroundCacheKey, (Instant, bool)>;
static DISK_PRESSURE_CACHE: OnceLock<Mutex<BTreeMap<PathBuf, (Instant, bool)>>> = OnceLock::new();
static AGED_BACKGROUND_CACHE: OnceLock<Mutex<AgedBackgroundCache>> = OnceLock::new();

fn invalidate_aged_background_cache(root: &Path) {
    let Some(cache) = AGED_BACKGROUND_CACHE.get() else {
        return;
    };
    cache
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .retain(|key, _| key.root != root);
}
#[cfg(windows)]
const WINDOWS_SLOT_IO_RETRY_ATTEMPTS: usize = 100;
#[cfg(windows)]
const WINDOWS_SLOT_IO_RETRY_DELAY: Duration = Duration::from_millis(10);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ResourceClass {
    Watch,
    Light,
    Heavy,
    #[serde(rename = "io-heavy")]
    IoHeavy,
}

impl ResourceClass {
    #[must_use]
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "watch" => Self::Watch,
            "heavy" => Self::Heavy,
            "io-heavy" | "io_heavy" | "ioheavy" => Self::IoHeavy,
            _ => Self::Light,
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Watch => "watch",
            Self::Light => "light",
            Self::Heavy => "heavy",
            Self::IoHeavy => "io-heavy",
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
    pub heavy_capacity: usize,
    pub heavy_weight: usize,
    pub io_heavy_capacity: usize,
    pub io_heavy_weight: usize,
    pub background_priority_age: Duration,
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
        self.heavy_weight = self.heavy_weight.max(1).min(self.max_concurrent);
        self.heavy_capacity = self
            .heavy_capacity
            .max(self.heavy_weight)
            .min(self.max_concurrent);
        self.io_heavy_weight = self.io_heavy_weight.max(1).min(self.max_concurrent);
        self.io_heavy_capacity = self
            .io_heavy_capacity
            .max(self.io_heavy_weight)
            .min(self.max_concurrent);
        self.background_priority_age = self.background_priority_age.max(Duration::from_millis(1));
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
    pub fn interactive_weighted(
        label: impl Into<String>,
        resource_class: ResourceClass,
        weight: usize,
    ) -> Self {
        Self {
            kind: ExecutionKind::Interactive,
            resource_class,
            weight: weight.max(1),
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
    pub heavy_capacity: usize,
    pub io_heavy_capacity: usize,
    pub background_priority_age_ms: u64,
    pub background_capacity: usize,
    pub watch_capacity: usize,
    pub occupied: usize,
    pub occupied_slots: Vec<Value>,
    pub watch_occupied: usize,
    pub watch_slots: Vec<Value>,
    /// Cross-process admission tickets currently waiting for execution/watch capacity.
    pub global_queued: usize,
    pub global_queued_by_class: BTreeMap<String, usize>,
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
    process_instance: Option<String>,
    kind: String,
    pool: String,
    resource_class: String,
    weight: usize,
    label: String,
    acquired_at_utc: String,
}

#[derive(Debug, Clone)]
struct OwnedSlot {
    path: PathBuf,
    token: String,
    index: usize,
}

#[derive(Debug)]
struct QueueTicket {
    root: PathBuf,
    class: String,
    path: PathBuf,
    name: String,
    released: bool,
}

impl QueueTicket {
    async fn release(&mut self) -> Result<()> {
        if self.released {
            return Ok(());
        }
        match remove_slot_file(&self.path).await {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
        if self.class == "execution-background"
            && let Some(root) = self.root.parent()
        {
            invalidate_aged_background_cache(root);
        }
        refresh_queue_head(&self.root, &self.class, Duration::from_secs(1)).await?;
        self.released = true;
        Ok(())
    }
}

impl Drop for QueueTicket {
    fn drop(&mut self) {
        if !self.released {
            schedule_queue_ticket_cleanup(self.root.clone(), self.class.clone(), self.path.clone());
        }
    }
}

#[derive(Debug, Clone)]
struct AcquirePlan {
    pool: String,
    resource_class: ResourceClass,
    total: usize,
    reserved: usize,
    usable_slots: usize,
    requested_weight: usize,
    disk_pressure_constrained: bool,
    protected_low_slots: usize,
}

impl AcquirePlan {
    fn new(
        config: &SchedulerConfig,
        request: &AcquireRequest,
        disk_pressure_constrained: bool,
    ) -> Self {
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
        let base_usable_slots = if request.kind == ExecutionKind::Background && pool == "execution"
        {
            total.saturating_sub(reserved).max(1)
        } else {
            total
        };
        let usable_slots = if pool == "execution" {
            match resource_class {
                ResourceClass::Heavy => {
                    let capacity = if disk_pressure_constrained {
                        config.heavy_capacity.min(request.weight.max(1))
                    } else {
                        config.heavy_capacity
                    };
                    base_usable_slots.min(capacity).max(1)
                }
                ResourceClass::IoHeavy => {
                    let capacity = if disk_pressure_constrained {
                        config.io_heavy_capacity.min(request.weight.max(1))
                    } else {
                        config.io_heavy_capacity
                    };
                    base_usable_slots.min(capacity).max(1)
                }
                ResourceClass::Watch | ResourceClass::Light => base_usable_slots,
            }
        } else {
            base_usable_slots
        };
        let requested_weight = if pool == "watch" {
            1
        } else {
            request.weight.max(1).min(usable_slots)
        };
        let protected_low_slots = if pool == "execution"
            && disk_pressure_constrained
            && resource_class == ResourceClass::Light
        {
            config
                .heavy_weight
                .max(config.io_heavy_weight)
                .min(usable_slots.saturating_sub(1))
        } else {
            0
        };
        Self {
            pool,
            resource_class,
            total,
            reserved,
            usable_slots,
            requested_weight,
            disk_pressure_constrained,
            protected_low_slots,
        }
    }

    fn queue_class(&self, kind: ExecutionKind) -> String {
        if self.pool == "watch" {
            "watch".to_owned()
        } else if kind == ExecutionKind::Interactive && self.disk_pressure_constrained {
            match self.resource_class {
                ResourceClass::Heavy | ResourceClass::IoHeavy => {
                    "execution-interactive-weighted".to_owned()
                }
                ResourceClass::Watch | ResourceClass::Light => {
                    "execution-interactive-light".to_owned()
                }
            }
        } else if kind == ExecutionKind::Interactive {
            "execution-interactive".to_owned()
        } else {
            "execution-background".to_owned()
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
        let owned = std::mem::take(&mut self.owned);
        schedule_owned_cleanup(owned);
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
        let disk_pressure_constrained = disk_pressure_constrained(&self.config.root).await;
        let plan = AcquirePlan::new(&self.config, &request, disk_pressure_constrained);
        let mut ticket = match create_queue_ticket(
            &self.config.root,
            &plan.queue_class(request.kind),
            &request,
            timeout,
        )
        .await
        {
            Ok(ticket) => ticket,
            Err(error) => {
                let mut metrics = lock_metrics(&self.metrics);
                metrics.queued = metrics.queued.saturating_sub(1);
                return Err(error);
            }
        };
        let mut outcome = self
            .acquire_inner(&request, &plan, &ticket, cancellation, started, timeout)
            .await;
        if let Err(cleanup_error) = ticket.release().await {
            if let Ok((owned, _, _, _)) = &outcome {
                release_owned_files(owned).await.ok();
                outcome = Err(cleanup_error);
            } else {
                tracing::warn!(%cleanup_error, "queue ticket cleanup failed after an already-failed acquisition");
            }
        }

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
        plan: &AcquirePlan,
        ticket: &QueueTicket,
        cancellation: &CancellationToken,
        started: Instant,
        timeout: Duration,
    ) -> Result<(Vec<OwnedSlot>, String, ResourceClass, usize)> {
        loop {
            if cancellation.is_cancelled() {
                return Err(ExecutionQueueCancelledError.into());
            }
            let elapsed = started.elapsed();
            if elapsed >= timeout {
                return Err(timeout_error(request, plan, elapsed, false).into());
            }
            if !queue_ticket_is_head(&self.config.root, ticket).await? {
                cancellable_sleep(
                    queue_poll_interval(elapsed).min(timeout.saturating_sub(elapsed)),
                    cancellation,
                )
                .await?;
                continue;
            }
            if request.kind == ExecutionKind::Interactive
                && aged_background_waiter_competes(
                    &self.config,
                    plan,
                    self.config.background_priority_age,
                )
                .await?
            {
                cancellable_sleep(
                    queue_poll_interval(elapsed).min(timeout.saturating_sub(elapsed)),
                    cancellation,
                )
                .await?;
                continue;
            }
            if let Some(owned) = self
                .claim_slots_once(request, plan, cancellation, started, timeout)
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
                return Err(timeout_error(request, plan, elapsed, false).into());
            }
            cancellable_sleep(
                queue_poll_interval(elapsed).min(timeout.saturating_sub(elapsed)),
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
        let mut index = plan.protected_low_slots;
        while index < plan.usable_slots && owned.len() < plan.requested_weight {
            let path = slot_path(&self.config.root, &plan.pool, index);
            let token = unique_token();
            let slot_owner = SlotOwner {
                token: token.clone(),
                pid: std::process::id(),
                process_instance: current_process_instance(),
                kind: request.kind.as_str().to_owned(),
                pool: plan.pool.clone(),
                resource_class: plan.resource_class.as_str().to_owned(),
                weight: plan.requested_weight,
                label: request.label.clone(),
                acquired_at_utc: utc_now(),
            };
            match create_owner_file(&path, slot_owner).await {
                Ok(()) => owned.push(OwnedSlot { path, token, index }),
                Err(error) if is_already_exists(&error) => match remove_stale_slot(&path).await {
                    Ok(true) => continue,
                    Ok(false) => {}
                    Err(error) if is_transient_slot_error(&error) => {}
                    Err(error) => {
                        release_owned_files(&owned).await.ok();
                        return Err(error);
                    }
                },
                Err(error) if is_transient_slot_error(&error) => {}
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
                "processInstance": current_process_instance(),
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
                Err(error) if is_already_exists(&error) => match remove_stale_slot(&path).await {
                    Ok(true) => continue,
                    Ok(false) => {}
                    Err(error) if is_transient_slot_error(&error) => {}
                    Err(error) => return Err(error),
                },
                Err(error) if is_transient_slot_error(&error) => {}
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
        let (occupied_slots, watch_slots, queued) = tokio::try_join!(
            read_pool_entries(&self.config.root, "execution"),
            read_pool_entries(&self.config.root, "watch"),
            read_queue_snapshot(&self.config.root)
        )?;
        let global_queued = queued.values().copied().sum();
        Ok(ExecutionSlotSnapshot {
            max_concurrent: self.config.max_concurrent,
            reserved_interactive: self.config.reserved_interactive,
            heavy_capacity: self.config.heavy_capacity,
            io_heavy_capacity: self.config.io_heavy_capacity,
            background_priority_age_ms: duration_ms(self.config.background_priority_age),
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
            global_queued,
            global_queued_by_class: queued,
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
            schedule_owned_cleanup(vec![OwnedSlot {
                path: self.path.clone(),
                token: self.token.clone(),
                index: 0,
            }]);
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

async fn create_queue_ticket(
    root: &Path,
    class: &str,
    request: &AcquireRequest,
    timeout: Duration,
) -> Result<QueueTicket> {
    let queue_root = root.join("queue");
    fs::create_dir_all(&queue_root).await?;
    let token = unique_token();
    let sequence = next_queue_sequence(&queue_root, class, timeout).await?;
    let queued_at_unix_ms = unix_ms_now();
    let path = queue_root.join(format!("{class}-{sequence:032}-{token}.json"));
    let value = json!({
        "token": token,
        "pid": std::process::id(),
        "processInstance": current_process_instance(),
        "class": class,
        "kind": request.kind.as_str(),
        "resourceClass": request.resource_class.as_str(),
        "weight": request.weight,
        "label": request.label,
        "sequence": sequence.to_string(),
        "queuedAtUnixMs": queued_at_unix_ms,
        "queuedAtUtc": utc_now(),
        "queueTimeoutMs": duration_ms(timeout),
    });
    write_queue_ticket_atomic(&path, &value).await?;
    if class == "execution-background" {
        invalidate_aged_background_cache(root);
    }
    if let Err(error) = refresh_queue_head(&queue_root, class, timeout).await {
        remove_slot_file(&path).await.ok();
        return Err(error);
    }
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or_default()
        .to_owned();
    Ok(QueueTicket {
        root: queue_root,
        class: class.to_owned(),
        path,
        name,
        released: false,
    })
}

async fn acquire_queue_head_lock(
    queue_root: &Path,
    class: &str,
    timeout: Duration,
) -> Result<ClaimLock> {
    let path = queue_root.join(format!(".{class}-head.lock"));
    let deadline = Instant::now()
        + timeout
            .min(Duration::from_secs(5))
            .max(Duration::from_millis(100));
    loop {
        let token = unique_token();
        let owner = json!({
            "token": token,
            "pid": std::process::id(),
            "processInstance": current_process_instance(),
            "class": class,
            "acquiredAtUtc": utc_now(),
        });
        match create_json_new(&path, &owner).await {
            Ok(()) => {
                return Ok(ClaimLock {
                    path,
                    token,
                    released: false,
                });
            }
            Err(error) if is_already_exists(&error) => {
                if remove_stale_slot(&path).await.unwrap_or(false) {
                    continue;
                }
            }
            Err(error) if is_transient_slot_error(&error) => {}
            Err(error) => return Err(error),
        }
        if Instant::now() >= deadline {
            anyhow::bail!("timed out acquiring queue-head lock for {class}");
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

fn queue_sequence_path(queue_root: &Path, class: &str) -> PathBuf {
    queue_root.join(format!(".{class}-sequence.txt"))
}

async fn next_queue_sequence(queue_root: &Path, class: &str, timeout: Duration) -> Result<u128> {
    let mut lock = acquire_queue_head_lock(queue_root, class, timeout).await?;
    let path = queue_sequence_path(queue_root, class);
    let current = fs::read_to_string(&path)
        .await
        .ok()
        .and_then(|value| value.trim().parse::<u128>().ok());
    let seed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    let next = current.unwrap_or(seed).max(seed).saturating_add(1);
    let temporary = queue_root.join(format!(".{class}-sequence.{}.tmp", unique_token()));
    fs::write(&temporary, format!("{next}\n")).await?;
    let write_result = replace_file_preserving_previous(&temporary, &path).await;
    let release_result = lock.release().await;
    match (write_result, release_result) {
        (Ok(()), Ok(())) => Ok(next),
        (Err(error), _) | (Ok(()), Err(error)) => Err(error),
    }
}

fn unix_ms_now() -> u64 {
    u64::try_from(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis(),
    )
    .unwrap_or(u64::MAX)
}

fn queue_head_path(queue_root: &Path, class: &str) -> PathBuf {
    queue_root.join(format!(".{class}-head.json"))
}

async fn read_queue_head(queue_root: &Path, class: &str) -> Result<Option<String>> {
    let path = queue_head_path(queue_root, class);
    match fs::read(&path).await {
        Ok(bytes) => Ok(serde_json::from_slice::<Value>(&bytes)
            .ok()
            .and_then(|value| value.get("name").and_then(Value::as_str).map(str::to_owned))),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.into()),
    }
}

async fn write_queue_head(queue_root: &Path, class: &str, name: Option<&str>) -> Result<()> {
    let path = queue_head_path(queue_root, class);
    let Some(name) = name else {
        match fs::remove_file(&path).await {
            Ok(()) => return Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(error) => return Err(error.into()),
        }
    };
    let temporary = queue_root.join(format!(".{class}-head.{}.tmp", unique_token()));
    let bytes = serde_json::to_vec(&json!({ "name": name }))?;
    fs::write(&temporary, bytes).await?;
    replace_file_preserving_previous(&temporary, &path).await
}

async fn refresh_queue_head(
    queue_root: &Path,
    class: &str,
    timeout: Duration,
) -> Result<Option<String>> {
    let mut lock = acquire_queue_head_lock(queue_root, class, timeout).await?;
    let result = async {
        let prefix = format!("{class}-");
        let mut reader = match fs::read_dir(queue_root).await {
            Ok(reader) => reader,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                return Ok::<Option<String>, anyhow::Error>(None);
            }
            Err(error) => return Err(error.into()),
        };
        let mut head: Option<String> = None;
        while let Some(entry) = reader.next_entry().await? {
            let name = entry.file_name().to_string_lossy().into_owned();
            if !name.starts_with(&prefix)
                || !Path::new(&name)
                    .extension()
                    .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
            {
                continue;
            }
            if head.as_ref().is_none_or(|current| name < *current) {
                head = Some(name);
            }
        }
        write_queue_head(queue_root, class, head.as_deref()).await?;
        Ok(head)
    }
    .await;
    let release_result = lock.release().await;
    match (result, release_result) {
        (Ok(value), Ok(())) => Ok(value),
        (Err(error), _) | (Ok(_), Err(error)) => Err(error),
    }
}
async fn queue_ticket_is_head(_root: &Path, ticket: &QueueTicket) -> Result<bool> {
    let prefix = format!("{}-", ticket.class);
    loop {
        let Some(head_name) = read_queue_head(&ticket.root, &ticket.class).await? else {
            refresh_queue_head(&ticket.root, &ticket.class, Duration::from_secs(1)).await?;
            continue;
        };
        if head_name == ticket.name {
            return Ok(true);
        }
        let head_path = ticket.root.join(&head_name);
        let owner = fs::read(&head_path)
            .await
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok());
        if owner.is_none() {
            let fresh = fs::metadata(&head_path)
                .await
                .ok()
                .and_then(|metadata| metadata.modified().ok())
                .and_then(|modified| modified.elapsed().ok())
                .is_some_and(|age| age < CORRUPT_QUEUE_TICKET_STALE);
            if fresh {
                return Ok(false);
            }
            remove_slot_file(&head_path).await.ok();
            refresh_queue_head(&ticket.root, &ticket.class, Duration::from_secs(1)).await?;
            continue;
        }
        let owner = owner.expect("queue head owner checked above");
        if queue_ticket_should_reap(&head_name, &prefix, &owner) {
            remove_slot_file(&head_path).await.ok();
            refresh_queue_head(&ticket.root, &ticket.class, Duration::from_secs(1)).await?;
            continue;
        }
        return Ok(false);
    }
}

async fn aged_background_waiter_competes(
    config: &SchedulerConfig,
    interactive_plan: &AcquirePlan,
    threshold: Duration,
) -> Result<bool> {
    let normalized = config.clone().normalized();
    let threshold_ms = duration_ms(threshold);
    let candidate_start = interactive_plan.protected_low_slots;
    let candidate_end = interactive_plan.usable_slots;
    let key = AgedBackgroundCacheKey {
        root: normalized.root.clone(),
        threshold_ms,
        interactive_pool: interactive_plan.pool.clone(),
        candidate_start,
        candidate_end,
        disk_pressure_constrained: interactive_plan.disk_pressure_constrained,
        max_concurrent: normalized.max_concurrent,
        reserved_interactive: normalized.reserved_interactive,
        watch_max_concurrent: normalized.watch_max_concurrent,
        heavy_capacity: normalized.heavy_capacity,
        io_heavy_capacity: normalized.io_heavy_capacity,
    };
    let cache = AGED_BACKGROUND_CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
    {
        let values = cache
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some((sampled, result)) = values.get(&key)
            && sampled.elapsed() < AGED_BACKGROUND_CACHE_TTL
        {
            return Ok(*result);
        }
    }
    let result =
        aged_background_waiter_competes_uncached(&normalized, interactive_plan, threshold).await?;
    cache
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .insert(key, (Instant::now(), result));
    Ok(result)
}

fn slot_ranges_overlap(
    first_start: usize,
    first_end: usize,
    second_start: usize,
    second_end: usize,
) -> bool {
    first_start < second_end && second_start < first_end
}

async fn aged_background_waiter_competes_uncached(
    config: &SchedulerConfig,
    interactive_plan: &AcquirePlan,
    threshold: Duration,
) -> Result<bool> {
    let queue_root = config.root.join("queue");
    let mut reader = match fs::read_dir(&queue_root).await {
        Ok(reader) => reader,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(error.into()),
    };
    let prefix = "execution-background-";
    let now_ms = unix_ms_now();
    let threshold_ms = duration_ms(threshold);
    while let Some(entry) = reader.next_entry().await? {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !name.starts_with(prefix)
            || !Path::new(&name)
                .extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
        {
            continue;
        }
        let Some(owner) = fs::read(entry.path())
            .await
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        else {
            continue;
        };
        if queue_ticket_should_reap(&name, prefix, &owner) {
            continue;
        }
        let queued_ms = owner
            .get("queuedAtUnixMs")
            .and_then(Value::as_u64)
            .unwrap_or(now_ms);
        if now_ms.saturating_sub(queued_ms) < threshold_ms {
            continue;
        }
        let resource_class = ResourceClass::parse(
            owner
                .get("resourceClass")
                .and_then(Value::as_str)
                .unwrap_or("light"),
        );
        let weight = owner
            .get("weight")
            .and_then(Value::as_u64)
            .and_then(|value| usize::try_from(value).ok())
            .unwrap_or(1)
            .max(1);
        let background_request =
            AcquireRequest::background("aged-background-probe", resource_class, weight);
        let background_plan = AcquirePlan::new(
            config,
            &background_request,
            interactive_plan.disk_pressure_constrained,
        );
        if background_plan.pool == interactive_plan.pool
            && slot_ranges_overlap(
                interactive_plan.protected_low_slots,
                interactive_plan.usable_slots,
                background_plan.protected_low_slots,
                background_plan.usable_slots,
            )
        {
            return Ok(true);
        }
    }
    Ok(false)
}

async fn disk_pressure_constrained(root: &Path) -> bool {
    let cache = DISK_PRESSURE_CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
    {
        let values = cache
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some((sampled, constrained)) = values.get(root)
            && sampled.elapsed() < DISK_PRESSURE_CACHE_TTL
        {
            return *constrained;
        }
    }
    let path = root.join(".disk-pressure.json");
    let constrained = if let Ok(metadata) = fs::metadata(&path).await
        && let Ok(modified) = metadata.modified()
        && let Ok(age) = SystemTime::now().duration_since(modified)
        && age <= DISK_PRESSURE_STATE_STALE
        && let Ok(raw) = fs::read(&path).await
        && let Ok(value) = serde_json::from_slice::<Value>(&raw)
    {
        matches!(
            value.get("diskPressure").and_then(Value::as_str),
            Some("warning" | "critical")
        )
    } else {
        false
    };
    cache
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .insert(root.to_path_buf(), (Instant::now(), constrained));
    constrained
}

fn queue_poll_interval(elapsed: Duration) -> Duration {
    if elapsed < Duration::from_secs(1) {
        POLL_INTERVAL
    } else if elapsed < Duration::from_secs(5) {
        Duration::from_millis(100)
    } else if elapsed < Duration::from_secs(30) {
        Duration::from_millis(250)
    } else {
        MAX_QUEUE_POLL_INTERVAL
    }
}

fn current_process_instance() -> Option<String> {
    crate::process_identity::current_process_instance().map(|value| value.to_string())
}

fn owner_process_instance(owner: &Value) -> Option<u64> {
    owner.get("processInstance").and_then(|value| {
        value
            .as_u64()
            .or_else(|| value.as_str().and_then(|text| text.parse::<u64>().ok()))
    })
}

fn owner_process_alive(owner: &Value) -> bool {
    let Some(pid) = owner
        .get("pid")
        .and_then(Value::as_u64)
        .and_then(|v| u32::try_from(v).ok())
    else {
        return false;
    };
    crate::process_identity::process_matches_instance(pid, owner_process_instance(owner))
}

fn queue_ticket_should_reap(name: &str, prefix: &str, owner: &Value) -> bool {
    let alive = owner_process_alive(owner);
    if !alive {
        return true;
    }
    // Modern identity-bearing tickets are governed by the waiter's monotonic
    // deadline and explicit Drop cleanup. Never reap a verified live owner
    // solely because the wall clock jumped forward. Legacy identity-less
    // tickets retain the old timestamp expiry as a compatibility escape hatch.
    let has_identity = owner
        .get("processInstance")
        .is_some_and(|value| !value.is_null());
    !has_identity && queue_ticket_expired(name, prefix, owner)
}

fn queue_ticket_expired(name: &str, prefix: &str, owner: &Value) -> bool {
    let timeout_ms = owner
        .get("queueTimeoutMs")
        .and_then(Value::as_u64)
        .unwrap_or_else(|| duration_ms(CORRUPT_SLOT_STALE));
    let now_ms = unix_ms_now();
    let queued_ms = owner
        .get("queuedAtUnixMs")
        .and_then(Value::as_u64)
        .or_else(|| {
            name.strip_prefix(prefix)
                .and_then(|rest| rest.split('-').next())
                .and_then(|value| value.parse::<u128>().ok())
                .and_then(|timestamp_ns| u64::try_from(timestamp_ns / 1_000_000).ok())
        });
    queued_ms.is_some_and(|queued_ms| {
        now_ms.saturating_sub(queued_ms) > timeout_ms.saturating_add(1_000)
    })
}

async fn write_queue_ticket_atomic(path: &Path, value: &Value) -> Result<()> {
    let parent = path.parent().context("queue ticket path has no parent")?;
    let file_name = path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("ticket");
    let temporary = parent.join(format!(".{file_name}.{}.tmp", unique_token()));
    let mut bytes = serde_json::to_vec(value)?;
    bytes.push(b'\n');
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .await?;
    file.write_all(&bytes).await?;
    file.flush().await?;
    drop(file);
    if let Err(error) = fs::rename(&temporary, path).await {
        fs::remove_file(&temporary).await.ok();
        return Err(error.into());
    }
    Ok(())
}

async fn read_queue_snapshot(root: &Path) -> Result<BTreeMap<String, usize>> {
    let queue_root = root.join("queue");
    let mut result = BTreeMap::new();
    let mut reader = match fs::read_dir(&queue_root).await {
        Ok(reader) => reader,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(result),
        Err(error) => return Err(error.into()),
    };
    while let Some(entry) = reader.next_entry().await? {
        let name = entry.file_name().to_string_lossy().into_owned();
        if !Path::new(&name)
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
        {
            continue;
        }
        let path = entry.path();
        let owner = fs::read(&path)
            .await
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok());
        let class = owner
            .as_ref()
            .and_then(|owner| owner.get("class").and_then(Value::as_str))
            .filter(|class| {
                *class == "watch"
                    || class.starts_with("execution-interactive")
                    || class.starts_with("execution-background")
            });
        let Some(class) = class else {
            continue;
        };
        let prefix = format!("{class}-");
        let valid = owner
            .as_ref()
            .is_some_and(|owner| !queue_ticket_should_reap(&name, &prefix, owner));
        if valid {
            *result.entry(class.to_owned()).or_insert(0) += 1;
        } else if owner.is_some() {
            remove_slot_file(&path).await.ok();
        }
    }
    Ok(result)
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
    let mut file = open_new_slot_file(path)
        .await
        .with_context(|| format!("claim execution slot {}", path.display()))?;
    let mut bytes = serde_json::to_vec(value)?;
    bytes.push(b'\n');
    file.write_all(&bytes).await?;
    file.flush().await?;
    Ok(())
}

async fn open_new_slot_file(path: &Path) -> std::io::Result<tokio::fs::File> {
    #[cfg(windows)]
    {
        let mut last_error = None;
        for attempt in 0..WINDOWS_SLOT_IO_RETRY_ATTEMPTS {
            match OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(path)
                .await
            {
                Ok(file) => return Ok(file),
                Err(error) if is_transient_slot_io(&error) => {
                    last_error = Some(error);
                    if attempt + 1 < WINDOWS_SLOT_IO_RETRY_ATTEMPTS {
                        tokio::time::sleep(WINDOWS_SLOT_IO_RETRY_DELAY).await;
                    }
                }
                Err(error) => return Err(error),
            }
        }
        Err(last_error.expect("Windows slot retry loop records a transient error"))
    }
    #[cfg(not(windows))]
    {
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .await
    }
}

#[cfg(windows)]
fn is_transient_slot_io(error: &std::io::Error) -> bool {
    error.kind() == std::io::ErrorKind::PermissionDenied
}

fn is_transient_slot_error(error: &anyhow::Error) -> bool {
    #[cfg(windows)]
    {
        error.chain().any(|source| {
            source
                .downcast_ref::<std::io::Error>()
                .is_some_and(is_transient_slot_io)
        })
    }
    #[cfg(not(windows))]
    {
        let _ = error;
        false
    }
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
    if owner.as_ref().is_some_and(owner_process_alive) {
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
    match remove_slot_file(path).await {
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
        match remove_slot_file(&owned.path).await {
            Ok(()) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
    }
    Ok(())
}

async fn remove_slot_file(path: &Path) -> std::io::Result<()> {
    #[cfg(windows)]
    {
        let mut last_error = None;
        for attempt in 0..WINDOWS_SLOT_IO_RETRY_ATTEMPTS {
            match fs::remove_file(path).await {
                Ok(()) => return Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Err(error),
                Err(error) if is_transient_slot_io(&error) => {
                    last_error = Some(error);
                    if attempt + 1 < WINDOWS_SLOT_IO_RETRY_ATTEMPTS {
                        tokio::time::sleep(WINDOWS_SLOT_IO_RETRY_DELAY).await;
                    }
                }
                Err(error) => return Err(error),
            }
        }
        Err(last_error.expect("Windows slot retry loop records a transient error"))
    }
    #[cfg(not(windows))]
    {
        fs::remove_file(path).await
    }
}

fn schedule_owned_cleanup(owned: Vec<OwnedSlot>) {
    if owned.is_empty() {
        return;
    }
    if let Ok(handle) = tokio::runtime::Handle::try_current() {
        handle.spawn(async move {
            if let Err(error) = release_owned_files(&owned).await {
                tracing::warn!(%error, "asynchronous execution-slot Drop cleanup failed; stale-slot reconciliation will retry later");
            }
        });
        return;
    }
    for entry in &owned {
        release_owned_file_sync_once(entry);
    }
}

fn schedule_queue_ticket_cleanup(queue_root: PathBuf, class: String, path: PathBuf) {
    if let Ok(handle) = tokio::runtime::Handle::try_current() {
        handle.spawn(async move {
            if let Err(error) = remove_slot_file(&path).await
                && error.kind() != std::io::ErrorKind::NotFound
            {
                tracing::warn!(path = %path.display(), %error, "asynchronous queue-ticket Drop cleanup failed");
            }
            if let Err(error) = refresh_queue_head(&queue_root, &class, Duration::from_secs(1)).await {
                tracing::warn!(%error, class = %class, "failed to promote execution queue head after ticket cleanup");
            }
        });
        return;
    }
    let _ = std::fs::remove_file(path);
}

fn release_owned_file_sync_once(owned: &OwnedSlot) {
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
        let alive = owner_process_alive(&value);
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
pub(crate) fn process_alive(pid: u32) -> std::future::Ready<bool> {
    std::future::ready(crate::windows_process::process_alive(pid))
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
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_secs(30),
        })
    }

    #[cfg(windows)]
    #[test]
    fn windows_slot_retry_is_limited_to_transient_permission_denied() {
        assert!(is_transient_slot_io(&std::io::Error::from(
            std::io::ErrorKind::PermissionDenied
        )));
        assert!(!is_transient_slot_io(&std::io::Error::from(
            std::io::ErrorKind::AlreadyExists
        )));
        assert!(!is_transient_slot_io(&std::io::Error::from(
            std::io::ErrorKind::NotFound
        )));
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

    #[test]
    fn live_identity_ticket_is_not_reaped_by_wall_clock_expiry() {
        let prefix = "execution-background-";
        let name = "execution-background-00000000000000000000000000000001-fixture.json";
        let live = json!({
            "pid": std::process::id(),
            "processInstance": current_process_instance(),
            "queuedAtUnixMs": 1,
            "queueTimeoutMs": 1,
        });
        assert!(!queue_ticket_should_reap(name, prefix, &live));
        let legacy = json!({
            "pid": std::process::id(),
            "processInstance": null,
            "queuedAtUnixMs": 1,
            "queueTimeoutMs": 1,
        });
        assert!(queue_ticket_should_reap(name, prefix, &legacy));
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
    async fn dropped_queue_head_lock_is_released_by_nonblocking_raii_cleanup() {
        let temp = tempfile::tempdir().unwrap();
        let queue_root = temp.path().join("queue");
        fs::create_dir_all(&queue_root).await.unwrap();
        let lock_path = queue_root.join(".fixture-head.lock");
        {
            let _lock = acquire_queue_head_lock(&queue_root, "fixture", Duration::from_secs(1))
                .await
                .unwrap();
            assert!(lock_path.exists());
        }
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline && lock_path.exists() {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert!(!lock_path.exists(), "dropped queue-head lock was stranded");
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
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline && scheduler.snapshot().await.unwrap().occupied != 0 {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert_eq!(scheduler.snapshot().await.unwrap().occupied, 0);
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn drop_cleanup_never_blocks_tokio_on_windows_file_sharing_retry() {
        use std::os::windows::fs::OpenOptionsExt as _;

        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 1, 0, 1);
        let cancellation = CancellationToken::new();
        let lease = scheduler
            .acquire(AcquireRequest::interactive("drop-locked"), &cancellation)
            .await
            .unwrap();
        let slot_path = lease.owned[0].path.clone();
        let locked = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .share_mode(0x1 | 0x2)
            .open(&slot_path)
            .unwrap();
        let started = Instant::now();
        drop(lease);
        assert!(
            started.elapsed() < Duration::from_millis(50),
            "Drop blocked a Tokio worker while Windows denied slot deletion"
        );
        drop(locked);
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline && slot_path.exists() {
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert!(
            !slot_path.exists(),
            "async Drop cleanup never reclaimed the slot"
        );
    }

    #[tokio::test]
    async fn disk_pressure_light_request_bypasses_blocked_weighted_waiter_outside_corridor() {
        let temp = tempfile::tempdir().unwrap();
        fs::write(
            temp.path().join(".disk-pressure.json"),
            br#"{"diskPressure":"warning"}"#,
        )
        .await
        .unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 4,
            reserved_interactive: 1,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_secs(1),
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_secs(30),
        });
        let cancellation = CancellationToken::new();
        let mut first = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("pressure-first", ResourceClass::Heavy, 2),
                &cancellation,
            )
            .await
            .unwrap();
        let weighted_scheduler = scheduler.clone();
        let weighted_cancel = cancellation.clone();
        let weighted = tokio::spawn(async move {
            weighted_scheduler
                .acquire(
                    AcquireRequest::interactive_weighted(
                        "pressure-second",
                        ResourceClass::Heavy,
                        2,
                    ),
                    &weighted_cancel,
                )
                .await
        });
        tokio::time::sleep(Duration::from_millis(40)).await;
        let mut light = tokio::time::timeout(
            Duration::from_millis(400),
            scheduler.acquire(AcquireRequest::interactive("pressure-light"), &cancellation),
        )
        .await
        .expect("light request should bypass pressure-blocked weighted waiter")
        .unwrap();
        assert!(light.slot.is_some_and(|slot| slot >= 2));
        assert!(!weighted.is_finished());
        light.release().await.unwrap();
        first.release().await.unwrap();
        let mut weighted = tokio::time::timeout(Duration::from_secs(1), weighted)
            .await
            .expect("weighted waiter should resume")
            .unwrap()
            .unwrap();
        assert_eq!(weighted.slots, vec![0, 1]);
        weighted.release().await.unwrap();
    }

    #[tokio::test]
    async fn warning_disk_pressure_serializes_heavy_claims() {
        let temp = tempfile::tempdir().unwrap();
        fs::write(
            temp.path().join(".disk-pressure.json"),
            br#"{"diskPressure":"warning"}"#,
        )
        .await
        .unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 4,
            reserved_interactive: 1,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_millis(150),
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_secs(30),
        });
        let cancellation = CancellationToken::new();
        let mut first = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("pressure-first", ResourceClass::Heavy, 2),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(first.slots, vec![0, 1]);
        let second = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("pressure-second", ResourceClass::Heavy, 2),
                &cancellation,
            )
            .await;
        assert!(
            second.is_err(),
            "warning pressure should serialize heavy claims"
        );
        first.release().await.unwrap();
    }

    #[tokio::test]
    async fn aged_background_cache_is_scoped_to_capacity_configuration() {
        let temp = tempfile::tempdir().unwrap();
        fs::write(
            temp.path().join(".disk-pressure.json"),
            br#"{"diskPressure":"warning"}"#,
        )
        .await
        .unwrap();
        let narrow = SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 4,
            reserved_interactive: 1,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_secs(2),
            heavy_capacity: 2,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_millis(1),
        }
        .normalized();
        let wide = SchedulerConfig {
            heavy_capacity: 3,
            ..narrow.clone()
        }
        .normalized();
        let background_request =
            AcquireRequest::background("capacity-cache-fixture", ResourceClass::Heavy, 3);
        let mut ticket = create_queue_ticket(
            &narrow.root,
            "execution-background",
            &background_request,
            Duration::from_secs(2),
        )
        .await
        .unwrap();
        tokio::time::sleep(Duration::from_millis(5)).await;
        let interactive = AcquireRequest::interactive("capacity-cache-interactive");
        let narrow_plan = AcquirePlan::new(&narrow, &interactive, true);
        let wide_plan = AcquirePlan::new(&wide, &interactive, true);
        assert_eq!(narrow_plan.protected_low_slots, 2);
        assert_eq!(wide_plan.protected_low_slots, 2);
        assert!(
            !aged_background_waiter_competes(&narrow, &narrow_plan, Duration::from_millis(1),)
                .await
                .unwrap()
        );
        assert!(
            aged_background_waiter_competes(&wide, &wide_plan, Duration::from_millis(1),)
                .await
                .unwrap()
        );
        ticket.release().await.unwrap();
    }

    #[tokio::test]
    async fn aged_background_from_different_pool_does_not_block_interactive_execution() {
        let temp = tempfile::tempdir().unwrap();
        let config = SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 4,
            reserved_interactive: 1,
            watch_max_concurrent: 2,
            queue_timeout: Duration::from_secs(2),
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_millis(1),
        }
        .normalized();
        let malformed_watch =
            AcquireRequest::background("legacy-watch-fixture", ResourceClass::Watch, 1);
        let mut ticket = create_queue_ticket(
            &config.root,
            "execution-background",
            &malformed_watch,
            Duration::from_secs(2),
        )
        .await
        .unwrap();
        tokio::time::sleep(Duration::from_millis(5)).await;
        let interactive = AcquireRequest::interactive("execution-interactive");
        let plan = AcquirePlan::new(&config, &interactive, false);
        assert_eq!(plan.pool, "execution");
        assert!(
            !aged_background_waiter_competes(&config, &plan, Duration::from_millis(1),)
                .await
                .unwrap()
        );
        ticket.release().await.unwrap();
    }

    #[tokio::test]
    async fn disk_pressure_light_interactive_bypasses_non_overlapping_aged_heavy_background() {
        let temp = tempfile::tempdir().unwrap();
        fs::write(
            temp.path().join(".disk-pressure.json"),
            br#"{"diskPressure":"warning"}"#,
        )
        .await
        .unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 4,
            reserved_interactive: 1,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_secs(2),
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_millis(1),
        });
        let cancellation = CancellationToken::new();
        let mut first = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("pressure-holder", ResourceClass::Heavy, 2),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(first.slots, vec![0, 1]);

        let background_scheduler = scheduler.clone();
        let background_cancel = cancellation.clone();
        let background = tokio::spawn(async move {
            background_scheduler
                .acquire(
                    AcquireRequest::background("aged-heavy-background", ResourceClass::Heavy, 2),
                    &background_cancel,
                )
                .await
        });
        tokio::time::sleep(Duration::from_millis(30)).await;

        let mut light = tokio::time::timeout(
            Duration::from_millis(400),
            scheduler.acquire(
                AcquireRequest::interactive("pressure-light-after-aged-heavy"),
                &cancellation,
            ),
        )
        .await
        .expect("light request should not yield to a non-overlapping aged heavy waiter")
        .unwrap();
        assert!(light.slot.is_some_and(|slot| slot >= 2));
        assert!(!background.is_finished());
        light.release().await.unwrap();
        first.release().await.unwrap();

        let mut background_lease = tokio::time::timeout(Duration::from_secs(1), background)
            .await
            .expect("aged heavy background should resume after corridor release")
            .unwrap()
            .unwrap();
        assert_eq!(background_lease.slots, vec![0, 1]);
        background_lease.release().await.unwrap();
    }

    #[tokio::test]
    async fn aged_background_waiter_temporarily_blocks_new_interactive_claims() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 1,
            reserved_interactive: 0,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_secs(2),
            heavy_capacity: 1,
            heavy_weight: 1,
            io_heavy_capacity: 1,
            io_heavy_weight: 1,
            background_priority_age: Duration::from_millis(1),
        });
        let cancellation = CancellationToken::new();
        let mut blocker = scheduler
            .acquire(AcquireRequest::interactive("aging-blocker"), &cancellation)
            .await
            .unwrap();
        let background_scheduler = scheduler.clone();
        let background_cancel = cancellation.clone();
        let background = tokio::spawn(async move {
            background_scheduler
                .acquire(
                    AcquireRequest::background("aged-background", ResourceClass::Light, 1),
                    &background_cancel,
                )
                .await
        });
        tokio::time::sleep(Duration::from_millis(30)).await;
        let interactive_scheduler = scheduler.clone();
        let interactive_cancel = cancellation.clone();
        let interactive = tokio::spawn(async move {
            interactive_scheduler
                .acquire(
                    AcquireRequest::interactive("later-interactive"),
                    &interactive_cancel,
                )
                .await
        });
        blocker.release().await.unwrap();
        let mut background_lease = tokio::time::timeout(Duration::from_secs(1), background)
            .await
            .expect("aged background should acquire")
            .unwrap()
            .unwrap();
        tokio::time::sleep(Duration::from_millis(40)).await;
        assert!(
            !interactive.is_finished(),
            "later interactive claim bypassed aged background waiter"
        );
        background_lease.release().await.unwrap();
        let mut interactive_lease = tokio::time::timeout(Duration::from_secs(1), interactive)
            .await
            .expect("interactive should resume after aged background")
            .unwrap()
            .unwrap();
        interactive_lease.release().await.unwrap();
    }

    #[tokio::test]
    async fn io_heavy_capacity_serializes_storage_work_without_consuming_all_slots() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 4,
            reserved_interactive: 1,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_millis(500),
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_secs(30),
        });
        let cancellation = CancellationToken::new();
        let mut first = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("io-first", ResourceClass::IoHeavy, 2),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(first.weight, 2);
        assert_eq!(scheduler.snapshot().await.unwrap().io_heavy_capacity, 2);
        let second = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("io-second", ResourceClass::IoHeavy, 2),
                &cancellation,
            )
            .await;
        assert!(
            second.is_err(),
            "second io-heavy workload should be serialized by class capacity"
        );
        first.release().await.unwrap();
    }

    #[tokio::test]
    async fn interactive_heavy_request_consumes_weighted_capacity() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = scheduler(temp.path(), 4, 1, 2);
        let cancellation = CancellationToken::new();
        let mut lease = scheduler
            .acquire(
                AcquireRequest::interactive_weighted("sync-heavy", ResourceClass::Heavy, 2),
                &cancellation,
            )
            .await
            .unwrap();
        assert_eq!(lease.kind, ExecutionKind::Interactive);
        assert_eq!(lease.resource_class, ResourceClass::Heavy);
        assert_eq!(lease.weight, 2);
        assert_eq!(lease.slots.len(), 2);
        assert_eq!(scheduler.snapshot().await.unwrap().occupied, 2);
        lease.release().await.unwrap();
    }

    #[tokio::test]
    async fn expired_queue_head_is_reclaimed_even_when_pid_is_alive() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let queue_root = root.join("queue");
        fs::create_dir_all(&queue_root).await.unwrap();
        let stale = queue_root.join(format!("execution-background-{:032}-stale.json", 1_u128));
        fs::write(
            &stale,
            serde_json::to_vec(&json!({
                "pid": std::process::id(),
                "queueTimeoutMs": 100,
                "queuedAtUnixMs": unix_ms_now().saturating_sub(5_000),
            }))
            .unwrap(),
        )
        .await
        .unwrap();
        let request = AcquireRequest::background("later", ResourceClass::Light, 1);
        let ticket = create_queue_ticket(
            root,
            "execution-background",
            &request,
            Duration::from_secs(1),
        )
        .await
        .unwrap();
        assert!(queue_ticket_is_head(root, &ticket).await.unwrap());
        assert!(!stale.exists(), "expired queue head should be reclaimed");
    }

    #[tokio::test]
    async fn fifo_ticket_prevents_later_light_job_from_overtaking_heavy_waiter() {
        let temp = tempfile::tempdir().unwrap();
        let scheduler = ExecutionScheduler::new(SchedulerConfig {
            root: temp.path().to_path_buf(),
            max_concurrent: 2,
            reserved_interactive: 0,
            watch_max_concurrent: 1,
            queue_timeout: Duration::from_secs(2),
            heavy_capacity: 4,
            heavy_weight: 2,
            io_heavy_capacity: 2,
            io_heavy_weight: 2,
            background_priority_age: Duration::from_secs(30),
        });
        let mut blocker = scheduler
            .acquire(
                AcquireRequest::background("blocker", ResourceClass::Light, 1),
                &CancellationToken::new(),
            )
            .await
            .unwrap();
        let heavy_scheduler = scheduler.clone();
        let heavy = tokio::spawn(async move {
            heavy_scheduler
                .acquire(
                    AcquireRequest::background("heavy-first", ResourceClass::Heavy, 2),
                    &CancellationToken::new(),
                )
                .await
        });
        tokio::time::sleep(Duration::from_millis(40)).await;
        let light_scheduler = scheduler.clone();
        let light = tokio::spawn(async move {
            light_scheduler
                .acquire(
                    AcquireRequest::background("light-later", ResourceClass::Light, 1),
                    &CancellationToken::new(),
                )
                .await
        });
        tokio::time::sleep(Duration::from_millis(40)).await;
        blocker.release().await.unwrap();
        let mut heavy_lease = tokio::time::timeout(Duration::from_secs(1), heavy)
            .await
            .expect("heavy waiter should acquire first")
            .unwrap()
            .unwrap();
        assert_eq!(heavy_lease.weight, 2);
        assert!(
            !light.is_finished(),
            "later light request overtook FIFO heavy waiter"
        );
        heavy_lease.release().await.unwrap();
        let mut light_lease = tokio::time::timeout(Duration::from_secs(1), light)
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        light_lease.release().await.unwrap();
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn windows_process_liveness_uses_native_probe() {
        assert!(process_alive(std::process::id()).await);
        let snapshot = crate::windows_process::metrics_snapshot();
        assert_eq!(snapshot["backend"], "win32-openprocess");
        assert!(snapshot["count"].as_u64().unwrap_or_default() > 0);
    }

    #[tokio::test]
    async fn rust_waiter_honors_a_live_javascript_protocol_ticket() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path();
        let scheduler = scheduler(root, 1, 0, 1);
        let queue_root = root.join("queue");
        fs::create_dir_all(&queue_root).await.unwrap();
        let queue_class = "execution-background";
        let sequence = next_queue_sequence(&queue_root, queue_class, Duration::from_secs(1))
            .await
            .unwrap();
        let name = format!("{queue_class}-{sequence:032}-jsfixture.json");
        let path = queue_root.join(&name);
        write_queue_ticket_atomic(
            &path,
            &json!({
                "token": "jsfixture",
                "pid": std::process::id(),
                "processInstance": null,
                "class": queue_class,
                "kind": "background",
                "resourceClass": "light",
                "weight": 1,
                "label": "javascript-protocol-fixture",
                "sequence": sequence.to_string(),
                "queuedAtUnixMs": unix_ms_now(),
                "queuedAtUtc": utc_now(),
                "queueTimeoutMs": 2_000,
            }),
        )
        .await
        .unwrap();
        refresh_queue_head(&queue_root, queue_class, Duration::from_secs(1))
            .await
            .unwrap();

        let error = scheduler
            .acquire(
                AcquireRequest {
                    kind: ExecutionKind::Background,
                    resource_class: ResourceClass::Light,
                    weight: 1,
                    label: "rust-later".to_owned(),
                    queue_timeout: Some(Duration::from_millis(120)),
                },
                &CancellationToken::new(),
            )
            .await
            .expect_err("Rust waiter must not overtake the live JS FIFO head");
        assert!(error.downcast_ref::<ExecutionQueueTimeoutError>().is_some());
        remove_slot_file(&path).await.unwrap();
        refresh_queue_head(&queue_root, queue_class, Duration::from_secs(1))
            .await
            .unwrap();
        let mut lease = scheduler
            .acquire(
                AcquireRequest::background("rust-after-js", ResourceClass::Light, 1),
                &CancellationToken::new(),
            )
            .await
            .unwrap();
        lease.release().await.unwrap();
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
                            queue_timeout: Some(Duration::from_secs(10)),
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
