//! Durable submission identity, admission and bounded discovery for the Rust runtime.
use crate::{Config, atomic_file, jobs::JobStore};
use anyhow::{Context, Result, bail};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    fs::OpenOptions,
    path::Path,
    time::{Duration, Instant},
};
use tokio::{fs, io::AsyncReadExt};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct Submission {
    pub task_id: String,
    pub operation_id: String,
    pub label: String,
}

pub(crate) fn validate_key(value: &str) -> Result<()> {
    if value.is_empty()
        || value.len() > 80
        || !value
            .bytes()
            .all(|c| (c.is_ascii_lowercase() || c.is_ascii_digit()) || matches!(c, b'-' | b'_'))
    {
        bail!(
            "Task/operation IDs must be 1-80 lowercase ASCII letters, digits, hyphens or underscores"
        );
    }
    Ok(())
}

pub(crate) fn terminal(value: &Value) -> bool {
    matches!(
        value.get("status").and_then(Value::as_str),
        Some("succeeded" | "failed" | "cancelled" | "timed_out" | "interrupted")
    )
}

pub(crate) async fn gate(root: &Path) -> Result<std::fs::File> {
    fs::create_dir_all(root).await?;
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join(".submission.lock"))?;
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        match file.try_lock_exclusive() {
            Ok(()) => return Ok(file),
            Err(error)
                if error.kind() == std::io::ErrorKind::WouldBlock
                    || error.raw_os_error().is_some_and(|code| {
                        Some(code) == fs2::lock_contended_error().raw_os_error()
                    }) => {}
            Err(error) => return Err(error.into()),
        }
        if Instant::now() >= deadline {
            bail!("SUBMISSION_BUSY: retry with the same operation ID");
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

pub(crate) async fn ids(root: &Path) -> Result<Vec<String>> {
    let mut reader = match fs::read_dir(root).await {
        Ok(reader) => reader,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e.into()),
    };
    let mut ids = Vec::new();
    while let Some(entry) = reader.next_entry().await? {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with("job-") && entry.file_type().await?.is_dir() {
            if ids.len() >= 10000 {
                bail!(
                    "Job store exceeds the bounded discovery budget; reconcile retention before submitting more work"
                );
            }
            ids.push(name);
        }
    }
    ids.sort();
    Ok(ids)
}

pub(crate) async fn admit(store: &JobStore, config: &Config, task: Option<&str>) -> Result<()> {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
    tokio::time::timeout_at(deadline, async {
        let mut active = 0; let mut task_active = 0;
        for id in ids(&config.jobs_root).await? {
            let status = store.get_status(&id).await.context("admission requires readable job state")?;
            if !terminal(&status) {
                active += 1;
                if let Some(task) = task { let request = store.read_request(&id).await?; if request.pointer("/agent/taskId").and_then(Value::as_str) == Some(task) { task_active += 1; } }
            }
            if active >= config.job_max_active || task.is_some() && task_active >= config.job_max_per_task { bail!("JOB_CAPACITY: active or queued runner limit reached; retry the same operation after a job completes"); }
        }
        Ok(())
    }).await.context("Job admission inspection exceeded its deadline")?
}

pub(crate) async fn list(
    store: &JobStore,
    task: Option<&str>,
    status_filter: &[String],
    cursor: Option<&str>,
    limit: usize,
) -> Result<Value> {
    if let Some(task) = task {
        validate_key(task)?;
    }
    if !(1..=100).contains(&limit) {
        bail!("limit must be between 1 and 100");
    }
    let mut jobs = Vec::new();
    let mut next_cursor = None;
    for id in ids(&store.config().root).await? {
        if cursor.is_some_and(|cursor| id.as_str() <= cursor) {
            continue;
        }
        let request = store.read_request(&id).await?;
        if task.is_some_and(|task| {
            request.pointer("/agent/taskId").and_then(Value::as_str) != Some(task)
        }) {
            continue;
        }
        let status = store.get_status(&id).await?;
        if !status_filter.is_empty()
            && !status_filter
                .iter()
                .any(|filter| status.get("status").and_then(Value::as_str) == Some(filter))
        {
            continue;
        }
        if jobs.len() == limit {
            next_cursor = jobs.last().and_then(|v: &Value| v.get("id")).cloned();
            break;
        }
        let mut summary = json!({});
        for key in [
            "id",
            "status",
            "createdAtUtc",
            "startedAtUtc",
            "completedAtUtc",
            "exitCode",
            "runnerAlive",
        ] {
            if let Some(value) = status.get(key) {
                summary[key] = value.clone();
            }
        }
        if let Some(agent) = request.get("agent") {
            summary["agent"] = agent.clone();
        }
        jobs.push(summary);
    }
    Ok(json!({"jobs": jobs, "next_cursor": next_cursor, "order": "job_id", "limit": limit}))
}

pub(crate) async fn read_receipt(root: &Path, id: &str) -> Result<Option<Value>> {
    match fs::File::open(root.join(".operations").join(format!("{id}.json"))).await {
        Ok(file) => {
            let mut bytes = Vec::new();
            file.take(65537).read_to_end(&mut bytes).await?;
            if bytes.len() > 65536 {
                bail!("Operation receipt exceeds size limit");
            }
            Ok(Some(serde_json::from_slice(&bytes)?))
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.into()),
    }
}

pub(crate) async fn write_receipt(root: &Path, id: &str, value: &Value) -> Result<()> {
    atomic_file::write(
        root.join(".operations").join(format!("{id}.json")),
        serde_json::to_vec(value)?,
        false,
        true,
        atomic_file::Preconditions::default(),
    )
    .await?;
    Ok(())
}

pub(crate) async fn check_receipt_capacity(config: &Config) -> Result<()> {
    let mut reader = match fs::read_dir(config.jobs_root.join(".operations")).await {
        Ok(reader) => reader,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => return Err(e.into()),
    };
    let mut count = 0;
    while reader.next_entry().await?.is_some() {
        count += 1;
        if count >= config.job_max_operations {
            bail!(
                "OPERATION_CAPACITY: durable operation receipts are full; archive the task records explicitly before accepting new operation IDs"
            );
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn global_and_task_admission_limits_reject_before_spawning() {
        let root = tempfile::tempdir().unwrap();
        let mut config = crate::config::test_config(root.path());
        config.job_max_active = 2;
        config.job_max_per_task = 1;
        let store = JobStore::new(crate::job_manager::job_store_config(&config));
        store.create_job("job-pending-fixture",&json!({"agent":{"taskId":"task-a"}}),&json!({"id":"job-pending-fixture","status":"queued","createdAtUtc":chrono::Utc::now().to_rfc3339()})).await.unwrap();
        assert!(admit(&store, &config, Some("task-a")).await.is_err());
        admit(&store, &config, Some("task-b")).await.unwrap();
        config.job_max_active = 1;
        assert!(admit(&store, &config, None).await.is_err());
        let page = list(&store, Some("task-a"), &[], None, 1).await.unwrap();
        assert_eq!(page["jobs"][0]["id"], "job-pending-fixture");
    }
}
