from __future__ import annotations

import struct

import numpy as np

SYSTEM_BASE = 0x50000000  # system[n]
GRAPHICS_BASE = 0x60000000  # graphics[n]
RESOURCE_FILE_BASE_SIZE = 16


class ResourceError(RuntimeError):
    pass


def split_payload(data, system_size):
    if system_size < 0 or system_size > len(data):
        raise ResourceError(
            f'system size {system_size} outside a {len(data)}-byte payload')
    return data[:system_size], data[system_size:]


class ResourceReader:

    def __init__(self, system, graphics=b''):
        self.system = memoryview(system)
        self.graphics = memoryview(graphics)

    def window(self, addr):
        if addr is None:
            raise ResourceError('null pointer')
        addr = int(addr)
        if addr & SYSTEM_BASE == SYSTEM_BASE:
            return self.system, addr - SYSTEM_BASE
        if addr & GRAPHICS_BASE == GRAPHICS_BASE:
            return self.graphics, addr - GRAPHICS_BASE
        raise ResourceError(f'address {addr:#x} is in no known window')

    def slice(self, addr, length):
        buf, offset = self.window(addr)
        if length < 0 or offset + length > len(buf):
            raise ResourceError(
                f'read of {length} at {addr:#x} runs past its '
                f'{len(buf) - offset}-byte window')
        return buf[offset:offset + length]

    def bytes_at(self, addr, length):
        return bytes(self.slice(addr, length))

    def u8(self, addr):
        return self.slice(addr, 1)[0]

    def u16(self, addr):
        return struct.unpack_from('<H', self.slice(addr, 2))[0]

    def i16(self, addr):
        return struct.unpack_from('<h', self.slice(addr, 2))[0]

    def u32(self, addr):
        return struct.unpack_from('<I', self.slice(addr, 4))[0]

    def i32(self, addr):
        return struct.unpack_from('<i', self.slice(addr, 4))[0]

    def u64(self, addr):
        return struct.unpack_from('<Q', self.slice(addr, 8))[0]

    def f32(self, addr):
        return struct.unpack_from('<f', self.slice(addr, 4))[0]

    def vec3(self, addr):
        return struct.unpack_from('<3f', self.slice(addr, 12))

    def string_at(self, addr):
        if not addr:
            return ''
        buf, offset = self.window(addr)
        end = bytes(buf[offset:]).find(b'\0')
        if end < 0:
            end = len(buf) - offset
        return bytes(buf[offset:offset + end]).decode('utf-8', 'replace')

    def array(self, addr, dtype, count):
        dtype = np.dtype(dtype)
        if count <= 0:
            return np.empty(0, dtype=dtype)
        return np.frombuffer(self.slice(addr, dtype.itemsize * count),
                             dtype=dtype, count=count)

    def u16s(self, addr, count):
        return self.array(addr, '<u2', count)

    def u32s(self, addr, count):
        return self.array(addr, '<u4', count)

    def u64s(self, addr, count):
        return self.array(addr, '<u8', count)


def read_floats_at(reader, addr, count):
    return reader.array(addr, '<f4', count)


def pages_info(reader):
    root = SYSTEM_BASE
    pointer = reader.u64(root + 8)
    if pointer == 0:
        return None
    system_pages = reader.u8(pointer + 8)
    graphics_pages = reader.u8(pointer + 9)
    return {'system_pages': system_pages, 'graphics_pages': graphics_pages}
