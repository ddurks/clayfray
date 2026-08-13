#!/usr/bin/env python3
"""Toleranced screenshot diff for replay/lookdev gating.

Replays are ledger-exact and sim-deterministic, but the GPU redistance pass
heals brick aprons in dispatch order, which wiggles a handful of pixels by a
few LSBs between runs. Byte-compare is therefore the wrong gate; this is the
honest one: identical dimensions, few differing pixels, small max delta.

usage: tools/imgdiff.py a.png b.png [--max-delta 16] [--max-frac 0.001]
exit: 0 within tolerance, 1 outside, 2 usage/IO
"""
import argparse
import sys

try:
    from PIL import Image
except ImportError:
    print("imgdiff: needs Pillow (pip install pillow)", file=sys.stderr)
    sys.exit(2)

p = argparse.ArgumentParser()
p.add_argument("a")
p.add_argument("b")
p.add_argument("--max-delta", type=int, default=16,
               help="max per-channel difference (default 16)")
p.add_argument("--max-frac", type=float, default=0.001,
               help="max fraction of differing pixels (default 0.1%%)")
args = p.parse_args()

ia, ib = Image.open(args.a), Image.open(args.b)
if ia.size != ib.size:
    print(f"imgdiff: size mismatch {ia.size} vs {ib.size}")
    sys.exit(1)

pa, pb = ia.convert("RGBA").tobytes(), ib.convert("RGBA").tobytes()
total = ia.size[0] * ia.size[1]
diff_px = 0
max_d = 0
for i in range(0, len(pa), 4):
    d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]),
            abs(pa[i + 2] - pb[i + 2]))
    if d:
        diff_px += 1
        if d > max_d:
            max_d = d

frac = diff_px / total
ok = max_d <= args.max_delta and frac <= args.max_frac
print(f"imgdiff: {diff_px}/{total} px differ ({frac:.5%}), max delta {max_d} "
      f"-> {'OK' if ok else 'FAIL'} (limits: delta<={args.max_delta}, "
      f"frac<={args.max_frac:.3%})")
sys.exit(0 if ok else 1)
