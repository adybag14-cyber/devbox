#[cfg(target_os = "linux")]
use sha2::{Digest, Sha256};

#[must_use]
pub fn current_process_instance() -> Option<u64> {
    process_instance(std::process::id())
}

#[cfg(windows)]
#[must_use]
pub fn process_instance(pid: u32) -> Option<u64> {
    crate::windows_process::process_instance(pid)
}

#[cfg(target_os = "linux")]
#[must_use]
pub fn process_instance(pid: u32) -> Option<u64> {
    let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let close = stat.rfind(')')?;
    let start_ticks = stat.get(close + 1..)?.split_whitespace().nth(19)?;
    let boot_id = std::fs::read_to_string("/proc/sys/kernel/random/boot_id").ok()?;
    let mut digest = Sha256::new();
    digest.update(boot_id.trim().as_bytes());
    digest.update(b":");
    digest.update(start_ticks.as_bytes());
    let bytes = digest.finalize();
    Some(u64::from_le_bytes(bytes[..8].try_into().ok()?))
}

#[cfg(target_os = "macos")]
#[must_use]
/// Return a macOS process start-time identity. `sysinfo` exposes whole-second
/// precision here, so an extremely rare PID reuse inside the same second cannot
/// be distinguished by this token alone; callers still require PID liveness.
pub fn process_instance(pid: u32) -> Option<u64> {
    use sysinfo::{Pid, ProcessesToUpdate, System};
    let pid = Pid::from_u32(pid);
    let mut system = System::new();
    system.refresh_processes(ProcessesToUpdate::Some(&[pid]), false);
    let start = system.process(pid)?.start_time();
    (start > 0).then_some(start)
}

#[cfg(all(unix, not(any(target_os = "linux", target_os = "macos"))))]
#[must_use]
pub fn process_instance(_pid: u32) -> Option<u64> {
    None
}

#[cfg(not(any(windows, unix)))]
#[must_use]
pub fn process_instance(_pid: u32) -> Option<u64> {
    None
}

#[must_use]
pub fn process_matches_instance(pid: u32, expected: Option<u64>) -> bool {
    let observed = process_instance(pid);
    match (expected, observed) {
        (Some(expected), Some(observed)) => expected == observed,
        (Some(_), None) => false,
        (None, Some(_)) => true,
        (None, None) => process_alive_without_identity(pid),
    }
}
fn process_alive_without_identity(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    #[cfg(windows)]
    {
        crate::windows_process::process_alive(pid)
    }
    #[cfg(unix)]
    {
        use nix::{errno::Errno, sys::signal, unistd::Pid};
        let Ok(pid) = i32::try_from(pid) else {
            return false;
        };
        matches!(
            signal::kill(Pid::from_raw(pid), None),
            Ok(()) | Err(Errno::EPERM)
        )
    }
    #[cfg(not(any(windows, unix)))]
    {
        let _ = pid;
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pid_zero_is_never_treated_as_a_live_identity_fallback() {
        assert!(!process_matches_instance(0, None));
    }

    #[test]
    fn current_process_identity_is_stable_on_supported_platforms() {
        #[cfg(any(windows, target_os = "linux", target_os = "macos"))]
        {
            let first = current_process_instance().expect("current process instance");
            let second = current_process_instance().expect("current process instance repeat");
            assert_eq!(first, second);
            assert!(process_matches_instance(std::process::id(), Some(first)));
            assert!(process_matches_instance(std::process::id(), None));
            assert!(!process_matches_instance(
                std::process::id(),
                Some(first.wrapping_add(1))
            ));
        }
    }
}
