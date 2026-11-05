import struct
import zlib

from . import crypto, rpf

_MAGICS = (b'<?xml', b'2TXG', b'GXT2', b'#', b'RBF0', b'ADAT')


def _plausible_plain(d):
    if any(d.startswith(m) for m in _MAGICS):
        return True
    return crypto._printable_fraction(d[:256]) > 0.95


def _inflates(d):
    try:
        out = zlib.decompressobj(-15).decompress(d)
        return len(out) > 32
    except zlib.error:
        return False


def read_binary(arch, entry):
    if entry.kind != rpf.BINARY or not entry.encrypted:
        return arch.read(entry)
    raw = arch.read_raw(entry)
    compressed = entry.file_size != 0
    probe = raw[:4096 - (4096 % 16)] if len(raw) >= 16 else raw
    c = arch.cipher
    first = c.key_for(entry.name, entry.uncompressed_size)
    order = [first] + [k for k in c.keys if k is not first]
    for key in order:
        head = c.decrypt(probe, key)
        if (compressed and _inflates(head)) or (not compressed and _plausible_plain(head)):
            data = c.decrypt(raw, key)
            if compressed:
                return zlib.decompressobj(-15).decompress(data)
            return data
    raise rpf.RpfError(f'{entry.path}: no NG subkey decrypts this binary entry')


def parse_gxt2(data):
    if data[:4] not in (b'2TXG', b'GXT2'):
        raise ValueError('not a GXT2 table')
    # magic, count, count x (hash, offset), strings
    count = struct.unpack_from('<I', data, 4)[0]
    out = {}
    for i in range(count):
        h, off = struct.unpack_from('<II', data, 8 + i * 8)
        end = data.find(b'\0', off)
        out[h] = data[off:end if end >= 0 else len(data)].decode('utf-8', 'replace')
    return out
