//! RenderWare 4 ".rx2" (Xbox 360) models and textures, as much as the board
//! needs. Geometry is a port of vendor/skate3_anim/rx2_skeleton.py, textures
//! of vendor/utt/rx2_parser.py (+ rx2_fast.py's untile ordering).

const TYPE_RAW_BUFFER: u32 = 0x00010031;
const TYPE_VTX_DESC: u32 = 0x000200E9;
const TYPE_VB_DESC: u32 = 0x000200EA;
const TYPE_IB_DESC: u32 = 0x000200EB;
const TYPE_MESH_DESC: u32 = 0x00EB0023;
const TOC_TEXTURE_SKATE: u32 = 0x000200E8;
const TOC_TEXTURE_NHL: u32 = 0x00020003;

fn u16be(d: &[u8], o: usize) -> Result<u16, String> {
    d.get(o..o + 2).map(|b| u16::from_be_bytes([b[0], b[1]])).ok_or_else(|| format!("RX2 truncated at {o:#x}"))
}
fn u32be(d: &[u8], o: usize) -> Result<u32, String> {
    d.get(o..o + 4).map(|b| u32::from_be_bytes(b.try_into().unwrap())).ok_or_else(|| format!("RX2 truncated at {o:#x}"))
}

#[derive(Default)]
pub(crate) struct Mesh {
    /// Model space, metres (Y up).
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u16>,
}

struct Section {
    offset: u32,
    file_offset: usize,
    size: u32,
    type_code: u32,
}

fn sections(d: &[u8]) -> Result<Vec<Section>, String> {
    if d.get(..12).is_none_or(|m| !m.starts_with(b"\x89RW4xb2")) {
        return Err("Not an Xbox 360 RW4 file".into());
    }
    let count = u32be(d, 0x20)? as usize;
    let index = u32be(d, 0x30)? as usize;
    let gpu_arena = u32be(d, 0x44)? as usize;
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let base = index + i * 24;
        let offset = u32be(d, base)?;
        let size = u32be(d, base + 8)?;
        let type_code = u32be(d, base + 20)?;
        let file_offset = if type_code == TYPE_RAW_BUFFER { gpu_arena + offset as usize } else { offset as usize };
        out.push(Section { offset, file_offset, size, type_code });
    }
    Ok(out)
}

fn section_by_index(sections: &[Section], index: u32, want: u32) -> Option<&Section> {
    match sections.get(index as usize) {
        Some(s) if s.type_code == want => Some(s),
        _ => sections.iter().find(|s| s.type_code == want),
    }
}

/// Signed 11/11/10 normalised, low bits first.
fn dec_11_11_10(u: u32) -> [f32; 3] {
    let mut out = [0f32; 3];
    for (k, (shift, bits)) in [(0, 11), (11, 11), (22, 10)].into_iter().enumerate() {
        let mut v = ((u >> shift) & ((1 << bits) - 1)) as i32;
        if v >= 1 << (bits - 1) {
            v -= 1 << bits;
        }
        out[k] = v as f32 / ((1 << (bits - 1)) - 1) as f32;
    }
    out
}

