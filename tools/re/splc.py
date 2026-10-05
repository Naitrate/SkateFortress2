#!/usr/bin/env python3
"""Parse EA Splicer (SPLC v3) banks, as read by Skate 3's runtime (TU3
0x82975700 construct / 0x829757D0 init / 0x82976DD8 pick).

Header: "SPLC", version, data offset, nSampleRefs @0xC, nClusters @0x10,
nSamples @0x18, name @0x1C. Then:
  sample refs  @0x3C, 36 bytes: u16 index @4, u8 layerCount @7,
               f32 duration_ms @0x18; the layer pointer @0x20 is zero on disk
               and filled at load: each ref's layers follow the previous ref's
  clusters     72 bytes: u16 refs[32] @4 (0xFFFF = empty), u8 mode @0x44
  layers       12-byte header (u8 sampleCount @8, u8 mode @9) followed by
               sampleCount x 72-byte sample descriptors (u16 sample @0,
               f32 probability @0x40)
A splice ID below nSampleRefs is a sample ref; up to nSampleRefs+nClusters it
is a cluster whose refs are picked by its mode. Samples index the bank's
SNR/SNS audio in storage order.
"""
import struct


class Splc:
    def __init__(self, data: bytes):
        if data[:4] != b"SPLC":
            raise ValueError("not SPLC")
        self.d = data
        self.n_refs, self.n_clusters = struct.unpack_from(">II", data, 0xC)
        self.n_samples = struct.unpack_from(">I", data, 0x18)[0]
        self.name = data[0x1C:0x3C].split(b"\0")[0].lstrip(b"\x00").decode("latin-1")
        self.refs_base = 0x3C
        self.clusters_base = self.refs_base + 36 * self.n_refs
        self.layers_base = self.clusters_base + 72 * self.n_clusters
        # Layers are stored back to back in ref order.
        self.layer_start = []
        p = self.layers_base
        for i in range(self.n_refs):
            self.layer_start.append(p)
            for _ in range(data[self.refs_base + 36 * i + 7]):
                p += 12 + 72 * data[p + 8]
        self.layers_end = p

    def ref(self, i):
        o = self.refs_base + 36 * i
        index = struct.unpack_from(">H", self.d, o + 4)[0]
        layer_count = self.d[o + 7]
        duration_ms = struct.unpack_from(">f", self.d, o + 0x18)[0]
        layers = []
        p = self.layer_start[i]
        for _ in range(layer_count):
            count, mode = self.d[p + 8], self.d[p + 9]
            samples = []
            for k in range(count):
                s = p + 12 + 72 * k
                samples.append((struct.unpack_from(">H", self.d, s)[0], struct.unpack_from(">f", self.d, s + 0x40)[0]))
            layers.append({"mode": mode, "samples": samples})
            p += 12 + 72 * count
        return {"ref": i, "index": index, "duration_ms": duration_ms, "layers": layers}

    def cluster(self, i):
        o = self.clusters_base + 72 * i
        refs = [r for r in struct.unpack_from(">32H", self.d, o + 4) if r != 0xFFFF]
        return {"cluster": i, "mode": self.d[o + 0x44], "refs": refs}

    def splice(self, splice_id):
        """Every audio sample a splice ID can play, with the path taken."""
        if splice_id < self.n_refs:
            refs = [splice_id]
            via = None
        elif splice_id < self.n_refs + self.n_clusters:
            via = self.cluster(splice_id - self.n_refs)
            refs = via["refs"]
        else:
            raise ValueError(f"splice {splice_id} out of range")
        return {"id": splice_id, "cluster": via, "refs": [self.ref(r) for r in refs]}

    def samples(self, splice_id):
        out = []
        for ref in self.splice(splice_id)["refs"]:
            for layer in ref["layers"]:
                out.extend(s for s, _ in layer["samples"])
        return out
