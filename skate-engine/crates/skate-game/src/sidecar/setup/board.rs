//! Skate 3's default board (deck, trucks, wheels) for the TF2 mod's renderer:
//! models/skate/board.skbd plus materials/models/skate/board_*.vtf/.vmt.
//! Same output as tools/skate_board_model.py, but read straight from the
//! disc's createacharacter.big instead of a converted skater.glb.
//!
//! board.skbd (little endian): "SKBD", u32 version 1, u32 part count; per
//! part: u32 name length + material path, u32 vertex count, vertices
//! (f32 pos[3] board space, f32 normal[3], f32 uv[2]), u32 index count, u16s.
//! Board space is Source deck space in Hammer units: x nose, y left, z up,
//! origin at the middle of the deck; (x, y, z)_source = (z, x, y)_model.
use super::{big::BigArchive, rx2};
use std::path::Path;

const INCHES_PER_METRE: f32 = 1.0 / 0.0254;
const CHARACTER_ARCHIVE: &str = "data/content/createacharacter.big";

/// The default skater's board pieces (LOD0 model and diffuse texture IDs, from
/// skate-engine tools/default_skater_retail_manifest.json).
const PARTS: [(&str, &str, &str, &str); 3] = [
    ("board_deck", "SkateBoard", "0000157803e38811", "0000136403e38818"),
    ("board_truck", "SkateTruck", "0000157a03e38811", "000013a703e38818"),
    ("board_wheel", "SkateWheel", "0000157c03e38811", "000015b703e38818"),
];

pub(crate) fn convert(disc: &super::disc::Disc, mod_dir: &Path) -> Result<String, String> {
    let archive = BigArchive::open(disc, CHARACTER_ARCHIVE)?;
    let model_dir = mod_dir.join("models/skate");
    let material_dir = mod_dir.join("materials/models/skate");
    std::fs::create_dir_all(&model_dir).map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&material_dir).map_err(|e| e.to_string())?;

    let mut parts = Vec::new();
    for (name, slot, model, texture) in PARTS {
        let path = format!("data/content/createacharacter/model/cas_db/{slot}/0x{model}.rx2");
        let meshes = rx2::meshes(&archive.read_path(&path)?)?;
        // The LOD0 file holds one renderable mesh; take the largest if not.
        let mesh = meshes.into_iter().max_by_key(|m| m.positions.len()).ok_or_else(|| format!("{path} has no mesh"))?;
        let texture = rx2::texture(&archive.read_path(&format!("data/content/createacharacter/texture/0x{texture}.rx2"))?)?;
        parts.push((name, mesh, texture));
    }

    let to_source = |p: [f32; 3]| [p[2] * INCHES_PER_METRE, p[0] * INCHES_PER_METRE, p[1] * INCHES_PER_METRE];
    // Centre on the deck: middle of its length, width and thickness.
    let deck: Vec<[f32; 3]> = parts[0].1.positions.iter().map(|&p| to_source(p)).collect();
    let mut centre = [0f32; 3];
    for (k, c) in centre.iter_mut().enumerate() {
        let lo = deck.iter().map(|p| p[k]).fold(f32::INFINITY, f32::min);
        let hi = deck.iter().map(|p| p[k]).fold(f32::NEG_INFINITY, f32::max);
        *c = (lo + hi) / 2.0;
    }

    let mut out = b"SKBD".to_vec();
    out.extend(1u32.to_le_bytes());
    out.extend((parts.len() as u32).to_le_bytes());
    for (name, mesh, texture) in &parts {
        let material = format!("models/skate/{name}");
        out.extend((material.len() as u32).to_le_bytes());
        out.extend(material.as_bytes());
        out.extend((mesh.positions.len() as u32).to_le_bytes());
        for i in 0..mesh.positions.len() {
            let p = to_source(mesh.positions[i]);
            let n = mesh.normals[i];
            let values = [p[0] - centre[0], p[1] - centre[1], p[2] - centre[2], n[2], n[0], n[1], mesh.uvs[i][0], mesh.uvs[i][1]];
            for v in values {
                out.extend(v.to_le_bytes());
            }
        }
        out.extend((mesh.indices.len() as u32).to_le_bytes());
        for &i in &mesh.indices {
            out.extend(i.to_le_bytes());
        }
        write_vtf(&material_dir.join(format!("{name}.vtf")), texture.width, texture.height, &texture.rgba)?;
        // Unlit with vertex colour: C_TFSkateboard shades each vertex from the
        // world lighting at the board (a dynamic mesh has no studio lighting).
        std::fs::write(
            material_dir.join(format!("{name}.vmt")),
            format!("\"UnlitGeneric\"\n{{\n\t\"$basetexture\" \"{material}\"\n\t\"$vertexcolor\" \"1\"\n}}\n"),
        )
        .map_err(|e| e.to_string())?;
    }
    std::fs::write(model_dir.join("board.skbd"), &out).map_err(|e| e.to_string())?;
    let vertices: usize = parts.iter().map(|p| p.1.positions.len()).sum();
    Ok(format!("{} board parts, {vertices} vertices", parts.len()))
}

/// VTF 7.2, RGBA8888, full box-filtered mip chain, smallest mip first.
pub(crate) fn write_vtf(path: &Path, width: usize, height: usize, rgba: &[u8]) -> Result<(), String> {
    let mut mips = vec![(width, height, rgba.to_vec())];
    let (mut w, mut h) = (width, height);
    while w > 1 || h > 1 {
        let (nw, nh) = ((w / 2).max(1), (h / 2).max(1));
        let src = &mips.last().unwrap().2;
        let mut dst = vec![0u8; nw * nh * 4];
        for y in 0..nh {
            for x in 0..nw {
                for c in 0..4 {
                    let mut total = 0u32;
                    for (dx, dy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
                        let (sx, sy) = ((2 * x + dx).min(w - 1), (2 * y + dy).min(h - 1));
                        total += src[(sy * w + sx) * 4 + c] as u32;
                    }
                    dst[(y * nw + x) * 4 + c] = (total / 4) as u8;
                }
            }
        }
        mips.push((nw, nh, dst));
        (w, h) = (nw, nh);
    }
    let has_alpha = rgba.chunks_exact(4).any(|p| p[3] != 255);
    let mut header = Vec::with_capacity(80);
    header.extend(b"VTF\0");
    for v in [7u32, 2, 80] {
        header.extend(v.to_le_bytes());
    }
    header.extend((width as u16).to_le_bytes());
    header.extend((height as u16).to_le_bytes());
    header.extend((if has_alpha { 0x2000u32 } else { 0 }).to_le_bytes()); // EIGHTBITALPHA
    header.extend(1u16.to_le_bytes()); // frames
    header.extend(0u16.to_le_bytes()); // first frame
    header.extend([0u8; 4]);
    for v in [0.5f32, 0.5, 0.5] {
        header.extend(v.to_le_bytes()); // reflectivity
    }
    header.extend([0u8; 4]);
    header.extend(1.0f32.to_le_bytes()); // bump scale
    header.extend(0u32.to_le_bytes()); // RGBA8888
    header.push(mips.len() as u8);
    header.extend(0xFFFF_FFFFu32.to_le_bytes()); // no low-res image
    header.extend([0u8, 0u8]);
    header.extend(1u16.to_le_bytes()); // depth
    header.resize(80, 0);
    for mip in mips.iter().rev() {
        header.extend(&mip.2);
    }
    std::fs::write(path, header).map_err(|e| format!("{}: {e}", path.display()))
}
