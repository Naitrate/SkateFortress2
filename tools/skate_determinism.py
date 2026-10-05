#!/usr/bin/env python3
"""Check that the Skate 3 simulation repeats itself exactly.

Client prediction (the client simulating its own skater beside the server)
only works if the same inputs give the same result. This runs one skater
through a fixed input script in several libskate3 instances and compares
every STEP reply byte for byte:

  repeat   a second instance, same inputs: the client/server case
  crowd    a third instance whose skater shares the map with other skaters
           riding different inputs: the busy-server case
  copy     a copy (COPY) made partway through carries on exactly like the
           original
  rewind   a skater fed wrong inputs for a while, then put back to an
           earlier copy and fed the right ones: the client's misprediction
           recovery

    SKATE3_ASSET_ROOT=... tools/skate_determinism.py map.bsp --lib libskate3.so [--seconds 20]
"""
import argparse
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sidecar_smoke import DESPAWN, IN_JUMP, SPAWN, STEP, WORLD, Library, player_start, request, string  # noqa: E402

IN_ATTACK, IN_ATTACK2, IN_DUCK = 1 << 0, 1 << 11, 1 << 2
DT = 0.015
LOADING = 0xFFFFFFFF
COPY = 6


def script(seconds, seed):
    """Per-step (buttons, forward, side, mouse_x, mouse_y, flags): a scripted
    push and ollie, then seeded random riding with flicks and a forced bail."""
    rng = random.Random(seed)
    steps = []
    held = (0, 1.0, 0.0)
    for i in range(int(seconds / DT)):
        t = i * DT
        mouse = (0.0, 0.0)
        flags = 0
        if t < 4.0:
            buttons = IN_JUMP if 0.2 < t < 0.5 or 1.2 < t < 1.5 else 0
            forward, side = (1.0 if t < 3.0 else 0.0), 0.0
            if 3.0 <= t < 3.1:
                mouse = (0.0, 60.0)
            elif 3.1 <= t < 3.15:
                mouse = (0.0, -200.0)
        else:
            if rng.random() < 0.04:   # change what's held now and then
                held = (rng.choice([0, 0, IN_JUMP, IN_DUCK, IN_ATTACK, IN_ATTACK2]),
                        rng.choice([-1.0, 0.0, 1.0, 1.0]), rng.choice([-1.0, 0.0, 0.0, 1.0]))
            buttons, forward, side = held
            if rng.random() < 0.05:
                mouse = (rng.uniform(-150, 150), rng.uniform(-200, 200))
            if abs(t - seconds * 0.7) < DT / 2:
                flags = 1   # STEP_FORCE_WIPEOUT
        steps.append((buttons, forward, side, *mouse, flags))
    return steps


class Instance:
    def __init__(self, lib, root, bsp, origin, yaw):
        self.lib = Library(lib, root)
        request(self.lib, WORLD, string("determinism") + struct.pack("<fI", 0.0254, len(bsp)) + bsp + struct.pack("<II", 0, 0))
        self.origin, self.yaw = origin, yaw

    def spawn(self, ids):
        for sid in ids:
            request(self.lib, SPAWN, struct.pack("<I3ff", sid, *self.origin, self.yaw) + string(""))
        for sid in ids:
            while struct.unpack_from("<I", self.step(sid, (0, 0.0, 0.0, 0.0, 0.0, 0), dt=0.0))[0] == LOADING:
                time.sleep(0.005)

    def copy(self, src, dst):
        request(self.lib, COPY, struct.pack("<II", src, dst))

    def step(self, sid, inputs, dt=DT):
        buttons, forward, side, mx, my, flags = inputs
        return request(self.lib, STEP, struct.pack("<IfIffffIff", sid, dt, buttons, forward, side, mx, my, flags, 0.0, 0.0))


def summary(reply):
    values = struct.unpack_from("<II" + "f" * 22, reply)
    return values[0], values[2:5], values[8:11]


def run(inst, steps, crowd=()):
    """Skater 1 rides `steps`; the skaters in `crowd` ride their own scripts in between."""
    out = []
    for i, inputs in enumerate(steps):
        for sid, other in crowd:
            inst.step(sid, other[i])
        out.append(inst.step(1, inputs))
    return out


