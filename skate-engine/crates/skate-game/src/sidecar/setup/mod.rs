//! First-time setup: converts the player's Skate 3 disc (ISO, default.xex or
//! extracted folder) into what the TF2 mod needs, with no other tools:
//!
//! - `<mod>/skate3_data/assets/private/...`: the simulation's data (animation banks,
//!   graphs, camera and stick tables copied from the disc's archives; the
//!   tuning database, physics skeletons and trick names converted)
//! - `<mod>/sound/skate/...`: the skateboard sounds and Skate 3's event table
//! - `<mod>/models/skate/board.skbd` + materials: the default board
//!
//! The game's setup panel calls `skate3_setup_start` and polls
//! `skate3_setup_status` (ffi below); `skate3-sidecar --setup` runs it from a
//! terminal.
mod audio;
mod big;
mod board;
mod disc;
mod dynlib;
mod rx2;
mod tables;
mod vlt;

use big::BigArchive;
use disc::Disc;
use serde_json::json;
use std::{
    ffi::{CStr, c_char, c_float, c_int},
    path::{Path, PathBuf},
    sync::Mutex,
};

/// The converted data's folder inside the mod.
pub(crate) const DATA_FOLDER: &str = "skate3_data";

/// Converts everything into the mod folder. `report(message, fraction)`
/// follows progress.
pub(crate) fn run(source: &Path, mod_dir: &Path, report: &(dyn Fn(String, f32) + Sync)) -> Result<PathBuf, String> {
    let started = std::time::Instant::now();
    let data_dir = mod_dir.join(DATA_FOLDER);
    report("Opening the Skate 3 disc".into(), 0.0);
    let disc = Disc::open(source)?;
    report(format!("Reading {}", disc.describe()), 0.01);
    let assets = data_dir.join("assets");
    let private = assets.join("private");
    let stock = private.join("stock");
    let write = |path: &Path, data: &[u8]| -> Result<(), String> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        }
        std::fs::write(path, data).map_err(|e| format!("{}: {e}", path.display()))
    };
    let safe = |p: &str| !p.is_empty() && !p.starts_with('/') && !p.split('/').any(|c| c.is_empty() || c == "." || c == ".." || c.contains(':'));

    // Animation banks, state graphs, camera and stick tables.
    let miscload = BigArchive::open(&disc, "data/big/miscload.big")?;
    for (n, entry) in miscload.entries.iter().enumerate() {
        if safe(&entry.path) {
            write(&stock.join(&entry.path), &miscload.read(entry)?)?;
        }
        if n % 100 == 0 {
            report(format!("Extracting animations and graphs ({n}/{})", miscload.entries.len()), 0.02 + 0.13 * n as f32 / miscload.entries.len() as f32);
        }
    }
    let miscboot = BigArchive::open(&disc, "data/big/miscboot.big")?;
    write(&stock.join("data/config/input.cfg"), &miscboot.read_path("data/config/input.cfg")?)?;
    for path in disc.list("data/anim") {
        write(&stock.join(&path), &disc.read(&path)?)?;
    }

    report("Converting physics and tuning data".into(), 0.16);
    let db = BigArchive::open(&disc, "data/big/db.big")?;
    let collections = vlt::convert(
        (db.read_path("data/db/skaterschema.vlt")?, db.read_path("data/db/skaterschema.bin")?),
        (db.read_path("data/db/skatercollections.vlt")?, db.read_path("data/db/skatercollections.bin")?),
    )?;
    write(&stock.join("skater-collections.json"), collections.to_string().as_bytes())?;
    let onboard = std::fs::read(stock.join("data/anim/OnBoard.abin")).map_err(|e| format!("OnBoard.abin: {e}"))?;
    write(&stock.join("physics-skeletons.json"), tables::physics_skeletons(&onboard)?.to_string().as_bytes())?;

    report("Reading trick names".into(), 0.24);
    let language = |name: &str| -> Result<Vec<u8>, String> {
        miscboot
            .entries
            .iter()
            .find(|e| e.path.eq_ignore_ascii_case(name))
            .ok_or_else(|| format!("No {name}"))
            .and_then(|e| miscboot.read(e))
    };
    let tricks = tables::trick_display(
        &language("data/fe/languages/labels/LANGUAGE_Labels_Global_skate3ng.BIN")?,
        &language("data/fe/languages/english/LANGUAGE_English_Global_skate3ng.BIN")?,
    )?;
    write(&private.join("hud/runtime/trickdisplay.json"), tricks.to_string().as_bytes())?;

    // The asset manifest. The simulation only checks that the character
    // scene exists (the windowed engine renders it), so a minimal glTF does.
    let manifest = json!({
        "version": 1,
        "character_scene": "private/skater.glb",
        "initial_animation": "R_IDLE_HCOM_000",
        "action_graph": "private/stock/data/state/ActionGraph_OnBoard.stategraph",
        "motion_graph": "private/stock/data/state/MotionGraph_OnBoard.stategraph",
    });
    write(&private.join("game.json"), manifest.to_string().as_bytes())?;
    write(&private.join("skater.glb"), &minimal_glb())?;

    report("Converting the skateboard".into(), 0.27);
    let board = board::convert(&disc, mod_dir)?;

    let sounds = audio::convert(&disc, &mod_dir.join("sound/skate"), &data_dir.join(".setup-tmp"), &collections, report, 0.30, 0.68)?;

    report(
        format!("Done in {:.0}s: {board}; {sounds}.", started.elapsed().as_secs_f32()),
        1.0,
    );
    Ok(assets)
}

