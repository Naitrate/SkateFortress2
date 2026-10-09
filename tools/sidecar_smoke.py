#!/usr/bin/env python3
"""Drive a running skate3-sidecar without TF2.

Loads a BSP, spawns a skater at the map's first player start (or --origin),
then pushes forward and ollies, printing the skater's state and position.

    tools/sidecar_smoke.py path/to/map.bsp [--seconds 6] [--origin x y z]
"""
import argparse
import lzma
import re
import socket
import time
import struct
import sys

HELLO, WORLD, SPAWN, STEP, DESPAWN = 1, 2, 3, 4, 5
IN_JUMP = 1 << 1
IN_GRENADE1 = 1 << 23   # the mod's Bail (dive) action
BONES = ["board root", "skull", "neck", "left wrist", "left forearm", "left humerus", "left collarbone",
         "right wrist", "right forearm", "right humerus", "right collarbone", "upper back", "ribs",
         "spine", "lower back", "left toes", "left ankle", "left shin", "left femur", "right toes",
         "right ankle", "right shin", "right femur", "pelvis"]


class Library:
    """libskate3.so in this process, as the game's server.so loads it."""

    def __init__(self, path, asset_root):
        import ctypes
        self.ct = ctypes
        lib = ctypes.CDLL(str(path))
        lib.skate3_open.restype = ctypes.c_void_p
        lib.skate3_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        lib.skate3_request.argtypes = [ctypes.c_void_p, ctypes.c_uint8, ctypes.c_char_p, ctypes.c_size_t,
                                       ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
        lib.skate3_free.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        lib.skate3_close.argtypes = [ctypes.c_void_p]
        error = ctypes.create_string_buffer(1024)
        self.handle = lib.skate3_open(str(asset_root).encode(), error, len(error))
        if not self.handle:
            raise RuntimeError(error.value.decode())
        self.lib = lib

    def request(self, kind, payload):
        reply, size = self.ct.c_void_p(), self.ct.c_size_t()
        if self.lib.skate3_request(self.handle, kind, payload, len(payload), self.ct.byref(reply), self.ct.byref(size)) != 0:
            raise RuntimeError("skate3_request failed")
        body = self.ct.string_at(reply, size.value)
        self.lib.skate3_free(reply, size)
        return body


def request(sock, kind, payload=b""):
    if isinstance(sock, Library):
        body = sock.request(kind, payload)
        if body[0] == 0:
            (n,) = struct.unpack_from("<I", body, 1)
            raise RuntimeError(body[5 : 5 + n].decode())
        return body[1:]
    sock.sendall(struct.pack("<IB", len(payload) + 1, kind) + payload)
    header = recv(sock, 5)
    length, reply_kind = struct.unpack("<IB", header)
    body = recv(sock, length - 1)
    if reply_kind != kind:
        raise RuntimeError(f"reply type {reply_kind} for request {kind}")
    if body[0] == 0:
        (n,) = struct.unpack_from("<I", body, 1)
        raise RuntimeError(body[5 : 5 + n].decode())
    return body[1:]


def recv(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise RuntimeError("sidecar closed the connection")
        data += chunk
    return data


def string(s):
    b = s.encode()
    return struct.pack("<I", len(b)) + b


def player_start(bsp):
    """Origin and yaw of the first info_player_* entity in the entity lump."""
    offset, length = struct.unpack_from("<ii", bsp, 8)
    lump = bsp[offset : offset + length]
    if lump[:4] == b"LZMA":
        # Valve lzma_header_t -> classic .lzma header (props + u64 size).
        actual, compressed = struct.unpack_from("<II", lump, 4)
        alone = lump[12:17] + struct.pack("<Q", actual) + lump[17 : 17 + compressed]
        lump = lzma.decompress(alone, format=lzma.FORMAT_ALONE)
    text = lump.decode("latin-1")
    for block in re.findall(r"\{[^{}]*\}", text):
        if re.search(r'"classname"\s+"info_player_(start|teamspawn|deathmatch)"', block):
            origin = [float(v) for v in re.search(r'"origin"\s+"([^"]+)"', block).group(1).split()]
            angles = re.search(r'"angles"\s+"([^"]+)"', block)
            yaw = float(angles.group(1).split()[1]) if angles else 0.0
            return origin, yaw
    raise RuntimeError("no player start in map; pass --origin")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bsp")
    parser.add_argument("--port", type=int, default=27720)
    parser.add_argument("--lib", help="load this libskate3.so in-process instead of connecting (needs SKATE3_ASSET_ROOT)")
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--scale", type=float, default=0.0254)
    parser.add_argument("--origin", type=float, nargs=3)
    parser.add_argument("--yaw", type=float)
    parser.add_argument("--bail-at", type=float, help="force a wipeout at this time (seconds)")
    parser.add_argument("--velocity", type=float, nargs=3, default=[0.0, 0.0, 0.0], help="spawn velocity (units/s), e.g. mid rocket jump")
    parser.add_argument("--dive-at", type=float, help="press the Bail (dive) button at this time (seconds)")
    parser.add_argument("--kick", type=float, nargs=4, metavar=("T", "VX", "VY", "VZ"), help="add this velocity (units/s) at time T, like an explosion")
    args = parser.parse_args()

    bsp = open(args.bsp, "rb").read()
    origin, yaw = player_start(bsp) if args.origin is None else (args.origin, 0.0)
    if args.yaw is not None:
        yaw = args.yaw

    if args.lib:
        import os
        sock = Library(args.lib, os.environ["SKATE3_ASSET_ROOT"])
    else:
        sock = socket.create_connection(("127.0.0.1", args.port))
    if not args.lib:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    (version,) = struct.unpack("<I", request(sock, HELLO))
    print(f"protocol {version}")
    reply = request(sock, WORLD, string("smoke") + struct.pack("<f", args.scale) + struct.pack("<I", len(bsp)) + bsp + struct.pack("<II", 0, 0))  # no extra triangles, no rails
    print("world:", reply[4:].decode())
    reply = request(sock, SPAWN, struct.pack("<I3ff", 1, *origin, yaw) + string("") + struct.pack("<3f", *args.velocity))
    print("spawn:", reply[4:].decode())
    # The skater loads on a worker thread; STEP answers LOADING until it is ready.
    waited = time.monotonic()
    while struct.unpack_from("<I", request(sock, STEP, struct.pack("<IfIffffIff3f", 1, 0.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)))[0] == 0xFFFFFFFF:
        time.sleep(0.005)
    print(f"skater ready after {time.monotonic() - waited:.2f}s")

    dt = 0.015
    ticks = int(args.seconds / dt)
    last_state = None
    last_seq = None
    last_breaks = 0
    for i in range(ticks):
        t = i * dt
        buttons = 0
        forward = 1.0 if t < 3.0 else 0.0
        mouse_y = 0.0
        # Push for a while, then ollie: mouse back (down) then flick forward (up).
        if 0.2 < t < 0.5 or 1.2 < t < 1.5:
            buttons |= IN_JUMP
        if 3.0 <= t < 3.1:
            mouse_y = 60.0
        elif 3.1 <= t < 3.15:
            mouse_y = -200.0
        flags = 1 if args.bail_at is not None and args.bail_at <= t < args.bail_at + dt else 0
        if args.dive_at is not None and args.dive_at <= t < args.dive_at + 0.1:
            buttons |= IN_GRENADE1
        kick = args.kick[1:] if args.kick and args.kick[0] <= t < args.kick[0] + dt else (0.0, 0.0, 0.0)
        payload = struct.pack("<IfIffffIff3f", 1, dt, buttons, forward, 0.0, 0.0, mouse_y, flags, 0.0, 0.0, *kick)
        body = request(sock, STEP, payload)
        values = struct.unpack_from("<II" + "f" * 22, body)
        state, native_ticks = values[0], values[1]
        o = values[2:5]
        v = values[8:11]
        speed = (v[0] ** 2 + v[1] ** 2 + v[2] ** 2) ** 0.5
        off = 8 + 22 * 4
        (count,) = struct.unpack_from("<I", body, off)
        off += 4 + count * 12
        seq, n = struct.unpack_from("<II", body, off)
        trick = body[off + 8 : off + 8 + n].decode()
        trick_score, line, mult, total, sflags = struct.unpack_from("<ffffI", body, off + 8 + n)
        bail, broken, meat, breaks, bone = struct.unpack_from("<IIfII", body, off + 8 + n + 20)
        if breaks != last_breaks:
            print(f"   BONE #{breaks}: {BONES[bone] if bone < len(BONES) else bone}  (bail {bail}, score {meat:.0f}, broken {bin(broken).count('1')})")
            last_breaks = breaks
        if seq != last_seq:
            print(f"   TRICK #{seq} '{trick}' +{trick_score:.0f}  line={line:.0f} x{mult:.1f} total={total:.0f} flags={sflags:03b}")
            last_seq = seq
        if state != last_state or i % 40 == 0:
            print(f"t={t:5.2f} state={state:3d} origin=({o[0]:8.1f} {o[1]:8.1f} {o[2]:8.1f}) "
                  f"speed={speed:6.1f} yaw={values[6]:6.1f} cam_rel=({values[17]-o[0]:6.1f} {values[18]-o[1]:6.1f} {values[19]-o[2]:6.1f}) cam_ang=({values[20]:5.1f} {values[21]:6.1f} {values[22]:5.1f}) fov={values[23]:4.1f}")
            last_state = state
    request(sock, DESPAWN, struct.pack("<I", 1))
    print("ok")


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:
        print("error:", error, file=sys.stderr)
        sys.exit(1)
