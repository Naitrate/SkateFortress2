//! Headless skater server for the TF2 mod.
//!
//! The Source game DLL connects over loopback TCP, sends its map's BSP once,
//! then drives one Skate skater per TF2 player with per-usercmd input. Each
//! skater owns the same runtime set the Bevy app schedules (GamePhysics,
//! SkaterRuntime, PlayerControls, CameraRuntime, ControllerInput) and steps
//! it at the native 60 Hz with the stock FixedUpdate ordering.
//!
//! Units: the wire carries Source coordinates (Z-up inches, degrees). The
//! conversion to native Y-up metres happens only here.

mod bsp;
pub(crate) mod ffi;
mod pad;
mod protocol;
pub(crate) mod setup;

use crate::{
    camera::CameraRuntime,
    difficulty::Difficulty,
    graph_runtime::StockGraphs,
    input::{ControllerInput, platform},
    physics::{GamePhysics, PlayerControls, SkaterRuntime},
};
use protocol::{Reader, Writer};
use skate_core::physics::board::BodyId;
use skate_data::skate_map;
use std::{
    collections::HashMap,
    io::{BufReader, BufWriter, Write},
    net::{TcpListener, TcpStream},
    path::PathBuf,
    sync::{Arc, mpsc},
    time::Instant,
};

pub(crate) const PROTOCOL_VERSION: u32 = 10;
/// STEP flag: start a wipeout now (e.g. the TF2 player hit deep water).
const STEP_FORCE_WIPEOUT: u32 = 1;
/// PhysicalStateId of the first tick, which places a new skater.
const SPAWN_STATE: u32 = 700;
/// STEP flag: reply with the previous step's result and run this one in the
/// background, so many skaters (bots) step in parallel. Adds one usercmd of
/// latency, which only matters for the player holding the controls.
const STEP_PIPELINED: u32 = 2;
/// STEP reply state while the skater is still loading on its worker thread.
const STATE_LOADING: u32 = u32::MAX;

/// Joints sent for retargeting, in wire order. Must match SKATE_JOINT_* in
/// src/game/shared/tf/tf_skate_shared.h.
const RETARGET_JOINTS: [&str; 19] = [
    "HIPS", "SPINE", "SPINE3", "NECK", "HEAD",
    "LEFTARM", "LEFTFOREARM", "LEFTHAND",
    "RIGHTARM", "RIGHTFOREARM", "RIGHTHAND",
    "LEFTUPLEG", "LEFTLEG", "LEFTFOOT", "LEFTTOEBASE",
    "RIGHTUPLEG", "RIGHTLEG", "RIGHTFOOT", "RIGHTTOEBASE",
];
const DEFAULT_PORT: u16 = 27720;
/// Most native ticks one usercmd may run. Larger gaps are a hitch, not input.
const MAX_TICKS_PER_STEP: u32 = 6;

/// Standalone server for tools/sidecar_smoke.py and benchmarks. The game
/// loads the same code as libskate3.so (see ffi.rs) instead.
pub(crate) fn run() -> Result<(), String> {
    let root = std::env::var_os("SKATE3_ASSET_ROOT")
        .map(PathBuf::from)
        .or_else(|| std::env::args_os().nth(1).map(PathBuf::from))
        .ok_or("Set SKATE3_ASSET_ROOT (or pass it as the first argument) to a prepared skate3rust data folder")?;
    let port = std::env::var("SKATE_SIDECAR_PORT")
        .ok()
        .map(|p| p.parse::<u16>().map_err(|e| format!("SKATE_SIDECAR_PORT: {e}")))
        .transpose()?
        .unwrap_or(DEFAULT_PORT);
    let mut server = Server::open(root)?;

    let listener = TcpListener::bind(("127.0.0.1", port)).map_err(|e| format!("bind 127.0.0.1:{port}: {e}"))?;
    eprintln!("SIDECAR listening on 127.0.0.1:{port}");
    for stream in listener.incoming() {
        let stream = match stream {
            Ok(stream) => stream,
            Err(error) => {
                eprintln!("SIDECAR accept failed: {error}");
                continue;
            }
        };
        eprintln!("SIDECAR game server connected");
        if let Err(error) = server.serve(stream) {
            eprintln!("SIDECAR connection closed: {error}");
        }
        // A new game server (or map change reconnect) starts from scratch.
        server.skaters.clear();
        server.world = None;
    }
    Ok(())
}

struct World {
    name: String,
    /// The map's collision, built once; each skater gets a `share()` of it.
    collision: skate_core::physics::board_world::BoardWorld,
    /// Native Y-up metres, wound out of the solid.
    triangles: Vec<bsp::Triangle>,
    /// Grind rails from the map's skate_rail entities, native space.
    rails: Vec<skate_map::Rail>,
    scale: f32,
}

