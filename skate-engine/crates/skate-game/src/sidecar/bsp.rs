//! Player-solid collision triangles from a Source (v19-v21) BSP.
//!
//! World brushes are rebuilt by clipping each side's plane against the other
//! sides, the same convex solids vbsp hands to the physics compiler. Only
//! brushes reachable from model 0 count, so trigger and func_ brushes are left
//! out. Displacements are tessellated from their base face. Every triangle
//! winds so (b - a) x (c - a) points out of the solid, in Source units.

pub type Triangle = [[f32; 3]; 3];

const LUMP_PLANES: usize = 1;
const LUMP_VERTEXES: usize = 3;
const LUMP_NODES: usize = 5;
const LUMP_FACES: usize = 7;
const LUMP_LEAFS: usize = 10;
const LUMP_EDGES: usize = 12;
const LUMP_SURFEDGES: usize = 13;
const LUMP_MODELS: usize = 14;
const LUMP_LEAFBRUSHES: usize = 17;
const LUMP_BRUSHES: usize = 18;
const LUMP_BRUSHSIDES: usize = 19;
const LUMP_DISPINFO: usize = 26;
const LUMP_DISP_VERTS: usize = 33;

// CONTENTS_SOLID | WINDOW | GRATE | MOVEABLE | PLAYERCLIP | MONSTER (MASK_PLAYERSOLID).
const PLAYER_SOLID: i32 = 0x1 | 0x2 | 0x8 | 0x4000 | 0x10000 | 0x200_0000;
const CLIP_EPSILON: f64 = 0.01;
const HUGE: f64 = 65536.0;

struct Bsp<'a> {
    data: &'a [u8],
    lumps: [(usize, usize, i32); 64],
    /// Lumps stored LZMA-compressed (most shipped TF2 maps), inflated on demand.
    inflated: std::cell::RefCell<[Option<std::rc::Rc<Vec<u8>>>; 64]>,
}

impl<'a> Bsp<'a> {
    fn parse(data: &'a [u8]) -> Result<Self, String> {
        if data.len() < 1036 || &data[..4] != b"VBSP" {
            return Err("Not a Source BSP (missing VBSP header)".into());
        }
        let version = i32_at(data, 4);
        if !(19..=21).contains(&version) {
            return Err(format!("Unsupported BSP version {version}"));
        }
        let mut lumps = [(0, 0, 0); 64];
        for (i, lump) in lumps.iter_mut().enumerate() {
            let base = 8 + i * 16;
            *lump = (
                i32_at(data, base) as usize,
                i32_at(data, base + 4) as usize,
                i32_at(data, base + 8),
            );
        }
        Ok(Self { data, lumps, inflated: std::cell::RefCell::new(std::array::from_fn(|_| None)) })
    }

    fn lump(&self, index: usize, stride: usize) -> Result<(Lump<'a>, usize), String> {
        let (offset, length, _) = self.lumps[index];
        let bytes = self
            .data
            .get(offset..offset + length)
            .ok_or_else(|| format!("BSP lump {index} lies outside the file"))?;
        let lump = if bytes.len() >= 17 && &bytes[..4] == b"LZMA" {
            let mut cache = self.inflated.borrow_mut();
            let inflated = match &cache[index] {
                Some(inflated) => inflated.clone(),
                None => {
                    let inflated = std::rc::Rc::new(inflate(bytes).map_err(|e| format!("BSP lump {index}: {e}"))?);
                    cache[index] = Some(inflated.clone());
                    inflated
                }
            };
            Lump::Owned(inflated)
        } else {
            Lump::Borrowed(bytes)
        };
        let size = lump.len();
        if stride != 0 && size % stride != 0 {
            return Err(format!("BSP lump {index} size {size} is not a multiple of {stride}"));
        }
        Ok((lump, if stride == 0 { 0 } else { size / stride }))
    }
}

enum Lump<'a> {
    Borrowed(&'a [u8]),
    Owned(std::rc::Rc<Vec<u8>>),
}

impl std::ops::Deref for Lump<'_> {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match self {
            Lump::Borrowed(bytes) => bytes,
            Lump::Owned(bytes) => bytes,
        }
    }
}

