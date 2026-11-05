import argparse
import os
import struct
import sys
import urllib.request
import zlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEYS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'exporter', 'gtav', 'keys')
MAGIC_URL = ('https://raw.githubusercontent.com/dexyfex/CodeWalker/master/'
             'CodeWalker.Core/Resources/magic.dat')
SIZES = (27472, 278528, 256, 16)  # same order as NAMES
NAMES = ('gtav_ng_key.dat', 'gtav_ng_decrypt_tables.dat', 'gtav_hash_lut.dat',
         'gtav_awc_key.dat')


def _i32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v >= 0x80000000 else v


class DotNetRandom:
    MBIG, MSEED = 2147483647, 161803398

    def __init__(self, seed):
        sub = 2147483647 if seed == -2147483648 else abs(seed)
        mj = self.MSEED - sub
        sa = [0] * 56
        sa[55] = mj
        mk = 1
        for i in range(1, 55):
            ii = (21 * i) % 55
            sa[ii] = mk
            mk = mj - mk
            if mk < 0:
                mk += self.MBIG
            mj = sa[ii]
        for _ in range(4):
            for i in range(1, 56):
                sa[i] = _i32(sa[i] - sa[1 + (i + 30) % 55])
                if sa[i] < 0:
                    sa[i] += self.MBIG
        self.sa, self.inext, self.inextp = sa, 0, 21

    def _sample(self):
        self.inext = 1 if self.inext + 1 >= 56 else self.inext + 1
        self.inextp = 1 if self.inextp + 1 >= 56 else self.inextp + 1
        v = _i32(self.sa[self.inext] - self.sa[self.inextp])
        if v == self.MBIG:
            v -= 1
        if v < 0:
            v += self.MBIG
        self.sa[self.inext] = v
        return v

    def next_bytes(self, n):
        return bytes(self._sample() & 0xFF for _ in range(n))


def joaat_bytes(data):
    h = 0
    for b in data:
        h = (h + b) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


RS_XXTEA_XOR = 0x7B3A207F


def vgmstream_awckey(awc_key):
    words = struct.unpack('<4I', awc_key)
    return b''.join(struct.pack('>I', w ^ RS_XXTEA_XOR) for w in words)


def unlock(magic, aes_key):
    seed = joaat_bytes(aes_key)
    if seed >= 0x80000000:
        seed -= 0x100000000  # (int) cast of the uint hash
    rnd = DotNetRandom(seed)
    n = len(magic)
    rs = [rnd.next_bytes(n) for _ in range(4)]
    db = bytes((magic[i] - rs[0][i] - rs[1][i] - rs[2][i] - rs[3][i]) & 0xFF for i in range(n))
    full = n - n % 16
    dec = Cipher(algorithms.AES(aes_key), modes.ECB()).decryptor()
    db = dec.update(db[:full]) + dec.finalize() + db[full:]
    return zlib.decompressobj(-15).decompress(db)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--magic', help='local magic.dat (default: fetch from the CodeWalker repo)')
    ap.add_argument('--write', action='store_true', help='write gtav_awc_key.dat')
    a = ap.parse_args()

    aes_key = open(os.path.join(KEYS, 'gtav_aes_key.dat'), 'rb').read()
    if a.magic:
        magic = open(a.magic, 'rb').read()
    else:
        magic = urllib.request.urlopen(MAGIC_URL, timeout=60).read()
    b = unlock(magic, aes_key)
    if len(b) < sum(SIZES):
        raise SystemExit(f'unlocked blob too short ({len(b)} B) - wrong AES key or magic.dat')
    parts, p = [], 0
    for s in SIZES:
        parts.append(b[p:p + s])
        p += s
    ok = True
    for name, part in zip(NAMES[:3], parts[:3]):
        path = os.path.join(KEYS, name)
        same = os.path.isfile(path) and open(path, 'rb').read() == part
        print('%-28s %s' % (name, 'matches existing file' if same else 'DIFFERS / missing'))
        ok &= same
    if not ok:
        raise SystemExit('cross-check failed - not writing anything')
    if a.write:
        with open(os.path.join(KEYS, NAMES[3]), 'wb') as fh:
            fh.write(parts[3])
        print('wrote', NAMES[3], '(16 bytes)')
    else:
        print('cross-check OK; rerun with --write to save', NAMES[3])
    return 0


if __name__ == '__main__':
    sys.exit(main())
