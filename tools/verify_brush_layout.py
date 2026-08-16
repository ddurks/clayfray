#!/usr/bin/env python3
"""Check the three-brush rest-space layout against the asset and brick.h.

The skeleton-free rig (M-RIG) packs three DISJOINT brushes into the one rest
volume: the body and the rest hand where the artist authored them, and the
grab hand translated into the empty negative-x half. Correctness of the
shader's `max(field, boxDist)` clip depends on two things that are easy to
break with an innocent re-export:

  * every brush lies inside the volume box, and
  * no two brushes come within the narrow band of each other.

This recomputes both from assets/fighter.glb and src/brick.h, so the numbers
in the comment on kGrabBrushOffset are checked rather than asserted.

    python3 tools/verify_brush_layout.py        # exits 1 if the layout breaks
"""

import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def brick_constants():
    """kExtent / kGrid / kBrickUsable / kBand and the derived origin."""
    src = (ROOT / "src" / "brick.h").read_text()

    def num(name):
        m = re.search(r"constexpr\s+(?:int|float)\s+%s\s*=\s*([-\d.f]+)" % name, src)
        if not m:
            sys.exit("could not find %s in src/brick.h" % name)
        return float(m.group(1).rstrip("f"))

    grid = int(num("kGrid"))
    usable = int(num("kBrickUsable"))
    extent = num("kExtent")
    band = num("kBand")
    span = extent / grid
    voxel = span / usable
    nudge = voxel * 0.5
    origin = (-extent / 2 + nudge, -0.1582, -extent / 2 + nudge)
    return dict(grid=grid, extent=extent, span=span, voxel=voxel, band=band,
                origin=origin)


def grab_offset():
    src = (ROOT / "src" / "asset.h").read_text()
    m = re.search(r"kGrabBrushOffset\[3\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        sys.exit("could not find kGrabBrushOffset in src/asset.h")
    return [float(x.strip().rstrip("f")) for x in m.group(1).split(",")]


def load_glb(path):
    data = path.read_bytes()
    if data[:4] != b"glTF":
        sys.exit("%s is not a binary glTF" % path)
    off, js, binoff = 12, None, None
    while off < len(data):
        ln, ty = struct.unpack_from("<II", data, off)
        if ty == 0x4E4F534A:
            js = json.loads(data[off + 8: off + 8 + ln])
        elif ty == 0x004E4942:
            binoff = off + 8
        off += 8 + ln
    return data, js, binoff


def accessor(data, js, binoff, idx):
    a = js["accessors"][idx]
    bv = js["bufferViews"][a["bufferView"]]
    start = binoff + bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    ctype = {5121: "B", 5123: "H", 5125: "I", 5126: "f"}[a["componentType"]]
    ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    size = struct.calcsize("<" + ctype)
    stride = bv.get("byteStride") or size * ncomp
    return [struct.unpack_from("<" + ctype * ncomp, data, start + k * stride)
            for k in range(a["count"])]


def mesh_by_node(js, name):
    for node in js["nodes"]:
        if node.get("name") == name:
            return js["meshes"][node["mesh"]]
    return None


def bounds(points):
    lo = [min(p[a] for p in points) for a in range(3)]
    hi = [max(p[a] for p in points) for a in range(3)]
    return lo, hi


def main():
    k = brick_constants()
    ofs = grab_offset()
    data, js, binoff = load_glb(ROOT / "assets" / "fighter.glb")

    body = mesh_by_node(js, "body")
    hand = mesh_by_node(js, "hand")
    if body is None or hand is None:
        sys.exit("asset is missing a node named 'body' or 'hand'")

    bpos = accessor(data, js, binoff, body["primitives"][0]["attributes"]["POSITION"])
    hprim = hand["primitives"][0]
    hpos = accessor(data, js, binoff, hprim["attributes"]["POSITION"])
    if not hprim.get("targets"):
        sys.exit("the hand primitive has no morph target — no grab pose to build")
    hdelta = accessor(data, js, binoff, hprim["targets"][0]["POSITION"])
    grab = [(hpos[i][0] + hdelta[i][0] + ofs[0],
             hpos[i][1] + hdelta[i][1] + ofs[1],
             hpos[i][2] + hdelta[i][2] + ofs[2]) for i in range(len(hpos))]

    brushes = [("body", bounds(bpos)), ("hand.rest", bounds(hpos)),
               ("hand.grab", bounds(grab))]

    vlo = k["origin"]
    vhi = [vlo[a] + k["extent"] for a in range(3)]
    band_m = k["band"] * k["voxel"]

    print("volume box   x %.4f..%.4f  y %.4f..%.4f  z %.4f..%.4f"
          % (vlo[0], vhi[0], vlo[1], vhi[1], vlo[2], vhi[2]))
    print("narrow band  %.4f m  (kBand %.0f voxels x %.6f m)"
          % (band_m, k["band"], k["voxel"]))
    print("grab offset  (%.4f, %.4f, %.4f)" % tuple(ofs))
    print()

    ok = True
    for name, (lo, hi) in brushes:
        print("%-10s x %8.4f..%8.4f  y %8.4f..%8.4f  z %8.4f..%8.4f"
              % (name, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
        for a in range(3):
            if lo[a] < vlo[a] or hi[a] > vhi[a]:
                print("  FAIL axis %d leaves the volume box" % a)
                ok = False
            clear = min(lo[a] - vlo[a], vhi[a] - hi[a])
            if clear < band_m:
                print("  WARN axis %d is only %.4f m from the wall (band %.4f)"
                      % (a, clear, band_m))
    print()

    # pairwise separation: the gap along the best separating axis
    for i in range(len(brushes)):
        for j in range(i + 1, len(brushes)):
            ni, (loi, hii) = brushes[i]
            nj, (loj, hij) = brushes[j]
            gap = max(max(loj[a] - hii[a], loi[a] - hij[a]) for a in range(3))
            status = "ok"
            if gap <= 0:
                status = "FAIL (overlap)"
                ok = False
            elif gap < band_m:
                status = "FAIL (closer than the narrow band)"
                ok = False
            print("%-10s <-> %-10s gap %8.4f m  %s" % (ni, nj, gap, status))

    print()
    print("LAYOUT OK" if ok else "LAYOUT BROKEN")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