/// Valve's lzma_header_t: "LZMA", u32 actual size, u32 compressed size, five
/// property bytes, then a raw LZMA stream. Rebuild the classic .lzma header
/// (properties + u64 unpacked size) so a stock decoder accepts it.
fn inflate(bytes: &[u8]) -> Result<Vec<u8>, String> {
    let actual = u32::from_le_bytes(bytes[4..8].try_into().unwrap()) as usize;
    let compressed = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let stream = bytes.get(17..17 + compressed).ok_or("truncated LZMA lump")?;
    let mut input = Vec::with_capacity(13 + stream.len());
    input.extend_from_slice(&bytes[12..17]);
    input.extend_from_slice(&(actual as u64).to_le_bytes());
    input.extend_from_slice(stream);
    let mut output = Vec::with_capacity(actual);
    lzma_rs::lzma_decompress(&mut std::io::Cursor::new(input), &mut output).map_err(|e| format!("LZMA: {e}"))?;
    if output.len() != actual {
        return Err(format!("LZMA lump inflated to {} bytes, expected {actual}", output.len()));
    }
    Ok(output)
}

fn i32_at(b: &[u8], o: usize) -> i32 {
    i32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap())
}
fn i16_at(b: &[u8], o: usize) -> i16 {
    i16::from_le_bytes(b[o..o + 2].try_into().unwrap())
}
fn f32_at(b: &[u8], o: usize) -> f32 {
    f32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn vec_at(b: &[u8], o: usize) -> [f64; 3] {
    [0, 4, 8].map(|d| f64::from(f32_at(b, o + d)))
}

#[derive(Clone, Copy)]
struct Plane {
    normal: [f64; 3],
    dist: f64,
}

pub struct Extracted {
    pub triangles: Vec<Triangle>,
    pub brushes: usize,
    pub displacements: usize,
}

pub fn extract(data: &[u8]) -> Result<Extracted, String> {
    let bsp = Bsp::parse(data)?;
    let (planes_raw, plane_count) = bsp.lump(LUMP_PLANES, 20)?;
    let planes_raw = &*planes_raw;
    let planes: Vec<Plane> = (0..plane_count)
        .map(|i| Plane { normal: vec_at(planes_raw, i * 20), dist: f64::from(f32_at(planes_raw, i * 20 + 12)) })
        .collect();

    let brush_ids = world_brushes(&bsp)?;
    let (brushes, brush_count) = bsp.lump(LUMP_BRUSHES, 12)?;
    let brushes = &*brushes;
    let (sides, side_count) = bsp.lump(LUMP_BRUSHSIDES, 8)?;
    let sides = &*sides;
    let mut solids = Vec::new();
    for &b in &brush_ids {
        if b >= brush_count {
            return Err(format!("Leaf references brush {b} of {brush_count}"));
        }
        let first = i32_at(brushes, b * 12) as usize;
        let count = i32_at(brushes, b * 12 + 4) as usize;
        let contents = i32_at(brushes, b * 12 + 8);
        if contents & PLAYER_SOLID == 0 || first + count > side_count {
            continue;
        }
        let brush_planes: Vec<(Plane, bool)> = (first..first + count)
            .map(|s| {
                let plane = planes[u16_at(sides, s * 8) as usize];
                let bevel = sides[s * 8 + 6] != 0;
                (plane, bevel)
            })
            .collect();
        let solid = brush_solid(&brush_planes);
        if !solid.faces.is_empty() {
            solids.push(solid);
        }
    }
    let used = solids.len();
    let mut triangles = Vec::new();
    for (i, solid) in solids.iter().enumerate() {
        for (winding, normal) in &solid.faces {
            if !buried(winding, *normal, i, &solids) {
                fan(winding, *normal, &mut triangles);
            }
        }
    }

    let displacements = displacement_triangles(&bsp, &planes, &mut triangles)?;
    Ok(Extracted { triangles, brushes: used, displacements })
}

/// Brush indices in the leaves under model 0's head node.
fn world_brushes(bsp: &Bsp) -> Result<Vec<usize>, String> {
    let (models, _) = bsp.lump(LUMP_MODELS, 48)?;
    let models = &*models;
    if models.len() < 48 {
        return Err("BSP has no world model".into());
    }
    let head = i32_at(models, 36);
    let (nodes, node_count) = bsp.lump(LUMP_NODES, 32)?;
    let nodes = &*nodes;
    // Leaf lump version 0 carries a 24-byte ambient cube per leaf.
    let leaf_stride = if bsp.lumps[LUMP_LEAFS].2 == 0 { 56 } else { 32 };
    let (leafs, leaf_count) = bsp.lump(LUMP_LEAFS, leaf_stride)?;
    let leafs = &*leafs;
    let (leaf_brushes, leaf_brush_count) = bsp.lump(LUMP_LEAFBRUSHES, 2)?;
    let leaf_brushes = &*leaf_brushes;
    let mut seen = vec![false; usize::from(u16::MAX) + 1];
    let mut result = Vec::new();
    let mut stack = vec![head];
    while let Some(node) = stack.pop() {
        if node >= 0 {
            let n = node as usize;
            if n >= node_count {
                return Err(format!("BSP node {n} out of range"));
            }
            stack.push(i32_at(nodes, n * 32 + 4));
            stack.push(i32_at(nodes, n * 32 + 8));
            continue;
        }
        let leaf = (-1 - node) as usize;
        if leaf >= leaf_count {
            return Err(format!("BSP leaf {leaf} out of range"));
        }
        // firstleafbrush / numleafbrushes follow contents, cluster, area, bounds,
        // and the leaf-face range.
        let base = leaf * leaf_stride;
        let first = usize::from(u16_at(leafs, base + 24));
        let count = usize::from(u16_at(leafs, base + 26));
        for i in first..(first + count).min(leaf_brush_count) {
            let brush = usize::from(u16_at(leaf_brushes, i * 2));
            if !std::mem::replace(&mut seen[brush], true) {
                result.push(brush);
            }
        }
    }
    result.sort_unstable();
    Ok(result)
}

/// One convex brush: its bounding planes, its faces, and its bounds.
struct Solid {
    planes: Vec<Plane>,
    faces: Vec<(Vec<[f64; 3]>, [f64; 3])>,
    min: [f64; 3],
    max: [f64; 3],
}

impl Solid {
    fn contains(&self, p: [f64; 3]) -> bool {
        (0..3).all(|k| p[k] > self.min[k] - 1.0 && p[k] < self.max[k] + 1.0)
            && self.planes.iter().all(|plane| dot(p, plane.normal) - plane.dist < -BURIED_EPSILON)
    }
}

/// How far outside a face its samples are taken, and how deep inside another
/// brush they must lie, in Hammer units.
const BURIED_OFFSET: f64 = 0.5;
const BURIED_EPSILON: f64 = 0.01;

/// True if a face is wholly inside other brushes, like the walls between the
/// pieces of a curved ramp. Skate would otherwise meet those walls' top edges
/// at every seam of the riding surface (and snag the board). Sampled at the
/// face's centre and corners (pulled a little toward it), each just outside.
fn buried(winding: &[[f64; 3]], normal: [f64; 3], own: usize, solids: &[Solid]) -> bool {
    let n = winding.len() as f64;
    let centre = winding.iter().fold([0.0; 3], |acc, p| add(acc, scale(*p, 1.0 / n)));
    let samples = std::iter::once(centre).chain(winding.iter().map(|p| add(centre, scale(sub(*p, centre), 0.9))));
    samples.map(|p| add(p, scale(normal, BURIED_OFFSET))).all(|p| {
        solids.iter().enumerate().any(|(j, other)| j != own && other.contains(p))
    })
}

fn brush_solid(sides: &[(Plane, bool)]) -> Solid {
    let mut faces = Vec::new();
    let (mut min, mut max) = ([f64::MAX; 3], [f64::MIN; 3]);
    for (i, (plane, bevel)) in sides.iter().enumerate() {
        if *bevel {
            continue;
        }
        let mut winding = base_winding(plane);
        for (j, (other, _)) in sides.iter().enumerate() {
            if i == j {
                continue;
            }
            // Coincident duplicate planes would clip this face away entirely.
            if dot(plane.normal, other.normal) > 0.999_99 && (plane.dist - other.dist).abs() < CLIP_EPSILON {
                if j < i {
                    winding.clear();
                    break;
                }
                continue;
            }
            winding = clip_behind(&winding, other);
            if winding.len() < 3 {
                break;
            }
        }
        if winding.len() >= 3 {
            for p in &winding {
                for k in 0..3 {
                    min[k] = min[k].min(p[k]);
                    max[k] = max[k].max(p[k]);
                }
            }
            faces.push((winding, plane.normal));
        }
    }
    Solid { planes: sides.iter().map(|(plane, _)| *plane).collect(), faces, min, max }
}

fn base_winding(plane: &Plane) -> Vec<[f64; 3]> {
    let n = plane.normal;
    let axis = if n[2].abs() > 0.9 { [1.0, 0.0, 0.0] } else { [0.0, 0.0, 1.0] };
    let u = normalize(cross(axis, n));
    let v = cross(n, u);
    let o = scale(n, plane.dist);
    [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)]
        .map(|(a, b)| add(o, add(scale(u, a * HUGE), scale(v, b * HUGE))))
        .to_vec()
}

