//! Skate 3's big-endian, 64-bit AttribSys schema and collections (db.big's
//! skaterschema / skatercollections .bin + .vlt pairs) -> the
//! skater-collections.json the simulation loads. Port of skate-engine
//! tools/asset_pipeline/vlt.py; produces the same JSON.
use serde_json::{Map, Value, json};
use std::collections::HashMap;

/// Field and type names the hashes are matched against (with any strings
/// found in the binaries themselves).
const NAMES: &str = include_str!("../../../../../tools/asset_pipeline/names.txt");

fn mix(mut a: u64, mut b: u64, mut c: u64) -> (u64, u64, u64) {
    a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 43);
    b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 9);
    c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 8);
    a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 38);
    b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 23);
    c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 5);
    a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 35);
    b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 49);
    c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 11);
    a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 12);
    b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 18);
    c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 22);
    (a, b, c)
}

fn le_tail(bytes: &[u8]) -> u64 {
    bytes.iter().take(8).enumerate().fold(0u64, |acc, (i, &b)| acc | (b as u64) << (8 * i))
}

/// AttribSys 64-bit string hash (lookup2 style).
pub(crate) fn hash64(text: &str) -> u64 {
    if text.is_empty() {
        return 0;
    }
    let data = text.as_bytes();
    let (mut a, mut b, mut c) = (0xABCDEF0011223344u64, 0xABCDEF0011223344u64, 0x9E3779B97F4A7C13u64);
    let mut pos = 0;
    while data.len() - pos >= 24 {
        let word = |o: usize| u64::from_le_bytes(data[pos + o..pos + o + 8].try_into().unwrap());
        (a, b, c) = mix(a.wrapping_add(word(0)), b.wrapping_add(word(8)), c.wrapping_add(word(16)));
        pos += 24;
    }
    let tail = &data[pos..];
    a = a.wrapping_add(le_tail(tail));
    b = b.wrapping_add(le_tail(tail.get(8..).unwrap_or(&[])));
    c = c.wrapping_add(data.len() as u64).wrapping_add(le_tail(tail.get(16..).unwrap_or(&[])) << 8);
    mix(a, b, c).2
}

fn take(data: &[u8], at: usize, size: usize) -> Result<&[u8], String> {
    data.get(at..at.checked_add(size).ok_or("overflow")?).ok_or_else(|| format!("Truncated AttribSys record at {at:#x}, size {size}"))
}
fn u16be(d: &[u8], at: usize) -> Result<u16, String> {
    Ok(u16::from_be_bytes(take(d, at, 2)?.try_into().unwrap()))
}
fn u32be(d: &[u8], at: usize) -> Result<u32, String> {
    Ok(u32::from_be_bytes(take(d, at, 4)?.try_into().unwrap()))
}
fn u64be(d: &[u8], at: usize) -> Result<u64, String> {
    Ok(u64::from_be_bytes(take(d, at, 8)?.try_into().unwrap()))
}
pub(crate) fn hex_upper(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02X}")).collect()
}
fn c_text(data: &[u8], at: usize) -> Result<String, String> {
    let end = data.get(at..).and_then(|d| d.iter().position(|&b| b == 0)).ok_or_else(|| format!("Unterminated VLT text at {at:#x}"))?;
    String::from_utf8(data[at..at + end].to_vec()).map_err(|e| e.to_string())
}

/// The same matches as Python's `re.findall(rb'[A-Za-z_][A-Za-z0-9_:./ -]{1,180}\x00', data)`.
fn embedded_strings(data: &[u8], out: &mut Vec<String>) {
    let first = |b: u8| b.is_ascii_alphabetic() || b == b'_';
    let rest = |b: u8| b.is_ascii_alphanumeric() || matches!(b, b'_' | b':' | b'.' | b'/' | b' ' | b'-');
    let mut s = 0;
    while s < data.len() {
        if first(data[s]) {
            let mut run = 0;
            while s + 1 + run < data.len() && run <= 180 && rest(data[s + 1 + run]) {
                run += 1;
            }
            if (1..=180).contains(&run) && data.get(s + 1 + run) == Some(&0) {
                out.push(String::from_utf8_lossy(&data[s..s + 1 + run]).into_owned());
                s += run + 2;
                continue;
            }
        }
        s += 1;
    }
}

struct Vault {
    v: Vec<u8>,
    b: Vec<u8>,
    /// (id, kind, size, offset)
    exports: Vec<(u64, u64, u32, u32)>,
}

