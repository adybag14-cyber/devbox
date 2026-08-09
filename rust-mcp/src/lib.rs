pub mod capture;
pub mod config;
pub mod contract;
pub mod docker_files;
pub mod execution;
pub mod files;
pub mod github_auth;
pub mod host_inspect;
pub mod job_logs;
pub mod job_manager;
pub mod job_runner;
pub mod jobs;
pub mod lifecycle;
pub mod oauth;
pub mod output;
pub mod performance;
pub mod process;
pub mod result;
pub mod runtime;
pub mod search;
pub mod server;

pub use config::{AuthMode, Config, Platform, RuntimeMode};
pub use contract::{IMPLEMENTED_TOOL_NAMES, ParityReport, TARGET_TOOL_NAMES};
pub use server::{DevboxMcp, build_router};

#[cfg(windows)]
pub mod windows_capture;
pub mod windows_shell;

#[cfg(any(not(windows), test))]
pub mod posix_capture;
