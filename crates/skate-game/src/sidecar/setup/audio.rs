//! Skate 3's skateboard sounds for the TF2 mod, decoded with libvgmstream
//! (.so/.dll, bundled beside libskate3; it carries FFmpeg's EA-XMA decoder).
//! Ports tools/extract_skate_sounds.py and tools/skate_audio_tables.py:
//!
//!   sound/skate/roll_<surface>.wav       wheel rolling loops (grains.big)
//!   sound/skate/banks/<bank>/NNN.wav     every sound of the foley banks
//!   sound/skate/loops/<bank>_NNN.wav     looping copies used by grinds
//!   sound/skate/skate3_events.txt        Skate 3's own event -> sample table
//!
//! Source plays 44.1/22.05/11.025 kHz only, so everything else is resampled.
use super::{big::BigArchive, disc::Disc, dynlib};
use serde_json::Value;
use std::{
    ffi::{CStr, CString, c_char, c_int, c_void},
    path::{Path, PathBuf},
    sync::atomic::{AtomicUsize, Ordering},
};

const ABK_BANKS: [&str; 14] = [
    "GRINDS.abk", "board_scrapes.abk", "Brd_Squeaks.abk", "Bodyslide.abk", "FOOT_DRAG.abk",
    "WHEEL_SKID_BANK.abk", "Sk8_Air_Flip_Tricks.abk", "fstep_skateshoe1_sm.abk",
    "Rolling_Rattles.abk", "Seams_Bank.abk", "sense_of_speed.abk",
    "PatchBank_Rolling_Surfaces.abk", "Treatments.abk", "Foley_Cloth.abk",
];
const SPLC_BANKS: [&str; 4] = ["Skate_Collisions.bnk", "Skate_Metal.bnk", "sk8_foley.bnk", "Sk82_Whsh_Bys.bnk"];
const SOURCE_RATES: [u32; 3] = [44100, 22050, 11025];

// ---------------------------------------------------------------- vgmstream

#[repr(C)]
struct VgmFormat {
    channels: c_int,
    sample_rate: c_int,
    sample_format: c_int,
    sample_size: c_int,
    channel_layout: u32,
    subsong_index: c_int,
    subsong_count: c_int,
    input_channels: c_int,
    stream_samples: i64,
    loop_start: i64,
    loop_end: i64,
    loop_flag: bool,
}
#[repr(C)]
struct VgmDecoder {
    buf: *mut c_void,
    buf_samples: c_int,
    buf_bytes: c_int,
    done: bool,
}
#[repr(C)]
struct VgmLib {
    private: *mut c_void,
    format: *const VgmFormat,
    decoder: *mut VgmDecoder,
}
#[repr(C)]
#[derive(Default)]
struct VgmConfig {
    disable_config_override: bool,
    allow_play_forever: bool,
    play_forever: bool,
    ignore_loop: bool,
    force_loop: bool,
    really_force_loop: bool,
    ignore_fade: bool,
    loop_count: f64,
    fade_time: f64,
    fade_delay: f64,
    stereo_track: c_int,
    auto_downmix_channels: c_int,
    force_sfmt: c_int,
}

struct Vgm {
    open_stdio: unsafe extern "C" fn(*const c_char) -> *mut c_void,
    close_file: unsafe extern "C" fn(*mut c_void),
    create: unsafe extern "C" fn(*mut c_void, c_int, *mut VgmConfig) -> *mut VgmLib,
    render: unsafe extern "C" fn(*mut VgmLib) -> c_int,
    free: unsafe extern "C" fn(*mut VgmLib),
}

impl Vgm {
    fn load() -> Result<Self, String> {
        let candidates: Vec<PathBuf> = std::env::var_os("SKATE_VGMSTREAM")
            .map(PathBuf::from)
            .into_iter()
            .chain(dynlib::own_folder().map(|f| f.join(dynlib::VGMSTREAM)))
            .collect();
        let mut errors = Vec::new();
        for path in candidates {
            let library = match dynlib::open(&path) {
                Ok(library) => library,
                Err(error) => {
                    errors.push(error);
                    continue;
                }
            };
            // SAFETY: the signatures match libvgmstream.h (API 1.1).
            return unsafe {
                Ok(Self {
                    open_stdio: std::mem::transmute(library.symbol("libstreamfile_open_from_stdio")?),
                    close_file: std::mem::transmute(library.symbol("libstreamfile_close")?),
                    create: std::mem::transmute(library.symbol("libvgmstream_create")?),
                    render: std::mem::transmute(library.symbol("libvgmstream_render")?),
                    free: std::mem::transmute(library.symbol("libvgmstream_free")?),
                })
            };
        }
        Err(format!("Couldn't load {} (build it with ./build-vgmstream.sh): {}", dynlib::VGMSTREAM, errors.join("; ")))
    }

