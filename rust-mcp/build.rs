use std::{
    env,
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

fn git(args: &[&str]) -> String {
    Command::new("git")
        .args(args)
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "unknown".to_owned())
}

fn main() {
    println!("cargo:rerun-if-env-changed=DEVBOX_BUILD_GIT_SHA");
    println!("cargo:rerun-if-env-changed=DEVBOX_BUILD_GIT_REF");
    println!("cargo:rerun-if-changed=../.git/HEAD");
    let sha = env::var("DEVBOX_BUILD_GIT_SHA").unwrap_or_else(|_| git(&["rev-parse", "HEAD"]));
    let git_ref = env::var("DEVBOX_BUILD_GIT_REF")
        .unwrap_or_else(|_| git(&["rev-parse", "--abbrev-ref", "HEAD"]));
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
