"""Command line front end for the GTA V archive reader."""

import argparse
import os
import sys
from pathlib import Path

try:
    from . import crypto, rpf
except ImportError:  # run as a plain script
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import crypto
    import rpf

HERE = Path(__file__).resolve().parent
KIND_NAMES = {rpf.DIR: 'dir', rpf.BINARY: 'bin', rpf.RESOURCE: 'res'}

HD_LOD_LEVELS = (0, 5)  # HD, ORPHANHD
import re as _re
# lod stand-ins and night emissive shells
LOD_NAME_RE = _re.compile(r'_s?lod\d*(?:_|\.|$)|emissive|_emi_|_proxy\.ydr$')


def human(size):
    if size < 1024:
        return f'{size} B'
    value = float(size)
    for unit in ('KB', 'MB', 'GB'):
        value /= 1024
        if value < 1024:
            return f'{value:.1f} {unit}'
    return f'{value / 1024:.1f} TB'


def resolve_keys(args):
    candidates = [args.keys, os.environ.get('GTAV_KEYS'), HERE / 'keys']
    for candidate in candidates:
        if candidate and Path(candidate).is_dir():
            return Path(candidate)
    raise SystemExit(
        'no key material found')


def open_archive(args, cipher):
    archive = rpf.RpfArchive(args.archive, cipher)
    if getattr(args, 'rpf', None):
        entry = archive.get(args.rpf)
        if entry is None:
            archive.close()
            raise SystemExit(f'no such archive inside: {args.rpf}')
        child = archive.open_child(entry)
        archive.close()
        return child
    return archive


def cmd_tree(args, cipher):
    archive = open_archive(args, cipher)
    try:
        base_depth = archive.root.path.count('/')
        for entry in archive.walk():
            if not entry.is_dir:
                continue
            depth = entry.path.count('/') - base_depth
            if depth > args.depth:
                continue
            files = sum(1 for e in archive.children(entry) if not e.is_dir)
            folders = sum(1 for e in archive.children(entry) if e.is_dir)
            label = entry.name or archive.name
            print(f'{"  " * depth}{label}/  ({folders} dirs, {files} files)')
            if args.files:
                for child in archive.children(entry):
                    if child.is_dir:
                        continue
                    size = archive.file_size(child)
                    print(f'{"  " * (depth + 1)}{child.name}  '
                          f'{human(size)}  {KIND_NAMES[child.kind]}')
    finally:
        archive.close()


def cmd_list(args, cipher):
    archive = open_archive(args, cipher)
    try:
        entries = archive.walk()
        if args.filter:
            needle = args.filter.lower()
            entries = (e for e in entries if needle in e.path.lower())
        if args.kind:
            kind = {'bin': rpf.BINARY, 'res': rpf.RESOURCE,
                    'binary': rpf.BINARY, 'resource': rpf.RESOURCE,
                    'dir': rpf.DIR}.get(args.kind)
            if kind is None:
                raise SystemExit(f'unknown kind: {args.kind}')
            entries = (e for e in entries if e.kind == kind)
        shown = 0
        for entry in entries:
            if entry.is_dir:
                continue
            if args.limit and shown >= args.limit:
                break
            size = archive.file_size(entry)
            print(f'{human(size):>10}  {KIND_NAMES[entry.kind]}  {entry.path}')
            shown += 1
        print(f'-- {shown} file(s)')
    finally:
        archive.close()


def cmd_children(args, cipher):
    archive = open_archive(args, cipher)
    try:
        children = [e for e in archive.walk()
                    if e.kind == rpf.BINARY and e.name.lower().endswith('.rpf')]
        for entry in children:
            print(f'{human(archive.file_size(entry)):>10}  {entry.path}')
        print(f'-- {len(children)} archive(s)')
    finally:
        archive.close()


def cmd_extract(args, cipher):
    archive = open_archive(args, cipher)
    try:
        entry = archive.get(args.entry)
        if entry is None:
            raise SystemExit(f'no such entry: {args.entry}')
        if entry.is_dir:
            raise SystemExit(f'{entry.path} is a directory')
        data = archive.read_raw(entry) if args.raw else archive.read(entry)
        out = Path(args.out or '.')
        out.mkdir(parents=True, exist_ok=True)
        target = out / (entry.name + '.raw' if args.raw else entry.name)
        target.write_bytes(data)
        print(f'{entry.path} -> {target} ({human(len(data))}, '
              f'{"raw" if args.raw else "decoded"})')
    finally:
        archive.close()


