#!/usr/bin/env python3
"""Generate skate_park.vmf: a test skatepark for the skate mechanics.

    tools/make_skatepark.py [out.vmf]      (default game/mod_tf/mapsrc/skate_park.vmf)

Then compile it with tools/compile_map.sh. The .vmf also opens in Hammer.

Units are Hammer units (the mod's skate_world_scale 0.0254: one unit is an
inch). The park, inside a sky-roofed box:

  - a halfpipe (two 9 ft quarter pipes, a flat bottom) with coping rails
  - a quarter pipe and a spine
  - a funbox: kicker up, flat top with a ledge, ramp down
  - ledges and manual pads of a few heights
  - flat rails, a stair set with a handrail
  - kickers of three heights, and a big drop-in for air

Every rail, coping and ledge edge also gets a skate_rail chain (skate.fgd),
so Skate grinds them for sure.
"""
import math
import os
import sys

FLOOR = "dev/dev_measuregeneric01b"
WALL = "dev/dev_measurewall01d"
RAMP = "dev/dev_measuregeneric01"
METAL = "metal/metalwall001a"
SKY = "tools/toolsskybox"

_ids = iter(range(1, 1_000_000))


def nid():
    return next(_ids)


class Brush:
    """A convex solid from its vertices and faces (vertex index loops)."""

    def __init__(self, verts, faces, material):
        self.verts, self.faces, self.material = verts, faces, material

    def vmf(self):
        cx = sum(v[0] for v in self.verts) / len(self.verts)
        cy = sum(v[1] for v in self.verts) / len(self.verts)
        cz = sum(v[2] for v in self.verts) / len(self.verts)
        out = [f'\tsolid\n\t{{\n\t\t"id" "{nid()}"']
        for face in self.faces:
            p = [self.verts[i] for i in face[:3]]
            # Hammer: normal = (p2 - p0) x (p1 - p0) points out of the solid.
            n = cross(sub(p[2], p[0]), sub(p[1], p[0]))
            if dot(n, sub(p[0], (cx, cy, cz))) < 0:
                p = [p[0], p[2], p[1]]
                n = (-n[0], -n[1], -n[2])
            u, v = axes(n)
            plane = " ".join("(%g %g %g)" % q for q in p)
            out.append(
                f'\t\tside\n\t\t{{\n\t\t\t"id" "{nid()}"\n\t\t\t"plane" "{plane}"\n'
                f'\t\t\t"material" "{self.material.upper()}"\n'
                f'\t\t\t"uaxis" "[{u[0]} {u[1]} {u[2]} 0] 0.25"\n\t\t\t"vaxis" "[{v[0]} {v[1]} {v[2]} 0] 0.25"\n'
                f'\t\t\t"rotation" "0"\n\t\t\t"lightmapscale" "16"\n\t\t\t"smoothing_groups" "0"\n\t\t}}'
            )
        out.append("\t}")
        return "\n".join(out)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def axes(n):
    """World-aligned texture axes, as Hammer picks them."""
    ax = [abs(n[0]), abs(n[1]), abs(n[2])]
    if ax[2] >= ax[0] and ax[2] >= ax[1]:
        return (1, 0, 0), (0, -1, 0)
    if ax[0] >= ax[1]:
        return (0, 1, 0), (0, 0, -1)
    return (1, 0, 0), (0, 0, -1)


def box(x0, y0, z0, x1, y1, z1, material):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4], [1, 2, 6, 5], [2, 3, 7, 6], [3, 0, 4, 7]]
    return Brush(v, f, material)


def prism(profile, a0, a1, axis, material):
    """Extrude a convex 2D profile [(s, z)] along `axis` ('x' or 'y') from a0 to
    a1; s runs along the other horizontal axis."""
    # Drop repeated corners (a ramp's tip meets the ground in one point).
    profile = [pt for i, pt in enumerate(profile) if pt != profile[i - 1]]
    n = len(profile)
    def at(s, z, a):
        return (a, s, z) if axis == "x" else (s, a, z)
    v = [at(s, z, a0) for s, z in profile] + [at(s, z, a1) for s, z in profile]
    f = [list(range(n)), list(range(n, 2 * n))]
    for i in range(n):
        j = (i + 1) % n
        f.append([i, j, n + j, n + i])
    return Brush(v, f, material)