/// The model's meshes with positions, derived normals, UVs and indices.
pub(crate) fn meshes(d: &[u8]) -> Result<Vec<Mesh>, String> {
    let sections = sections(d)?;
    let mut out = Vec::new();
    for mesh_desc in sections.iter().filter(|s| s.type_code == TYPE_MESH_DESC) {
        let base = mesh_desc.file_offset;
        let (Some(vdesc_s), Some(ibd_s), Some(vbd_s)) = (
            section_by_index(&sections, u32be(d, base + 0x28)?, TYPE_VTX_DESC),
            section_by_index(&sections, u32be(d, base + 0x30)?, TYPE_IB_DESC),
            section_by_index(&sections, u32be(d, base + 0x34)?, TYPE_VB_DESC),
        ) else {
            continue;
        };
        // Vertex descriptor: elements of 16 bytes, then one stride byte each.
        let vbase = vdesc_s.file_offset;
        let n = u16be(d, vbase + 8)? as usize;
        let stride = *d.get(vbase + 0x10 + 16 * n).ok_or("RX2 truncated vertex descriptor")? as usize;
        if stride == 0 {
            continue;
        }
        let vb_bytes = u32be(d, vbd_s.file_offset + 0x20)?;
        let ib_bytes = u32be(d, ibd_s.file_offset + 0x1C)?;
        let index_count = u32be(d, ibd_s.file_offset + 0x20)? as usize;
        // Raw GPU buffers are matched to their descriptors by size; the
        // vertex buffer precedes the index buffer.
        let mut raws: Vec<&Section> = sections.iter().filter(|s| s.type_code == TYPE_RAW_BUFFER).collect();
        raws.sort_by_key(|s| s.offset);
        let vb_index = raws.iter().position(|s| s.size == vb_bytes).or((!raws.is_empty()).then_some(0));
        let ib_index = raws
            .iter()
            .enumerate()
            .position(|(i, s)| s.size == ib_bytes && Some(i) != vb_index)
            .or((raws.len() > 1).then_some(1));
        let vb = vb_index.map(|i| raws[i]);
        let ib = ib_index.map(|i| raws[i]);
        let (Some(vb), Some(ib)) = (vb, ib) else { continue };
        let count = vb_bytes as usize / stride;

        let mut mesh = Mesh::default();
        let mut tangents = Vec::new();
        let mut binormals = Vec::new();
        for e in 0..n {
            let o = vbase + 0x10 + 16 * e;
            let offset = u16be(d, o + 2)? as usize;
            let format = u32be(d, o + 4)?;
            let usage = d[o + 9];
            let usage_index = d[o + 10];
            let at = |v: usize| vb.file_offset + v * stride + offset;
            match (usage, usage_index, format) {
                (0, 0, 0x001A215A) => {
                    // SHORT4 position: 2^-14 scale, +0.8 m Y bias.
                    for v in 0..count {
                        let x = u16be(d, at(v))? as i16 as f32;
                        let y = u16be(d, at(v) + 2)? as i16 as f32;
                        let z = u16be(d, at(v) + 4)? as i16 as f32;
                        mesh.positions.push([x / 16384.0, y / 16384.0 + 0.8, z / 16384.0]);
                    }
                }
                (5, 0, 0x002C2159) => {
                    for v in 0..count {
                        let u = u16be(d, at(v))? as i16 as f32 / 32768.0;
                        let w = u16be(d, at(v) + 2)? as i16 as f32 / 32768.0;
                        mesh.uvs.push([u, w]);
                    }
                }
                (5, 0, 0x002C23A5) => {
                    for v in 0..count {
                        let u = f32::from_bits(u32be(d, at(v))?);
                        let w = f32::from_bits(u32be(d, at(v) + 4)?);
                        mesh.uvs.push([u, w]);
                    }
                }
                (6, 0, 0x002A2190) => {
                    for v in 0..count {
                        tangents.push(dec_11_11_10(u32be(d, at(v))?));
                    }
                }
                (7, 0, 0x002A2190) => {
                    for v in 0..count {
                        binormals.push(dec_11_11_10(u32be(d, at(v))?));
                    }
                }
                _ => {}
            }
        }
        for i in 0..index_count {
            mesh.indices.push(u16be(d, ib.file_offset + 2 * i)?);
        }
        if mesh.positions.is_empty() || mesh.indices.is_empty() {
            continue;
        }
        mesh.normals = derive_normals(&mesh, &tangents, &binormals);
        if mesh.uvs.len() != mesh.positions.len() {
            mesh.uvs = vec![[0.0, 0.0]; mesh.positions.len()];
        }
        out.push(mesh);
    }
    Ok(out)
}

fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
fn length(v: [f32; 3]) -> f32 {
    (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt()
}

/// No normals are stored: the shader uses cross(tangent, binormal). A
/// collapsed frame falls back to the area-weighted face normal.
fn derive_normals(mesh: &Mesh, tangents: &[[f32; 3]], binormals: &[[f32; 3]]) -> Vec<[f32; 3]> {
    let n = mesh.positions.len();
    let mut face = vec![[0f32; 3]; n];
    for tri in mesh.indices.chunks_exact(3) {
        let [a, b, c] = [tri[0] as usize, tri[1] as usize, tri[2] as usize];
        if a.max(b).max(c) >= n {
            continue;
        }
        let (pa, pb, pc) = (mesh.positions[a], mesh.positions[b], mesh.positions[c]);
        let f = cross([pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]], [pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]]);
        for v in [a, b, c] {
            for k in 0..3 {
                face[v][k] += f[k];
            }
        }
    }
    (0..n)
        .map(|i| {
            let framed = match (tangents.get(i), binormals.get(i)) {
                (Some(t), Some(b)) => Some(cross(*t, *b)),
                _ => None,
            };
            let v = match framed {
                Some(v) if length(v) >= 1e-3 => v,
                _ => face[i],
            };
            let l = length(v);
            if l > 1e-12 { [v[0] / l, v[1] / l, v[2] / l] } else { [0.0, 1.0, 0.0] }
        })
        .collect()
}