    /// How many subsongs a file holds (0 if vgmstream can't open it).
    fn subsongs(&self, file: &Path) -> i32 {
        let Ok(c) = CString::new(file.to_string_lossy().as_bytes()) else { return 0 };
        // SAFETY: libvgmstream API; the stream is freed right away.
        unsafe {
            let sf = (self.open_stdio)(c.as_ptr());
            if sf.is_null() {
                return 0;
            }
            let mut config = VgmConfig { ignore_loop: true, force_sfmt: 1, ..VgmConfig::default() };
            let lib = (self.create)(sf, 0, &mut config);
            (self.close_file)(sf);
            if lib.is_null() {
                return 0;
            }
            let count = (*(*lib).format).subsong_count.max(1);
            (self.free)(lib);
            count
        }
    }

    /// Decodes one (sub)song once through to interleaved 16-bit PCM.
    fn decode(&self, file: &Path, subsong: i32) -> Result<Decoded, String> {
        let c = CString::new(file.to_string_lossy().as_bytes()).map_err(|e| e.to_string())?;
        // SAFETY: libvgmstream API; every handle is closed/freed below.
        unsafe {
            let sf = (self.open_stdio)(c.as_ptr());
            if sf.is_null() {
                return Err(format!("vgmstream can't open {}", file.display()));
            }
            let mut config = VgmConfig { ignore_loop: true, force_sfmt: 1, ..VgmConfig::default() };
            let lib = (self.create)(sf, subsong, &mut config);
            (self.close_file)(sf);
            if lib.is_null() {
                return Err(format!("vgmstream doesn't recognise {} (subsong {subsong})", file.display()));
            }
            let format = &*(*lib).format;
            let (channels, rate) = (format.channels.max(1) as usize, format.sample_rate as u32);
            // With ignore_loop the flag reads false, but defined loop points
            // stay set ("looping forcefully disabled").
            let looped = (format.loop_end > format.loop_start && format.loop_start >= 0)
                .then_some((format.loop_start as usize, format.loop_end as usize));
            let mut pcm = Vec::new();
            loop {
                if (self.render)(lib) < 0 {
                    (self.free)(lib);
                    return Err(format!("vgmstream failed decoding {}", file.display()));
                }
                let decoder = &*(*lib).decoder;
                if decoder.buf_bytes > 0 && !decoder.buf.is_null() {
                    let samples = std::slice::from_raw_parts(decoder.buf as *const i16, decoder.buf_bytes as usize / 2);
                    pcm.extend_from_slice(samples);
                }
                if decoder.done {
                    break;
                }
            }
            (self.free)(lib);
            Ok(Decoded { channels, rate, pcm, looped })
        }
    }
}

struct Decoded {
    channels: usize,
    rate: u32,
    pcm: Vec<i16>,
    /// The sound's own loop (start, end) in frames, if it has one.
    looped: Option<(usize, usize)>,
}

// ---------------------------------------------------------------- PCM / WAV

