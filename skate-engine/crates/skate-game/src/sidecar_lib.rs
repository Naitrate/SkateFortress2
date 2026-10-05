// libskate3.so: the TF2 mod's in-process Skate 3 simulation (built by the
// skate3-lib crate). Same module tree as the game; the C interface is
// src/sidecar/ffi.rs.
#![allow(dead_code, unused_imports)]
include!("modules.rs");
mod sidecar;
mod det_math;
