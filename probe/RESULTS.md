# Editor probe: GDI text rendering, file I/O, memory, platform facts (VM A)

Probe program: `probe/edprobe.cpp` (VC6, Win32 + GDI, builds clean at /W3), `probe/Makefile`.
Measured 2026-11-09 on 86Box VM A: Pentium II 233, 128 MB, Voodoo3, Win98 SE (4.10.2222 A), 800x600, 16 bpp RGB565, 96 dpi.

| Mode | What it does | Output |
|---|---|---|
| `edprobe -bench` | all render + file/memory tests (about 8-9 min) | `BENCH.TXT` (copy of run 1); runs: `BENCH1.TXT`, `BENCH2.TXT` |
| `edprobe -render` / `-io` | only one half | `BENCH.TXT` (`-o NAME` for another name, `-reps N`) |
| `edprobe -facts` | platform facts (APIs, charsets, file semantics, byte loops, ExtTextOut run length) | `FACTS.TXT` |
| `edprobe -chunks` | only the ReadFile chunk / find-in-files part of `-facts` | `CHUNKS.TXT` |
| `edprobe -show text\|hex\|fonts\|prop\|soft [fontIdx]` | draws one view and stays open (for screenshots) | |

Screenshots (86Box window, Retina 2x): `shot_text.png` (Courier New 10pt text view), `shot_hex.png` (hex view with
selection, byte colouring), `shot_fonts.png` (every font candidate in bands), `shot_charsets.png` (Cyrillic/Polish test).

## Method

- Window: top-level, `WS_EX_TOPMOST`, `CS_OWNDC`, client **780x540**, shown with `ShowWindow(SW_SHOWNORMAL)` twice,
  started with `w98 start` (visible, nothing covering it), no VNC connected, VM lock held.
- Text: 3000 lines of generated C (deterministic LCG): average **42.9 chars/line** (tabs expanded to 4), max 104,
  **4.9 colour runs/line** (keywords, types, numbers, strings, comments, preprocessor; operators/identifiers default colour),
  plus a 6-char line-number gutter run per row and one highlighted current line. Courier New 10pt = 34 rows x 91 text columns,
  about 1600 chars per screen. A "dense" set (116 chars/line, 14 runs/line) is the worst case.
- One repaint = `InvalidateRect(NULL)` + `UpdateWindow` (or `ScrollWindowEx` + `UpdateWindow`), WM_PAINT draws, then a
  `GetPixel` on the window DC so the display driver has finished before the clock stops. QPC (1.19 MHz). 60 repaints per test
  after 3 warm-ups, scroll position varies between repaints. Median / worst reported.
- Two complete runs: they agree to within about 1 ms. The emulator quantises GDI-to-card work to roughly 1 ms steps (many
  values sit at x.99), so treat differences under 1 ms as noise.

## System observations

- 16 bpp **RGB565**. `SPI_GETFONTSMOOTHING` = 0, and black-on-white text renders with exactly **2 colours** for Courier New,
  Lucida Console, Tahoma and Arial 24px: **no antialiasing at all** (and no ClearType on 9x).
- Fixed-pitch faces installed: **Courier New** (TT), **Lucida Console** (TT), **Fixedsys**, **Terminal** (OEM charset),
  **Courier** (raster). Nothing else. All candidate fonts exist (cells: Courier New 10pt 8x16, 9pt 7x15; Lucida Console 10pt
  8x13, 9pt 7x12; Fixedsys 8x15; Terminal 8x12; Courier raster 8x13). CreateFont + select: about 0.05 ms.
- Caret blink 500 ms. `usp10.dll` (Uniscribe) loads; `unicows.dll` is not installed. Code pages 1250/1251/1252/437/850/65001
  valid, 932 not valid (no CJK support installed).

## 1. Full repaint, text view (median ms per repaint, run 1 / run 2)

