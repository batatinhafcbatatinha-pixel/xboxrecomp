"""Library-code signatures: a short window of code with its absolute addresses and rel32 branch targets masked,
which locates the same library code (and the addresses it holds or uses) in another XBE built with that
library version."""
import re
import struct

BEFORE, AFTER = 24, 40     # window around an operand / length of a window at an address
MAX_REFS = 12              # references to one address tried as windows


def mask(xbe, data):
    m = [True] * len(data)
    for i in range(len(data) - 3):
        v, = struct.unpack_from('<I', data, i)
        if xbe.lo <= v < xbe.hi:
            for k in range(4):
                m[i + k] = False
    for i, b in enumerate(data):
        if b in (0xE8, 0xE9) and i + 5 <= len(data):
            for k in range(1, 5):
                m[i + k] = False
        if b == 0x0F and i + 6 <= len(data) and 0x80 <= data[i + 1] <= 0x8F:
            for k in range(2, 6):
                m[i + k] = False
    return m


def pattern(data, m):
    return b''.join(re.escape(bytes([b])) if keep else b'.' for b, keep in zip(data, m))


_rx = {}


def find_all(xbe, pat):
    rx = _rx.get(pat)
    if rx is None:
        rx = _rx[pat] = re.compile(pat, re.S)
    hits = []
    for a, blob in xbe.code_blobs():
        hits += [a + m.start() for m in rx.finditer(blob)]
        if len(hits) > 1:
            break
    return hits


def signatures_for(xbe, value):
    """Windows that find `value` again in this XBE: unique, and giving back the value."""
    out = []
    if xbe.is_code(value):
        data = xbe.read(value, AFTER)
        pat = pattern(data, mask(xbe, data))
        if find_all(xbe, pat) == [value]:
            out.append({'kind': 'sig', 'at': True, 'pat': pat.hex()})
    needle = struct.pack('<I', value)
    for a, blob in xbe.code_blobs():
        start, n = 0, 0
        while n < MAX_REFS:
            i = blob.find(needle, start)
            if i < 0:
                break
            start, n = i + 1, n + 1
            ref = a + i
            data = xbe.read(ref - BEFORE, BEFORE + AFTER)
            pat = pattern(data, mask(xbe, data))
            if find_all(xbe, pat) == [ref - BEFORE]:
                out.append({'kind': 'sig', 'at': False, 'operand': BEFORE, 'pat': pat.hex()})
    return out


def resolve(xbe, sig):
    hits = find_all(xbe, bytes.fromhex(sig['pat']))
    if len(hits) != 1:
        return None
    if sig['at']:
        return hits[0]
    raw = xbe.read(hits[0] + sig['operand'], 4)
    return struct.unpack('<I', raw)[0] if len(raw) == 4 else None