#[derive(Clone)]
struct Skater {
    physics: GamePhysics,
    runtime: SkaterRuntime,
    controls: PlayerControls,
    camera: CameraRuntime,
    controller: ControllerInput,
    pad: pad::VirtualPad,
    packet: u32,
    accumulator: f32,
    /// Render-pose index of each RETARGET_JOINTS entry.
    joints: [usize; RETARGET_JOINTS.len()],
    /// Pose before the latest native tick. Replies blend from it to the
    /// current pose by the accumulator fraction, so the 60 Hz simulation reads
    /// smoothly at TF2's 66.7 Hz instead of skipping every tenth usercmd.
    previous: Option<Pose>,
    /// The spawn velocity (native m/s). The first native tick places the
    /// skater (state 700) and settles its bodies at rest, so this is added
    /// once that tick has run.
    spawn_velocity: Option<[f32; 3]>,
}

struct StepJob {
    dt: f32,
    command: pad::Command,
    flags: u32,
    /// The player's flick-stick gain and decay (0 = keep the default).
    mouse_gain: f32,
    mouse_decay: f32,
    /// Velocity added before this step (native m/s): explosions, knockback.
    impulse: [f32; 3],
}

/// Work for a skater's thread, in order.
enum Job {
    Step(StepJob),
    /// Send back a copy of the skater as it is now.
    Snapshot(mpsc::Sender<Box<Skater>>),
    /// Continue from this copy instead (client prediction rewinding).
    Replace(Box<Skater>),
}

/// The request thread's side of one skater. The skater itself lives on its
/// own worker thread: it is built there (~0.2 s of loading that would stall
/// the TF2 server tick on SPAWN), then steps whenever a job arrives. Dropping
/// the handle ends the thread.
struct SkaterHandle {
    jobs: mpsc::Sender<Job>,
    /// Encoded STEP reply payloads, one per job.
    replies: mpsc::Receiver<Result<Vec<u8>, String>>,
    /// One message once loaded: the yaw the skater spawned with.
    loaded: mpsc::Receiver<Result<f32, String>>,
    ready: bool,
    started: Instant,
    yaw: f32,
    in_flight: u32,
    last: Option<Vec<u8>>,
}

impl SkaterHandle {
    /// Takes one finished reply, waiting for it if `block`.
    fn collect(&mut self, block: bool) -> Result<bool, String> {
        if self.in_flight == 0 {
            return Ok(false);
        }
        let reply = if block {
            self.replies.recv().map_err(|_| "Skater thread stopped".to_string())?
        } else {
            match self.replies.try_recv() {
                Ok(reply) => reply,
                Err(mpsc::TryRecvError::Empty) => return Ok(false),
                Err(mpsc::TryRecvError::Disconnected) => return Err("Skater thread stopped".into()),
            }
        };
        self.in_flight -= 1;
        self.last = Some(reply?);
        Ok(true)
    }
}

struct Server {
    root: PathBuf,
    difficulty: Difficulty,
    graphs: Arc<StockGraphs>,
    /// Animation banks and clips, shared by every skater (~40 MB each otherwise).
    animation: Arc<crate::skater_animation::AnimationSource>,
    /// ID_TRICK_* -> English display name, from the HUD's language table.
    trick_names: Arc<HashMap<String, String>>,
    world: Option<World>,
    skaters: HashMap<u32, SkaterHandle>,
}

impl Server {
    /// Loads the stock data every skater shares. `SKATE_DIFFICULTY` picks
    /// the physics mode (easy | normal | hardcore | motorized).
    pub(crate) fn open(root: PathBuf) -> Result<Self, String> {
        let difficulty = std::env::var("SKATE_DIFFICULTY")
            .ok()
            .map(|d| Difficulty::parse(&d))
            .transpose()?
            .unwrap_or(Difficulty::Normal);
        let started = Instant::now();
        let assets = skate_data::GameAssets::load(&root).map_err(|e| format!("{e}"))?;
        let graphs = StockGraphs::load(&root, &assets)?;
        let animation = crate::skater_animation::AnimationSource::load(&root)?;
        let trick_names = load_trick_names(&root);
        eprintln!(
            "SIDECAR assets ready in {:.1}s (root {}, difficulty {}, {} trick names)",
            started.elapsed().as_secs_f32(),
            root.display(),
            difficulty.key(),
            trick_names.len()
        );
        Ok(Self {
            root,
            difficulty,
            graphs: Arc::new(graphs),
            animation,
            trick_names: Arc::new(trick_names),
            world: None,
            skaters: HashMap::new(),
        })
    }

    fn serve(&mut self, stream: TcpStream) -> Result<(), String> {
        stream.set_nodelay(true).map_err(|e| e.to_string())?;
        let mut input = BufReader::new(stream.try_clone().map_err(|e| e.to_string())?);
        let mut output = BufWriter::new(stream);
        loop {
            let Some((kind, payload)) = protocol::read_frame(&mut input)? else {
                return Ok(());
            };
            let reply = self.handle(kind, &payload);
            protocol::write_frame(&mut output, kind, &reply)?;
            output.flush().map_err(|e| e.to_string())?;
        }
    }

