//! EB BIG v3 archives (Skate 3's .big files), read in place from the disc.
//! Port of skate-engine tools/owned_game/big.py and refpack.py.
use super::disc::Disc;
use std::io::Read;

pub(crate) struct BigEntry {
    /// As stored (original case), forward slashes.
    pub path: String,
    offset: u64,
    stored_size: u64,
    pub unpacked_size: u64,
    compression: u8,
}

pub(crate) struct BigArchive<'a> {
    disc: &'a Disc,
    disc_path: String,
    pub entries: Vec<BigEntry>,
}

fn be16(d: &[u8], o: usize) -> Result<u16, String> {
    d.get(o..o + 2).map(|b| u16::from_be_bytes([b[0], b[1]])).ok_or_else(|| "BIG: truncated".into())
}
fn be32(d: &[u8], o: usize) -> Result<u32, String> {
    d.get(o..o + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap())).ok_or_else(|| "BIG: truncated".into())
}
fn align(value: usize, alignment: usize) -> usize {
    (value + alignment - 1) & !(alignment - 1)
}

impl<'a> BigArchive<'a> {
    pub fn open(disc: &'a Disc, disc_path: &str) -> Result<Self, String> {
        let file_size = disc.size(disc_path)?;
        let header = disc.read_at(disc_path, 0, 48.min(file_size))?;
        if header.len() < 48 {
            return Err(format!("{disc_path}: truncated BIG header"));
        }
        let index_size = be32(&header, 12)? as u64;
        let names_size = be32(&header, 16)? as u64;
        if index_size + names_size > file_size {
            return Err(format!("{disc_path}: BIG index exceeds the file"));
        }
        let meta = disc.read_at(disc_path, 0, index_size + names_size)?;
        if be16(&meta, 0)? != 0x4542 || be16(&meta, 2)? != 3 {
            return Err(format!("{disc_path}: not an EB BIG v3 archive"));
        }
        let count = be32(&meta, 4)? as usize;
        if count > 1_000_000 {
            return Err(format!("{disc_path}: unreasonable entry count"));
        }
        let flags = be16(&meta, 8)?;
        let shift = meta[10] as u32;
        let name_record = meta[20] as usize;
        let directory_record = meta[21] as usize;
        if shift > 31 || name_record < 3 || directory_record < 2 {
            return Err(format!("{disc_path}: invalid BIG layout"));
        }
        let entry_size = if flags & 1 != 0 { 20 } else { 16 };
        let entries_start = 48;
        let compression_start = entries_start + align(entry_size * count, 16);
        let names_start = index_size as usize;
        let directories_start = names_start + align(name_record * count, 16);
        let directories_end = (index_size + names_size) as usize;
        let mut directories = Vec::new();
        let mut cursor = directories_start;
        while cursor + directory_record <= directories_end {
            directories.push(c_string(&meta[cursor..cursor + directory_record]));
            cursor += directory_record;
        }
        let mut entries = Vec::with_capacity(count);
        for index in 0..count {
            let at = entries_start + index * entry_size;
            let offset = (be32(&meta, at)? as u64) << shift;
            let declared = be32(&meta, at + 4)? as u64;
            let unpacked = match be32(&meta, at + 8)? as u64 { 0 => declared, n => n };
            let stored = if declared == 0 { unpacked } else { declared };
            let compression = *meta.get(compression_start + index).ok_or("BIG: truncated")?;
            let record = names_start + index * name_record;
            let directory_index = be16(&meta, record)? as usize;
            let filename = c_string(meta.get(record + 2..record + name_record).ok_or("BIG: truncated")?);
            let directory = directories.get(directory_index).map(String::as_str).unwrap_or(".");
            let path = if directory.is_empty() || directory == "." { filename } else { format!("{directory}/{filename}") };
            if offset + stored > file_size {
                return Err(format!("{disc_path}: {path} exceeds the archive"));
            }
            entries.push(BigEntry { path: path.replace('\\', "/"), offset, stored_size: stored, unpacked_size: unpacked, compression });
        }
        Ok(Self { disc, disc_path: disc_path.to_string(), entries })
    }

    pub fn find(&self, path: &str) -> Option<&BigEntry> {
        self.entries.iter().find(|e| e.path.eq_ignore_ascii_case(path))
    }

    pub fn read(&self, entry: &BigEntry) -> Result<Vec<u8>, String> {
        let packed = self.disc.read_at(&self.disc_path, entry.offset, entry.stored_size)?;
        let expected = entry.unpacked_size as usize;
        let decoded = match entry.compression {
            0 => packed,
            1 => refpack(&packed, Some(expected))?,
            2..=4 => chunkref(&packed, expected)?,
            other => return Err(format!("{}: unsupported BIG compression {other}", entry.path)),
        };
        if decoded.len() != expected {
            return Err(format!("{}: decoded {} bytes, expected {expected}", entry.path, decoded.len()));
        }
        Ok(decoded)
    }

    pub fn read_path(&self, path: &str) -> Result<Vec<u8>, String> {
        let entry = self.find(path).ok_or_else(|| format!("{} has no {path}", self.disc_path))?;
        self.read(entry)
    }
}