def cmd_export(args, cipher):
    import numpy as np
    from . import rpf, world as gtav
    from .ybn import parse_ybn, ybn_bounds
    from .resource import ResourceError

    archive = open_archive(args, cipher)
    try:
        candidates = [e for e in archive.walk()
                      if e.kind == rpf.RESOURCE and e.name.lower().endswith('.ybn')]
        if args.list:
            print(f'{len(candidates)} .ybn file(s):')
            for e in candidates:
                print(f'  {human(archive.file_size(e)):>10}  {e.name}')
            return 0
        if not candidates:
            raise SystemExit('no .ybn files in this archive')

        selected = candidates
        if args.ybn:
            wanted = [w.lower() for w in args.ybn]
            exact = [e for e in candidates
                     if e.name.lower() in wanted
                     or e.name.lower()[:-4] in wanted]
            if exact:
                selected = exact
            else:
                selected = [e for e in candidates
                            if any(w in e.name.lower() for w in wanted)]
        elif not args.include_hi:
            # hi@ stacks on top of the base
            selected = [e for e in candidates if not e.name.lower().startswith('hi@')]

        if not selected:
            raise SystemExit('no matching .ybn; use --list to see the names')

        print(f'exporting {len(selected)} .ybn file(s) from {archive.path.name}:')
        parsed = {}

        def parse(entry):
            if entry.path in parsed:
                return parsed[entry.path]
            data = archive.read(entry)
            system_size = rpf.size_from_flags(entry.system_flags)
            try:
                result = parse_ybn(data, system_size, entry.name)
            except ResourceError:  # boxes/capsules only
                print(f'  ! {entry.name}: no collision triangles; skipping')
                result = None
            parsed[entry.path] = result
            if result is not None:
                print(f'  {entry.name}: {result.triangle_count} triangles, '
                      f'{result.stats.blocks} bounds blocks '
                      f'({result.stats.bvhs} bvh, {result.stats.composites} composite)')
                print(f'    GTA bounds {np.round(result.bounds_min, 1).tolist()} .. '
                      f'{np.round(result.bounds_max, 1).tolist()} m')
            return result

        parts = []
        block_min = np.array([np.inf, np.inf, np.inf])
        block_max = np.array([-np.inf, -np.inf, -np.inf])
        for entry in selected:
            base = parse(entry)
            if base is None:
                continue
            block_min = np.minimum(block_min, base.bounds_min)
            block_max = np.maximum(block_max, base.bounds_max)

        by_name = {e.name.lower(): e for e in candidates}
        for entry in selected:
            hi_entry = by_name.get('hi@' + entry.name.lower())
            if args.hi and hi_entry is not None:
                print(f'    --hi: using {hi_entry.name} instead of {entry.name}')
                base = parse(hi_entry)
                if base is not None:
                    parts.append(base)
                continue
            base = parse(entry)
            if args.merge_hi and hi_entry is not None:
                hi = parse(hi_entry)
                if base is None:
                    base, hi = hi, None
                if hi is not None:
                    print(f'    --merge-hi: {hi_entry.name} inside its bounds, '
                          f'{entry.name} outside')
                    base = gtav.merge_hi(base, hi)
            if base is not None:
                parts.append(base)

        if args.roads:
            outer = rpf.RpfArchive(args.archive, cipher)
            try:
                road_entry = outer.get(args.roads)
                if road_entry is None or not road_entry.name.lower().endswith('.rpf'):
                    raise SystemExit(f'--roads: no such .rpf in {args.archive}: {args.roads}')
                roads = outer.open_child(road_entry)
            finally:
                outer.close()
            try:
                road_all = [e for e in roads.walk()
                            if e.kind == rpf.RESOURCE
                            and e.name.lower().endswith('.ybn')]
                road_files = [e for e in road_all
                              if not e.name.lower().startswith(('hi@', 'ma@'))]
                print(f'  roads: {len(road_files)} .ybn file(s) from {road_entry.name}, '
                      f'clipped to the block +-{args.clip_margin:g} m')
                for entry in road_files:
                    data = roads.read(entry)
                    size = rpf.size_from_flags(entry.system_flags)
                    lo, hi = ybn_bounds(data, size)
                    pad = args.clip_margin
                    if (hi[0] < block_min[0] - pad or lo[0] > block_max[0] + pad
                            or hi[1] < block_min[1] - pad or lo[1] > block_max[1] + pad):
                        continue
                    parsed_road = parse_ybn(data, size, entry.name)
                    clipped = gtav.clip_world(parsed_road, block_min, block_max,
                                            args.clip_margin)
                    if clipped is None:
                        continue
                    print(f'    {entry.name}: {clipped.triangle_count} of '
                          f'{parsed_road.triangle_count} triangles in range')
                    parts.append(clipped)
            finally:
                roads.close()

        if not parts:
            raise SystemExit('no collision triangles in any .ybn of this archive')
        combined = parts[0] if len(parts) == 1 else _combine(parts)

        visual = None
        extra_archives = []
        if args.ydr:
            import posixpath

            from . import materials as matmod, ymap as ymapmod, ytd as ytdmod
            from .ydr import parse_ydr, place_drawable

            if not args.materials:
                raise SystemExit('--materials <addon materials folder> is required '
                                 'with --ydr (the drawable textures must be installed)')
            drawables = []
            extra_archives = []
            if args.ydr == ['all']:
                block_path = args.rpf
                if not block_path:
                    raise SystemExit('--ydr all needs --rpf <block.rpf>')
                block_dir = posixpath.dirname(block_path)
                stem = Path(block_path).stem

                outer = rpf.RpfArchive(args.archive, cipher)
                try:
                    candidates = [e for e in outer.walk()
                                  if e.kind == rpf.BINARY
                                  and e.name.lower().endswith('.rpf')
                                  and (block_dir + '/') in e.path
                                  and 'metadata' in e.name.lower()]
                    if not candidates:
                        raise SystemExit(f'no metadata .rpf next to {block_path}')
                    # a dir can hold several metadata rpfs
                    want = posixpath.basename(block_dir) + '_metadata.rpf'
                    exact = [e for e in candidates if e.name.lower() == want]
                    meta_entry = exact[0] if exact else candidates[0]
                    meta_arch = outer.open_child(meta_entry)
                    gta_dir = Path(args.archive).parent

                    def open_inner(spec, label):
                        if ':' in spec:
                            archive_name, inner = spec.split(':', 1)
                            other = rpf.RpfArchive(gta_dir / archive_name, cipher)
                            try:
                                entry = other.get(inner)
                                if entry is None:
                                    raise SystemExit(f'{label}: no such file: {inner}')
                                return other.open_child(entry)
                            finally:
                                other.close()
                        entry = outer.get(spec)
                        if entry is None:
                            raise SystemExit(f'{label}: no such file: {spec}')
                        return outer.open_child(entry)

                    for extra in args.ydr_rpf:
                        extra_archives.append(open_inner(extra, '--ydr-rpf'))
                    map_archives = []
                    for extra in args.map_rpf:
                        map_archives.append(open_inner(extra, '--map-rpf'))
                finally:
                    outer.close()
                try:
                    archetypes = []
                    for entry in meta_arch.walk():
                        if entry.name.lower().endswith('.ytyp'):
                            archetypes += ymapmod.parse_ytyp(
                                meta_arch.read(entry),
                                rpf.size_from_flags(entry.system_flags), entry.name)
                    for src in extra_archives + map_archives:
                        for entry in src.walk():
                            if entry.name.lower().endswith('.ytyp'):
                                archetypes += ymapmod.parse_ytyp(
                                    src.read(entry),
                                    rpf.size_from_flags(entry.system_flags), entry.name)
                    entities = []
                    for entry in meta_arch.walk():
                        if entry.name.lower().endswith('.ymap'):
                            entities += ymapmod.parse_ymap(
                                meta_arch.read(entry),
                                rpf.size_from_flags(entry.system_flags), entry.name)
                finally:
                    meta_arch.close()
                from collections import defaultdict
                by_arch = defaultdict(list)
                for entity in entities:
                    by_arch[entity.archetype].append(entity)
                print(f'  ymap: {len(entities)} entities, '
                      f'{len(archetypes)} archetypes from {meta_entry.name}')

                margin = max(args.clip_margin, 30.0) + 20.0
                lo = block_min - margin
                hi = block_max + margin
                center = (block_min + block_max) * 0.5
                seen = set()
                lod_skipped = 0
                for src in [archive] + extra_archives + map_archives:
                    for entry in src.walk():
                        if not entry.name.lower().endswith('.ydr'):
                            continue
                        drawable = parse_ydr(src.read(entry),
                                             rpf.size_from_flags(entry.system_flags),
                                             entry.name)
                        archetype = ymapmod.match_archetype(drawable, archetypes)
                        if archetype is None:
                            continue
                        placements = [e for e in by_arch.get(archetype.name, [])
                                      if np.all(e.position >= lo)
                                      and np.all(e.position <= hi)]
                        if not args.keep_lod_entities:
                            # stand-ins would sit on top of the detail
                            kept = [e for e in placements
                                    if e.lod_level in HD_LOD_LEVELS]
                            lod_skipped += len(placements) - len(kept)
                            placements = kept
                            if placements and LOD_NAME_RE.search(entry.name.lower()):
                                lod_skipped += len(placements)
                                placements = []
                        if src is not archive and src not in map_archives:
                            placements = [e for e in placements
                                          if np.linalg.norm(e.position - center)
                                          <= args.prop_margin]
                            placements.sort(key=lambda e: float(
                                np.linalg.norm(e.position - center)))
                            placements = placements[:args.prop_limit]
                        for entity in placements:
                            key = (entry.name, tuple(np.round(entity.position, 2)))
                            if key in seen:
                                continue
                            seen.add(key)
                            placed = ymapmod.transform_drawable(drawable, entity)
                            drawables.append(placed)
                            print(f'  ydr {entry.name}: {drawable.triangle_count} tris '
                                  f'at {tuple(np.round(entity.position, 1))}')
                if lod_skipped:
                    print(f'  LODSKIP={lod_skipped} LOD/SLOD placements not exported '
                          f'(HD/ORPHANHD only; --keep-lod-entities to include)')

                import math
                # drawable dictionaries cant match by bbox
                for spec in args.place:
                    if '@' not in spec:
                        raise SystemExit(f'--place expects YDR@X,Y,Z[,YAW]: {spec}')
                    ydr_name, coords = spec.split('@', 1)
                    values = [float(v) for v in coords.split(',')]
                    if len(values) not in (3, 4):
                        raise SystemExit(f'--place expects 3 or 4 numbers: {spec}')
                    position = np.array(values[:3])
                    yaw = math.radians(values[3]) if len(values) == 4 else 0.0
                    owner = entry = None
                    for src in [archive] + extra_archives + map_archives:
                        candidate = src.get(ydr_name)
                        if candidate is not None:
                            owner, entry = src, candidate
                            break
                    if entry is None:
                        raise SystemExit(f'--place: no such .ydr: {ydr_name}')
                    drawable = parse_ydr(owner.read(entry),
                                         rpf.size_from_flags(entry.system_flags),
                                         entry.name)
                    entity = ymapmod.YmapEntity(
                        archetype=0, position=position,
                        rotation=(0.0, 0.0, math.sin(yaw * 0.5),
                                  math.cos(yaw * 0.5)),
                        scale_xy=1.0, scale_z=1.0, flags=0, lod_level=0)
                    drawables.append(ymapmod.transform_drawable(drawable, entity))
                    print(f'  placed {ydr_name}: {drawable.triangle_count} tris '
                          f'at {tuple(position)} yaw {values[3] if len(values) == 4 else 0:g}')
            else:
                for name in args.ydr:
                    entry = archive.get(name)
                    if entry is None:
                        raise SystemExit(f'--ydr: no such file: {name}')
                    drawable = parse_ydr(archive.read(entry),
                                         rpf.size_from_flags(entry.system_flags), name)
                    place_drawable(drawable, block_min, block_max, log=print)
                    print(f'  ydr {name}: {drawable.triangle_count} triangles, '
                          f'{len(drawable.geometries)} geometries, '
                          f'{len(drawable.textures)} embedded textures')
                    drawables.append(drawable)
            if not drawables:
                print('  ! --ydr: no drawables placed; exporting collision only')

            # biggest copy of each texture wins
            available = {}
            atlas_keys = set()
            for drawable in drawables:
                for tex in drawable.textures:
                    key = tex.name.lower()
                    old = available.get(key)
                    if old is None or tex.width * tex.height > old.width * old.height:
                        available[key] = tex
            for src in [archive] + extra_archives + map_archives:
                for tentry in src.walk():
                    if tentry.kind != rpf.RESOURCE or not tentry.name.lower().endswith('.ytd'):
                        continue
                    try:
                        textures = ytdmod.parse_ytd(
                            src.read(tentry),
                            rpf.size_from_flags(tentry.system_flags), tentry.name)
                    except Exception:
                        continue
                    for tex in textures:
                        key = tex.name.lower()
                        old = available.get(key)
                        if old is None or tex.width * tex.height > old.width * old.height:
                            available[key] = tex

            for spec in args.tex_ytd:
                if ':' not in spec:
                    raise SystemExit(f'--tex-ytd expects '
                                     f'"x64g.rpf:inner/path.ytd": {spec}')
                archive_name, inner = spec.split(':', 1)
                other = rpf.RpfArchive(gta_dir / archive_name, cipher)
                opened = []
                try:
                    owner = other
                    path = inner
                    textures = None
                    label = inner
                    while True:  # walk down nested rpfs
                        entry = owner.get(path)
                        if entry is not None and entry.name.lower().endswith('.ytd'):
                            textures = ytdmod.parse_ytd(
                                owner.read(entry),
                                rpf.size_from_flags(entry.system_flags),
                                entry.name)
                            label = entry.name
                            break
                        cut = path.lower().rfind('.rpf/')
                        if cut < 0:
                            raise SystemExit(f'--tex-ytd: no such .ytd: {inner}')
                        parent = owner.get(path[:cut + 4])
                        if parent is None:
                            raise SystemExit(f'--tex-ytd: no such .rpf: '
                                             f'{path[:cut + 4]}')
                        child = owner.open_child(parent)
                        opened.append(child)
                        owner, path = child, path[cut + 5:]
                    for tex in textures:
                        key = tex.name.lower()
                        atlas_keys.add(key)
                        old = available.get(key)
                        if old is None or tex.width * tex.height > old.width * old.height:
                            available[key] = tex
                    print(f'  ytd {label}: {len(textures)} textures')
                finally:
                    for child in reversed(opened):
                        child.close()
                    other.close()

            for spec in args.texture_ydr:
                owner = entry = None
                for src in [archive] + extra_archives + map_archives:
                    candidate = src.get(spec)
                    if candidate is not None:
                        owner, entry = src, candidate
                        break
                if entry is None:
                    raise SystemExit(f'--texture-ydr: no such .ydr: {spec}')
                source_drawable = parse_ydr(
                    owner.read(entry), rpf.size_from_flags(entry.system_flags),
                    entry.name)
                for tex in source_drawable.textures:
                    key = tex.name.lower()
                    old = available.get(key)
                    if old is None or tex.width * tex.height > old.width * old.height:
                        available[key] = tex
                print(f'  embedded {entry.name}: '
                      f'{len(source_drawable.textures)} textures')

            used = {(geom.texture or '').lower()
                    for drawable in drawables for geom in drawable.geometries}
            fallbacks = {}
            aliases = {}
            fallback_count = 0
            for key in sorted(used):
                if key in available:
                    continue
                # atlas names fold the shader suffix
                near = _near_texture(key, available)
                if near is not None and near.endswith('_lod') and not key.endswith('_lod'):
                    exact = matmod.find_installed(args.materials, key)
                    if exact:
                        fallbacks[key] = exact
                        print(f'  = no {key} in this archive; borrowed {exact} (over {near})')
                        continue
                if near is not None:
                    aliases[key] = near
                    print(f'  = no {key} in this archive; using {near}')
                    continue
                borrowed = matmod.find_installed(args.materials, key)
                if borrowed:
                    fallbacks[key] = borrowed
                    print(f'  = no {key} in this archive; borrowed {borrowed}')
                    continue
                # guess from the name, this sucks but it works
                if 'window' in key:
                    fallbacks[key] = 'models/gtav/bh1_06/bh1_06_rsn_os_window_0001'
                elif 'shop' in key:
                    fallbacks[key] = 'models/gtav/ap1_01_a/im_concrete17'
                elif 'glass' in key:
                    fallbacks[key] = 'models/gtav/bh1_06/bh1_06_rsn_os_window_0001'
                elif matmod.is_marking_name(key):
                    fallbacks[key] = False  # drop the geometry
                elif matmod.is_overlay_name(key):
                    fallbacks[key] = False
                elif 'drain' in key:
                    fallbacks[key] = 'models/gtav/dt1_02/im_drains02'
                elif any(k in key for k in ('kerb', 'sidewalk', 'pavement',
                                            'pave', 'floortile')):
                    fallbacks[key] = 'models/gtav/dt1_02/im_sidewalk010'
                elif any(k in key for k in ('weed', 'litter', 'splat', 'stain',
                                            'cable', 'wire')):
                    fallbacks[key] = False
                elif any(k in key for k in ('grass', 'hedge', 'leaflitter')):
                    fallbacks[key] = 'models/gtav/bh1_06/im_grass11b_fur'
                elif any(k in key for k in ('dirt', 'gravel', 'sand', 'mud',
                                            'stone', 'rock', 'erode', 'cliff')):
                    fallbacks[key] = 'models/gtav/bh1_21/am_glue_dirtbrown'
                elif 'metal' in key or 'plate' in key:
                    fallbacks[key] = 'models/gtav/bh1_06/bh1_06_rsn_mc_metal_0001'
                elif any(k in key for k in ('road', 'crossing', 'tarmac',
                                            'blend', 'overlay', 'ground')):
                    fallbacks[key] = 'models/gtav/dt1_02/im_road_012'
                else:
                    fallbacks[key] = 'models/gtav/ap1_01_a/im_concrete17'
                fallback_count += 1
                if fallbacks[key] is False:
                    print(f'  - dropped {key} geometry (no texture)')
                else:
                    print(f'  ! no {key} in this archive; using {fallbacks[key]}')
            if fallback_count:
                print(f'  FALLBACK={fallback_count} textures substituted or dropped '
                      f'(not found in this archive or installed elsewhere)')

            block_name = (Path(args.rpf).stem if args.rpf
                          else Path(args.ydr[0]).stem.split('_')[0])
            prefix = args.ydr_prefix or f'models/gtav/{block_name}'
            items = [(d.source, available[k]) for d in drawables
                     for k in {(g.texture or '').lower() for g in d.geometries}
                     if k in available]
            for near in set(aliases.values()):
                items.append(('alias', available[near]))
            for palette_path, _metres in gtav.resolve_texture_palette(
                    args.textures or 'dt1_02') or []:
                if not palette_path.startswith(prefix + '/'):
                    continue
                key = palette_path.rsplit('/', 1)[-1].lower()
                if key in available:
                    items.append(('palette', available[key]))
            installed, skipped = matmod.install_textures(
                [it for it in items if it[1].name.lower() not in atlas_keys],
                args.materials, prefix, log=print)
            for name, why in skipped:
                print(f'  ! skipped {name}: {why}')
            # shared atlases go in one folder
            shared_items = [it for it in items
                            if it[1].name.lower() in atlas_keys]
            if shared_items:
                shared, skipped = matmod.install_textures(
                    shared_items, args.materials, 'models/gtav/roads',
                    log=print)
                for name, why in skipped:
                    print(f'  ! skipped {name}: {why}')
                installed.update(shared)
            texture_materials = {name.lower(): path
                                 for name, path in installed.items()}
            texture_materials.update(fallbacks)
            for key, near in aliases.items():
                path = texture_materials.get(near)
                if path:
                    texture_materials[key] = path
            # im_road_012 is a damage overlay, use clean tarmac
            if args.textures == 'bh1_17' and 'im_road_027b' in texture_materials:
                texture_materials['im_road_012'] = texture_materials['im_road_027b']
            for extra in extra_archives + map_archives:
                extra.close()
            visual = gtav.build_ydr_visual(
                combined, drawables, texture_materials,
                palette_prefix=args.textures or 'dt1_02',
                road_pack=gtav.is_road_pack(args.rpf or ''))

        out_root = args.out
        if out_root is None:
            out_root = os.environ.get('GTAV_DATA', 'lossantos')
        if args.clear:
            gtav.clear_output(out_root, args.name)
        report = gtav.export(combined, out_root, args.name, scale=args.scale,
                           texture_prefix=args.textures, visual=visual,
                           tile_size=args.tile_size,
                           double_sided=(visual is None))
        print()
        if report.duplicates_removed:
            print(f'dupes     {report.duplicates_removed} removed')
        print(f'tiles     {report.tiles} ({report.tile_triangles} triangles)')
        print(f'chunks    {report.chunks} ({report.chunk_triangles} triangles)')
        print(f'BUDGET_DROPPED={report.dropped_tile_tris} '
              f'CHUNK_DROPPED={report.dropped_chunk_tris}')
        print(f'spawn     {[round(v, 1) for v in report.spawn]} (absolute Source units)')
        print(f'gta spawn {[round(v / args.scale, 1) for v in report.spawn]} m')
        print(f'output    {Path(out_root).resolve() / args.name}')
        return 0
    finally:
        archive.close()