| Method | Courier New 10 | Courier New 9 | Lucida Con. 10 | Lucida Con. 9 | Fixedsys | Terminal | Courier raster |
|---|---|---|---|---|---|---|---|
| **direct: ExtTextOutA, ETO_OPAQUE per run** | **4.0 / 4.0** | 4.3 / 4.5 | 4.8 / 4.9 | 5.0 / 5.0 | 4.0 / 4.0 | 4.8 / 5.2 | 5.0 / 4.7 |
| direct: one FillRect + transparent runs | 3.1 / 3.8 | 3.3 / 4.0 | 3.9 / 3.9 | 4.6 / 4.2 | 3.4 / 4.3 | 3.8 / 5.9 | 4.1 / 3.9 |
| direct: ExtTextOutW, ETO_OPAQUE per run | 4.3 / 5.0 | 4.6 / 5.0 | 5.1 / 5.2 | 5.6 / 5.5 | 4.2 / 4.9 | 5.3 / 6.5 | 5.9 / 5.0 |
| direct: ExtTextOutA per run + lpDx | 4.4 / 5.0 | 5.0 / 5.0 | 5.0 / 5.0 | 5.4 / 5.1 | 4.0 / 4.8 | 5.1 / 6.4 | 5.7 / 5.0 |
| direct: one ExtTextOutA per line (single colour) | 3.5 / 6.8 | 4.2 / 5.0 | 4.9 / 5.0 | 5.1 / 5.0 | 4.8 / 5.0 | 5.2 / 6.0 | 5.0 / 5.0 |
| direct: one per line + lpDx (single colour) | 3.5 / 6.0 | 4.4 / 4.9 | 4.8 / 4.9 | 4.7 / 4.8 | 4.7 / 5.7 | 5.5 / 5.4 | 4.9 / 5.0 |
| compatible bitmap (DDB), ETO_A runs + 1 BitBlt | 7.0 / 9.6 | 7.5 / 8.2 | 8.0 / 8.0 | 8.0 / 10.0 | 7.5 / 9.0 | 10.0 / 8.1 | 7.9 / 8.4 |
| compatible bitmap, ETO_W runs + BitBlt | 7.1 / 8.3 | 7.8 / 7.9 | 8.0 / 8.5 | 8.7 / 9.7 | 8.0 / 8.0 | 10.0 / 8.6 | 8.0 / 8.8 |
| 16-bpp DIB section (565), ETO_A + BitBlt | 23.5 | 23.9 | 24.4 | 24.9 | 23.4 | 24.2 | 23.9 |
| 16-bpp DIB, wrong format (555) + BitBlt | 24.9 | 25.3 | 25.7 | 26.3 | 24.8 | 25.6 | 25.3 |
| glyph cache: BitBlt per char into DDB + BitBlt | 12.6 / 12.3 | 13.2 / 13.0 | 15.0 | 16.0 | 13.7 / 13.5 | 16.0 | 15.0 |
| glyph cache: own memcpy of cells into DIB16 + BitBlt | 21.6 | 26.5 | 21.7 | 26.7 | 21.6 | 21.7 | 21.7 |
| present only: DDB BitBlt full window | 2.5 / 3.0 | 2.5 / 3.9 | 2.9 / 3.0 | 2.8 / 3.0 | 2.9 / 4.0 | 2.9 / 3.0 | 2.9 / 3.2 |
| present only: DIB16 BitBlt full window | 16.6 | 16.6 | 16.6 | 16.6 | 16.6 | 16.6 | 16.6 |
| empty WM_PAINT (BeginPaint/EndPaint overhead) | 0.03 | 0.03 | 0.03 | 0.03 | 0.03 | 0.03 | 0.03 |

Glyph cache build time (96 glyphs x 40 fg/bg combinations): about 30 ms into a DDB, about 60 ms for the software copy.

Dense text (116 chars/line, 14 runs/line), Courier New 10 / Fixedsys: direct 6.7 / 6.7 ms, DDB + blit 10-14 / 10 ms,
DIB16 26.4 / 26.2 ms, glyph BitBlt 21-22 / 23 ms, glyph memcpy into DIB 19.1 / 19.2 ms.