    /// One request: the reply is an ok byte, then the payload (or, on
    /// failure, the reason as a string).
    pub(crate) fn handle(&mut self, kind: u8, payload: &[u8]) -> Vec<u8> {
        let mut reply = Writer::default();
        let mut reader = Reader::new(payload);
        let result = match kind {
            protocol::HELLO => {
                reply.u32(PROTOCOL_VERSION);
                Ok(())
            }
            protocol::WORLD => self.load_world(&mut reader, &mut reply),
            protocol::SPAWN => self.spawn(&mut reader, &mut reply),
            protocol::STEP => self.step(&mut reader, &mut reply),
            protocol::COPY => self.copy(&mut reader),
            protocol::POLL => self.poll(&mut reader, &mut reply),
            protocol::DESPAWN => reader.u32().map(|id| {
                self.skaters.remove(&id);
            }),
            other => Err(format!("Unknown message type {other}")),
        };
        match result {
            Ok(()) => reply.finish(true),
            Err(error) => {
                eprintln!("SIDECAR request {kind} failed: {error}");
                let mut failure = Writer::default();
                failure.string(&error);
                failure.finish(false)
            }
        }
    }

    fn load_world(&mut self, reader: &mut Reader, reply: &mut Writer) -> Result<(), String> {
        let name = reader.string()?;
        let scale = reader.f32()?;
        let data = reader.bytes()?;
        // Collision outside the BSP's brushes (static props, func_brush), in
        // world-space Source units, already wound out of the solid.
        let extra_count = reader.u32()? as usize;
        let mut extra = Vec::with_capacity(extra_count.min(1 << 22));
        for _ in 0..extra_count {
            extra.push([reader.vec3()?, reader.vec3()?, reader.vec3()?]);
        }
        // Grind rails placed in the map (skate_rail node chains), Source units.
        let rail_count = reader.u32()? as usize;
        let mut rails = Vec::with_capacity(rail_count.min(4096));
        for _ in 0..rail_count {
            let rail_name = reader.string()?;
            let closed = reader.u32()? != 0;
            let points = reader.u32()? as usize;
            let mut path = Vec::with_capacity(points.min(4096));
            for _ in 0..points {
                path.push(to_native(reader.vec3()?, scale));
            }
            if path.len() >= 2 {
                rails.push(skate_map::Rail { name: rail_name, closed, points: path, native: None });
            }
        }
        if !(scale.is_finite() && scale > 0.0) {
            return Err(format!("Invalid world scale {scale}"));
        }
        let started = Instant::now();
        let extracted = bsp::extract(data)?;
        let converted = extracted.triangles.len() + extra.len();
        let triangles = drop_degenerate(
            extracted.triangles.iter().chain(&extra).map(|t| t.map(|p| to_native(p, scale))).collect(),
        );
        let message = format!(
            "{name}: {} triangles ({} slivers dropped) from {} brushes, {} displacements and {} prop/entity triangles, {} rails in {:.2}s",
            triangles.len(),
            converted - triangles.len(),
            extracted.brushes,
            extracted.displacements,
            extra.len(),
            rails.len(),
            started.elapsed().as_secs_f32()
        );
        eprintln!("SIDECAR world {message}");
        self.skaters.clear();
        let mut world = World { name, collision: skate_core::physics::board_world::BoardWorld::new(Vec::new()), triangles, rails, scale };
        world.collision = GamePhysics::map_world(&self.root, &skate_map_for(&world, [0.0; 3], 0.0, true))?;
        self.world = Some(world);
        reply.string(&message);
        Ok(())
    }

    /// Starts the skater's thread and replies at once; STEP reports
    /// STATE_LOADING until it is built.
    fn spawn(&mut self, reader: &mut Reader, reply: &mut Writer) -> Result<(), String> {
        let id = reader.u32()?;
        let origin = reader.vec3()?;
        let yaw = reader.f32()?;
        // The server's skate_difficulty; empty keeps the sidecar's default.
        let difficulty_name = reader.string()?;
        // The TF2 player's velocity: a skater started mid rocket jump keeps
        // flying (and lands on the board) instead of starting from rest.
        let velocity = reader.vec3()?;
        let world = self.world.as_ref().ok_or("SPAWN before WORLD")?;
        let scale = world.scale;
        let map = skate_map_for(world, to_native(origin, scale), yaw, false);
        let difficulty = if difficulty_name.trim().is_empty() { self.difficulty } else { Difficulty::parse(difficulty_name.trim())? };
        let (root, build_graphs) = (self.root.clone(), self.graphs.clone());
        let (animation, collision) = (self.animation.clone(), world.collision.share());
        let start_velocity = to_native(velocity, scale);
        let handle = self.start_worker(id, yaw, scale, move || {
            let mut skater = build_skater(&root, &build_graphs, difficulty, &map, collision, animation)?;
            skater.spawn_velocity = (start_velocity != [0.0; 3]).then_some(start_velocity);
            // Loading parses ~35 MB of JSON on this thread. glibc keeps
            // each thread's freed heap resident, which cost every skater
            // that much RSS for nothing; hand it back.
            release_free_heap();
            Ok(skater)
        })?;
        self.skaters.insert(id, handle);
        reply.string(&format!("skater {id} loading on {}", world.name));
        Ok(())
    }