def _combine(parts):
    from . import world as gtav
    return gtav.combine(parts)


_AUX_MAP_RE = _re.compile(r'(_n|_s|_nm|_spec|_normal|normal_only)$|flatnormal', _re.I)


def _is_aux_map(name):
    return bool(_AUX_MAP_RE.search((name or '').lower()))


def _near_texture(key, available):
    for suffix in ('_a', '_d', '_n', '_s', '_b'):
        if key.endswith(suffix) and key[:-len(suffix)] in available:
            return key[:-len(suffix)]
    if len(key) < 6:
        return None

    def _suffix(name):
        n = name.lower()
        for s in ('_a', '_d', '_n', '_s', '_b'):
            if n.endswith(s):
                return n[:-len(s)]
        return n

    def _folded(a, b):
        return a.startswith(b) or a.endswith(b)

    k = _suffix(key)
    key_is_map = _is_aux_map(key)
    best = None
    for cand in available:
        # never paint a normal/spec map as colour
        if not key_is_map and _is_aux_map(cand):
            continue
        d = _suffix(cand)
        if len(d) >= 6 and len(k) >= 6 and (d == k or _folded(k, d)
                                           or _folded(d, k)):
            # shortest wins, ties on name
            if best is None or (len(d), d) < (len(_suffix(best)), _suffix(best)):
                best = cand
    return best


