// Headless skater server for the TF2 Source mod. See src/sidecar/mod.rs.
// It shares the game's module tree, most of which only the windowed app uses.
#![allow(dead_code, unused_imports)]
include!("modules.rs");
mod sidecar;

fn main() {
    // First-time setup from a terminal: skate3-sidecar --setup <disc> <mod folder>
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("--setup") && args.len() == 4 {
        let report = |message: String, progress: f32| eprintln!("[{:3.0}%] {message}", progress * 100.0);
        match sidecar::setup::run(args[2].as_ref(), args[3].as_ref(), &report) {
            Ok(assets) => println!("{}", assets.display()),
            Err(error) => {
                eprintln!("Setup failed: {error}");
                std::process::exit(1);
            }
        }
        return;
    }
    if let Err(error) = sidecar::run() {
        eprintln!("SIDECAR fatal: {error}");
        std::process::exit(1);
    }
}