Proportional reference (GetTextExtentPoint32 per run to advance x, then ExtTextOutA): Tahoma 8pt 5.4 ms direct / 8.7-9.0 ms
DDB; Arial 10pt 5.0 / 8.0; MS Sans Serif 8pt 5.0-5.2 / 8.0-8.7.

### Present / blit characterisation (median ms, 780 px wide, by height)

| Operation | h=16 | h=68 | h=135 | h=270 | h=540 |
|---|---|---|---|---|---|
| BitBlt DDB -> window | 0.35 | 1.0 | 1.0 | 2.0 | 1.9-2.8 |
| BitBlt DIB16 section -> window | 0.51 | 2.1 | 4.2 | 8.3 | 16.6 |
| SetDIBitsToDevice -> window | 0.62 | 2.2 | 4.3 | 8.4 | 16.7 |
| BitBlt DIB16 -> DDB | 0.43 | 1.7 | 3.4 | 6.9 | 13.7 |
| BitBlt window -> window (scroll) | 0.36 | 1.0 | 1.0-1.6 | 2.0 | 1.9-3.0 |
| FillRect window | 0.34 | 1.0 | 1.0-2.0 | 2.0 | 2.5-3.0 |
| FillRect DDB | 0.34 | 1.0 | 2.0 | 2.0 | 3.0 |
| FillRect DIB16 (software) | 0.13 | 0.47 | 0.93 | 1.8 | 3.7 |

DIB-to-screen cost scales with pixel count (about 50 MB/s, a CPU push through the emulated card; half width = half time).
That's an emulator property. Real AGP hardware would be much faster, but the DDB/direct paths stay faster in either case.

## 2. Scrolling (median ms)

| Path (Courier New 10 / Fixedsys) | 1 line, full repaint | 1 line, ScrollWindowEx + paint exposed rows | Page down (full) |
|---|---|---|---|
| direct ETO_A runs | 4.3 / 4.2-4.6 | **2.9-4.0 / 3.0-4.6** | 4.0-4.3 / 4.8-5.1 |
| DDB + blit | 7.3-7.9 / 8.0-8.9 | 4.0 / 4.0 | 7.4-8.0 / 8.3-8.7 |
| glyph memcpy DIB16 + blit | 21.5 | 2.9-3.0 | 21.6 |

Hex view: 1 row ScrollWindowEx 3.0-3.5 ms, page down 7-8 ms (DDB path).

## 3. Hex view (offset + 16 hex bytes + 16 ASCII per row, 6-row selection, ExtTextOutA per attribute run)

| Font (rows) | plain direct | plain DDB+blit | plain DIB16 | byte-coloured direct | byte-coloured DDB | byte-coloured DIB16 |
|---|---|---|---|---|---|---|
| Courier New 10 (34) | 5.0-5.2 | 8.0 | 22.5 | 6.0-6.3 | 9.0 | 25.7 |
| Lucida Console 10 (42) | 5.1-5.5 | 8.9 | 23.1 | 7.1 | 9.5-9.8 | 26.2-26.4 |
| Fixedsys (36) | 5.4-5.6 | 8.0-8.4 | 22.5 | 6.0 | 9.0-9.1 | 25.4 |

Byte-coloured = zero bytes grey, printable black, others blue (up to about 30 runs per row).

## 4. Typing (insert/delete 1 char in a visible line, relex that line, InvalidateRect of the row + UpdateWindow, caret move)

| Path | Courier New 10 | Fixedsys |
|---|---|---|
| direct ETO_A runs | **0.40-0.42 ms** (worst 0.64) | 0.40 ms (worst 0.76) |
| DDB + blit of that row | 0.60-0.65 ms | 0.51-0.57 ms |
| glyph memcpy DIB16 + blit | 0.83 ms | 0.80 ms |

Caret move (HideCaret + SetCaretPos + ShowCaret): 38 us. Relex of one ~50-char line with the toy lexer: 15 us.

## 5. ExtTextOut call cost by run length (us per call, window DC, synced; from FACTS.TXT)

