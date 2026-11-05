from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

from .resource import ResourceReader, split_payload

# textureFormat (legacy D3D)
FMT_A8R8G8B8 = 21
FMT_X8R8G8B8 = 22
FMT_A1R5G5B5 = 25
FMT_A8 = 28
FMT_A8B8G8R8 = 32
FMT_L8 = 50
FMT_DXT1 = 0x31545844
FMT_DXT3 = 0x33545844
FMT_DXT5 = 0x35545844
FMT_ATI1 = 0x31495441
FMT_ATI2 = 0x32495441
FMT_BC7 = 0x20374342

FORMAT_NAMES = {
    FMT_A8R8G8B8: 'A8R8G8B8',
    FMT_X8R8G8B8: 'X8R8G8B8',
    FMT_A1R5G5B5: 'A1R5G5B5',
    FMT_A8: 'A8',
    FMT_A8B8G8R8: 'A8B8G8R8',
    FMT_L8: 'L8',
    FMT_DXT1: 'DXT1',
    FMT_DXT3: 'DXT3',
    FMT_DXT5: 'DXT5',
    FMT_ATI1: 'BC4',
    FMT_ATI2: 'BC5',
    FMT_BC7: 'BC7',
}

# VTF ImageFormat
VTF_RGBA8888 = 0
VTF_BGRA8888 = 12
VTF_DXT1 = 13
VTF_DXT3 = 14
VTF_DXT5 = 15
VTF_A8 = 8
VTF_I8 = 5

# texture flags
VTF_ONEBITALPHA = 0x1000
VTF_EIGHTBITALPHA = 0x2000

# bytes per 4x4 block
_BLOCK_BYTES = {
    FMT_DXT1: 8, FMT_ATI1: 8,
    FMT_DXT3: 16, FMT_DXT5: 16, FMT_ATI2: 16, FMT_BC7: 16,
}
# bytes per texel
_TEXEL_BYTES = {
    FMT_A8R8G8B8: 4, FMT_X8R8G8B8: 4, FMT_A8B8G8R8: 4,
    FMT_A1R5G5B5: 2, FMT_A8: 1, FMT_L8: 1,
}


def _level_sizes(fmt, width, height, stride, levels):
    sizes = []
    w, h = max(1, width), max(1, height)
    block = _BLOCK_BYTES.get(fmt)
    for _ in range(max(1, levels)):
        if block:
            # tail mips still cost a full block
            sizes.append(max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block)
        else:
            row = max(1, (stride * w) // width) if width else max(1, stride)
            sizes.append(row * max(1, h))
        w = max(1, w // 2)
        h = max(1, h // 2)
    return sizes


@dataclass
class GtaTexture:
    name: str
    width: int
    height: int
    depth: int
    stride: int
    format: int
    levels: int
    data: bytes

    @property
    def format_name(self):
        return FORMAT_NAMES.get(self.format, f'0x{self.format:08X}')

    def level_sizes(self):
        return _level_sizes(self.format, self.width, self.height,
                            self.stride, self.levels)

    def mip_chunks(self):
        chunks = []
        offset = 0
        for length in self.level_sizes():
            chunk = self.data[offset:offset + length]
            if len(chunk) < length:
                break
            chunks.append(chunk)
            offset += length
        return chunks


def parse_ytd(data, system_size, source=''):
    system, graphics = split_payload(data, system_size)
    return parse_dictionary_at(ResourceReader(system, graphics), 0x50000000)


def parse_dictionary_at(r, addr):
    tex_ptr = r.u64(addr + 0x30)
    tex_count = r.u16(addr + 0x38)
    if not tex_ptr or not tex_count:
        return []
    pointers = r.u64s(tex_ptr, tex_count)
    textures = []
    for pointer in pointers:
        if not pointer:
            continue
        t = _read_texture(r, int(pointer))
        if t is not None:
            textures.append(t)
    return textures


def _read_texture(r, addr):
    # Texture block, 144 bytes on Legacy
    name_ptr = r.u64(addr + 0x28)
    width = r.u16(addr + 0x50)
    height = r.u16(addr + 0x52)
    depth = r.u16(addr + 0x54) or 1
    stride = r.u16(addr + 0x56)  # row pitch, block pitch for DXT
    fmt = r.u32(addr + 0x58)
    levels = r.u8(addr + 0x5D)
    data_ptr = r.u64(addr + 0x70)
    if not width or not height or not data_ptr or not stride:
        return None
    payload = b''
    used = 0
    # as many full mips as the window holds
    for length in _level_sizes(fmt, width, height, stride, levels):
        try:
            chunk = r.bytes_at(data_ptr + len(payload), length)
        except Exception:
            break
        payload += chunk
        used += 1
    if not payload:
        return None
    return GtaTexture(
        name=r.string_at(name_ptr) or f'texture_{addr:08x}',
        width=width, height=height, depth=depth, stride=stride,
        format=fmt, levels=used, data=payload,
    )


def _vtf_format(tex):
    if tex.format == FMT_DXT1:
        return VTF_DXT1, VTF_ONEBITALPHA
    if tex.format == FMT_DXT3:
        return VTF_DXT3, VTF_EIGHTBITALPHA
    if tex.format == FMT_DXT5:
        return VTF_DXT5, VTF_EIGHTBITALPHA
    if tex.format == FMT_A8R8G8B8:
        return VTF_BGRA8888, 0
    if tex.format == FMT_X8R8G8B8:
        return VTF_BGRA8888, 0
    if tex.format == FMT_A8B8G8R8:
        return VTF_RGBA8888, 0
    if tex.format == FMT_L8:
        return VTF_I8, 0
    if tex.format == FMT_A8:
        return VTF_A8, 0
    return None, 0


def _rgb565(value):
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)],
                    np.uint8)


