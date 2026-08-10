use serde::Serialize;

pub const TARGET_TOOL_NAMES: &[&str] = &[
    "devbox_github_auth_status",
    "devbox_sync_github_auth_from_host",
    "devbox_status",
    "devbox_start",
    "devbox_stop",
    "devbox_restart",
    "devbox_recreate",
    "devbox_exec_readonly",
    "devbox_exec",
    "devbox_run_program",
    "devbox_exec_start",
    "devbox_run_program_start",
    "devbox_job_status",
    "devbox_job_logs",
    "devbox_job_cancel",
    "devbox_wait",
    "devbox_wait_for_file",
    "devbox_list_files",
    "devbox_read_file",
    "devbox_read_large_file",
    "devbox_write_file",
    "devbox_write_large_file",
    "devbox_search_files",
    "host_status",
    "windows_host_status",
    "host_capture_display",
    "host_capture_window",
    "host_capture_program",
    "windows_host_capture_display",
    "windows_host_capture_program",
    "host_exec",
    "windows_host_inspect_file",
    "windows_host_read_large_file",
    "windows_host_write_large_file",
    "windows_host_exec",
    "host_run_program",
    "windows_host_run_program",
];

pub const IMPLEMENTED_TOOL_NAMES: &[&str] = &[
    "devbox_status",
    "devbox_wait",
    "devbox_wait_for_file",
    "host_status",
    "windows_host_status",
    "windows_host_read_large_file",
    "windows_host_write_large_file",
    "devbox_run_program",
    "devbox_run_program_start",
    "devbox_job_status",
    "devbox_job_logs",
    "devbox_job_cancel",
    "devbox_exec",
    "devbox_exec_readonly",
    "devbox_exec_start",
    "devbox_list_files",
    "devbox_read_file",
    "devbox_read_large_file",
    "devbox_write_file",
    "devbox_write_large_file",
    "devbox_search_files",
    "host_exec",
    "windows_host_exec",
    "host_run_program",
    "windows_host_run_program",
    "windows_host_inspect_file",
    "devbox_start",
    "devbox_stop",
    "devbox_restart",
    "devbox_recreate",
    "devbox_github_auth_status",
    "devbox_sync_github_auth_from_host",
    "host_capture_display",
    "host_capture_window",
    "host_capture_program",
    "windows_host_capture_display",
    "windows_host_capture_program",
];

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct ParityReport {
    pub target_count: usize,
    pub implemented_count: usize,
    pub missing_count: usize,
    pub implemented: Vec<&'static str>,
    pub missing: Vec<&'static str>,
    pub complete: bool,
}

impl ParityReport {
    #[must_use]
    pub fn current() -> Self {
        let missing = TARGET_TOOL_NAMES
            .iter()
            .copied()
            .filter(|name| !IMPLEMENTED_TOOL_NAMES.contains(name))
            .collect::<Vec<_>>();
        let complete = missing.is_empty();
        Self {
            target_count: TARGET_TOOL_NAMES.len(),
            implemented_count: IMPLEMENTED_TOOL_NAMES.len(),
            missing_count: missing.len(),
            implemented: IMPLEMENTED_TOOL_NAMES.to_vec(),
            missing,
            complete,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;

    #[test]
    fn target_contract_has_37_unique_tools() {
        assert_eq!(TARGET_TOOL_NAMES.len(), 37);
        assert_eq!(
            TARGET_TOOL_NAMES
                .iter()
                .copied()
                .collect::<BTreeSet<_>>()
                .len(),
            37
        );
    }

    #[test]
    fn implemented_tools_are_unique() {
        assert_eq!(
            IMPLEMENTED_TOOL_NAMES
                .iter()
                .copied()
                .collect::<BTreeSet<_>>()
                .len(),
            IMPLEMENTED_TOOL_NAMES.len()
        );
    }

    #[test]
    fn implemented_tools_are_a_subset_of_target_contract() {
        assert!(
            IMPLEMENTED_TOOL_NAMES
                .iter()
                .all(|name| TARGET_TOOL_NAMES.contains(name))
        );
    }
}
