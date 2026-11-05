from __future__ import annotations

import hashlib
import os
import re

import numpy as np

from .ytd import write_vtf, decode_rgba
from .vtf_shrink import shrink_file

_SAFE = re.compile(r'[^a-z0-9_]+')


def clean_name(name, max_len=None):
    stem = _SAFE.sub('_', name.lower()).strip('_') or 'texture'
    if max_len and len(stem) > max_len:
        digest = hashlib.sha1(stem.encode('ascii', 'replace')).hexdigest()[:8]
        stem = stem[:max_len - 9].rstrip('_') + '_' + digest
    return stem

DT1_02_PALETTE = [
    'im_road_012',
    'im_sidewalk011',
    'im_dt1_02_windows_01',
    'im_grass07b',
    'im_dt1_02_wall_02',
    'im_dt1_02_wall_01',
    'im_dt1_02_ground_01',
    'im_roof_underside003',
]

BH1_06_PALETTE = [
    'bh1_06_rsn_os_marblewall_0003',
    'bh1_06_rsn_os_window_0001',
    'bh1_06_rsn_mc_metal_0001',
    'im_grass11b_fur',
]


def vmt_text(texture_path, surfaceprop='concrete', alpha=False, translucent=False,
             opacity=0.35, nocull=False, selfillum=False, additive=False):
    lines = [
        # not VertexLitGeneric, it drops $vertexcolor
        '"UnlitGeneric"',
        '{',
        '    "$model" "1"',
        '    "$vertexcolor" "1"',
        f'    "$basetexture" "{texture_path}"',
        f'    "$surfaceprop" "{surfaceprop}"',
    ]
    # blended only, vertex alpha kills depth writes
    if translucent or additive:
        lines.append('    "$vertexalpha" "1"')
    if translucent:
        lines += [
            '    "$translucent" "1"',
            f'    "$alpha" "{opacity}"',
        ]
    elif alpha:
        lines.append('    "$alphatest" "1"')
    # dead under UnlitGeneric
    if selfillum:
        lines.append('    "$selfillum" "1"')
    if additive:
        lines.append('    "$additive" "1"')
    if nocull:
        lines.append('    "$nocull" "1"')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def _mean_filter(array, radius):
    k = 2 * radius + 1
    if array.ndim == 2:
        padded = np.pad(array, radius, mode='edge')
        windows = np.lib.stride_tricks.sliding_window_view(padded, (k, k))
        return windows.mean(axis=(-1, -2))
    padded = np.pad(array, ((radius, radius), (radius, radius), (0, 0)),
                    mode='edge')
    windows = np.lib.stride_tricks.sliding_window_view(
        padded, (k, k), axis=(0, 1))
    return windows.mean(axis=(-1, -2))


def _median_filter(array, radius):
    k = 2 * radius + 1
    padded = np.pad(array, ((radius, radius), (radius, radius), (0, 0)),
                    mode='edge')
    windows = np.lib.stride_tricks.sliding_window_view(
        padded, (k, k), axis=(0, 1))
    return np.median(windows, axis=(-1, -2))


def detail_rgba(rgba):
    rgb = rgba[:, :, :3].astype(np.float32)
    alpha = rgba[:, :, 3].astype(np.float32)
    mask = _mean_filter(alpha, 1) > 90
    opaque = mask & (alpha > 80)
    if opaque.any():
        base = rgb[opaque].mean(axis=0)
    else:
        base = rgb.reshape(-1, 3).mean(axis=0)
    luminance = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
    smooth = _mean_filter(luminance, 2)
    reference = smooth[opaque].mean() if opaque.any() else smooth.mean()
    scale = np.clip(0.65 + 0.55 * (smooth / max(reference, 1e-6)), 0.55, 1.35)
    result = np.empty_like(rgba)
    result[:, :, :3] = np.clip(scale[:, :, None] * base[None, None, :], 0, 255).astype(np.uint8)
    result[:, :, 3] = np.where(mask, 255, 0).astype(np.uint8)
    return result


def foliage_rgba(rgba):
    rgb = _mean_filter(rgba[:, :, :3].astype(np.float32), 3)
    alpha = _mean_filter(rgba[:, :, 3].astype(np.float32), 4)
    mask = alpha > 145
    luminance = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
    green = np.clip(luminance * 1.1 + 18.0, 0, 255)
    result = np.empty_like(rgba)
    result[:, :, 0] = np.clip(green * 0.55, 0, 255)
    result[:, :, 1] = np.clip(green * 1.05, 0, 255)
    result[:, :, 2] = np.clip(green * 0.42, 0, 255)
    result[:, :, 3] = np.where(mask, 255, 0).astype(np.uint8)
    return result


