use crate::protocol::*;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    io::{self, Read, Write},
    mem, ptr,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use windows_sys::Win32::{
    Foundation::*,
    Security::Authorization::*,
    Security::*,
    Storage::FileSystem::*,
    System::{IO::*, Pipes::*, Registry::*, Threading::*},
};
type Pending = Arc<Mutex<HashMap<String, mpsc::Sender<Value>>>>;
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}
pub struct Handle(pub HANDLE);
impl Drop for Handle {
    fn drop(&mut self) {
        if !self.0.is_null() && self.0 != INVALID_HANDLE_VALUE {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }
}
unsafe impl Send for Handle {}
pub fn user_sid() -> io::Result<String> {
    unsafe {
        let mut token = ptr::null_mut();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return Err(io::Error::last_os_error());
        }
        let token = Handle(token);
        let mut size = 0;
        GetTokenInformation(token.0, TokenUser, ptr::null_mut(), 0, &mut size);
        if size == 0 || size > 65536 {
            return Err(io::Error::other("invalid token size"));
        }
        let mut data = vec![0_usize; (size as usize).div_ceil(mem::size_of::<usize>())];
        if GetTokenInformation(
            token.0,
            TokenUser,
            data.as_mut_ptr().cast(),
            size,
            &mut size,
        ) == 0
        {
            return Err(io::Error::last_os_error());
        }
        let user = &*(data.as_ptr().cast::<TOKEN_USER>());
        let mut out = ptr::null_mut();
        if ConvertSidToStringSidW(user.User.Sid, &mut out) == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut n = 0;
        while n < 256 && *out.add(n) != 0 {
            n += 1;
        }
        let sid = String::from_utf16_lossy(std::slice::from_raw_parts(out, n));
        LocalFree(out.cast());
        Ok(sid)
    }
}
pub fn pipe_name() -> io::Result<String> {
    let hash = Sha256::digest(user_sid()?.as_bytes());
    Ok(format!(
        "\\\\.\\pipe\\devbox-chrome-v2-{}",
        hash[..12]
            .iter()
            .map(|v| format!("{v:02x}"))
            .collect::<String>()
    ))
}
pub struct Pipe {
    handle: Handle,
    deadline: Instant,
}
impl Pipe {
    fn operation(&self, read: bool, buffer: *mut u8, length: usize) -> io::Result<usize> {
        unsafe {
            let event = Handle(CreateEventW(ptr::null(), 1, 0, ptr::null()));
            if event.0.is_null() {
                return Err(io::Error::last_os_error());
            }
            let mut overlap: OVERLAPPED = mem::zeroed();
            overlap.hEvent = event.0;
            let result = if read {
                ReadFile(
                    self.handle.0,
                    buffer,
                    length.min(u32::MAX as usize) as u32,
                    ptr::null_mut(),
                    &mut overlap,
                )
            } else {
                WriteFile(
                    self.handle.0,
                    buffer.cast(),
                    length.min(u32::MAX as usize) as u32,
                    ptr::null_mut(),
                    &mut overlap,
                )
            };
            if result == 0 && GetLastError() != ERROR_IO_PENDING {
                return Err(io::Error::last_os_error());
            }
            let wait = self
                .deadline
                .saturating_duration_since(Instant::now())
                .as_millis()
                .min(u32::MAX as u128 - 1) as u32;
            if WaitForSingleObject(event.0, wait) != WAIT_OBJECT_0 {
                CancelIoEx(self.handle.0, &overlap);
                WaitForSingleObject(event.0, INFINITE);
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "bridge pipe deadline",
                ));
            }
            let mut done = 0;
            if GetOverlappedResult(self.handle.0, &overlap, &mut done, 0) == 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(done as usize)
        }
    }
}
impl Read for Pipe {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.operation(true, buf.as_mut_ptr(), buf.len())
    }
}
impl Write for Pipe {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.operation(false, buf.as_ptr().cast_mut(), buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
fn connect() -> io::Result<Pipe> {
    connect_to(&pipe_name()?)
}
fn connect_to(name: &str) -> io::Result<Pipe> {
    unsafe {
        let name = wide(name);
        let deadline = Instant::now() + Duration::from_millis(1200);
        loop {
            let handle = CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                ptr::null(),
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                ptr::null_mut(),
            );
            if handle != INVALID_HANDLE_VALUE {
                return Ok(Pipe {
                    handle: Handle(handle),
                    deadline: Instant::now() + Duration::from_secs(35),
                });
            }
            if GetLastError() != ERROR_PIPE_BUSY || Instant::now() >= deadline {
                return Err(io::Error::last_os_error());
            }
            WaitNamedPipeW(name.as_ptr(), 200);
        }
    }
}
pub fn request(value: &Value) -> Value {
    if let Err(e) = validate_request(value) {
        return error("INVALID_ARGUMENTS", e, false);
    }
    let mut pipe = match connect() {
        Ok(p) => p,
        Err(e) => {
            return error(
                "BRIDGE_UNAVAILABLE",
                format!("Open the Devbox Chrome Bridge extension and connect: {e}"),
                false,
            );
        }
    };
    if let Err(e) = write_frame(&mut pipe, value, REQUEST_LIMIT) {
        return error("OUTCOME_UNKNOWN", e, true);
    }
    match read_frame(&mut pipe, RESPONSE_LIMIT) {
        Ok(Some(v)) => {
            // The server must not disconnect until the complete reply was read.
            // A lost acknowledgement cannot undo an already received response.
            let _ = write_frame(&mut pipe, &serde_json::json!({"ack":true}), 128);
            v
        }
        Ok(None) => error("OUTCOME_UNKNOWN", "bridge disconnected", true),
        Err(e) => error("OUTCOME_UNKNOWN", e, true),
    }
}
fn listen(name: &str) -> io::Result<Pipe> {
    unsafe {
        let sid = user_sid()?;
        let sddl = wide(&format!("D:P(A;;GA;;;SY)(A;;GA;;;{sid})"));
        let mut descriptor = ptr::null_mut();
        if ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            1,
            &mut descriptor,
            ptr::null_mut(),
        ) == 0
        {
            return Err(io::Error::last_os_error());
        }
        let attributes = SECURITY_ATTRIBUTES {
            nLength: mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: descriptor,
            bInheritHandle: 0,
        };
        let name = wide(name);
        let handle = CreateNamedPipeW(
            name.as_ptr(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            65536,
            65536,
            0,
            &attributes,
        );
        LocalFree(descriptor);
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        Ok(Pipe {
            handle: Handle(handle),
            deadline: Instant::now(),
        })
    }
}
fn accept(pipe: &mut Pipe, timeout: u32) -> io::Result<()> {
    unsafe {
        let event = Handle(CreateEventW(ptr::null(), 1, 0, ptr::null()));
        if event.0.is_null() {
            return Err(io::Error::last_os_error());
        }
        let mut overlap: OVERLAPPED = mem::zeroed();
        overlap.hEvent = event.0;
        let connected = ConnectNamedPipe(pipe.handle.0, &mut overlap);
        if connected == 0 {
            let code = GetLastError();
            if code == ERROR_IO_PENDING {
                if WaitForSingleObject(event.0, timeout) != WAIT_OBJECT_0 {
                    CancelIoEx(pipe.handle.0, &overlap);
                    WaitForSingleObject(event.0, INFINITE);
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "pipe accept deadline",
                    ));
                }
                let mut ignored = 0;
                if GetOverlappedResult(pipe.handle.0, &overlap, &mut ignored, 0) == 0 {
                    return Err(io::Error::last_os_error());
                }
            } else if code != ERROR_PIPE_CONNECTED {
                return Err(io::Error::from_raw_os_error(code as i32));
            }
        }
        Ok(())
    }
}
fn send_response(pipe: &mut Pipe, response: &Value, ack_timeout: Duration) -> io::Result<()> {
    write_frame(pipe, response, RESPONSE_LIMIT)?;
    // DisconnectNamedPipe discards unread bytes. Wait for the client
    // acknowledgement with a bounded deadline instead of an unbounded
    // FlushFileBuffers, which a stalled client could hold forever.
    pipe.deadline = pipe.deadline.min(Instant::now() + ack_timeout);
    let acknowledgement = read_frame(pipe, 128)?;
    if acknowledgement
        .as_ref()
        .and_then(|v| v.get("ack"))
        .and_then(Value::as_bool)
        != Some(true)
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "missing response acknowledgement",
        ));
    }
    Ok(())
}
fn server(
    pending: Pending,
    output: Arc<Mutex<io::Stdout>>,
    ready: Arc<AtomicBool>,
) -> io::Result<()> {
    unsafe {
        let mut pipe = listen(&pipe_name()?)?;
        loop {
            accept(&mut pipe, INFINITE)?;
            pipe.deadline = Instant::now() + Duration::from_secs(30);
            let result = (|| -> io::Result<()> {
                let Some(mut value) = read_frame(&mut pipe, REQUEST_LIMIT)? else {
                    return Ok(());
                };
                let response = if !ready.load(Ordering::Acquire) {
                    error(
                        "BRIDGE_NOT_READY",
                        "The extension has not completed protocol version 2 handshake",
                        false,
                    )
                } else if let Err(e) = validate_request(&value) {
                    error("INVALID_ARGUMENTS", e, false)
                } else {
                    let id = nonce().map_err(io::Error::other)?;
                    value["id"] = Value::String(id.clone());
                    let budget = pipe
                        .deadline
                        .saturating_duration_since(Instant::now())
                        .saturating_sub(Duration::from_secs(5))
                        .min(Duration::from_secs(20));
                    if budget < Duration::from_millis(100) {
                        return Err(io::Error::new(
                            io::ErrorKind::TimedOut,
                            "request arrived after its deadline",
                        ));
                    }
                    let expires = SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .map_err(io::Error::other)?
                        .as_millis()
                        + budget.as_millis();
                    value["_bridge_deadline_unix_ms"] = serde_json::json!(expires as u64);
                    let (tx, rx) = mpsc::channel();
                    pending.lock().unwrap().insert(id.clone(), tx);
                    let sent =
                        write_frame(&mut *output.lock().unwrap(), &value, NATIVE_OUTPUT_LIMIT);
                    let response = if sent.is_ok() {
                        loop {
                            let remaining = pipe
                                .deadline
                                .saturating_duration_since(Instant::now())
                                .saturating_sub(Duration::from_secs(3));
                            if remaining.is_zero() {
                                break error(
                                    "OUTCOME_UNKNOWN",
                                    "extension response deadline; do not automatically repeat input",
                                    true,
                                );
                            }
                            match rx.recv_timeout(remaining.min(Duration::from_millis(100))) {
                                Ok(value) => break value,
                                Err(mpsc::RecvTimeoutError::Disconnected) => {
                                    break error(
                                        "OUTCOME_UNKNOWN",
                                        "extension response disconnected",
                                        true,
                                    );
                                }
                                Err(mpsc::RecvTimeoutError::Timeout) => {
                                    if PeekNamedPipe(
                                        pipe.handle.0,
                                        ptr::null_mut(),
                                        0,
                                        ptr::null_mut(),
                                        ptr::null_mut(),
                                        ptr::null_mut(),
                                    ) == 0
                                    {
                                        let _ = write_frame(
                                            &mut *output.lock().unwrap(),
                                            &serde_json::json!({"control":"cancel","id":id}),
                                            NATIVE_OUTPUT_LIMIT,
                                        );
                                        break error(
                                            "OUTCOME_UNKNOWN",
                                            "client disconnected; cancellation requested",
                                            true,
                                        );
                                    }
                                }
                            }
                        }
                    } else {
                        error("OUTCOME_UNKNOWN", "Chrome pipe disconnected", true)
                    };
                    pending.lock().unwrap().remove(&id);
                    response
                };
                send_response(&mut pipe, &response, Duration::from_secs(2))
            })();
            if let Err(e) = result {
                eprintln!("bridge request ended: {}", e.kind());
            }
            DisconnectNamedPipe(pipe.handle.0);
        }
    }
}
pub fn native_host() -> io::Result<()> {
    let pending: Pending = Arc::new(Mutex::new(HashMap::new()));
    let output = Arc::new(Mutex::new(io::stdout()));
    let ready = Arc::new(AtomicBool::new(false));
    let ready_server = ready.clone();
    let p = pending.clone();
    let o = output.clone();
    std::thread::spawn(move || {
        if let Err(e) = server(p, o, ready_server) {
            eprintln!("native bridge unavailable: {e}");
            std::process::exit(2);
        }
    });
    let mut input = io::stdin().lock();
    while let Some(value) = read_frame(&mut input, RESPONSE_LIMIT)? {
        if value.get("control").and_then(Value::as_str) == Some("hello")
            && value.get("protocol").and_then(Value::as_u64) == Some(2)
        {
            ready.store(true, Ordering::Release);
            write_frame(
                &mut *output.lock().unwrap(),
                &serde_json::json!({"control":"ready","protocol":2,"version":env!("CARGO_PKG_VERSION")}),
                NATIVE_OUTPUT_LIMIT,
            )?;
            continue;
        }
        if let Some(id) = value.get("id").and_then(Value::as_str) {
            if let Some(tx) = pending.lock().unwrap().remove(id) {
                let _ = tx.send(value);
            }
        }
    }
    Ok(())
}
pub fn register_host(manifest: &std::path::Path) -> io::Result<()> {
    unsafe {
        let key = wide("Software\\Google\\Chrome\\NativeMessagingHosts\\com.devbox.chrome_bridge");
        let mut handle = ptr::null_mut();
        let result = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            key.as_ptr(),
            0,
            ptr::null(),
            0,
            KEY_SET_VALUE,
            ptr::null(),
            &mut handle,
            ptr::null_mut(),
        );
        if result != 0 {
            return Err(io::Error::from_raw_os_error(result as i32));
        }
        let value = wide(&manifest.to_string_lossy());
        let result = RegSetValueExW(
            handle,
            ptr::null(),
            0,
            REG_SZ,
            value.as_ptr().cast(),
            (value.len() * 2) as u32,
        );
        RegCloseKey(handle);
        if result != 0 {
            return Err(io::Error::from_raw_os_error(result as i32));
        }
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn pair() -> (String, Pipe) {
        let name = format!("\\\\.\\pipe\\devbox-chrome-test-{}", nonce().unwrap());
        let server = listen(&name).unwrap();
        (name, server)
    }
    #[test]
    fn buffered_reply_survives_a_slow_reader_until_acknowledged() {
        let (name, mut server) = pair();
        let (tx, rx) = mpsc::channel();
        let expected = serde_json::json!({"ok":true,"text":"real pipe 🌍"});
        let reply = expected.clone();
        let task = std::thread::spawn(move || {
            accept(&mut server, 5000).unwrap();
            server.deadline = Instant::now() + Duration::from_secs(5);
            let result = send_response(&mut server, &reply, Duration::from_secs(3));
            tx.send(result).unwrap();
            unsafe {
                DisconnectNamedPipe(server.handle.0);
            }
        });
        let mut client = connect_to(&name).unwrap();
        std::thread::sleep(Duration::from_millis(50));
        assert!(
            matches!(rx.try_recv(), Err(mpsc::TryRecvError::Empty)),
            "server disconnected before acknowledgement"
        );
        assert_eq!(
            read_frame(&mut client, RESPONSE_LIMIT).unwrap(),
            Some(expected)
        );
        write_frame(&mut client, &serde_json::json!({"ack":true}), 128).unwrap();
        rx.recv_timeout(Duration::from_secs(5)).unwrap().unwrap();
        task.join().unwrap();
    }
    #[test]
    fn missing_acknowledgement_has_a_deadline() {
        let (name, mut server) = pair();
        let task = std::thread::spawn(move || {
            accept(&mut server, 5000).unwrap();
            server.deadline = Instant::now() + Duration::from_secs(5);
            send_response(
                &mut server,
                &serde_json::json!({"ok":true}),
                Duration::from_millis(40),
            )
        });
        let mut client = connect_to(&name).unwrap();
        assert!(read_frame(&mut client, RESPONSE_LIMIT).unwrap().is_some());
        assert_eq!(
            task.join().unwrap().unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
    }
    #[test]
    fn incomplete_request_and_oversized_frame_are_bounded() {
        let (name, mut server) = pair();
        let task = std::thread::spawn(move || {
            accept(&mut server, 5000).unwrap();
            server.deadline = Instant::now() + Duration::from_millis(40);
            read_frame(&mut server, REQUEST_LIMIT)
        });
        let mut client = connect_to(&name).unwrap();
        client.write_all(&100_u32.to_le_bytes()).unwrap();
        client.write_all(b"{").unwrap();
        assert_eq!(
            task.join().unwrap().unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        assert!(read_frame(&mut &u32::MAX.to_le_bytes()[..], REQUEST_LIMIT).is_err());
    }
}