| Courier New 10pt | 1 | 4 | 8 | 16 | 32 | 64 | 128 chars |
|---|---|---|---|---|---|---|---|
| W, no flags | 12.9 | 12.5 | 18.4 | 29.2 | 53.4 | 133 | 123 |
| W, ETO_OPAQUE | 9.7 | 13.2 | 19.3 | 27.4 | 46.6 | 76.9 | 122 |
| W, OPAQUE + CLIPPED | 9.9 | 13.2 | 17.3 | 27.8 | 45.3 | 78.9 | 124 |
| W, OPAQUE + lpDx | 10.0 | 13.5 | 18.4 | 27.7 | 46.6 | 83.2 | 128 |
| A, ETO_OPAQUE | 8.4 | 11.9 | 16.1 | 27.0 | 41.0 | 76.6 | 130 |
| A, OPAQUE + lpDx | 8.7 | 11.6 | 16.1 | 26.0 | 46.6 | 86.6 | 137 |

Fixedsys is similar for short runs (7-9 us per call), slightly slower per char (1.3-1.5 us). Rule of thumb on the
emulator: **about 8 us per call + about 0.9 us per char**. ETO_CLIPPED and lpDx are free; W costs at most 5-10% more than A.
`SelectObject` font switch 2 us, `SetTextColor` / `SetBkColor` 1.6 us each, so per-run attribute changes are cheap.

## 6. Text measuring

| Call | Courier New 10 (TT) | Fixedsys (raster) | Tahoma 8 / Arial 10 (TT) |
|---|---|---|---|
| **GetCharWidth32A** (any range) | **STUB: returns 0, ERROR_CALL_NOT_IMPLEMENTED** | stub | stub |
| GetCharWidthA, 224-char table, per call | 25.7 us | 9.4 us | 25.7 us |
| GetCharABCWidthsA, 224-char table | 42.8 us | fails (TrueType only) | 40-43 us |
| GetTextExtentPoint32A, 1 char x1000 | 3.3 ms | 2.1 ms | 3.3 ms |
| GetTextExtentPoint32A, 10-char runs, per 1000 chars | 0.45 ms | 0.23 ms | 0.45-0.47 ms |
| GetTextExtentPoint32A, one 1000-char call | 0.11 ms | 0.015 ms | 0.10-0.11 ms |
| GetTextExtentPoint32W, 10-char runs, per 1000 chars | 0.59-0.61 ms | 0.36 ms | 0.59-0.62 ms |
| **GetTextExtentExPointA, 1000 chars + dx array** | **50-57 ms** | 7.8 ms | 50-57 ms |

## 7. File I/O (64 KB chunks unless noted)

| Test | C: (local FAT32, emulated IDE) | M: (Samba over SLiRP) |
|---|---|---|
| Write 10 / 50 MB (+FlushFileBuffers) | **6.2 MB/s** (1.6 s / 8.0 s) | **0.40 MB/s** (25 s / 125 s) |
| Read, first time | 210-215 MB/s (served by VCACHE: the files we just wrote are cached) | **1.05-1.1 MB/s** uncached |
| Read again | 212-218 MB/s | 1.1 MB/s, or 2-2.6 MB/s, or up to 264 MB/s when the redirector happened to cache it (unpredictable) |
| Read + count lines (memchr) | 63 MB/s | as uncached read |
| FILE_FLAG_NO_BUFFERING | accepted, no effect (same numbers) | accepted |
| Chunk size 4 / 16 / 64 / 256 KB | **104 / 181 / 216 / 225** MB/s | fresh file: 1.05 / 1.10 / 1.10 / 1.10 MB/s (link-bound) |
| 100 random 64 KB reads | 0.30 ms each (cached) | 0.25 ms each (cached by then) |
| Create 200 files of 2-8 KB | 0.47 ms/file | 6.3 ms/file |
| FindFirstFile/FindNextFile over 202 entries | 6.4 ms | 120-139 ms |
| Find-in-files: enum + open + read + scan + close | 0.60 ms/file first, 0.25 cached | **5.5-5.7 ms/file** first, 2.6 cached |
| CreateFile + CloseHandle only / GetFileAttributes | 0.12 / 0.06 ms | 1.85 / 1.03 ms |