    /// Runs skater `id` on its own thread, built there by `build`.
    fn start_worker(
        &self,
        id: u32,
        yaw: f32,
        scale: f32,
        build: impl FnOnce() -> Result<Skater, String> + Send + 'static,
    ) -> Result<SkaterHandle, String> {
        let (graphs, names) = (self.graphs.clone(), self.trick_names.clone());
        let (job_send, job_recv) = mpsc::channel::<Job>();
        let (reply_send, replies) = mpsc::channel();
        let (loaded_send, loaded) = mpsc::channel();
        std::thread::Builder::new()
            .name(format!("skater-{id}"))
            .spawn(move || {
                let mut skater = match build() {
                    Ok(skater) => skater,
                    Err(error) => {
                        let _ = loaded_send.send(Err(error));
                        return;
                    }
                };
                let _ = loaded_send.send(Ok(Pose::of(&skater, scale).angles[1]));
                while let Ok(job) = job_recv.recv() {
                    match job {
                        Job::Step(job) => {
                            let mut out = Writer::default();
                            let result = step_skater(&mut skater, &graphs, &names, scale, &job, &mut out).map(|()| out.into_bytes());
                            if reply_send.send(result).is_err() {
                                break;
                            }
                        }
                        Job::Snapshot(to) => {
                            let _ = to.send(Box::new(skater.clone()));
                        }
                        Job::Replace(copy) => skater = *copy,
                    }
                }
            })
            .map_err(|e| format!("Skater thread: {e}"))?;
        Ok(SkaterHandle {
            jobs: job_send,
            replies,
            loaded,
            ready: false,
            started: Instant::now(),
            yaw,
            in_flight: 0,
            last: None,
        })
    }

    /// POLL: whether skater `id` has finished loading (u32 1) or not (0),
    /// without stepping it.
    fn poll(&mut self, reader: &mut Reader, reply: &mut Writer) -> Result<(), String> {
        let id = reader.u32()?;
        let skater = self.skaters.get_mut(&id).ok_or_else(|| format!("POLL for unknown skater {id}"))?;
        if !skater.ready {
            match skater.loaded.try_recv() {
                Ok(Ok(_)) => skater.ready = true,
                Ok(Err(error)) => {
                    self.skaters.remove(&id);
                    return Err(error);
                }
                Err(mpsc::TryRecvError::Empty) => {}
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.skaters.remove(&id);
                    return Err("Skater thread panicked while loading".into());
                }
            }
        }
        reply.u32(u32::from(self.skaters.get(&id).is_some_and(|s| s.ready)));
        Ok(())
    }

    /// COPY: skater `dst` becomes an exact copy of `src` (created if it
    /// doesn't exist). Client prediction keeps a confirmed copy and rewinds
    /// the predicted skater to it after a misprediction.
    fn copy(&mut self, reader: &mut Reader) -> Result<(), String> {
        let (src, dst) = (reader.u32()?, reader.u32()?);
        if src == dst {
            return Ok(());
        }
        let scale = self.world.as_ref().ok_or("COPY before WORLD")?.scale;
        let source = self.skaters.get_mut(&src).ok_or_else(|| format!("COPY from unknown skater {src}"))?;
        if !source.ready {
            match source.loaded.recv() {
                Ok(Ok(_)) => source.ready = true,
                Ok(Err(error)) => return Err(error),
                Err(_) => return Err("Skater thread panicked while loading".into()),
            }
        }
        while source.in_flight > 0 {
            source.collect(true)?;
        }
        let (to, from) = mpsc::channel();
        source.jobs.send(Job::Snapshot(to)).map_err(|_| "Skater thread stopped".to_string())?;
        let copy = from.recv().map_err(|_| "Skater thread stopped".to_string())?;
        let (yaw, last) = (source.yaw, source.last.clone());
        match self.skaters.get_mut(&dst) {
            Some(target) => {
                while target.in_flight > 0 {
                    target.collect(true)?;
                }
                target.jobs.send(Job::Replace(copy)).map_err(|_| "Skater thread stopped".to_string())?;
                target.last = last;
            }
            None => {
                let mut handle = self.start_worker(dst, yaw, scale, move || Ok(*copy))?;
                handle.last = last;
                self.skaters.insert(dst, handle);
            }
        }
        Ok(())
    }

    fn step(&mut self, reader: &mut Reader, reply: &mut Writer) -> Result<(), String> {
        let id = reader.u32()?;
        let job = StepJob {
            dt: reader.f32()?,
            command: pad::Command {
                buttons: reader.u32()?,
                forward: reader.f32()?,
                side: reader.f32()?,
                mouse: [reader.f32()?, reader.f32()?],
            },
            flags: reader.u32()?,
            mouse_gain: reader.f32()?,
            mouse_decay: reader.f32()?,
            impulse: reader.vec3()?,
        };
        let world = self.world.as_ref().ok_or("STEP before WORLD")?;
        let skater = self.skaters.get_mut(&id).ok_or_else(|| format!("STEP for unknown skater {id}"))?;
        if !skater.ready {
            match skater.loaded.try_recv() {
                Ok(Ok(got)) => {
                    eprintln!(
                        "SIDECAR spawn skater {id} on {} in {:.2}s; asked yaw {:.1}, got {got:.1}",
                        world.name,
                        skater.started.elapsed().as_secs_f32(),
                        skater.yaw
                    );
                    skater.ready = true;
                }
                Ok(Err(error)) => {
                    self.skaters.remove(&id);
                    return Err(error);
                }
                Err(mpsc::TryRecvError::Empty) => {
                    reply.u32(STATE_LOADING);
                    return Ok(());
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.skaters.remove(&id);
                    return Err("Skater thread panicked while loading".into());
                }
            }
        }

        let pipelined = job.flags & STEP_PIPELINED != 0;
        // Steps stay in order: finish the one in flight first. When pipelined
        // it normally completed during the last server frame, so no wait.
        while skater.in_flight > 0 {
            skater.collect(true)?;
        }
        skater.jobs.send(Job::Step(job)).map_err(|_| "Skater thread stopped".to_string())?;
        skater.in_flight += 1;
        if !pipelined || skater.last.is_none() {
            skater.collect(true)?;
        }
        reply.raw(skater.last.as_deref().expect("a reply was collected"));
        Ok(())
    }
}

