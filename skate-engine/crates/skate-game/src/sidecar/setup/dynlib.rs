//! Loading a shared library at run time (libvgmstream beside this library),
//! on Linux (dlopen) and Windows (LoadLibrary).
use std::{
    ffi::{CString, c_void},
    path::{Path, PathBuf},
};

/// libvgmstream's file name on this platform.
#[cfg(windows)]
pub(super) const VGMSTREAM: &str = "libvgmstream.dll";
#[cfg(not(windows))]
pub(super) const VGMSTREAM: &str = "libvgmstream.so";

/// An opened library. Never closed: it lives as long as the process.
pub(super) struct Library {
    handle: *mut c_void,
    path: PathBuf,
}

impl Library {
    pub(super) fn symbol(&self, name: &str) -> Result<*mut c_void, String> {
        let c = CString::new(name).map_err(|e| e.to_string())?;
        // SAFETY: looking up a symbol in a handle we opened.
        let pointer = unsafe { sys::symbol(self.handle, c.as_ptr()) };
        if pointer.is_null() { Err(format!("{} lacks {name}", self.path.display())) } else { Ok(pointer) }
    }
}

pub(super) fn open(path: &Path) -> Result<Library, String> {
    // SAFETY: loading a library by its full path; its initialisers run.
    let handle = unsafe { sys::open(path) }?;
    Ok(Library { handle, path: path.to_path_buf() })
}

/// The folder holding this library (libskate3.so / skate3.dll).
pub(super) fn own_folder() -> Option<PathBuf> {
    sys::own_path().and_then(|path| path.parent().map(Path::to_path_buf))
}

#[cfg(not(windows))]
mod sys {
    use std::{
        ffi::{CStr, CString, c_char, c_int, c_void},
        os::unix::ffi::OsStrExt,
        path::{Path, PathBuf},
    };

    #[link(name = "dl")]
    unsafe extern "C" {
        fn dlopen(file: *const c_char, mode: c_int) -> *mut c_void;
        fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
        fn dlerror() -> *const c_char;
        fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    }
    #[repr(C)]
    struct DlInfo {
        fname: *const c_char,
        fbase: *mut c_void,
        sname: *const c_char,
        saddr: *mut c_void,
    }

    pub(super) unsafe fn open(path: &Path) -> Result<*mut c_void, String> {
        let c = CString::new(path.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
        // SAFETY: RTLD_NOW | RTLD_LOCAL of a path we were given.
        let handle = unsafe { dlopen(c.as_ptr(), 2) };
        if !handle.is_null() {
            return Ok(handle);
        }
        // SAFETY: dlerror returns a static message or null.
        let error = unsafe { dlerror() };
        Err(if error.is_null() { path.display().to_string() } else { unsafe { CStr::from_ptr(error) }.to_string_lossy().into_owned() })
    }

    pub(super) unsafe fn symbol(handle: *mut c_void, name: *const c_char) -> *mut c_void {
        // SAFETY: the caller passes a handle from dlopen.
        unsafe { dlsym(handle, name) }
    }

    pub(super) fn own_path() -> Option<PathBuf> {
        let mut info = DlInfo { fname: std::ptr::null(), fbase: std::ptr::null_mut(), sname: std::ptr::null(), saddr: std::ptr::null_mut() };
        // SAFETY: dladdr fills `info` for an address inside a loaded object.
        let found = unsafe { dladdr(own_path as *const c_void, &mut info) } != 0 && !info.fname.is_null();
        // SAFETY: dli_fname is a NUL-terminated path owned by the loader.
        found.then(|| PathBuf::from(unsafe { CStr::from_ptr(info.fname) }.to_string_lossy().into_owned()))
    }
}

#[cfg(windows)]
mod sys {
    use std::{
        ffi::{OsString, c_char, c_void},
        os::windows::ffi::{OsStrExt, OsStringExt},
        path::{Path, PathBuf},
    };

    const LOAD_WITH_ALTERED_SEARCH_PATH: u32 = 0x8;
    const GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS: u32 = 0x4;
    const GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT: u32 = 0x2;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn LoadLibraryExW(name: *const u16, file: *mut c_void, flags: u32) -> *mut c_void;
        fn GetProcAddress(module: *mut c_void, name: *const c_char) -> *mut c_void;
        fn GetModuleHandleExW(flags: u32, name: *const u16, module: *mut *mut c_void) -> i32;
        fn GetModuleFileNameW(module: *mut c_void, name: *mut u16, size: u32) -> u32;
        fn GetLastError() -> u32;
    }

    pub(super) unsafe fn open(path: &Path) -> Result<*mut c_void, String> {
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        // Altered search path: the library's own dependencies are looked for
        // beside it, not beside the game's executable.
        // SAFETY: a NUL-terminated wide path.
        let handle = unsafe { LoadLibraryExW(wide.as_ptr(), std::ptr::null_mut(), LOAD_WITH_ALTERED_SEARCH_PATH) };
        if handle.is_null() {
            // SAFETY: reads the calling thread's last error.
            return Err(format!("{} (Windows error {})", path.display(), unsafe { GetLastError() }));
        }
        Ok(handle)
    }

    pub(super) unsafe fn symbol(handle: *mut c_void, name: *const c_char) -> *mut c_void {
        // SAFETY: the caller passes a module handle from LoadLibraryExW.
        unsafe { GetProcAddress(handle, name) }
    }

    pub(super) fn own_path() -> Option<PathBuf> {
        let mut module = std::ptr::null_mut();
        // SAFETY: with FROM_ADDRESS the "name" is an address inside this module.
        let found = unsafe {
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                own_path as *const u16,
                &mut module,
            )
        } != 0;
        if !found {
            return None;
        }
        let mut buffer = vec![0u16; 32768];
        // SAFETY: the buffer's length is passed.
        let length = unsafe { GetModuleFileNameW(module, buffer.as_mut_ptr(), buffer.len() as u32) } as usize;
        (length > 0 && length < buffer.len()).then(|| PathBuf::from(OsString::from_wide(&buffer[..length])))
    }
}
