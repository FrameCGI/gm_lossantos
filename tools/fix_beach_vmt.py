"""Patch beach/ground decal VMTs in the installed materials tree."""
import sys as _sys
from pathlib import Path as _P
_sys.path.insert(0, str(_P(__file__).resolve().parents[1]))
from exporter.gtav import paths as _paths
import argparse
import sys
from pathlib import Path

DEFAULT_ROOT = str(_paths.GTA_MATERIALS)  # exporter/gtav/paths.py

FOAM_WORDS = ("beachfoam",)
FOAM_EXCLUDE = ("insulation",)

DECAL_WORDS = ("waterstain", "beach_decal", "beachdecal", "beachraking",
               "beach_raking", "sandmoss", "waterdamage", "im_beach_decals",
               "beachrake")

POLYGON_OFFSET = "2"


def is_foam(name):
    low = name.lower()
    return (any(k in low for k in FOAM_WORDS)
            and not any(k in low for k in FOAM_EXCLUDE))


def is_decal(name):
    low = name.lower()
    return any(k in low for k in DECAL_WORDS)


def patch_foam(text):
    lines = text.splitlines()
    out = [ln for ln in lines
           if ln.strip() not in ('"$alphatest" "1"', '"$translucent" "1"')
           and not ln.strip().startswith('"$alpha"')
           and ln.strip() != '"$additive" "1"']
    for i in range(len(out) - 1, -1, -1):
        if out[i].strip() == "}":
            out.insert(i, '    "$additive" "1"')
            break
    return "\n".join(out) + "\n"


def patch_decal(text):
    if '"$polygonoffset"' in text:
        return text
    lines = text.splitlines()
    for i in range(len(lines) - 1, -1, -1):
        if lines[i].strip() == "}":
            lines.insert(i, f'    "$polygonoffset" "{POLYGON_OFFSET}"')
            break
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    root = Path(a.root)
    n_foam = n_decal = n_skip = 0
    for vmt in sorted(root.rglob("*.vmt")):
        name = vmt.stem
        foam = is_foam(name)
        decal = is_decal(name)
        if not foam and not decal:
            continue
        text = vmt.read_text(encoding="utf-8")
        if foam:
            if '"$additive"' in text:
                n_skip += 1
                continue
            new = patch_foam(text)
            print(f"  foam  -> $additive  {vmt.relative_to(root)}")
            n_foam += 1
        else:
            if '"$polygonoffset"' in text:
                n_skip += 1
                continue
            new = patch_decal(text)
            print(f"  decal -> $polygonoffset {POLYGON_OFFSET}  "
                  f"{vmt.relative_to(root)}")
            n_decal += 1
        if not a.dry_run:
            vmt.write_text(new, encoding="utf-8", newline="\n")
    print(f"DONE foam {n_foam}, decal {n_decal}, already-ok {n_skip}"
          + (" (dry-run)" if a.dry_run else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