def _dxt_color_blocks(chunk, four_color, bw, bh):
    n = bw * bh
    data = np.frombuffer(chunk, np.uint8, n * 8).reshape(n, 8)
    c0 = data[:, 0].astype(np.uint16) | (data[:, 1].astype(np.uint16) << 8)
    c1 = data[:, 2].astype(np.uint16) | (data[:, 3].astype(np.uint16) << 8)
    indices = (data[:, 4].astype(np.uint32)
               | (data[:, 5].astype(np.uint32) << 8)
               | (data[:, 6].astype(np.uint32) << 16)
               | (data[:, 7].astype(np.uint32) << 24))

    palette = np.zeros((n, 4, 3), np.float32)
    for i in range(n):
        e0 = _rgb565(int(c0[i])).astype(np.float32)
        e1 = _rgb565(int(c1[i])).astype(np.float32)
        palette[i, 0] = e0
        palette[i, 1] = e1
        if four_color or c0[i] > c1[i]:
            palette[i, 2] = (2 * e0 + e1) / 3
            palette[i, 3] = (e0 + 2 * e1) / 3
        else:
            palette[i, 2] = (e0 + e1) / 2
            palette[i, 3] = 0
    idx = (indices[:, None] >> (2 * np.arange(16))[None, :]) & 3
    rgb = palette[np.arange(n)[:, None], idx]
    rgba = np.ones((n, 16, 4), np.float32) * 255
    rgba[:, :, :3] = rgb
    # DXT1 3 colour mode, index 3 is transparent
    if not four_color:
        alpha = np.where(idx == 3, 0.0, 255.0)
        rgba[:, :, 3] = np.where((c0 > c1)[:, None], 255.0, alpha)
    return rgba


def _dxt5_alpha(chunk):
    data = np.frombuffer(chunk, np.uint8)
    n = len(data) // 8
    data = data[:n * 8].reshape(n, 8)
    out = np.zeros((n, 16), np.float32)
    for i in range(n):
        a0, a1 = int(data[i, 0]), int(data[i, 1])
        bits = int.from_bytes(data[i, 2:8].tobytes(), 'little')
        lut = [a0, a1]
        if a0 > a1:
            lut += [((7 - k) * a0 + k * a1) / 7 for k in range(1, 7)]
        else:
            lut += [((5 - k) * a0 + k * a1) / 5 for k in range(1, 5)]
            lut += [0, 255]
        for p in range(16):
            out[i, p] = lut[(bits >> (3 * p)) & 7]
    return out