def markings_rgba(rgba):
    alpha = _mean_filter(rgba[:, :, 3].astype(np.float32), 2)
    rgb = _mean_filter(rgba[:, :, :3].astype(np.float32), 2)
    mask = alpha > 110
    if mask.any():
        base = rgb[mask].mean(axis=0)
    else:
        base = np.full(3, 220.0)
    if base.mean() < 90:
        base = np.full(3, 220.0)
    result = np.empty_like(rgba)
    result[:, :, :3] = np.clip(base[None, None, :], 0, 255).astype(np.uint8)
    result[:, :, 3] = np.where(mask, 255, 0).astype(np.uint8)
    return result


def overlay_rgba(rgba, radius=3):
    rgb = _mean_filter(rgba[:, :, :3].astype(np.float32), radius)
    alpha = _mean_filter(rgba[:, :, 3].astype(np.float32), radius)
    result = np.empty_like(rgba)
    result[:, :, :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    result[:, :, 3] = np.clip(alpha, 0, 255).astype(np.uint8)
    return result


def _inpaint_rgb(rgb, valid, radius=2):
    w = valid.astype(np.float32)
    num = _mean_filter(rgb * w[:, :, None], radius)
    den = _mean_filter(w, radius)[:, :, None]
    filled = num / np.maximum(den, 1e-6)
    empty = den[:, :, 0] <= 1e-6
    if empty.any():
        gmean = (rgb[valid].mean(axis=0) if valid.any()
                 else rgb.reshape(-1, 3).mean(axis=0))
        filled[empty] = gmean
    out = rgb.copy()
    out[~valid] = filled[~valid]
    return out


def cutout_rgba(rgba, thresh=128, smooth=1):
    alpha_src = rgba[:, :, 3]
    alpha = _mean_filter(alpha_src.astype(np.float32), smooth)
    mask = alpha > thresh
    valid = alpha_src > 0
    rgb = _inpaint_rgb(rgba[:, :, :3].astype(np.float32), valid)
    result = np.empty_like(rgba)
    result[:, :, :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    result[:, :, 3] = np.where(mask, 255, 0).astype(np.uint8)
    return result


def alpha_binary(rgba):
    a = rgba[:, :, 3]
    return float(((a == 0) | (a == 255)).mean())


def soft_alpha(rgba):
    if alpha_binary(rgba) >= 0.35:
        return False
    cov = float((rgba[:, :, 3] > 127).mean())
    return 0.005 < cov < 0.995


STIPPLE_WORDS = ('railing', 'fence', 'mesh', 'net', 'grill', 'grid', 'trellis',
                 'chainlink', 'wicker', 'shutter', 'gate', 'doorgate',
                 'canopy')


def is_stipple_name(lower):
    return (any(k in lower for k in STIPPLE_WORDS)
            or ('canopy' in lower and lower.endswith('_a')))


GLOW_WORDS = ('glow', 'neon')


def is_glow_name(lower):
    return any(k in lower for k in GLOW_WORDS)


def soft_fade_alpha(rgba, min_partial=0.08):
    a = rgba[:, :, 3]
    partial = float(((a > 8) & (a < 248)).mean())
    return partial > min_partial

MASK_AHF_MIN = 11.0  # mean neighbour alpha diff
MASK_BINARY_MAX = 0.85  # above this its a clean cutout

NATURE_WORDS = ('grass', 'weed', 'ivy', 'hedge', 'plam', 'palm', 'branch',
                'leaf', 'leav', 'fern', 'flower', 'flw', 'plant', 'tree',
                'bush', 'vine', 'scrub', 'jungle', 'frond', 'cactus', 'moss')

NATURE_EXCLUDE = ('dirt', 'ground', 'surround', 'gravel', 'rock', 'sand',
                  'mud', 'road', 'asphalt', 'tarmac', 'paint', 'marking',
                  'litter', 'decal', 'concrete', 'stone', 'brick', 'bark',
                  'trunk', 'log')

MARKING_WORDS = ('roadmark', 'road_mark', 'marking', 'genallmark', 'linemark')

OVERLAY_WORDS = (
    'overlay', 'ovly',
    'splat',  # dc_parking_splats, birdsplats
    'blend',
    'glue',  # am_glue_dirtbrown / am_glue_grassdirt
    'road_damage', 'roadblend', 'ground_dirt', 'dirt_overlay',
    'stain',  # oilstain, waterstains, road_stain_overlay
    'crack',  # concrete/ground cracks, alleycrack
    'skid', 'tyremark', 'tyreskid',
    'litter',  # im_litter01, leaflitter
)
OVERLAY_EXCLUDE = ('stainless',)  # has "stain" in it


def is_marking_name(lower):
    return any(k in lower for k in MARKING_WORDS)


def is_overlay_name(lower):
    return (any(k in lower for k in OVERLAY_WORDS)
            and not any(k in lower for k in OVERLAY_EXCLUDE))


def find_installed(material_root, name, budgets=range(30, 41)):
    stem = _SAFE.sub('_', (name or '').lower()).strip('_')
    if not stem:
        return None
    candidates = [clean_name(name, max_len=b) for b in budgets]
    if stem not in candidates:
        candidates.append(stem)
    root = os.path.join(material_root, 'models', 'gtav')
    try:
        regions = sorted(os.listdir(root))
    except OSError:
        return None
    for region in regions:
        for cand in candidates:
            if os.path.isfile(os.path.join(root, region, cand + '.vtf')):
                return f'models/gtav/{region}/{cand}'
    return None


def is_dither_mask(rgba):
    a = rgba[:, :, 3].astype(np.float32)
    ahf = 0.5 * (np.abs(np.diff(a, axis=0)).mean()
                 + np.abs(np.diff(a, axis=1)).mean())
    if ahf <= MASK_AHF_MIN:
        return False
    binary = float(((a == 0) | (a == 255)).mean())
    return binary < MASK_BINARY_MAX


MASKED_FACADES = ('bld01_rsn_ql_window', 'bld01_rsn_ql_shop')


def facade_alpha(rgba, pane_alpha=100):
    rgb = rgba[:, :, :3].astype(np.int16)
    red, green, blue = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
    luminance = 0.299 * red + 0.587 * green + 0.114 * blue
    panes = (luminance < 85) & (blue >= red - 12)
    return np.where(panes, np.uint8(pane_alpha), np.uint8(255))


def install_textures(items, material_root, prefix, log=print, png_dir=None):
    target = os.path.join(material_root, *prefix.split('/'))
    os.makedirs(target, exist_ok=True)

    # 63 chars fit in the tile name field
    budget = min(40, 63 - len(prefix) - 1)
    if budget < 12:
        raise ValueError(f'material prefix is too long for GVT1: {prefix}')

    best = {}
    for ytd, tex in items:
        prev = best.get(tex.name)
        if prev is None or tex.width * tex.height > prev[1].width * prev[1].height:
            best[tex.name] = (ytd, tex)
    items = [best[name] for name in sorted(best)]

    used = set()
    installed = {}
    skipped = []

    for ytd, tex in items:
        stem = clean_name(tex.name, max_len=budget)
        lower = tex.name.lower()

        marking = is_marking_name(lower)
        overlay = is_overlay_name(lower)
        stipple_name = is_stipple_name(lower)

        base = stem
        n = 2
        while stem in used:
            sfx = f'_{n}'
            stem = f'{base[:budget - len(sfx)]}{sfx}'
            n += 1

        vtf = os.path.join(target, stem + '.vtf')
        lower = tex.name.lower()
        glass = ('glass' in lower
                 and not any(k in lower for k in ('shop', 'brand', 'interior')))
        facade = any(k in lower for k in MASKED_FACADES)
        foliage = 'branches' in lower or 'leaf' in lower
        # not stipple_name, dont touch
        stipple = ('railing' in lower or 'fence' in lower
                   or ('canopy' in lower and lower.endswith('_a')))
        alpha_map = None
        rgba_map = None
        rgba = None
        soft = False
        src_opaque = False
        if marking or overlay or stipple_name:
            rgba = decode_rgba(tex)
            if rgba is None:
                skipped.append((tex.name, f'unsupported {tex.format_name} '
                                          f'{tex.width}x{tex.height}'))
                continue
            src_opaque = float((rgba[:, :, 3] > 250).mean()) > 0.995
            if src_opaque:
                # opaque, keep it as is
                pass
            elif marking:
                rgba_map = cutout_rgba(rgba, thresh=110, smooth=2)
            elif is_stipple_name(lower):
                rgba_map = cutout_rgba(rgba, thresh=128)
            elif is_overlay_name(lower):
                if soft_alpha(rgba):
                    soft = True
                else:
                    rgba_map = cutout_rgba(rgba, thresh=128)
        elif facade:
            rgba = decode_rgba(tex)
            if rgba is None:
                skipped.append((tex.name, f'unsupported {tex.format_name} '
                                          f'{tex.width}x{tex.height}'))
                continue
            alpha_map = facade_alpha(rgba)
        elif not glass:
            rgba = decode_rgba(tex)
            if rgba is not None:
                src_opaque = float((rgba[:, :, 3] > 250).mean()) > 0.995
                if is_dither_mask(rgba):
                    rgba_map = cutout_rgba(rgba)
                elif soft_alpha(rgba):
                    soft = True
        alpha_max = rgba_map is not None or alpha_map is not None
        # per process tmp, parallel exports share this tree
        vtf_tmp = f'{vtf}.{os.getpid()}.tmp'
        if not write_vtf(vtf_tmp, tex, alpha_mul=0.35 if glass else None,
                         alpha_map=alpha_map, rgba_map=rgba_map,
                         alpha_max=alpha_max):
            if os.path.exists(vtf_tmp):
                os.remove(vtf_tmp)
            skipped.append((tex.name, f'unsupported {tex.format_name} '
                                      f'{tex.width}x{tex.height}'))
            continue
        shrink_file(vtf_tmp)
        os.replace(vtf_tmp, vtf)
        used.add(stem)

        if png_dir:
            rgba = rgba_map if rgba_map is not None else decode_rgba(tex)
            if rgba is not None:
                if alpha_map is not None:
                    rgba = rgba.copy()
                    rgba[:, :, 3] = alpha_map
                try:
                    from PIL import Image
                except ImportError:
                    pass
                else:
                    os.makedirs(png_dir, exist_ok=True)
                    Image.fromarray(rgba, 'RGBA').save(
                        os.path.join(png_dir, stem + '.png'))

        surface = 'glass' if (glass or facade) else 'concrete'
        if foliage:
            surface = 'grass'
        elif stipple:
            surface = 'metal'
        elif not glass:
            if any(k in lower for k in ('road', 'tarmac', 'asphalt')):
                surface = 'asphalt'
            elif 'sidewalk' in lower or 'pavement' in lower:
                surface = 'concrete'
            elif any(k in lower for k in ('dirt', 'mud', 'gravel', 'sand')):
                surface = 'dirt'
            elif 'grass' in lower or 'hedge' in lower or 'ivy' in lower:
                surface = 'grass'
            elif 'metal' in lower:
                surface = 'metal'

        glow = is_glow_name(lower)
        if glow:  # additive neon
            alpha, translucent, opacity = False, False, 1.0
            selfillum, additive = True, True
        elif soft:  # blend layer
            alpha, translucent, opacity = False, True, 1.0
            selfillum, additive = False, False
        elif glass or facade:
            alpha, translucent, opacity = False, True, (1.0 if facade
                                                        else 0.35)
            selfillum, additive = False, False
        elif rgba_map is not None or alpha_map is not None:  # rebuilt cutout
            alpha, translucent, opacity = True, False, 1.0
            selfillum, additive = False, False
        elif src_opaque:  # opaque
            alpha, translucent, opacity = False, False, 1.0
            selfillum, additive = False, False
        elif rgba is not None and soft_fade_alpha(rgba):  # soft fade, alphatest eats it
            alpha, translucent, opacity = False, True, 1.0
            selfillum, additive = False, False
        else:  # hard cutout
            alpha, translucent, opacity = True, False, 1.0
            selfillum, additive = False, False
        vmt = os.path.join(target, stem + '.vmt')
        vmt_tmp = f'{vmt}.{os.getpid()}.tmp'
        with open(vmt_tmp, 'w', encoding='utf-8', newline='\n') as f:
            f.write(vmt_text(f'{prefix}/{stem}', surfaceprop=surface,
                             alpha=alpha, translucent=translucent,
                             opacity=opacity,
                             nocull=foliage or stipple or glass or facade,
                             selfillum=selfillum, additive=additive))
        os.replace(vmt_tmp, vmt)
        installed[tex.name] = f'{prefix}/{stem}'
        log(f'  {ytd}/{tex.name} -> {stem}.vtf ({tex.width}x{tex.height} '
            f'{tex.format_name})')

    return installed, skipped