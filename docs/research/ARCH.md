# Editor architecture research (Win98 SE text + hex editor, VC6, all own code)

Date 2026-11-09. Desk research only; no VM was used. Fact tags:
**[WEB]** verified at the cited URL; **[MEM]** my recollection, NOT verified (confirm with a probe or a
`GetProcAddress` check); **[PROBE]** must be measured on VM A / the X41 (see section 12).
Sizes are rough line counts of VC6-dialect C++ (no tests).

## 0. Bottom line

| Topic | Recommendation |
|---|---|
| Buffer | **Byte-oriented piece table in a balanced tree (treap with split/merge)**, nodes in a pool, each node carrying byte length + line-break count, subtree sums. No piece longer than 64 KB, so finding a line scans at most 64 KB. The same bytes serve the hex view (raw) and the text view (decoded on the fly). |
| Big files | Two source modes. **In-memory** (whole file read into one heap block, file handle closed) up to ~16-32 MB or 1/4 of RAM. **On-demand** above that: file stays open deny-write, 64 KB block cache (~4 MB LRU), background indexer. **No memory-mapped files** (the Win9x shared arena and the DeleteFile-of-mapped-file data-loss risk, see 2.2). Max doc size 4 GB - 2 (32-bit unsigned offsets). |
| Save | Write a temp file in the same folder, then `MoveFile` original -> backup name, `MoveFile` temp -> original, roll back on failure. `MoveFileEx` and `ReplaceFile` are not available on 98. |
| Unicode | **ANSI windows** (RegisterClassA/CreateWindowExA/DefWindowProcA), **raw bytes in the buffer**, decoders to code points in the core, **draw with ExtTextOutW** (native on 9x). Per-charset font handles for fallback. No MSLU, no Uniscribe in 1.0. Own UTF-8/UTF-16 codecs; OS (MultiByteToWideChar) only for ANSI/OEM/DBCS code page tables. Invalid bytes survive untouched because unmodified bytes are never re-encoded. |
| Rendering | Monospace-first layout (cell widths 0/1/2) behind a width interface. One `ExtTextOutW(ETO_OPAQUE)` per colour run, clipped to the visible columns; per-line band buffer (or full back buffer, pick by benchmark); `ScrollWindowEx` for scrolling; line-granular invalidation. Respect the 9x limits: 8192 chars per text call, 16-bit coordinates. |
| Wrap | View anchored at (logical line, sub-row); per-line wrap results cached only for lines near the viewport; scrollbar in logical lines (scaled to 0..32767). |
| Undo | Undo log of primitive byte ops (insert/delete) that reference piece-table spans (no text copies), with groups, a sequence number save point and one shared history for hex and text edits. |
| Syntax | Incremental line-state lexer (re-lex from the edited line until the state converges). One **data-driven generic lexer** (INI-style language files) covers most languages; hand-coded lexers only for HTML/XML (+embedded JS/CSS), Markdown and maybe Python. Around 12 languages in 1.0. |
| Regex | Own engine: parser -> one program -> **Pike VM** (linear time, leftmost-first, lazy quantifiers, captures) for patterns without backrefs/lookahead, plus a **step-limited backtracker** on a windowed flat copy for patterns with them. Literal prefilter (Horspool) does most of the work in find-in-files. |
| Hex | Same document and same selection (byte offsets). Offset/hex/ASCII columns, 16 bytes per row at 800x600, nibble-level insert and overwrite, go to offset, hex/text byte search, data inspector. |
| Robustness | Periodic snapshot backups + session file (temp + rename), unclean-exit flag, FindFirstChangeNotification **plus polling** (Samba `M:` will not notify reliably), deny-write handle in on-demand mode, chunked handling of multi-MB lines, NUL and control pictures. |
| Code | Portable core (`core/`, no `windows.h`, C++98 subset that VC6 accepts) tested with clang `-std=c++98 -pedantic -Werror` + ASan/UBSan on the Mac and with VC6 in Win98; thin Win32 layer (`win/`). About 20-25k lines total. |

## 1. Text buffer

### 1.1 Options

| | Gap buffer | Piece table (flat array of pieces) | Piece tree (balanced tree of pieces) | Rope (tree of text chunks) |
|---|---|---|---|---|
| Idea | One array with a hole at the cursor (Emacs) | Original (read-only) + add buffer (append-only) + a list of pieces | Same, with pieces in a balanced tree, subtree sums (VS Code) | Text itself stored in tree leaves (~1-4 KB chunks) |
| Load 100 MB | Must copy everything into the array (100 MB RAM) | Zero copy: one piece over the original | Zero copy | Must copy all text into leaves |
| Edit cost | O(gap move): moving the cursor 50 MB away memmoves 50 MB (~1 s on a P2) | O(number of pieces) array insert | O(log n) | O(log n) |
| Undo | Separate copies of text | Natural: pieces reference old text, nothing is ever overwritten | Natural | Needs copies or persistent nodes |
| Lazy file loading | No | Yes (original can be a file on disk) | Yes | No |
| Hex view | Fine | Fine | Fine | Fine |
| Complexity | Very low (~300 lines) | Low (~600) | Medium (~1500-2000) | Medium |

The VS Code team moved from a line array to a piece tree. Their old line array used about 600 MB for a
35 MB, 13.7M-line file (about 20x the file size); after the move, memory was "very close to the original
file size". [WEB https://code.visualstudio.com/blogs/2018/03/23/text-buffer-reimplementation]
They chose a red-black tree whose nodes cache the left subtree's length and line-break count, and their
buffers keep line-start arrays. They also found CRLF across node boundaries "a programmer's nightmare"
(we avoid most of that, see 1.3).

**Recommendation: piece tree.** It is the only option that gives zero-copy loading (needed for 100 MB in
128 MB RAM), O(log n) edits anywhere and free undo. The extra complexity over a flat piece array pays off
after the first few thousand pieces: a 100 MB file starts as ~1600 pieces (64 KB cap), and replace-all
creates thousands more.

### 1.2 Tree choice

- **Treap with split/merge** (recommended). Every operation is "split at offset A, split at offset B,
  merge". Insert and delete are the same few lines, and invariants are easy to test (heap order,
  BST order, sums). Expected O(log n), and a deterministic seeded PRNG keeps tests reproducible. About 400 lines.