/// Windowed-sinc resampler (Blackman, 16 zero crossings).
fn resample(input: &[i16], channels: usize, from: u32, to: u32) -> Vec<i16> {
    let frames = input.len() / channels;
    let ratio = to as f64 / from as f64;
    let cutoff = ratio.min(1.0) * 0.97;
    let half = (16.0 / cutoff).ceil() as i64;
    let out_frames = ((frames as f64) * ratio).ceil() as usize;
    let mut out = vec![0i16; out_frames * channels];
    let blackman = |x: f64| 0.42 + 0.5 * (std::f64::consts::PI * x).cos() + 0.08 * (2.0 * std::f64::consts::PI * x).cos();
    for n in 0..out_frames {
        let t = n as f64 / ratio;
        let centre = t.floor() as i64;
        let mut acc = vec![0f64; channels];
        let mut weight_sum = 0.0;
        for k in centre - half + 1..=centre + half {
            if k < 0 || k as usize >= frames {
                continue;
            }
            let x = t - k as f64;
            let arg = std::f64::consts::PI * cutoff * x;
            let sinc = if arg.abs() < 1e-9 { 1.0 } else { arg.sin() / arg };
            let w = sinc * blackman((x / half as f64).clamp(-1.0, 1.0));
            weight_sum += w;
            for (c, a) in acc.iter_mut().enumerate() {
                *a += w * input[k as usize * channels + c] as f64;
            }
        }
        let norm = if weight_sum.abs() > 1e-9 { 1.0 / weight_sum } else { 0.0 };
        for (c, a) in acc.iter().enumerate() {
            out[n * channels + c] = (a * norm).round().clamp(-32768.0, 32767.0) as i16;
        }
    }
    out
}

/// 16-bit PCM WAV. A 'cue ' point at frame `loop_from` makes Source loop
/// from there to the end.
fn write_wav(path: &Path, channels: usize, rate: u32, pcm: &[i16], loop_from: Option<u32>) -> Result<(), String> {
    let data_bytes = pcm.len() * 2;
    let cue_bytes = if loop_from.is_some() { 36 } else { 0 };
    let mut out = Vec::with_capacity(44 + data_bytes + cue_bytes);
    out.extend(b"RIFF");
    out.extend(((36 + data_bytes + cue_bytes) as u32).to_le_bytes());
    out.extend(b"WAVEfmt ");
    out.extend(16u32.to_le_bytes());
    out.extend(1u16.to_le_bytes());
    out.extend((channels as u16).to_le_bytes());
    out.extend(rate.to_le_bytes());
    out.extend((rate * channels as u32 * 2).to_le_bytes());
    out.extend((channels as u16 * 2).to_le_bytes());
    out.extend(16u16.to_le_bytes());
    out.extend(b"data");
    out.extend((data_bytes as u32).to_le_bytes());
    for s in pcm {
        out.extend(s.to_le_bytes());
    }
    if let Some(start) = loop_from {
        // One cue point: name 1, position and sample offset `start`.
        out.extend(b"cue ");
        for v in [28u32, 1, 1, start] {
            out.extend(v.to_le_bytes());
        }
        out.extend(b"data");
        for v in [0u32, 0, start] {
            out.extend(v.to_le_bytes());
        }
    }
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    std::fs::write(path, out).map_err(|e| format!("{}: {e}", path.display()))
}

// ---------------------------------------------------------------- EA framing

/// A .grain: grain table, then an EA SNR header and one SNS block (EA-XMA).
/// Rewrapped as an EA "SPS" stream, which vgmstream decodes.
fn grain_to_sps(grain: &[u8]) -> Result<Vec<u8>, String> {
    let start = u32::from_be_bytes(grain.get(0..4).ok_or("short grain")?.try_into().unwrap()) as usize;
    let snr = grain.get(start..start + 8).ok_or("grain SNR out of range")?;
    if snr[0] >> 4 != 0 || snr[0] & 0x0F != 3 {
        return Err("unexpected grain SNR header".into());
    }
    let mut out = vec![0x48, 0, 0, 12];
    out.extend(snr);
    out.extend(&grain[start + 8..]);
    out.extend([0x45, 0, 0, 4]);
    Ok(out)
}

