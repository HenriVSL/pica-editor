#!/usr/bin/env python3
"""T19 test helper: copy an exe with a different version resource (VS_FIXEDFILEINFO + the version strings).
  python3 tools/patch_ver.py pica.exe setup/obj/pica09.exe 0.9
The new version string must have the same length as the old one (APP_VERSION, e.g. "1.0" -> "0.9").
Used to build obj\\setup09.exe for the installer's upgrade and downgrade tests (setup/Makefile target "old").
"""
import struct
import sys


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    src, dst, ver = sys.argv[1], sys.argv[2], sys.argv[3]
    data = bytearray(open(src, 'rb').read())
    sig = struct.pack('<I', 0xFEEF04BD)
    i = data.find(sig)
    if i < 0:
        print('no VS_FIXEDFILEINFO in', src)
        return 1
    parts = [int(x) for x in ver.split('.')] + [0, 0, 0, 0]
    ms = (parts[0] << 16) | parts[1]
    ls = (parts[2] << 16) | parts[3]
    old_ms, old_ls = struct.unpack_from('<II', data, i + 8)
    old = '%d.%d' % (old_ms >> 16, old_ms & 0xFFFF)
    if len(old) != len(ver):
        print('version strings differ in length: %s vs %s' % (old, ver))
        return 1
    struct.pack_into('<IIII', data, i + 8, ms, ls, ms, ls)          # file and product version
    # the StringFileInfo block follows VS_FIXEDFILEINFO: replace the UTF-16 version strings there
    end = min(len(data), i + 2048)
    o, n = old.encode('utf-16-le') + b'\0\0', ver.encode('utf-16-le') + b'\0\0'
    count = 0
    j = data.find(o, i, end)
    while j >= 0:
        data[j:j + len(o)] = n
        count += 1
        j = data.find(o, j + len(o), end)
    open(dst, 'wb').write(data)
    print('%s: version %s -> %s (%d strings)' % (dst, old, ver, count))
    return 0


if __name__ == '__main__':
    sys.exit(main())
