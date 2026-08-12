use std::fmt::Write as _;

#[must_use]
pub fn lower_hex(value: impl AsRef<[u8]>) -> String {
    let bytes = value.as_ref();
    let mut encoded = String::with_capacity(bytes.len().saturating_mul(2));
    for byte in bytes {
        let _ = write!(&mut encoded, "{byte:02x}");
    }
    encoded
}