/// SPS streams for each sound of an SPLC bank (see extract_skate_sounds.py).
fn splc_sounds(bank: &[u8]) -> Result<Vec<Vec<u8>>, String> {
    if bank.get(..4) != Some(b"SPLC") {
        return Err("not an SPLC bank".into());
    }
    let be32 = |o: usize| u32::from_be_bytes(bank[o..o + 4].try_into().unwrap());
    let mut sounds = Vec::new();
    let mut i = be32(8) as usize;
    while i + 16 <= bank.len() {
        let word = be32(i);
        let samples = be32(i + 4) & 0x1FFF_FFFF;
        if word >> 28 == 0 && (word >> 24) & 0xF == 3 && [22050, 24000, 32000, 44100, 48000].contains(&(word & 0x3FFFF)) && samples != 0 {
            let (mut j, mut covered) = (i + 8, 0u64);
            while covered < samples as u64 && j + 8 <= bank.len() {
                let flag = bank[j];
                let size = ((bank[j + 1] as usize) << 16) | ((bank[j + 2] as usize) << 8) | bank[j + 3] as usize;
                let block_samples = be32(j + 4);
                if !(flag == 0x00 || flag == 0x80) || size < 8 || j + size > bank.len() || block_samples == 0 {
                    break;
                }
                covered += block_samples as u64;
                j += size;
            }
            if covered == samples as u64 {
                let mut sps = vec![0x48, 0, 0, 12];
                sps.extend(&bank[i..i + 8]);
                sps.extend(&bank[i + 8..j]);
                sps.extend([0x45, 0, 0, 4]);
                sounds.push(sps);
                i = j;
                continue;
            }
        }
        i += 1;
    }
    Ok(sounds)
}

// ---------------------------------------------------------------- jobs

struct Job {
    input: PathBuf,
    subsong: i32,
    output: PathBuf,
    looped: bool,
}

fn run_jobs(vgm: &Vgm, jobs: &[Job], report: &(dyn Fn(String, f32) + Sync), first: f32, span: f32) -> (usize, Vec<String>) {
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let failures = std::sync::Mutex::new(Vec::new());
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).min(16);
    std::thread::scope(|scope| {
        for _ in 0..threads {
            scope.spawn(|| loop {
                let index = next.fetch_add(1, Ordering::Relaxed);
                let Some(job) = jobs.get(index) else { break };
                let result = vgm.decode(&job.input, job.subsong).and_then(|d| {
                    let Decoded { channels, rate, mut pcm, looped } = d;
                    // A sound with its own loop ends at its loop end. Only
                    // looping outputs get a cue (from the loop start, else
                    // 0): in Source a cue loops even a one-shot EmitSound,
                    // so the bank files stay one-shots and grinds use the
                    // loops/ copies.
                    let mut loop_from = job.looped.then_some(0usize);
                    if let Some((start, end)) = looped {
                        pcm.truncate(end.min(pcm.len() / channels) * channels);
                        if job.looped {
                            loop_from = Some(start.min(pcm.len() / channels));
                        }
                    }
                    let (out_rate, pcm, loop_from) = if SOURCE_RATES.contains(&rate) {
                        (rate, pcm, loop_from)
                    } else {
                        let scale = 44100.0 / rate as f64;
                        (44100, resample(&pcm, channels, rate, 44100), loop_from.map(|f| (f as f64 * scale).round() as usize))
                    };
                    write_wav(&job.output, channels, out_rate, &pcm, loop_from.map(|f| f as u32))
                });
                if let Err(error) = result {
                    failures.lock().unwrap().push(error);
                }
                let finished = done.fetch_add(1, Ordering::Relaxed) + 1;
                if finished % 64 == 0 || finished == jobs.len() {
                    report(format!("Decoding sounds: {finished}/{}", jobs.len()), first + span * finished as f32 / jobs.len() as f32);
                }
            });
        }
    });
    let failures = failures.into_inner().unwrap();
    (jobs.len() - failures.len(), failures)
}

