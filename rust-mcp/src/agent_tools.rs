//! Rust-native agent APIs, kept separate from the frozen legacy tool family.
use super::*;
use crate::{atomic_file, job_control, task_store};

pub(super) fn router() -> ToolRouter<DevboxMcp> {
    DevboxMcp::agent_tool_router()
}

pub(super) fn configure_schema(schema: &mut serde_json::Map<String, Value>, config: &Config) {
    let Some(properties) = schema.get_mut("properties").and_then(Value::as_object_mut) else {
        return;
    };
    if properties.get("state") == Some(&Value::Bool(true)) {
        properties.insert("state".into(), json!({}));
    }
    for name in ["path", "task_id", "operation_id", "tool_name"] {
        if let Some(property) = properties.get_mut(name).and_then(Value::as_object_mut) {
            property.insert("minLength".into(), json!(1));
            if matches!(name, "task_id" | "operation_id") {
                property.insert("maxLength".into(), json!(80));
                property.insert("pattern".into(), json!("^[a-z0-9_-]+$"));
            }
        }
    }
    if let Some(property) = properties
        .get_mut("expected_file_sha256")
        .and_then(Value::as_object_mut)
    {
        property.insert("pattern".into(), json!("^(missing|[a-fA-F0-9]{64})$"));
    }
    if let Some(property) = properties
        .get_mut("content_base64")
        .and_then(Value::as_object_mut)
    {
        property.insert(
            "maxLength".into(),
            json!((config.max_mcp_transfer_chars as u64).min(9_007_199_254_740_991)),
        );
    }
    for (name, min, max) in [
        ("limit", 1, 100),
        ("timeout_seconds", 1, 86400),
        ("expected_revision", 0, 9_007_199_254_740_991_u64),
        ("expected_offset_bytes", 0, 9_007_199_254_740_991_u64),
    ] {
        if let Some(property) = properties.get_mut(name).and_then(Value::as_object_mut) {
            property.remove("format");
            property.insert("minimum".into(), json!(min));
            property.insert("maximum".into(), json!(max));
        }
    }
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct FileStateRequest {
    path: String,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct AtomicWriteRequest {
    path: String,
    content_base64: String,
    /// Previous complete file SHA-256, or "missing" for create-only.
    expected_file_sha256: String,
    #[serde(default)]
    append: bool,
    /// Required for retry-safe append, measured before the first attempt.
    expected_offset_bytes: Option<u64>,
    #[serde(default = "default_true")]
    create_dirs: bool,
}

#[derive(Debug, Default, Deserialize, schemars::JsonSchema)]
#[serde(rename_all = "kebab-case")]
enum AgentResource {
    #[default]
    Auto,
    Watch,
    Light,
    Heavy,
    IoHeavy,
}
impl AgentResource {
    fn name(&self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Watch => "watch",
            Self::Light => "light",
            Self::Heavy => "heavy",
            Self::IoHeavy => "io-heavy",
        }
    }
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct SubmitRequest {
    task_id: String,
    operation_id: String,
    #[serde(default)]
    label: String,
    program: Option<String>,
    command: Option<String>,
    #[serde(default)]
    args: Vec<String>,
    input: Option<String>,
    working_dir: Option<String>,
    #[serde(default = "default_async_timeout")]
    timeout_seconds: u64,
    #[serde(default)]
    resource_class: AgentResource,
    #[serde(default)]
    read_only: bool,
}

#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct ListJobsRequest {
    task_id: Option<String>,
    #[serde(default)]
    statuses: Vec<String>,
    cursor: Option<String>,
    #[serde(default = "page_size")]
    limit: usize,
}
#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct TaskGetRequest {
    task_id: String,
}
#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct TaskPutRequest {
    task_id: String,
    expected_revision: u64,
    state: Value,
}
#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct ListTasksRequest {
    cursor: Option<String>,
    #[serde(default = "page_size")]
    limit: usize,
}
#[derive(Debug, Deserialize, schemars::JsonSchema)]
struct CapabilitiesRequest {
    tool_name: Option<String>,
}
const fn page_size() -> usize {
    50
}
fn response(summary: &str, result: anyhow::Result<Value>) -> CallToolResult {
    match result {
        Ok(value) => {
            let text = format!("{summary}\n\n{value}");
            ToolEnvelope::success_with_text(summary, Some(value), text)
        }
        Err(error) => ToolEnvelope::error(error.to_string(), None),
    }
}

