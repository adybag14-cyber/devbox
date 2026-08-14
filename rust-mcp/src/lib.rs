pub mod allocator_metrics;
pub mod background;
pub mod capture;
pub mod config;
pub mod contract;
pub mod docker_files;
pub mod execution;
pub mod files;
pub mod gateway;
pub mod github_auth;
pub mod hex;
pub mod host_inspect;
mod incident_task;
pub mod job_logs;
pub mod job_manager;
pub mod job_runner;
pub mod jobs;
pub mod lifecycle;
pub mod oauth;
pub mod output;
pub mod performance;
pub mod process;
mod process_identity;
pub mod provenance;
mod quota_task;
pub mod request_control;
pub mod result;
pub mod runtime;
pub mod schema_parity;
pub mod search;
pub mod server;
pub mod usage;

pub use config::{AuthMode, Config, Platform, RuntimeMode};
pub use contract::{IMPLEMENTED_TOOL_NAMES, ParityReport, TARGET_TOOL_NAMES};
pub use server::{DevboxMcp, build_router};

#[cfg(windows)]
pub mod windows_capture;
#[cfg(windows)]
pub mod windows_job;
#[cfg(windows)]
pub mod windows_process;
pub mod windows_shell;

#[cfg(any(not(windows), test))]
pub mod posix_capture;
