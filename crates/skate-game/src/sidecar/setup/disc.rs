//! Files of a Skate 3 disc: read straight out of an Xbox 360 ISO (XDVDFS), or
//! from an extracted disc folder. Paths are disc-relative with forward
//! slashes and match case-insensitively.
use std::{
    collections::HashMap,
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::Mutex,
};

const SECTOR: u64 = 2048;
const MAGIC: &[u8; 20] = b"MICROSOFT*XBOX*MEDIA";
/// Game partition offsets: xiso rebuild, XGD3, XGD2, XGD1.
const PARTITIONS: [u64; 4] = [0, 0x2080000, 0xFD90000, 0x18300000];

pub(crate) enum Disc {
    Iso {
        file: Mutex<File>,
        /// lowercase path -> (absolute offset, size)
        files: HashMap<String, (u64, u64)>,
    },
    Folder {
        root: PathBuf,
        /// lowercase path -> real path
        files: HashMap<String, PathBuf>,
    },
}

impl Disc {
    /// `source`: an .iso, a default.xex, or the folder holding default.xex.
    pub fn open(source: &Path) -> Result<Self, String> {
        let is_iso = source.is_file()
            && source.extension().is_some_and(|e| e.eq_ignore_ascii_case("iso"));
        let disc = if is_iso {
            Self::open_iso(source)?
        } else {
            let root = if source.is_file() { source.parent().unwrap_or(source) } else { source };
            let mut files = HashMap::new();
            walk_folder(root, root, &mut files)?;
            Self::Folder { root: root.to_path_buf(), files }
        };
        if !disc.exists("default.xex") || !disc.exists("data/big/miscload.big") {
            return Err(format!(
                "{} doesn't look like Skate 3 (no default.xex and data/big/miscload.big). Pick the ISO, its default.xex, or the extracted disc folder.",
                source.display()
            ));
        }
        Ok(disc)
    }

    fn open_iso(path: &Path) -> Result<Self, String> {
        let mut file = File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let mut found = None;
        for base in PARTITIONS {
            let mut head = [0u8; 28];
            if file.seek(SeekFrom::Start(base + 32 * SECTOR)).is_ok() && file.read_exact(&mut head).is_ok() && &head[..20] == MAGIC {
                let root = u32::from_le_bytes(head[20..24].try_into().unwrap()) as u64;
                let size = u32::from_le_bytes(head[24..28].try_into().unwrap()) as u64;
                found = Some((base, root, size));
                break;
            }
        }
        let (base, root, size) = found.ok_or_else(|| format!("{} is not an Xbox disc image", path.display()))?;
        let mut files = HashMap::new();
        let mut pending = vec![(root, size, String::new())];
        while let Some((sector, size, prefix)) = pending.pop() {
            if size == 0 || size > 64 << 20 {
                continue;
            }
            let mut table = vec![0u8; size as usize];
            file.seek(SeekFrom::Start(base + sector * SECTOR)).map_err(|e| e.to_string())?;
            file.read_exact(&mut table).map_err(|e| format!("Reading the ISO's directory: {e}"))?;
            let mut stack = vec![0usize];
            let mut seen = std::collections::HashSet::new();
            while let Some(offset) = stack.pop() {
                if !seen.insert(offset) || offset + 14 > table.len() {
                    continue;
                }
                let u16le = |o: usize| u16::from_le_bytes([table[o], table[o + 1]]);
                let u32le = |o: usize| u32::from_le_bytes(table[o..o + 4].try_into().unwrap());
                let (left, right) = (u16le(offset), u16le(offset + 2));
                if left == 0xFFFF {
                    continue;
                }
                let (data_sector, data_size) = (u32le(offset + 4) as u64, u32le(offset + 8) as u64);
                let attributes = table[offset + 12];
                let name_length = table[offset + 13] as usize;
                if offset + 14 + name_length > table.len() {
                    continue;
                }
                let name: String = table[offset + 14..offset + 14 + name_length].iter().map(|&b| b as char).collect();
                if left != 0 {
                    stack.push(left as usize * 4);
                }
                if right != 0 {
                    stack.push(right as usize * 4);
                }
                let path = format!("{prefix}{name}");
                if attributes & 0x10 != 0 {
                    pending.push((data_sector, data_size, format!("{path}/")));
                } else {
                    files.insert(path.to_ascii_lowercase(), (base + data_sector * SECTOR, data_size));
                }
            }
        }
        Ok(Self::Iso { file: Mutex::new(file), files })
    }

    pub fn exists(&self, path: &str) -> bool {
        let key = path.to_ascii_lowercase();
        match self {
            Self::Iso { files, .. } => files.contains_key(&key),
            Self::Folder { files, .. } => files.contains_key(&key),
        }
    }

    pub fn read(&self, path: &str) -> Result<Vec<u8>, String> {
        let size = self.size(path)?;
        self.read_at(path, 0, size)
    }

    pub fn size(&self, path: &str) -> Result<u64, String> {
        let key = path.to_ascii_lowercase();
        match self {
            Self::Iso { files, .. } => files.get(&key).map(|f| f.1),
            Self::Folder { files, .. } => files.get(&key).and_then(|p| p.metadata().ok()).map(|m| m.len()),
        }
        .ok_or_else(|| format!("The disc has no {path}"))
    }

    /// `length` bytes of a disc file from `offset`.
    pub fn read_at(&self, path: &str, offset: u64, length: u64) -> Result<Vec<u8>, String> {
        let key = path.to_ascii_lowercase();
        let mut out = vec![0u8; length as usize];
        match self {
            Self::Iso { file, files } => {
                let (start, size) = *files.get(&key).ok_or_else(|| format!("The disc has no {path}"))?;
                if offset + length > size {
                    return Err(format!("{path}: read past the end"));
                }
                let mut file = file.lock().map_err(|_| "ISO reader poisoned".to_string())?;
                file.seek(SeekFrom::Start(start + offset)).map_err(|e| e.to_string())?;
                file.read_exact(&mut out).map_err(|e| format!("{path}: {e}"))?;
            }
            Self::Folder { files, .. } => {
                let real = files.get(&key).ok_or_else(|| format!("The disc has no {path}"))?;
                let mut file = File::open(real).map_err(|e| format!("{}: {e}", real.display()))?;
                file.seek(SeekFrom::Start(offset)).map_err(|e| e.to_string())?;
                file.read_exact(&mut out).map_err(|e| format!("{path}: {e}"))?;
            }
        }
        Ok(out)
    }

    /// Disc-relative paths under `folder` (lowercase), e.g. "data/anim".
    pub fn list(&self, folder: &str) -> Vec<String> {
        let prefix = format!("{}/", folder.trim_end_matches('/').to_ascii_lowercase());
        let mut out: Vec<String> = match self {
            Self::Iso { files, .. } => files.keys().filter(|k| k.starts_with(&prefix)).cloned().collect(),
            Self::Folder { files, .. } => files.keys().filter(|k| k.starts_with(&prefix)).cloned().collect(),
        };
        out.sort();
        out
    }

    pub fn describe(&self) -> String {
        match self {
            Self::Iso { files, .. } => format!("disc image ({} files)", files.len()),
            Self::Folder { root, files } => format!("{} ({} files)", root.display(), files.len()),
        }
    }
}

fn walk_folder(root: &Path, dir: &Path, files: &mut HashMap<String, PathBuf>) -> Result<(), String> {
    for entry in std::fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))? {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        if path.is_dir() {
            walk_folder(root, &path, files)?;
        } else if let Ok(relative) = path.strip_prefix(root) {
            let key = relative.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
            files.insert(key, path);
        }
    }
    Ok(())
}
