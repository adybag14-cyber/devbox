use std::{
    env,
    path::PathBuf,
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

fn git_optional(args: &[&str]) -> Option<String> {
    Command::new("git")
        .args(args)
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|value| !value.is_empty())
}

fn git(args: &[&str]) -> String {
    git_optional(args).unwrap_or_else(|| "unknown".to_owned())
}

fn emit_git_rerun_path(git_path: &str) {
    if let Some(value) = git_optional(&["rev-parse", "--git-path", git_path]) {
        let path = PathBuf::from(value);
        let resolved = if path.is_absolute() {
            path
        } else {
            env::current_dir().unwrap_or_default().join(path)
        };
        println!("cargo:rerun-if-changed={}", resolved.display());
    }
}

fn emit_git_rerun_metadata() {
    emit_git_rerun_path("HEAD");
    if let Some(symbolic_ref) = git_optional(&["symbolic-ref", "-q", "HEAD"]) {
        emit_git_rerun_path(&symbolic_ref);
    }
    emit_git_rerun_path("packed-refs");
}

fn main() {
    println!("cargo:rerun-if-env-changed=DEVBOX_BUILD_GIT_SHA");
    println!("cargo:rerun-if-env-changed=DEVBOX_BUILD_GIT_REF");
    emit_git_rerun_metadata();
    emit_git_rerun_path("index");
    let sha = git(&["rev-parse", "HEAD"]);
    let git_ref = git(&["rev-parse", "--abbrev-ref", "HEAD"]);
    let tree = git(&["rev-parse", "HEAD^{tree}"]);
    let dirty = Command::new("git")
        .args(["status", "--porcelain", "--untracked-files=no"])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| !o.stdout.is_empty());
    let untracked = Command::new("git")
        .args(["ls-files", "--others", "--exclude-standard", "."])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| !o.stdout.is_empty());
    println!("cargo:rustc-env=DEVBOX_BUILD_SOURCE_TREE={tree}");
    println!(
        "cargo:rustc-env=DEVBOX_BUILD_SOURCE_DIRTY={}",
        match (dirty, untracked) {
            (Some(a), Some(b)) =>
                if a || b {
                    "true"
                } else {
                    "false"
                },
            _ => "unknown",
        }
    );
    // Re-run provenance when any tracked build input changes, including uncommitted edits.
    if let Some(root) = git_optional(&["rev-parse", "--show-toplevel"])
        && let Ok(output) = Command::new("git")
            .args(["-C", root.as_str(), "ls-files", "-z", "--full-name"])
            .output()
    {
        for file in output
            .stdout
            .split(|byte| *byte == 0)
            .filter(|p| !p.is_empty())
        {
            println!(
                "cargo:rerun-if-changed={}",
                PathBuf::from(&root)
                    .join(String::from_utf8_lossy(file).as_ref())
                    .display()
            );
        }
    }
    let rustc_program = env::var("RUSTC").unwrap_or_else(|_| "rustc".to_owned());
    let rustc = Command::new(rustc_program)
        .arg("--version")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "unknown".to_owned());
    let built_at = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
        .to_string();
    println!("cargo:rustc-env=DEVBOX_BUILD_GIT_SHA={sha}");
    println!("cargo:rustc-env=DEVBOX_BUILD_GIT_REF={git_ref}");
    println!("cargo:rustc-env=DEVBOX_BUILD_UNIX_SECONDS={built_at}");
    println!("cargo:rustc-env=DEVBOX_BUILD_RUSTC={rustc}");
}
