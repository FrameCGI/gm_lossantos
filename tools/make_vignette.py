from __future__ import annotations

import argparse
import os
import sys

SIZE = 512
VIGNETTE_ALPHA = 0.55
INNER = 0.35  # no darkening inside this
OUTER = 1.05  # full darkening here

DEFAULT_ROOT = (r"C:\Program Files (x86)\Steam\steamapps\common\GarrysMod"
                r"\garrysmod\addons\lossantos\materials")


def smoothstep(t: float) -> float:
    if t <= 0.0:
        return 0.0
    if t >= 1.0:
        return 1.0
    return t * t * (3.0 - 2.0 * t)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_ROOT, help="materials root")
    ap.add_argument("--size", type=int, default=SIZE)
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("PIL is required", file=sys.stderr)
        return 1

    n = args.size
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    px = im.load()
    half = (n - 1) * 0.5
    maxr = (2.0 * half * half) ** 0.5  # corner distance
    for y in range(n):
        for x in range(n):
            dx = x - half
            dy = y - half
            r = (dx * dx + dy * dy) ** 0.5 / maxr
            t = (r - INNER) / (OUTER - INNER)
            a = int(smoothstep(t) * VIGNETTE_ALPHA * 255.0 + 0.5)
            px[x, y] = (0, 0, 0, a)

    out_dir = os.path.join(args.out, "gtav")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "vignette.png")
    im.save(out)
    print(f"wrote {out} ({n}x{n}, alpha {VIGNETTE_ALPHA})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