/// One usercmd for one skater: run its native ticks, write the STEP reply.
fn step_skater(
    skater: &mut Skater,
    graphs: &StockGraphs,
    trick_names: &HashMap<String, String>,
    scale: f32,
    job: &StepJob,
    reply: &mut Writer,
) -> Result<(), String> {
    let period = skater.physics.period().as_secs_f32();
    skater.accumulator = (skater.accumulator + job.dt.clamp(0.0, 0.25)).min(period * MAX_TICKS_PER_STEP as f32);
    let ticks = (skater.accumulator / period) as u32;
    skater.accumulator -= ticks as f32 * period;
    skater.pad.set_mouse_response(job.mouse_gain, job.mouse_decay);
    skater.pad.accept(&job.command, ticks);
    add_velocity(skater, to_native(job.impulse, scale));
    if job.flags & STEP_FORCE_WIPEOUT != 0 {
        // Raised as the stock "contact force too high" request; the
        // following tick's state selection turns it into a ragdoll bail.
        skater.runtime.external_wipeout = true;
    }
    for tick in 0..ticks {
        if tick + 1 == ticks {
            skater.previous = Some(Pose::of(skater, scale));
        }
        let state = skater.pad.tick();
        skater.packet = skater.packet.wrapping_add(1);
        skater.controller.collect([
            Ok(platform::DevicePacket { number: skater.packet, state, subtype: 1 }),
            Err(platform::DeviceError::Disconnected),
            Err(platform::DeviceError::Disconnected),
            Err(platform::DeviceError::Disconnected),
        ]);
        skater.controller.publish_actions();
        crate::physics::step_headless(
            &mut skater.physics,
            &mut skater.runtime,
            &mut skater.controls,
            graphs,
            skater.controller.tick_input(),
            &mut skater.camera,
        )?;
        if skater.spawn_velocity.is_some() && skater.runtime.player_state.current() as u32 != SPAWN_STATE {
            let v = skater.spawn_velocity.take().unwrap_or_default();
            add_velocity(skater, v);
        }
    }
    // One second in, the skater is riding with a full animated pose.
    if skater.packet >= 60 && skater.packet - ticks < 60 {
        if let Ok(path) = std::env::var("SKATE_DUMP_SKELETON") {
            dump_skeleton(skater, &path)?;
        }
    }
    let current = Pose::of(skater, scale);
    let pose = match &skater.previous {
        Some(previous) => previous.blend(&current, skater.accumulator / period),
        None => current,
    };
    pose.write(reply, ticks);
    write_score(skater, trick_names, reply);
    Ok(())
}

#[cfg(all(target_os = "linux", target_env = "gnu"))]
fn release_free_heap() {
    unsafe extern "C" {
        fn malloc_trim(pad: usize) -> i32;
    }
    // SAFETY: glibc's malloc_trim only returns free pages to the kernel.
    unsafe {
        malloc_trim(0);
    }
}

#[cfg(not(all(target_os = "linux", target_env = "gnu")))]
fn release_free_heap() {}

