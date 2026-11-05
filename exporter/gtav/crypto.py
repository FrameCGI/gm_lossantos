import struct
from pathlib import Path

NG_KEY_COUNT = 101
NG_KEY_SIZE = 272  # 17 rounds x 16 bytes
NG_ROUNDS = 17

ENC_NONE = 0
ENC_OPEN = 0x4E45504F  # b'OPEN', an OpenIV-style unencrypted archive
ENC_AES = 0x0FFFFFF9
ENC_NG = 0x0FEFFFFF

_PERM_A = ((0, 1, 2, 3), (4, 5, 6, 7), (8, 9, 10, 11), (12, 13, 14, 15))  # keeps byte positions
_PERM_B = ((0, 7, 10, 13), (1, 4, 11, 14), (2, 5, 8, 15), (3, 6, 9, 12))  # shuffled

try:
    import numpy as np
except ImportError:  # the exporter runs without NumPy, just slower
    np = None


class CryptoError(RuntimeError):
    pass


def _read(path):
    path = Path(path)
    if not path.is_file():
        raise CryptoError(f'missing key file: {path}')
    return path.read_bytes()


def toc_shaped(head, entry_count):
    if len(head) < 16:
        return False
    name_offset, ident, first, count = struct.unpack_from('<IIII', head, 0)
    return (ident == 0x7FFFFF00 and name_offset == 0
            and 1 <= first <= entry_count and 0 < count <= entry_count)


def _printable_fraction(data):
    if not data:
        return 0.0
    good = sum(1 for b in data if 0x20 <= b <= 0x7E or b == 0)
    return good / len(data)


class NgCipher:
    def __init__(self, keys, tables, lut):
        self.keys = keys
        self.tables = tables
        self.lut = lut
        self._np_tables = None

    @classmethod
    def load(cls, folder):
        folder = Path(folder)
        blob = _read(folder / 'gtav_ng_key.dat')
        if len(blob) != NG_KEY_COUNT * NG_KEY_SIZE:
            raise CryptoError(f'{folder / "gtav_ng_key.dat"}: expected '
                              f'{NG_KEY_COUNT * NG_KEY_SIZE} bytes, got {len(blob)}')
        keys = [blob[i * NG_KEY_SIZE:(i + 1) * NG_KEY_SIZE]
                for i in range(NG_KEY_COUNT)]

        raw = _read(folder / 'gtav_ng_decrypt_tables.dat')
        expected = NG_ROUNDS * 16 * 256 * 4  # 17 x 16 x 256 u32
        if len(raw) != expected:
            raise CryptoError(f'{folder / "gtav_ng_decrypt_tables.dat"}: '
                              f'expected {expected} bytes, got {len(raw)}')
        tables = [[[struct.unpack_from('<I', raw, ((r * 16 + c) * 256 + i) * 4)[0]
                    for i in range(256)]
                   for c in range(16)]
                  for r in range(NG_ROUNDS)]

        lut = _read(folder / 'gtav_hash_lut.dat')
        if len(lut) != 256:
            raise CryptoError(f'{folder / "gtav_hash_lut.dat"}: expected 256 '
                              f'bytes, got {len(lut)}')
        return cls(keys, tables, lut)

    def filename_hash(self, name):
        result = 0
        for ch in name:
            code = ord(ch)
            if code > 0xFF:
                raise CryptoError(f'name is not byte text: {name!r}')
            temp = (1025 * (self.lut[code] + result)) & 0xFFFFFFFF
            result = ((temp >> 6) ^ temp) & 0xFFFFFFFF
        return (32769 * (((9 * result) >> 11) ^ (9 * result))) & 0xFFFFFFFF

    def key_for(self, name, size):
        return self.keys[(self.filename_hash(name) + (size & 0xFFFFFFFF) + 61)
                         % NG_KEY_COUNT]


    def decrypt(self, data, key):
        if not data:
            return b''
        blocks = len(data) // 16
        if not blocks:
            return bytes(data)
        # trailing partial block stays as is
        out = bytearray(data)
        out[:blocks * 16] = self._decrypt_blocks(bytes(data[:blocks * 16]), key)
        return bytes(out)

    def _decrypt_blocks(self, data, key):
        if np is not None and len(data) >= 16 * 8:
            try:
                return self._decrypt_np(data, key)
            except Exception:
                pass
        return self._decrypt_py(data, key)

    def _decrypt_np(self, data, key):
        if self._np_tables is None:
            self._np_tables = [[np.asarray(col, dtype=np.uint32) for col in rnd]
                               for rnd in self.tables]
        tables = self._np_tables
        subkeys = np.frombuffer(key, dtype='<u4').reshape(NG_ROUNDS, 4)

        n = len(data) // 16
        src = np.frombuffer(data, dtype=np.uint8, count=n * 16).reshape(n, 16)
        for r in range(NG_ROUNDS):
            perm = _PERM_A if r in (0, 1, NG_ROUNDS - 1) else _PERM_B
            rnd = tables[r]
            dst = np.empty_like(src)
            for w in range(4):
                a, b, c, d = perm[w]
                acc = rnd[a][src[:, a]] ^ rnd[b][src[:, b]]
                acc ^= rnd[c][src[:, c]] ^ rnd[d][src[:, d]]
                acc ^= subkeys[r, w]
                dst[:, w * 4:(w + 1) * 4] = acc.view(np.uint8).reshape(n, 4)
            src = dst
        return src.tobytes()

    def _decrypt_py(self, data, key):
        words = struct.unpack_from('<%dI' % (NG_KEY_SIZE // 4), key)
        subkeys = [words[i * 4:i * 4 + 4] for i in range(NG_ROUNDS)]
        out = bytearray(len(data))
        for block in range(len(data) // 16):
            b = data[block * 16:block * 16 + 16]
            for r in range(NG_ROUNDS):
                perm = _PERM_A if r in (0, 1, NG_ROUNDS - 1) else _PERM_B
                rnd, sk = self.tables[r], subkeys[r]
                packed = 0
                for w in range(4):
                    a, c, d, e = perm[w]
                    value = (rnd[a][b[a]] ^ rnd[c][b[c]]
                             ^ rnd[d][b[d]] ^ rnd[e][b[e]] ^ sk[w])
                    packed |= (value & 0xFFFFFFFF) << (w * 32)
                b = packed.to_bytes(16, 'little')
            out[block * 16:block * 16 + 16] = b
        return bytes(out)

    def detect_key(self, entries, names, entry_count):
        head = entries[:64]
        name_head = names[:64]
        # brute force all 101
        for key in self.keys:
            if not toc_shaped(self.decrypt(head, key), entry_count):
                continue
            if _printable_fraction(self.decrypt(name_head, key)) < 0.8:
                continue
            return key
        return None