#[tool_router(router = agent_tool_router)]
impl DevboxMcp {
    #[tool(name="devbox_file_state", description="Read a complete file digest and length before an atomic checkpoint write. Host runtime only.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_file_state(
        &self,
        Parameters(request): Parameters<FileStateRequest>,
    ) -> CallToolResult {
        let result = async {
            self.agent_host()?;
            let path = self.agent_path(&request.path)?;
            let value = atomic_file::blocking(move || atomic_file::state(&path)).await?;
            Ok(serde_json::to_value(value)?)
        }
        .await;
        response("Read file version.", result)
    }

    #[tool(name="devbox_write_file_atomic", description="Atomically replace a host file after checking its previous SHA-256 (or missing). Append requires the previous offset; identical retries return replayed=true without duplicating bytes. Uses bounded-memory staging; atomic append copies the previous file.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_write_file_atomic(
        &self,
        Parameters(request): Parameters<AtomicWriteRequest>,
    ) -> CallToolResult {
        let result = async {
            self.agent_host()?;
            if request.content_base64.len() > self.config.max_mcp_transfer_chars {
                anyhow::bail!("Payload exceeds transfer limit");
            }
            if request.append && request.expected_offset_bytes.is_none() {
                anyhow::bail!("Atomic append requires expected_offset_bytes");
            }
            let payload = STANDARD.decode(&request.content_base64)?;
            if STANDARD.encode(&payload) != request.content_base64 {
                anyhow::bail!("content_base64 must be canonical");
            }
            let receipt = atomic_file::write(
                self.agent_path(&request.path)?,
                payload,
                request.append,
                request.create_dirs,
                atomic_file::Preconditions {
                    sha256: Some(request.expected_file_sha256),
                    offset: request.expected_offset_bytes,
                },
            )
            .await?;
            Ok(serde_json::to_value(receipt)?)
        }
        .await;
        response("Committed atomic file write.", result)
    }

    #[tool(name="devbox_job_submit", description="Submit a durable host job using a task_id and operation_id. Exactly one of program or command is required. Retry the identical operation after response loss: it returns the existing job, never launches a duplicate. Conflicting reuse is rejected; admission limits bound queued runners.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_job_submit(
        &self,
        Parameters(request): Parameters<SubmitRequest>,
    ) -> CallToolResult {
        let operation = request
            .command
            .as_deref()
            .or(request.program.as_deref())
            .unwrap_or("");
        let class = if let Some(program) = request.program.as_ref() {
            infer_program_resource_class(program, &request.args, request.resource_class.name())
        } else {
            infer_shell_resource_class(operation, request.resource_class.name())
        };
        if let Some(rejection) = self
            .disk_pressure_rejection(
                class,
                request.program.is_none() && request.read_only,
                operation,
            )
            .await
        {
            return rejection;
        }
        let result = async {
            self.agent_host()?;
            if !(1..=86400).contains(&request.timeout_seconds) || request.args.len() > 256 {
                anyhow::bail!("Invalid job timeout or argument count");
            }
            if request.command.is_some() && (!request.args.is_empty() || request.input.is_some()) {
                anyhow::bail!("args/input require a program job");
            }
            let agent = job_control::Submission {
                task_id: request.task_id,
                operation_id: request.operation_id,
                label: request.label,
            };
            let working_dir = request.working_dir.unwrap_or_else(|| {
                self.config
                    .devbox_workspace_path
                    .to_string_lossy()
                    .into_owned()
            });
            let timeout = Duration::from_secs(request.timeout_seconds);
            let resource_class = request.resource_class.name().to_owned();
            match (request.program, request.command) {
                (Some(program), None) if !program.trim().is_empty() => {
                    self.jobs
                        .submit_program(
                            StartProgramJob {
                                program,
                                args: request.args,
                                input: request.input,
                                working_dir,
                                timeout,
                                user: self.config.devbox_default_user.clone(),
                                resource_class,
                            },
                            agent,
                        )
                        .await
                }
                (None, Some(command)) if !command.trim().is_empty() => {
                    self.jobs
                        .submit_shell(
                            crate::job_manager::StartShellJob {
                                command,
                                working_dir,
                                timeout,
                                user: self.config.devbox_default_user.clone(),
                                read_only: request.read_only,
                                resource_class,
                            },
                            agent,
                        )
                        .await
                }
                _ => anyhow::bail!("Exactly one non-empty program or command is required"),
            }
        }
        .await;
        response("Submitted or recovered durable job.", result)
    }

    #[tool(name="devbox_job_list", description="Discover persisted jobs with task/status filters and bounded job-ID pagination. Recover job IDs after reconnecting.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_job_list(
        &self,
        Parameters(request): Parameters<ListJobsRequest>,
    ) -> CallToolResult {
        response(
            "Listed durable jobs.",
            job_control::list(
                self.jobs.store(),
                request.task_id.as_deref(),
                &request.statuses,
                request.cursor.as_deref(),
                request.limit,
            )
            .await,
        )
    }

    #[tool(name="devbox_task_get", description="Read a versioned task checkpoint, including plan, job IDs and artifact references saved by the agent.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_task_get(
        &self,
        Parameters(request): Parameters<TaskGetRequest>,
    ) -> CallToolResult {
        response(
            "Read task checkpoint.",
            task_store::get(&self.task_root(), &request.task_id).await,
        )
    }

    #[tool(name="devbox_task_put", description="Atomically save task state using expected_revision (zero creates a task). Stale updates are rejected; identical retries recover the committed revision. State is data and does not grant execution permissions.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_task_put(
        &self,
        Parameters(request): Parameters<TaskPutRequest>,
    ) -> CallToolResult {
        response(
            "Saved task checkpoint.",
            task_store::put(
                &self.task_root(),
                &request.task_id,
                request.expected_revision,
                request.state,
            )
            .await,
        )
    }

    #[tool(name="devbox_task_list", description="Discover task IDs and revisions after reconnecting, without loading large task states.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_task_list(
        &self,
        Parameters(request): Parameters<ListTasksRequest>,
    ) -> CallToolResult {
        response(
            "Listed task checkpoints.",
            task_store::list(&self.task_root(), request.cursor.as_deref(), request.limit).await,
        )
    }

    #[tool(name="devbox_capabilities", description="Read the current Rust contract version, schema hash, tools and scheduling/admission limits. Supply a tool_name for its authoritative schema to diagnose cached connector definitions.", output_schema=rmcp::handler::server::tool::schema_for_type::<ToolEnvelope>())]
    async fn devbox_capabilities(
        &self,
        Parameters(request): Parameters<CapabilitiesRequest>,
    ) -> CallToolResult {
        let result = if let Some(name) = request.tool_name {
            self.tool_router.get(&name).cloned().map_or_else(
                || Err(anyhow::anyhow!("Unknown tool")),
                |tool| serde_json::to_value(self.configured_tool(tool)).map_err(Into::into),
            )
        } else {
            Ok(self.capability_manifest())
        };
        response("Read native Rust capabilities.", result)
    }
}