/// Extracts and decodes everything into `sound_dir` (…/mod_tf/sound/skate),
/// then writes skate3_events.txt from the collections. Progress spans
/// `first`..`first + span`.
pub(crate) fn convert(
    disc: &Disc,
    sound_dir: &Path,
    temp: &Path,
    collections: &Value,
    report: &(dyn Fn(String, f32) + Sync),
    first: f32,
    span: f32,
) -> Result<String, String> {
    let vgm = Vgm::load()?;
    std::fs::create_dir_all(temp).map_err(|e| e.to_string())?;
    // Start clean: everything here but the sound picker's events.txt/.json
    // is the converter's. Source looks sounds up in lowercase on Linux, so
    // every path written below is lowercase too.
    let _ = std::fs::remove_dir_all(sound_dir.join("banks"));
    let _ = std::fs::remove_dir_all(sound_dir.join("loops"));
    if let Ok(entries) = std::fs::read_dir(sound_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if (name.starts_with("roll_") && name.ends_with(".wav")) || name == "skate3_events.txt" {
                let _ = std::fs::remove_file(entry.path());
            }
        }
    }
    std::fs::create_dir_all(sound_dir).map_err(|e| e.to_string())?;
    let mut jobs = Vec::new();

    report("Reading sound archives".into(), first);
    let grains = BigArchive::open(disc, "data/audio/grains.big")?;
    for entry in &grains.entries {
        let stem = Path::new(&entry.path).file_stem().map(|s| s.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
        let Ok(sps) = grain_to_sps(&grains.read(entry)?) else { continue };
        let input = temp.join(format!("grain_{stem}.sps"));
        std::fs::write(&input, sps).map_err(|e| e.to_string())?;
        jobs.push(Job { input, subsong: 0, output: sound_dir.join(format!("roll_{stem}.wav")), looped: true });
    }

    let files = BigArchive::open(disc, "data/audio/audiofiles.big")?;
    let by_name = |name: &str| files.entries.iter().find(|e| e.path.rsplit('/').next().is_some_and(|n| n.eq_ignore_ascii_case(name)));
    for bank in ABK_BANKS {
        let Some(entry) = by_name(bank) else { continue };
        let input = temp.join(bank);
        std::fs::write(&input, files.read(entry)?).map_err(|e| e.to_string())?;
        let stem = bank.trim_end_matches(".abk").to_ascii_lowercase();
        let count = vgm.subsongs(&input);
        for n in 1..=count {
            jobs.push(Job { input: input.clone(), subsong: n, output: sound_dir.join(format!("banks/{stem}/{n:03}.wav")), looped: false });
        }
    }
    let mut splc = Vec::new();
    for bank in SPLC_BANKS {
        let Some(entry) = by_name(bank) else { continue };
        let data = files.read(entry)?;
        let stem = bank.trim_end_matches(".bnk").to_ascii_lowercase();
        for (n, sps) in splc_sounds(&data)?.into_iter().enumerate() {
            let input = temp.join(format!("{stem}_{:03}.sps", n + 1));
            std::fs::write(&input, sps).map_err(|e| e.to_string())?;
            jobs.push(Job { input, subsong: 0, output: sound_dir.join(format!("banks/{stem}/{:03}.wav", n + 1)), looped: false });
        }
        splc.push((bank.trim_end_matches(".bnk").to_string(), data));
    }

    let (decoded, failures) = run_jobs(&vgm, &jobs, report, first, span * 0.95);
    if decoded == 0 {
        return Err(format!("No sounds decoded: {}", failures.first().cloned().unwrap_or_default()));
    }
    let events = events::write(sound_dir, &splc, collections)?;
    let _ = std::fs::remove_dir_all(temp);
    Ok(format!("{decoded} sounds ({} failed), {events} Skate 3 sound events", failures.len()))
}

mod events {
    //! sound/skate/skate3_events.txt: Skate 3's riding and grind sound events
    //! -> SPLC samples. Port of tools/skate_audio_tables.py (see
    //! docs/skate3-audio-re.md for how the table was recovered).
    use serde_json::Value;
    use std::path::Path;

    /// Riding audio fields of collection class C26949FCB638A2CA.
    const RIDING: [(&str, &str); 9] = [
        ("pop_hard", "3C1E3B965A93594A"),
        ("pop_wood", "84DFABF76D821DEC"),
        ("land_hard", "F262042EAA295711"),
        ("land_hard_big", "3A2F1C788E21D92D"),
        ("land_wood", "1E86469556ACD80A"),
        ("land_wood_big", "BF22DD8BC69DDE53"),
        ("takeoff_rattle", "9C4CDCF0DD84C281"),
        ("impact_hard", "5A93802D11B00173"),
        ("impact_wood", "797EC34502499EC3"),
    ];
    /// Grind sounds per surface: (bank, start, loop, slide) splice IDs.
    const GRINDS: [(&str, &str, u32, u32, u32); 8] = [
        ("metal_round", "Skate_Metal", 0x20C, 0x18F, 0x18F),
        ("metal_square", "Skate_Metal", 0x1BC, 0x1BD, 0x1BD),
        ("metal_hollow", "Skate_Metal", 0x1D5, 0x1D6, 0x1D6),
        ("metal_sheet", "Skate_Metal", 0x1B4, 0x1B5, 0x1B5),
        ("metal_coarse", "Skate_Metal", 0x192, 0x193, 0x193),
        ("wood", "Skate_Collisions", 0x388, 0x389, 0x38B),
        ("plastic", "Skate_Collisions", 0x38D, 0x38E, 0x390),
        ("metal_ramp", "Skate_Collisions", 0x402, 0x403, 0x403),
    ];