fn build_skater(
    root: &std::path::Path,
    graphs: &StockGraphs,
    difficulty: Difficulty,
    map: &skate_map::SkateMap,
    collision: skate_core::physics::board_world::BoardWorld,
    animation: Arc<crate::skater_animation::AnimationSource>,
) -> Result<Skater, String> {
    let physics = GamePhysics::load_with_shared_world(root, map, difficulty, collision)?;
    let runtime = SkaterRuntime::load_for_world(root, graphs, &physics, difficulty.profile_key(), Some(animation))?;
    let controls = PlayerControls::load(root)?;
    let names = &runtime.animation.evaluator.frames.bone_names;
    let mut joints = [0; RETARGET_JOINTS.len()];
    for (slot, wanted) in joints.iter_mut().zip(RETARGET_JOINTS) {
        *slot = names
            .iter()
            .position(|n| n.eq_ignore_ascii_case(wanted))
            .ok_or_else(|| format!("Skater rig has no {wanted} bone"))?;
    }
    Ok(Skater {
        physics,
        runtime,
        controls,
        camera: CameraRuntime::load(root)?,
        controller: ControllerInput::default(),
        pad: pad::VirtualPad::default(),
        packet: 0,
        accumulator: 0.0,
        joints,
        previous: None,
        spawn_velocity: None,
    })
}

/// English trick names from the converted HUD (`language` in trickdisplay.json).
/// Missing data is not fatal: the IDs are shown instead.
fn load_trick_names(root: &std::path::Path) -> HashMap<String, String> {
    let path = root.join("private/hud/runtime/trickdisplay.json");
    let Ok(text) = std::fs::read_to_string(&path) else {
        eprintln!("SIDECAR no trick names ({} missing)", path.display());
        return HashMap::new();
    };
    let Ok(json) = serde_json::from_str::<serde_json::Value>(&text) else {
        return HashMap::new();
    };
    json.get("language")
        .and_then(|l| l.as_object())
        .map(|language| {
            language
                .iter()
                .filter(|(k, _)| k.starts_with("ID_TRICK"))
                .filter_map(|(k, v)| Some((k.clone(), v.as_str()?.to_owned())))
                .collect()
        })
        .unwrap_or_default()
}

/// Skate's own trick recognition and scoring (ScoreModule), as the HUD sees it.
fn write_score(skater: &Skater, names: &HashMap<String, String>, reply: &mut Writer) {
    let scoring = &skater.runtime.scoring;
    let snapshot = &scoring.session.holder.snapshot;
    reply.u32(scoring.trick_seq());
    let id = scoring.trick_name();
    reply.string(names.get(id).map_or(id, String::as_str));
    reply.f32(scoring.sequence_score());
    reply.f32(snapshot.line);
    reply.f32(scoring.multiplier());
    reply.f32(snapshot.completed_lines);
    reply.u32(
        u32::from(scoring.clean())
            | u32::from(scoring.sketchy()) << 1
            | u32::from(scoring.sequence_active()) << 2,
    );
}

/// Development aid for retargeting: bone names, parents and the current
/// render pose (animation-space globals, native Y-up metres) as JSON.
fn dump_skeleton(skater: &Skater, path: &str) -> Result<(), String> {
    let frames = &skater.runtime.animation.evaluator.frames;
    let bones: Vec<_> = frames
        .bone_names
        .iter()
        .enumerate()
        .map(|(i, name)| {
            serde_json::json!({
                "index": i,
                "name": name,
                "parent": frames.parents[i],
                "global": skater.runtime.render_pose.get(i),
            })
        })
        .collect();
    let text = serde_json::to_string_pretty(&serde_json::json!({
        "animation_to_world": skater.runtime.animated_skeleton.roots.animation_to_world,
        "bones": bones,
    }))
    .map_err(|e| e.to_string())?;
    std::fs::write(path, text).map_err(|e| format!("{path}: {e}"))
}

/// One-material map holding the TF2 collision, with the spawn under the player.
/// `with_collision`: include the map's triangles. Only building the shared
/// collision world needs them; a skater's own map just carries the spawn.
fn skate_map_for(world: &World, spawn: [f32; 3], yaw_degrees: f32, with_collision: bool) -> skate_map::SkateMap {
    let triangles: &[bsp::Triangle] = if with_collision { &world.triangles } else { &[] };
    let collision = triangles
        .iter()
        .map(|points| skate_map::Collision {
            points: *points,
            // Native physics surface class (below 16; wheel votes pick one).
            // 1 is the stock default; per-material classes would go here.
            surface: 1,
            material: 1,
            native_edges: None,
        })
        .collect();
    skate_map::SkateMap {
        version: 8,
        name: world.name.clone(),
        spawn,
        // Native board At (+Z) rotated by `heading` about +Y must equal Source
        // forward (cos yaw, sin yaw, 0) -> native (cos yaw, 0, -sin yaw).
        heading: yaw_degrees.to_radians() + std::f32::consts::FRAC_PI_2,
        environment: Vec::new(),
        materials: vec![skate_map::Material {
            name: "tf2 world".into(),
            flags: 0,
            friction: 0.6,
            restitution: 0.1,
            color: [1.0; 3],
            roughness: 1.0,
            emissive: 0.0,
            textures: [0; 5],
            indirect_strength: 1.0,
            alpha_mode: 0,
            alpha_cutoff: 0.5,
            // Same audio/physics/pattern ids as the bundled format demo's floor.
            audio: 3,
            physics: 1,
            pattern: 0,
            depth_layer: None,
            retail_definition: None,
        }],
        textures: Vec::new(),
        geometry: skate_map::Geometry { vertices: Vec::new(), indices: Vec::new(), collision },
        rails: world
            .rails
            .iter()
            .map(|r| skate_map::Rail { name: r.name.clone(), closed: r.closed, points: r.points.clone(), native: None })
            .collect(),
        doors: Vec::new(),
        lights: Vec::new(),
        routes: Vec::new(),
        extensions: Vec::new(),
    }
}