The "cold disk" speed of C: could not be isolated: VCACHE keeps the test files and NO_BUFFERING is ignored. That doesn't
matter much on the emulator (the host caches the VHD anyway).

**Load whole file + scan in memory (C:)**

| | 10 MB | 50 MB |
|---|---|---|
| 64 KB reads into a fresh VirtualAlloc buffer (first touch) | 129 ms (run 1), 1249 ms (run 2) | **3.1 s / 8.6 s** |
| Single ReadFile call into the same buffer | 42 ms | 0.41 s / 5.1 s |
| Count lines, memchr | 113 ms (88 MB/s) | 563-568 ms |
| Count lines, byte loop | 137 ms (73 MB/s) | 685-691 ms |
| Build line-start index (DWORD per line) | 121-126 ms (0.9 MB index) | 607 ms (4.5 MB index, 1.16 M lines) |

With the 50 MB buffer, free physical memory dropped to **4 MB of 127**: the 128 MB machine starts paging. Load times swing
by 3-10x depending on what VCACHE and other programs hold.

**Memory-mapped files**

| | C: 50 MB | M: 50 MB |
|---|---|---|
| CreateFile + CreateFileMapping + MapViewOfFile (whole file) | 6-7 ms | 10-11 ms (works on the share) |
| First scan of the view (page faults read the file) | 1.2-5.7 s (9-41 MB/s) | 47.5 s (1.05 MB/s) |
| Scan again | 88 MB/s | 88 MB/s |
| 50 sliding 1 MB views (map, scan, unmap) | 70-73 MB/s | 73 MB/s (cached by then) |
| MapViewOfFile + UnmapViewOfFile of a 1 MB view, no touch | **2.3-2.4 ms each** | 2.3 ms |

