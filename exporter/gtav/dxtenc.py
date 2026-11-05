from __future__ import annotations

import numpy as np

# same fourccs as ytd.py
FMT_DXT1 = 0x31545844
FMT_DXT5 = 0x35545844

LUM = np.array([0.299, 0.587, 0.114], np.float32)


def to_blocks(img):
    h, w = img.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        img = np.pad(img, ((0, ph), (0, pw), (0, 0)), mode='edge')
    return (img.reshape((h + ph) // 4, 4, (w + pw) // 4, 4, -1)
               .transpose(0, 2, 1, 3, 4).reshape(-1, 16, img.shape[2]))


def pack565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.uint32)
    return ((c[:, 0] >> 3) << 11) | ((c[:, 1] >> 2) << 5) | (c[:, 2] >> 3)


def unpack565(v):
    r = ((v >> 11) & 31).astype(np.float32) * 255.0 / 31.0
    g = ((v >> 5) & 63).astype(np.float32) * 255.0 / 63.0
    b = (v & 31).astype(np.float32) * 255.0 / 31.0
    return np.stack([r, g, b], axis=1)


def encode_color(px):
    x = px.astype(np.float32)
    lum = x @ LUM
    i0 = np.argmin(lum, axis=1)
    i1 = np.argmax(lum, axis=1)
    n = x.shape[0]

    def gather(idx):
        return x[np.arange(n), idx]

    c0, c1 = gather(i0), gather(i1)

    for _ in range(2):  # fit, refit once
        d = c1 - c0
        den = (d * d).sum(axis=1) + 1e-6
        t = ((x - c0[:, None, :]) * d[:, None, :]).sum(axis=2) / den[:, None]
        idx = np.clip(np.rint(t * 3.0), 0, 3).astype(np.int64)
        w0 = np.take(np.array([1.0, 0.0, 2 / 3, 1 / 3], np.float32), idx)
        w1 = 1.0 - w0
        # least squares endpoint refit
        a = (w0 * w0).sum(axis=1) + 1e-6
        b = (w0 * w1).sum(axis=1)
        c = (w1 * w1).sum(axis=1) + 1e-6
        r0 = (w0[:, :, None] * x).sum(axis=1)
        r1 = (w1[:, :, None] * x).sum(axis=1)
        det = a * c - b * b
        l0 = (c[:, None] * r0 - b[:, None] * r1) / det[:, None]
        l1 = (a[:, None] * r1 - b[:, None] * r0) / det[:, None]
        ok = np.isfinite(l0).all(axis=1) & np.isfinite(l1).all(axis=1)
        c0 = np.where(ok[:, None], l0, c0)
        c1 = np.where(ok[:, None], l1, c1)

    v0, v1 = pack565(c0), pack565(c1)
    swap = v0 < v1  # 4-colour mode needs c0 > c1
    v0, v1 = np.where(swap, v1, v0), np.where(swap, v0, v1)
    idx = np.where(swap[:, None],
                   np.take(np.array([1, 0, 3, 2], np.int64), idx), idx)

    p0 = unpack565(v0)
    p1 = unpack565(v1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], axis=1)
    dist = ((x[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(axis=3)
    idx = dist.argmin(axis=2)

    packed = np.zeros(n, np.uint32)
    for i in range(16):
        packed |= idx[:, i].astype(np.uint32) << (2 * i)
    out = np.empty((n, 8), np.uint8)
    out[:, 0:2] = v0.astype('<u2').view(np.uint8).reshape(n, 2)
    out[:, 2:4] = v1.astype('<u2').view(np.uint8).reshape(n, 2)
    out[:, 4:8] = packed.view(np.uint8).reshape(n, 4)
    return out


def encode_alpha(a):
    x = a.astype(np.float32)
    n = x.shape[0]
    # idx 0 = a0, 1 = a1, 2-7 interpolated
    w1 = np.empty(8, np.float32)
    w1[0] = 0.0
    w1[1] = 1.0
    w1[2:8] = np.arange(1, 7, dtype=np.float32) / 7.0
    w0 = 1.0 - w1

    a0 = x.max(axis=1)
    a1 = x.min(axis=1)
    for _ in range(3):
        pal = a0[:, None] * w0[None, :] + a1[:, None] * w1[None, :]
        idx = np.abs(x[:, :, None] - pal[:, None, :]).argmin(axis=2)
        ww0 = w0[idx]
        ww1 = w1[idx]
        aa = (ww0 * ww0).sum(axis=1) + 1e-6
        bb = (ww0 * ww1).sum(axis=1)
        cc = (ww1 * ww1).sum(axis=1) + 1e-6
        r0 = (ww0 * x).sum(axis=1)
        r1 = (ww1 * x).sum(axis=1)
        det = aa * cc - bb * bb
        ok = np.abs(det) > 1e-6
        safe = np.where(ok, det, 1.0)
        l0 = np.where(ok, (cc * r0 - bb * r1) / safe, a0)
        l1 = np.where(ok, (aa * r1 - bb * r0) / safe, a1)
        a0 = np.clip(l0, 0, 255)
        a1 = np.clip(l1, 0, 255)

    a0v = np.clip(np.rint(a0), 0, 255).astype(np.uint8)
    a1v = np.clip(np.rint(a1), 0, 255).astype(np.uint8)
    swap = a0v < a1v  # 8-interp mode needs a0 >= a1
    a0v, a1v = np.where(swap, a1v, a0v), np.where(swap, a0v, a1v)
    p0 = a0v.astype(np.float32)
    p1 = a1v.astype(np.float32)
    pal = p0[:, None] * w0[None, :] + p1[:, None] * w1[None, :]
    dist = np.abs(x[:, :, None] - pal[:, None, :])
    idx = dist.argmin(axis=2)

    bits = np.zeros(n, np.uint64)
    for i in range(16):
        bits |= idx[:, i].astype(np.uint64) << (3 * i)
    out = np.empty((n, 8), np.uint8)
    out[:, 0] = a0v
    out[:, 1] = a1v
    out[:, 2:8] = bits.view(np.uint8).reshape(n, 8)[:, :6]
    return out


def encode_image(rgba, fmt=None):
    rgba = np.asarray(rgba, np.uint8)
    a = rgba[:, :, 3]
    if fmt is None:
        fmt = FMT_DXT1 if (a == 255).all() else FMT_DXT5
    if fmt == FMT_DXT1:
        blk = encode_color(to_blocks(rgba[:, :, :3]))
        return blk.tobytes(), fmt
    rgb = rgba[:, :, :3].copy()
    # opaque mean under the holes, stops colour bleed
    opaque = a == 255
    if opaque.any():
        fill = rgb[opaque].mean(axis=0)
    else:
        fill = np.full(3, 128, np.float32)
    rgb[~opaque] = fill.astype(np.uint8)
    col = encode_color(to_blocks(rgb))
    alp = encode_alpha(to_blocks(a[:, :, None])[:, :, 0])
    out = np.concatenate([alp, col], axis=1)  # BC3 = alpha then colour
    return out.tobytes(), fmt


def encode_levels(levels):
    if not levels:
        return None
    a0 = np.asarray(levels[0])[:, :, 3]
    fmt = FMT_DXT1 if (a0 == 255).all() else FMT_DXT5
    blobs = []
    for level in levels:
        blob, _ = encode_image(level, fmt)
        blobs.append(blob)
    return blobs, fmt