- Red-black / AVL: worst-case O(log n), but augmented delete-rebalance code is error-prone. Use this only if
  treap depth turns out to be a problem (it won't at these sizes).
- B+tree of piece arrays (e.g. 32 pieces per leaf): better cache behaviour on the P2 (16 KB L1,
  half-speed L2) and fewer nodes, but about twice the code. Keep it as an optimisation behind the same interface.

Node (~36 bytes): `u8 src; u32 start, len, nbreaks; u32 sum_len, sum_breaks; u32 left, right, prio;`
Use **32-bit indices into a node pool** rather than pointers (the pool can grow with realloc, nodes are cheap
to allocate on the slow Win9x heap, and they can be dumped for debugging). Free nodes go on a free list.

### 1.3 Line indexing

- Each piece stores its number of **break units**. Subtree sums let us map line <-> offset in O(log n) down to
  a piece, then scan inside that piece. The **64 KB piece cap** bounds the scan to 64 KB (roughly 0.2-0.6 ms on a P2-233
  **[PROBE]**). Add a small LRU cache (e.g. 8 entries) of "line starts inside block X" arrays for hot blocks, so
  caret up/down and painting don't rescan. This costs about 50 KB of nodes for a 100 MB file instead of VS Code's
  per-buffer line-start arrays (4 bytes per line = 8 MB for 2M lines).
- **Break unit per document** (simplification, decided for 1.0): LF for LF and CRLF documents; CR for CR-only
  (classic Mac) documents. CR before LF is part of the EOL and hidden in rendering. A lone CR in an LF/CRLF
  document is not a line break; it is drawn as a `CR` control picture. This removes the CRLF-split-across-pieces
  problem entirely: counting is a single byte compare and a piece's count never depends on its neighbours.
  Trade-off: Notepad++ breaks lines on all three forms, so a file with mixed CR-only lines looks different.
  EOL conversion (Edit > EOL) rewrites the document anyway.
- **UTF-16:** the break unit is the 2-byte code unit 0x000A (or 0x000D) at an even offset from the
  text origin. Pieces are split only at even lengths, and hex insert/delete in a UTF-16 document is
  restricted to multiples of 2 bytes (or asks to reinterpret the document as binary). Otherwise piece-local
  counts would depend on the absolute parity. Hex overwrite of single bytes is always allowed.

### 1.4 One buffer for the text view and the hex view

The buffer stores the **file's bytes exactly** (including any BOM); there is no conversion on load or
save. The hex view reads bytes directly. The text view goes through a `Codec` (section 3) that
decodes bytes to code points starting at `text_origin` (the BOM length). Consequences:

- Lossless: bytes you did not touch are written back as they were, even if invalid in the encoding.
- Hex and text edits are the same primitive operation (insert/delete bytes), so there is one undo history and
  one modified flag.
- "Modified bytes" highlighting in the hex view is free: bytes coming from the ADD source are modified.
- Text operations (caret movement, lexing, search) need encoding-aware iteration. That is the real cost of
  this design; see 3.6 for fast paths.

The alternative is to convert to an internal UTF-8 or UTF-16 copy, as Scintilla/Notepad++ do with UTF-8 for
Unicode files. It makes text code simpler, but the hex view would show converted bytes rather than the file,
memory doubles for UTF-16 files, and invalid bytes need an escape scheme. That is rejected for a hex-capable editor.

### 1.5 Memory budget, 100 MB file in 128 MB RAM

Win98 plus 86Box guest overhead leaves very roughly 80-100 MB of usable RAM **[MEM]**. Reading 100 MB into the heap
works (Win98 grows the swap file) but pages heavily, so above the threshold use on-demand mode:

| Item | On-demand mode, 100 MB, ~2M lines |
|---|---|
| Piece nodes (1600 x 36 B) | 58 KB |
| Block cache (64 x 64 KB) | 4 MB (configurable) |
| Line-start cache (8 blocks) | ~100 KB |
| Lexer line states (if enabled; 2 B/line) | 4 MB (colouring is off by default above 8 MB, see 7) |
| Add buffer | grows with edits (a few KB to MB) |
| Undo log | tens of bytes per edit |
| **Total** | **~5-9 MB** |

## 2. Large files

### 2.1 Loading strategies

| Strategy | Pros | Cons on Win98 |
|---|---|---|
| Whole read into heap | Simplest; file handle closed afterwards (no locking); fastest random access | RAM = file size; 100 MB on a 128 MB machine thrashes |
| Memory-mapped file | OS paging, no explicit cache | 9x specifics below make it risky |
| On-demand reads + own block cache | Bounded RAM; works on any drive including Samba `M:`; predictable failure modes | More code (~400 lines); we must keep the file from changing under us |

### 2.2 Win9x file mapping specifics (why not MMF)

- Views of file mappings are placed in the **shared arena at 2-3 GB, visible to all processes**; private memory is
  4 MB-2 GB. [WEB https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/win95/chilimit_3gvn.htm,
  https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/125/691.HTM]
- A single view is limited to the **largest contiguous free block of that shared arena, at most 1 GB minus what
  other components (16-bit apps, other processes' views) use, and the limit is system-wide**.
  [WEB https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/125/713.HTM]
- `dwMaximumSizeHigh`/`dwOffsetHigh` are ignored (32-bit only); SEC_IMAGE/SEC_NOCACHE are unsupported; copy-on-write needs
  PAGE_WRITECOPY, and other processes can **see** each other's copy-on-write changes (no private copy, unlike NT).
  [same KB 125713]
- Views are **coherent only if every access goes through views**; ReadFile/WriteFile on the same file are not
  guaranteed coherent. [WEB chilimit_3gvn above]
- **"DeleteFile removes files that are open or memory-mapped, which can cause data loss"** (NT refuses). [WEB chilimit_3gvn above]
  This also applies to plain open handles, so on-demand mode carries the same risk (2.5).
- `VirtualProtect` does not work on memory in the shared region. [WEB https://techshelps.github.io/MSDN/KB/Source/win32sdk/q170426.htm]
- Mapping files on network redirectors (our Samba `M:`) on 9x: **not verified [MEM: works but slow and fragile]**.

Verdict: **no MMF** for document storage. The only advantage (no explicit cache) is small, and the failure modes are
global and opaque (another process can exhaust the arena).

### 2.3 Thresholds and modes

- `size <= T_mem` (default `min(32 MB, physRAM/4)` from `GlobalMemoryStatus`): **in-memory**. Read the whole file
  in 64 KB ReadFile chunks into one heap block (or a VirtualAlloc reservation), close the handle, then split it into
  64 KB pieces while counting breaks (same pass). Files on `M:` behave like local files, and other programs can
  overwrite the file freely (we detect it, section 10).
- `T_mem < size < 4 GB`: **on-demand**. Open with `GENERIC_READ`, `FILE_SHARE_READ` only (deny write) so the
  original cannot change under the piece table. A **background indexer thread** opens its own handle, reads
  sequentially, counts breaks per 64 KB block, sniffs the encoding and finds the longest line, then posts the
  piece list to the UI. Meanwhile the **hex view works immediately** (it needs no line index), and the text view shows
  the first screen. Editing, go-to-line and the line-number scrollbar are enabled once indexing completes
  (status bar "indexing 43%"). Note: Win9x has **no asynchronous file I/O except on serial ports**, so use a thread
  with synchronous ReadFile, not overlapped I/O. [WEB chilimit_3gvn]
- `size >= 4 GB` (only possible on network shares; FAT32 files max out at 4 GB - 1 byte
  [WEB: several search results agree on 2^32-1 bytes; one old post says 2^32-2, so test the boundary]): refuse with a clear
  message. Whether Win98's SMB redirector can even address beyond 2 GB is **unverified [MEM]**.
- If the on-demand open fails with a sharing violation (another process is writing the file): open
  `FILE_SHARE_READ|FILE_SHARE_WRITE` as **read-only with a "file may change" banner**, and re-check the size and time
  before each block read; editing is disabled.
- Offsets: `typedef unsigned long epos;` (32-bit) everywhere, with max document size `0xFFFFFFFE` and no signed
  offset arithmetic. Always call `SetFilePointer(h, (LONG)lo, &hi /* = 0 */, FILE_BEGIN)` with a non-NULL
  high-dword pointer: with `NULL`, the low value is treated as signed and offsets 2-4 GB are unreachable; with
  a high part, the 64-bit value is used and the low dword counts as unsigned (check errors with
  `GetLastError`, since 0xFFFFFFFF is a valid low dword). **[MEM: verify 2-4 GB reads on FAT32 with a probe file]**

### 2.4 Encoding conversion and huge documents

In-buffer encoding conversion of a 100 MB on-demand document would create a 100-200 MB add buffer. In on-demand
mode, "Convert to UTF-8" etc. therefore becomes a **streaming save-as**: write the converted file, then reopen it.
In in-memory mode it is a whole-document replace (one undo group).

### 2.5 Saving safely

`ReplaceFile` needs Windows 2000+ [WEB https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilea];
`MoveFileEx` is **not available on Windows 95/98** (only the WININIT.INI `[rename]` reboot mechanism exists)
[WEB https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/filesio_9oe0.htm]. Sequence:

1. Check free space (`GetDiskFreeSpaceEx`, via GetProcAddress: **95 OSR2+ [MEM]**; fall back to `GetDiskFreeSpace`).
2. Write the document bytes from the piece tree in 64 KB chunks to `<dir>\~ED<nnnn>.TMP` (8.3-safe name, same
   directory, so the rename is not a cross-volume copy). `FlushFileBuffers`, `CloseHandle`, verify the size.
3. Copy attributes (archive/hidden/system) and the creation time (`SetFileTime`) from the original.
4. On-demand mode: close our handle to the original. Renaming an open file on 9x most likely fails with a
   sharing violation **[MEM, probe]**.
5. `MoveFile(orig, <dir>\~ED<nnnn>.BAK)`. If that fails (another program holds the file open): in **in-memory mode**
   fall back to `CopyFile(orig, bak)`, then overwrite the original in place (CREATE_ALWAYS + write). In on-demand
   mode, report the error and keep the temp file.
6. `MoveFile(tmp, orig)`; on failure `MoveFile(bak, orig)` (roll back) and report.
7. Backup handling: if "keep backup" is on, rename `.BAK` to `name.ext.bak`. In in-memory mode, delete it otherwise.
   In **on-demand mode, reopen the .BAK (deny write) and point the ORIGINAL source at it**: its bytes equal the old
   original, so every piece and undo record stays valid without rebasing. Delete it when the tab closes.
   (In in-memory mode the original is a heap block, so nothing needs rebasing either.)
8. Record the new size and time for change detection (section 10).

Side effects to accept: the 8.3 alias can change (LONGFI~1 -> LONGFI~2); FAT timestamps have 2 s resolution
[WEB chilimit_3gvn].

## 3. Encodings and Unicode on Win98

### 3.1 Which wide APIs really work on Win9x without MSLU

| API | Status on 95/98 | Source |
|---|---|---|
| `TextOutW`, `ExtTextOutW` | **Native** | [WEB KB 210341 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/210/341.HTM, KB 125671 https://techshelps.github.io/MSDN/KB/Source/win32sdk/q125671.htm] |
| `GetTextExtentPoint32W`, `GetTextExtentPointW` | **Native** | same |
| `GetTextExtentExPointW` | **Native** (useful for hit testing / fitting) | KB 125671 |
| `GetCharWidthW` | **Native** | same |
| `GetCharWidth32W`, `GetTextMetricsW`, `GetOutlineTextMetricsW`, `GetGlyphOutlineW` | **Stubs** on 9x (need unicows) | [WEB Emacs src/w32font.c comment and code, https://github.com/emacs-mirror/emacs/blob/master/src/w32font.c] |
| `GetCharABCWidthsW` | Not in any native list; **assume stub [MEM]**. Use `GetCharABCWidthsA` (TrueType only) if needed | - |
| `MultiByteToWideChar` / `WideCharToMultiByte` | **Native**; `CP_UTF8` supported on 98/Me and NT4+; for `CP_UTF8`, WideCharToMultiByte needs dwFlags 0 and NULL default-char args | [WEB https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar]; invalid-input behaviour varies by OS version (drops or fails): [WEB https://devblogs.microsoft.com/oldnewthing/20120504-00/?p=7703] |
| `lstrlenW`, `MessageBoxW`, `MessageBoxExW`, `GetCommandLineW`, resource W functions | Native; `lstrcpyW`, `lstrcatW` added in 98 | KB 210341 |
| Clipboard `CF_UNICODETEXT` | **Available on Windows 95 and later** | KB 210341 |
| `RegisterClassW`, `CreateWindowExW`, `DefWindowProcW`, `SendMessageW`, `SetWindowTextW`, `DrawTextW`... | **Stubs**: Win9x messages are ANSI; the W window machinery is stubbed out | [WEB Michael Kaplan, https://archives.miloush.net/michkap/archive/2006/01/24/516693.html]; KB 125671 ("stubs that simply return success") |
| `IsTextUnicode` | **[MEM: NT-only / stub on 9x]**; write our own detector anyway |
| `CharLowerW`/`CharUpperW` | Not in the native lists; **assume unavailable**; use our own case tables | - |
| Uniscribe `usp10.dll` | Not part of the base 98 install; it shipped with IE5 / Office 2000 / reportedly 98 SE; availability on our VMs is unknown | [WEB https://en.wikipedia.org/wiki/Uniscribe, https://unicode.org/mail-arch/unicode-ml/Archives-Old/UML019/0029.html] |

**Glyph coverage rule on 9x:** `ExtTextOutW` displays only characters in the code page of the selected font's
character set. Others come out **blank** even if the glyph is in the font file.
[WEB KB 145754 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/145/754.HTM]

### 3.2 Decision: ANSI windows, raw-byte model, W drawing

- Windows are ANSI (W classes don't work). File names, menus, dialogs and the status bar use the system ANSI code page
  (FAT long names on 9x go through the ANSI API anyway).
- The document model is **raw bytes + a codec** (1.4), not UTF-16 or UTF-8. The text view decodes the visible
  line segment into a `WCHAR` line buffer, plus a parallel array mapping char -> byte offset, and draws it with
  `ExtTextOutW`. Code points above U+FFFF are drawn as a replacement (9x GDI has no surrogate support **[MEM]**) but stay intact in the bytes.
- No MSLU: everything we need is native, and unicows.dll would add a redistributable plus a loader lib that VC6 may
  not link cleanly **[MEM]**.

### 3.3 Codecs (core, portable)

Interface (per document): `decode(const u8* p, const u8* end, ecp* out) -> length` (returns 1 + an invalid marker
for bad bytes), `prev_boundary(...)`, `encode(ecp, u8* out) -> length or 0 (unrepresentable)`, `unit` (1 or 2) and
`break_unit` bytes.

| Encoding | Implementation |
|---|---|
| UTF-8 | Own strict decoder (rejects overlongs, surrogates, > U+10FFFF; each bad byte becomes one invalid unit). Self-synchronising, so backward stepping is cheap |
| UTF-16 LE/BE | Own; surrogate pairs; lone surrogates are invalid units of 2 bytes |
| SBCS code pages (1250-1258, 437, 850, 866, KOI8-R, ISO-8859-x) | **256-entry table** decode, reverse via a sorted 256-pair array. Tables are generated by a Python script (`codecs` module) into C arrays, so they are portable and tested on the Mac. Optionally built at run time on Windows with MultiByteToWideChar over bytes 0..255 for code pages we don't ship |
| DBCS (932, 936, 949, 950) | Windows only: lead-byte table from `GetCPInfo`/`IsDBCSLeadByteEx`, rows built lazily with MultiByteToWideChar (~64 KB per code page). Mac tests use Python-generated vectors. Backward stepping is **ambiguous** (trail bytes 0x40-0x7E overlap ASCII, e.g. 0x5C '\'), so step back to the line start (or a cached checkpoint) and decode forward. If `IsValidCodePage` fails, the code page is unavailable |

**Invalid bytes:** returned as `0x110000 + byte` (outside Unicode, similar to Python's surrogateescape) and
drawn as a small inverse `xE9` cell. The caret treats each one as one character. Because storage is raw, round-trip
is automatic: there is nothing to re-encode.

**Typing unrepresentable characters** into an ANSI document: `encode()` returns 0, so insert '?' (or refuse), show
a status-bar warning and offer "Convert document to UTF-8".

### 3.4 Detection heuristics (first 64 KB, plus the last 4 KB for UTF-16 hints)

1. BOM: `EF BB BF` UTF-8; `FF FE` UTF-16LE (`FF FE 00 00` UTF-32LE: unsupported in 1.0, so open as binary/hex);
   `FE FF` UTF-16BE.
2. NUL pattern: count NULs at even and odd offsets. If odd-position NULs exceed ~40% of pairs and even-position
   NULs are under ~5%, it is UTF-16LE (ASCII-heavy text); the mirror case is BE. The sample must contain no
   `00 00` pairs at even offsets except rarely.
3. Other NULs present (more than a couple, or `00 00` runs): **binary**, so open in hex view (the user can switch to text).
4. Valid UTF-8 with at least one multi-byte sequence (a truncated sequence at the sample end is OK): UTF-8.
5. Pure ASCII: "ANSI (ASCII)". The setting "treat ASCII files as UTF-8" decides how new non-ASCII characters are encoded.
6. Otherwise ANSI with the default code page (`GetACP()` or a user setting). 1.1 option: statistical
   1251-vs-1252 / Shift-JIS guessing. Always offer "Reopen with encoding..." (re-decode; the bytes don't change).

EOL detection: count CRLF / LF / CR in the sample. The majority sets the mode for newly typed lines and the break unit (1.3).

### 3.5 Clipboard

Copy: put **both** `CF_UNICODETEXT` (decoded; invalid units become U+FFFD) and `CF_TEXT` (WideCharToMultiByte to the ACP), plus a
private registered format `EdRawBytes` (raw bytes + source encoding id), so copy/paste inside the editor between
documents with the same encoding is byte-exact. Don't rely on 9x clipboard synthesis between text formats **[MEM: unverified on 9x]**.
Paste preference: `EdRawBytes` (same encoding) > `CF_UNICODETEXT` > `CF_TEXT` (code page from `CF_LOCALE` if present, else the ACP).
In the hex view, paste of `EdRawBytes` or hex text ("DE AD BE EF") inserts or overwrites bytes.

### 3.6 Fast paths

Most documents are SBCS or UTF-8 and mostly ASCII. Lexers and regex loops check `b < 0x80` inline and call the
codec only for high bytes. For DBCS the shortcut is wrong (trail bytes in the ASCII range), so DBCS documents always go through the codec.

### 3.7 Fonts and fallback on 98

- Create one `HFONT` per (face, style, **charset**) on demand: ANSI_CHARSET, EASTEUROPE, RUSSIAN, GREEK, TURKISH,
  BALTIC, plus HEBREW/ARABIC (drawn LTR without shaping in 1.0), and SHIFTJIS/GB2312/HANGUL/CHINESEBIG5 if such a
  font exists (`EnumFontFamiliesEx`). Courier New and Arial are WGL4 multi-charset fonts on 98 **[MEM]**.
- Coverage: per charset, a 64 Kbit bitmap of the BMP built once with WideCharToMultiByte(charset code page,
  `lpUsedDefaultChar`). The renderer splits runs where the covering charset changes. Characters no font covers are drawn as a box/hex cell.
- Combining marks (cell width 0) are drawn as separate glyphs (GDI on 9x does no composition). Right-to-left and complex scripts
  are out of scope for 1.0 (documented); usp10 could be loaded dynamically later.
- Bold: GDI-synthesised bold on raster fonts (Fixedsys) can be 1 px wider. Use bold only if
  `tmAveCharWidth`/`tmMaxCharWidth` match the normal font, otherwise colour only. No italic in monospace mode (overhang).

### 3.8 Keyboard and IME input on 9x

- Typed characters arrive as WM_CHAR **bytes in the code page of the current input language**, which may differ
  from the ACP (e.g. a Greek layout on a Western 98). Convert with the code page from
  `GetLocaleInfoA(MAKELCID(LOWORD(GetKeyboardLayout(0))), LOCALE_IDEFAULTANSICODEPAGE)` and refresh it on
  `WM_INPUTLANGCHANGE` **[MEM: Kaplan blog, verify on a non-Western layout]**.
- DBCS: handle `WM_IME_CHAR` directly (lead byte = `(BYTE)(wParam >> 8)`) and **don't pass it to DefWindowProc**,
  which would split it into two WM_CHARs. Also buffer WM_CHAR lead bytes (`IsDBCSLeadByteEx`) for IMEs that
  send byte pairs. [WEB https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-char]
- Composition window placement: `ImmSetCompositionWindow`/`ImmSetCompositionFont` from imm32.dll loaded with
  LoadLibrary. Best effort; we can't test East Asian IMEs (no such Win98 install). WM_UNICHAR is XP+ (ignore).

## 4. Rendering

### 4.1 Constraints from this platform

- CLAUDE.md: GDI text is slow on the 86Box P2; hardware blits (Voodoo3) are fast; work in 16-bit; use QPC for timing;
  benchmarks need visible windows.
- **9x GDI coordinates are 16-bit** (+-32K, upper bits truncated, not clipped).
  [WEB https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/win95/chilimit_6qp1.htm, KB 136989
  https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/136/989.HTM] So never pass a line's absolute pixel x;
  always draw only the visible segment at window-relative coordinates.
- **ExtTextOut on Windows 95: 8192 characters max per call** (Scintilla clamps to 8192 on 9x, 65535 on NT)
  [WEB https://bazaar.launchpad.net/~masmullin/scintilla-cocoa/updates/revision/61/win32/PlatWin.cxx]. Moot if we
  draw only visible columns (~100-200 chars).
- 9x logical objects other than regions share a **64 KB GDI heap** [WEB chilimit_6qp1]. Keep few HFONT/HBRUSH/HBITMAP
  objects: no bitmap per tab, a font cache with a small fixed set, one back buffer shared by all views.
- USER/GDI calls on 9x go through the 16-bit Win16Mutex **[MEM]**, so worker threads (indexer, find in files) must
  not call USER/GDI except PostMessage.

### 4.2 Recommended pipeline

1. **Layout (core):** for the visible part of a logical line, decode to `ecp`, map to display cells (tab expansion to the
   next tab stop, control pictures for C0/NUL/DEL/invalid bytes, width 2 for East Asian wide/fullwidth, 0 for
   combining marks), and produce runs of (style, charset font, cell range, WCHAR text). Monospace means
   x = cell * cw, exact and cheap. Keep a `width(cp)` hook so proportional fonts can come later (width tables
   from `GetCharWidthW` per 256-char page, lazily).
2. **Drawing:** per visible line, one `ExtTextOutW(hdc, x, y, ETO_OPAQUE|ETO_CLIPPED, &cellRect, text, n, lpDx)`
   **per run**. ETO_OPAQUE paints background and text in one call, so there is no separate FillRect and no flicker.
   Selection, current-line highlight and find marks change the run's background colour, so runs are split at
   selection edges. Pass `lpDx` (constant cw) to pin glyphs to the grid (needed for fallback fonts with different
   advances); measure whether it costs anything **[PROBE]**. Draw pure-ASCII runs with `ExtTextOutA` if that turns out faster **[PROBE]**.
   The trailing area after the last char is one FillRect (or an ETO_OPAQUE call with 0 chars).
3. **Buffering:** two candidates, chosen by benchmark:
   - *Line band:* one compatible bitmap of client width x line height (800 x 16 x 2 B = 25 KB). Draw a line into it,
     BitBlt to the screen. Minimal memory, no flicker, and partial repaints are cheap. (Scintilla's "buffered draw" works per line.)
   - *Full back buffer:* one client-sized compatible bitmap (800 x 600 x 2 B = 960 KB); repaint only invalid lines into it,
     then BitBlt the invalid rect. This allows overlays (find highlights, caret-line) without extra cost.
   - Direct drawing with ETO_OPAQUE (no buffer) also doesn't flicker, because each pixel is painted once. It may be the fastest, so benchmark it.
   Use `CreateCompatibleBitmap` (device format, 16 bpp, possibly driver-accelerated) rather than a 32-bpp DIB.
4. **Gutter:** line numbers as one short ExtTextOut per line. If GDI text proves very slow, blit digits from a pre-rendered
   10-glyph strip (the CLAUDE.md "pre-render glyphs" lesson). Fold markers: none in 1.0.
5. **Caret:** system caret (`CreateCaret`/`SetCaretPos`), 2 px wide in insert mode, a block in overwrite mode.
   BeginPaint/EndPaint and ScrollWindow(Ex) hide and restore it automatically **[MEM]**. Blink is system-driven.
6. **Scrolling:** `ScrollWindowEx(dy, SW_INVALIDATE)` + `UpdateWindow`, so only the exposed lines are drawn and the
   rest is a screen-to-screen blit (hardware-accelerated on the Voodoo3). Scrollbar: scale positions to 0..32767
   ourselves and read the thumb with `GetScrollInfo(SIF_TRACKPOS)`. WM_VSCROLL only carries 16-bit positions.
   [WEB KB 152252 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/152/252.HTM] `WM_MOUSEWHEEL` is native on 98.
7. **Invalidation:** line-granular. An edit inside one line invalidates that line, plus following lines whose lexer
   state changed (section 7). An edit that changes the line count invalidates from that line to the bottom (or
   scrolls the tail with ScrollWindowEx). Coalesce via WM_PAINT (low priority, merges naturally). Never paint
   synchronously per keystroke except the caret line.
8. **Horizontal scrolling** in whole cells; x origin = `hscroll_cell * cw`.

### 4.3 What the probe must measure (section 12 has the full list)

The cost per `ExtTextOutW` call as a function of run length (1/8/32/100 chars) with and without ETO_OPAQUE and
lpDx; A vs W; Courier New / Lucida Console / Fixedsys; screen DC vs compatible bitmap vs 16-bpp DIB section; a
full 800x600 repaint with 37 lines x 100 cols x ~6 runs; ScrollWindowEx one line; font SelectObject switch cost.
Target: **full repaint <= 30 ms, one-line repaint <= 3 ms** on VM A.

## 5. Word wrap

- View state is `(top_line, top_subrow)` plus the cached layouts of visible lines. Scrolling moves by visual rows
  relative to the anchor (lay out the neighbouring lines as needed), so there is never a whole-document layout.
- Scrollbar thumb = logical line position (`top_line / line_count`, scaled). It is slightly non-linear when wrap
  counts vary, but always exact and O(1). (Scintilla computes all wrap counts in the background; that is optional
  later: a `u16 rows[]` Fenwick tree per line computed at idle, switching the scrollbar to exact visual rows when complete.)
- Wrap cache: `line -> {byte_len_at_layout, row_break_offsets[]}` for ~3 screens around the viewport, invalidated by edits
  to that line, and cleared on resize or font change. With monospace fonts, breaking is column arithmetic on decoded
  cells: break at the last space/punctuation before the width, else hard break. Wide chars never split.
- **Long lines (> 64 KB):** wrapping a 5 MB line means ~60k rows, so lay out lazily in 64 KB chunks with checkpoints
  (byte offset -> visual row), building only what the viewport needs plus a running estimate. Go to end of line
  builds the rest with a progress indicator.
- Caret up/down in wrap mode: keep a "desired x" and map it to the target row's cell.
- Word wrap is allowed in on-demand mode but defaults to off there.

## 6. Undo / redo

- **Piece-native log:** a record is `{op, pos, len, src_span or piece list}`. Insert references the add-buffer
  span it appended; delete keeps the list of piece descriptors it removed (the bytes stay alive in the
  append-only/immutable sources). There are no text copies, so memory is tens of bytes per edit. Undo of an insert = delete; undo of a delete
  = re-insert the saved pieces.
- **Groups:** `begin_group()/end_group()`. Typing merges consecutive single-char inserts adjacent to the previous
  insert. The group breaks on caret movement by navigation or mouse, on direction change (typing vs backspace),
  on newline, after ~1.5 s of idle **[UX choice]**, and when selection replacement starts. Backspace and Delete runs merge the
  same way. Replace-all, encoding/EOL conversion, indent block, paste and hex multi-byte overwrite are single groups.
- **Hex and text share the history** (same byte ops). Hex overwrite = delete n + insert n in one group.
- **Save point:** each group gets a sequence number; `saved_seq` is stored at save. Modified = `cur_seq != saved_seq`
  (so undoing back to the save point clears the modified flag). If the redo branch containing the save point is discarded, `saved_seq = -1`.
- **Across saves:** no rebasing is needed (2.5 step 7), so undo works after saving in both modes.
- **Limits:** unlimited by count by default. A replace-all with 100k matches costs ~100k x 32 B = 3.2 MB, acceptable.
  Offer "clear undo history" and an optional cap (e.g. 1M records). The add buffer is never compacted in 1.0; it is
  released when the tab closes.
- Caret/selection before and after each group are stored, so undo restores the view state.

## 7. Syntax colouring

### 7.1 Engine

- **Per-line start state**, a `u16` encoding (lexer mode, sub-language, nesting depth). States live in a
  gap array parallel to lines (an insert of lines memmoves at the gap: cheap even at 2M lines = 4 MB).
- Paint path: to colour line L, its start state must be known. States are valid up to `valid_upto`. If L is beyond
  that, lex forward from `valid_upto` (bounded per paint; in very large files lex only up to the viewport). Colours are
  **not stored**; the visible lines are re-lexed during paint from their start states (cheap: ~40 lines).
- Edit at line L: invalidate from L. Re-lex L, L+1, ... and stop when the **computed start state of line k equals the stored
  state** (converged). Only invalidate the repaint of the lines whose state changed. Typing inside a C comment typically
  touches one line; typing `/*` re-lexes to the next `*/` or the end of the screen, and the rest happens lazily.
- Lexing runs on the UI thread (idle chunks of ~5 ms via PeekMessage, so it doesn't block input). Default off above 8 MB
  (user can enable it); lines > 64 KB are lexed only for their first 64 KB (the rest takes the default colour).
- Lexers read through a per-line `ecp` iterator with the 3.6 ASCII fast path (DBCS correct).

### 7.2 Data-driven vs hand-coded

A **generic lexer configured from `langs\*.ini`** covers most languages:
```ini
[language]
name=C/C++
extensions=c;h;cpp;hpp;cc;cxx;inl;rc
case=1
line_comment=//
block_comment=/* */
string="\ '\        ; delimiter + escape char
preprocessor=#       ; at line start (after whitespace)
number=c             ; c | pascal ($hex) | asm (0FFh) | basic (&H)
ident_start=A-Za-z_
ident_part=A-Za-z0-9_
keywords1=if else for while do switch case default break continue return goto sizeof typedef struct union enum ...
keywords2=int char short long float double void unsigned signed const static extern volatile register bool ...
keywords3=           ; e.g. Win32 types, user list
operators=+-*/%=<>!&|^~?:;,.()[]{}
```
Features: up to 4 keyword classes, block comment nesting flag (Pascal `{ }` and `(* *)` forms, two pairs),
multi-line strings (Python `"""`, JS template backticks: a state), line-start rules (INI `[section]`, BAT `:label`,
Makefile `target:` and `$(VAR)`, diff `+`/`-`/`@@`), "rest of line" rules (asm `;`, BAT `rem`, `REM` case-insensitive).
Hand-coded: **HTML/XML** (tags, attributes, entities, comments, CDATA; `<script>`/`<style>` switch the state to the
JS/CSS sub-language in the high bits of the state), **Markdown** (line-oriented: headings, lists, code fences, emphasis),
and possibly **Python** (f-strings/triple quotes fit the generic engine; indentation is not lexed).

### 7.3 Languages for 1.0 (suggested)

C/C++ (also RC), x86 asm (MASM/NASM), BAT/CMD, INI/REG, Makefile, HTML, XML, CSS, JavaScript, Python, Pascal/Delphi,
Markdown, plus cheap extras that are only data files: Java, C#, SQL, Perl/shell (hash comments), BASIC/VB, diff.

Size: engine + state store ~1000 lines, HTML/XML ~450, Markdown ~250, each .ini 20-80 lines; **~2000 lines + data.**
Style colours come from one theme file (`themes\default.ini`): ~12 style classes (default, comment, string, number,
keyword1-4, preprocessor, operator, tag, attribute, error/invalid).

## 8. Regex engine from scratch

### 8.1 Engines

| | Backtracking (Perl, Boost, Scintilla's own) | Thompson NFA / Pike VM (RE2-style) | Lazy DFA |
|---|---|---|---|
| Worst case | Exponential (`(a*)*b`) | **O(text x program)** | Linear, fastest |
| Captures | Yes | Yes (Pike VM with thread-saved positions, leftmost-first priority) | Only with extra passes |
| Lazy quantifiers | Yes | Yes (thread priority order) | n/a |
| Backreferences / lookaround | Yes | **No** | No |
| Streaming over the piece table | Needs random access (save/restore position) | **Pure forward, one char at a time** | Forward |
| Size | ~400 lines | ~400 lines | +800 lines, cache management |

Sources: Russ Cox, "Regular Expression Matching Can Be Simple And Fast" [WEB https://swtch.com/~rsc/regexp/regexp1.html],
"Regular Expression Matching: the Virtual Machine Approach" (Pike VM, submatches, non-greedy via priority)
[WEB https://swtch.com/~rsc/regexp/regexp2.html], "Regular Expression Matching in the Wild" (RE2) [WEB https://swtch.com/~rsc/regexp/regexp3.html].

**Recommendation:** one parser and compiler produce one instruction program, with two executors:
- **Pike VM** (default) for any pattern without backrefs or lookahead. It streams over piece-tree spans and code points,
  is never exponential, and handles multi-line matches naturally.
- **Backtracker** (only for `\1`..`\9` and `(?=` `(?!`): runs over a **flat decoded window** (the current line
  extended up to 64 KB; a match longer than the window is not found, a documented limit) with a **step budget**
  (e.g. 2M steps per start position block). Over budget, stop with "pattern too complex".
- Later (2.0): lazy DFA for the "does this line match" phase of find-in-files.

### 8.2 Syntax subset (ECMAScript/PCRE-like, matching what Notepad++ users expect)

- Literals and escapes `\t \n \r \f \v \0 \xHH \x{HHHH} \uHHHH \\ \. ...`; `.` (no newline unless the "dot matches newline"
  option or `(?s)`).
- Classes `[abc] [^a-z] [\w\-]`, `\d \w \s \D \W \S`, POSIX `[[:alpha:]]` etc. (cheap). `\w` = Unicode letters/digits
  + `_` from our own compact tables.
- Anchors `^ $` (line-wise by default, i.e. multiline: `$` matches before CR, LF or CRLF), `\A \z`, `\b \B`, `\< \>`.
- Groups `( )`, `(?: )`, alternation `|`; quantifiers `* + ? {n} {n,} {n,m}` and lazy `*? +? ?? {n,m}?`.
  Counted repetition expands the program, so cap it (program <= ~2000 instructions, `{n,m}` m <= 1000).
- Backrefs `\1-\9`; lookahead `(?= )` `(?! )` (backtracker). No lookbehind, atomic groups, possessive quantifiers or
  recursion in 1.0. Named groups: maybe `(?<name> )` later.
- Flags: case-insensitive (option or `(?i)`), using **simple case folding** tables generated from UnicodeData
  for the BMP (~2 KB of ranges). No `CharLowerW` on 9x.
- `\n` in a pattern matches LF; `\r\n` matches CRLF literally; `\R` = any line break (CRLF|LF|CR).
- Replace: `$0-$9`, `\0-\9`, `${10}`, `$$`, escapes `\n \r \t \\`; case operators `\U \L \E \u \l` (Boost/Notepad++
  style; small) **[UX decision]**. The inserted text is encoded into the document encoding; unrepresentable chars are reported.
- Non-regex modes: "Normal" (literal) and "Extended" (`\n \r \t \0 \xHH` only), both compiled to literal programs and searched by the prefilter.

### 8.3 Searching the piece table without copying

- A `TextCursor` iterates the piece tree span by span (contiguous byte runs: one tree descent, then sequential
  walk), decoding code points inline with the codec fast path. The Pike VM keeps the previous char for `\b`/`^`.
- **Prefilter:** from the program, extract (a) a required literal prefix (exact or ASCII case-folded) or (b) a first-byte
  set (256-bit). For literal prefixes use **Boyer-Moore-Horspool on raw bytes**, valid for SBCS/UTF-8 when the literal is
  encoded in the document encoding, and for UTF-16 with a 2-byte-aligned literal. Candidate positions then run the VM.
  Spans are searched with a small overlap buffer (literal length - 1) at span boundaries.
- **Find previous:** search forward over the block `[p - 64 KB, p)`, remember the last match, and widen backwards until found.
  Simple and correct for leftmost semantics.
- **Replace all:** iterate matches on a snapshot of positions, apply edits back to front in one undo group, and show progress +
  cancel for large documents.

### 8.4 Find in files

Worker thread (no GDI). `FindFirstFile` recursion with masks (`*.c;*.h`) and excluded folders; per file: read whole if < 4 MB,
otherwise stream 64 KB chunks with overlap; sniff the encoding (3.4); skip binaries (NUL in the first 4 KB) by default;
literal search via Horspool on bytes, regex via the Pike VM on decoded text. Results (file, line, column, line text
truncated to 256 chars) are posted in batches of 50 via PostMessage; there is a cancel flag. Open documents are searched in
their in-editor state (on the UI thread in idle chunks), not from disk.
Results display: **reuse the text view on a read-only document** (with a simple results lexer), as Notepad++
does with Scintilla. This avoids the 9x listbox 32,767-item limit **[MEM]**.
Expected speed: disk-bound. Horspool on a P2-233 should exceed 20 MB/s **[PROBE]**; the Pike VM on unanchored
patterns maybe 2-10 MB/s **[PROBE]**. The probe must measure ReadFile throughput on `C:` and on `M:` (Samba).

Size: parser 600 + compiler 300 + Pike VM 350 + backtracker 350 + prefilter/Horspool 250 + replace 150 + Unicode
tables 200 = **~2200 lines**, plus heavy tests.

## 9. Hex view

- **Layout at 800x600 (8-px-wide font):** `00012340  DE AD BE EF 00 01 02 03  04 05 06 07 08 09 0A 0B  ...ASCII...`.
  That is 8 offset digits (more above 4 GB, which we don't support) + 16 x 3 + 1 + 16 = ~75 cells, about 600 px, so it fits next
  to a scrollbar. Bytes per row 8/16/32 (setting); offsets hex or decimal. The ASCII column shows the byte through the document's SBCS
  code page if it is one, else ASCII 0x20-0x7E, with '.' for the rest.
- **Rendering:** precomputed "00".."FF" strings; per row, runs are split only where colour changes (selection,
  modified bytes = ADD-source bytes in red, caret column highlight). The same ETO_OPAQUE run drawing as the text view.
  Rows = ceil(size/16): 4 GB gives 268M rows, so the scrollbar is scaled (4.2.6).
- **Editing:** caret = (byte offset, nibble 0/1, pane hex/ASCII). Overwrite (default): typing a hex digit
  replaces the nibble; after the low nibble, advance. Insert mode: the first nibble inserts byte `N0`, the second sets
  the low nibble (one undo group per byte, merged while typing). In the ASCII pane, typed chars are encoded in the doc encoding
  (multi-byte for UTF-8). Delete/Backspace remove bytes (in both modes; in overwrite mode Backspace just moves back
  **[UX decision]**). Ins toggles the mode, shown in the status bar. Paste: hex text or raw bytes (3.5).
  UTF-16 documents: insert/delete only in 2-byte multiples (1.3).
- **Selection sync:** one selection per document in byte offsets (anchor, caret). Switching views snaps to
  character boundaries in the text view (via the codec) and is exact in the hex view. A split "text | hex" side by side
  is 1.1+ (it is just two views on one document).
- **Go to:** `1F00`/`0x1F00` hex, `#4096` or `d4096` decimal, `+100`/`-0x10` relative, `50%`.
- **Byte search:** hex pattern with `??` wildcards (`DE AD ?? EF`), or text in a chosen encoding (ANSI/UTF-8/UTF-16LE/BE),
  optional ASCII case-insensitive; forward/back; Horspool over spans (wildcards: pick the longest literal sub-run for the
  shift table, then verify). Count all / mark all for small files.
- **Data inspector** (side panel or status-bar popup): at the caret, int8/uint8, int16/uint16, int32/uint32, int64/uint64
  (`__int64`), float32/float64, LE/BE toggle, binary of the byte, the UTF-8 / UTF-16 char at the caret, DOS date/time,
  time_t, FILETIME. Editable fields write the bytes back (undoable). These are pure formatting functions in the core. VC6 cannot
  convert `unsigned __int64` to `double` (error C2520 **[MEM]**), and printf uses `%I64d` on VC6 vs `%lld` on clang,
  so write our own int64-to-string routine.

Size: model/formatting/inspector (core) ~700 lines, view (win) ~1200 lines.

## 10. Robustness

- **Periodic backup snapshots:** every 30 s (setting) after a change, for each dirty document below T_mem, write
  `backup\<tabid>.bak` (temp + rename) plus `backup\<tabid>.inf` (path, encoding, caret, saved seq, original size/time).
  Untitled documents too (Notepad++ "remember unsaved" behaviour). On-demand documents: 1.0 writes no snapshot (too big; warn once).
  1.1 option: an append-only **edit journal** (ops relative to the original file, plus the original's size/time/CRC of the first and last
  64 KB) replayed on recovery.
- **Crash recovery:** `session.ini` has `clean_exit=0` set at startup and `1` on normal exit. On a dirty start, offer
  to restore the backups (compare the original's size/time; warn if the file changed).
- **Session restore:** our own INI reader/writer (portable, tested). Don't use `WritePrivateProfileString`: the 9x profile API
  has size limits (64 KB per INI file **[MEM]**) and is slow. Store open files, active tab, encoding override, caret, scroll,
  view (text/hex), wrap and language per tab. The session is written on exit and every 60 s (temp + rename). Settings live next to
  the exe (portable, zip-deployable like the other projects); `C:\WINDOWS\Application Data` via
  `SHGetSpecialFolderPathA` (shell32 4.71+, present on 98 **[MEM]**) is only a fallback if the exe folder is read-only.
- **Change detection:** `FindFirstChangeNotification(dir, FALSE, LAST_WRITE|SIZE|FILE_NAME)`, one handle per distinct directory
  (<= 63 + 1 in `MsgWaitForMultipleObjects` in the main loop). Caveats: on 9x the last-write and size filters fire only when the writer
  **closes** the file [WEB https://techshelps.github.io/MSDN/WIN95DDK/HTML/S30C4.HTM], and remote file systems may not notify at all
  [WEB https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationa]. Our own
  workflow is on Samba `M:`, so **polling is mandatory**: on `WM_ACTIVATEAPP` (gaining focus) and every 3 s for network paths, compare
  size + last-write time from `FindFirstFile` (works everywhere; no need for GetFileAttributesEx). If changed: unmodified
  document = reload (prompt or auto, setting); modified = prompt (reload / keep mine / compare later). Deleted = mark the tab "missing",
  keep the content, and treat the document as modified. On-demand documents can't change (deny-write); the delete case is the 9x DeleteFile risk
  (2.2): on detection, show a warning and offer to save elsewhere immediately (reads of freed clusters could return garbage **[MEM]**).
- **Locked files:** open falls back from deny-write to share-write read-only (2.3). Save into a file locked by another process uses the
  in-place fallback (2.5 step 5) or reports "file is in use" with retry/save-as. A read-only attribute asks whether to clear it.
- **Very long lines:** never decode or measure a whole line for painting. Per line > 64 KB, keep checkpoints every 4 KB (byte
  offset -> cell x, plus the lexer state where relevant) so horizontal positioning costs O(4 KB). Caret Home/End and column
  display use the checkpoints. Lexing stops after 64 KB. Selection of a 5 MB line is fine (byte offsets). Line copy uses
  the same streaming path as save.
- **Binary in text view:** NUL and C0 controls are drawn as short inverse abbreviations (`NUL`, `SOH`, ... or `^@`) in 1-3
  cells; invalid bytes as `xNN`. The core never uses NUL-terminated strings for document data. A binary sniff opens hex
  view by default (3.4).
- **Out of memory:** every allocation in the core returns NULL-checked errors (VC6 `new` returns NULL instead of throwing **[MEM]**;
  we use our own allocator functions anyway). An edit that fails to allocate is rolled back and reported; never crash with unsaved data.
- **Exceptions:** wrap the message loop in `__try/__except` (SEH works in VC6 without /GX) to write emergency backups of all
  dirty documents on a crash, then exit. Cheap insurance.

## 11. Code structure, VC6 rules and testing

### 11.1 VC6 dialect rules for the shared core (lint-enforced where possible)

- No STL in the core (VC6's STL is buggy and slow, and it differs from libc++). Simple class templates only (e.g. `Vec<T>` for POD),
  with no partial specialization, member templates or `typename` tricks.
- No in-class `static const int X = 5;` initialisers (VC6 rejects them; use `enum`). No `long long` (use `typedef __int64 i64` on VC6
  and `long long` on clang, behind `etypes.h`). No exceptions, no RTTI. No `<stdint.h>`, `<cstdint>` or `snprintf` (`_snprintf` doesn't null-terminate:
  wrap it). No `std::min/max` (VC6 lacks them; use our own `emin/emax`).
- **Loop variables are declared at the top of the block** (`int i; for (i = 0; ...)`), because VC6 leaks `for` scope and clang
  has no switch to emulate that. Enforce with a grep lint for `for\s*\(\s*(const\s+)?(int|unsigned|long|epos|u8|u16|u32|size_t)\b`.
- Warnings: `/W3` clean on VC6 (the CLAUDE.md rule) and `-Wall -Wextra -Werror` on clang.

### 11.2 Modules

`core/` (portable; no `windows.h`; built by clang and VC6):

| File | Responsibility | Approx. lines |
|---|---|---|
| `etypes.h` | u8/u16/u32/i64, `epos`, `ecp`, asserts, emin/emax | 80 |
| `emem.h/.cpp` | allocator wrappers, node pool, `Vec<T>`, growable byte arena (add buffer) | 250 |
| `esource.h/.cpp` | `ByteSource` interface: `MemSource`, `SynthSource` (tests: generates GBs of pattern bytes), file source lives in win/ | 150 |
| `ptree.h/.cpp` | piece tree: insert/delete/read/span iteration, offset<->line, break counting, 64 KB cap, invariant checker | 1800 |
| `undo.h/.cpp` | undo log, groups, save point | 400 |
| `doc.h/.cpp` | `Document` = ptree + undo + codec + EOL + text_origin + listeners (edit notifications to views/lexer/wrap) | 600 |
| `codec.h/.cpp` + `cptables.c` | UTF-8/16, SBCS tables, DBCS hook, detection, encode/decode, invalid units | 900 + tables |
| `ucd.h/.cpp` | case folding, East Asian width, word class, combining (generated ranges) | 300 + tables |
| `layout.h/.cpp` | line -> cells/runs, tabs, control pictures, x<->offset, wrap breaking, long-line checkpoints | 1000 |
| `lexer.h/.cpp`, `lexgen.cpp`, `lexhtml.cpp`, `lexmd.cpp` | lexer interface, state store, generic data-driven lexer, hand-coded ones | 2000 |
| `regex.h/.cpp`, `rxvm.cpp`, `rxbt.cpp`, `rxpre.cpp` | parse/compile, Pike VM, backtracker, prefilter + Horspool, replace expansion | 2200 |
| `search.h/.cpp` | find/replace over a Document (cursor iteration, find previous, replace all), byte search | 600 |
| `hexfmt.h/.cpp` | hex row formatting, nibble edit state machine, go-to parser, data inspector conversions | 700 |
| `ini.h/.cpp`, `session.h/.cpp` | INI read/write (settings, languages, themes), session model | 600 |

`win/` (Win32/GDI, VC6 only):

| File | Responsibility | Approx. lines |
|---|---|---|
| `main.cpp` | WinMain, single instance (open in the running editor via `WM_COPYDATA`), message loop with MsgWaitForMultipleObjects, SEH guard | 300 |
| `frame.cpp` | main window, menus, accelerators, toolbar, status bar, tab strip, MRU | 1500 |
| `textview.cpp` | text view window: input (WM_CHAR/IME/keys/mouse/wheel), caret, selection, scrolling, painting via render | 2500 |
| `hexview.cpp` | hex view window | 1200 |
| `render.cpp` | font cache per charset + coverage bitmaps, run drawing, back/band buffer, measuring | 700 |
| `winfile.cpp` | open (modes, indexer thread), `FileSource` + block cache, save sequence, backups, change watch | 1200 |
| `winenc.cpp` | DBCS/OS code page tables via MultiByteToWideChar, input-locale code page | 250 |
| `clip.cpp` | clipboard formats | 200 |
| `dlgfind.cpp`, `dlgfif.cpp`, `dlggoto.cpp`, `dlgprefs.cpp`, `dlgenc.cpp` | dialogs (modeless find/replace, find in files + worker) | 1800 |
| `editor.rc`, `resource.h` | menus, dialogs, icons (CRLF line endings) | 600 |

Total: core ~12k, win ~10k lines, plus tests ~5k.

### 11.3 Key interfaces (sketch, C++98 + VC6)

```cpp
// esource.h
class ByteSource { public:
  virtual ~ByteSource() {}
  virtual epos size() const = 0;
  // returns pointer to >= 1 byte at off (valid until next call), *avail = contiguous bytes
  virtual const u8* get(epos off, epos* avail) = 0;   // NULL on I/O error
};
// ptree.h
struct PSpan { const u8* p; epos len; };
class PieceTree { public:
  int  init(ByteSource* original, u8 break_byte, int unit /*1|2*/);   // builds 64 KB pieces
  int  insert(epos pos, const u8* bytes, epos n, UndoRec* rec);       // 0 ok, <0 E_NOMEM/E_RANGE
  int  remove(epos pos, epos n, UndoRec* rec);
  epos length() const;  u32 line_count() const;
  epos line_start(u32 line);  u32 line_of(epos pos);
  int  read(epos pos, u8* out, epos n);
  // span iteration without copying
  void cursor_init(PCursor* c, epos pos);  int cursor_next(PCursor* c, PSpan* s);
  int  check() const;                                                  // invariant checker for tests
};
// doc.h: listeners get {pos, removed, inserted, first_line, lines_removed, lines_added}
// codec.h: struct Codec { int id; int unit; int (*decode)(const u8*,const u8*,ecp*); int (*encode)(ecp,u8*); ... };
// lexer.h: class Lexer { virtual u16 lex_line(LineIter& it, u16 start_state, StyleSink* out /*NULL = state only*/) = 0; };
// regex.h: rx_compile(const ecp* pat, int n, int flags, RxProg** out, RxError* err); rx_search(RxProg*, TextCursor*, epos from, epos to, RxMatch* m);
```

### 11.4 Waves for parallel agents

- **Wave 1 (independent, core only, Mac-tested):** W1a etypes/emem + ptree + esource + undo; W1b codec + ucd + tables
  generator (Python); W1c regex (all of 8) + search over a flat buffer; W1d lexer engine + language INIs + HTML/MD; W1e hexfmt +
  ini/session. All interfaces are frozen at the start of the wave in `docs/DESIGN.md`.
- **Wave 2:** doc.cpp integration + layout; search over Document; win skeleton (frame, tabs, open/save with both modes,
  render.cpp driven by the probe results); textview with basic editing.
- **Wave 3:** hexview, find/replace dialog, find in files, clipboard, IME, session restore, change detection, backups.
- **Wave 4:** robustness (long lines, binary, locked files, OOM paths), more languages, preferences, polish, X41 check.
  User play-test per milestone.

### 11.5 Testing

Following `dev/fw98` (`engine/test/Makefile`: strict clang lint, -O2 run, ASan+UBSan run, plus the same tests built as a
VC6 console exe via NMAKE):

- `core/test/Makefile` (Mac): `clang++ -std=c++98 -pedantic -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion
  -fno-exceptions -fno-rtti`, then an -O2 run and an ASan/UBSan run, plus the for-scope and banned-API grep lint. `core/Makefile` (NMAKE):
  `cl /nologo /W3 /O2` builds `coretest.exe` from the **same test sources**, run with `w98 run` (counts must match the Mac run).
- **Differential random tests** (portable, C++98): a piece tree vs a naive flat byte array model, with 1M random inserts/deletes/undos/redos
  and invariant checks after each; random line queries vs a brute-force scan; `SynthSource` for 3.9 GB documents (offset
  arithmetic near 2^31 and 2^32).
- **Mac-only fuzzers** (modern C++, not built by VC6): the regex vs `std::regex` ECMAScript on a generated pattern and text corpus (only the
  shared subset; compare match spans); codecs vs Python/iconv-generated vectors (all SBCS tables, cp932/936/949/950 samples,
  invalid UTF-8 corpus); decode->encode round-trip is byte-exact for any byte string; lexer convergence (incremental re-lex after a
  random edit == full re-lex).
- **Regex safety tests:** `(a*)*b`, `(a|aa)*c` on 100 KB inputs must finish in bounded time (Pike VM linear; the backtracker hits its budget).
- **Win-side tests:** a small scripted harness (`editor.exe -selftest`) that opens, edits and saves fixture files on `C:` and `M:`, plus a
  `-bench` mode writing `bench.txt` (repaint, scroll, open-100MB, find timings) as CLAUDE.md recommends.

## 12. Probe / benchmark checklist (for the separate GDI + file I/O probe)

Run with the window visible (`ShowWindow` twice), no VNC connected, timing with QPC, 800x600x16 desktop.

1. `ExtTextOutW` cost per call vs run length 1/4/8/16/32/64/128 chars, Courier New 10pt / Lucida Console / Fixedsys:
   (a) plain, (b) `ETO_OPAQUE`, (c) `ETO_OPAQUE|ETO_CLIPPED`, (d) with `lpDx`, (e) the same with `ExtTextOutA`.
2. Full-screen editor repaint: 37 lines x 100 cols, 1/6/15 colour runs per line: direct to screen vs into a line band
   compatible bitmap + BitBlt vs into a full compatible bitmap vs into a 16-bpp DIB section. Target <= 30 ms.
3. Single-line repaint (typing case) and caret-line + gutter cost.
4. `ScrollWindowEx` by 1 line and by 1 page + painting the exposed lines (time per scroll step; holding PageDown).
5. `SelectObject(font)` switch cost (fallback fonts per run) and `SetTextColor`/`SetBkColor` per run.
6. `GetTextExtentPoint32W`, `GetCharWidthW` (256-char range) and `GetTextExtentExPointW` cost.
7. "Smooth edges of screen fonts" on vs off, if available.
8. Verify that `ExtTextOutW` with RUSSIAN_CHARSET / EASTEUROPE_CHARSET Courier New shows Cyrillic/Polish on the Western Win98 (KB 145754 behaviour).
9. File I/O: ReadFile throughput with 4/16/64/256 KB chunks on `C:` and `M:` (Samba); sequential 100 MB read; random
   64 KB reads; CreateFile/FindFirstFile latency per file on `M:` (find-in-files cost); write + FlushFileBuffers 100 MB.
10. Byte-loop CPU throughput: LF counting, memchr, Horspool, UTF-8 validation, MultiByteToWideChar(CP_UTF8) on a 1 MB buffer.
11. API availability via GetProcAddress + behaviour: `GetDiskFreeSpaceExA`, `GetFileAttributesExA`, `SHGetSpecialFolderPathA`,
    `IsTextUnicode`, `GetCharABCWidthsW`, `CharLowerW` (does it work or is it a stub?).
12. File semantics: MoveFile of a file we hold open (expected: sharing violation); DeleteFile of a file another process holds open
    deny-write (does it really succeed on 98?); FindFirstChangeNotification on `C:` and on `M:` (does `M:` signal at all?);
    a 2-4 GB file on FAT32 (if a big enough disk exists): read beyond 2 GB with SetFilePointer high dword.
13. Memory: largest single `VirtualAlloc` reserve/commit and `HeapAlloc`, and how a 100 MB in-memory load behaves (time, swap).

## 13. Open questions

1. Thresholds T_mem (32 MB?) and "colouring off above 8 MB": confirm with probe timings on VM A and the X41.
2. Line band vs full back buffer vs direct drawing: decided by probe item 2.
3. CR-only documents: is the "break byte per document" simplification acceptable to the user, or must mixed CR lines split like Notepad++?
4. Does the user want proportional fonts in 1.0? (The layout keeps a hook; drawing and hit-testing would need width tables.)
5. How far to go on DBCS/IME given no East Asian Win98 to test on? Suggest: decoding/display yes, IME best-effort.
6. Is `usp10.dll` present on our VMs/X41 (98 SE + IE version)? Only relevant for a future complex-script feature.
7. On-demand documents: is "no autosave snapshot" acceptable for 1.0, or is the edit journal needed?
8. Does Win98's redirector support > 2 GB files on `M:`? (Low priority; FAT32 local is the main case.)

## Sources

- VS Code piece tree: https://code.visualstudio.com/blogs/2018/03/23/text-buffer-reimplementation
- Russ Cox regex series: https://swtch.com/~rsc/regexp/regexp1.html, https://swtch.com/~rsc/regexp/regexp2.html, https://swtch.com/~rsc/regexp/regexp3.html
- Unicode on 95/98: KB 210341 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/210/341.HTM; KB 125671 https://techshelps.github.io/MSDN/KB/Source/win32sdk/q125671.htm; KB 145754 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/145/754.HTM
- Emacs w32font.c (9x W-function stubs): https://github.com/emacs-mirror/emacs/blob/master/src/w32font.c
- Kaplan on 9x/ANSI messages and WM_IME_CHAR: https://archives.miloush.net/michkap/archive/2006/01/24/516693.html; https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-char
- MultiByteToWideChar: https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar; Raymond Chen https://devblogs.microsoft.com/oldnewthing/20120504-00/?p=7703
- Win95 system services limits (file mapping, async I/O, DeleteFile): https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/win95/chilimit_3gvn.htm
- Win95 GDI limits (16-bit coords, 64 KB heap): https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/win95/chilimit_6qp1.htm; KB 136989 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/136/989.HTM
- Address space and file mapping: KB 125691 https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/125/691.HTM; KB 125713 https://ftp.zx.net.nz/pub/Patches/ftp.microsoft.com/MISC/KB/en-us/125/713.HTM; VirtualProtect KB https://techshelps.github.io/MSDN/KB/Source/win32sdk/q170426.htm
- MoveFileEx on 9x: https://techshelps.github.io/MSDN/WINBASE/devdoc/live/pdwbase/filesio_9oe0.htm; ReplaceFile: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilea
- Change notification on 9x: https://techshelps.github.io/MSDN/WIN95DDK/HTML/S30C4.HTM; https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationa
- Scroll positions: KB 152252 https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/152/252.HTM
- ExtTextOut 8192 on 9x (Scintilla PlatWin.cxx): https://bazaar.launchpad.net/~masmullin/scintilla-cocoa/updates/revision/61/win32/PlatWin.cxx
- Uniscribe history: https://en.wikipedia.org/wiki/Uniscribe
- Workshop: `~/Documents/86box/CLAUDE.md` (performance lessons), `dev/fw98/docs/HANDOFF.md` + `engine/test/Makefile` (test pattern)
