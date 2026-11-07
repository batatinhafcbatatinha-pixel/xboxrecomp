"""Minimal XBE reader for the port pipeline: sections, flat image, library versions, kernel import table."""
import struct
from pathlib import Path

DATA_SECTIONS = ('.rdata', '.data', '.data1', '.XTLID', 'DOLBY')
KERNEL_THUNK_KEYS = (0x5B6D40B6, 0xEFB1F152)   # retail, debug


class Xbe:
    def __init__(self, path):
        d = self.data = Path(path).read_bytes()
        if d[:4] != b'XBEH':
            raise ValueError(f'{path}: not an XBE')
        self.path = Path(path)
        self.base, = struct.unpack_from('<I', d, 0x104)
        self.image_size, = struct.unpack_from('<I', d, 0x10C)
        n, at = struct.unpack_from('<II', d, 0x11C)
        self.sections = []
        for i in range(n):
            o = at - self.base + i * 56
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<IIIIII', d, o)
            no = name_va - self.base
            name = d[no:d.index(b'\0', no)].decode('ascii', 'replace')
            # retail XBEs flag data sections executable too: code = executable and not a data name
            code = bool(flags & 4) and name not in DATA_SECTIONS and not name.startswith(('$$', '.bss'))
            self.sections.append(dict(name=name, va=va, vsize=vsize, raw=raw, rsize=rsize, flags=flags, code=code))
        self.lo, self.hi = self.base, self.base + self.image_size
        self.img = bytearray(self.image_size)
        for s in self.sections:
            chunk = d[s['raw']:s['raw'] + min(s['rsize'], s['vsize'])]
            self.img[s['va'] - self.base:s['va'] - self.base + len(chunk)] = chunk
        self._blobs = None

    def code_ranges(self):
        return [(s['va'], s['va'] + s['vsize']) for s in self.sections if s['code']]

    def code_blobs(self):
        """(start VA, bytes) of each code section, read once."""
        if self._blobs is None:
            self._blobs = [(a, self.read(a, b - a)) for a, b in self.code_ranges()]
        return self._blobs

    def is_code(self, va):
        return any(a <= va < b for a, b in self.code_ranges())

    def read(self, va, n):
        o = va - self.base
        return bytes(self.img[max(0, o):o + n]) if 0 <= o < len(self.img) else b''

    def libraries(self):
        """{library name: build} from the XBE's library version table."""
        out = {}
        n, at = struct.unpack_from('<II', self.data, 0x160)
        for i in range(n):
            o = at - self.base + i * 16
            name = self.data[o:o + 8].rstrip(bytes(1)).decode('ascii', 'replace')
            out[name] = struct.unpack_from('<H', self.data, o + 12)[0]
        return out

    def d3d_library(self):
        """'D3D8:5849' / 'D3D8LTCG:5849', or None."""
        for name, build in self.libraries().items():
            if name.startswith('D3D8'):
                return f'{name}:{build}'
        return None

    def kernel_thunks(self):
        """(VA of the kernel thunk table, [ordinals])."""
        enc, = struct.unpack_from('<I', self.data, 0x158)
        for key in KERNEL_THUNK_KEYS:
            va = enc ^ key
            if self.lo <= va < self.hi:
                ords, i = [], 0
                while i <= 400:
                    raw = self.read(va + 4 * i, 4)
                    v = struct.unpack('<I', raw)[0] if len(raw) == 4 else 0
                    if v == 0:
                        break
                    ords.append(v & 0x7FFFFFFF)
                    i += 1
                if ords and all(o < 400 for o in ords):
                    return va, ords
        return None, []