fn vault(vlt: Vec<u8>, bin: Vec<u8>) -> Result<Vault, String> {
    let mut v = vlt;
    let mut b = bin;
    let mut exports = Vec::new();
    let mut at = 0usize;
    while at < v.len() {
        let tag = u32be(&v, at)?;
        let size = u32be(&v, at + 4)? as usize;
        if size < 8 {
            return Err("Invalid AttribSys chunk length".into());
        }
        take(&v, at, size)?;
        let (body, body_size) = (at + 8, size - 8);
        if tag == 0x5074724e {
            // "PtrN": pointer fix-ups, applied to the vault (index 0) or bin.
            let mut into_vlt = false;
            let mut p = body;
            while p < body + body_size {
                let offset = u32be(&v, p)? as usize;
                let kind = u16be(&v, p + 4)?;
                let index = u16be(&v, p + 6)?;
                let dest = u64be(&v, p + 8)?;
                p += 16;
                match kind {
                    0 => break,
                    2 => into_vlt = index == 0,
                    1 | 3 => {
                        let value = if kind == 1 { 0u32 } else { u32::try_from(dest).map_err(|_| "VLT pointer out of range")? };
                        let target = if into_vlt { &mut v } else { &mut b };
                        take(target, offset, 4)?;
                        target[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
                    }
                    4 => {}
                    other => return Err(format!("Unknown VLT pointer kind {other}")),
                }
            }
        } else if tag == 0x4578704e {
            // "ExpN": exports.
            let count = u64be(&v, body)? as usize;
            if count > body_size.saturating_sub(8) / 24 {
                return Err("Invalid VLT export count".into());
            }
            for i in 0..count {
                let e = body + 8 + i * 24;
                exports.push((u64be(&v, e)?, u64be(&v, e + 8)?, u32be(&v, e + 16)?, u32be(&v, e + 20)?));
            }
        }
        at += size;
        if tag == 0x456e6443 {
            break; // "EndC"
        }
    }
    Ok(Vault { v, b, exports })
}

#[derive(Clone, Copy)]
struct FieldDef {
    typ: u64,
    offset: u16,
    n: u16,
    flags: u8,
    alignment: u8,
}

fn array_items(data: &[u8], pos: usize, size: u16, alignment: u8) -> Result<Value, String> {
    let capacity = u16be(data, pos)? as usize;
    let count = u16be(data, pos + 2)? as usize;
    let stride = u16be(data, pos + 4)?;
    if count > capacity || stride != size || stride == 0 || alignment > 16 {
        return Err(format!("Invalid VLT array at {pos:#x}"));
    }
    let boundary = 1usize << alignment;
    let mut cursor = pos + 8;
    let mut items = Vec::new();
    for index in 0..capacity {
        cursor = (cursor + boundary - 1) & !(boundary - 1);
        let raw = take(data, cursor, stride as usize)?;
        if index < count {
            items.push(Value::String(hex_upper(raw)));
        }
        cursor += stride as usize;
    }
    Ok(json!({ "capacity": capacity, "element_size": stride, "alignment": boundary, "items": items }))
}

/// Converts the schema and collections vaults.
pub(crate) fn convert(schema: (Vec<u8>, Vec<u8>), collections: (Vec<u8>, Vec<u8>)) -> Result<Value, String> {
    let source_hash = skate_data::sha256::digest(&collections.0);
    let s = vault(schema.0, schema.1)?;
    let c = vault(collections.0, collections.1)?;
    let mut strings: Vec<String> = NAMES.lines().map(str::to_string).collect();
    embedded_strings(&s.b, &mut strings);
    embedded_strings(&c.b, &mut strings);
    let mut lookup: HashMap<u64, String> = HashMap::new();
    for text in strings {
        lookup.entry(hash64(&text)).or_insert(text);
    }
    let name = |lookup: &HashMap<u64, String>, key: u64| -> String {
        if key == 0 {
            String::new()
        } else {
            lookup.get(&key).cloned().unwrap_or_else(|| format!("Hash_{key:016X}"))
        }
    };

    // Classes: field definitions in schema order.
    let mut classes: HashMap<u64, Vec<(u64, FieldDef)>> = HashMap::new();
    for &(_, kind, _, at) in &s.exports {
        if kind != 0x2A7895AC4A876152 {
            continue;
        }
        let at = at as usize;
        let key = u64be(&s.v, at)?;
        let count = u32be(&s.v, at + 12)? as usize;
        let defs = u32be(&s.v, at + 16)? as usize;
        let mut fields: Vec<(u64, FieldDef)> = Vec::new();
        for i in 0..count {
            let d = defs + i * 24;
            let def = FieldDef {
                typ: u64be(&s.b, d + 8)?,
                offset: u16be(&s.b, d + 16)?,
                n: u16be(&s.b, d + 18)?,
                flags: *take(&s.b, d + 22, 1)?.first().unwrap(),
                alignment: *take(&s.b, d + 23, 1)?.first().unwrap(),
            };
            let fkey = u64be(&s.b, d)?;
            match fields.iter_mut().find(|(k, _)| *k == fkey) {
                Some(slot) => slot.1 = def,
                None => fields.push((fkey, def)),
            }
        }
        classes.insert(key, fields);
    }

    let mut rows = Vec::new();
    for &(_, kind, _, at) in &c.exports {
        if kind != 0xAD303B8F42B3307E {
            continue;
        }
        let at = at as usize;
        let key = u64be(&c.v, at)?;
        let class = u64be(&c.v, at + 8)?;
        let parent = u64be(&c.v, at + 16)?;
        let count = u32be(&c.v, at + 32)? as usize;
        let typeslen = u16be(&c.v, at + 38)? as usize;
        let layout = u32be(&c.v, at + 40)? as usize;
        let fields = classes.get(&class).ok_or_else(|| format!("Collection of unknown class {class:016X}"))?;
        let mut out = Map::new();

        let mut value = |lookup: &mut HashMap<u64, String>, fkey: u64, in_vlt: bool, pos: usize, inline: bool| -> Result<(), String> {
            let def = fields.iter().find(|(k, _)| *k == fkey).map(|f| f.1).ok_or_else(|| format!("Unknown schema field {fkey:x}"))?;
            let data: &[u8] = if in_vlt { &c.v } else { &c.b };
            let t = name(lookup, def.typ);
            let field_name = name(lookup, fkey);
            if t == "EA::Reflection::Text" && def.flags & 1 == 0 {
                let ptr = u32be(data, pos)? as usize;
                let raw = if ptr != 0 { c_text(&c.b, ptr)? } else { String::new() };
                if raw.is_ascii() {
                    lookup.insert(hash64(&raw), raw.clone());
                }
                out.insert(field_name, json!({ "type": t, "data": raw }));
            } else {
                let length = if inline { 4 } else { def.n as usize };
                let raw = take(data, pos, length)?;
                let mut entry = Map::new();
                entry.insert("type".into(), Value::String(t.clone()));
                entry.insert("data".into(), Value::String(hex_upper(raw)));
                if def.flags & 1 != 0 {
                    let mut array = array_items(data, pos, def.n, def.alignment)?;
                    if t == "EA::Reflection::Text" {
                        let mut texts = Vec::new();
                        for item in array["items"].as_array().cloned().unwrap_or_default() {
                            let hex = item.as_str().unwrap_or_default();
                            let ptr = u32::from_str_radix(&hex[..8.min(hex.len())], 16).unwrap_or(0) as usize;
                            texts.push(Value::String(if ptr == 0 { String::new() } else { c_text(&c.b, ptr)? }));
                        }
                        array["text_items"] = Value::Array(texts);
                    }
                    entry.insert("array".into(), array);
                }
                out.insert(field_name, Value::Object(entry));
            }
            Ok(())
        };

        if layout != 0 {
            for (fkey, def) in fields {
                if def.flags & 2 != 0 {
                    value(&mut lookup, *fkey, false, layout + def.offset as usize, false)?;
                }
            }
        }
        let entries = at + 48 + typeslen * 8;
        for i in 0..count {
            let pos = entries + i * 16;
            let fkey = u64be(&c.v, pos)?;
            let ptr = u32be(&c.v, pos + 8)? as usize;
            let def = fields.iter().find(|(k, _)| *k == fkey).map(|f| f.1).ok_or_else(|| format!("Unknown schema field {fkey:x}"))?;
            if def.n <= 4 && def.flags & 1 == 0 {
                value(&mut lookup, fkey, true, pos + 8, true)?;
            } else {
                value(&mut lookup, fkey, false, ptr, false)?;
            }
        }
        rows.push((name(&lookup, class), key, parent, out));
    }

    let collections: Vec<Value> = rows
        .into_iter()
        .map(|(class, key, parent, fields)| {
            json!({
                "class": class,
                "key": name(&lookup, key),
                "parent": name(&lookup, parent),
                "fields": fields,
                "source": "skatercollections.vlt",
                "sha256": source_hash,
            })
        })
        .collect();
    Ok(json!({ "version": 1, "collections": collections }))
}