def cmd_textures(args, cipher):
    from . import materials, ytd as ytdmod

    archive = open_archive(args, cipher)
    try:
        entries = [e for e in archive.walk()
                   if e.kind == rpf.RESOURCE and e.name.lower().endswith('.ytd')]
        if args.ytd:
            wanted = [w.lower() for w in args.ytd]
            entries = [e for e in entries if e.name.lower() in wanted]
        if not entries:
            raise SystemExit('no matching .ytd in this archive')

        wanted_textures = {w.lower() for w in args.texture} if args.texture else None
        if args.preset:
            if args.preset == 'dt1_02':
                wanted_textures = {n.lower() for n in materials.DT1_02_PALETTE}
            elif args.preset == 'bh1_06':
                wanted_textures = {n.lower() for n in materials.BH1_06_PALETTE}
            else:
                raise SystemExit(f'unknown preset: {args.preset}')
        prefix = args.prefix or 'models/gtav/dt1_02'
        items = []
        for entry in entries:
            textures = ytdmod.parse_ytd(
                archive.read(entry), rpf.size_from_flags(entry.system_flags),
                entry.name)
            for tex in textures:
                if wanted_textures and tex.name.lower() not in wanted_textures:
                    continue
                items.append((entry.name, tex))

        if args.list:
            for ytd_name, tex in items:
                print(f'{ytd_name:40s} {tex.name:44s} '
                      f'{tex.width}x{tex.height} {tex.format_name}')
            print(f'-- {len(items)} texture(s)')
            return 0

        if not items:
            raise SystemExit('no matching textures (use --list to see the names)')
        if not args.out:
            raise SystemExit('--out <materials folder> is required to install')

        installed, skipped = materials.install_textures(
            items, args.out, prefix, png_dir=args.png)
        print()
        print(f'installed {len(installed)} texture(s) under '
              f'{Path(args.out).resolve() / prefix}')
        for name, why in skipped:
            print(f'  skipped {name}: {why}')
        print('material names (for --textures):')
        for name in sorted(installed.values()):
            print(f'  {name}')
        return 0
    finally:
        archive.close()