    struct Splc<'a> {
        d: &'a [u8],
        n_refs: usize,
        n_samples: usize,
        clusters_base: usize,
        layer_start: Vec<usize>,
    }
    impl<'a> Splc<'a> {
        fn new(d: &'a [u8]) -> Result<Self, String> {
            let be32 = |o: usize| d.get(o..o + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap()) as usize).ok_or("SPLC truncated");
            let (n_refs, n_clusters, n_samples) = (be32(0xC)?, be32(0x10)?, be32(0x18)?);
            let clusters_base = 0x3C + 36 * n_refs;
            let mut p = clusters_base + 72 * n_clusters;
            let mut layer_start = Vec::with_capacity(n_refs);
            for i in 0..n_refs {
                layer_start.push(p);
                for _ in 0..*d.get(0x3C + 36 * i + 7).ok_or("SPLC truncated")? {
                    p += 12 + 72 * *d.get(p + 8).ok_or("SPLC truncated")? as usize;
                }
            }
            Ok(Self { d, n_refs, n_samples, clusters_base, layer_start })
        }
        fn be16(&self, o: usize) -> u16 {
            u16::from_be_bytes([self.d[o], self.d[o + 1]])
        }
        fn f32(&self, o: usize) -> f32 {
            f32::from_bits(u32::from_be_bytes(self.d[o..o + 4].try_into().unwrap()))
        }
    }

    struct Sample {
        wave: String,
        gain: f32,
        pitch: f32,
        delay: f32,
        chance: f32,
    }

    /// [variant][layer][sample] for one splice ID.
    fn variants(sound_dir: &Path, bank_name: &str, bank: &Splc, splice: usize, looped: bool) -> Result<Vec<Vec<Vec<Sample>>>, String> {
        let refs: Vec<usize> = if splice < bank.n_refs {
            vec![splice]
        } else {
            let o = bank.clusters_base + 72 * (splice - bank.n_refs);
            (0..32).map(|k| bank.be16(o + 4 + 2 * k)).filter(|&r| r != 0xFFFF).map(|r| r as usize).collect()
        };
        let mut out = Vec::new();
        for r in refs {
            let mut layers = Vec::new();
            let mut p = bank.layer_start[r];
            for _ in 0..bank.d[0x3C + 36 * r + 7] {
                let count = bank.d[p + 8] as usize;
                let mut layer = Vec::new();
                for k in 0..count {
                    let s = p + 12 + 72 * k;
                    let sample = bank.be16(s) as usize;
                    if sample >= bank.n_samples {
                        continue;
                    }
                    let wave = format!("skate/banks/{}/{:03}.wav", bank_name.to_ascii_lowercase(), sample + 1);
                    let wave = if looped { loop_copy(sound_dir, bank_name, sample, &wave)? } else { wave };
                    layer.push(Sample {
                        wave,
                        gain: bank.f32(s + 4),
                        pitch: bank.f32(s + 8),
                        delay: bank.f32(s + 0x14).max(0.0),
                        chance: bank.f32(s + 0x40),
                    });
                }
                if !layer.is_empty() {
                    layers.push(layer);
                }
                p += 12 + 72 * count;
            }
            if !layers.is_empty() {
                out.push(layers);
            }
        }
        Ok(out)
    }

    /// A copy of a sample with a 'cue ' point, so Source loops it.
    fn loop_copy(sound_dir: &Path, bank_name: &str, sample: usize, wave: &str) -> Result<String, String> {
        let relative = format!("skate/loops/{}_{:03}.wav", bank_name.to_ascii_lowercase(), sample + 1);
        let source = sound_dir.parent().unwrap_or(sound_dir).join(wave);
        let target = sound_dir.parent().unwrap_or(sound_dir).join(&relative);
        let mut data = std::fs::read(&source).map_err(|e| format!("{}: {e}", source.display()))?;
        if !data.windows(4).any(|w| w == b"cue ") {
            data.extend(b"cue ");
            for v in [28u32, 1, 1, 0] {
                data.extend(v.to_le_bytes());
            }
            data.extend(b"data");
            data.extend([0u8; 12]);
            let riff = (data.len() - 8) as u32;
            data[4..8].copy_from_slice(&riff.to_le_bytes());
        }
        std::fs::create_dir_all(target.parent().unwrap()).map_err(|e| e.to_string())?;
        std::fs::write(&target, data).map_err(|e| e.to_string())?;
        Ok(relative)
    }

    fn round3(v: f32) -> String {
        let r = (v as f64 * 1000.0).round() / 1000.0;
        let text = format!("{r}");
        if text.contains('.') || text.contains('e') { text } else { format!("{text}.0") }
    }

    pub(super) fn write(sound_dir: &Path, banks: &[(String, Vec<u8>)], collections: &Value) -> Result<usize, String> {
        let bank = |name: &str| banks.iter().find(|(n, _)| n == name).map(|(_, d)| d.as_slice());
        let riding = collections["collections"]
            .as_array()
            .and_then(|rows| rows.iter().find(|c| c["class"] == "Hash_C26949FCB638A2CA"))
            .map(|c| &c["fields"])
            .ok_or("No riding audio collection")?;
        let mut events: Vec<(String, Vec<Vec<Vec<Sample>>>)> = Vec::new();
        for (event, field) in RIDING {
            let f = &riding[format!("Hash_{field}")];
            let bank_name = f["type"].as_str().ok_or_else(|| format!("No riding field {field}"))?;
            let Some(data) = bank(bank_name) else { continue };
            let splc = Splc::new(data)?;
            let mut values: Vec<usize> = match f["array"]["items"].as_array() {
                Some(items) => items.iter().filter_map(|v| usize::from_str_radix(v.as_str()?, 16).ok()).collect(),
                None => f["data"].as_str().and_then(|d| usize::from_str_radix(d, 16).ok()).into_iter().collect(),
            };
            if event.starts_with("impact_") {
                values.truncate(3);
            }
            let single = values.len() == 1;
            for (tier, value) in values.into_iter().enumerate() {
                let name = if single { event.to_string() } else { format!("{event}_{tier}") };
                events.push((name, variants(sound_dir, bank_name, &splc, value, false)?));
            }
        }
        for (surface, bank_name, start, looped, slide) in GRINDS {
            let Some(data) = bank(bank_name) else { continue };
            let splc = Splc::new(data)?;
            events.push((format!("grind_start_{surface}"), variants(sound_dir, bank_name, &splc, start as usize, false)?));
            events.push((format!("grind_loop_{surface}"), variants(sound_dir, bank_name, &splc, looped as usize, true)?));
            events.push((format!("slide_loop_{surface}"), variants(sound_dir, bank_name, &splc, slide as usize, true)?));
        }

        let mut text = String::from("\"Skate3Sounds\"\n{\n");
        for (name, variants) in &events {
            text += &format!("\t\"{name}\"\n\t{{\n");
            for layers in variants {
                text += "\t\t\"variant\"\n\t\t{\n";
                for layer in layers {
                    text += "\t\t\t\"layer\"\n\t\t\t{\n";
                    for s in layer {
                        text += &format!(
                            "\t\t\t\t\"sample\" {{ \"wave\" \"{}\" \"gain\" \"{}\" \"pitch\" \"{}\" \"delay\" \"{}\" \"chance\" \"{}\" }}\n",
                            s.wave,
                            round3(s.gain),
                            round3(s.pitch),
                            round3(s.delay),
                            round3(s.chance)
                        );
                    }
                    text += "\t\t\t}\n";
                }
                text += "\t\t}\n";
            }
            text += "\t}\n";
        }
        text += "}\n";
        std::fs::write(sound_dir.join("skate3_events.txt"), text).map_err(|e| e.to_string())?;
        Ok(events.len())
    }
}