/// skate_world::collision_world welds vertices to 1 mm (keeping the first
/// position seen per key) and rejects the whole map if a welded triangle has no
/// normal or a zero edge. Run the same weld and Skate's own triangle check, and
/// drop such slivers (thin bevels, T-junction fill) up front. Dropping one can
/// change which position a key keeps, so repeat until nothing changes.
fn drop_degenerate(mut triangles: Vec<bsp::Triangle>) -> Vec<bsp::Triangle> {
    use bevy::math::Vec3;
    use skate_core::{
        math::Vector3,
        physics::{board_world::WorldTriangle, collision::TriangleFeature, contact::RetailContactMaterial},
    };
    let inverse = 1.0 / f64::from(0.001_f32);
    let material = RetailContactMaterial { static_friction: 0.6, dynamic_friction: 0.6, restitution: 0.0 };
    let flags = TriangleFeature::ONE_SIDED | TriangleFeature::USE_EDGE_COSINES | 0xe0;
    loop {
        let mut welded = HashMap::<[i64; 3], [f32; 3]>::new();
        let before = triangles.len();
        triangles.retain(|t| {
            let points = t.map(|p| *welded.entry(p.map(|v| (f64::from(v) * inverse).round() as i64)).or_insert(p));
            let [a, b, c] = points.map(Vec3::from_array);
            (b - a).cross(c - a).try_normalize().is_some()
                && WorldTriangle::from_vertices(
                    points.map(|p| Vector3::new(p[0], p[1], p[2])),
                    material,
                    0,
                    flags,
                    [1.0; 3],
                    0.0,
                )
                .is_some()
        });
        if triangles.len() == before {
            return triangles;
        }
    }
}

/// Source (x, y, z) inches -> native (x, z, -y) metres. A proper rotation, so
/// triangle winding and handedness survive.
/// Adds a velocity (native m/s) to the whole skater: every board body and
/// every body of the rider's skeleton, so their relative motion, riding or
/// ragdolling, is unchanged.
fn add_velocity(skater: &mut Skater, v: [f32; 3]) {
    if v == [0.0; 3] {
        return;
    }
    let add = |velocity: &mut skate_core::math::Vector3| {
        velocity.x += v[0];
        velocity.y += v[1];
        velocity.z += v[2];
    };
    for body in skater.physics.board.bodies_mut().iter_mut() {
        add(&mut body.rates.linear_velocity);
    }
    for body in skater.runtime.skeleton.bodies_mut().iter_mut() {
        add(&mut body.rates.linear_velocity);
    }
}

fn to_native(p: [f32; 3], scale: f32) -> [f32; 3] {
    [p[0] * scale, p[2] * scale, -p[1] * scale]
}
fn to_source(p: [f32; 3], scale: f32) -> [f32; 3] {
    [p[0] / scale, -p[2] / scale, p[1] / scale]
}
fn direction_to_source(v: [f32; 3]) -> [f32; 3] {
    [v[0], -v[2], v[1]]
}

/// Source QAngle (pitch, yaw, roll) from native [right, up, at] columns.
/// Native "right" is the skater's left in a right-handed frame (right x up = at).
fn angles_from_basis(columns: [[f32; 3]; 3]) -> [f32; 3] {
    let left = direction_to_source(columns[0]);
    let up = direction_to_source(columns[1]);
    let forward = direction_to_source(columns[2]);
    // mathlib MatrixAngles with forward/left/up columns.
    let xy = (forward[0] * forward[0] + forward[1] * forward[1]).sqrt();
    let (pitch, yaw, roll) = if xy > 0.001 {
        (
            (-forward[2]).atan2(xy),
            forward[1].atan2(forward[0]),
            left[2].atan2(up[2]),
        )
    } else {
        ((-forward[2]).atan2(xy), (-left[0]).atan2(left[1]), 0.0)
    };
    [pitch, yaw, roll].map(f32::to_degrees)
}

