//! Versioned task checkpoints. State is data; it never grants execution permission.
use crate::{
    atomic_file::{self, Preconditions},
    job_control::validate_key,
};
use anyhow::{Result, bail};
use serde_json::{Value, json};
use std::path::Path;
use tokio::{fs, io::AsyncReadExt};

pub(crate) const MAX_STATE_BYTES: usize = 65536;

pub(crate) async fn get(root: &Path, id: &str) -> Result<Value> {
    validate_key(id)?;
    match fs::File::open(root.join(format!("{id}.json"))).await {
        Ok(file) => {
            let mut bytes = Vec::new();
            file.take((MAX_STATE_BYTES + 2049) as u64)
                .read_to_end(&mut bytes)
                .await?;
            if bytes.len() > MAX_STATE_BYTES + 2048 {
                bail!("Task record exceeds size limit");
            }
            let record: Value = serde_json::from_slice(&bytes)?;
            Ok(json!({"exists":true,"record":record,"sha256":atomic_file::digest(&bytes)}))
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            Ok(json!({"exists":false,"task_id":id,"revision":0}))
        }
        Err(error) => Err(error.into()),
    }
}

pub(crate) async fn put(root: &Path, id: &str, revision: u64, state: Value) -> Result<Value> {
    validate_key(id)?;
    if revision >= 9_007_199_254_740_991 {
        bail!("Task revision exceeds the interoperable integer range");
    }
    if serde_json::to_vec(&state)?.len() > MAX_STATE_BYTES {
        bail!("Task state exceeds 65536 bytes; store large artifacts separately");
    }
    let _gate = crate::job_control::gate(root).await?;
    let current = get(root, id).await?;
    let current_revision = current
        .pointer("/record/revision")
        .and_then(Value::as_u64)
        .unwrap_or(0);
    if current_revision == revision + 1 && current.pointer("/record/state") == Some(&state) {
        return Ok(json!({"replayed":true,"record":current["record"],"sha256":current["sha256"]}));
    }
    if current_revision != revision {
        bail!("TASK_CONFLICT: expected_revision is stale");
    }
    if current["exists"] != true && task_ids(root).await?.len() >= 10000 {
        bail!("TASK_CAPACITY: archive completed task records before creating more");
    }
    let record = json!({"schema_version":1,"task_id":id,"revision":revision+1,"updated_at":chrono::Utc::now().to_rfc3339(),"state":state});
    let expected = current["sha256"].as_str().unwrap_or("missing").to_owned();
    let receipt = atomic_file::write(
        root.join(format!("{id}.json")),
        serde_json::to_vec(&record)?,
        false,
        true,
        Preconditions {
            sha256: Some(expected),
            offset: None,
        },
    )
    .await?;
    Ok(json!({"replayed":receipt.replayed,"record":record,"sha256":receipt.current.sha256}))
}

async fn task_ids(root: &Path) -> Result<Vec<String>> {
    let mut reader = match fs::read_dir(root).await {
        Ok(reader) => reader,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(e) => return Err(e.into()),
    };
    let mut ids = Vec::new();
    while let Some(entry) = reader.next_entry().await? {
        let name = entry.file_name().to_string_lossy().into_owned();
        if let Some(id) = name.strip_suffix(".json")
            && validate_key(id).is_ok()
        {
            if ids.len() >= 10000 {
                bail!("Task index exceeds its scan budget");
            }
            ids.push(id.to_owned());
        }
    }
    ids.sort();
    Ok(ids)
}

pub(crate) async fn list(root: &Path, cursor: Option<&str>, limit: usize) -> Result<Value> {
    if !(1..=100).contains(&limit) {
        bail!("limit must be between 1 and 100");
    }
    let ids = task_ids(root).await?;
    let ids = ids
        .into_iter()
        .filter(|id| cursor.is_none_or(|cursor| id.as_str() > cursor))
        .collect::<Vec<_>>();
    let next = if ids.len() > limit {
        Some(ids[limit - 1].clone())
    } else {
        None
    };
    let mut records = Vec::new();
    for id in ids.into_iter().take(limit) {
        let value = get(root, &id).await?;
        if value["exists"] == true {
            records.push(json!({"task_id":id,"revision":value["record"]["revision"],"updated_at":value["record"]["updated_at"]}));
        }
    }
    Ok(json!({"tasks":records,"next_cursor":next}))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn task_revision_retries_and_conflicts_are_durable() {
        let root = tempfile::tempdir().unwrap();
        let first = put(
            root.path(),
            "task-a",
            0,
            json!({"step":"build","job_id":"job-existing"}),
        )
        .await
        .unwrap();
        assert_eq!(first["record"]["revision"], 1);
        assert_eq!(
            put(
                root.path(),
                "task-a",
                0,
                json!({"step":"build","job_id":"job-existing"})
            )
            .await
            .unwrap()["replayed"],
            true
        );
        assert!(
            put(root.path(), "task-a", 0, json!({"step":"overwrite"}))
                .await
                .is_err()
        );
        assert_eq!(
            get(root.path(), "task-a").await.unwrap()["record"]["state"]["job_id"],
            "job-existing"
        );
        assert_eq!(
            list(root.path(), None, 1).await.unwrap()["tasks"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
        assert!(get(root.path(), "../escape").await.is_err());
    }
}