def quarter_pipe(s0, direction, z0, radius, a0, a1, axis, deck=128, segments=12, material=RAMP):
    """A quarter pipe whose transition starts at s0 (flat ground at z0) and
    curves up to vertical `radius` further along `direction` (+1/-1), with a
    deck behind it. Returns the brushes and the lip's (s, z)."""
    brushes = []
    for i in range(segments):
        t0, t1 = i / segments * math.pi / 2, (i + 1) / segments * math.pi / 2
        # Circle centred radius above the start, ground tangent at s0.
        sa, za = radius * math.sin(t0), radius * (1 - math.cos(t0))
        sb, zb = radius * math.sin(t1), radius * (1 - math.cos(t1))
        pa, pb = s0 + direction * sa, s0 + direction * sb
        lo, hi = sorted((pa, pb))
        za_, zb_ = (za, zb) if pa < pb else (zb, za)
        if hi - lo < 0.5:
            continue
        brushes.append(prism([(lo, z0), (hi, z0), (hi, z0 + zb_), (lo, z0 + za_)], a0, a1, axis, material))
    lip = s0 + direction * radius
    if deck > 0:
        lo, hi = sorted((lip, lip + direction * deck))
        brushes.append(prism([(lo, z0), (hi, z0), (hi, z0 + radius), (lo, z0 + radius)], a0, a1, axis, FLOOR))
    return brushes, (lip, z0 + radius)


def wedge(s0, s1, z0, height, a0, a1, axis, material=RAMP):
    """A kicker: rising from s0 (ground) to `height` at s1."""
    return prism([(s0, z0), (s1, z0), (s1, z0 + height)], a0, a1, axis, material)


def rail_brush(x0, y0, z0, x1, y1, z1, material=METAL, half=2):
    """A square rail between two points (horizontal or sloped along x or y)."""
    if abs(x1 - x0) >= abs(y1 - y0):
        prof = [(y0 - half, -half), (y0 + half, -half), (y0 + half, half), (y0 - half, half)]
        dz = (z1 - z0)
        v = [(x0, s, z0 + z) for s, z in prof] + [(x1, s, z1 + z) for s, z in prof]
    else:
        prof = [(x0 - half, -half), (x0 + half, -half), (x0 + half, half), (x0 - half, half)]
        v = [(s, y0, z0 + z) for s, z in prof] + [(s, y1, z1 + z) for s, z in prof]
    f = [[0, 1, 2, 3], [4, 5, 6, 7]] + [[i, (i + 1) % 4, 4 + (i + 1) % 4, 4 + i] for i in range(4)]
    return Brush(v, f, material)


class Map:
    def __init__(self):
        self.brushes, self.entities, self.rails = [], [], 0

    def add(self, *brushes):
        for b in brushes:
            if isinstance(b, list):
                self.brushes.extend(b)
            else:
                self.brushes.append(b)

    def entity(self, classname, origin, **keys):
        self.entities.append((classname, origin, keys))

    def rail(self, points, name):
        """A skate_rail chain along points (rail tops)."""
        self.rails += 1
        for i, p in enumerate(points):
            keys = {"targetname": f"{name}_{i}"}
            if i + 1 < len(points):
                keys["target"] = f"{name}_{i + 1}"
            self.entity("skate_rail", p, **keys)

    def vmf(self):
        out = ['versioninfo\n{\n\t"editorversion" "400"\n\t"editorbuild" "8864"\n\t"mapversion" "1"\n\t"formatversion" "100"\n\t"prefab" "0"\n}',
               'world\n{\n\t"id" "1"\n\t"mapversion" "1"\n\t"classname" "worldspawn"\n\t"skyname" "sky_day01_01"\n\t"maxpropscreenwidth" "-1"\n\t"detailvbsp" "detail.vbsp"\n\t"detailmaterial" "detail/detailsprites"']
        out += [b.vmf() for b in self.brushes]
        out.append("}")
        for classname, origin, keys in self.entities:
            lines = [f'entity\n{{\n\t"id" "{nid()}"\n\t"classname" "{classname}"\n\t"origin" "{origin[0]:g} {origin[1]:g} {origin[2]:g}"']
            lines += [f'\t"{k}" "{v}"' for k, v in keys.items()]
            lines.append("}")
            out.append("\n".join(lines))
        return "\n".join(out) + "\n"