def cmd_info(args, cipher):
    archive = open_archive(args, cipher)
    try:
        files = sum(1 for e in archive.walk() if not e.is_dir)
        dirs = sum(1 for e in archive.walk() if e.is_dir)
        print(f'archive   {archive.path}')
        print(f'inner     {archive.name}')
        print(f'start     0x{archive.start_pos:x}')
        print(f'size      {human(archive.size)} ({archive.size} bytes)')
        print(f'cipher    0x{archive.encryption:08x}')
        print(f'entries   {len(archive.entries)} ({dirs} dirs, {files} files)')
    finally:
        archive.close()


def cmd_verify(args, cipher):
    archive = open_archive(args, cipher)
    try:
        total = bad = 0
        worst = []
        for entry in archive.walk():
            if entry.kind != rpf.RESOURCE:
                continue
            total += 1
            try:
                data = archive.read(entry)
            except Exception as exc:
                bad += 1
                worst.append((entry.path, str(exc)))
                continue
            want = (rpf.size_from_flags(entry.system_flags)
                    + rpf.size_from_flags(entry.graphics_flags))
            if len(data) != want:
                bad += 1
                worst.append((entry.path, f'{len(data)} bytes, expected {want}'))
        for path, why in worst[:20]:
            print(f'BAD  {path}: {why}')
        print(f'-- {total - bad}/{total} resources match their page sizes')
        return 1 if bad else 0
    finally:
        archive.close()