def compare(name, ref, got):
    worst = 0.0
    first = None
    for i, (a, b) in enumerate(zip(ref, got)):
        if a == b:
            continue
        if first is None:
            first = i
        sa, sb = summary(a), summary(b)
        worst = max(worst, max(abs(x - y) for x, y in zip(sa[1], sb[1])))
    if first is None:
        print(f"{name:7s} IDENTICAL over {len(ref)} steps ({len(ref) * DT:.1f} s)")
        return True
    sa, sb = summary(ref[first]), summary(got[first])
    print(f"{name:7s} DIVERGED at step {first} (t={first * DT:.2f} s): state {sa[0]} vs {sb[0]}, "
          f"origin {tuple(round(v, 3) for v in sa[1])} vs {tuple(round(v, 3) for v in sb[1])}; "
          f"worst origin gap {worst:.3f} units; {sum(a != b for a, b in zip(ref, got))} of {len(ref)} steps differ")
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bsp")
    parser.add_argument("--lib", required=True)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--crowd", type=int, default=3, help="other skaters in the crowd run")
    args = parser.parse_args()

    root = os.environ["SKATE3_ASSET_ROOT"]
    bsp = open(args.bsp, "rb").read()
    origin, yaw = player_start(bsp)
    steps = script(args.seconds, args.seed)

    ref_inst = Instance(args.lib, root, bsp, origin, yaw)
    ref_inst.spawn([1])
    ref = run(ref_inst, steps)
    states = sorted({summary(r)[0] for r in ref})
    print(f"reference: {len(ref)} steps, states seen {states}, ends at {tuple(round(v, 1) for v in summary(ref[-1])[1])}")

    ok = True
    rep_inst = Instance(args.lib, root, bsp, origin, yaw)
    rep_inst.spawn([1])
    ok &= compare("repeat", ref, run(rep_inst, steps))

    crowd_inst = Instance(args.lib, root, bsp, origin, yaw)
    others = [(10 + n, script(args.seconds, args.seed + 100 + n)) for n in range(args.crowd)]
    crowd_inst.spawn([1] + [sid for sid, _ in others])
    ok &= compare("crowd", ref, run(crowd_inst, steps, others))

    # A copy made partway carries on like the original.
    split = len(steps) // 3
    copy_inst = Instance(args.lib, root, bsp, origin, yaw)
    copy_inst.spawn([1])
    head = run(copy_inst, steps[:split])
    copy_inst.copy(1, 2)
    rng = random.Random(args.seed + 7)
    tail = []
    for inputs in steps[split:]:
        copy_inst.step(1, (rng.choice([0, IN_JUMP]), rng.uniform(-1, 1), rng.uniform(-1, 1), 0.0, 0.0, 0))  # the original wanders off
        tail.append(copy_inst.step(2, inputs))
    ok &= compare("copy", ref, head + tail)

    # Rewind, as the client predicts: skater 1 runs ahead on guessed inputs
    # (here it wrongly guesses a bail now and then), skater 2 follows the
    # server's confirmed inputs `window` commands behind. A wrong guess found
    # on confirming puts 1 back to a copy of 2 and replays the rest.
    rewind_inst = Instance(args.lib, root, bsp, origin, yaw)
    rewind_inst.spawn([1])
    rewind_inst.copy(1, 2)
    guesses = [inputs[:5] + (1,) if i % 300 == 150 else inputs for i, inputs in enumerate(steps)]
    window = 8                                  # ~120 ms of commands in flight
    predicted = []
    confirmed = 0
    rewinds = 0

    def confirm_up_to(limit):
        nonlocal confirmed, rewinds
        while confirmed < limit:
            answer = rewind_inst.step(2, steps[confirmed])
            wrong = guesses[confirmed] != steps[confirmed]
            confirmed += 1
            if wrong:
                rewinds += 1
                predicted[confirmed - 1] = answer
                rewind_inst.copy(2, 1)
                for j in range(confirmed, len(predicted)):
                    predicted[j] = rewind_inst.step(1, guesses[j])

    for i in range(len(steps)):
        predicted.append(rewind_inst.step(1, guesses[i]))
        confirm_up_to(i + 1 - window)
    confirm_up_to(len(steps))
    print(f"rewind: {rewinds} rewinds")
    ok &= compare("rewind", ref, predicted)

    for inst in (ref_inst, rep_inst, crowd_inst, copy_inst, rewind_inst):
        request(inst.lib, DESPAWN, struct.pack("<I", 1))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
