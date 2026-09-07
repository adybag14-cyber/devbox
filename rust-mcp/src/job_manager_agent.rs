use super::{JobManager, JobRequest, Result, StartProgramJob, StartShellJob, json};
use crate::{
    atomic_file,
    job_control::{self, Submission},
};
use serde_json::Value;

impl JobManager {
    pub(crate) async fn submit_program(
        &self,
        options: StartProgramJob,
        agent: Submission,
    ) -> Result<Value> {
        self.submit_request(self.program_request(options), agent)
            .await
    }

    pub(crate) async fn submit_shell(
        &self,
        options: StartShellJob,
        agent: Submission,
    ) -> Result<Value> {
        self.submit_request(self.shell_request(options), agent)
            .await
    }

    async fn submit_request(&self, mut request: JobRequest, agent: Submission) -> Result<Value> {
        job_control::validate_key(&agent.task_id)?;
        job_control::validate_key(&agent.operation_id)?;
        if agent.label.len() > 200 {
            anyhow::bail!("label exceeds 200 bytes");
        }
        let key = serde_json::to_vec(&(&agent.task_id, &agent.operation_id))?;
        request.id = format!("job-op-{}", atomic_file::digest(&key));
        let mut value = serde_json::to_value(&request)?;
        value
            .as_object_mut()
            .expect("serialized job object")
            .remove("createdAtUtc");
        value["agent"] = serde_json::to_value(&agent)?;
        let fingerprint = atomic_file::digest(&serde_json::to_vec(&canonical(value))?);
        let _gate = job_control::gate(&self.config.jobs_root).await?;
        if let Some(receipt) =
            job_control::read_receipt(&self.config.jobs_root, &request.id).await?
        {
            if receipt["fingerprint"] != fingerprint {
                anyhow::bail!(
                    "OPERATION_CONFLICT: operation ID already belongs to a different request"
                );
            }
            let status = match self.store.get_status(&request.id).await {
                Ok(value) => value,
                Err(error) if self.store.paths(&request.id)?.dir.exists() => {
                    return Err(error.context(
                        "JOB_STATE_UNAVAILABLE: retained operation will not be executed again",
                    ));
                }
                Err(_) => {
                    json!({"id":request.id,"status":if receipt["submitted"]==true {"result_expired"} else {"submission_unknown"},"message":"Operation ID is retained and will not be executed again. Inspect the task before choosing a new operation ID."})
                }
            };
            return Ok(json!({"id":request.id,"replayed":true,"agent":agent,"job":status}));
        }
        job_control::check_receipt_capacity(&self.config).await?;
        job_control::admit(&self.store, &self.config, Some(&agent.task_id)).await?;
        let mut receipt = json!({"id":request.id,"agent":agent,"fingerprint":fingerprint,"submitted":false,"createdAtUtc":request.created_at_utc});
        job_control::write_receipt(&self.config.jobs_root, &request.id, &receipt).await?;
        let id = request.id.clone();
        let summary = self
            .persist_and_spawn_unlocked(request, Some(&agent))
            .await?;
        receipt["submitted"] = json!(true);
        job_control::write_receipt(&self.config.jobs_root, &id, &receipt).await?;
        Ok(json!({"id":id,"replayed":false,"agent":agent,"job":summary}))
    }
}

// Object field order must not change operation identity when code is refactored.
fn canonical(value: Value) -> Value {
    match value {
        Value::Object(object) => Value::Object(
            object
                .into_iter()
                .collect::<std::collections::BTreeMap<_, _>>()
                .into_iter()
                .map(|(key, value)| (key, canonical(value)))
                .collect(),
        ),
        Value::Array(array) => Value::Array(array.into_iter().map(canonical).collect()),
        value => value,
    }
}
