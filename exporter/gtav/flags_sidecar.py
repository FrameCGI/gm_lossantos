from __future__ import annotations

import json
import os
import re
from pathlib import Path

DEFAULT_GMOD = Path(r"C:\Program Files (x86)\Steam\steamapps\common\GarrysMod\garrysmod")


def material_names(index_json) -> list:
    m = index_json.get("materials") or {}
    if isinstance(m, list):
        return [str(x) for x in m]

    def slot_key(kv):
        k = str(kv[0])
        return (0, int(k)) if k.isdigit() else (1, k)

    return [str(v) for (_k, v) in sorted(m.items(), key=slot_key)]


def find_gmod_root(map_dir: Path):
    p = Path(map_dir).resolve()
    for cand in (p, *p.parents):
        if (cand / "addons").is_dir() or (cand / "materials").is_dir():
            return cand
        if cand.parent == cand:
            break
    return None


def build_vmt_index(gmod: Path) -> dict:
    roots = [gmod / "materials"]
    addons = gmod / "addons"
    if addons.is_dir():
        for d in sorted(os.listdir(addons)):
            base = addons / d / "materials"
            if base.is_dir():
                roots.append(base)
    from .paths import CONTENT_MATERIALS
    if CONTENT_MATERIALS.is_dir():
        roots.append(CONTENT_MATERIALS)

    index = {}
    for base in roots:
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in sorted(filenames):
                if not fn.lower().endswith(".vmt"):
                    continue
                real = os.path.join(dirpath, fn)
                rel = os.path.relpath(real, base).replace(os.sep, "/")
                vpath = ("materials/" + rel).lower()
                index.setdefault(vpath, real)
    return index


def read_vmt_text(name_lower: str, index: dict):
    candidates = [name_lower]
    if not name_lower.startswith("models/"):
        candidates.append("models/gtav/" + name_lower)
    for cand in candidates:
        real = index.get("materials/" + cand + ".vmt")
        if real is None:
            continue
        try:
            with open(real, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        if text:
            return text.lower()
    return None


def _vmt_flag_pat(key: str) -> re.Pattern:
    # same pattern as the lua side
    return re.compile(r'[$%\-]' + re.escape(key)
                      + r'[^A-Za-z0-9]["\s]*\s*"?([A-Za-z0-9+\.\-]*)')


def vmt_flag(text, key: str) -> bool:
    if not text:
        return False
    pat = _vmt_flag_pat(key)
    pos = 0
    while True:
        m = pat.search(text, pos)
        if not m:
            return False
        val = m.group(1)
        if val != "" and val != "0":
            return True
        pos = m.end()


def flags_for_name(name: str, index: dict) -> int:
    lower = name.lower()

    glass = ("glass" in lower) or ("window" in lower) or ("shop" in lower)
    nocull = (glass
              or "railing" in lower or "fence" in lower or "branch" in lower
              or "leaf" in lower or "hedge" in lower or "ivy" in lower)

    text = read_vmt_text(lower, index)
    if vmt_flag(text, "nocull"):
        nocull = True
    if vmt_flag(text, "translucent"):
        glass = True
    additive = bool(vmt_flag(text, "additive"))

    # 1  glass
    # 2  nocull
    # 4  additive
    return (1 if glass else 0) | (2 if nocull else 0) | (4 if additive else 0)


def generate(map_dir: Path, gmod: Path = None, check: bool = False,
             log=print) -> int:
    map_dir = Path(map_dir)
    idx_path = map_dir / "index.json"
    out_path = map_dir / "material_flags.json"
    if not idx_path.is_file():
        log(f"flags sidecar: no map index at {idx_path}")
        return 2

    index_json = json.loads(idx_path.read_text(encoding="utf-8"))
    materials = material_names(index_json)
    if not materials:
        log(f"flags sidecar: {idx_path} has no materials list")
        return 2

    gmod = Path(gmod) if gmod else find_gmod_root(map_dir)
    if not gmod:
        log(f"flags sidecar: no GarrysMod root above {map_dir} to scan for VMTs")
        return 2

    vmts = build_vmt_index(gmod)
    # no sidecar beats a wrong one
    if not vmts:
        log(f"flags sidecar: no VMTs under {gmod} - refusing to guess flags")
        return 2

    log(f"flags sidecar: {map_dir.name}: {len(materials)} material names, "
        f"{len(vmts)} VMTs indexed")

    flags = {}
    with_vmt = 0
    glass_n = nocull_n = additive_n = 0
    for name in materials:
        if name in flags:
            continue
        bits = flags_for_name(name, vmts)
        flags[name] = bits
        if read_vmt_text(name.lower(), vmts) is not None:
            with_vmt += 1
        if bits & 1:
            glass_n += 1
        if bits & 2:
            nocull_n += 1
        if bits & 4:
            additive_n += 1

    log(f"  resolved {len(flags)} names | VMT found {with_vmt} "
        f"missing {len(flags) - with_vmt}")
    log(f"  glass {glass_n} | nocull {nocull_n} | additive {additive_n}")

    doc = {"schema": 1, "flags": flags}
    text = json.dumps(doc, sort_keys=True, separators=(",", ":"))

    if check:
        if not out_path.is_file():
            log(f"CHECK FAIL: {out_path} does not exist")
            return 1
        old = out_path.read_text(encoding="utf-8")
        if old == text:
            log("CHECK OK: byte-identical")
            return 0
        try:
            old_flags = json.loads(old).get("flags", {})
        except Exception:
            old_flags = {}
        diffs = [k for k in set(old_flags) | set(flags)
                 if old_flags.get(k) != flags.get(k)]
        log(f"CHECK FAIL: {len(diffs)} differing names "
            f"(file has {len(old_flags)}, computed {len(flags)})")
        for k in sorted(diffs)[:20]:
            log(f"    {k}: file={old_flags.get(k)} computed={flags.get(k)}")
        return 1

    out_path.write_text(text, encoding="utf-8")
    log(f"  wrote {out_path.name} ({len(text)} bytes)")
    # runtime reads this twin first
    dat = ["GVMF1"] + [f"{k}\t{flags[k]}" for k in sorted(flags) if "\t" not in k and "\n" not in k]
    (map_dir / "material_flags.dat").write_text("\n".join(dat) + "\n", encoding="utf-8", newline="\n")
    log(f"  wrote material_flags.dat ({len(dat) - 1} names)")
    return 0


def write_sidecar(map_dir: Path, log=print) -> bool:
    try:
        return generate(map_dir, log=log) == 0
    except Exception as exc:
        log(f"flags sidecar: skipped ({exc})")
        return False