Pagefile-backed mappings (`CreateFileMapping(INVALID_HANDLE_VALUE)` + full view): 64/128/256/512/**768 MB OK**,
1024 and 1536 MB fail (ERROR_NOT_ENOUGH_MEMORY): views come from the 9x shared arena (about 1 GB, all processes share it).

## 8. Memory

| Test | Result |
|---|---|
| Largest VirtualAlloc MEM_RESERVE | 2027 MB |
| Largest VirtualAlloc MEM_COMMIT | 2027 MB (Win98 lets you commit it; pages are backed only when touched) |
| Largest HeapAlloc(GetProcessHeap()) | 255 MB |
| Largest malloc (VC6 CRT) | **128 MB** |
| Commit + first touch 32 MB (memset) | 350 ms (91 MB/s) |
| memset 32 MB again | 145 ms (220 MB/s) |
| memmove 32 MB by 1 byte (gap-buffer worst case) | 218 ms |
| 200k mallocs of 40-71 bytes / freeing them | 310 ms (1.55 us each) / 104 ms |

Side effects seen: (a) after the HeapAlloc/malloc binary searches the process had only **104 MB of free address space** left
(Win9x heaps don't give big blocks' address space back); (b) between run 1 and run 2, C: free space fell from 2537 MB to
553 MB and later recovered (2561 MB; WIN386.SWP now 12 MB). The 2 GB MEM_COMMIT probe very likely made Win98 grow the swap
file for a while. Don't ship code that commits huge amounts "just in case".

## 9. Platform facts (FACTS.TXT, CHUNKS.TXT, bomtest/)

**API availability** (GetProcAddress finds every one; the question is whether it works):

| API | Result on Win98 SE |
|---|---|
| GetCharABCWidthsW | **stub** (ERROR_CALL_NOT_IMPLEMENTED). GetCharABCWidthsA works (TrueType only) |
| CharLowerW / CharUpperW | **stub**: string unchanged, ERROR_CALL_NOT_IMPLEMENTED. Need our own case tables |
| GetCharWidth32W (and GetCharWidth32A) | **stub**. **GetCharWidthW works** (width('M') = 8), as does GetCharWidthA |
| GetTextMetricsW | **stub** (use GetTextMetricsA) |
| GetTextExtentExPointW | **stub** (GetTextExtentPoint32W and ExtTextOutW do work) |
| DrawTextW, CompareStringW | **stub** |
| CreateFileW | stub (sets ERROR_CALL_NOT_IMPLEMENTED; documented as unsupported on 9x. The probe mis-scored it as "works" because the stub doesn't return INVALID_HANDLE_VALUE) |
| IsTextUnicode (advapi32) | **stub** (returns 0, err 120): write our own encoding detection |
| GetDiskFreeSpaceExA | works (C: 2561 MB free of 3988 MB) |
| GetFileAttributesExA | works |
| SHGetSpecialFolderPathA (shell32) | works: CSIDL_APPDATA = `C:\WINDOWS\Application Data`, CSIDL_PERSONAL = `C:\My Documents` |
| SHGetFolderPathA (shfolder.dll) | present and works |
| usp10.dll | loads (not needed) |
| unicows.dll | not installed |
| MultiByteToWideChar / WideCharToMultiByte CP_UTF8 | work (round trip ok). Overlong `C0 AF` decodes to `/` without complaint; `MB_ERR_INVALID_CHARS` with CP_UTF8 fails with ERROR_INVALID_FLAGS (1004). Validate UTF-8 ourselves |

**Charsets** (`shot_charsets.png`): `ExtTextOutW` with Courier New shows Cyrillic ("Привет, мир! ЖДЯ") and Polish
("Zażółć gęślą jaźń Łódź") correctly **whatever the lfCharSet**: ANSI_CHARSET, RUSSIAN_CHARSET and EASTEUROPE_CHARSET all
give 6/6 real, distinct glyphs (glyph-bitmap check against '?', blank and an unassigned Thai default). `GetTextCharset`
reports the requested set (0/204/238). `ExtTextOutA` with cp1251/cp1250 bytes renders correctly only with the matching
RUSSIAN/EASTEUROPE font; with ANSI_CHARSET the bytes show as cp1252 glyphs. Conclusion: keep text as UTF-16 and draw with
ExtTextOutW. Charset fonts aren't needed for display (raster Fixedsys/Terminal only carry their own code page; not tested).

**File semantics**

| Test | C: (FAT32) | M: (Samba) |
|---|---|---|
| MoveFile of a file we hold open (share READ or READ\|WRITE) | fails, ERROR_ACCESS_DENIED (5) | fails, ERROR_SHARING_VIOLATION (32) |
| CreateFile with FILE_SHARE_DELETE (4) | **ERROR_INVALID_PARAMETER**: never pass it on 9x | same |
| DeleteFile while our own handle holds it (deny write) | fails, ERROR_ACCESS_DENIED, file stays | same |
| DeleteFile while **another process** holds it (deny write) | **fails**, ERROR_ACCESS_DENIED, file stays | same |
| Open for write while another process holds it deny-write | ERROR_SHARING_VIOLATION | same |
| MoveFile while another process holds it | ERROR_ACCESS_DENIED | ERROR_SHARING_VIOLATION |
| FindFirstChangeNotification(LAST_WRITE\|SIZE), file rewritten from inside the VM | **signalled** (rewrite with new size, and a same-size overwrite in place) | **signalled** |
| ... FILE_NAME filter, new file created in the VM | signalled | signalled |
| ... file edited **on the Mac side** (server-side change) | n/a | **never signalled** (waited 15 s); polling GetFileAttributesEx (every 250 ms) saw the new size/time right away (about 3.3 s after the probe raised its flag; the Mac script edits 3 s after seeing it) |

**Byte loops on a 1 MB buffer (best of 5)**

| Loop | MB/s |
|---|---|
| LF count, byte loop | 74 |
| LF count, memchr | 90 |
| Horspool, 11-char pattern absent / 10-char frequent / case-insensitive (fold table) | 328 / 257 / 177 |
| Horspool, 2-char pattern "if" | 60 |
| memchr first char + memcmp, absent | 119 |
| UTF-8 validation (own loop), pure ASCII / 3% 2-byte | 37 / 35 (unoptimised; an ASCII fast path would help) |
| MultiByteToWideChar CP_UTF8 / CP_ACP | **8.7** / 37 |
| WideCharToMultiByte CP_UTF8 / CP_ACP | 14 / 13 |
| CharLowerBuffA (in place) | **5.2** (slow: use our own 256-byte fold table) |

**VC6 toolchain vs UTF-8 BOM** (`bomtest/`):

| Tool | File starting with EF BB BF |
|---|---|
| `cl` (.cpp) | **fails**: `error C2018: unknown character '0xef'` (plus 0xbb, 0xbf); the same file without BOM compiles and runs |
| `rc` (.rc) | **fails**: `RC2135: file not found` errors (the first keyword is mangled); without BOM it builds |
| `nmake` (makefile) | **mangles the first token**: a first-line target `all` can't be named (`U1073 don't know how to make 'all'`; it only builds when it's the default target), and a first-line macro `X = hello` is undefined |
| command.com (.bat) | first line fails ("Bad command or file name"), the rest runs |

So the project's source files must stay BOM-less ANSI/ASCII, and the editor needs a "UTF-8 without BOM" save option (and must
not add a BOM silently).

## 10. Recommendations for the editor

**Renderer**
1. **Draw directly to the window DC in WM_PAINT with ExtTextOutW, one call per colour run, ETO_OPAQUE, using the run's cell
   rectangle**, then one ETO_OPAQUE call to clear the rest of the row. Every pixel is painted exactly once, so there is no flicker
   without a back buffer. Measured 4-5 ms per full 34-row repaint (6.7 ms for dense 14-runs/line text) on the emulated PII-233.
   ExtTextOutW is 5-10% slower than A, which is a small price for Unicode and the Cyrillic/Polish support verified above. Position
   by `x = gutter + col * cellWidth`. lpDx isn't needed for a fixed-pitch font (it costs nothing if we ever want it for
   tab/wide-char alignment).
2. **Back buffer only if overlapping layers are needed** (e.g. squiggles drawn over text, a translucent selection): use one
   window-sized **compatible bitmap (DDB)**, never a DIB section. It costs about +3-4 ms per full repaint (the full blit is
   2.5-3 ms). DIB-section paths (24 ms), per-glyph BitBlt caches (12-16 ms) and our own memcpy glyph renderer (19-22 ms) are all
   slower on this emulator, because DIB-to-screen moves only about 50 MB/s here. Don't build a custom glyph renderer.
3. **Font**: default **Courier New 10pt** (8x16 cell, TrueType, full WGL4 coverage via ExtTextOutW). Offer Lucida Console 10pt
   (8x13: 42 rows instead of 34) and Fixedsys (raster, ANSI only). Speed differs by at most about 1 ms between fonts, so choose
   by looks and coverage. No antialiasing exists on Win98, so text is crisp 2-colour in every case.
4. **Scrolling**: ScrollWindowEx by the line delta + paint only the exposed rows (3-4 ms vs 4.3 ms full; the screen-to-screen
   blit costs about 2-3 ms on the emulator, so the gain is small here but larger on real hardware and it avoids tearing). Full
   repaint for page jumps (4-5 ms). Holding PageDown at keyboard repeat (about 30/s) costs well under 10% of the CPU.
5. **60 Hz full repaint is affordable** (4-8 ms of a 16.7 ms frame even on the emulator), but not needed: repaint only
   invalidated rows. Per-keystroke work measured at **0.4 ms** (insert, relex line, repaint row, caret). Budget: **keep a
   keystroke under about 5 ms total on the emulator** (buffer edit, undo record, incremental lex with state carry-over, row
   repaint); that leaves lots of headroom. Relexing many lines after an opening `/*` should be lazy (visible area first).
6. **Text measurement**: build a per-font width table once with **GetCharWidthW/GetCharWidthA** (26 us per 224 chars).
   **Never use GetCharWidth32*** (stub), GetTextExtentExPoint (50-57 ms per 1000 chars on TrueType!), GetTextExtentExPointW,
   GetCharABCWidthsW or GetTextMetricsW (stubs). For the fixed-pitch grid, column math is enough; call GetCharWidthW lazily per
   256-char block for non-ASCII to detect missing/odd-width glyphs.
7. **Hex view**: same direct ExtTextOutA/W per attribute run. 5 ms plain, 6-7 ms with per-byte colouring, 3 ms per row
   scroll. No special renderer needed.
8. Use `CS_OWNDC` (font and colours persist; BeginPaint overhead 0.03 ms) and avoid `CharLowerBuffA` / `CharLowerW` (slow / stub):
   use our own case-fold tables for search.

**Big files / text model**
- Disk reads that VCACHE serves run at 200+ MB/s; real line scanning runs at about 88 MB/s (memchr), so building a line index
  costs about 12 ms per MB on the emulator (10 MB about 0.12 s, 50 MB about 0.6 s). On the 1.5 GHz ThinkPads expect several
  times faster.
- **RAM is the limit, not CPU.** On the 128 MB VM, 50 MB in memory pushed free physical RAM to 4 MB and load times varied
  3-10x (paging). Plan: **fully in memory (piece table: original buffer + add buffer + line index) comfortably up to about
  16 MB, acceptable to about 32 MB**. Above that, or above about 25% of `GlobalMemoryStatus.dwTotalPhys`, **switch to read-on-demand**:
  pieces refer to file offsets, a background memchr scan builds the line index (4 bytes per line: a 50 MB C file has 1.16 M
  lines, 4.5 MB index), and a small LRU cache of 64 KB blocks is filled with ReadFile (64 KB chunks: 4 KB reads are half speed).
  The hex view is on-demand by nature (read 64 KB around the view).
- Memory-mapped files work (also on M:), but each MapViewOfFile+Unmap costs 2.3 ms and views come out of the shared
  1 GB arena (768 MB max seen), so whole-file views of huge files are fragile. Use ReadFile into our own cache. Never map files on
  M: (page faults at 1 MB/s; the file can change on the server).
- Allocate big buffers with **VirtualAlloc/VirtualFree**, not malloc/HeapAlloc (CRT malloc tops out at 128 MB, and heap
  address space wasn't returned). Small allocations are fine (1.55 us), but a piece table avoids per-line blocks. A gap buffer
  would cost about 7 ms per MB moved on a far jump (memmove 32 MB = 218 ms); piece table preferred.
- **Network share M:** reads about 1.1 MB/s, writes 0.4 MB/s (a 10 MB save takes about 25 s), 5.5 ms per file for find-in-files.
  Saving must show progress and run in a worker thread. Write to a temp file, close all handles, then rename (MoveFile of an open
  file fails; FILE_SHARE_DELETE doesn't exist on 9x). Find in files over M: should run in a background thread with
  progress/cancel.
- **External change detection**: FindFirstChangeNotification works on C: and for changes made inside the VM on M:, but **never
  fires for server-side (Mac) edits on M:**. So also poll size + last-write time (GetFileAttributesEx, 1 ms on M:) for open
  files on WM_ACTIVATEAPP / a slow timer.
- Encodings: implement our own UTF-8 validation and BOM/UTF-16 detection (IsTextUnicode is a stub, and MultiByteToWideChar
  accepts overlong UTF-8). MultiByteToWideChar(CP_UTF8) is slow (8.7 MB/s): for big UTF-8 files decode lazily per block, not
  the whole file at load.

## Caveats

- These are 86Box numbers. CPU-bound parts (scans, lexing) should be several times faster on the 1.5 GHz ThinkPads. GDI
  speed there depends entirely on the display driver: if the X41 runs an unaccelerated VBE/VESA-style driver, all GDI drawing
  becomes software into uncached VRAM and the ranking of the paths could change (a DDB back buffer in system RAM might then win).
  **Re-run `edprobe -render` on the X41/T40 before tuning further** (a few minutes; window visible, `BENCH.TXT` next to the exe).
- C: read numbers come from VCACHE. Real cold-disk numbers weren't measurable here.
- M: caching by the Win98 redirector is inconsistent between runs (the same read took 1.1, 2.0 or 264 MB/s).