/// Keep the part of the polygon behind `plane` (inside the brush).
fn clip_behind(points: &[[f64; 3]], plane: &Plane) -> Vec<[f64; 3]> {
    let distances: Vec<f64> = points.iter().map(|p| dot(*p, plane.normal) - plane.dist).collect();
    if distances.iter().all(|d| *d <= CLIP_EPSILON) {
        return points.to_vec();
    }
    if distances.iter().all(|d| *d >= -CLIP_EPSILON) {
        return Vec::new();
    }
    let mut out = Vec::with_capacity(points.len() + 1);
    for i in 0..points.len() {
        let (p, q) = (points[i], points[(i + 1) % points.len()]);
        let (dp, dq) = (distances[i], distances[(i + 1) % points.len()]);
        if dp <= CLIP_EPSILON {
            out.push(p);
        }
        if (dp > CLIP_EPSILON && dq < -CLIP_EPSILON) || (dp < -CLIP_EPSILON && dq > CLIP_EPSILON) {
            let t = dp / (dp - dq);
            out.push(add(p, scale(sub(q, p), t)));
        }
    }
    out
}

/// Fan-triangulate a convex polygon, winding each triangle toward `normal`.
fn fan(points: &[[f64; 3]], normal: [f64; 3], out: &mut Vec<Triangle>) {
    for i in 1..points.len().saturating_sub(1) {
        push_oriented(points[0], points[i], points[i + 1], normal, out);
    }
}