#[derive(Clone)]
struct Pose {
    state: u32,
    origin: [f32; 3],
    angles: [f32; 3],
    velocity: [f32; 3],
    deck_origin: [f32; 3],
    deck_angles: [f32; 3],
    camera_origin: [f32; 3],
    camera_angles: [f32; 3],
    fov: f32,
    /// RETARGET_JOINTS in the skater root's frame, Source model axes
    /// (x forward, y left, z up) and units.
    joints: [[f32; 3]; RETARGET_JOINTS.len()],
}

impl Pose {
    /// Linear blend toward `next` (state from `next`); angles take the short way.
    fn blend(&self, next: &Pose, t: f32) -> Pose {
        let t = t.clamp(0.0, 1.0);
        let lerp = |a: [f32; 3], b: [f32; 3]| std::array::from_fn(|i| a[i] + (b[i] - a[i]) * t);
        let angle = |a: [f32; 3], b: [f32; 3]| {
            std::array::from_fn(|i| a[i] + ((b[i] - a[i] + 540.0).rem_euclid(360.0) - 180.0) * t)
        };
        Pose {
            state: next.state,
            origin: lerp(self.origin, next.origin),
            angles: angle(self.angles, next.angles),
            velocity: lerp(self.velocity, next.velocity),
            deck_origin: lerp(self.deck_origin, next.deck_origin),
            deck_angles: angle(self.deck_angles, next.deck_angles),
            camera_origin: lerp(self.camera_origin, next.camera_origin),
            camera_angles: angle(self.camera_angles, next.camera_angles),
            fov: self.fov + (next.fov - self.fov) * t,
            joints: std::array::from_fn(|j| lerp(self.joints[j], next.joints[j])),
        }
    }

    fn of(skater: &Skater, scale: f32) -> Self {
        let root = skater.runtime.animated_skeleton.roots.animation_to_world;
        let column = |i: usize| [root[i][0], root[i][1], root[i][2]];
        let deck = skater.physics.board.part_transforms()[BodyId::Deck.index()];
        let deck_body = skater.physics.board.bodies()[BodyId::Deck.index()];
        let v = deck_body.rates.linear_velocity;
        let camera = skater.camera.presentation_frame();
        let (camera_origin, camera_angles, fov) = match camera {
            Some(frame) => (
                to_source([frame.position[0], frame.position[1], frame.position[2]], scale),
                angles_from_basis(frame.basis.columns),
                frame.field_of_view_degrees,
            ),
            None => ([0.0; 3], [0.0; 3], 0.0),
        };
        Self {
            state: skater.runtime.player_state.current() as u32,
            origin: to_source(column(3), scale),
            angles: angles_from_basis([column(0), column(1), column(2)]),
            velocity: to_source([v.x, v.y, v.z], scale),
            deck_origin: to_source([deck.translation.x, deck.translation.y, deck.translation.z], scale),
            deck_angles: angles_from_basis(deck.basis.columns),
            camera_origin,
            camera_angles,
            fov,
            joints: skater.joints.map(|i| {
                // Animation-space globals: native [right(=left), up, at] axes.
                let p = skater.runtime.render_pose.get(i).map_or([0.0; 3], |m| [m[3][0], m[3][1], m[3][2]]);
                [p[2] / scale, p[0] / scale, p[1] / scale]
            }),
        }
    }

    fn write(&self, reply: &mut Writer, ticks: u32) {
        reply.u32(self.state);
        reply.u32(ticks);
        for v in [
            self.origin,
            self.angles,
            self.velocity,
            self.deck_origin,
            self.deck_angles,
            self.camera_origin,
            self.camera_angles,
        ] {
            reply.vec3(v);
        }
        reply.f32(self.fov);
        reply.u32(self.joints.len() as u32);
        for joint in self.joints {
            reply.vec3(joint);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coordinate_round_trip() {
        let p = [12.0, -34.0, 56.0];
        let back = to_source(to_native(p, 0.0254), 0.0254);
        for (a, b) in p.iter().zip(back) {
            assert!((a - b).abs() < 1e-3);
        }
    }

    #[test]
    fn heading_matches_source_yaw() {
        for yaw in [0.0_f32, 90.0, -135.0, 180.0] {
            let h = yaw.to_radians() + std::f32::consts::FRAC_PI_2;
            // Rotation about +Y applied to native At (+Z); +X rotates to -Z.
            let at = [h.sin(), 0.0, h.cos()];
            let right = [h.cos(), 0.0, -h.sin()];
            let angles = angles_from_basis([right, [0.0, 1.0, 0.0], at]);
            let delta = (angles[1] - yaw + 540.0).rem_euclid(360.0) - 180.0;
            assert!(delta.abs() < 1e-3, "yaw {yaw} -> {angles:?}");
            assert!(angles[0].abs() < 1e-3 && angles[2].abs() < 1e-3, "{angles:?}");
        }
    }
}