pub(crate) struct Texture {
    pub width: usize,
    pub height: usize,
    pub rgba: Vec<u8>,
}

/// The first texture in an .rx2, decoded to RGBA8.
pub(crate) fn texture(d: &[u8]) -> Result<Texture, String> {
    if d.len() < 0x5C || !d.starts_with(b"\x89RW4xb2") {
        return Err("Not an Xbox 360 RX2 texture".into());
    }
    let count = u32be(d, 0x20)? as usize;
    let table = u32be(d, 0x30)? as usize;
    let data_base = u32be(d, 0x44)? as usize;
    let mut previous: Option<(u32, u32)> = None;
    for i in 0..count {
        let p = table + i * 24;
        let f0 = u32be(d, p)?;
        let f2 = u32be(d, p + 8)?;
        let type_id = u32be(d, p + 20)?;
        if type_id == TOC_TEXTURE_SKATE || type_id == TOC_TEXTURE_NHL {
            let (data_offset, buffer_size) = previous.ok_or("RX2 texture without a data record")?;
            let header = d.get(f0 as usize..f0 as usize + 40).ok_or("RX2 texture header out of range")?;
            let format = header[35];
            let size = u32::from_be_bytes(header[36..40].try_into().unwrap());
            let width = (size & 0x1FFF) as usize + 1;
            let height = ((size >> 13) & 0x1FFF) as usize + 1;
            let dxt5_variant = header[28];
            let start = data_base + data_offset as usize;
            let raw = d.get(start..start + buffer_size as usize).ok_or("RX2 texture data out of range")?;
            let rgba = decode(raw, width, height, format, dxt5_variant)?;
            return Ok(Texture { width, height, rgba });
        }
        previous = Some((f0, f2));
    }
    Err("RX2 file has no texture".into())
}

/// XGAddress2DTiled: Xbox 360 tiled surface -> linear, `pitch` bytes per unit.
fn untile(src: &[u8], width_units: usize, pitch: usize) -> Vec<u8> {
    let units = src.len() / pitch;
    let mut dst = vec![0u8; src.len()];
    let aligned = (width_units + 31) & !31;
    let log = (pitch >> 2) + ((pitch >> 1) >> (pitch >> 2));
    for offset in 0..units / width_units * width_units {
        let b = offset << log;
        let t = ((b & !4095) >> 3) + ((b & 1792) >> 2) + (b & 63);
        let m = t >> (7 + log);
        let x = ((((m % (aligned >> 5)) << 2) + ((((t >> (5 + log)) & 2) + (b >> 6)) & 3)) << 3)
            + (((((t >> 1) & !15) + (t & 15)) & ((pitch << 3) - 1)) >> log);
        let y = ((((m / (aligned >> 5)) << 2) + ((t >> (6 + log)) & 1) + ((b & 2048) >> 10)) << 3)
            + ((((t & (((pitch << 6) - 1) & !31)) + ((t & 15) << 1)) >> (3 + log)) & !1)
            + ((t & 16) >> 4);
        let dest = y * width_units + x;
        if x < width_units && dest < units {
            // Later source units win, as in the reference decoder.
            dst[dest * pitch..dest * pitch + pitch].copy_from_slice(&src[offset * pitch..offset * pitch + pitch]);
        }
    }
    dst
}

