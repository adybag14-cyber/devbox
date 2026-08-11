use std::{fmt::Write as _, fs::File, io::Read as _, sync::OnceLock};

use serde_json::{Value, json};
use sha2::{Digest, Sha256};

static SNAPSHOT: OnceLock<Value> = OnceLock::new();

#[must_use]
pub fn snapshot() -> Value {
    SNAPSHOT.get_or_init(build_snapshot).clone()
}

fn build_snapshot() -> Value {
    let executable = std::env::current_exe().ok();
    let binary_sha256 = executable
        .as_deref()
        .and_then(hash_file)
        .unwrap_or_else(|| "unavailable".to_owned());
    json!({
        "gitSha": env!("DEVBOX_BUILD_GIT_SHA"),
        "gitRef": env!("DEVBOX_BUILD_GIT_REF"),
        "buildUnixSeconds": env!("DEVBOX_BUILD_UNIX_SECONDS"),
        "rustc": env!("DEVBOX_BUILD_RUSTC"),
        "binarySha256": binary_sha256,
        "executable": executable.map(|path| path.to_string_lossy().into_owned()),
        "deploymentGeneration": std::env::var("DEVBOX_DEPLOYMENT_GENERATION").ok(),
    })
}

fn hash_file(path: &std::path::Path) -> Option<String> {
    let mut file = File::open(path).ok()?;
    let mut digest = Sha256::new();
    let mut buffer = vec![0_u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer).ok()?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    let digest = digest.finalize();
    let mut encoded = String::with_capacity(digest.len() * 2);
    for byte in digest {
        let _ = write!(&mut encoded, "{byte:02x}");
    }
    Some(encoded)
}
