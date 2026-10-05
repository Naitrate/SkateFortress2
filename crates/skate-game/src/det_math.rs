//! The C maths library, replaced inside libskate3 / skate3.dll by the pure-Rust
//! `libm` crate.
//!
//! The TF2 mod predicts each player's skater on their own PC and checks it
//! against the server's (sdk/src/game/client/tf/c_tf_skate_predict.cpp), so
//! the simulation must give bit-identical results on every machine. Rust's
//! f32/f64 methods, and glam's, end up as calls to the platform's C functions
//! (glibc on Linux, MinGW's and msvcrt.dll's on Windows), and those don't
//! always round the same way: a Linux server and a Windows player drifted
//! apart from the first tick (one ulp of a spawn yaw). Defining the symbols
//! here makes this library use the same code everywhere. On Linux the
//! library links with -Bsymbolic (skate-game/build.rs) so these bind before
//! the system libm; on Windows the linker takes them before any import
//! library. Only the TF2 library compiles this file, not the game.
//!
//! The library is loaded RTLD_LOCAL / by LoadLibrary, so these don't replace
//! anything for the rest of the process.

macro_rules! unary {
    ($($name:ident: $ty:ty),* $(,)?) => {
        $(
            #[unsafe(no_mangle)]
            pub extern "C" fn $name(x: $ty) -> $ty {
                libm::$name(x)
            }
        )*
    };
}

macro_rules! binary {
    ($($name:ident: $ty:ty),* $(,)?) => {
        $(
            #[unsafe(no_mangle)]
            pub extern "C" fn $name(x: $ty, y: $ty) -> $ty {
                libm::$name(x, y)
            }
        )*
    };
}

unary!(
    sinf: f32, cosf: f32, tanf: f32, asinf: f32, acosf: f32, atanf: f32,
    sinhf: f32, coshf: f32, tanhf: f32, asinhf: f32, acoshf: f32, atanhf: f32,
    expf: f32, exp2f: f32, exp10f: f32, expm1f: f32,
    logf: f32, log2f: f32, log10f: f32, log1pf: f32, cbrtf: f32,
    sin: f64, cos: f64, tan: f64, asin: f64, acos: f64, atan: f64,
    sinh: f64, cosh: f64, tanh: f64, asinh: f64, acosh: f64, atanh: f64,
    exp: f64, exp2: f64, exp10: f64, expm1: f64,
    log: f64, log2: f64, log10: f64, log1p: f64, cbrt: f64,
);

binary!(atan2f: f32, powf: f32, hypotf: f32, atan2: f64, pow: f64, hypot: f64);

/// glibc's combined sine and cosine, which LLVM may call for `sin_cos`.
///
/// # Safety
/// `s` and `c` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sincosf(x: f32, s: *mut f32, c: *mut f32) {
    let (sin, cos) = libm::sincosf(x);
    // SAFETY: the caller passes two writable floats.
    unsafe {
        *s = sin;
        *c = cos;
    }
}

/// # Safety
/// `s` and `c` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sincos(x: f64, s: *mut f64, c: *mut f64) {
    let (sin, cos) = libm::sincos(x);
    // SAFETY: the caller passes two writable doubles.
    unsafe {
        *s = sin;
        *c = cos;
    }
}
