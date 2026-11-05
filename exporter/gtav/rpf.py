from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

try:
    from . import crypto
except ImportError:  # imported as a plain script
    import crypto

MAGIC = 0x52504637  # "RPF7"
BLOCK = 512  # file offsets are in these
RSC_HEADER = 16
TOC_SIZE = 0xFFFFFF  # saturated u24, real size in the RSC7 header

DIR = 'dir'
BINARY = 'binary'
RESOURCE = 'resource'


class RpfError(RuntimeError):
    pass


@dataclass
class Entry:
    index: int
    kind: str
    name: str
    name_offset: int
    file_size: int = 0
    file_offset: int = 0
    uncompressed_size: int = 0
    encrypted: bool = False
    entries_index: int = 0
    entries_count: int = 0
    system_flags: int = 0
    graphics_flags: int = 0
    path: str = ''
    parent: Entry | None = field(default=None, repr=False)

    @property
    def is_dir(self):
        return self.kind == DIR

    @property
    def extension(self):
        name = self.name.lower()
        dot = name.rfind('.')
        return name[dot:] if dot > 0 else ''

    def __str__(self):
        return self.path or self.name


def _cstr(blob, offset):
    if offset >= len(blob):
        return ''
    end = blob.find(b'\0', offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode('utf-8', 'replace')


def size_from_flags(flags):
    # low nibble is log2 page size, counts get added
    blocks = (((flags >> 27) & 1) * 1
              + ((flags >> 26) & 1) * 2
              + ((flags >> 25) & 1) * 4
              + ((flags >> 24) & 1) * 8
              + ((flags >> 17) & 0x7F) * 16
              + ((flags >> 11) & 0x3F) * 32
              + ((flags >> 7) & 0xF) * 64
              + ((flags >> 5) & 3) * 128
              + ((flags >> 4) & 1) * 256)
    return (0x200 << ((flags >> 0) & 0xF)) * blocks


def page_counts(flags):
    return [((flags >> 4) & 1), ((flags >> 5) & 3), ((flags >> 7) & 0xF),
            ((flags >> 11) & 0x3F), ((flags >> 17) & 0x7F), ((flags >> 24) & 1),
            ((flags >> 25) & 1), ((flags >> 26) & 1), ((flags >> 27) & 1)]


def _inflate(data):
    # raw deflate
    obj = zlib.decompressobj(-15)
    try:
        out = obj.decompress(data)
        out += obj.flush()
    except zlib.error:
        return None
    return out


class RpfArchive:
    def __init__(self, path, cipher, name=None, size=None, start_pos=0):
        self.path = Path(path)
        self.cipher = cipher
        self.start_pos = start_pos
        self.name = name or self.path.name
        self.size = size if size is not None else self.path.stat().st_size
        self.encryption = crypto.ENC_NONE
        self.entries = []
        self.root = None
        self._file = self.path.open('rb')
        try:
            self._read_toc()
        except Exception:
            self._file.close()
            raise


    def close(self):
        if self._file is not None:
            self._file.close()
            self._file = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


    def _read_toc(self):
        head = self._read_at(self.start_pos, 16, 'header')
        magic, count, names_length, encryption = struct.unpack('<IIII', head)
        if magic != MAGIC:
            raise RpfError(f'{self.path}: not an RPF7 archive at 0x{self.start_pos:x} '
                           f'(magic 0x{magic:08x})')
        if count == 0 or names_length > 0x1000000:
            raise RpfError(f'{self.path}: damaged header '
                           f'({count} entries, {names_length} name bytes)')
        self.encryption = encryption

        entries = self._read_at(self.start_pos + 16, count * 16, 'entries')
        names = self._read_at(self.start_pos + 16 + count * 16, names_length, 'names')

        if encryption in (crypto.ENC_NONE, crypto.ENC_OPEN):
            pass
        elif encryption == crypto.ENC_NG:
            key = self.cipher.key_for(self.name, self.size)
            if not crypto.toc_shaped(self.cipher.decrypt(entries[:64], key), count):
                key = self.cipher.detect_key(entries, names, count)
                if key is None:
                    raise RpfError(f'{self.path}: no NG key decrypts the table of '
                                   f'contents (wrong key files for this install?)')
            entries = self.cipher.decrypt(entries, key)
            names = self.cipher.decrypt(names, key)
        elif encryption == crypto.ENC_AES:
            raise RpfError(f'{self.path}: AES-encrypted archive, not supported')
        else:
            raise RpfError(f'{self.path}: unknown table encryption 0x{encryption:08x}')

        self._parse(entries, names, count)

    def _parse(self, entries, names, count):
        self.entries = []
        for i in range(count):
            # 0   u16 name offset
            # 2   u24 stored size
            # 5   u24 file offset in 512 blocks
            # 8   per kind
            #       dir       first child, child count
            #       binary    uncompressed size, encrypted
            #       resource  system flags, graphics flags
            raw = entries[i * 16:(i + 1) * 16]
            word0, word1, word2, word3 = struct.unpack('<IIII', raw)

            name_offset = word0 & 0xFFFF
            stored_size = (word0 >> 16) | ((word1 & 0xFF) << 16)
            file_offset = (word1 >> 8) & 0xFFFFFF

            entry = Entry(index=i, kind=RESOURCE, name=_cstr(names, name_offset),
                          name_offset=name_offset)
            entry.file_size = stored_size
            entry.file_offset = file_offset

            if word1 == 0x7FFFFF00:
                entry.kind = DIR
                entry.entries_index = word2
                entry.entries_count = word3
            elif (word1 & 0x80000000) == 0:
                entry.kind = BINARY
                entry.uncompressed_size = word2
                entry.encrypted = word3 == 1
            else:
                entry.file_offset &= 0x7FFFFF
                entry.system_flags = word2
                entry.graphics_flags = word3
                if entry.name.lower().endswith('.ysc'):
                    entry.encrypted = True

            self.entries.append(entry)

        if not self.entries or not self.entries[0].is_dir:
            raise RpfError(f'{self.path}: table of contents has no root directory')

        self.root = self.entries[0]
        self.root.path = self.name
        stack = [self.root]
        while stack:
            directory = stack.pop()
            first = directory.entries_index
            last = first + directory.entries_count
            if last > len(self.entries):
                raise RpfError(f'{self.path}: directory {directory.path} points past '
                               f'the table of contents')
            for entry in self.entries[first:last]:
                entry.parent = directory
                entry.path = f'{directory.path}/{entry.name}'
                if entry.is_dir:
                    stack.append(entry)


    def _read_at(self, offset, length, what):
        if length <= 0:
            return b''
        self._file.seek(offset)
        data = self._file.read(length)
        if len(data) != length:
            raise RpfError(f'{self.path}: short read for {what} at '
                           f'0x{offset:x} ({len(data)}/{length} bytes)')
        return data

    def get(self, path):
        parts = [p for p in path.replace('\\', '/').lower().split('/') if p]
        if parts and parts[0] == self.name.lower():
            parts = parts[1:]
        entry = self.root
        for part in parts:
            match = None
            for child in self.children(entry):
                if child.name.lower() == part:
                    match = child
                    break
            if match is None:
                return None
            entry = match
        return entry

    def children(self, directory):
        if not directory.is_dir:
            return []
        return self.entries[directory.entries_index:
                            directory.entries_index + directory.entries_count]

    def walk(self, directory=None):
        directory = directory or self.root
        for child in self.children(directory):
            yield child
            if child.is_dir:
                yield from self.walk(child)

    def find(self, needle, kind=None):
        needle = needle.lower()
        return [e for e in self.walk()
                if needle in e.name.lower() and (kind is None or e.kind == kind)]

    def file_size(self, entry):
        if entry.is_dir:
            return 0
        if entry.kind == BINARY:
            return entry.file_size or entry.uncompressed_size
        if entry.file_size == TOC_SIZE:
            head = self._read_at(self.start_pos + entry.file_offset * BLOCK, 16, 'rsc')
            # real size is scattered through the RSC7 header
            entry.file_size = ((head[7] << 0) | (head[14] << 8)
                               | (head[5] << 16) | (head[2] << 24))
        if entry.file_size == 0:
            return size_from_flags(entry.system_flags) + size_from_flags(entry.graphics_flags)
        return entry.file_size

    def read_raw(self, entry):
        size = self.file_size(entry)
        if size <= 0:
            return b''
        return self._read_at(self.start_pos + entry.file_offset * BLOCK, size, entry.name)

    def read(self, entry):
        if entry.is_dir:
            raise RpfError(f'{entry.path}: is a directory')
        size = self.file_size(entry)
        if size <= 0:
            return b''
        data = self.read_raw(entry)

        if entry.kind == RESOURCE:
            data = data[RSC_HEADER:]
        if entry.encrypted:
            length = entry.file_size if entry.kind == RESOURCE else entry.uncompressed_size
            data = self.cipher.decrypt(data, self.cipher.key_for(entry.name, length))
        # stored size 0 = not compressed
        if entry.kind == BINARY and entry.file_size == 0:
            return data

        inflated = _inflate(data)
        return data if inflated is None else inflated

    def open_child(self, entry):
        if entry.kind == DIR or not entry.name.lower().endswith('.rpf'):
            raise RpfError(f'{entry.path}: not an archive')
        return RpfArchive(self.path, self.cipher, name=entry.name,
                          size=self.file_size(entry),
                          start_pos=self.start_pos + entry.file_offset * BLOCK)


def open_archive(path, keys, **kwargs):
    return RpfArchive(path, crypto.NgCipher.load(keys), **kwargs)