impl DevboxMcp {
    fn agent_host(&self) -> anyhow::Result<()> {
        if self.config.runtime_mode != RuntimeMode::Host {
            anyhow::bail!("Durable host APIs require host runtime");
        }
        if !self.config.host_exec_enabled {
            anyhow::bail!("Host execution is disabled");
        }
        Ok(())
    }
    fn agent_path(&self, path: &str) -> anyhow::Result<PathBuf> {
        if path.trim().is_empty() {
            anyhow::bail!("path must not be empty");
        }
        let path = PathBuf::from(path);
        Ok(if path.is_absolute() {
            path
        } else {
            self.config.devbox_workspace_path.join(path)
        })
    }
    fn task_root(&self) -> PathBuf {
        self.config.project_root.join("run").join("tasks")
    }
    pub(super) fn capability_manifest(&self) -> Value {
        let mut tools = self
            .tool_router
            .list_all()
            .into_iter()
            .map(|tool| self.configured_tool(tool))
            .collect::<Vec<_>>();
        tools.sort_by(|a, b| a.name.cmp(&b.name));
        let hash = crate::atomic_file::digest(
            &serde_json::to_vec(&tools).expect("tool schemas serialize"),
        );
        json!({"contract_version":2,"implementation":"rust","schema_sha256":hash,"tools":tools.iter().map(|tool|tool.name.as_ref()).collect::<Vec<_>>(),"resource_classes":["auto","watch","light","heavy","io-heavy"],"limits":{"execution":self.config.exec_max_concurrent,"reserved_interactive":self.config.exec_reserved_interactive,"heavy_capacity":self.config.exec_heavy_capacity,"watch_capacity":self.config.watch_max_concurrent,"active_runners":self.config.job_max_active,"runners_per_task":self.config.job_max_per_task,"operation_receipts":self.config.job_max_operations,"task_state_bytes":task_store::MAX_STATE_BYTES},"build":crate::provenance::snapshot()})
    }
}
