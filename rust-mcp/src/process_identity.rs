#![cfg_attr(target_os = "macos", allow(unsafe_code))]

#[cfg(target_os = "linux")]
use sha2::{Digest, Sha256};
#[cfg(target_os = "linux")]
use std::sync::OnceLock;

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
    let boot_id = linux_boot_id()?;
    let mut digest = Sha256::new();
    digest.update(boot_id.as_bytes());
    digest.update(b":");
    digest.update(start_ticks.as_bytes());
    let bytes = digest.finalize();
    Some(u64::from_le_bytes(bytes[..8].try_into().ok()?))
}

#[cfg(target_os = "linux")]
fn linux_boot_id() -> Option<&'static str> {
    static BOOT_ID: OnceLock<Option<String>> = OnceLock::new();
    BOOT_ID
        .get_or_init(|| {
            std::fs::read_to_string("/proc/sys/kernel/random/boot_id")
                .ok()
                .map(|value| value.trim().to_owned())
                .filter(|value| !value.is_empty())
        })
        .as_deref()
}

#[cfg(target_os = "macos")]
#[repr(C)]
struct ProcBsdInfo {
    pbi_flags: u32,
    pbi_status: u32,
    pbi_xstatus: u32,
    pbi_pid: u32,
    pbi_ppid: u32,
    pbi_uid: u32,
    pbi_gid: u32,
    pbi_ruid: u32,
    pbi_rgid: u32,
    pbi_svuid: u32,
    pbi_svgid: u32,
    rfu_1: u32,
    pbi_comm: [std::os::raw::c_char; 16],
    pbi_name: [std::os::raw::c_char; 32],
    pbi_nfiles: u32,
    pbi_pgid: u32,
    pbi_pjobc: u32,
    e_tdev: u32,
    e_tpgid: u32,
    pbi_nice: i32,
    pbi_start_tvsec: u64,
    pbi_start_tvusec: u64,
}

#[cfg(target_os = "macos")]
const _: [(); 136] = [(); std::mem::size_of::<ProcBsdInfo>()];

#[cfg(target_os = "macos")]
#[link(name = "proc")]
unsafe extern "C" {
    fn proc_pidinfo(
        pid: std::os::raw::c_int,
        flavor: std::os::raw::c_int,
        arg: u64,
        buffer: *mut std::ffi::c_void,
        buffersize: std::os::raw::c_int,
    ) -> std::os::raw::c_int;
}

#[cfg(target_os = "macos")]
#[must_use]
/// Return a macOS process identity from `proc_pidinfo(PROC_PIDTBSDINFO)`.
/// The kernel supplies start time to microsecond precision, avoiding the
/// whole-second PID-reuse ambiguity of the previous `sysinfo` fallback.
pub fn process_instance(pid: u32) -> Option<u64> {
    const PROC_PIDTBSDINFO: std::os::raw::c_int = 3;
    let pid = std::os::raw::c_int::try_from(pid).ok()?;
    let mut info = std::mem::MaybeUninit::<ProcBsdInfo>::zeroed();
    let size = std::os::raw::c_int::try_from(std::mem::size_of::<ProcBsdInfo>()).ok()?;
    let read = unsafe {
        proc_pidinfo(
            pid,
            PROC_PIDTBSDINFO,
            0,
            info.as_mut_ptr().cast::<std::ffi::c_void>(),
            size,
        )
    };
    if read < size {
        return None;
    }
    let info = unsafe { info.assume_init() };
    (info.pbi_start_tvsec > 0).then(|| {
        info.pbi_start_tvsec
            .saturating_mul(1_000_000)
            .saturating_add(info.pbi_start_tvusec.min(999_999))
    })
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