fn c_string(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

/// EA RefPack (0x10FB / 0x90FB headers, or headerless).
pub(crate) fn refpack(data: &[u8], expected: Option<usize>) -> Result<Vec<u8>, String> {
    if data.len() < 2 {
        return Err("RefPack stream is too short".into());
    }
    let mut pos = 0usize;
    let mut header_size = None;
    if data[1] == 0xFB && (data[0] == 0x10 || data[0] == 0x90) {
        if data[0] & 0x80 != 0 {
            header_size = Some(u32::from_be_bytes(data.get(2..6).ok_or("truncated RefPack header")?.try_into().unwrap()) as usize);
            pos = 6;
        } else {
            let b = data.get(2..5).ok_or("truncated RefPack header")?;
            header_size = Some(((b[0] as usize) << 16) | ((b[1] as usize) << 8) | b[2] as usize);
            pos = 5;
        }
    }
    let expected = match (expected, header_size) {
        (Some(e), Some(h)) if e != h => return Err(format!("RefPack size {h} differs from archive size {e}")),
        (Some(e), _) => Some(e),
        (None, h) => h,
    };
    let mut out: Vec<u8> = Vec::with_capacity(expected.unwrap_or(data.len() * 4));
    let literal = |out: &mut Vec<u8>, pos: &mut usize, count: usize| -> Result<(), String> {
        let bytes = data.get(*pos..*pos + count).ok_or("truncated RefPack literal")?;
        out.extend_from_slice(bytes);
        *pos += count;
        Ok(())
    };
    let backref = |out: &mut Vec<u8>, distance: usize, count: usize| -> Result<(), String> {
        if distance == 0 || distance > out.len() {
            return Err("RefPack back-reference out of range".into());
        }
        let start = out.len() - distance;
        for i in 0..count {
            let b = out[start + i];
            out.push(b);
        }
        Ok(())
    };
    while pos < data.len() {
        let control = data[pos] as usize;
        pos += 1;
        if control < 0x80 {
            let b1 = *data.get(pos).ok_or("truncated RefPack command")? as usize;
            pos += 1;
            literal(&mut out, &mut pos, control & 3)?;
            backref(&mut out, ((control & 0x60) << 3) + b1 + 1, ((control >> 2) & 7) + 3)?;
        } else if control < 0xC0 {
            let b = data.get(pos..pos + 2).ok_or("truncated RefPack command")?;
            let (b1, b2) = (b[0] as usize, b[1] as usize);
            pos += 2;
            literal(&mut out, &mut pos, b1 >> 6)?;
            backref(&mut out, ((b1 & 0x3F) << 8) + b2 + 1, (control & 0x3F) + 4)?;
        } else if control < 0xE0 {
            let b = data.get(pos..pos + 3).ok_or("truncated RefPack command")?;
            let (b1, b2, b3) = (b[0] as usize, b[1] as usize, b[2] as usize);
            pos += 3;
            literal(&mut out, &mut pos, control & 3)?;
            backref(&mut out, ((control & 0x10) << 12) + (b1 << 8) + b2 + 1, ((control & 0x0C) << 6) + b3 + 5)?;
        } else if control < 0xFC {
            literal(&mut out, &mut pos, ((control & 0x1F) << 2) + 4)?;
        } else {
            literal(&mut out, &mut pos, control & 3)?;
            break;
        }
        if expected.is_some_and(|e| out.len() > e) {
            return Err("RefPack output exceeds its declared size".into());
        }
    }
    if let Some(e) = expected {
        if out.len() != e {
            return Err(format!("RefPack produced {} bytes, expected {e}", out.len()));
        }
    }
    Ok(out)
}

/// "chunkref" v2: chunks stored raw, RefPack or zlib.
fn chunkref(data: &[u8], expected: usize) -> Result<Vec<u8>, String> {
    if data.get(0..8) != Some(b"chunkref") || be32(data, 8)? != 2 {
        return Err("invalid chunkref header".into());
    }
    let total = be32(data, 12)? as usize;
    let chunk_size = be32(data, 16)? as usize;
    let chunks = be32(data, 20)? as usize;
    let alignment = be32(data, 24)? as usize;
    if total != expected || chunk_size == 0 || chunks == 0 || alignment == 0 || alignment > 4096 || alignment & (alignment - 1) != 0 {
        return Err("invalid chunkref dimensions".into());
    }
    let mut out = Vec::with_capacity(total);
    let mut cursor = 28;
    for _ in 0..chunks {
        let payload = align(cursor + 8, alignment);
        let packed_size = be32(data, payload - 8)? as usize;
        let method = be32(data, payload - 4)?;
        let packed = data.get(payload..payload + packed_size).ok_or("truncated chunkref chunk")?;
        let want = chunk_size.min(total - out.len());
        let decoded = match method {
            0 | 4 => packed.to_vec(),
            2 => refpack(packed, Some(want))?,
            3 => {
                let mut decoded = Vec::with_capacity(want);
                flate2::read::ZlibDecoder::new(packed).read_to_end(&mut decoded).map_err(|e| format!("chunkref zlib: {e}"))?;
                decoded
            }
            other => return Err(format!("unsupported chunkref method {other}")),
        };
        if decoded.len() != want {
            return Err(format!("chunkref chunk decoded to {} bytes, expected {want}", decoded.len()));
        }
        out.extend_from_slice(&decoded);
        cursor = payload + packed_size;
    }
    if out.len() != total {
        return Err("chunkref size mismatch".into());
    }
    Ok(out)
}
