//! Wire format shared with src/game/shared/tf/tf_skate_sidecar.cpp.
//!
//! Frame: u32 length (of everything after it), u8 message type, payload.
//! Replies reuse the request's type; their payload starts with u8 ok. A
//! failed reply carries one string (u32 length + UTF-8). All values are
//! little-endian.
use std::io::{Read, Write};

pub(crate) const HELLO: u8 = 1;
pub(crate) const WORLD: u8 = 2;
pub(crate) const SPAWN: u8 = 3;
pub(crate) const STEP: u8 = 4;
pub(crate) const DESPAWN: u8 = 5;
/// u32 src, u32 dst: dst becomes a copy of src (created if missing).
pub(crate) const COPY: u8 = 6;
/// u32 id: reply u32 1 once the skater has loaded, 0 while loading.
pub(crate) const POLL: u8 = 7;

/// A whole BSP is the largest legitimate message.
const MAX_FRAME: usize = 512 << 20;

pub(crate) fn read_frame(input: &mut impl Read) -> Result<Option<(u8, Vec<u8>)>, String> {
    let mut header = [0u8; 4];
    match input.read_exact(&mut header) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e.to_string()),
    }
    let length = u32::from_le_bytes(header) as usize;
    if length == 0 || length > MAX_FRAME {
        return Err(format!("Bad frame length {length}"));
    }
    let mut body = vec![0u8; length];
    input.read_exact(&mut body).map_err(|e| e.to_string())?;
    let kind = body[0];
    body.remove(0);
    Ok(Some((kind, body)))
}

pub(crate) fn write_frame(output: &mut impl Write, kind: u8, payload: &[u8]) -> Result<(), String> {
    let length = u32::try_from(payload.len() + 1).map_err(|_| "Reply too large")?;
    output.write_all(&length.to_le_bytes()).map_err(|e| e.to_string())?;
    output.write_all(&[kind]).map_err(|e| e.to_string())?;
    output.write_all(payload).map_err(|e| e.to_string())
}

pub(crate) struct Reader<'a> {
    data: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, at: 0 }
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8], String> {
        let bytes = self
            .data
            .get(self.at..self.at + n)
            .ok_or_else(|| format!("Message truncated at byte {}", self.at))?;
        self.at += n;
        Ok(bytes)
    }
    pub fn u32(&mut self) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub fn f32(&mut self) -> Result<f32, String> {
        let value = f32::from_le_bytes(self.take(4)?.try_into().unwrap());
        if value.is_finite() { Ok(value) } else { Err("Non-finite float in message".into()) }
    }
    pub fn vec3(&mut self) -> Result<[f32; 3], String> {
        Ok([self.f32()?, self.f32()?, self.f32()?])
    }
    pub fn bytes(&mut self) -> Result<&'a [u8], String> {
        let n = self.u32()? as usize;
        self.take(n)
    }
    pub fn string(&mut self) -> Result<String, String> {
        String::from_utf8(self.bytes()?.to_vec()).map_err(|e| e.to_string())
    }
}

#[derive(Default)]
pub(crate) struct Writer {
    data: Vec<u8>,
}

impl Writer {
    pub fn u32(&mut self, v: u32) {
        self.data.extend_from_slice(&v.to_le_bytes());
    }
    pub fn f32(&mut self, v: f32) {
        self.data.extend_from_slice(&v.to_le_bytes());
    }
    pub fn vec3(&mut self, v: [f32; 3]) {
        v.into_iter().for_each(|x| self.f32(x));
    }
    pub fn string(&mut self, s: &str) {
        self.u32(s.len() as u32);
        self.data.extend_from_slice(s.as_bytes());
    }
    /// Appends already-encoded payload bytes.
    pub fn raw(&mut self, bytes: &[u8]) {
        self.data.extend_from_slice(bytes);
    }
    pub fn into_bytes(self) -> Vec<u8> {
        self.data
    }
    pub fn finish(self, ok: bool) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.data.len() + 1);
        out.push(u8::from(ok));
        out.extend(self.data);
        out
    }
}