fn push_oriented(a: [f64; 3], b: [f64; 3], c: [f64; 3], normal: [f64; 3], out: &mut Vec<Triangle>) {
    let n = cross(sub(b, a), sub(c, a));
    let area = dot(n, n).sqrt();
    if area < 1e-3 {
        return;
    }
    let f = |p: [f64; 3]| p.map(|v| v as f32);
    if dot(n, normal) >= 0.0 {
        out.push([f(a), f(b), f(c)]);
    } else {
        out.push([f(a), f(c), f(b)]);
    }
}

fn displacement_triangles(bsp: &Bsp, planes: &[Plane], out: &mut Vec<Triangle>) -> Result<usize, String> {
    let (infos, info_count) = bsp.lump(LUMP_DISPINFO, 176)?;
    let infos = &*infos;
    if info_count == 0 {
        return Ok(0);
    }
    let (faces, face_count) = bsp.lump(LUMP_FACES, 56)?;
    let faces = &*faces;
    let (verts, vert_count) = bsp.lump(LUMP_VERTEXES, 12)?;
    let verts = &*verts;
    let (edges, edge_count) = bsp.lump(LUMP_EDGES, 4)?;
    let edges = &*edges;
    let (surfedges, surfedge_count) = bsp.lump(LUMP_SURFEDGES, 4)?;
    let surfedges = &*surfedges;
    let (disp_verts, disp_vert_count) = bsp.lump(LUMP_DISP_VERTS, 20)?;
    let disp_verts = &*disp_verts;
    let mut built = 0;
    for d in 0..info_count {
        let base = d * 176;
        let start = vec_at(infos, base);
        let first_vert = i32_at(infos, base + 12) as usize;
        let power = i32_at(infos, base + 20);
        let face = usize::from(u16_at(infos, base + 36));
        if !(2..=4).contains(&power) || face >= face_count {
            continue;
        }
        let f = face * 56;
        let plane = planes[usize::from(u16_at(faces, f))];
        let back = faces[f + 2] != 0;
        let first_edge = i32_at(faces, f + 4) as usize;
        let edge_total = i16_at(faces, f + 8);
        if edge_total != 4 || first_edge + 4 > surfedge_count {
            continue;
        }
        let mut corners = [[0.0; 3]; 4];
        for (k, corner) in corners.iter_mut().enumerate() {
            let se = i32_at(surfedges, (first_edge + k) * 4);
            let edge = se.unsigned_abs() as usize;
            if edge >= edge_count {
                return Err(format!("Surfedge references edge {edge}"));
            }
            let v = usize::from(u16_at(edges, edge * 4 + if se >= 0 { 0 } else { 2 }));
            if v >= vert_count {
                return Err(format!("Edge references vertex {v}"));
            }
            *corner = vec_at(verts, v * 12);
        }
        // Rotate so corner 0 is the displacement's start position.
        let nearest = (0..4)
            .min_by(|&a, &b| distance2(corners[a], start).total_cmp(&distance2(corners[b], start)))
            .unwrap();
        corners.rotate_left(nearest);

        let side = (1usize << power) + 1;
        if first_vert + side * side > disp_vert_count {
            return Err(format!("Displacement {d} vertices out of range"));
        }
        let mut grid = Vec::with_capacity(side * side);
        for y in 0..side {
            let ty = y as f64 / (side - 1) as f64;
            let left = lerp(corners[0], corners[1], ty);
            let right = lerp(corners[3], corners[2], ty);
            for x in 0..side {
                let tx = x as f64 / (side - 1) as f64;
                let o = (first_vert + y * side + x) * 20;
                let offset = scale(vec_at(disp_verts, o), f64::from(f32_at(disp_verts, o + 12)));
                grid.push(add(lerp(left, right, tx), offset));
            }
        }
        let up = if back { scale(plane.normal, -1.0) } else { plane.normal };
        grid_triangles(&grid, side, up, out);
        built += 1;
    }
    Ok(built)
}