fn rgb565(v: u32) -> [u32; 3] {
    [((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31]
}

/// One 8-byte colour block (byte-swapped X360 words) -> 16 RGBA texels.
fn colour_block(b: &[u8], transparent: bool, out: &mut [[u8; 4]; 16]) {
    let c0 = (b[0] as u32) << 8 | b[1] as u32;
    let c1 = (b[2] as u32) << 8 | b[3] as u32;
    let (a, z) = (rgb565(c0), rgb565(c1));
    let four = c0 > c1 || !transparent;
    let mut palette = [[0u8, 0, 0, 255]; 4];
    for k in 0..3 {
        palette[0][k] = a[k] as u8;
        palette[1][k] = z[k] as u8;
        palette[2][k] = if four { ((2 * a[k] + z[k] + 1) / 3) as u8 } else { ((a[k] + z[k]) / 2) as u8 };
        palette[3][k] = if four { ((a[k] + 2 * z[k] + 1) / 3) as u8 } else { 0 };
    }
    if transparent && !four {
        palette[3][3] = 0;
    }
    for y in 0..4 {
        let selectors = b[4 + (y ^ 1)];
        for x in 0..4 {
            out[y * 4 + x] = palette[((selectors >> (2 * x)) & 3) as usize];
        }
    }
}

/// One 8-byte DXT5/ATI2 alpha block -> 16 values.
fn alpha_block(b: &[u8]) -> [u8; 16] {
    let (a, z) = (b[1] as u32, b[0] as u32);
    let mut table = [0u32; 8];
    table[0] = a;
    table[1] = z;
    for i in 0..6u32 {
        table[i as usize + 2] = if a > z {
            ((6 - i) * a + (i + 1) * z) / 7
        } else if i < 4 {
            ((4 - i) * a + (i + 1) * z) / 5
        } else if i == 4 {
            0
        } else {
            255
        };
    }
    let bits = [3, 2, 5, 4, 7, 6].iter().enumerate().fold(0u64, |acc, (k, &i)| acc | (b[i] as u64) << (8 * k));
    let mut out = [0u8; 16];
    for (p, v) in out.iter_mut().enumerate() {
        *v = table[((bits >> (3 * p)) & 7) as usize] as u8;
    }
    out
}

fn decode(raw: &[u8], width: usize, height: usize, format: u8, dxt5_variant: u8) -> Result<Vec<u8>, String> {
    let (kind, block) = match format {
        0x52 => (1, 8),
        0x53 => (3, 16),
        0x54 => (5, 16),
        0x71 => (2, 16),
        0x86 => {
            let raw = untile(raw, width, 4);
            let mut out = vec![0u8; width * height * 4];
            for (i, px) in raw.chunks_exact(4).take(width * height).enumerate() {
                out[i * 4..i * 4 + 4].copy_from_slice(&[px[1], px[2], px[3], px[0]]);
            }
            return Ok(out);
        }
        other => return Err(format!("Unhandled RX2 texture format {other:#04X}")),
    };
    let tiled = !(kind == 5 && matches!(dxt5_variant, 1 | 2 | 84));
    let (bw, bh) = (width.div_ceil(4), height.div_ceil(4));
    let data = if tiled { untile(raw, bw, block) } else { raw.to_vec() };
    let mut out = vec![0u8; width * height * 4];
    for by in 0..bh {
        for bx in 0..bw {
            let o = (by * bw + bx) * block;
            let Some(b) = data.get(o..o + block) else { continue };
            let mut px = [[0u8; 4]; 16];
            match kind {
                1 => colour_block(b, true, &mut px),
                3 => {
                    colour_block(&b[8..], false, &mut px);
                    for (p, texel) in px.iter_mut().enumerate() {
                        texel[3] = ((b[(p / 2) ^ 1] >> (4 * (p & 1))) & 15) * 17;
                    }
                }
                5 => {
                    colour_block(&b[8..], false, &mut px);
                    for (texel, a) in px.iter_mut().zip(alpha_block(&b[..8])) {
                        texel[3] = a;
                    }
                }
                _ => {
                    let (r, g) = (alpha_block(&b[..8]), alpha_block(&b[8..]));
                    for p in 0..16 {
                        px[p] = [r[p], g[p], 0, 255];
                    }
                }
            }
            for yy in 0..4 {
                for xx in 0..4 {
                    let (x, y) = (bx * 4 + xx, by * 4 + yy);
                    if x < width && y < height {
                        out[(y * width + x) * 4..(y * width + x) * 4 + 4].copy_from_slice(&px[yy * 4 + xx]);
                    }
                }
            }
        }
    }
    Ok(out)
}
