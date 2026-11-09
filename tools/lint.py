#!/usr/bin/env python3
"""Pica lint (DESIGN 5.3): VC6 dialect and Win98 API rules for src/core, src/win and tests (not tests/mac).
  python3 tools/lint.py [files...]      exit 1 on findings, prints file:line: message
Checks: for-loop declarations, banned tokens (STL, std::, long long outside etypes.h, snprintf, nullptr, variadic
macros, stub/slow Win32 APIs, FILE_SHARE_DELETE, windows.h in core), non-ASCII bytes and BOMs in sources, CRLF line
ends for res/*, the NMAKE Makefile, *.lng and help.md.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FOR_DECL = re.compile(r'for\s*\(\s*(const\s+)?(unsigned\s+|signed\s+)?(int|long|char|short|u8|u16|u32|i32|epos|ecp|size_t)\b')
BANNED = [
    (re.compile(r'\bstd::'), 'std:: (no STL)'),
    (re.compile(r'#\s*include\s*<(vector|string|map|list|set|deque|queue|stack|algorithm|iostream|sstream|fstream|'
                r'memory|utility|functional|iterator|new|cstdint|cstdio|cstdlib|cstring|cstdarg|limits)>'),
     'C++ standard library header (no STL)'),
    (re.compile(r'#\s*include\s*<(stdint|inttypes|stdbool)\.h>'), 'C99 header (not in VC6)'),
    (re.compile(r'\bsnprintf\b|\b_snprintf\b|\bvsnprintf\b|\b_vsnprintf\b'), 'snprintf (use es_fmt)'),
    (re.compile(r'\bnullptr\b'), 'nullptr (C++11)'),
    (re.compile(r'__VA_ARGS__'), 'variadic macro (not in VC6)'),
    (re.compile(r'\bFILE_SHARE_DELETE\b'), 'FILE_SHARE_DELETE (invalid on Win9x)'),
    (re.compile(r'\bGetTextExtentExPoint[AW]?\b'), 'GetTextExtentExPoint (50 ms per 1000 chars on the VM)'),
    (re.compile(r'\bGetCharWidth32[AW]?\b'), 'GetCharWidth32 (stub on Win98)'),
    (re.compile(r'\bGetCharABCWidthsW\b'), 'GetCharABCWidthsW (stub on Win98)'),
    (re.compile(r'\bCharLowerW\b|\bCharUpperW\b|\bCharLowerBuffW\b|\bCharUpperBuffW\b'), 'CharLower/UpperW (stub)'),
    (re.compile(r'\bGetTextMetricsW\b'), 'GetTextMetricsW (stub on Win98)'),
    (re.compile(r'\bDrawTextW\b'), 'DrawTextW (stub on Win98)'),
    (re.compile(r'\bCreateFileW\b'), 'CreateFileW (stub on Win98)'),
    (re.compile(r'\bCompareStringW\b|\bIsTextUnicode\b'), 'stub API on Win98'),
    (re.compile(r'\bdynamic_cast\b|\btypeid\b'), 'RTTI'),
    (re.compile(r'\btry\s*\{|\bcatch\s*\(|\bthrow\b'), 'C++ exceptions'),
]
LONG_LONG = re.compile(r'\blong\s+long\b')
WINDOWS_H = re.compile(r'#\s*include\s*[<"](windows|winbase|winuser|wingdi|commctrl|shellapi)\.h[>"]', re.I)
SRC_EXT = ('.cpp', '.h')


def strip_code(text):
    """Remove comments and string/char literals (keeps line structure)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i < n and not (text[i] == '*' and i + 1 < n and text[i + 1] == '/'):
                if text[i] == '\n':
                    out.append('\n')
                i += 1
            i += 2
        elif c == '"' or c == "'":
            q = c
            out.append(q)
            i += 1
            while i < n and text[i] != q and text[i] != '\n':
                if text[i] == '\\':
                    i += 1
                i += 1
            out.append(q)
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def keep_includes(raw_lines, stripped_lines):
    # includes are checked on the raw line (strip_code blanks the "..." of #include "x.h")
    return [r if r.lstrip().startswith('#') else s for r, s in zip(raw_lines, stripped_lines)]


def rel(p):
    return os.path.relpath(p, ROOT).replace(os.sep, '/')