def build():
    m = Map()
    S, H, T = 3072, 2048, 64   # half size, height, wall thickness
    # Shell: floor, walls, sky.
    m.add(box(-S - T, -S - T, -T, S + T, S + T, 0, FLOOR))
    m.add(box(-S - T, -S - T, H, S + T, S + T, H + T, SKY))
    m.add(box(-S - T, -S - T, 0, -S, S + T, H, WALL), box(S, -S - T, 0, S + T, S + T, H, WALL))
    m.add(box(-S, -S - T, 0, S, -S, H, WALL), box(-S, S, 0, S, S + T, H, WALL))

    # Halfpipe along x, north: two quarter pipes facing each other across a
    # 640 flat bottom, 1280 long. Coping rails along both lips.
    hp_x0, hp_x1, r, flat = -1600, -320, 384, 640
    y_mid = 2000
    left, lip_l = quarter_pipe(y_mid - flat / 2, -1, 0, r, hp_x0, hp_x1, "x")
    right, lip_r = quarter_pipe(y_mid + flat / 2, +1, 0, r, hp_x0, hp_x1, "x")
    m.add(left, right)
    for lip_s, lip_z, name in ((lip_l[0], lip_l[1], "coping_hp_a"), (lip_r[0], lip_r[1], "coping_hp_b")):
        m.add(rail_brush(hp_x0, lip_s, lip_z + 2, hp_x1, lip_s, lip_z + 2, METAL))
        m.rail([(hp_x0 + 16, lip_s, lip_z + 4), (hp_x1 - 16, lip_s, lip_z + 4)], name)

    # A lone 6 ft quarter pipe along the east wall, facing west.
    qp, qlip = quarter_pipe(S - 200 - 256, +1, 0, 256, -800, 800, "y", deck=200)
    m.add(qp)
    m.add(rail_brush(qlip[0], -800, qlip[1] + 2, qlip[0], 800, qlip[1] + 2))
    m.rail([(qlip[0], -784, qlip[1] + 4), (qlip[0], 784, qlip[1] + 4)], "coping_qp")

    # A spine: two 4 ft quarter pipes back to back, west side.
    sp_a, _ = quarter_pipe(-2200 + 192, -1, 0, 192, -1200, -400, "y", deck=0)
    sp_b, spine_lip = quarter_pipe(-2200 - 192, +1, 0, 192, -1200, -400, "y", deck=0)
    m.add(sp_a, sp_b)
    m.rail([(-2200, -1184, 196), (-2200, -416, 196)], "spine")

    # Funbox in the middle: kicker up (24 high), flat top 512 long with a
    # ledge, ramp down the far side.
    fb_y0, fb_y1, fb_h = -300, 300, 24
    m.add(wedge(-700, -400, 0, fb_h, fb_y0, fb_y1, "y"))
    m.add(box(-400, fb_y0, 0, 112, fb_y1, fb_h, FLOOR))
    m.add(prism([(112, 0), (412, 0), (112, fb_h)], fb_y0, fb_y1, "y", RAMP))
    m.add(box(-300, fb_y1 - 48, fb_h, 0, fb_y1, fb_h + 16, WALL))     # ledge on top
    m.rail([(-296, fb_y1 - 48, fb_h + 16), (-4, fb_y1 - 48, fb_h + 16)], "funbox_ledge")
    # A flat rail beside the funbox.
    m.add(box(-404, fb_y0 - 120, 0, -396, fb_y0 - 112, 20, METAL), box(-4, fb_y0 - 120, 0, 4, fb_y0 - 112, 20, METAL))
    m.add(rail_brush(-420, fb_y0 - 116, 22, 20, fb_y0 - 116, 22))
    m.rail([(-416, fb_y0 - 116, 24), (16, fb_y0 - 116, 24)], "flat_rail_funbox")

    # Ledges and manual pads, south: 8, 16 and 24 high.
    for i, h in enumerate((8, 16, 24)):
        x0 = -1800 + i * 500
        m.add(box(x0, -1700, 0, x0 + 384, -1640, h, WALL))
        m.rail([(x0 + 4, -1640, h), (x0 + 380, -1640, h)], f"ledge_{h}")
        m.add(box(x0, -2100, 0, x0 + 256, -1900, h / 2, FLOOR))         # manual pad

    # Stair set with a handrail, south-east: 6 steps of 12 down, 24 deep, a
    # platform before it and a run-in kicker up to the platform.
    st_x0, st_x1, top = 600, 900, 72
    m.add(wedge(-260, 340, 0, top, st_x0, st_x1, "x"))                # run-up ramp (y -260..340)
    m.add(box(st_x0, 340, 0, st_x1, 600, top, FLOOR))                   # platform
    for i in range(5):              # the sixth step down is the ground
        y0 = 600 + i * 24
        m.add(box(st_x0, y0, 0, st_x1, y0 + 24, top - (i + 1) * 12, FLOOR))
    rail_x = st_x1 + 24
    m.add(box(rail_x - 4, 560, 0, rail_x + 4, 568, top + 24, METAL))
    m.add(box(rail_x - 4, 744, 0, rail_x + 4, 752, 24, METAL))
    m.add(rail_brush(rail_x, 564, top + 26, rail_x, 748, 26))
    m.rail([(rail_x, 568, top + 28), (rail_x, 744, 28)], "stair_handrail")

    # Kickers: 24, 48, 96 high, east middle, facing north.
    for i, h in enumerate((24, 48, 96)):
        x0 = 1200 + i * 360
        m.add(wedge(-1400, -1400 + h * 3.5, 0, h, x0, x0 + 256, "x"))

    # Long flat rail for grinds, mid west.
    m.add(box(-1600, 900, 0, -1592, 908, 18, METAL), box(-608, 900, 0, -600, 908, 18, METAL))
    m.add(rail_brush(-1620, 904, 20, -580, 904, 20))
    m.rail([(-1616, 904, 22), (-584, 904, 22)], "flat_rail_long")

    # Big drop-in for air: a 512 high platform reached by a stair-free ramp,
    # rolling down a 30 degree slope into a launch kicker.
    di_x0, di_x1 = 1600, 2000
    m.add(box(di_x0, 2400, 0, 2350, 2900, 512, FLOOR))                  # tower, roll-in side and ramp side
    m.add(prism([(1500, 0), (2400, 0), (2400, 512)], di_x0, di_x1, "x", RAMP))   # roll-in down to y=1500
    m.add(wedge(1100, 800, 0, 64, di_x0, di_x1, "x"))                    # launch kicker, rising toward -y
    # Ladder-free way up: a long gentle ramp along the east wall to the tower.
    m.add(prism([(-200, 0), (2400, 0), (2400, 512)], 2050, 2350, "x", RAMP))

    # Spawns: centre south, facing north.
    for team, x in ((2, -200), (3, 200)):
        for k in range(4):
            m.entity("info_player_teamspawn", (x + (k % 2) * 64, -900 - (k // 2) * 64, 8), TeamNum=team, angles="0 90 0")
    m.entity("info_player_start", (0, -900, 8), angles="0 90 0")
    return m


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "game", "mod_tf", "mapsrc", "skate_park.vmf")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    m = build()
    with open(out, "w") as f:
        f.write(m.vmf())
    print(f"{out}: {len(m.brushes)} brushes, {m.rails} skate_rail chains, {len(m.entities)} entities")


if __name__ == "__main__":
    main()
