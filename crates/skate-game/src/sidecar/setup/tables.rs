//! Small converted tables: physics-skeletons.json from OnBoard.abin (port of
//! tools/asset_pipeline/physics_skeleton.py) and the trick display names from
//! the front-end language tables (port of vendor/skate3_ui/language.py, keeping
//! only what trickdisplay.json's "language" map needs).
use serde_json::{Map, Value, json};

fn u32be(d: &[u8], at: usize) -> Result<u32, String> {
    d.get(at..at + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap())).ok_or_else(|| format!("Truncated record at {at:#x}"))
}

/// EA FastString: base-38 digits, six per word.
fn fast_name(data: &[u8], at: usize, words: usize) -> Result<String, String> {
    const ALPHABET: &[u8] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    let mut result = String::new();
    for w in 0..words {
        let mut word = u32be(data, at + 4 * w)? as u64;
        let mut divisor = 38u64.pow(5);
        for _ in 0..6 {
            let digit = word / divisor;
            word %= divisor;
            if digit == 0 {
                if word != 0 {
                    return Err("Invalid FastString padding".into());
                }
                break;
            }
            let ch = *ALPHABET.get(digit as usize - 1).ok_or("Invalid FastString digit")?;
            result.push(ch as char);
            divisor /= 38;
        }
    }
    Ok(result)
}

pub(crate) fn physics_skeletons(data: &[u8]) -> Result<Value, String> {
    let end = u32be(data, 0)? as usize;
    let version = u32be(data, 4)?;
    if version != 1 || end > data.len() {
        return Err("Invalid animation bank".into());
    }
    let mut skeletons = Vec::new();
    let mut at = 48;
    while at < end {
        let size = u32be(data, at)? as usize;
        let kind = u32be(data, at + 4)?;
        if size < 48 || size % 16 != 0 || at + size > end {
            return Err("Invalid ABIN record".into());
        }
        if kind == 5 {
            let payload = (at + 55) & !15;
            let count = u32be(data, payload)? as usize;
            let start = payload + 16;
            if start + count * 112 > data.len() {
                return Err("Truncated ABIN skeleton".into());
            }
            let mut bones = Vec::new();
            for i in 0..count {
                let pos = start + i * 112;
                let words: Result<Vec<u32>, String> = (0..28).map(|k| u32be(data, pos + 4 * k)).collect();
                bones.push(json!({ "name": fast_name(data, pos + 92, 5)?, "source_offset": pos, "words": words? }));
            }
            skeletons.push(json!({ "name": fast_name(data, at + 16, 6)?, "source_offset": at, "bones": bones }));
        }
        at += size;
    }
    if skeletons.is_empty() {
        return Err("No physics skeletons in animation bank".into());
    }
    Ok(json!({ "version": 1, "source_sha256": skate_data::sha256::digest(data), "skeletons": skeletons }))
}

/// One language pool: its null-separated byte strings, as Latin-1.
fn language_strings(data: &[u8]) -> Result<Vec<String>, String> {
    if data.len() < 28 {
        return Err("Language table shorter than its header".into());
    }
    let le = |o: usize| u32::from_le_bytes(data[o..o + 4].try_into().unwrap()) as usize;
    let (declared, header, pool) = (le(8), le(12), le(16));
    if header != 28 || pool + 8 > data.len() {
        return Err("Unsupported language table".into());
    }
    let strings: Vec<String> = data[pool + 8..].split(|&b| b == 0).map(|s| s.iter().map(|&b| b as char).collect()).collect();
    if strings.len() != declared + 2 {
        return Err(format!("Expected {} language strings, found {}", declared + 2, strings.len()));
    }
    Ok(strings)
}

/// trickdisplay.json's "language" map (label -> English text), which the
/// simulation uses for trick names.
pub(crate) fn trick_display(labels: &[u8], english: &[u8]) -> Result<Value, String> {
    let labels = language_strings(labels)?;
    let values = language_strings(english)?;
    if labels.len() != values.len() {
        return Err("Language label and value pools differ in size".into());
    }
    let mut language = Map::new();
    for (label, value) in labels.iter().zip(values) {
        language.insert(label.trim().to_string(), Value::String(value));
    }
    Ok(json!({ "language": language }))
}
