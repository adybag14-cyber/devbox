#![allow(unsafe_code)]

//! Native Windows Job Object and process-tree termination primitives.

use std::{
    collections::{HashMap, HashSet},
    io,
};

use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE},
    System::{
        Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW,
            TH32CS_SNAPPROCESS,
        },
        JobObjects::{AssignProcessToJobObject, CreateJobObjectW, TerminateJobObject},
        Threading::{
            OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SET_QUOTA, PROCESS_TERMINATE,
            TerminateProcess,
        },
    },
};

#[derive(Debug)]
pub struct WindowsJob {
    handle: usize,
}

impl WindowsJob {
    /// Assign an existing process to a new Windows Job Object.
    ///
    /// # Errors
    /// Returns the underlying Win32 error if the job/process cannot be opened or assigned.
    pub fn assign(pid: u32) -> io::Result<Self> {
        unsafe {
            let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if job.is_null() {
                return Err(io::Error::last_os_error());
            }
            let process = OpenProcess(
                PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                0,
                pid,
            );
            if process.is_null() {
                let error = io::Error::last_os_error();
                let _ = CloseHandle(job);
                return Err(error);
            }
            let assigned = AssignProcessToJobObject(job, process);
            let assign_error = if assigned == 0 {
                Some(io::Error::last_os_error())
            } else {
                None
            };
            let _ = CloseHandle(process);
            if let Some(error) = assign_error {
                let _ = CloseHandle(job);
                return Err(error);
            }
            Ok(Self {
                handle: job as usize,
            })
        }
    }

    #[must_use]
    pub fn terminate(&self, exit_code: u32) -> bool {
        unsafe { TerminateJobObject(self.handle as HANDLE, exit_code) != 0 }
    }
}

impl Drop for WindowsJob {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.handle as HANDLE);
        }
    }
}

/// Native fallback used if a process could not be assigned to a Job Object.
/// Descendants are terminated before the root to reduce the chance of orphaning children.
pub fn terminate_process_tree_fallback(root_pid: u32, exit_code: u32) {
    let root_created = crate::windows_process::process_instance(root_pid);
    terminate_process_tree_fallback_with_instance(root_pid, root_created, exit_code);
}

/// Native fallback with a caller-captured root creation fingerprint. This is used after
/// Job Object termination, when the root process may already have exited and its PID may recycle.
pub fn terminate_process_tree_fallback_with_instance(
    root_pid: u32,
    root_created: Option<u64>,
    exit_code: u32,
) {
    if root_pid == 0 {
        return;
    }
    let descendants = descendant_pids(root_pid, root_created);
    for pid in descendants
        .into_iter()
        .rev()
        .chain(std::iter::once(root_pid))
    {
        terminate_pid(pid, exit_code);
    }
}

fn terminate_pid(pid: u32, exit_code: u32) {
    unsafe {
        let handle = OpenProcess(PROCESS_TERMINATE, 0, pid);
        if !handle.is_null() {
            let _ = TerminateProcess(handle, exit_code);
            let _ = CloseHandle(handle);
        }
    }
}

fn creation_is_safe(root_created: Option<u64>, child_created: Option<u64>) -> bool {
    root_created.is_none_or(|root| child_created.is_some_and(|child| child >= root))
}

fn descendant_pids(root_pid: u32, root_created: Option<u64>) -> Vec<u32> {
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snapshot == INVALID_HANDLE_VALUE {
            return Vec::new();
        }
        let mut children: HashMap<u32, Vec<u32>> = HashMap::new();
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = u32::try_from(std::mem::size_of::<PROCESSENTRY32W>()).unwrap_or(u32::MAX);
        if Process32FirstW(snapshot, &raw mut entry) != 0 {
            loop {
                children
                    .entry(entry.th32ParentProcessID)
                    .or_default()
                    .push(entry.th32ProcessID);
                if Process32NextW(snapshot, &raw mut entry) == 0 {
                    break;
                }
            }
        }
        let _ = CloseHandle(snapshot);
        let mut seen = HashSet::new();
        let mut stack = vec![root_pid];
        let mut result = Vec::new();
        while let Some(parent) = stack.pop() {
            if let Some(items) = children.get(&parent) {
                for &child in items {
                    let creation_safe = creation_is_safe(
                        root_created,
                        crate::windows_process::process_instance(child),
                    );
                    if child != root_pid && creation_safe && seen.insert(child) {
                        result.push(child);
                        stack.push(child);
                    }
                }
            }
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::creation_is_safe;

    #[test]
    fn descendant_creation_filter_rejects_older_or_unknown_children_when_root_is_known() {
        assert!(creation_is_safe(None, None));
        assert!(creation_is_safe(Some(100), Some(100)));
        assert!(creation_is_safe(Some(100), Some(101)));
        assert!(!creation_is_safe(Some(100), Some(99)));
        assert!(!creation_is_safe(Some(100), None));
    }
}