/// The smallest valid glTF binary: one empty scene.
fn minimal_glb() -> Vec<u8> {
    let mut json = br#"{"asset":{"version":"2.0"},"scenes":[{}],"scene":0}"#.to_vec();
    while json.len() % 4 != 0 {
        json.push(b' ');
    }
    let mut out = b"glTF".to_vec();
    out.extend(2u32.to_le_bytes());
    out.extend((12 + 8 + json.len() as u32).to_le_bytes());
    out.extend((json.len() as u32).to_le_bytes());
    out.extend(b"JSON");
    out.extend(json);
    out
}

// ---------------------------------------------------------------- C interface

struct Status {
    state: c_int, // 0 idle, 1 running, 2 done, 3 failed
    message: String,
    progress: f32,
}

static STATUS: Mutex<Status> = Mutex::new(Status { state: 0, message: String::new(), progress: 0.0 });

fn set_status(state: Option<c_int>, message: String, progress: f32) {
    if let Ok(mut status) = STATUS.lock() {
        if let Some(state) = state {
            status.state = state;
        }
        status.message = message;
        status.progress = progress;
    }
}

fn path_arg(p: *const c_char) -> Option<PathBuf> {
    // SAFETY: the caller passes NUL-terminated strings (or null).
    (!p.is_null()).then(|| PathBuf::from(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()))
}

/// Starts converting `source` into the mod folder `mod_dir`, on a
/// background thread. 0 on start, -1 if one is already running or an
/// argument is missing.
#[unsafe(no_mangle)]
pub extern "C" fn skate3_setup_start(source: *const c_char, mod_dir: *const c_char) -> c_int {
    let (Some(source), Some(mod_dir)) = (path_arg(source), path_arg(mod_dir)) else {
        return -1;
    };
    {
        let Ok(mut status) = STATUS.lock() else { return -1 };
        if status.state == 1 {
            return -1;
        }
        *status = Status { state: 1, message: "Starting".into(), progress: 0.0 };
    }
    let spawned = std::thread::Builder::new().name("skate3-setup".into()).spawn(move || {
        let outcome = std::panic::catch_unwind(|| run(&source, &mod_dir, &|message, progress| set_status(None, message, progress)));
        match outcome {
            Ok(Ok(_)) => {
                let message = STATUS.lock().map(|s| s.message.clone()).unwrap_or_default();
                set_status(Some(2), message, 1.0);
            }
            Ok(Err(error)) => set_status(Some(3), error, 0.0),
            Err(_) => set_status(Some(3), "The converter crashed (see the console)".into(), 0.0),
        }
    });
    if spawned.is_err() {
        set_status(Some(3), "Couldn't start the converter thread".into(), 0.0);
        return -1;
    }
    0
}

/// The current state (0 idle, 1 running, 2 done, 3 failed), with the latest
/// message and progress (0..1).
#[unsafe(no_mangle)]
pub extern "C" fn skate3_setup_status(message: *mut c_char, message_size: usize, progress: *mut c_float) -> c_int {
    let Ok(status) = STATUS.lock() else { return 3 };
    if !message.is_null() && message_size > 0 {
        let bytes = status.message.as_bytes();
        let n = bytes.len().min(message_size - 1);
        // SAFETY: the caller passes a writable buffer of message_size bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), message.cast::<u8>(), n);
            *message.add(n) = 0;
        }
    }
    if !progress.is_null() {
        // SAFETY: valid out-pointer from the caller.
        unsafe { *progress = status.progress };
    }
    status.state
}