def build_parser():
    parser = argparse.ArgumentParser(prog='gtav-rpf', description=__doc__.split('\n')[0])
    parser.add_argument('--keys', help='folder with the gtav_*.dat key files')
    sub = parser.add_subparsers(dest='command', required=True)

    def add(name, func, help_text):
        cmd = sub.add_parser(name, help=help_text)
        cmd.add_argument('archive')
        cmd.add_argument('--rpf', help='descend into this nested .rpf entry first')
        cmd.set_defaults(func=func)
        return cmd

    info = add('info', cmd_info, 'header summary')

    tree = add('tree', cmd_tree, 'directory tree')
    tree.add_argument('--depth', type=int, default=3)
    tree.add_argument('--files', action='store_true', help='list files too')

    listing = add('list', cmd_list, 'list files')
    listing.add_argument('--filter', help='substring of the path')
    listing.add_argument('--kind', help='dir, bin/binary or res/resource')
    listing.add_argument('--limit', type=int, default=0)

    add('children', cmd_children, 'nested .rpf archives')
    add('verify', cmd_verify, 'check every resource page size')

    extract = add('extract', cmd_extract, 'write one file out')
    extract.add_argument('entry')
    extract.add_argument('--out', help='output folder (default: .)')
    extract.add_argument('--raw', action='store_true',
                         help='stored bytes, RSC7 header included')

    world = add('export', cmd_export, 'convert .ybn collision to GVT1/GVC1')
    world.add_argument('--ybn', action='append', default=[],
                       help='only this .ybn (name or substring); repeatable')
    world.add_argument('--include-hi', action='store_true',
                       help='also use hi@*.ybn (overlaps the base collision)')
    world.add_argument('--merge-hi', action='store_true',
                       help='use hi@*.ybn inside its bounds, base collision outside')
    world.add_argument('--hi', action='store_true',
                       help='use hi@*.ybn instead of the base when present')
    world.add_argument('--roads',
                       help='add road collision from this .rpf (path inside the main archive)')
    world.add_argument('--clip-margin', type=float, default=30.0,
                       help='metres of road collision to keep around the block (default 30)')
    world.add_argument('--textures', metavar='PREFIX',
                       help='use VMTs under this material prefix (e.g. models/gtav/dt1_02)')
    world.add_argument('--ydr', action='append', default=[],
                       help='drawable .ydr (or "all" to place every ydr via the block ymap)')
    world.add_argument('--ydr-rpf', action='append', default=[],
                       help='extra .rpf whose drawables should also be placed '
                          '(prefix with "x64i.rpf:" for another archive)')
    world.add_argument('--map-rpf', action='append', default=[],
                       help='extra map-mesh .rpf (e.g. the roads rpf); its '
                          'drawables are placed anywhere inside the clip box, '
                          'with no prop limit')
    world.add_argument('--tex-ytd', action='append', default=[],
                       help='extra texture dictionary, as '
                          '"x64g.rpf:levels/.../gtxd.rpf/name+hi.ytd"')
    world.add_argument('--texture-ydr', action='append', default=[],
                       metavar='YDR',
                       help='read embedded textures from this map drawable '
                          'without placing it (e.g. a parking-lot section)')
    world.add_argument('--place', action='append', default=[],
                       metavar='YDR@X,Y,Z[,YAW]',
                       help='force-place a map drawable by name at GTA metres, '
                          'e.g. bh1_rd2_road_sect.ydr@-547.5,-350.2,34.1')
    world.add_argument('--materials',
                       help='addon materials folder to install the .ydr textures into')
    world.add_argument('--ydr-prefix',
                       help='VMT prefix for the .ydr textures (default models/gtav/<block>)')
    world.add_argument('--prop-margin', type=float, default=40.0,
                       help='metres around the block centre for props from --ydr-rpf (default 40)')
    world.add_argument('--prop-limit', type=int, default=8,
                       help='max placements per drawable from --ydr-rpf (default 8)')
    world.add_argument('--keep-lod-entities', action='store_true',
                       help='also place LOD/SLOD stand-in entities (default: HD/ORPHANHD only)')
    world.add_argument('--tile-size', type=int, default=4096,
                       help='visual tile size in Source units (default 4096)')
    world.add_argument('--list', action='store_true',
                       help='list the .ybn files and exit')
    world.add_argument('--name', default='lossantos',
                       help='lossantos data folder / BSP name')
    world.add_argument('--out', help='data root (default: $GTAV_DATA or '
                                     './lossantos)')
    world.add_argument('--scale', type=float, default=39.3700787,
                       help='GTA metres -> Source units (default 39.3700787)')
    world.add_argument('--clear', action='store_true',
                       help='delete the previous .bin files first')

    tex = add('textures', cmd_textures, 'install .ytd textures as VTF/VMT')
    tex.add_argument('--ytd', action='append', default=[],
                     help='only this .ytd (repeatable)')
    tex.add_argument('--texture', action='append', default=[],
                     help='only this texture name (repeatable)')
    tex.add_argument('--preset', choices=['dt1_02', 'bh1_06'],
                     help='install the palette used by a known block')
    tex.add_argument('--list', action='store_true',
                     help='list matching textures and exit')
    tex.add_argument('--out', help='materials folder to write into')
    tex.add_argument('--prefix', default='models/gtav/dt1_02',
                     help='material path inside the folder (default models/gtav/dt1_02)')
    tex.add_argument('--png', help='also write decoded preview PNGs here')

    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    cipher = crypto.NgCipher.load(resolve_keys(args))
    return args.func(args, cipher) or 0


if __name__ == '__main__':
    raise SystemExit(main())
