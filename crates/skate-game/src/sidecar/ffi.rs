//! C interface of libskate3.so, the in-process form of the sidecar.
//!
//! The TF2 mod's server.so dlopens bin/linux64/libskate3.so and sends the
//! same request frames the TCP sidecar takes (protocol.rs), so one code path
//! serves both. Panics never cross into C++: a request that panics fails with
//! the panic message, and the game only stops that player skating.
//!
//! ```c
//! void *skate3_open(const char *asset_root, char *error, size_t error_size);
//! int   skate3_request(void *, uint8_t kind, const uint8_t *payload, size_t size,
//!                      uint8_t **reply, size_t *reply_size);   // 0 = reply set
//! void  skate3_free(uint8_t *reply, size_t reply_size);
//! void  skate3_close(void *);
//! uint32_t skate3_protocol_version(void);
//! ```
use super::{PROTOCOL_VERSION, Server};
use std::{
    ffi::{CStr, c_char, c_void},
    panic::{AssertUnwindSafe, catch_unwind},
    path::PathBuf,
};

fn panic_message(panic: Box<dyn std::any::Any + Send>) -> String {
    panic
        .downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| panic.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "unknown panic".into())
}

fn write_error(error: *mut c_char, size: usize, message: &str) {
    if error.is_null() || size == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let n = bytes.len().min(size - 1);
    // SAFETY: the caller passes a writable buffer of `size` bytes.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), error.cast::<u8>(), n);
        *error.add(n) = 0;
    }
}

/// Loads the shared Skate 3 data from `asset_root`. Null on failure, with
/// the reason in `error`.
#[unsafe(no_mangle)]
pub extern "C" fn skate3_open(asset_root: *const c_char, error: *mut c_char, error_size: usize) -> *mut c_void {
    if asset_root.is_null() {
        write_error(error, error_size, "No Skate 3 asset folder given");
        return std::ptr::null_mut();
    }
    // SAFETY: the caller passes a NUL-terminated string.
    let root = PathBuf::from(unsafe { CStr::from_ptr(asset_root) }.to_string_lossy().into_owned());
    match catch_unwind(|| Server::open(root)) {
        Ok(Ok(server)) => Box::into_raw(Box::new(server)).cast(),
        Ok(Err(message)) => {
            write_error(error, error_size, &message);
            std::ptr::null_mut()
        }
        Err(panic) => {
            write_error(error, error_size, &format!("Skate 3 loader panicked: {}", panic_message(panic)));
            std::ptr::null_mut()
        }
    }
}

/// Handles one request. On 0, `*reply` holds an ok byte and the payload (or
/// the failure reason); free it with `skate3_free`.
#[unsafe(no_mangle)]
pub extern "C" fn skate3_request(
    handle: *mut c_void,
    kind: u8,
    payload: *const u8,
    size: usize,
    reply: *mut *mut u8,
    reply_size: *mut usize,
) -> i32 {
    if handle.is_null() || reply.is_null() || reply_size.is_null() || (payload.is_null() && size != 0) {
        return -1;
    }
    // SAFETY: `handle` came from skate3_open and is used from one thread at a
    // time (the game's server thread); `payload` holds `size` bytes.
    let server = unsafe { &mut *handle.cast::<Server>() };
    let input = if size == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(payload, size) } };
    let bytes = match catch_unwind(AssertUnwindSafe(|| server.handle(kind, input))) {
        Ok(bytes) => bytes,
        Err(panic) => {
            let message = format!("Skate 3 simulation panicked: {}", panic_message(panic));
            eprintln!("SIDECAR {message}");
            let mut failure = super::protocol::Writer::default();
            failure.string(&message);
            failure.finish(false)
        }
    };
    let boxed = bytes.into_boxed_slice();
    // SAFETY: both out-pointers were checked non-null above.
    unsafe {
        *reply_size = boxed.len();
        *reply = Box::into_raw(boxed).cast::<u8>();
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn skate3_free(reply: *mut u8, reply_size: usize) {
    if !reply.is_null() {
        // SAFETY: allocated by skate3_request as a boxed slice of this size.
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(reply, reply_size)) });
    }
}

/// Drops every skater (their threads end) and the shared data.
#[unsafe(no_mangle)]
pub extern "C" fn skate3_close(handle: *mut c_void) {
    if !handle.is_null() {
        // SAFETY: `handle` came from skate3_open and isn't used afterwards.
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(handle.cast::<Server>()) })));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn skate3_protocol_version() -> u32 {
    PROTOCOL_VERSION
}