def source_files():
    out = []
    for d in ('src/core', 'src/win', 'tests'):
        for dp, dn, fn in os.walk(os.path.join(ROOT, d)):
            r = rel(dp)
            if r.startswith('tests/mac') or r.startswith('tests/build') or r.startswith('tests/data') or r.startswith('tests/tmp'):
                continue
            for f in sorted(fn):
                if f.endswith(SRC_EXT):
                    out.append(os.path.join(dp, f))
    return out


def crlf_files():
    out = []
    resdir = os.path.join(ROOT, 'res')
    if os.path.isdir(resdir):
        for f in sorted(os.listdir(resdir)):
            if f.lower().endswith(('.h', '.rc', '.rc2')):
                out.append(os.path.join(resdir, f))
    mk = os.path.join(ROOT, 'Makefile')
    if os.path.exists(mk):
        out.append(mk)
    langs = os.path.join(ROOT, 'src', 'langs')
    if os.path.isdir(langs):
        out += [os.path.join(langs, f) for f in sorted(os.listdir(langs)) if f.endswith('.lng')]
    h = os.path.join(ROOT, 'src', 'help', 'help.md')
    if os.path.exists(h):
        out.append(h)
    return out


def check_source(path, findings):
    data = open(path, 'rb').read()
    r = rel(path)
    if data.startswith(b'\xef\xbb\xbf'):
        findings.append('%s:1: UTF-8 BOM' % r)
    for ln, line in enumerate(data.split(b'\n'), 1):
        if any(b > 0x7F for b in line):
            findings.append('%s:%d: non-ASCII byte (use \\xNN escapes)' % (r, ln))
    text = data.decode('latin-1')
    raw_lines = text.split('\n')
    lines = keep_includes(raw_lines, strip_code(text).split('\n'))
    is_core = r.startswith('src/core/')
    if not is_core and r.endswith('.cpp'):
        check_include_order(path, raw_lines, findings)
    for ln, line in enumerate(lines, 1):
        if FOR_DECL.search(line):
            findings.append('%s:%d: declaration in for() (declare the loop variable before the loop)' % (r, ln))
        for rx, msg in BANNED:
            if rx.search(line):
                findings.append('%s:%d: %s' % (r, ln, msg))
        if LONG_LONG.search(line) and not r.endswith('src/core/etypes.h'):
            findings.append('%s:%d: long long (use i64/u64)' % (r, ln))
        if is_core and WINDOWS_H.search(line):
            findings.append('%s:%d: Windows header in core' % (r, ln))
        if is_core and re.search(r'#\s*include\s*"\.\./win/|#\s*include\s*"(app|tab|views|cmds|ui)\.h"', line):
            findings.append('%s:%d: win header in core' % (r, ln))


def check_include_order(path, raw_lines, findings):
    """src/win and tests: <windows.h> (if included) must come before any project header (E_NOTIMPL macro clash)."""
    first_local = None
    for ln, line in enumerate(raw_lines, 1):
        t = line.strip()
        if re.match(r'#\s*include\s*"', t) and first_local is None:
            first_local = ln
        if re.match(r'#\s*include\s*<windows\.h>', t, re.I) and first_local is not None:
            findings.append('%s:%d: <windows.h> after a project header (line %d): include it first' % (rel(path), ln, first_local))
            return


def check_crlf(path, findings):
    data = open(path, 'rb').read()
    r = rel(path)
    if data.startswith(b'\xef\xbb\xbf'):
        findings.append('%s:1: UTF-8 BOM' % r)
    lines = data.split(b'\n')
    for ln, line in enumerate(lines, 1):
        if ln == len(lines) and line == b'':
            break
        if not line.endswith(b'\r'):
            findings.append('%s:%d: LF line end (CRLF required)' % (r, ln))
            break
    for ln, line in enumerate(lines, 1):
        if any(b > 0x7F for b in line):
            findings.append('%s:%d: non-ASCII byte' % (r, ln))
            break


def main():
    args = sys.argv[1:]
    findings = []
    crlf = set(crlf_files())
    if args:
        files = [os.path.abspath(a) for a in args]
    else:
        files = source_files() + sorted(crlf)
    for f in files:
        if f in crlf:
            check_crlf(f, findings)
            if f.endswith('.h'):
                check_source(f, findings)
        elif f.endswith(SRC_EXT):
            check_source(f, findings)
    for x in findings:
        print(x)
    print('lint: %d files, %d findings' % (len(files), len(findings)))
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