def decode_rgba(tex, level=0):
    level = max(0, min(level, tex.levels - 1))
    chunks = tex.mip_chunks()
    if level >= len(chunks):
        return None
    chunk = chunks[level]
    width = max(1, tex.width >> level)
    height = max(1, tex.height >> level)
    fmt = tex.format

    if fmt in (FMT_DXT1, FMT_DXT3, FMT_DXT5):
        bw = max(1, (width + 3) // 4)
        bh = max(1, (height + 3) // 4)
        if fmt == FMT_DXT1:
            rgba = _dxt_color_blocks(chunk, False, bw, bh)
        elif fmt == FMT_DXT3:
            # 8 bytes alpha nibbles, 8 bytes colour
            blocks = np.frombuffer(chunk, np.uint8, bw * bh * 16).reshape(-1, 16)
            alpha = blocks[:, :8]
            rgba = _dxt_color_blocks(blocks[:, 8:].tobytes(), True, bw, bh)
            a = np.zeros((bw * bh, 16), np.float32)
            for i in range(bw * bh):
                for byte in range(8):
                    lo = alpha[i, byte] & 0xF
                    hi = alpha[i, byte] >> 4
                    a[i, byte * 2] = lo * 17
                    a[i, byte * 2 + 1] = hi * 17
            rgba[:, :, 3] = a
        else:
            # 8 bytes alpha, 8 bytes colour
            blocks = np.frombuffer(chunk, np.uint8, bw * bh * 16).reshape(-1, 16)
            alpha = _dxt5_alpha(blocks[:, :8].tobytes())
            rgba = _dxt_color_blocks(blocks[:, 8:].tobytes(), True, bw, bh)
            rgba[:, :, 3] = alpha

        n = bw * bh
        b = np.arange(n)
        by, bx = b // bw, b % bw
        rows = by[:, None] * 4 + (np.arange(16) // 4)[None, :]
        cols = bx[:, None] * 4 + (np.arange(16) % 4)[None, :]
        image = np.zeros((bh * 4, bw * 4, 4), np.uint8)
        image[rows.reshape(-1), cols.reshape(-1)] = np.clip(
            rgba.reshape(-1, 4), 0, 255).astype(np.uint8)
        return image[:height, :width]

    if fmt in (FMT_A8R8G8B8, FMT_X8R8G8B8):
        image = np.frombuffer(chunk, np.uint8, width * height * 4).reshape(height, width, 4)
        return image[:, :, [2, 1, 0, 3]].copy()
    if fmt == FMT_A8B8G8R8:
        image = np.frombuffer(chunk, np.uint8, width * height * 4).reshape(height, width, 4)
        return image.copy()
    if fmt == FMT_L8:
        g = np.frombuffer(chunk, np.uint8, width * height).reshape(height, width)
        return np.stack([g, g, g, np.full_like(g, 255)], axis=-1)
    if fmt == FMT_A8:
        a = np.frombuffer(chunk, np.uint8, width * height).reshape(height, width)
        return np.stack([a, a, a, a], axis=-1)
    return None


def _downsample_rgba(rgba):
    h, w = rgba.shape[:2]
    half_h, half_w = max(1, h // 2), max(1, w // 2)
    crop = rgba[:half_h * 2, :half_w * 2].astype(np.float32)
    return crop.reshape(half_h, 2, half_w, 2, 4).mean(axis=(1, 3)).round().astype(np.uint8)


def _downsample_alpha_max(alpha):
    h, w = alpha.shape
    if h % 2 or w % 2:
        alpha = np.pad(alpha, ((0, h % 2), (0, w % 2)), mode='edge')
        h, w = alpha.shape
    half_h, half_w = h // 2, w // 2
    return alpha.reshape(half_h, 2, half_w, 2).max(axis=(1, 3))


def write_vtf(path, tex, max_size=4096, alpha_mul=None, alpha_map=None,
              rgba_map=None, alpha_max=False):
    fmt, flags = _vtf_format(tex)
    # source mangles partial DXT blocks
    force_rgba = (fmt in (VTF_DXT1, VTF_DXT3, VTF_DXT5)
                  and (tex.width % 4 or tex.height % 4))
    if (alpha_mul is not None or alpha_map is not None or rgba_map is not None
            or alpha_max or force_rgba):
        if rgba_map is not None:
            rgba = np.asarray(rgba_map, dtype=np.uint8)
            if rgba.ndim != 3 or rgba.shape[2] != 4:
                return False
        else:
            rgba = decode_rgba(tex)
            if rgba is None or tex.width > max_size or tex.height > max_size:
                return False
            rgba = rgba.copy()
            if alpha_map is not None:
                rgba[:, :, 3] = np.asarray(alpha_map, dtype=np.uint8)
            elif alpha_mul is not None:
                rgba[:, :, 3] = np.clip(rgba[:, :, 3].astype(np.float32) * alpha_mul,
                                        0, 255).astype(np.uint8)
        levels = [rgba]
        while min(levels[-1].shape[:2]) > 1:
            levels.append(_downsample_rgba(levels[-1]))
        if alpha_max:
            alpha_levels = [rgba[:, :, 3]]
            while min(alpha_levels[-1].shape) > 1:
                alpha_levels.append(_downsample_alpha_max(alpha_levels[-1]))
            for level, alpha in zip(levels, alpha_levels):
                level[:, :, 3] = alpha
        blobs = None
        dxtenc = None
        if not force_rgba:
            try:
                from . import dxtenc
            except ImportError:
                dxtenc = None
            if dxtenc is not None:
                enc = dxtenc.encode_levels(levels)
                if enc is not None:
                    blobs, gta_fmt = enc
        if blobs is not None:
            vfmt, flags = ((VTF_DXT1, VTF_ONEBITALPHA)
                           if gta_fmt == dxtenc.FMT_DXT1
                           else (VTF_DXT5, VTF_EIGHTBITALPHA))
            header = bytearray(80)
            struct.pack_into('<4sII', header, 0, b'VTF\0', 7, 2)
            struct.pack_into('<I', header, 12, 80)
            struct.pack_into('<HH', header, 16, tex.width, tex.height)
            struct.pack_into('<I', header, 20, flags)
            struct.pack_into('<HH', header, 24, 1, 0)
            struct.pack_into('<3f', header, 32, 0.5, 0.5, 0.5)
            struct.pack_into('<f', header, 48, 1.0)
            struct.pack_into('<I', header, 52, vfmt)
            struct.pack_into('<B', header, 56, len(blobs))
            struct.pack_into('<I', header, 57, 0xFFFFFFFF)
            struct.pack_into('<H', header, 63, 1)
            with open(path, 'wb') as f:
                f.write(header)
                for blob in reversed(blobs):
                    f.write(blob)
            return True
        header = bytearray(80)
        struct.pack_into('<4sII', header, 0, b'VTF\0', 7, 2)
        struct.pack_into('<I', header, 12, 80)
        struct.pack_into('<HH', header, 16, tex.width, tex.height)
        struct.pack_into('<HH', header, 24, 1, 0)
        struct.pack_into('<3f', header, 32, 0.5, 0.5, 0.5)
        struct.pack_into('<f', header, 48, 1.0)
        struct.pack_into('<I', header, 52, VTF_BGRA8888)
        struct.pack_into('<B', header, 56, len(levels))
        struct.pack_into('<I', header, 57, 0xFFFFFFFF)
        struct.pack_into('<H', header, 63, 1)
        with open(path, 'wb') as f:
            f.write(header)
            for level in reversed(levels):
                f.write(level[:, :, [2, 1, 0, 3]].tobytes())
        return True

    fmt, flags = _vtf_format(tex)
    if fmt is None:
        return False
    if tex.width > max_size or tex.height > max_size:
        return False
    if fmt in (VTF_DXT1, VTF_DXT3, VTF_DXT5):
        if tex.width % 4 or tex.height % 4:
            return False
    chunks = tex.mip_chunks()
    if not chunks or any(len(c) == 0 for c in chunks):
        return False

    # third copy of this header, whatever
    header = bytearray(80)
    struct.pack_into('<4sII', header, 0, b'VTF\0', 7, 2)  # magic, version 7.2
    struct.pack_into('<I', header, 12, 80)  # header size
    struct.pack_into('<HH', header, 16, tex.width, tex.height)  # width, height
    struct.pack_into('<I', header, 20, flags)  # flags
    struct.pack_into('<HH', header, 24, 1, 0)  # frames, first frame
    struct.pack_into('<3f', header, 32, 0.5, 0.5, 0.5)  # reflectivity
    struct.pack_into('<f', header, 48, 1.0)  # bumpmap scale
    struct.pack_into('<I', header, 52, fmt)  # image format
    struct.pack_into('<B', header, 56, len(chunks))  # mip count
    struct.pack_into('<I', header, 57, 0xFFFFFFFF)  # no low-res image
    struct.pack_into('<H', header, 63, tex.depth or 1)  # depth

    with open(path, 'wb') as f:
        f.write(header)
        for chunk in reversed(chunks):  # smallest mip first
            f.write(chunk)
    return True