/// Triangulate a displacement grid. Every triangle keeps the grid's own
/// winding so folded terrain (cliffs, overhangs) still faces outward; the
/// orientation is decided once, from the summed normal against `up`.
fn grid_triangles(grid: &[[f64; 3]], side: usize, up: [f64; 3], out: &mut Vec<Triangle>) {
    let mut tris = Vec::with_capacity((side - 1) * (side - 1) * 2);
    for y in 0..side - 1 {
        for x in 0..side - 1 {
            let i = y * side + x;
            let (a, b, c, e) = (grid[i], grid[i + 1], grid[i + side], grid[i + side + 1]);
            // Alternate the split like CCoreDispInfo so ridges match the engine.
            if (x + y) % 2 == 0 {
                tris.push([a, c, e]);
                tris.push([a, e, b]);
            } else {
                tris.push([a, c, b]);
                tris.push([b, c, e]);
            }
        }
    }
    let total = tris.iter().fold([0.0; 3], |n, t| add(n, cross(sub(t[1], t[0]), sub(t[2], t[0]))));
    let flip = dot(total, up) < 0.0;
    for [a, b, c] in tris {
        let n = cross(sub(b, a), sub(c, a));
        if dot(n, n).sqrt() < 1e-3 {
            continue;
        }
        let f = |p: [f64; 3]| p.map(|v| v as f32);
        out.push(if flip { [f(a), f(c), f(b)] } else { [f(a), f(b), f(c)] });
    }
}

fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn cross(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
fn add(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn scale(a: [f64; 3], s: f64) -> [f64; 3] {
    a.map(|v| v * s)
}
fn normalize(a: [f64; 3]) -> [f64; 3] {
    scale(a, 1.0 / dot(a, a).sqrt())
}
fn lerp(a: [f64; 3], b: [f64; 3], t: f64) -> [f64; 3] {
    add(a, scale(sub(b, a), t))
}
fn distance2(a: [f64; 3], b: [f64; 3]) -> f64 {
    let d = sub(a, b);
    dot(d, d)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn axial_box(min: [f64; 3], max: [f64; 3]) -> Vec<(Plane, bool)> {
        let mut planes = Vec::new();
        for axis in 0..3 {
            let mut n = [0.0; 3];
            n[axis] = 1.0;
            planes.push((Plane { normal: n, dist: max[axis] }, false));
            n[axis] = -1.0;
            planes.push((Plane { normal: n, dist: -min[axis] }, false));
        }
        planes
    }

    #[test]
    fn box_brush_yields_twelve_outward_triangles() {
        let mut out = Vec::new();
        brush_triangles(&axial_box([0.0; 3], [64.0, 32.0, 16.0]), &mut out);
        assert_eq!(out.len(), 12);
        let centre = [32.0, 16.0, 8.0];
        for t in &out {
            let p = t.map(|v| v.map(f64::from));
            let n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
            assert!(dot(n, sub(p[0], centre)) > 0.0, "triangle faces into the brush");
            for v in p {
                for axis in 0..3 {
                    assert!(v[axis] >= -0.02 && v[axis] <= [64.0, 32.0, 16.0][axis] + 0.02);
                }
            }
        }
    }

    #[test]
    fn folded_displacement_keeps_grid_winding() {
        // 3x3 grid; the second row of quads folds back over the first (an
        // overhang). Per-triangle forcing would point every normal up; the
        // grid winding must leave the folded quads facing down.
        let rows = [[0.0, 0.0], [10.0, 0.0], [5.0, 8.0]];
        let mut grid = Vec::new();
        for [y, z] in rows {
            for x in 0..3 {
                grid.push([x as f64 * 10.0, y, z]);
            }
        }
        let mut out = Vec::new();
        grid_triangles(&grid, 3, [0.0, 0.0, 1.0], &mut out);
        assert_eq!(out.len(), 8);
        let facing: Vec<f64> = out
            .iter()
            .map(|t| {
                let p = t.map(|v| v.map(f64::from));
                cross(sub(p[1], p[0]), sub(p[2], p[0]))[2]
            })
            .collect();
        assert!(facing[..4].iter().all(|&z| z > 0.0), "flat part faces up: {facing:?}");
        assert!(facing[4..].iter().all(|&z| z < 0.0), "folded part keeps its winding: {facing:?}");
    }

    #[test]
    fn bevel_sides_add_no_faces() {
        let mut sides = axial_box([0.0; 3], [8.0; 3]);
        sides.push((Plane { normal: normalize([1.0, 1.0, 0.0]), dist: 16.0_f64.sqrt() * 2.0 * 2.0_f64.sqrt() }, true));
        let mut out = Vec::new();
        brush_triangles(&sides, &mut out);
        assert_eq!(out.len(), 12);
    }

    /// Parse a real map when one is supplied, e.g.
    /// SKATE_TEST_BSP=.../sourcetest/maps/background01.bsp cargo test -- --ignored
    #[test]
    #[ignore = "requires SKATE_TEST_BSP"]
    fn extracts_supplied_map() {
        let path = std::env::var("SKATE_TEST_BSP").expect("SKATE_TEST_BSP");
        let data = std::fs::read(&path).unwrap();
        let world = extract(&data).unwrap();
        let mut min = [f32::MAX; 3];
        let mut max = [f32::MIN; 3];
        for t in &world.triangles {
            for v in t {
                for a in 0..3 {
                    min[a] = min[a].min(v[a]);
                    max[a] = max[a].max(v[a]);
                }
            }
        }
        eprintln!(
            "{path}: {} triangles, {} brushes, {} displacements, bounds {min:?}..{max:?}",
            world.triangles.len(),
            world.brushes,
            world.displacements
        );
        assert!(!world.triangles.is_empty());
        if let Ok(obj) = std::env::var("SKATE_TEST_OBJ") {
            let mut text = String::new();
            for t in &world.triangles {
                for v in t {
                    text += &format!("v {} {} {}\n", v[0], v[1], v[2]);
                }
            }
            for i in 0..world.triangles.len() {
                text += &format!("f {} {} {}\n", 3 * i + 1, 3 * i + 2, 3 * i + 3);
            }
            std::fs::write(obj, text).unwrap();
        }
        assert!(max.iter().zip(min).all(|(hi, lo)| hi - lo < 65536.0));
    }
}
