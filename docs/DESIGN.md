# Pica 1.0: design specification

Text editor with a built-in hex editor for Windows 98 SE. VC6, Win32 + GDI, all our own code.
Folder `dev/editor` = `M:\dev\editor`. Owner session 86box-d1, VM A.

This document is the contract for the coding agents. `docs/TASKS.md` splits the work into waves; `docs/HANDOFF.md`
collects what each task learned. Research behind the decisions: `docs/research/UX.md`, `docs/research/ARCH.md`,
`probe/RESULTS.md` (measured on VM A). Section numbers below are referenced from TASKS.md.

Contents
0. Summary
1. Goals, non-goals, scope
2. Decisions (user, overseer, design author)
3. Platform facts and performance budgets
4. Architecture: layers, modules, data flow, threads
5. Core conventions (types, errors, memory, VC6 dialect)
6. Core interfaces (exact headers)
7. Document model
8. Text view
9. Hex view and view sync
10. Rendering
11. Search
12. Encodings and line endings (UX)
13. Files: load, save, backup, session, settings, change detection
14. Large files and long lines
15. Win layer: windows, commands, menus, shortcuts, status bar, dialogs
16. Error and edge-case policy
17. Test strategy
18. Folder layout and build
Appendix A: product name. Appendix B: default colours. Appendix C: settings keys.

---

## 0. Summary

- **Product name: Pica** (exe `pica.exe`). "Pica" is the typewriter monospace pitch (10 characters per inch), which fits
  a monospace editor. The name lives in one header (`res/appinfo.h`, `APP_NAME`) plus one Makefile line (`APP = pica`).
- **One document, two views.** Each tab holds a byte-exact `Document` (piece tree over the file's bytes). The text view
  decodes; the hex view shows raw bytes. One undo history, one selection (byte offsets), lossless round trip of any bytes.
- **Portable core** (`src/core`, no `windows.h`, C++98 that builds with VC6 and with `clang++ -std=c++98 -pedantic`) holds
  everything that can be tested on the Mac: buffer, undo, codecs, detection, layout, lexers, regex, search, hex
  formatting, editing commands, INI/config/session, file operations over an abstract `FileSys`. The **win layer**
  (`src/win`) holds windows, GDI, dialogs, threads and the Win32 `FileSys`.
- **Rendering** (probe): draw directly to the window DC in WM_PAINT, one `ExtTextOutW(ETO_OPAQUE|ETO_CLIPPED)` per colour
  run, no back buffer. Full repaint 4-5 ms, keystroke 0.4 ms, `ScrollWindowEx` for line scrolls. Default font Courier New
  10 pt.
- **Files**: in memory up to 16 MB, read on demand above (deny-write handle, 64 KB block cache, background indexer).
  Saves run on a worker thread (the M: share writes 0.4 MB/s) and use temp file + rename with rollback. Unsaved work is
  backed up every few seconds into generation-numbered backup files; the session uses two alternating files. No renames
  are needed for either, so a crash at any moment leaves a valid copy.
- **Change detection**: poll size + time of every open file every 3 s and on activation (Mac-side edits on M: never
  signal a change notification), plus `FindFirstChangeNotification` for instant response to in-VM writes.
- Scope = the agreed focused 1.0 (section 1). Polish over features.

## 1. Goals, non-goals, scope

### 1.1 Goals
1. Never lose or corrupt user data: safe save, crash-proof backups, session written often, lossless bytes.
2. Feel instant on the PII-233: keystroke < 5 ms, full repaint < 15 ms, scrolling smooth, no UI freeze over 0.5 s
   without a progress indicator and Cancel.
3. Look and behave like a well-made Win98 program: standard controls, system colours, menus with accelerators,
   VC6/Notepad++ keys.
4. An honest hex view: it shows the bytes the file will have on disk, always.

### 1.2 Non-goals for 1.0
Plugins, macros, split view, column selection, folding, autocomplete, printing, run-tool/jump-to-error (first 1.1
candidate), replace in files, proportional fonts, right-to-left/complex scripts, user key remapping, theme gallery,
network access of any kind.

### 1.3 In scope (the 1.0 feature list)
From UX.md 8.1 "must":
- Tabs (close button on the active tab, middle-click close, drag to reorder, modified marker, path tooltip, context menu
  Close / Close Others / Copy Path / Open Containing Folder).
- New / Open / Save / Save As / Save All / Revert / Close / Close All / Close Others, 10 recent files, drag-and-drop
  open, single instance, command line.
- Safe save, never a 0-byte or half-written file.
- Encodings ANSI / OEM (reload) / UTF-8 / UTF-8 BOM / UTF-16 LE / UTF-16 BE with BOM handling, line endings
  CRLF / LF / CR, status-bar display (clickable), Reload as / Convert to, lossless undecodable bytes.
- Syntax colouring for the 12 languages of UX.md 8.5 (+ CSS, needed inside HTML), auto by extension, manual override.
- Line numbers, Ln/Col in the status bar, auto-indent, tab width + tabs/spaces per language.
- Unlimited undo/redo grouped by typing runs, shared with the hex view.
- Find/Replace dialog (Normal / Extended / Regex; case, whole word, wrap, in selection, direction; Count, Find All,
  Mark All, Replace All, Replace All in Open Tabs), F3 / Shift+F3 / Ctrl+F3 / Ctrl+Shift+F3, Ctrl+G.
- Find in Files with a results panel (F4 / Shift+F4).
- Session restore including unsaved and untitled tabs; periodic backup; crash recovery.
- File-changed-on-disk handling (quiet reload of unmodified documents, non-modal bar for modified ones).
- Hex view of the same document: offset | hex | chars, overwrite default + insert mode, nibble caret, Tab switches
  panes, selection sync, go to offset, hex/text byte search, modified bytes in red, binary auto-detect.
- Large-file mode with load progress and Cancel.
- Line commands Ctrl+D, Ctrl+L, Ctrl+Shift+Up/Down, Ctrl+Q, case keys Ctrl+U / Ctrl+Shift+U, block indent.
- Options property sheet (General, Editor, Files, Colours, Hex), each option with a one-line explanation.
- Help: keyboard shortcuts + regex cheat sheet, shown as a read-only tab.

Plus these UX.md "should" items (overseer: cheap and central to editing): word wrap, show whitespace/EOL, brace match
highlight + Ctrl+B, incremental search bar (Ctrl+I), bookmarks (Ctrl+F2 / F2 / Shift+F2), go to line/offset,
read-only data inspector in the hex view.

### 1.4 Deferred (not in 1.0, in rough 1.1 order)
Run tool + capture output + jump to error; reopen closed tab; current-line highlight; smart highlight; zoom; printing;
Ctrl+J join lines; hex bytes-per-row and decimal-offset options (1.0: 16 per row, hex offsets); hex "Copy as hex
text" as a separate command (1.0 gets the same effect from Copy in the hex pane, see 9.6); editable inspector; column
selection; replace in files; macros; split view; compare; folding; key remapping; user-defined languages; on-demand
document autosave journal; proportional fonts; IME composition polish beyond best effort.

## 2. Decisions

### 2.1 User decisions (2026-11-09, STATUS.md)
| Decision | Reason |
|---|---|
| All our own code, no Scintilla or other component | The point of the project; full control on Win98 |
| Focused 1.0 (tabs, encodings, colouring, find/replace + regex + find in files, session restore, hex view of the same file) | Small and polished beats broad |
| Research + probe, then DESIGN, then task waves by sub-agents | Proven on Retro Strike and Lahio |

### 2.2 Overseer decisions (2026-11-09)
| Decision | Reason |
|---|---|
| Scope = UX.md must list + word wrap, show whitespace/EOL, brace match, Ctrl+I, bookmarks, line commands, go to line/offset, read-only inspector | These are cheap and central to editing; everything else waits (polish over features) |
| Byte piece table in a balanced tree with per-node byte and break counts, pieces <= 64 KB | Zero-copy load, O(log n) edits, natural undo, bounded line scans; same bytes serve both views |
| Text view decodes, hex view reads raw bytes, lossless round trip | The hex view must show the real file; unmodified bytes are never re-encoded |
| In memory up to a threshold, on-demand 64 KB block reading + cache above it, no memory-mapped files | Bounded RAM on a 128 MB machine; MMF views share a global 1 GB arena and cost 2.3 ms each |
| Safe save = temp + rename dance with rollback | No ReplaceFile/MoveFileEx on Win98; never truncate the original first |
| ANSI windows + ExtTextOutW for drawing | Win98 W window functions are stubs; ExtTextOutW is native and draws Cyrillic/Polish (probe) |
| Own UTF-8/UTF-16 codecs + generated single-byte tables | MultiByteToWideChar(CP_UTF8) accepts overlongs and runs at 8.7 MB/s (probe) |
| Pike-VM regex + budgeted backtracker only for backrefs/lookahead, literal prefilter | Linear time for normal patterns, Notepad++-like syntax; Horspool runs at 250-330 MB/s (probe) |
| Per-line lexer state, re-lex until convergence; data-driven language files + hand-written HTML/XML and Markdown lexers | Incremental and cheap (15 us per line, probe); new languages are data |
| One undo history over byte edits shared by both views, save-point tracking | Hex and text edits are the same primitive; undo back to the save point clears "modified" |
| Change detection = FindFirstChangeNotification + polling (focus + every ~3 s) | Mac-side edits on M: never signal (probe); polling costs 1 ms per file on M: |
| Monospace fonts only | Cell arithmetic is exact and cheap; proportional later behind the same layout interface |
| CRLF and LF both work in one document (mixed allowed); a lone CR in an LF/CRLF document is drawn as a symbol; CR-only documents are detected and use CR as the break | One break byte per document keeps piece counts local (no CRLF-split problem) |
| DBCS display yes, IME best effort | No East Asian Win98 to test on (code page 932 is not even installed on VM A) |
| On-demand (huge) documents get safe save but no autosave snapshot in 1.0 | Snapshots of 100 MB files are too slow; a journal is 1.1 work |
| New-file default = ANSI (system code page) + CRLF, no BOM | Probe: a UTF-8 BOM breaks cl, rc, nmake and batch files; Win98 tools expect ANSI + CRLF |
| Product name chosen by the design author, kept in one constant | Trivial rename later |

### 2.3 Design author decisions (this document)
| Decision | Reason |
|---|---|
| Name **Pica**, `pica.exe`, settings `pica.ini` | Short, 8.3-safe, original, fits monospace; alternatives in Appendix A |
| Direct drawing, no back buffer; ExtTextOutW for every run (no A fast path) | Probe: direct 4.0-4.3 ms vs compatible bitmap 7.0 ms vs DIB 23.5 ms; ETO_OPAQUE paints each pixel once (no flicker); W costs 5-10% and keeps one code path |
| Default font Courier New 10 pt (8x16) | Fastest measured (4.0 ms), TrueType with WGL4 coverage (Cyrillic, Greek, Polish, box drawing) through ExtTextOutW; Lucida Console and Fixedsys offered |
| In-memory threshold 16 MB (`min(option, totalPhys/8)`, floor 4 MB); "large mode" (no colouring, no wrap by default) from 8 MB | Probe: 10 MB loads in 129 ms; 50 MB pushed free RAM to 4 MB and took 3-9 s |
| Saves always run on a save worker thread over a piece snapshot; flows that must finish first wait with a modal progress | M: writes 0.4 MB/s (a 1 MB save = 2.5 s); one code path, no UI freeze |
| Backups: generation-numbered files (`write gen N+1, verify, delete gen N`); session: two alternating files with a sequence number | No rename at all, so no Win98/Samba rename semantics in the crash path; a torn write leaves the previous copy valid |
| Settings next to the exe if writable, else `C:\WINDOWS\Application Data\Pica` (SHGetSpecialFolderPathA works, probe) | Portable zip deploy like the other workshop projects |
| Language files embedded as RCDATA, overridable by a `langs\` folder next to the exe | Single-exe robustness, still data-driven |
| Hex view: 16 bytes per row, hex offsets, BOM bytes protected | Polish over options; editing a BOM in hex would desync the text view's origin |
| UTF-16 documents: all edits are 2-byte aligned (hex overwrite rewrites the whole unit) | Keeps piece break counts independent of absolute position |
| Typing a character the encoding can't store is refused (beep + notification bar offering "Convert to UTF-8"); paste replaces such characters with `?` and says so | No silent data change; one click to fix |
| Find in Files searches files on disk, also for open documents | The worker must not touch documents; the summary notes modified open files |
| Ctrl+Tab cycles tabs in bar order (not MRU) | Simple and predictable; MRU is 1.1 polish |
| Mark All / search marks shift with edits, capped at 10,000 | Cheap, avoids stale highlights |
| Data inspector is a strip at the bottom of the hex view | A side panel does not fit 800x600 next to a 16-byte row |
| Help is a read-only tab showing the embedded `help.md` | Reuses the text view and the Markdown lexer; no WinHelp compiler needed |
| No exceptions anywhere (/GX off); error codes; SEH guard only around the message loop for emergency backups | VC6 + clang both happy; predictable OOM handling |

## 3. Platform facts and performance budgets

### 3.1 Facts from the probe (VM A: PII-233, 128 MB, Win98 SE, 800x600x16 RGB565, 96 dpi)
| Fact | Consequence |
|---|---|
| Full repaint 34 rows: direct ETO per run 4.0 ms (A) / 4.3 ms (W); compatible bitmap 7.0; DIB16 23.5; glyph caches 12-22 ms; dense text 6.7 ms | Direct drawing (10.1) |
| 1-line scroll with ScrollWindowEx + exposed row 2.9-4.0 ms vs 4.3 full | ScrollWindowEx for small deltas, full repaint for jumps |
| ExtTextOut ~8 us per call + ~0.9 us per char; lpDx and ETO_CLIPPED free; SelectObject 2 us; SetTextColor/SetBkColor 1.6 us | Per-run colour changes are cheap; no need to merge runs aggressively |
| Keystroke (edit + relex line + repaint row + caret) 0.4 ms; caret move 38 us; relex 50-char line 15 us | Budgets below have large headroom |
| Stubs: GetCharABCWidthsW, CharLowerW/UpperW, GetCharWidth32A/W, GetTextMetricsW, GetTextExtentExPointW, DrawTextW, CompareStringW, IsTextUnicode, CreateFileW | Use GetTextMetricsA, GetCharWidthW/A, own case tables, own detection, A file APIs |
| GetTextExtentExPoint 50-57 ms per 1000 chars | Never call it |
| ExtTextOutW + Courier New draws Cyrillic and Polish with any lfCharSet | One font handle per style; no per-charset fonts for European text |
| No antialiasing on Win98 (2 colours) | Nothing to handle |
| Fixed-pitch fonts installed: Courier New, Lucida Console (TT), Fixedsys, Terminal, Courier (raster) | Font choices |
| C: write 6.2 MB/s; reads 210+ MB/s (VCACHE); line count memchr 88 MB/s | 10 MB load + index ~0.25 s; 10 MB save 1.6 s |
| M: read 1.1 MB/s, write 0.4 MB/s, 5.5 ms per file in find-in-files, GetFileAttributes 1 ms | Save worker, progress everywhere, FiF on a thread, polling affordable |
| 50 MB in memory: free RAM 4 MB, load 3-9 s | 16 MB in-memory threshold |
| malloc max 128 MB, HeapAlloc 255 MB, VirtualAlloc 2 GB; small malloc 1.55 us | Big blocks via VirtualAlloc; node pools for small objects |
| MoveFile/DeleteFile fail on a file anyone holds open (C: and M:); FILE_SHARE_DELETE = invalid parameter | Close our handle before renaming; never pass share-delete; deny-write handle protects on-demand sources |
| Change notifications fire for in-VM writes on C: and M:, never for Mac-side edits on M: | Polling is mandatory |
| UTF-8 BOM breaks cl, rc, nmake (first token) and command.com | Never add a BOM silently; project sources are BOM-less ASCII |
| Code pages 1250/1251/1252/437/850/65001 valid; 932 invalid; ACP 1252, OEMCP 437 | DBCS path only testable with Mac vectors |
| Horspool 257-328 MB/s (case-insensitive 177); own UTF-8 validation 35 MB/s unoptimised | Literal prefilter everywhere; give UTF-8 an ASCII fast path |
| CharLowerBuffA 5.2 MB/s | Own 256-entry fold tables |

### 3.2 Budgets (VM A, window visible, no VNC; checked by `pica -bench`, section 17.5)
| Operation | Budget | Probe basis |
|---|---|---|
| Keystroke in a normal line (edit, undo record, relex, repaint row, caret, status) | <= 3 ms | 0.4 ms |
| Full text-view repaint, 34 rows, coloured | <= 12 ms (dense text <= 15) | 4-7 ms + layout/lex |
| Line scroll (arrow at edge, wheel 3 lines) | <= 8 ms | 3-4 ms |
| Page down | <= 12 ms | 4-5 ms |
| Hex view full repaint | <= 12 ms | 5-7 ms |
| Open 1 MB file from C: (to first paint) | <= 150 ms | read + index 12 ms/MB |
| Open 10 MB from C: | <= 600 ms, progress after 0.5 s | |
| Open 100 MB (on-demand) from C: | first screen + hex usable <= 1 s; full index <= 3 s | 88 MB/s scan |
| Find next, literal, in-memory doc | >= 30 MB/s scanned | Horspool 250+ |
| Find next, regex without literal prefix | >= 2 MB/s | estimate |
| Replace All, 10,000 matches in 1 MB | <= 2 s | |
| Startup to window visible (no session) | <= 300 ms | |
| Session restore, 10 small files on C: | <= 1 s | |
| Memory: in-memory doc | file size + ~3% + edits | piece nodes 40 B per 64 KB |
| Memory: on-demand doc | <= 8 MB + edits (+ 4 B per line not needed: counts live in pieces) | |

Anything over 0.5 s on the UI thread shows a progress window with Cancel (15.10). Anything on M: assumes 1 MB/s.

## 4. Architecture

### 4.1 Layers and modules

```
 +----------------------------------------------- win layer (src/win, VC6 only) ------------------------------------+
 | main.cpp  WinMain, single instance, message loop (MsgWaitForMultipleObjects), SEH guard, idle work               |
 | frame.cpp toolbar, tab bar, status bar, notify bar, layout   tabs.cpp Tab lifecycle   cmdtable.cpp command routing |
 | textview.cpp  hexview.cpp  render.cpp (fonts, runs)   clip.cpp   winenc.cpp (ACP/OEM/DBCS/IME code pages)       |
 | filecmds.cpp  editcmds.cpp  doccmds.cpp  docio.cpp (open/save/reload)   largefile.cpp (indexer thread)          |
 | dlgfind.cpp  isearch.cpp  fifpanel.cpp  fifworker.cpp   dlggoto.cpp  dlgopts.cpp  dlgmisc.cpp  progress.cpp      |
 | sessmgr.cpp (session, backup, recovery, exit)   iothread.cpp (save/backup/session writer)   watch.cpp           |
 | fs_win32.cpp (FileSys)   selftest.cpp   bench.cpp   util.cpp                                                     |
 +------------------------------------------------------------------------------------------------------------------+
            | calls (no core header includes windows.h)
 +----------------------------------------------- core (src/core, VC6 + clang C++98) ------------------------------+
 | etypes emem estr esource | ptree undo doc docbind marks | codec cptables detect ucd ucdtables | layout           |
 | lexer lexgen lexhtml lexmd (+ langs/*.lng) | regex rxvm rxbt rxpre | search fif | hexfmt | edtype edlines edhex  |
 | ini config session | fsys.h faultfs fileops                                                                      |
 +------------------------------------------------------------------------------------------------------------------+
```

Dependency rules (enforced by review and by the Mac build, which has no Windows headers):
- `core` never includes `windows.h` or any `src/win` header. Files under `src/core` include only `src/core` headers and
  the C library (`string.h`, `stdlib.h`, `stdio.h` for sprintf of floats only, `stdarg.h`).
- Within core, lower modules don't include higher ones: etypes/emem/estr < esource < ptree < undo < doc < docbind
  (doc also uses codec; docbind uses lexer.h);
  codec < detect; ucd < codec; layout uses codec + ucd + esource; lexer uses ucd only (and LineProvider);
  regex uses codec + ucd + esource; search uses regex + codec + hexfmt; fif uses search + detect + fsys;
  edops (edtype/edlines/edhex) use doc + codec + ucd + lexer; fileops uses fsys + esource + detect + ptree + session.
- Win modules talk to each other only through the headers in `src/win` declared in Wave 0 (15.1).

### 4.2 Data flow
- **Open:** `docio_open` -> `file_load` (core, through `fs_os()`) reads the file into a `MemSource` (or opens a
  `FileSource` for on-demand) and runs `detect` on the first 64 KB -> `Document::init_source` builds the piece tree
  (counting breaks) -> a `Tab` gets the document, a `Highlighter` (via `DocLines`), `LineMarks`, `RangeMarks` -> the
  view attaches.
- **Edit:** view or command builds an `EdCtx` from the tab -> `ed_*` (core) -> `Document::replace` (the only mutator)
  -> piece tree edit + undo record -> `EditEvent` to listeners: views (invalidate), `DocLines` -> Highlighter
  (`lines_changed`), marks (shift), `sessmgr` (backup timer), frame (modified marker, title).
- **Paint:** text view: for each visible row: `Document::read` the line bytes (at most 64 KB window) -> `LineLayout::build`
  -> `Highlighter::style_line` -> merge selection/marks/brace -> `render_runs` -> `ExtTextOutW`.
- **Save:** `docio_save` -> snapshot of the piece list -> save worker writes the temp file -> UI thread performs the
  rename sequence -> `Document::set_saved` (at the undo sequence captured at snapshot time).
- **Search:** `Searcher` (core) over `Document::bytes()` + codec; results become a selection or `RangeMarks`.

### 4.3 Threads
Win98 GDI/USER calls go through the Win16Mutex; worker threads never call USER/GDI except `PostMessage`. All threads
are created with `_beginthreadex` and the multithreaded CRT (`/MT`).

| Thread | Owner | Work | Talks to UI by |
|---|---|---|---|
| UI | main.cpp | everything else, incl. lexing in idle slices and searching in the current document | - |
| Indexer (one per loading on-demand document) | largefile.cpp | `file_index`: own file handle, counts breaks per 64 KB block | `WM_APP_INDEX` progress/done, payload heap-allocated, freed by the receiver |
| I/O worker (one) | iothread.cpp | FIFO jobs: save temp file, write backup generation, write session file, delete files | `WM_APP_IODONE` with job id + result |
| Find in Files (one at a time) | fifworker.cpp | `fif_run` (walk, read, detect, search) | `WM_APP_FIF` batches of up to 50 hits, progress, done |

Rules:
- Cancel flags are `volatile i32` set with `InterlockedExchange`. A thread never frees UI-owned memory; payloads are
  allocated by the sender and freed by the receiver.
- Core objects are not shared between threads, with exactly two documented exceptions:
  1. `SnapshotSource` (6.5) reads a document's `MemSource` (immutable after load) and its `AddBuffer` chunks
     (append-only, fixed chunk table, never moved) from the I/O thread while the UI keeps editing other bytes.
  2. Codec singletons and generated tables are immutable after `codec_init()` (called once at startup on the UI thread
     before any worker starts). DBCS providers build their tables eagerly at registration.
- On-demand documents: the save job opens its own `FileSource` on the original path (a second read handle; our first
  handle allows shared reading). The indexer also uses its own handle.
- A tab with pending I/O jobs cannot be freed: `tab_close` cancels and waits for that tab's jobs (they check the cancel
  flag between 64 KB chunks).
- File watching needs no thread: the main loop waits on up to 16 change-notification handles with
  `MsgWaitForMultipleObjects` and a 3 s polling timer (13.9).

## 5. Core conventions

### 5.1 Types and errors
- `u8 u16 u32 i32 i64 u64` from `etypes.h`. `epos` (u32) is a byte offset; documents are at most `EPOS_MAX`
  (0xFFFFFFFE) bytes. `EPOS_NONE` (0xFFFFFFFF) means "no position". Never use signed arithmetic on offsets; check
  `a > EPOS_MAX - b` before `a + b`.
- `ecp` (u32) is a decoded character: a Unicode scalar value, or `ECP_BADBYTE(b)` (0x110000 + b) for an undecodable
  byte, or `ECP_BADUNIT(u)` (0x120000 + u) for a lone UTF-16 surrogate unit. Bad markers encode back to their exact
  bytes in every codec.
- Functions return `int`: `E_OK` (0), a positive count/flag where documented, or a negative `E_*` code (6.1). No
  exceptions, no RTTI, no `dynamic_cast`. Out-of-memory never crashes: the operation fails, leaves the object in its
  previous state and returns `E_NOMEM`.
- Ownership is stated in each signature comment ("owned" = callee takes ownership and frees with the matching
  allocator).

### 5.2 Memory
- `e_alloc/e_realloc/e_free` wrap malloc (NULL on failure, never throw). Big buffers (file contents, block cache,
  add-buffer chunks) use `e_valloc/e_vfree` (VirtualAlloc on Win32; malloc on POSIX).
- Objects with constructors are created with `new` only at the top level (Document, PieceTree, Highlighter, views) and
  the result is checked for NULL. Containers use `Vec<T>` (POD elements only). No STL anywhere (core or win).
- Tests can make the Nth allocation fail (`e_test_fail_after`) to exercise rollback paths; `e_alloc_count` checks leaks.

### 5.3 VC6 dialect rules (all code that VC6 compiles: core, win, tests except `tests/mac/`)
1. Declare loop variables before the loop: `u32 i; for (i = 0; ...)`. VC6 leaks `for` scope and clang can't emulate it;
   `tools/lint.py` rejects `for (int`-style declarations.
2. No in-class `static const int X = 1;` (use `enum`). No `long long` outside `etypes.h` (`i64/u64`). No `<stdint.h>`,
   `snprintf`, `nullptr`, `std::`, member templates, partial specialisation, `typename` tricks, default template
   arguments, `explicit` conversions operators, variadic macros.
3. Simple class templates (`Vec<T>`) and function templates (`emin/emax`) are fine; define them in the header.
4. Use `es_fmt` instead of `sprintf` for strings (VC6 `_snprintf` doesn't terminate). `sprintf` is allowed only for
   floating point into a buffer of at least 64 bytes (data inspector).
5. 64-bit: VC6 can't convert `unsigned __int64` to `double` (C2520): convert through `i64` when the value is < 2^63.
   Print 64-bit numbers with `es_u64_to_dec`, never `%I64d`/`%lld`.
6. Source files are **ASCII only** (use `\xNN` escapes in strings) and have no BOM. `.rc`, `.rc2`, `resource.h`,
   `res/*.h`, `Makefile`, `*.lng`, `help.md` use CRLF line ends; `.cpp/.h` may use LF or CRLF.
7. Warnings: VC6 `/W3` with zero warnings; clang `-Wall -Wextra -Werror -Wshadow` (plus `-pedantic`).
8. `wchar_t` is not a distinct type in VC6: use `u16` in core; `WCHAR` only in win code.
9. Never pass `FILE_SHARE_DELETE`. Never call a W function except `ExtTextOutW`, `GetTextExtentPoint32W`,
   `GetCharWidthW`, `TextOutW`, clipboard `CF_UNICODETEXT` handling, `MultiByteToWideChar`, `WideCharToMultiByte`,
   `lstrlenW`, `MessageBoxW` (not used). The probe list of stubs (3.1) is binding.
10. Win98 has a 64 KB GDI heap for fonts/brushes/pens: create GDI objects once (render.cpp caches), never per paint.

## 6. Core interfaces

These declarations are copied into the Wave 0 headers verbatim (comments may be shortened). Later tasks may add
private helpers in their `.cpp` files and new members at the **end** of a class they own; changing a declared signature
needs the overseer (note it in HANDOFF.md). Include guards: `PICA_<NAME>_H`.

### 6.1 etypes.h
```cpp
typedef unsigned char  u8;   typedef unsigned short u16;
typedef unsigned int   u32;  typedef signed int     i32;
#if defined(_MSC_VER)
typedef __int64 i64;  typedef unsigned __int64 u64;
#else
__extension__ typedef long long i64;  __extension__ typedef unsigned long long u64;
#endif
typedef u32 epos;   // byte offset in a document
typedef u32 ecp;    // decoded character or bad marker
#define EPOS_NONE 0xFFFFFFFFu
#define EPOS_MAX  0xFFFFFFFEu
#define ECP_BADBYTE(b) (0x110000u + (u32)(b))
#define ECP_BADUNIT(u) (0x120000u + (u32)(u))
#define ECP_IS_BAD(c)  ((c) >= 0x110000u)
#define ECP_REPLACEMENT 0xFFFDu
enum EErr {
  E_OK = 0, E_NOMEM = -1, E_RANGE = -2, E_IO = -3, E_ALIGN = -4, E_READONLY = -5, E_SYNTAX = -6,
  E_CANCEL = -7, E_TOOBIG = -8, E_BUSY = -9, E_NOTFOUND = -10, E_FORMAT = -11, E_ACCESS = -12,
  E_SHARING = -13, E_DISKFULL = -14, E_EXISTS = -15, E_BUDGET = -16, E_NOTIMPL = -17, E_STATE = -18,
  E_UNSUPPORTED = -19
};
const char* e_errname(int e);                 // "E_NOMEM" etc.
template <class T> inline T emin(T a, T b) { return a < b ? a : b; }
template <class T> inline T emax(T a, T b) { return a > b ? a : b; }
void e_assert_fail(const char* file, int line, const char* expr);   // E_DEBUG builds: print + count
#ifdef E_DEBUG
#define EASSERT(x) do { if (!(x)) e_assert_fail(__FILE__, __LINE__, #x); } while (0)
#else
#define EASSERT(x) do { } while (0)
#endif
u32 e_assert_count();
struct MarkRange { epos pos; epos len; };    // search hits / Mark All ranges (used by search.h and marks.h)
class ProgressCb {                            // long operations report here; return nonzero to cancel
public:
  virtual ~ProgressCb() {}
  virtual int progress(u32 done, u32 total) = 0;   // units chosen by the caller (bytes)
};
```

### 6.2 emem.h
```cpp
void* e_alloc(u32 n);                 // NULL on failure; e_alloc(0) returns a unique non-NULL block
void* e_realloc(void* p, u32 n);      // NULL on failure (p unchanged)
void  e_free(void* p);                // NULL ok
void* e_valloc(u32 n);                // big blocks: VirtualAlloc (Win32) / malloc (POSIX); NULL on failure
void  e_vfree(void* p, u32 n);
void  e_test_fail_after(i32 n);       // n-th following e_alloc/e_realloc/e_valloc fails; n < 0 disables
u32   e_alloc_count();                // live blocks (tests check for leaks)

template <class T> class Vec {        // POD elements; copying a Vec is forbidden
public:
  Vec();  ~Vec();
  u32  size() const;  bool empty() const;
  T*   data();  const T* data() const;
  T&   operator[](u32 i);  const T& operator[](u32 i) const;
  T&   back();
  int  reserve(u32 n);                // E_OK / E_NOMEM
  int  resize(u32 n);                 // new elements zero-filled
  int  push(const T& v);
  void pop();
  int  insert(u32 at, const T* p, u32 n);
  void erase(u32 at, u32 n);
  void clear();                       // size 0, keeps capacity
  void release();                     // frees storage
  void swap(Vec<T>& o);
private:
  T* d_; u32 n_; u32 cap_;
  Vec(const Vec<T>&); Vec<T>& operator=(const Vec<T>&);
};
class EBytes : public Vec<u8> { public: int append(const void* p, u32 n); };
```

### 6.3 estr.h
```cpp
u32   es_len(const char* s);
void  es_copy(char* dst, u32 cap, const char* src);        // truncates, always NUL-terminates
void  es_cat(char* dst, u32 cap, const char* src);
int   es_icmp(const char* a, const char* b);                // ASCII case-insensitive
int   es_nicmp(const char* a, const char* b, u32 n);
int   es_fmt(char* dst, u32 cap, const char* fmt, ...);     // %s %c %d %u %x %X %%, width, '0' and '-' flags; returns length
int   es_vfmt(char* dst, u32 cap, const char* fmt, va_list ap);
u32   es_u64_to_dec(u64 v, char* out);                      // out >= 21 bytes
char* es_trim(char* s);                                     // in place
int   es_kv(char* line, char** key, char** val);            // "key=value" (both trimmed); 0 if not a key line
int   es_parse_u32(const char* s, u32* out, int base);      // base 0: "0x1F", "1Fh" hex, else decimal; E_SYNTAX/E_RANGE
int   es_parse_i32(const char* s, i32* out);
int   es_wildcard(const char* mask, const char* name);      // DOS * and ?, case-insensitive; "*.*" matches all
u32   es_utf8_encode(ecp c, char* out);                     // 1..4 bytes
int   es_utf8_to_ecp(const char* s, u32 n, Vec<ecp>* out);  // lenient (bad bytes -> ECP_BADBYTE)
class EStr {
public:
  EStr(); ~EStr();
  const char* c_str() const;  u32 len() const;
  int  append(const char* s);  int appendn(const char* s, u32 n);  int appendf(const char* fmt, ...);
  void clear();  char* detach();    // caller frees with e_free
private:
  char* p_; u32 n_; u32 cap_;
  EStr(const EStr&); EStr& operator=(const EStr&);
};
```

### 6.4 esource.h
```cpp
class ByteSource {
public:
  virtual ~ByteSource() {}
  virtual epos size() const = 0;
  // Pointer to bytes [off, off + *avail), *avail >= 1. Valid until the next get() on this object or until the
  // underlying data changes. Returns NULL with *avail = 0 if off >= size() or on I/O error (err() != E_OK).
  virtual const u8* get(epos off, epos* avail) = 0;
  virtual int err() const { return E_OK; }
  int read(epos off, u8* out, epos n);          // helper over get(); E_RANGE, E_IO
};
class MemSource : public ByteSource {           // owns its block; immutable after adopt/copy (thread-shareable)
public:
  MemSource();  ~MemSource();
  int  adopt(u8* data, epos n, int is_valloc);  // takes ownership (e_free or e_vfree)
  int  copy_from(const u8* data, epos n);
  epos size() const;  const u8* get(epos off, epos* avail);  const u8* data() const;
};
class ViewSource : public ByteSource {          // non-owning view of memory
public:
  ViewSource(const u8* p, epos n);
  epos size() const;  const u8* get(epos off, epos* avail);
};
enum SynthKind { SYN_TEXT_CRLF = 0, SYN_TEXT_LF = 1, SYN_BINARY = 2, SYN_LONGLINE = 3, SYN_UTF8 = 4 };
class SynthSource : public ByteSource {         // deterministic pseudo content of any size (tests, bench)
public:
  SynthSource(epos size, u32 seed, int kind);
  epos size() const;  const u8* get(epos off, epos* avail);   // generated into an internal 64 KB block
  static u8 byte_at(epos off, u32 seed, int kind);            // reference for tests
};
```

### 6.5 ptree.h
```cpp
enum { PT_PIECE_MAX = 65536, PT_ADD_CHUNK = 262144, PT_ADD_MAX_CHUNKS = 16384 };
struct BreakSpec { u8 unit; u8 b0; u8 b1; u8 pad; };  // unit 1: byte b0 (0x0A or 0x0D); unit 2: bytes b0,b1 at even offsets
struct PieceRef  { u16 src; u16 flags; u32 start; u32 len; u32 nbreaks; };  // src 0 = original, n >= 1 = add chunk n-1
struct PSpan     { const u8* p; epos len; u16 src; };   // src != 0 means bytes added since load (hex "modified")
typedef Vec<PieceRef> PieceVec;

class AddBuffer {                 // append-only; chunk table allocated once; chunks never move or shrink
public:
  AddBuffer();  ~AddBuffer();
  int  append(const u8* p, epos n, PieceVec* refs);   // splits at chunk ends and PT_PIECE_MAX; nbreaks left 0
  const u8* chunk(u16 src) const;                     // src >= 1
  u32  bytes_used() const;
};

class PieceTree {                 // treap with split/merge; nodes in a pool; subtree sums of len and nbreaks
public:
  PieceTree();  ~PieceTree();
  int  init(ByteSource* original, const BreakSpec& bs, ProgressCb* cb);  // original not owned; counts breaks
  int  init_uncounted(ByteSource* original, const BreakSpec& bs);        // pieces with nbreaks 0 (indexing pending)
  int  set_counts(const u32* block_breaks, u32 nblocks);                  // per 64 KB block of the original
  int  set_break_spec(const BreakSpec& bs, ProgressCb* cb);               // recount every piece
  void clear();
  epos length() const;
  u32  break_count() const;
  const BreakSpec& break_spec() const;
  int  insert(epos pos, const u8* p, epos n, PieceVec* added);   // added: refs created (undo); may be NULL
  int  insert_refs(epos pos, const PieceRef* refs, u32 n);       // re-insert removed pieces (undo/redo)
  int  remove(epos pos, epos n, PieceVec* removed);              // removed: refs taken out; may be NULL
  int  read(epos pos, u8* out, epos n);                          // E_RANGE, E_IO
  int  span(epos pos, PSpan* s);                                 // contiguous bytes from pos to its piece end
  epos break_pos(u32 b);                                         // offset of break unit b; EPOS_NONE if none
  u32  breaks_before(epos pos);                                  // break units ending at or before pos
  int  snapshot(PieceVec* out) const;                            // piece list in order (for SnapshotSource)
  ByteSource* original() const;
  void set_original(ByteSource* s);                              // same bytes, new object (after on-demand save)
  AddBuffer* add_buffer();
  u32  piece_count() const;
  int  check(char* why, u32 cap);                                // invariants (order, heap, sums, caps); 0 = ok
  void dump(EStr* out);
};

class SnapshotSource : public ByteSource {   // reads a frozen piece list; usable from the I/O thread (4.3)
public:
  SnapshotSource();  ~SnapshotSource();
  int  init(const PieceVec& pieces, const AddBuffer* add, ByteSource* original_private);  // copies the list
  epos size() const;  const u8* get(epos off, epos* avail);  int err() const;
};
```
Invariants: every piece has `1 <= len <= PT_PIECE_MAX`; for unit-2 specs every piece start and length is even;
`nbreaks` = number of break units inside the piece (unit-2: only at even offsets relative to the piece start, which
equals even absolute offsets because of the alignment rule); subtree sums are exact. Adjacent pieces may be merged when
contiguous in the same source and the result stays <= PT_PIECE_MAX (optional).

### 6.6 undo.h
```cpp
enum UndoKind { UK_OTHER = 0, UK_TYPING = 1, UK_DELETE = 2, UK_BACKSPACE = 3, UK_HEXTYPE = 4 };
struct UndoSel  { epos anchor; epos caret; };
struct UndoOp   { u8 kind; u8 pad[3]; epos pos; epos len; u32 ref_first; u32 ref_count; };
// kind 1 insert, 2 delete, 3 state change (encoding/break mode, 7.5): pos = old EncSpec packed (kind | bom<<8 | cp<<16),
// len = new EncSpec packed, ref_first = old EolStyle, ref_count = new EolStyle
struct UndoStep { const UndoOp* ops; u32 nops; const PieceRef* refs; UndoSel before; UndoSel after; };
class UndoLog {
public:
  UndoLog();  ~UndoLog();
  void clear();
  int  begin(u32 kind, const UndoSel& before, u32 now_ms);   // returns 1 if it reopened (merged into) the last group
  int  rec_insert(epos pos, epos len, const PieceVec& added);
  int  rec_delete(epos pos, epos len, const PieceVec& removed);
  int  rec_state(u32 old_enc, u32 new_enc, u32 old_eol, u32 new_eol);   // kind 3 op
  void end(const UndoSel& after);
  void break_merge();
  bool can_undo() const;  bool can_redo() const;
  int  pop_undo(UndoStep* out);   // the group moves to the redo side; Document applies ops in reverse
  int  pop_redo(UndoStep* out);
  void set_save_point();  bool at_save_point() const;  void invalidate_save_point();
  u32  seq() const;               // id of the current state (changes on every group, undo, redo)
  void set_save_point_seq(u32 s); // mark a past state as saved (async save completes later)
  u32  memory_bytes() const;
};
```
Merge rule: `begin(kind, before, now)` merges into the previous group iff kind != UK_OTHER, kind equals the previous
group's kind, merging was not broken, `before.anchor == before.caret == previous.after.caret`, and
`now - previous_end_time <= 2000` ms. A new group discards the redo side (if the save point was on the redo side it
becomes invalid: "modified" stays true until the next save).

### 6.7 doc.h
```cpp
enum EolStyle { EOL_CRLF = 0, EOL_LF = 1, EOL_CR = 2 };
struct EolCounts { u32 crlf; u32 lf; u32 cr; };   // LF mode: CRLF pairs, bare LFs, lone CRs. CR mode: cr = breaks
struct Selection { epos anchor; epos caret; };
inline epos sel_lo(const Selection& s) { return s.anchor < s.caret ? s.anchor : s.caret; }
inline epos sel_hi(const Selection& s) { return s.anchor < s.caret ? s.caret : s.anchor; }
inline bool sel_empty(const Selection& s) { return s.anchor == s.caret; }
struct EditEvent { epos pos; epos removed; epos inserted; u32 line; u32 lines_removed; u32 lines_added; u32 flags; };
enum { EV_UNDO = 1, EV_REDO = 2, EV_RESET = 4, EV_INDEXED = 8 };
class Document;
class DocListener { public: virtual ~DocListener() {} virtual void doc_changed(Document* d, const EditEvent& e) = 0; };
struct ConvertReport { u32 unrepresentable; u32 invalid_src; epos first_bad; };

class Document {
public:
  Document();  ~Document();
  // lifecycle
  int  init_empty(const EncSpec& enc, EolStyle eol);                       // writes the BOM if enc.bom
  int  init_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb); // src owned
  int  init_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol);         // on-demand
  int  finish_indexing(const u32* block_breaks, u32 nblocks, const EolCounts& eol);                   // EV_INDEXED
  int  reset_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb); // reload: clears undo, EV_RESET
  int  reload_as_edit(ByteSource* src);    // undoable reload: replaces all bytes in one group (small docs)
  bool indexing() const;
  ByteSource* source() const;              // original (MemSource / FileSource)
  ByteSource* bytes();                     // live view of the current bytes (UI thread only)
  int  snapshot(SnapshotSource* out, ByteSource* original_private);  // for the I/O thread (in-memory: pass NULL)
  // size and lines (line = 0-based)
  epos length();
  u32  line_count();                       // breaks + 1
  epos line_start(u32 line);
  epos line_end(u32 line);                 // offset of the EOL (the CR of CRLF in LF mode) or length()
  epos line_next(u32 line);                // start of line + 1, or length() for the last line
  u32  line_of(epos pos);
  epos text_origin() const;                // BOM length: text view never shows [0, origin)
  // bytes and characters
  int  read(epos pos, u8* out, epos n);
  int  span(epos pos, PSpan* s);
  u32  decode(epos pos, ecp* cp);          // bytes used (>= 1), 0 at end
  epos char_next(epos pos);
  epos char_prev(epos pos);                // never below text_origin()
  epos snap_char(epos pos);                // start of the character containing pos
  // editing: the only mutators (with undo/redo/reset)
  int  replace(epos pos, epos del, const u8* ins, epos ins_len);           // E_READONLY, E_ALIGN, E_RANGE, E_TOOBIG, E_NOMEM
  int  replace_text(epos pos, epos del, const ecp* s, u32 n, u32* unrepresentable);  // encodes; unrepresentable -> E_RANGE, nothing changed
  void begin_edit(u32 kind, const Selection& before, u32 now_ms);
  void end_edit(const Selection& after);
  int  undo(Selection* sel);  int redo(Selection* sel);
  bool can_undo() const;  bool can_redo() const;
  void break_undo_merge();  void clear_undo();
  bool modified() const;
  void set_saved();                        // current state is on disk
  u32  undo_seq() const;  void set_saved_seq(u32 seq);   // async save: state captured at snapshot time
  void set_always_modified(bool on);       // restored from backup / file deleted on disk
  // encoding and line ends
  const EncSpec& enc() const;  const Codec* codec() const;
  int  reinterpret(const EncSpec& enc, epos bom_len, ProgressCb* cb);      // bytes unchanged, recount; EV_RESET; not undoable
  int  check_convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb);
  int  convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb);     // one undo group; E_TOOBIG for on-demand docs
  EolStyle eol_style() const;  void set_eol_style(EolStyle s);             // style for new line breaks
  const EolCounts& eol_counts() const;  bool eol_mixed() const;
  int  convert_eol(EolStyle to, ProgressCb* cb);                            // one undo group; may change the break mode
  const u8* eol_bytes(u32* n) const;       // newline for eol_style() encoded in the document encoding
  // state
  void set_read_only(bool ro);  bool read_only() const;
  void set_busy(bool busy);                // saving: edits refused with E_BUSY
  bool editable() const;                   // !read_only && !indexing && !busy
  int  add_listener(DocListener* l);  void remove_listener(DocListener* l);
  PieceTree* tree();
  u32  change_seq() const;                 // increments on every content change
};
```

### 6.8 docbind.h and marks.h
```cpp
// docbind.h: feeds a Highlighter from a Document (LineProvider + DocListener adapter)
class DocLines : public LineProvider, public DocListener {
public:
  DocLines();  ~DocLines();
  void bind(Document* d, Highlighter* h);          // registers as listener; h may be NULL
  void unbind();
  u32  lp_count();
  int  lp_get(u32 line, ecp* out, u32 cap, u32* n); // decodes up to cap chars of the line (EOL excluded)
  void doc_changed(Document* d, const EditEvent& e);// -> h->lines_changed / lines_reset
};
// marks.h
class LineMarks : public DocListener {             // bookmarks: sorted unique line numbers
public:
  void attach(Document* d);  void detach();
  int  toggle(u32 line);  bool has(u32 line) const;
  u32  next(u32 line) const;  u32 prev(u32 line) const;   // wrap around; EPOS_NONE if empty
  void clear();  u32 count() const;  u32 at(u32 i) const;  int set(const u32* lines, u32 n);
  void doc_changed(Document* d, const EditEvent& e);      // lines inside a removed range move to its first line (deduped)
};
class RangeMarks : public DocListener {            // Mark All / result highlights: sorted, non-overlapping, max 10,000
public:
  void attach(Document* d);  void detach();
  int  set(const MarkRange* r, u32 n);  void clear();  u32 count() const;
  const MarkRange& at(u32 i) const;
  u32  first_ending_after(epos pos) const;          // binary search for painting
  void doc_changed(Document* d, const EditEvent& e);// shift after edit; drop ranges overlapping the edit
};
```

### 6.9 codec.h
```cpp
enum EncKind { EK_SBCS = 0, EK_DBCS = 1, EK_UTF8 = 2, EK_UTF16LE = 3, EK_UTF16BE = 4 };
struct EncSpec { u8 kind; u8 bom; u16 cp; };   // cp: 1252, 437, 1251, 932, 65001 (UTF-8), 1200 (LE), 1201 (BE)
bool    enc_equal(const EncSpec& a, const EncSpec& b);
void    enc_set_system(u16 acp, u16 oemcp);    // once at startup (tests: 1252, 437)
EncSpec enc_ansi();  EncSpec enc_oem();
EncSpec enc_utf8(int bom);  EncSpec enc_utf16le(int bom);  EncSpec enc_utf16be(int bom);
EncSpec enc_cp(u16 cp);                        // SBCS or DBCS by code page
const char* enc_label(const EncSpec& e, char* buf, u32 cap);  // status bar: "ANSI", "OEM", "UTF-8", "UTF-8 BOM", "UTF-16 LE", "Windows-1251"
int     enc_parse(const char* token, EncSpec* out);           // ini tokens: ansi, oem, utf8, utf8bom, utf16le, utf16be, cp1251
void    enc_token(const EncSpec& e, char* buf, u32 cap);
u32     enc_bom(const EncSpec& e, u8* out);    // writes the BOM bytes (0, 2 or 3), returns length

class Codec {
public:
  virtual ~Codec() {}
  u8 kind; u8 unit; u8 max_len; u8 ascii_compat; u8 self_sync; u8 pad; u16 cp;
  // Decode one character at p. avail = bytes available at p; eof = nothing follows p + avail.
  // Returns bytes consumed (>= 1) and sets *cp (a bad marker for invalid input). Returns 0 only when the
  // sequence is incomplete, avail is short and !eof (caller supplies more bytes).
  virtual u32 decode(const u8* p, u32 avail, int eof, ecp* cp) const = 0;
  // Encode into out[0..3]. Returns length, or 0 if the character is not representable.
  // Bad markers always encode back to their original byte (BADBYTE) or unit (BADUNIT, 2 bytes).
  virtual u32 encode(ecp cp, u8* out) const = 0;
};
int          codec_init();                       // build reverse tables; UI thread, before any worker
const Codec* codec_get(const EncSpec& e);        // NULL if unsupported (e.g. DBCS without a provider)
int          codec_sbcs_supported(u16 cp);
void         codec_break_spec(const EncSpec& e, int cr_mode, BreakSpec* bs);
class DbcsProvider {                             // implemented by winenc.cpp (MultiByteToWideChar tables)
public:
  virtual ~DbcsProvider() {}
  virtual int is_lead(u8 b) const = 0;
  virtual ecp decode_pair(u8 lead, u8 trail) const = 0;   // bad marker if invalid
  virtual ecp decode_single(u8 b) const = 0;
  virtual u32 encode(ecp c, u8* out) const = 0;            // 0 if not representable
};
int codec_register_dbcs(u16 cp, DbcsProvider* p);         // provider not owned; must outlive all codecs
// helpers
u32  codec_decode_buf(const Codec* c, const u8* p, u32 n, int eof, ecp* out, u32 cap, u32* bytes_used);
int  codec_encode_str(const Codec* c, const ecp* s, u32 n, EBytes* out, u32* unrepresentable, ecp subst);  // subst 0 = stop
epos codec_prev(ByteSource* s, const Codec* c, epos pos, epos floor);   // char start before pos; floor = known boundary
class CharReader {                               // forward decoding across ByteSource chunks
public:
  void init(ByteSource* s, const Codec* c, epos pos, epos end);
  int  next(ecp* cp, epos* at);                  // 1 char, 0 end, < 0 error
  epos pos() const;
};
```
Supported SBCS tables (generated by `tools/gen_cptables.py` from Python's codecs into `cptables.cpp`): 1250, 1251,
1252, 1253, 1254, 1255, 1256, 1257, 1258, 874, 437, 850, 852, 855, 857, 860, 861, 862, 863, 865, 866, 869, 20866
(KOI8-R), 21866 (KOI8-U), 28591-28599, 28605. Bytes undefined in a code page decode to `ECP_BADBYTE` (lossless). C0
bytes decode to themselves in every SBCS table (for OEM, 0x00-0x1F are controls, not the CP437 graphics).

### 6.10 detect.h
```cpp
struct DetectOpts   { u8 utf8_nobom; u8 utf16_nobom; u8 pad[2]; EncSpec ansi; EolStyle default_eol; };
struct DetectResult { EncSpec enc; epos bom_len; u8 binary; u8 pure_ascii; u8 utf32; u8 eol_mixed;
                      EolCounts eol; EolStyle eol_style; };
int detect(const u8* p, u32 n, int whole_file, const DetectOpts& o, DetectResult* r);
int detect_eol(const u8* p, u32 n, const EncSpec& e, EolCounts* c);    // counts in code units of e
EolStyle detect_dominant_eol(const EolCounts& c, EolStyle dflt);
int detect_is_binary(const u8* p, u32 n);   // find-in-files skip: NUL in the sample and not UTF-16
```
Rules, in order (sample = first 64 KB):
1. BOM: `EF BB BF` UTF-8 (bom_len 3); `FF FE 00 00` UTF-32 LE -> `utf32 = 1, binary = 1`; `00 00 FE FF` same;
   `FF FE` UTF-16 LE; `FE FF` UTF-16 BE (bom_len 2).
2. If `utf16_nobom` and the sample has >= 16 bytes: count NULs at even and odd offsets over the pairs. Odd-NUL ratio
   >= 40% and even-NUL ratio <= 5% -> UTF-16 LE; the mirror -> UTF-16 BE.
3. Any NUL byte left -> `binary = 1`, encoding = ANSI.
4. If `utf8_nobom`, the sample has at least one valid multi-byte sequence and no invalid sequence (a truncated
   sequence at the very end of a partial sample is allowed) -> UTF-8 without BOM.
5. Only bytes < 0x80 -> ANSI with `pure_ascii = 1`. 6. Otherwise ANSI.
EOL: count CRLF, bare LF and lone CR in code units. If `cr > 0 && crlf == 0 && lf == 0` -> `EOL_CR` (CR mode).
Otherwise the larger of crlf/lf wins (tie -> CRLF); no breaks -> `default_eol`. `eol_mixed` = more than one kind > 0.

### 6.11 ucd.h
```cpp
int uc_width(ecp c);         // 0 combining / zero-width, 2 East Asian Wide/Fullwidth, else 1 (callers handle bad markers and controls)
ecp uc_fold(ecp c);          // simple case folding, BMP + supplementary
ecp uc_upper(ecp c);  ecp uc_lower(ecp c);   // simple 1:1 mappings
int uc_is_word(ecp c);       // letter, digit, connector punctuation (_), combining mark
int uc_is_space(ecp c);
int uc_is_drawable(ecp c);   // Courier New (WGL4) can draw it on Win98: U+0020-052F, U+1E00-1EFF, U+2000-25FF, U+FB01-FB02, U+FFFD
```
Tables generated by `tools/gen_ucd.py` from Python's `unicodedata` into `ucdtables.cpp` (sorted ranges, binary search).

### 6.12 layout.h
```cpp
enum { LAY_LONG = 65536, LAY_CHECKPOINT = 4096, LAY_WINDOW = 65536 };
enum LayKind { LK_CHAR = 0, LK_TAB, LK_CTRL, LK_BAD, LK_WIDE, LK_ZERO, LK_SPACE, LK_NODRAW };
struct LayoutOpts { u8 tab_width; u8 show_ws; u8 pad[2]; };
struct LayChar { u32 off; u32 cell; u16 glyph; u8 width; u8 kind; u8 nglyph; u8 pad[3]; };
// off: byte offset relative to the segment start; cell: absolute display cell; glyph: index of the first
// glyph (u16, UTF-16 code unit) in glyphs(); nglyph: glyphs drawn (control picture "NUL" = 3)
class LineLayout {
public:
  LineLayout();  ~LineLayout();
  // Lay out bytes [p, p + n) of one line (EOL excluded) starting at display cell start_cell.
  int  build(const u8* p, u32 n, const Codec* c, const LayoutOpts& o, u32 start_cell, int eof);
  u32  count() const;  const LayChar* chars() const;
  const u16* glyphs() const;  u32 glyph_count() const;
  u32  end_cell() const;
  u32  char_at_offset(u32 off) const;            // char containing byte off; count() if off >= bytes laid out
  u32  cell_of_offset(u32 off) const;            // caret x
  u32  offset_of_cell(u32 cell, int nearest) const;  // hit test: nearest char boundary (or floor)
  int  wrap_rows(u32 width_cells, int word, Vec<u32>* row_starts) const;  // char indices where rows start (first 0)
};
class LongLineIndex {                            // checkpoints every LAY_CHECKPOINT bytes for lines > LAY_LONG
public:
  LongLineIndex();  ~LongLineIndex();
  int  build(ByteSource* s, const Codec* c, const LayoutOpts& o, epos start, epos end, ProgressCb* cb);
  int  at_offset(epos pos, epos* cp_pos, u32* cp_cell) const;   // checkpoint at or before pos
  int  at_cell(u32 cell, epos* cp_pos, u32* cp_cell) const;
  u32  total_cells() const;  epos start() const;  epos end() const;  void clear();
};
const char* lay_ctrl_name(u8 c);                 // "NUL", "SOH", ... "US", "DEL"; "CR" for a lone CR
```
Display rules: TAB advances to the next multiple of `tab_width` (kind LK_TAB, width >= 1, drawn as spaces or as `»`
(U+00BB) + spaces when show_ws). Space drawn as `·` (U+00B7) when show_ws. C0 controls (except TAB) and DEL become
control pictures, width = name length (2-3 cells). A lone CR inside a line (LF mode) is a control picture "CR". Bad
byte: "xHH" (3 cells, uppercase hex). Bad unit: "uHHHH" (5 cells). Wide chars: 2 cells (LK_WIDE). Combining marks
(width 0): appended to the previous char's cell (LK_ZERO, drawn overlapping; GDI on 9x does no composition). Code
points not drawable (`uc_is_drawable` false, e.g. CJK without a CJK font): LK_NODRAW, drawn as a placeholder box of
their width (render decides; a CJK font, if present, is used instead, 10.4). Code points > U+FFFF: LK_NODRAW width 1
(9x GDI has no surrogate support), bytes untouched.

### 6.13 lexer.h
```cpp
enum StyleId {
  ST_DEFAULT = 0, ST_COMMENT, ST_STRING, ST_CHAR, ST_NUMBER, ST_KEYWORD1, ST_KEYWORD2, ST_KEYWORD3, ST_KEYWORD4,
  ST_PREPROC, ST_OPERATOR, ST_LABEL, ST_VARIABLE, ST_SECTION, ST_KEY, ST_TAG, ST_ATTR, ST_ENTITY, ST_HEADING,
  ST_EMPH, ST_CODE, ST_DIFF_ADD, ST_DIFF_DEL, ST_DIFF_HUNK, ST_DIFF_HEADER, ST_INVALID, ST_COUNT
};
enum { LEX_LINE_MAX = 16384 };          // lexers see at most this many chars of a line; the rest is ST_DEFAULT
class Lexer {
public:
  virtual ~Lexer() {}
  virtual u16 lex_line(const ecp* s, u32 n, u16 state, u8* styles) const = 0;   // styles may be NULL; returns end state
};
struct LangInfo {
  char id[16]; char name[32];
  char exts[160];              // "c;cpp;cc;cxx;h;hpp;inl"
  char names[96];              // whole basenames: "makefile;makefile.vc"
  ecp  line_comment[8];  u8 line_comment_len;
  ecp  block_open[4];    u8 block_open_len;
  ecp  block_close[4];   u8 block_close_len;
  u8   tab_width;              // 0 = use the global option
  u8   use_tabs;               // 0 spaces, 1 tabs, 2 global option
  u8   case_sensitive;
  char braces[8];              // "()[]{}"
  const Lexer* lexer;          // owned by the registry
};
class LangSource { public: virtual ~LangSource() {} virtual u32 count() = 0;
                   virtual int get(u32 i, const char** name, const char** text, u32* len) = 0; };
int  lang_init(LangSource* builtin, LangSource* overrides, EStr* errors);  // overrides may be NULL; same file name replaces
void lang_shutdown();
u32  lang_count();  const LangInfo* lang_at(u32 i);  const LangInfo* lang_find(const char* id);
const LangInfo* lang_for_file(const char* basename);   // NULL = plain text
const LangInfo* lang_results();                         // find-results panel lexer (not in menus)
int  lang_parse(const char* text, u32 len, LangInfo* out, EStr* err);   // test entry point (generic languages)

class LineProvider {
public:
  virtual ~LineProvider() {}
  virtual u32 lp_count() = 0;
  virtual int lp_get(u32 line, ecp* out, u32 cap, u32* n) = 0;
};
class Highlighter {
public:
  Highlighter();  ~Highlighter();
  int  attach(LineProvider* lines, const LangInfo* lang);  void detach();
  void set_enabled(bool on);  bool enabled() const;
  void lines_changed(u32 first, u32 removed, u32 added);   // after every edit
  void lines_reset();
  void set_target(u32 line);           // idle work lexes at least to here
  int  work(u32 budget_lines);         // returns 1 while more work remains
  bool state_known(u32 line) const;
  int  style_line(u32 line, const ecp* s, u32 n, u8* styles);   // 1 styled; 0 start state unknown (styles = default)
  int  take_dirty(u32* first, u32* last);   // lines whose styling changed (to repaint); 0 if none
  u32  valid_upto() const;
};
```

### 6.14 regex.h
```cpp
enum RxFlag { RXF_ICASE = 1, RXF_DOTALL = 2, RXF_WORD = 4 };
enum { RX_MAX_PATTERN = 1000, RX_MAX_PROG = 4000, RX_MAX_REPEAT = 1000, RX_GROUPS = 10 };
struct RxError { int code; u32 pos; char msg[96]; };
struct RxMatch { epos pos; epos len; epos gpos[RX_GROUPS]; epos glen[RX_GROUPS]; };   // gpos EPOS_NONE = unset
class RxProg;
int  rx_compile(const ecp* pat, u32 n, u32 flags, RxProg** out, RxError* err);
void rx_free(RxProg* p);
u32  rx_group_count(const RxProg* p);
int  rx_uses_backtracker(const RxProg* p);
int  rx_literal_prefix(const RxProg* p, Vec<ecp>* lit, int* icase);   // required literal at the start (may be empty)
class RxMatcher {                      // per-thread matching state
public:
  RxMatcher();  ~RxMatcher();
  int  init(const RxProg* p);
  void set_budget(u32 steps);         // backtracker step limit per search call (default 10,000,000)
  // Leftmost-first match starting in [from, hi], ending <= hi. lo <= from is the context floor for ^ \b \<.
  // Returns 1 found, 0 none, E_BUDGET, E_CANCEL, E_IO, E_NOMEM.
  int  search(ByteSource* s, const Codec* c, epos lo, epos from, epos hi, RxMatch* m, ProgressCb* cb);
};
```

### 6.15 search.h
```cpp
enum SearchMode { SM_NORMAL = 0, SM_EXTENDED = 1, SM_REGEX = 2, SM_HEX = 3 };
struct SearchOpts  { u8 mode; u8 match_case; u8 whole_word; u8 dot_all; };
struct SearchError { int code; u32 pos; char msg[96]; };
int search_parse_extended(const ecp* in, u32 n, Vec<ecp>* out, SearchError* err);
class Searcher {
public:
  Searcher();  ~Searcher();
  int  compile(const ecp* pat, u32 n, const SearchOpts& o, const EncSpec& doc_enc, SearchError* err);
  int  find(ByteSource* s, const Codec* c, epos lo, epos hi, epos from, int backward, RxMatch* m, ProgressCb* cb);
  int  count(ByteSource* s, const Codec* c, epos lo, epos hi, u32* n, ProgressCb* cb);
  int  find_all(ByteSource* s, const Codec* c, epos lo, epos hi, Vec<MarkRange>* out, u32 max, ProgressCb* cb);
  int  expand(const ecp* rep, u32 n, ByteSource* s, const Codec* c, const RxMatch& m, EBytes* out, u32* unrepresentable);
  int  impossible() const;            // pattern can't occur in this encoding (find returns 0)
  const SearchOpts& opts() const;
};
class EditSink { public: virtual ~EditSink() {} virtual int apply(epos pos, epos del, const u8* ins, epos n) = 0; };
int search_replace_all(Searcher& s, ByteSource* src, const Codec* c, epos lo, epos hi, const ecp* rep, u32 n,
                       EditSink* sink, ProgressCb* cb, u32* replaced, u32* unrepresentable);  // edits back to front
```

### 6.16 fif.h
```cpp
struct FifQuery {
  char folder[260]; char masks[260]; u8 subfolders; u8 pad[3];
  SearchOpts opts; ecp pattern[RX_MAX_PATTERN]; u32 pattern_len;
  DetectOpts det; u32 max_hits;        // default 50,000
};
struct FifHit { u32 file_index; u32 line; u32 col; char text[1025]; u32 text_len; u32 hl_start; u32 hl_len; };
// text: the line in UTF-8, at most 256 chars (tabs kept); hl_*: match position in chars within text
class FifSink {
public:
  virtual ~FifSink() {}
  virtual int  on_file(const char* path, u32 index) = 0;      // a file with hits begins
  virtual int  on_hit(const FifHit& h) = 0;                   // nonzero = stop
  virtual int  on_progress(u32 done, u32 seen, const char* current) = 0;   // nonzero = cancel
  virtual void on_done(int result, u32 searched, u32 files_with_hits, u32 hits) = 0;
};
int fif_mask_match(const char* masks, const char* name);       // "*.cpp;*.h *.rc !*.obj" (; or space; ! excludes)
int fif_run(FileSys* fs, const FifQuery& q, FifSink* sink, volatile i32* cancel);
int fif_search_buffer(const u8* p, u32 n, const DetectResult& det, Searcher& s, u32 file_index, FifSink* sink, u32* hits);
```

### 6.17 hexfmt.h
```cpp
enum { HEX_BPR = 16 };
void hex_offset_text(epos off, char out[9]);            // "00001F40"
void hex_byte_text(u8 b, char out[2]);                  // "4D"
u16  hex_char_glyph(u8 b, const Codec* sbcs);           // char column glyph; '.' for controls/non-printables; sbcs may be NULL (ASCII only)
int  hex_parse_bytes(const char* text, u32 n, EBytes* out);                  // "4D 5A", "4d5a", "0x4D,0x5A", newlines ok
int  hex_parse_pattern(const ecp* text, u32 n, EBytes* bytes, EBytes* mask); // "DE AD ?? EF"
int  goto_parse_offset(const char* expr, epos cur, epos size, epos* out, char* err, u32 errcap);
int  goto_parse_line(const char* expr, u32 cur_line, u32 line_count, u32* line, u32* col, char* err, u32 errcap);
struct InspectOut { char i8[8]; char u8_[8]; char i16[12]; char u16_[12]; char i32_[16]; char u32_[16];
                    char i64[24]; char u64_[24]; char f32[40]; char f64[48]; char bin[12]; u8 avail; };
int  inspect_bytes(const u8* p, u32 avail, int big_endian, InspectOut* out);  // fields "-" when too few bytes
```
Go-to syntax: offset: `1F40`, `0x1F40`, `1F40h` (hex is the default in the hex view), `#8000` or `8000d` decimal,
`+100`, `-0x20` relative to the caret, `end`, `end-16`, `50%`. Line: `120`, `120:5` (line:column), `+10`, `-5`,
`end`. Errors give a short message ("Offset beyond end of file (size 0x1F40)").

### 6.18 edops.h (edtype.cpp, edlines.cpp, edhex.cpp)
```cpp
struct EdLang { ecp lc[8]; u32 lc_len; ecp bo[4]; u32 bo_len; ecp bc[4]; u32 bc_len;
                u8 tab_width; u8 use_spaces; char braces[8]; };
struct EdCtx  { Document* doc; Selection sel; EdLang lang; u8 overwrite; u8 auto_indent; u8 pad[2]; u32 now_ms; };
void ed_lang_from(const LangInfo* li, u8 tab_width, u8 use_spaces, EdLang* out);  // li may be NULL
// edtype.cpp: typing and deleting; all update e->sel; E_OK, E_READONLY, E_BUSY, E_ALIGN, E_NOMEM, E_RANGE (unrepresentable)
int  ed_type(EdCtx* e, ecp c);                    // replaces the selection; overwrite replaces the next char (not the EOL)
int  ed_newline(EdCtx* e);                        // doc EOL + copied indent (auto_indent); breaks undo merging
int  ed_backspace(EdCtx* e);  int ed_delete(EdCtx* e);
int  ed_delete_word(EdCtx* e, int forward);
int  ed_insert_text(EdCtx* e, const ecp* s, u32 n, u32* replaced);  // paste: CR/LF/CRLF -> doc EOL; unrepresentable -> '?' counted
int  ed_insert_bytes(EdCtx* e, const u8* p, epos n);                 // raw paste (same encoding)
epos ed_word_left(Document* d, epos pos);  epos ed_word_right(Document* d, epos pos);
void ed_word_at(Document* d, epos pos, Selection* out);
epos ed_smart_home(Document* d, epos pos);        // first non-blank; at it -> line start
// edlines.cpp: line commands (act on all lines touched by the selection)
int  ed_duplicate(EdCtx* e);                      // selection non-empty: duplicate the selection; else the line
int  ed_delete_lines(EdCtx* e);
int  ed_move_lines(EdCtx* e, int down);
int  ed_toggle_comment(EdCtx* e);                 // line comment if the language has one, else block comment
int  ed_change_case(EdCtx* e, int upper);         // selection, else the char at the caret
int  ed_indent(EdCtx* e, int outdent);            // Tab / Shift+Tab on a multi-line selection
int  ed_match_brace(Document* d, epos pos, Highlighter* hl, const char* braces, epos* brace, epos* match);
// edhex.cpp: hex view edits (HexCaret.pos is a byte offset)
struct HexCaret { epos pos; u8 nibble; u8 pane; u8 insert; u8 pad; epos unit_end; };   // pane 0 hex, 1 chars
int  hx_type_nibble(Document* d, HexCaret* c, Selection* sel, u8 v, u32 now_ms);
int  hx_type_char(Document* d, HexCaret* c, Selection* sel, ecp ch, u32 now_ms);
int  hx_write(Document* d, HexCaret* c, Selection* sel, const u8* p, epos n);   // paste in the current mode
int  hx_delete(Document* d, HexCaret* c, Selection* sel, int backspace);
```

### 6.19 ini.h, config.h, session.h
```cpp
class Ini {                            // own reader/writer; tolerant; keeps order, comments and unknown lines
public:
  Ini();  ~Ini();
  int  parse(const char* text, u32 n);                 // never fails on bad lines (kept verbatim); E_NOMEM only
  const char* get(const char* sec, const char* key) const;                    // NULL if absent
  const char* get_s(const char* sec, const char* key, const char* def) const;
  i32  get_i(const char* sec, const char* key, i32 def, i32 lo, i32 hi) const; // clamped; bad -> def
  u32  get_x(const char* sec, const char* key, u32 def) const;                 // hex or decimal
  int  get_b(const char* sec, const char* key, int def) const;                 // 0/1, yes/no, true/false
  int  set(const char* sec, const char* key, const char* val);
  int  set_i(const char* sec, const char* key, i32 v);  int set_x(const char* sec, const char* key, u32 v);
  int  remove(const char* sec, const char* key);  int remove_section(const char* sec);
  u32  section_count() const;  const char* section(u32 i) const;
  u32  key_count(const char* sec) const;  const char* key_at(const char* sec, u32 i, const char** val) const;
  int  write(EStr* out) const;                          // CRLF
};
// config.h: see Appendix C for every key and default
enum UiColor { UIC_MARK_BG = 0, UIC_BRACE_BG, UIC_BRACE_BAD_FG, UIC_WS_FG, UIC_CTRL_FG, UIC_CTRL_BG, UIC_BAD_FG,
  UIC_BAD_BG, UIC_GUTTER_BG, UIC_GUTTER_FG, UIC_BOOKMARK, UIC_HEX_OFFSET, UIC_HEX_MODIFIED, UIC_HEX_MIRROR_BG,
  UIC_HEX_BOM, UIC_ISEARCH_FAIL, UIC_COUNT };
#define COLOR_SYSDEFAULT 0xFF000000u
enum { CFG_MRU_MAX = 10, CFG_HIST_MAX = 15 };
struct StyleColor { u32 rgb; u8 bold; u8 pad[3]; };   // rgb = 0x00BBGGRR (COLORREF layout) or COLOR_SYSDEFAULT
struct Config { /* fields exactly as Appendix C, grouped General/Editor/Files/Hex/Colors/Window/Find/Lists */ };
void config_defaults(Config* c);
void config_load(Config* c, const Ini& ini);          // starts from defaults; bad values ignored
void config_store(const Config* c, Ini* ini);         // keeps unknown keys already in ini
void config_mru_add(Config* c, const char* path);     // move to front, dedupe (case-insensitive)
void config_hist_add(char (*list)[256], u32 max, const char* s);
// session.h
struct SessionTab {
  u32 id; char path[260]; u8 untitled; u8 view; u8 hex_pane; u8 hex_insert; u32 untitled_no;
  EncSpec enc; u8 enc_forced; u8 eol; u8 lang_forced; u8 wrap; char lang[16];
  u8 tab_width; u8 use_spaces; u8 read_only; u8 modified; u8 has_backup; u8 disk_missing; u8 pad[2];
  epos caret, anchor; u32 top_line, top_subrow; epos hex_top;
  u32 disk_size, disk_mtime_lo, disk_mtime_hi;
  Vec<u32> bookmarks;
};
struct Session { u32 seq; u32 active; u32 next_tabid; u32 next_untitled; u8 clean_exit; u8 pad[3];
                 Vec<SessionTab*> tabs; };
void session_init(Session* s);  void session_free(Session* s);
int  session_to_ini(const Session& s, Ini* ini);
int  session_from_ini(const Ini& ini, Session* s);       // E_FORMAT if "[end] ok=1" is missing
int  session_validate(const u8* p, u32 n, u32* seq);      // for ab_read (13.6)
struct BackupMeta { u32 tabid; u32 gen; u32 seq; char path[260]; u8 untitled; u8 eol; u8 pad[2]; u32 untitled_no;
                    EncSpec enc; char lang[16]; epos caret, anchor;
                    u32 disk_size, disk_mtime_lo, disk_mtime_hi; u32 written_lo, written_hi; };
// backup_header_write/parse live in fileops.h (6.20)
```

### 6.20 fsys.h and fileops.h
```cpp
typedef void* FsFile;
enum FsMode { FS_READ = 1, FS_WRITE = 2, FS_DENY_WRITE = 0x10, FS_SHARE_ALL = 0x20, FS_NEW = 0x40 };
// FS_READ: open existing for reading. FS_WRITE: create/truncate for writing (FS_NEW: fail if exists).
// Sharing: default = share read; FS_DENY_WRITE = share read only (same as default, explicit); FS_SHARE_ALL = share
// read + write. FILE_SHARE_DELETE is never used.
enum FsAttr { FSA_READONLY = 1, FSA_HIDDEN = 2, FSA_SYSTEM = 4, FSA_DIR = 0x10, FSA_ARCHIVE = 0x20 };
struct FsStat { u32 exists; u32 attrs; u32 size_lo, size_hi; u32 mtime_lo, mtime_hi; u32 ctime_lo, ctime_hi; };
struct FsFind { char name[260]; u32 attrs; u32 size_lo, size_hi; void* h; };
class FileSys {                         // thread-safe: no shared mutable state besides OS handles
public:
  virtual ~FileSys() {}
  virtual int  open(const char* path, u32 mode, FsFile* out) = 0;   // E_NOTFOUND E_ACCESS E_SHARING E_EXISTS E_IO
  virtual int  read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got) = 0;
  virtual int  write(FsFile f, const void* buf, u32 n) = 0;         // sequential; E_DISKFULL E_IO
  virtual int  flush(FsFile f) = 0;
  virtual int  size(FsFile f, u32* lo, u32* hi) = 0;
  virtual int  close(FsFile f) = 0;
  virtual int  stat(const char* path, FsStat* st) = 0;              // missing file: E_OK with exists = 0
  virtual int  set_attrs(const char* path, u32 attrs) = 0;
  virtual int  set_ctime(const char* path, u32 lo, u32 hi) = 0;
  virtual int  move(const char* from, const char* to) = 0;          // E_EXISTS if 'to' exists (Win98 semantics)
  virtual int  remove(const char* path) = 0;
  virtual int  copy(const char* from, const char* to) = 0;          // overwrites 'to'
  virtual int  make_temp(const char* dir, const char* prefix3, char* out, u32 cap) = 0;  // creates an empty unique 8.3 file
  virtual int  mkdir(const char* path) = 0;
  virtual int  free_space(const char* dir, u64* bytes) = 0;
  virtual int  find_first(const char* dir, FsFind* f) = 0;          // all entries except . and ..; E_NOTFOUND if none
  virtual int  find_next(FsFind* f) = 0;                            // E_NOTFOUND at end
  virtual void find_close(FsFind* f) = 0;
  virtual int  is_remote(const char* path) = 0;                     // network drive (polling only, longer timeouts)
};
FileSys* fs_os();                       // fs_win32.cpp (VC6) / tests/fs_posix.cpp (Mac)
class FaultFs : public FileSys {        // tests: wraps another FileSys and fails the Nth call of a kind
public:
  enum { F_OPEN = 0, F_READ, F_WRITE, F_FLUSH, F_CLOSE, F_MOVE, F_REMOVE, F_COPY, F_TEMP, F_KINDS };
  FaultFs(FileSys* inner);
  void fail_on(int kind, u32 nth, int err);   // nth = 1-based call count of that kind
  void reset();  u32 calls(int kind) const;
  /* all FileSys methods forward */
};

// fileops.h
enum LoadMode { LM_MEMORY = 0, LM_ONDEMAND = 1 };
struct LoadOpts   { epos ondemand_threshold; DetectOpts det; u8 force_enc; u8 pad[3]; EncSpec enc; u32 cache_blocks; };
struct LoadResult { ByteSource* src; u8 mode; u8 ro_attr; u8 shared_ro; u8 pad; DetectResult det; FsStat st; };
int  file_load(FileSys* fs, const char* path, const LoadOpts& o, ProgressCb* cb, LoadResult* r);
class FileSource : public ByteSource {   // on-demand: read handle + LRU cache of 64 KB blocks (UI thread only)
public:
  FileSource();  ~FileSource();
  int  open(FileSys* fs, const char* path, u32 mode, u32 cache_blocks);
  int  reopen(const char* path);         // same bytes under another name (after save)
  void close();                          // before the rename sequence
  epos size() const;  const u8* get(epos off, epos* avail);  int err() const;  const char* path() const;
};
struct IndexResult { Vec<u32> block_breaks; EolCounts eol; epos size; };
int  file_index(FileSys* fs, const char* path, const BreakSpec& bs, IndexResult* r, ProgressCb* cb, volatile i32* cancel);
struct SaveOpts   { u8 keep_bak; u8 pad[3]; };
class SaveHooks   { public: virtual ~SaveHooks() {} virtual void before_replace() {} virtual void after_replace(const char* old_copy) {} };
struct SaveResult { FsStat st; u8 in_place; u8 failed_step; u8 pad[2]; char temp[260]; char old_copy[260]; };
int  file_write_temp(FileSys* fs, const char* path, ByteSource* data, ProgressCb* cb, volatile i32* cancel, SaveResult* r);  // steps 1-3 (13.3), worker thread
int  file_replace(FileSys* fs, const char* path, const SaveOpts& o, SaveHooks* hooks, SaveResult* r);   // steps 4-9, UI thread
int  file_save(FileSys* fs, const char* path, ByteSource* data, const SaveOpts& o, SaveHooks* hooks, ProgressCb* cb, SaveResult* r); // both (tests, settings)
int  file_read_all(FileSys* fs, const char* path, EBytes* out, u32 max);
int  file_write_direct(FileSys* fs, const char* path, const u8* p, u32 n);   // create/truncate + flush (A/B files)
typedef int (*AbValidator)(const u8* p, u32 n, u32* seq);
int  ab_read(FileSys* fs, const char* p1, const char* p2, AbValidator v, EBytes* out, int* which);  // newest valid
int  ab_write(FileSys* fs, const char* p1, const char* p2, int last_which, const u8* p, u32 n, int* which);  // writes the other one
int  backup_write(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource* data, ProgressCb* cb, volatile i32* cancel);
int  backup_list(FileSys* fs, const char* dir, Vec<BackupMeta>* out);   // newest valid generation per tab id
int  backup_open(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource** data);   // MemSource of the data
int  backup_delete_tab(FileSys* fs, const char* dir, u32 tabid);
void backup_file_name(u32 tabid, u32 gen, char out[13]);              // "T0002A07.BAK": T + 5 hex tab id + 2 hex gen (8.3)
int  backup_header_write(const BackupMeta& m, epos data_len, EStr* out);   // ends with "@DATA <n>\r\n"
int  backup_header_parse(const u8* p, u32 n, BackupMeta* m, u32* header_len, epos* data_len);
extern const char BACKUP_TRAILER[];                      // "\r\n@END PICABAK1\r\n"
```

## 7. Document model

### 7.1 Bytes, origin, characters
- The document is the file's exact bytes, including any BOM. Byte offsets (`epos`) are the only position type stored
  anywhere (selection, undo, marks, session, search results). Character and column values are computed on demand.
- `text_origin()` = BOM length (0, 2 or 3), fixed at load/convert/reinterpret. The text view never places the caret
  below it; Ctrl+Home goes to the origin. In the hex view the bytes `[0, origin)` are shown grey and are read-only
  (status note: "The BOM is changed with Document > Encoding").
- Characters: `Document::decode/char_next/char_prev/snap_char` use the codec. For self-synchronising codecs (SBCS,
  UTF-8, UTF-16) `char_prev` uses the rule "largest k <= max_len such that decoding at pos-k consumes exactly k
  bytes"; for DBCS it decodes forward from the line start (at most 64 KB back; beyond that from pos - 64 KB, accepting
  rare desync on a DBCS line longer than 64 KB).
- Invalid input is one character per bad byte (UTF-8, SBCS undefined byte, DBCS bad pair: lead byte only) or per bad
  2-byte unit (UTF-16 lone surrogate; a final odd byte is one bad byte). They are drawn as `xHH`/`uHHHH`, selected and
  deleted as one character, and preserved byte-exact because untouched bytes are never re-encoded.

### 7.2 Lines and line breaks
- Each document has a **break mode**: LF mode (break unit = LF) or CR mode (break unit = CR). CR mode is used only when
  detection finds CR breaks and no LF at all (classic Mac files), or after "Convert to CR".
- LF mode: a line ends at LF; a CR immediately before that LF belongs to the EOL (CRLF) and is not shown. A CR not
  followed by LF is an ordinary character drawn as the control picture "CR". CRLF and LF lines can be mixed freely.
- UTF-16: the break unit is the 2-byte code unit `0A 00` (LE) / `00 0A` (BE) (or `0D` for CR mode) at an even offset.
- `line_start(L)` = 0 for L = 0, else `break_pos(L-1) + unit`. `line_end(L)` = `break_pos(L)` minus one unit if the
  preceding unit is CR (LF mode), or `length()` for the last line. The last line has no EOL; a document ending with a
  break has an empty last line (as in every editor).
- `eol_style` (CRLF/LF/CR) is the style for newly typed breaks: from detection (dominant), or the Options default for
  new documents, or after a conversion. `eol_counts` are maintained exactly: before each edit the Document counts
  CRLF/LF/CR in the window `[pos-unit, pos+del+unit)`, after the edit in `[pos-unit, pos+ins+unit)`, and adjusts.
  `eol_mixed()` = more than one of the counters > 0 (lone CRs count only in CR mode). Status bar shows `Mixed` then.
- Typing Enter inserts `eol_bytes()`. Paste normalises CR, LF and CRLF in the pasted text to `eol_style`
  (raw same-encoding paste keeps bytes exactly).

### 7.3 Alignment rule for UTF-16
All edits in a UTF-16 document must have even `pos`, `del` and `ins_len`; otherwise `replace` returns `E_ALIGN` and
changes nothing. Consequences: hex overwrite of one byte rewrites its whole 2-byte unit (one undo group); hex insert
inserts whole units; hex delete removes whole units; an odd trailing byte can be deleted (pos even, del 1 at the end is
the one allowed odd case: the Document permits `pos + del == length()` with odd del to remove a dangling byte) but
nothing can be inserted after it (status note "Delete the odd trailing byte first").

### 7.4 Editing and undo
- `Document::replace` is the only content mutator. It checks `editable()`, alignment and size (`length() - del + ins
  <= EPOS_MAX`), records the undo op, updates EOL counts, increments `change_seq`, and fires one `EditEvent`
  (`line` = line of pos before the edit, `lines_removed`/`lines_added` = break units removed/added).
- Callers bracket logical commands with `begin_edit(kind, sel_before, now)` / `end_edit(sel_after)`. Multi-step
  commands (move lines, replace all, convert) are one group. Typing groups merge per 6.6; `ed_newline` and
  `set_saved` call `break_undo_merge`.
- Undo/redo apply the group's ops in reverse/forward order through the tree directly (`insert_refs` / `remove`),
  fire EditEvents with EV_UNDO/EV_REDO, and return the stored selection, which the views apply.
- Modified flag: `modified() = !log.at_save_point() || always_modified`. Async save: `docio` captures `undo_seq()` at
  snapshot time and calls `set_saved_seq(seq)` when the save succeeds; edits during the save are impossible (the
  document is `busy`).
- Undo is unlimited. If memory runs out while recording, the edit fails with E_NOMEM and nothing changes; the status
  bar suggests closing tabs. Document > "Clear undo history" is not offered in 1.0 (memory per edit is ~40 bytes).

### 7.5 Encoding changes
- **Reload as X** (reinterpret): bytes unchanged; `reinterpret` recomputes origin (BOM of X if present in the bytes,
  else 0), break spec and counts; undo history is kept but is now interpreted in the new encoding (byte ops are still
  valid). For an unmodified file docio rereads the disk instead (13.2), which is the same thing.
- **Convert to X**: `check_convert` counts characters that X can't store and invalid source bytes; if non-zero the UI
  asks (12.3). `convert` decodes everything from the origin, encodes into X with the new BOM (`?` for unrepresentable,
  U+FFFD (or `?` in ANSI/OEM) for invalid source bytes), replaces the whole document in one group, and switches
  enc/origin/break spec. Undo restores the old bytes and encoding (the group records the encoding before/after: the
  Document stores enc changes as a special op).
- Converting EOLs: every CRLF, bare LF and lone CR becomes the target. Converting to CR switches to CR mode; from CR
  to CRLF/LF switches to LF mode. One group, undo restores the bytes and mode.

## 8. Text view

### 8.1 Layout
```
| m | 1234 | text...                                                         |^|
  ^ marker cell (bookmark bullet U+2022 in UIC_BOOKMARK)
      ^ line number, right-aligned, digits = max(3, digits(line_count)), then 1 gap cell
```
- Gutter: `(1 + digits + 1)` cells; background `UIC_GUTTER_BG`, numbers `UIC_GUTTER_FG`. Hidden numbers (View > Line
  numbers off) leave only the marker cell. Text starts at gutter width + 4 px.
- Row height = font `tmHeight` (GetTextMetricsA). Columns = cell width (`tmAveCharWidth`, checked against
  GetCharWidthA('M')).
- No wrap: one row per line; horizontal scroll in whole cells; the scroll range is the widest line laid out so far
  this session for this tab (grows lazily; long lines report `LongLineIndex::total_cells`), at least the client width.
- Wrap: rows break at the last space or punctuation before the right edge, else hard at the edge (wide chars never
  split, combining marks stay with their base). Continuation rows have no number. Lines longer than LAY_LONG wrap by
  characters (no word search) using the checkpoint index. The view is anchored at (top_line, top_subrow); the
  vertical scrollbar is in logical lines (ARCH 5), the thumb position is `top_line`.
- Vertical scrollbar range > 32767 lines: positions are scaled to 0..32767 and the thumb is read with
  `GetScrollInfo(SIF_TRACKPOS)`.

### 8.2 Caret and selection
- One selection per tab (`Tab::sel`, byte offsets, always on character boundaries >= origin). Caret = `sel.caret`.
- System caret: 2 px wide in insert mode, a full cell block in overwrite mode (Ins toggles; text and hex keep separate
  modes; the status bar shows INS/OVR). Position updated with SetCaretPos after every change; hidden when the caret
  is outside the client area.
- "Desired column" (cells) is kept for Up/Down/PgUp/PgDn and reset by horizontal moves and edits.
- Selected text is drawn with COLOR_HIGHLIGHTTEXT on COLOR_HIGHLIGHT (syntax colours are not shown inside the
  selection); a selected EOL is shown as one highlighted cell after the line end.

### 8.3 Keyboard (text view)
| Key | Action |
|---|---|
| Arrows / Ctrl+Left/Right | char / word (ucd word classes: word, punctuation, space) |
| Home / End | smart home (first non-blank, then column 0) / line end; in wrap mode first press goes to the row start/end |
| Ctrl+Home / Ctrl+End | document start (origin) / end |
| PgUp / PgDn | one screen minus one row, caret keeps its screen row |
| Ctrl+Up / Ctrl+Down | scroll one row without moving the caret |
| Shift + any of the above | extend the selection |
| Backspace / Del | delete char before / after (or the selection); Ctrl+ deletes a word |
| Enter | newline + auto-indent |
| Tab / Shift+Tab | selection spans lines: indent/outdent those lines; else insert a tab (or spaces to the next stop) / outdent the line |
| Ins | toggle overwrite |
| Esc | clear Mark All marks if any, else nothing (the find dialog/isearch handle their own Esc) |
| Characters | `ed_type`; AltGr characters arrive as WM_CHAR with Ctrl+Alt down and must be inserted (only Ctrl without Alt is a control key) |

Global shortcuts (accelerators) are in 15.5. WM_CHAR bytes are converted from the input-locale code page (winenc,
`GetKeyboardLayout` -> `LOCALE_IDEFAULTANSICODEPAGE`, refreshed on WM_INPUTLANGCHANGE) to a code point. DBCS lead bytes
are buffered; WM_IME_CHAR is handled directly and not passed to DefWindowProc. The IME composition window is placed at
the caret with `ImmSetCompositionWindow` (best effort, imm32 linked statically).

### 8.4 Mouse
Click sets the caret (nearest boundary); Shift+click extends; drag selects with auto-scroll (50 ms timer, speed by
distance); double-click selects a word (`ed_word_at`); a third click within the double-click time selects the line.
Gutter: click selects the line, drag selects lines, click in the marker cell toggles a bookmark. Wheel scrolls
`SPI_GETWHEELSCROLLLINES` rows (default 3). Right-click: context menu Undo, Redo | Cut, Copy, Paste, Delete | Select
All | Toggle Comment, UPPERCASE, lowercase. Middle button does nothing in the text area.

### 8.5 Painting and invalidation
- WM_PAINT paints only rows intersecting the update rect. For each row: get the line bytes (at most LAY_WINDOW bytes
  starting at the first visible cell's checkpoint for long lines), `LineLayout::build`, `Highlighter::style_line`
  (if the start state is unknown the row is drawn in ST_DEFAULT and repainted when the highlighter catches up),
  overlay selection, RangeMarks, brace match, then `render_row` (10.2).
- Edits invalidate from the edited row: one row if `lines_added == lines_removed == 0`, else from that row to the
  bottom (or `ScrollWindowEx` the tail when lines were inserted/removed above the bottom). Rows whose lexer state
  changed come from `Highlighter::take_dirty` after idle work.
- Brace match: when the caret is next to a brace from the language's `braces` (and not inside a string/comment), both
  braces get `UIC_BRACE_BG` + bold; an unmatched brace gets `UIC_BRACE_BAD_FG`. The search is limited to 20,000 lines
  each way.
- Show whitespace: `·` for spaces, `»` at tab starts, and the EOL drawn after the line end as the text `CRLF`, `LF` or
  `CR` in `UIC_WS_FG` (outside the cell model; not hit-testable).

### 8.6 Large-mode gates
For a tab in large mode (13.8, 14): colouring off (Document > Syntax shows "(off: large file)" and can force it on
for in-memory documents), wrap off by default, brace matching limited to 2,000 lines, Mark All capped at 10,000.

## 9. Hex view and view sync

### 9.1 Layout
```
Offset(h) 00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  0123456789ABCDEF     <- fixed ruler row
00000000  4D 5A 90 00 03 00 00 00  04 00 00 00 FF FF 00 00  MZ..............
00000010  B8 00 00 00 00 00 00 00  40 00 00 00 00 00 00 00  ........@.......
```
- 16 bytes per row, 8 hex offset digits, a double space after byte 7. 76 cells = 608 px with Courier New 10 pt; fits
  the 780 px client with the scrollbar.
- Char column: SBCS documents decode bytes through their code page (`hex_char_glyph`; OEM shows CP437 graphics for
  0x80-0xFF), other encodings show ASCII 0x20-0x7E only. Non-printables are `.`.
- Colours: offsets `UIC_HEX_OFFSET`; bytes from the add buffer (`PSpan.src != 0`) in `UIC_HEX_MODIFIED` (red) until
  saved or reloaded; BOM bytes `UIC_HEX_BOM`; selection highlight in both panes; the byte under the caret outlined
  in the other pane with `UIC_HEX_MIRROR_BG`.
- Rows = ceil(length / 16) + 1 (the last row allows appending); scrollbar scaled to 0..32767 for big files.
- Data inspector strip (View > Data Inspector, `Config.inspector_on`): 3 text rows at the bottom of the hex view:
  `int8 -1  uint8 255  int16 -257  uint16 65279  int32 ...  uint32 ...` / `int64 ... uint64 ...` /
  `float ...  double ...  bin 11111111  [LE]`. Clicking `[LE]`/`[BE]` toggles byte order. Values for the bytes at the
  caret (`inspect_bytes`), "-" when the file ends too early. Read-only.

### 9.2 Caret, modes and keys
- `HexCaret`: byte offset, nibble (hex pane), pane (hex/chars), insert flag. Overwrite is the default
  (`Config.hex_overwrite`); Ins toggles; insert mode shows a 2 px caret and a highlighted `INS` in the status bar,
  overwrite a cell block and `OVR`.
- Keys: Left/Right move by nibble in the hex pane and by byte in the char pane; Up/Down by row; PgUp/PgDn by screen;
  Home/End row start/end; Ctrl+Home/End file start/end; Ctrl+Left/Right previous/next 8-byte group; Shift extends
  the selection (whole bytes); Tab switches panes (the caret keeps its byte); Esc does nothing.
- Typing hex digits (hex pane) or characters (char pane, encoded in the document encoding):
  - Overwrite: replaces the nibble/byte(s) at the caret; at the end of the file it appends. A multi-byte character
    overwrites as many bytes as it encodes to.
  - Insert: the first nibble inserts a new byte `n0` at the caret, the second sets its low nibble; a typed character
    inserts its bytes. UTF-16: inserts whole units (7.3).
  - A selection is replaced first (deleted, then the typed byte inserted) in both modes.
  - Consecutive typing merges into one undo group (UK_HEXTYPE).
- Del deletes the selection or the byte (unit) at the caret in both modes. Backspace: insert mode deletes the byte
  before the caret; overwrite mode only moves left (never shifts data).
- Edits in `[0, origin)` beep and show the BOM note. Read-only/indexing/busy documents beep with a status note.
- Mouse: click places the caret on a byte (and nibble) in either pane; drag selects bytes; double-click selects the
  byte; wheel scrolls 3 rows; right-click menu: Undo, Redo | Cut, Copy, Paste, Delete | Select All.

### 9.3 Status bar in hex view
`Offset 0x00001F4A (8010) | Sel 16 bytes | Byte 0x4D 77 | <encoding> | <EOL> | OVR | HEX | Size 81,920 (+16)` -
the size shows `+N`/`-N` against the size on disk when they differ.

### 9.4 Go to and search in hex view
Ctrl+G opens the Go To dialog in offset mode (6.17 syntax, live preview "= 0x00001F40 (8000)", error text in red).
Ctrl+F opens the same Find dialog; the mode list gains **Hex bytes** (`DE AD ?? EF`). Text modes search the decoded
text exactly as in the text view; a match selects its byte range.

### 9.5 View switching and sync
- View > Hex View (Ctrl+Shift+H), toolbar button, or a click on `TEXT`/`HEX` in the status bar toggles the active tab.
  Switching never changes a byte.
- Text -> hex: the hex selection is the same byte range (caret = `sel.caret`). Hex -> text: the caret snaps to the
  start of its character, the selection low end down and high end up to character boundaries, and both are clamped
  to `>= origin`.
- The new view scrolls so that the caret is on the same screen row as before if possible, else centred.
- Binary files (detection `binary`) open in hex view with the notification bar "Binary file: opened in hex view.
  [Show as text]" (auto-hides after 10 s).

### 9.6 Clipboard in the hex view
- Copy in the hex pane: private format `Pica.RawBytes` (exact bytes) + `CF_TEXT`/`CF_UNICODETEXT` as hex text
  ("4D 5A 90 00", 16 per line). Copy in the char pane: RawBytes + the decoded text.
- Paste: RawBytes if present; else text that parses completely as hex bytes (`hex_parse_bytes`) when pasting into the
  hex pane; else the text encoded in the document encoding. Paste follows the mode (overwrite writes over N bytes,
  extending at the end; insert inserts). One undo group.
- Copy is refused above 16 MB ("Selection too large to copy (16 MB max)").

## 10. Rendering

### 10.1 Decisions (probe numbers in 3.1)
1. Paint directly to the window DC in WM_PAINT (BeginPaint/EndPaint; class style `CS_OWNDC` keeps the font and colours
   selected). No back buffer, no DIB sections, no glyph cache.
2. One `ExtTextOutW(hdc, x, y, ETO_OPAQUE | ETO_CLIPPED, &cellrect, text, n, dx)` per run of equal colours/font. After
   the last run, one `ExtTextOutW(ETO_OPAQUE, rect, NULL, 0)` clears the rest of the row. Every pixel is painted once
   (no flicker).
3. x = text_left + (cell - hscroll_cell) * cw; only the visible cell range is drawn (9x GDI coordinates are 16-bit;
   never pass a line's absolute x). Each call draws at most 1024 glyphs (8192 is the 9x limit).
4. `dx` (lpDx) is passed only for runs containing wide characters, combining marks or fallback-font glyphs (pins every
   glyph to the grid); plain runs pass NULL.
5. Scrolling: `ScrollWindowEx(0, dy, ..., SW_INVALIDATE)` + `UpdateWindow` when |dy| < visible rows, else invalidate
   all. Horizontal scroll by cells the same way.
6. Measuring: GetTextMetricsA once per font; GetCharWidthW lazily per 256-char page to detect glyphs whose advance is
   not one or two cells (they get lpDx). Never GetTextExtentExPoint, GetCharWidth32, GetCharABCWidthsW,
   GetTextMetricsW, DrawTextW.
7. Fallback if flicker ever shows up in testing: a single window-sized compatible bitmap (DDB) shared by both views
   (+3 ms per full repaint). The render API takes an HDC so this is a local change in textview/hexview.

### 10.2 render.h (win)
```cpp
struct RenderFonts { HFONT normal, bold; HFONT cjk; int cw, ch, ascent; int bold_ok; };
int  render_init(const char* face, int height_px, int pt, int charset);   // creates fonts; returns E_*
void render_shutdown();
const RenderFonts* render_fonts();
struct RunAttr { COLORREF fg, bg; u8 bold; u8 font; /* 0 normal, 1 cjk */ u8 box; /* placeholder outline */ };
void render_run(HDC dc, int x, int y, const RECT* cell_rect, const WCHAR* s, u32 n, const RunAttr& a, const int* dx);
void render_fill(HDC dc, const RECT* r, COLORREF bg);
COLORREF render_style_fg(u8 style);  COLORREF render_style_bg(u8 style);  int render_style_bold(u8 style);
COLORREF render_ui(int uic);         // resolves COLOR_SYSDEFAULT to system colours
```
`render.cpp` caches SetTextColor/SetBkColor/SelectObject state per DC to avoid redundant calls.

### 10.3 Fonts
- Default **Courier New 10 pt** (8x16). Options: any fixed-pitch screen font via `ChooseFont(CF_FIXEDPITCHONLY |
  CF_SCREENFONTS)`; tested choices Courier New 9/10/12, Lucida Console 9/10, Fixedsys, Terminal.
- Bold: used for styles marked bold only if the bold font has the same `tmAveCharWidth` and `tmMaxCharWidth`
  (`bold_ok`); otherwise colour only (Fixedsys synthesised bold is 1 px wider). No italic.
- Raster fonts (Fixedsys, Terminal, Courier) only carry their own code page: characters outside cp1252 (or the OEM code
  page for Terminal) are drawn with Courier New at the same cell height, with lpDx.

### 10.4 Characters the font can't draw
`uc_is_drawable` false (e.g. CJK, Thai, Arabic): if `EnumFontFamiliesEx` finds a font for SHIFTJIS / GB2312 / HANGEUL /
CHINESEBIG5 charset at startup, CJK ranges are drawn with it (`RenderFonts.cjk`, wide cells, lpDx); otherwise a
placeholder: the cell(s) filled with `UIC_CTRL_BG` and a `?` in `UIC_CTRL_FG` (the bytes are untouched; the status bar
shows the code point of the char at the caret: `U+4E2D`). Right-to-left text is drawn left to right without shaping.

## 11. Search

### 11.1 Modes and syntax
- **Normal**: the text is literal. Whole word = the match must not be preceded/followed by a word char (`uc_is_word`).
- **Extended**: literal with escapes `\n \r \t \0 \\ \xHH \uHHHH \dNNN` (character codes, encoded in the document
  encoding). Invalid escapes are an error shown in the dialog.
- **Regex** (ECMAScript/PCRE-like, Notepad++ users' expectations):
  - Literals and escapes `\t \n \r \f \v \0 \xHH \x{HHHH} \uHHHH \\ \. \[` etc.; `.` (not CR/LF unless
    ". matches newline" or `(?s)`).
  - Classes `[abc] [^a-z] [\w-]`, `\d \w \s \D \W \S` (Unicode via ucd), POSIX `[[:alpha:]] [[:digit:]] [[:space:]]
    [[:upper:]] [[:lower:]] [[:alnum:]] [[:punct:]] [[:xdigit:]]`.
  - Anchors `^ $` (line-wise: `$` matches before CRLF, LF or end), `\A \z`, `\b \B`, `\< \>`.
  - Groups `( )`, `(?: )`, alternation `|`; quantifiers `* + ? {n} {n,} {n,m}` and lazy forms; `{n,m}` m <= 1000.
  - Inline flags `(?i)` `(?s)` at the pattern start. `\R` = CRLF | LF | CR.
  - Backreferences `\1`-`\9` and lookahead `(?= )` `(?! )` use the backtracker over a window of the current line
    extended by up to 64 KB; matches longer than the window are not found (documented). Step budget 10,000,000 per
    search call: over it the search stops with "Pattern too complex (backtracking limit)".
  - Not supported in 1.0 (compile error with a clear message): lookbehind, atomic groups, possessive quantifiers,
    recursion, named groups, Unicode properties `\p{}`.
  - Whole word in regex mode wraps the pattern as `\b(?:...)\b`.
- **Hex bytes** (hex view only): `DE AD ?? EF`, spaces optional, `??` wildcard byte.
- Case-insensitive matching uses `uc_fold` (simple case folding); Normal-mode ASCII patterns in ASCII-compatible
  encodings use a byte fold table with Horspool.
- Limits: pattern <= 1000 chars, program <= 4000 instructions; regex compile errors carry a position.

### 11.2 Replace syntax
Normal: literal. Extended: same escapes as find. Regex: `$0`-`$9`, `\0`-`\9`, `${n}`, `$$`, `\n \r \t \\`, and case
operators `\U \L \E \u \l` (Boost/Notepad++ style). The expansion is encoded into the document encoding; characters it
can't store become `?` and the count is reported ("3 characters became '?'").

### 11.3 Engine behaviour
- The `Searcher` encodes Normal/Extended literals (and regex literal prefixes) in the document encoding and runs
  Boyer-Moore-Horspool over raw spans with an overlap buffer; candidates are verified (word boundaries, regex).
- Find previous: forward search in blocks `[p - 64 KB, p)`, keep the last match, widen backwards.
- Empty matches: after an empty match the next search starts one character later. Replace All handles them (`^` adds
  a prefix to every line).
- Replace All collects matches and expansions first (bounded by memory; > 1,000,000 matches stops with a message),
  then applies them back to front in one undo group, with progress and Cancel (cancel = nothing applied).
- Searches longer than 0.5 s show the progress window. Default scope: whole document from the caret, wrapping when
  "Wrap around" is on ("Wrapped to top" in the dialog status line).

### 11.4 Find / Replace dialog (modeless)
```
+ Find ---------------------------------------------------------------+
| [Find] [Replace] [Find in Files]                                    |
| Find what:    [combo with history_____________________] [>]         |
| Replace with: [combo with history_____________________] [>]  (Replace tab)
| [ ] Match case  [ ] Whole word  [x] Wrap around  [ ] In selection   |
| Direction (o) Down ( ) Up     Mode (o) Normal ( ) Extended ( ) Regex ( ) Hex bytes |
| [ ] . matches newline                                               |
| <status line: "12 matches" / "Not found" / red regex error>         |
|                 [Find Next] [Count] [Find All] [Mark All] [Close]   |  Find tab
|  [Find Next] [Replace] [Replace All] [Replace All in Open Tabs] [Close] Replace tab
+---------------------------------------------------------------------+
```
- Ctrl+F / Ctrl+H / Ctrl+Shift+F open it on the matching tab, pre-filled with a single-line selection or the word at
  the caret. Fields are combo boxes with the last 15 entries (Config). The `>` button pops up a menu of regex tokens
  (`.` `\d` `\w` `\s` `[...]` `^` `$` `(...)` `\1` `*?` `+?` `|` ...) that are inserted at the field caret
  (regex mode only).
- Live validation in regex/extended/hex mode (on each edit, 200 ms debounce): errors appear in red in the status line;
  Find buttons stay enabled and show the error when pressed.
- "In selection" is ticked automatically when the selection spans more than one line when the dialog opens.
- Not found: status line text + MessageBeep(MB_OK); no message box.
- The dialog moves itself (above or below) when a found match would be covered.
- F3/Shift+F3 work while the dialog has focus. Esc closes it. Position is remembered.
- Count: "N matches" in the status line. Find All: results panel (format as 11.6, header `"foo" in main.cpp: 17
  hits`). Mark All: sets RangeMarks (`UIC_MARK_BG`), status "N marked"; Search > Clear Marks removes them.
- Replace: replaces the current selection if it is a match, then finds the next. Replace All: in the selection or the
  whole document, one undo group, status "N replaced". Replace All in Open Tabs: asks once ("Replace in all 5 open
  documents?"), one undo group per tab, skips read-only/busy/on-demand-indexing tabs, reports per-tab counts in the
  status line.

### 11.5 Incremental search bar (Ctrl+I)
A strip above the status bar: `Find: [______] 3/17  [ ] Match case  [ ] Highlight all  [x]`. Typing searches
literally from the caret position where the bar opened; Enter/F3 next, Shift+Enter/Shift+F3 previous; Esc closes and
keeps the selection; Ctrl+I again = next. Not found: field background `UIC_ISEARCH_FAIL` + beep. The `3/17` count
is shown only for documents <= 1 MB. Highlight all = RangeMarks (<= 1 MB documents).

### 11.6 Find in Files and the results panel
- Find in Files tab: Find what, Files (`*.cpp;*.h` default from the current tab's language extensions, history),
  Folder (default: current file's folder; `...` = SHBrowseForFolder), Subfolders, Match case, Whole word, mode
  (Normal/Extended/Regex). Buttons: Find All, Stop, Close. No replace in files.
- Worker thread (`fif_run`): recursive FindFirstFile walk (skips hidden/system folders), masks with `!` excludes,
  files with a NUL in the first 4 KB are skipped (unless UTF-16 is detected), encoding detected per file, files <= 16
  MB read whole, larger files searched in 1 MB windows overlapping by 64 KB. Matches are line-based results; at most
  50,000 hits (then "stopped at 50,000 hits"). Disk content is searched even for open documents; the summary adds
  "(N open files have unsaved changes; disk versions searched)".
- Results panel: docked under the editor, resizable splitter, a second `TextView` instance on a read-only UTF-8
  `Document` with the results lexer:
  ```
  Search "Blt" (17 hits in 6 files of 230 searched)
  M:\dev\runner\game.cpp (3 hits)
      120: 	if (dd->Blt(&dst, surf, &src, DDBLT_KEYSRC, NULL) != DD_OK)
  ```
  Match spans are RangeMarks. A new search replaces the old results. Progress shows in the header line while running
  ("Searching... 120 files"); Stop/Esc cancels.
- Double-click or Enter on a hit opens the file (or activates its tab) and selects the match; F4 / Shift+F4 go to the
  next/previous hit from anywhere; Ctrl+C copies the selected lines; Esc in the panel closes it.

## 12. Encodings and line endings (UX)

### 12.1 Detection and defaults
- Detection as 6.10. `.nfo` files open as OEM when `Config.nfo_oem` (default on). Pure ASCII files are "ANSI".
- New documents: `Config.new_enc` (default ANSI, no BOM) + `Config.new_eol` (default CRLF). Pica never adds a BOM
  unless the user picks an encoding labelled "with BOM" (UTF-8 BOM, UTF-16 LE/BE always have one).
- Status bar fields (click or right-click opens the popup = the Document menu submenu): encoding label (`ANSI`,
  `OEM`, `UTF-8`, `UTF-8 BOM`, `UTF-16 LE`, `UTF-16 BE`, `Windows-1251` ...) and EOL (`CRLF`, `LF`, `CR`, `Mixed`).

### 12.2 Document > Encoding
```
Encoding >  Reload as >  ANSI | OEM (DOS) | UTF-8 | UTF-16 LE | UTF-16 BE | Code page > (1250, 1251, 1253, 1254, 1257, 437, 850, 852, 866, KOI8-R)
            ------------------------------------------
            (o) ANSI                 <- radio = the encoding the file is saved in; picking one converts
            ( ) UTF-8
            ( ) UTF-8 with BOM
            ( ) UTF-16 LE (BOM)
            ( ) UTF-16 BE (BOM)
```
- Reload as: unmodified file -> reread from disk with that decoding (not undoable, nothing becomes modified); modified
  file -> ask "Reloading discards your changes to X. Continue?"; untitled -> reinterpret the current bytes.
- Convert (radio): `check_convert`; if anything is lost, ask: "14 characters can't be stored in ANSI and will become
  '?'. First at line 12. [Convert] [Go to first] [Cancel]". The document becomes modified; the hex view shows the new
  bytes immediately. One undo step. Not available for on-demand documents in 1.0 (disabled, tooltip-like status note).
- The Save As dialog has an **Encoding** combo (the five save encodings) and a **Line endings** combo (CRLF/LF/CR)
  through an `OFN_ENABLETEMPLATE | OFN_ENABLEHOOK` template; defaults = the document's values; a different choice
  converts before saving (same loss prompt).

### 12.3 Line endings
Document > Line Endings > Windows (CRLF) / Unix (LF) / Mac (CR): converts every break (7.5), one undo step, sets
`eol_style`. The current dominant style has the radio mark; `Mixed` documents show no mark and the status bar says
`Mixed`; saving keeps each line's ending.

### 12.4 Unrepresentable typing and paste
Typing a character the encoding can't store: nothing is inserted, a beep, and the notification bar: "'Ж' (U+0416) can't
be stored in ANSI (Windows-1252). [Convert to UTF-8] [Dismiss]". Paste: such characters become `?`, and the bar says
"3 pasted characters became '?' (not in ANSI). [Undo and convert to UTF-8] [Dismiss]".

## 13. Files: load, save, backup, session, settings, change detection

### 13.1 Settings location
- `cfg_dir` = the exe's folder if a test file can be created there (`~pica.tst`, deleted at once), else
  `SHGetSpecialFolderPathA(CSIDL_APPDATA)\Pica` (probe: `C:\WINDOWS\Application Data`), else `<GetWindowsDirectory>\Pica`.
  `-cfg <dir>` overrides (selftest, multiple profiles).
- Contents: `pica.ini` (settings, Appendix C), `session1.ini` + `session2.ini` (13.6), `backup\` (13.5), `langs\`
  (optional overrides, next to the exe, not in cfg_dir).
- Recommended install: `C:\PICA\pica.exe` (backups and session then live on the fast local disk). Running from `M:`
  works (the I/O worker hides the slow writes).

### 13.2 Opening files
1. Path normalisation: `GetFullPathNameA`; tabs are matched case-insensitively by full path (and by
   `GetShortPathNameA` when both exist) so a file is never opened twice; opening an open file activates its tab.
2. `file_load` (core): stat; size >= 4 GB-1 or > EPOS_MAX -> refuse ("Files of 4 GB or more can't be opened"). Size
   <= `ondemand_threshold` (`min(Config.ondemand_mb, totalPhys/8)`, floor 4 MB, default 16 MB): open with share
   read+write (so files other programs keep open still load), read in 64 KB chunks into one `e_valloc` block (progress
   callback), close, `detect` on the first 64 KB (whole file if smaller). Above: on-demand (14.1).
3. Open failures: not found (from MRU/session: status note, MRU entry removed), access denied, sharing violation
   ("X is locked by another program. [Retry] [Cancel]"), out of memory (offer on-demand mode for files > 4 MB).
4. Encoding override from Reload-as/session/`.nfo` rule is applied (`LoadOpts.force_enc`). Detection results set enc,
   origin, EOL style, binary flag.
5. Read-only attribute -> the document is read-only with the bar "Read-only file. [Edit anyway]" ("Edit anyway" lets
   you edit; saving then asks to clear the attribute or Save As).
6. The language is chosen by `lang_for_file(basename)` unless forced. Large mode applies (13.8).
7. The tab records the disk state (size, mtime) for change detection.

### 13.3 Safe save sequence
Steps 1-3 run on the I/O worker (`file_write_temp`), 4-9 on the UI thread (`file_replace`), in under 50 ms on C:.
1. Check the target folder exists and is writable; free space >= size + 64 KB (`free_space`, GetDiskFreeSpaceExA).
2. `make_temp(dir, "~PC")` (GetTempFileNameA in the target folder: same volume, unique 8.3 name). Write all document
   bytes in 64 KB chunks from a `SnapshotSource`, checking the cancel flag; `flush` (FlushFileBuffers); `close`.
3. Verify: stat the temp file; its size must equal the snapshot size. Any failure: delete the temp file, the
   original is untouched, report.
4. If the target exists: remember its attributes and creation time; if it is read-only, stop with E_ACCESS (the UI
   offers "Clear read-only and save" / Save As).
5. On-demand document: `hooks->before_replace()` closes our FileSource handle (MoveFile fails on open files, probe).
6. `make_temp(dir, "~PO")` gives a unique name; delete it; `move(target, old_copy)`. If the move fails with
   E_ACCESS/E_SHARING (another program holds the file): **in-place fallback** for in-memory documents: `copy(target,
   old_copy)`, then `copy(temp, target)` (overwrites in place), delete temp; if that copy fails, the target may be
   damaged: report with the path of `old_copy` ("Your previous version is kept as ..."). For on-demand documents:
   report "X is in use by another program", reopen the source, keep the temp file (path in the message).
7. `move(temp, target)`. On failure: `move(old_copy, target)` (rollback), report; the temp file is kept and named in the
   message.
8. Restore attributes (`set_attrs`) and creation time (`set_ctime`) on the new file.
9. Old copy: on-demand -> `hooks->after_replace(old_copy)` reopens the source on it (same bytes, so pieces stay valid)
   and the tab deletes it on close/reload; in-memory -> if `keep_bak`, rename to `<name>.bak` (replacing an older
   .bak), else delete it. Stat the new file; the tab records size/mtime (FAT has 2 s resolution).
- Writing a new file (target doesn't exist): steps 1-3, then `move(temp, target)`.
- Save As to another path: same sequence on the new path; the tab takes the new path, language re-detected (unless
  forced), MRU updated.
- While a save runs the document is busy (edits refused with a status note "Saving... please wait"), the tab label
  shows `(saving)`, and the status bar shows progress for writes > 0.5 s. Other tabs stay fully usable.
- `docio_save_wait` runs the save and pumps messages in a modal progress window until it finishes (used by exit,
  close-with-save, Save All before exit).

### 13.4 Revert, reload, close
- File > Revert to Saved: confirm ("Discard all changes to X?"), then reload from disk (13.2) with the same encoding,
  caret line kept. Not undoable.
- Reload after an external change (13.9): in-memory documents <= 4 MB use `reload_as_edit` (one undo step, so an
  accidental external overwrite can be undone); larger ones use `reset_source` (undo cleared).
- Closing a modified tab always asks "Save changes to X? [Yes] [No] [Cancel]", also with backups on and for untitled
  tabs (closing a tab is an explicit discard; only exiting keeps unsaved tabs for the next session). Closing deletes
  the tab's backups after the session is written without it.

### 13.5 Backups of unsaved work
- Which documents: modified, in-memory (<= ondemand threshold), when `Config.backup_enabled` (default on). Untitled
  documents with no bytes are not backed up.
- When: `Config.backup_seconds` (default 5) after the last edit, but at least every 30 s while edits continue; also
  when the app is deactivated (WM_ACTIVATEAPP false) and on exit.
- How: the I/O worker writes `backup\T<tabid 5 hex><gen 2 hex>.BAK` from a `SnapshotSource`: header (backup_header_write),
  the bytes, trailer; flush; close; verify the size with `stat`; then deletes the previous generation of that tab.
  A crash mid-write leaves an invalid newest file (missing trailer or wrong size) and the previous generation intact.
- Backup file format (ASCII header, CRLF):
  ```
  PICABAK1
  tabid=5
  gen=7
  seq=42
  path=M:\dev\editor\src\core\ptree.cpp     (empty for untitled)
  untitled=0
  untitled_no=0
  enc=utf8bom
  eol=crlf
  lang=cpp
  caret=1234
  anchor=1234
  disk_size=12345
  disk_mtime=01D9F1A2B3C4D5E6
  written=01D9F1A2B3C4D5E6
  @DATA 15000
  <15000 raw bytes>
  \r\n@END PICABAK1\r\n
  ```
  Valid iff the header parses, the file size = header_len + data_len + trailer length, and the trailer matches.
- After a successful save of a tab, its backups are deleted (the disk file is the truth).

### 13.6 Session
- Written (as text, via the I/O worker) when tabs open/close/reorder/activate (debounced 2 s), every 60 s if any tab
  state changed (caret, scroll, view), after a save, and on exit (synchronously).
- Two files, `session1.ini` and `session2.ini`; each write goes to the file not holding the newest valid copy
  (`ab_write`), with `seq` incremented; `ab_read` + `session_validate` pick the newest valid one at startup. A torn
  write can only damage the older copy.
- Format:
  ```
  [Session]
  version=1
  seq=42
  active=1
  next_tabid=17
  next_untitled=3
  clean_exit=0
  count=2
  [Tab0]
  id=5
  path=M:\dev\editor\src\core\ptree.cpp
  untitled=0
  untitled_no=0
  view=text            ; text | hex
  hex_pane=0
  hex_insert=0
  enc=utf8             ; enc_token()
  enc_forced=0
  eol=crlf
  lang=cpp
  lang_forced=0
  wrap=0
  tab_width=4
  use_spaces=0
  read_only=0
  caret=1234
  anchor=1234
  top_line=100
  top_subrow=0
  hex_top=0
  bookmarks=10,20,30
  modified=1
  has_backup=1
  disk_size=12345
  disk_mtime=01D9F1A2B3C4D5E6
  disk_missing=0
  [Tab1]
  ...
  [end]
  ok=1
  ```
- `clean_exit=0` is written right after startup restore; `1` on a normal exit.

### 13.7 Startup restore and crash recovery
1. Read the session (`ab_read`). If none is valid, continue with backups only.
2. `backup_list` gives the newest valid generation per tab id.
3. For each session tab in order: if a backup exists for its id -> open the backup bytes as an in-memory document with
   the backup's encoding/EOL/language, caret, path; mark it always-modified; compare the file's current disk state
   with `disk_size/disk_mtime` from the backup: if different, show the bar "X changed on disk after your unsaved
   changes were backed up. [Reload from disk] [Keep my version]". Without a backup: open the file from disk (missing
   files are skipped and counted: one status note "2 files from the last session were not found"). Restore caret,
   scroll, view, bookmarks, wrap, tab settings.
4. Backups whose tab id is not in the session (session lost or older than the backup) are restored as extra tabs
   (titled by their path or "recovered N").
5. If `clean_exit == 0`, the status bar says "Pica did not close normally; unsaved changes were restored from backups."
6. Write a new session with `clean_exit=0`.
- `-nosession` skips 1-6 and never writes the session. With `Config.restore_session` off, only backups of unsaved tabs
  are restored (so nothing is ever lost).

### 13.8 Large mode
A tab is in large mode when its size >= `Config.large_mb` (default 8 MB) or it is on-demand. Effects: 8.6. The status
bar shows `Large`. Large files still open in the text view (only binary files start in hex view).

### 13.9 Change detection
- Polling: every 3 s (`Config.poll_seconds`), on WM_ACTIVATEAPP (activation) and when a tab becomes active, `stat`
  (GetFileAttributesExA, ~1 ms on M:) every named tab whose file is not busy/indexing. Changes are acted on only while
  Pica is the active app and no modal dialog or progress window is open; otherwise they wait.
- Change notifications: `FindFirstChangeNotificationA(dir, FALSE, LAST_WRITE | SIZE | FILE_NAME)` for up to 16 distinct
  folders of open files on local drives (`GetDriveTypeA` fixed/removable) and on M: (they fire for writes made inside
  the VM); a signal triggers an immediate poll of the tabs in that folder.
- Change = size or mtime differ from the tab's recorded disk state. A change must be stable for one more check 1 s
  later before acting (a writer may still be writing).
- Unmodified document and `Config.reload_silent` (default on): reload (13.4), keep caret line/column and scroll, status
  note "Reloaded X (changed on disk)". Otherwise the notification bar in that tab: "X was changed by another program.
  [Reload] [Keep my version]". Keep = record the new disk state (asks again only on the next change).
- File missing: the bar "X no longer exists on disk. [Save] [Close tab]"; the document becomes always-modified (the
  exit dialog will list it). If it reappears, treat as changed.
- On-demand documents hold a deny-write handle, so the file can't change; if we had to open with share-write (14.1)
  a change shows the bar with [Reload] only.
- Our own saves record the new state immediately, so they never trigger a reload.

## 14. Large files and long lines

### 14.1 On-demand documents (size > ondemand threshold, < 4 GB)
- Open with `FS_READ | FS_DENY_WRITE` (others can still read; nobody can write or delete it while the tab is open,
  probe). If that fails with a sharing violation, open with `FS_SHARE_ALL` as read-only with the bar "Another program
  has this file open for writing; opened read-only."
- `FileSource` with a 64-block (4 MB) LRU cache of 64 KB blocks.
- `Document::init_source_indexing` (pieces over the file, no counts) and start the indexer thread (`file_index`, its own
  handle, 64 KB reads, counts breaks per block + EOL counts, posts progress every 1%). Meanwhile:
  - the hex view works (read-only, `indexing` blocks edits);
  - the text view shows the first screen of text from offset 0 (lines laid out by scanning forward; no line numbers,
    no scrollbar thumb) with a bar "Indexing X... 43% [Cancel]";
  - Cancel closes the tab.
- When indexing completes: `finish_indexing` -> EV_INDEXED -> full text view, editing enabled.
- Encoding detection uses the first 64 KB only.
- Editing works like any document (add buffer in memory). Saving: 13.3 with the on-demand hooks. Convert encoding:
  disabled in 1.0. Backups: none ("Unsaved changes to files over 16 MB are not backed up" note once per tab when it
  first becomes modified). Replace All: allowed with progress.

### 14.2 Long lines
- Lines longer than LAY_LONG (64 KB) get a `LongLineIndex` (checkpoint every 4 KB: byte offset + display cell), built
  on demand with progress for lines > 4 MB, cached for up to 8 lines per view (LRU), invalidated by edits inside the
  line. Painting decodes only a LAY_WINDOW (64 KB) window around the visible cells; Home/End/column use checkpoints.
- Lexing sees the first LEX_LINE_MAX (16,384) chars of a line; the rest is default colour.
- Wrap mode on long lines: character wrap via checkpoints (8.1).
- Binary data opened as text (no NULs, e.g. minified files) stays responsive by these rules.

## 15. Win layer

### 15.1 Shared win headers (created in Wave 0)
- `app.h`: `struct App` (hinst, frame/toolbar/tabbar/statusbar/notifybar/isearch/results HWNDs, the two main views,
  `Config cfg`, `char cfg_dir[260]`, `Vec<Tab*> tabs`, active index, selftest flag) and functions:
  `app_tab_count, app_tab, app_active, app_active_index, app_activate(int), app_command(int id), app_status_note(fmt, ...),
  app_update_title, app_update_status, app_update_tab(Tab*), app_layout, app_apply_config, app_on_doc_changed(Tab*),
  app_now_ms`.
- `tab.h`: `struct Tab` (id, Document* doc, Highlighter* hl, DocLines lines, LineMarks bookmarks, RangeMarks marks,
  const LangInfo* lang, lang_forced, enc_forced, path, untitled, untitled_no, view (VIEW_TEXT/VIEW_HEX), Selection sel,
  TextViewState tv (top_line, top_subrow, hscroll, desired_cell, wrap, tab_width, use_spaces, overwrite), HexViewState
  hv (top_row, HexCaret caret), disk state (FsStat, missing, shared_ro, pending change), FileSource* fsrc, ondemand,
  large, indexing + pct, saving, notify state, backup state (gen, last_seq, dirty_since), pending io job count) and
  `tab_new, tab_free, tab_find_path, tab_title(Tab*, char*, u32)`.
- `views.h`: text view and hex view APIs: `textview_create(HWND parent, int id, int is_panel)`,
  `textview_attach(HWND, Tab*)`, `textview_set_selection(HWND, epos anchor, epos caret, int scroll_mode)`,
  `textview_ensure_visible(HWND, epos, int center)`, `textview_invalidate(HWND)`, `textview_config_changed(HWND)`,
  `textview_caret_info(HWND, CaretInfo*)`, `textview_set_panel_handler(HWND, PanelHandler*)`; the same set for
  `hexview_*` plus `hexview_set_inspector(HWND, int on)`. Views notify the frame with `WM_APP_CARET` (caret/selection
  moved) and `WM_APP_DOCSTATE` (modified/overwrite changed).
- `cmds.h`: every command handler prototype `void cmd_xxx(void)` and state function `UINT cmdstate_xxx(void)`
  (MF_GRAYED / MF_CHECKED bits); `cmdtable.cpp` maps IDM_* to them (Wave 0 fills the whole table).
- `ui.h` (util.cpp): `ui_ask(int qid, const char* text, int buttons)` (all modal questions; in selftest mode answers
  come from a script queue), `ui_error(const char* fmt, ...)`, `ui_ansi_to_ecp`, `ui_ecp_to_ansi`, path helpers.
- `progress.h`: `class UiProgress : public ProgressCb` (shows a modal progress window with Cancel after 500 ms, pumps
  paint messages), `progress_status_begin/step/end` (non-modal status-bar progress for background jobs).
- `docio.h`: `docio_open(const char* path, const OpenOpts*, Tab** out)`, `docio_new(Tab** out)`,
  `docio_save(Tab*, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null, SaveDone cb, void* ctx)`,
  `docio_save_wait(Tab*, ...)`, `docio_reload(Tab*, const EncSpec* as_or_null, int undoable)`, `docio_revert(Tab*)`.
- `sessmgr.h`, `watch.h`, `largefile.h`, `iothread.h`, `fifworker.h`, `selftest.h`, `bench.h`, `winenc.h`, `clip.h`,
  `render.h`, `notifybar.h` (`notifybar_show(Tab*, int kind, const char* text, const NotifyButton* b, u32 n, NotifyFn fn,
  void* ctx)`, `notifybar_hide(Tab*, int kind)`).
- Cross-module functions that Wave 0 must declare (with stubs) so parallel tasks can call each other:
  - `dialogs.h` (T12, dlgmisc/dlggoto/dlgopts): `int dlg_exit_prompt(Tab** tabs, u32 n, u8* save_flags)` (returns
    EXIT_SAVE / EXIT_KEEP / EXIT_DISCARD / EXIT_CANCEL), `int dlg_convert_loss(const char* enc_label, const
    ConvertReport& r, u32 first_line)` (CONVERT_OK / CONVERT_GOTO / CONVERT_CANCEL), `void dlg_properties(Tab*)`,
    `void dlg_about()`, `int dlg_goto(Tab*, int hex_mode)`, `int dlg_options(int page)`.
  - `clip.h` (T12): `int clip_copy(Tab*, const Selection&, int hex_pane)`, `int clip_get(ClipData* out)` +
    `void clip_free(ClipData*)` (ClipData: raw bytes + their EncSpec if from Pica, else decoded `ecp` text),
    `int clip_has_data()`.
  - `docio.h` (T09): also `void doccmd_offer_utf8(Tab*, const char* msg)` (notification bar of 12.4) and
    `int docio_close(Tab*, int ask)`.
  - `sessmgr.h` (T13): `sessmgr_startup()`, `sessmgr_restore_from(const char* dir)`, `sessmgr_doc_changed(Tab*)`,
    `sessmgr_state_changed()`, `sessmgr_tab_saved(Tab*)`, `sessmgr_tab_closed(Tab*)`, `int sessmgr_exit()` (0 =
    cancel), `sessmgr_emergency()`, `sessmgr_idle()`.
  - `watch.h` (T14): `u32 watch_handles(HANDLE* out, u32 max)`, `void watch_signalled(u32 i)`, `void watch_poll(int all)`,
    `void watch_tab_added(Tab*)`, `void watch_tab_removed(Tab*)`, `void watch_tab_saved(Tab*)`.
  - `largefile.h` (T14): `int largefile_begin(Tab*)`, `void largefile_cancel(Tab*)`, `void largefile_on_msg(WPARAM,
    LPARAM)`.
  - `iothread.h` (T09/T13): `iothread_start/stop`, `u32 iothread_submit(IoJob* job)` (job heap-allocated, owned by the
    queue), `void iothread_cancel_tab(u32 tabid)`, `int iothread_wait_tab(u32 tabid, u32 timeout_ms)`,
    `int iothread_flush(u32 timeout_ms)`, `void iothread_on_done(WPARAM, LPARAM)`.
  - `fifworker.h` (T11): `fif_start(const FifQuery&)`, `fif_stop()`, `fif_on_msg(WPARAM, LPARAM)`; `isearch_show()`,
    `results_show(int)` in the same header.
  - Window messages (app.h): `WM_APP_CARET = WM_APP+1, WM_APP_DOCSTATE, WM_APP_IODONE, WM_APP_INDEX, WM_APP_FIF,
    WM_APP_COPYDATA_OPEN, WM_APP_NOTIFY`.

### 15.2 Main window layout
Top to bottom: menu, toolbar (flat, 16x16 16-colour icons, tooltips), tab bar (owner-drawn SysTabControl32: modified
marker `*`, `(saving)`/`(indexing)` suffix, close box on the active tab, middle-click closes, drag to reorder, tooltip
with the full path, right-click menu Close / Close Others / Copy Full Path / Open Containing Folder), notification bar
(one per tab, shown when the active tab has one), the active view (text or hex), the incremental search bar (when
open), the results panel with splitter (when open), status bar. Window title: `name.ext * - Pica` (`new 1` for
untitled; `[Read-only]` suffix).

### 15.3 Status bar parts
| Part | Text view | Hex view | Click |
|---|---|---|---|
| 1 position | `Ln 120, Col 9` | `Offset 0x00001F4A (8010)` | Go To |
| 2 selection | `Sel 12` / `Sel 3 lines` | `Sel 16 bytes` | - |
| 3 char | `U+00E9` of the char at the caret | `Byte 0x4D 77` | - |
| 4 language | `C/C++` / `Plain text` | same | Syntax popup |
| 5 encoding | `ANSI` | same | Encoding popup |
| 6 EOL | `CRLF` / `Mixed` | same | Line endings popup |
| 7 mode | `INS` / `OVR` | same | toggles |
| 8 view | `TEXT` / `HEX` | same | toggles |
| 9 flags | `RO`, `Large`, `Indexing 43%`, `Saving 20%`, transient notes (5 s) | same + `Size 81,920 (+16)` | - |

### 15.4 Menus (command IDs in `res/resource.h`, range 40000-40999)
```
File      New  Ctrl+N (IDM_FILE_NEW) | Open... Ctrl+O (IDM_FILE_OPEN) | Revert to Saved (IDM_FILE_REVERT)
          --- Save Ctrl+S (IDM_FILE_SAVE) | Save As... Ctrl+Alt+S (IDM_FILE_SAVEAS) | Save All Ctrl+Shift+S (IDM_FILE_SAVEALL)
          --- Close Ctrl+W (IDM_FILE_CLOSE) | Close All (IDM_FILE_CLOSEALL) | Close Others (IDM_FILE_CLOSEOTHERS)
          --- 1..10 recent files (IDM_MRU_FIRST..+9) | Clear Recent List (IDM_MRU_CLEAR) --- Exit Alt+F4 (IDM_FILE_EXIT)
Edit      Undo Ctrl+Z | Redo Ctrl+Y (IDM_EDIT_UNDO, IDM_EDIT_REDO)
          --- Cut Ctrl+X | Copy Ctrl+C | Paste Ctrl+V | Delete Del | Select All Ctrl+A (IDM_EDIT_CUT/COPY/PASTE/DELETE/SELECTALL)
          --- Line > Duplicate Ctrl+D | Delete Line Ctrl+L | Move Up Ctrl+Shift+Up | Move Down Ctrl+Shift+Down
                     (IDM_LINE_DUP, IDM_LINE_DEL, IDM_LINE_UP, IDM_LINE_DOWN)
              Toggle Comment Ctrl+Q (IDM_EDIT_COMMENT)
              Case > UPPERCASE Ctrl+Shift+U | lowercase Ctrl+U (IDM_EDIT_UPPER, IDM_EDIT_LOWER)
              Indent Tab | Unindent Shift+Tab (IDM_EDIT_INDENT, IDM_EDIT_UNINDENT; menu text only, keys handled by the view)
          --- Insert/Overwrite Ins (IDM_EDIT_OVERWRITE)
Search    Find... Ctrl+F (IDM_SEARCH_FIND) | Find Next F3 | Find Previous Shift+F3 (IDM_SEARCH_NEXT, IDM_SEARCH_PREV)
          Find Selection Ctrl+F3 | Find Selection Backwards Ctrl+Shift+F3 (IDM_SEARCH_SELNEXT, IDM_SEARCH_SELPREV)
          Incremental Search Ctrl+I (IDM_SEARCH_ISEARCH) | Replace... Ctrl+H (IDM_SEARCH_REPLACE)
          Find in Files... Ctrl+Shift+F (IDM_SEARCH_FIF) | Next Result F4 | Previous Result Shift+F4 (IDM_SEARCH_RESNEXT, IDM_SEARCH_RESPREV)
          Clear Marks (IDM_SEARCH_CLEARMARKS)
          --- Go To... Ctrl+G (IDM_SEARCH_GOTO) | Go to Matching Brace Ctrl+B (IDM_SEARCH_BRACE)
          --- Bookmarks > Toggle Ctrl+F2 | Next F2 | Previous Shift+F2 | Clear All (IDM_BM_TOGGLE/NEXT/PREV/CLEAR)
View      Hex View Ctrl+Shift+H (IDM_VIEW_HEX) | Data Inspector (IDM_VIEW_INSPECTOR)
          --- Word Wrap (IDM_VIEW_WRAP) | Show Whitespace and Line Endings (IDM_VIEW_SHOWWS) | Line Numbers (IDM_VIEW_LINENUM)
          --- Toolbar (IDM_VIEW_TOOLBAR) | Status Bar (IDM_VIEW_STATUSBAR) | Find Results (IDM_VIEW_RESULTS)
Document  Syntax > (Auto) | Plain Text | C/C++ | Resource Script | Makefile | Batch | INI | Assembly | Python | HTML |
                   JavaScript | CSS | XML | Markdown | Diff   (IDM_LANG_AUTO, IDM_LANG_PLAIN, IDM_LANG_FIRST + registry index)
          Encoding > (12.2)  (IDM_ENC_RELOAD_ANSI/OEM/UTF8/UTF16LE/UTF16BE, IDM_ENC_RELOAD_CP_FIRST + i,
                              IDM_ENC_CONV_ANSI/UTF8/UTF8BOM/UTF16LE/UTF16BE)
          Line Endings > Windows (CRLF) | Unix (LF) | Mac (CR) (IDM_EOL_CRLF/LF/CR)
          Tabs > Width 2 | 4 | 8 | Insert Spaces (IDM_TAB_2/4/8, IDM_TAB_SPACES)
          --- Read-Only (IDM_DOC_READONLY) | Properties... (IDM_DOC_PROPERTIES: path, size, dates, encoding, EOL counts, lines)
Tools     Options... (IDM_TOOLS_OPTIONS)
Window    Next Tab Ctrl+Tab / Ctrl+PgDn | Previous Tab Ctrl+Shift+Tab / Ctrl+PgUp (IDM_WIN_NEXT, IDM_WIN_PREV)
          --- open tabs 1..30 with a check on the active one (IDM_WIN_FIRST + i)
Help      Keyboard Shortcuts F1 (IDM_HELP_KEYS) | Regular Expressions (IDM_HELP_REGEX) | About Pica (IDM_HELP_ABOUT)
Tab bar context menu: IDM_TAB_CLOSE, IDM_TAB_CLOSEOTHERS, IDM_TAB_COPYPATH, IDM_TAB_OPENFOLDER
```
Menu state (grey/check) is set in WM_INITMENUPOPUP from the `cmdstate_*` functions; the Window and recent-file lists
are rebuilt there.

### 15.5 Keyboard shortcuts (accelerator table `IDA_MAIN` in `res/menu.rc2`)
| Keys | Command | Keys | Command |
|---|---|---|---|
| Ctrl+N / Ctrl+O | New / Open | Ctrl+F / Ctrl+H | Find / Replace |
| Ctrl+S | Save | Ctrl+Shift+F | Find in Files |
| Ctrl+Alt+S | Save As | F3 / Shift+F3 | Find next / previous |
| Ctrl+Shift+S | Save All | Ctrl+F3 / Ctrl+Shift+F3 | Find selection next / previous |
| Ctrl+W, Ctrl+F4 | Close tab | Ctrl+I | Incremental search |
| Alt+F4 | Exit (system) | F4 / Shift+F4 | Next / previous result |
| Ctrl+Z, Alt+Backspace | Undo | Ctrl+G | Go to line / offset |
| Ctrl+Y, Ctrl+Shift+Z | Redo | Ctrl+B, Ctrl+] | Matching brace |
| Ctrl+X, Shift+Del | Cut | Ctrl+F2 / F2 / Shift+F2 | Bookmark toggle / next / previous |
| Ctrl+C, Ctrl+Ins | Copy | Ctrl+Shift+H | Hex view |
| Ctrl+V, Shift+Ins | Paste | Ctrl+Tab, Ctrl+PgDn | Next tab |
| Ctrl+A | Select all | Ctrl+Shift+Tab, Ctrl+PgUp | Previous tab |
| Ctrl+D | Duplicate | F1 | Help (keyboard shortcuts) |
| Ctrl+L, Ctrl+Shift+L | Delete line | Ctrl+Q | Toggle comment |
| Ctrl+Shift+Up / Down | Move line(s) | Ctrl+Shift+U / Ctrl+U | UPPER / lower case |
View-local keys (not accelerators): arrows, Home/End, PgUp/PgDn, Tab/Shift+Tab, Ins, Del, Backspace, Enter, Esc.
Rules: never add Ctrl+Alt+<digit>, Ctrl+Alt+E, Ctrl+Alt+<+ or <> accelerators (AltGr characters on Finnish/German
layouts); Del and Ins are view keys (the menu shows them but the accelerator table does not contain plain Del/Ins).
Message loop order: if the message is for the Find dialog or one of its children, first try a small dialog
accelerator table (F3, Shift+F3, Ctrl+F3, F4, Shift+F4), then `IsDialogMessage`; else `TranslateAccelerator(frame,
IDA_MAIN)`, then `TranslateMessage/DispatchMessage`.

### 15.6 Toolbar
New, Open, Save, Save All | Cut, Copy, Paste | Undo, Redo | Find, Replace, Find in Files | Hex View (check) | Word Wrap
(check), Show Whitespace (check). 15 buttons from `res/toolbar.bmp` (generated by `tools/make_icons.py`, 16x16, 16
colours, magenta transparent), tooltips from the string table.

### 15.7 Command line and single instance
`pica.exe [-n<line>] [-hex] [-ro] [-multi] [-nosession] [-cfg <dir>] [-selftest [names]] [-log <file>] [-bench] [files...]`.
`-n`, `-hex`, `-ro` apply to the next file. If another Pica runs (FindWindowA on the frame class
`APP_NAME "Frame"`) and `Config.single_instance` is on and `-multi`/`-selftest`/`-bench` are not given, the command line
is sent with `WM_COPYDATA` (current directory + arguments) and the new process exits; the running one opens the files
and comes to the front. Files dropped on the window (WM_DROPFILES) open as tabs.

### 15.8 Options dialog (PropertySheetA, 5 pages, every option with a one-line explanation)
- **General**: Restore last session; Remember unsaved changes (backup) + after [5] s idle; Ask to save untitled
  documents on exit; Single instance; Recent files [10]; Check for changes on disk; Reload unmodified files silently.
- **Editor**: Font [Courier New, 10 pt] [Change...]; Tab width [4]; Insert spaces; Use language tab settings;
  Auto-indent; Line numbers; Word wrap for new tabs; Show whitespace; Highlight matching braces.
- **Files**: New document encoding [ANSI / UTF-8 / UTF-8 with BOM / UTF-16 LE / UTF-16 BE] and line ending
  [CRLF / LF / CR]; Detect UTF-8 without BOM; Open .nfo files as OEM; Open binary files in hex view; Read files on
  demand above [16] MB; Large-file mode above [8] MB; Keep a .bak copy when saving.
- **Colours**: list of the 26 styles + UI colours, sample line, [Colour...] (ChooseColor), [Bold], [Default], [Reset
  All]. System-colour defaults are shown as "(system)".
- **Hex**: Start in overwrite mode; Show the data inspector; Inspector byte order LE/BE; Modified-byte colour.
OK/Apply write `pica.ini` (`file_save` temp + rename) and call `app_apply_config`.

### 15.9 Other dialogs
- Go To (Ctrl+G): one edit field + live result line; text view: line[:col], hex view: offset (6.17).
- Exit / close-many prompt: list of modified named files with checkboxes; buttons [Save Selected] [Keep for Next
  Session] (only when backups are on; on-demand files can't be kept and are listed with "(not kept: too large)")
  [Discard] [Cancel]. Untitled documents are not listed when backups are on, unless `ask_untitled_on_exit`.
- Encoding loss prompt (12.2), Properties, About (name, version, build date, "all our own code", cfg_dir path).
- Save As dialog template with Encoding and Line endings combos (12.2).

### 15.10 Progress
`UiProgress` (modal): appears 500 ms after start, shows text + bar + Cancel, pumps only WM_PAINT/timer/mouse for the
progress window (the main window repaints, input to it is ignored). Used by open (in-memory reads), find/replace on
big documents, conversion, `docio_save_wait`, index build of long lines. Background work (save worker, indexer, FiF)
uses the status bar and tab labels instead.

### 15.11 Idle work and timers
The main loop runs idle work when the queue is empty: highlighter `work(200 lines)` slices for the active tab
(target = viewport bottom + 1 screen), then other tabs' backup scheduling. Timers on the frame: 1 = poll (3 s),
2 = backup check (1 s), 3 = session debounce (2 s one-shot), 4 = status note expiry. The text view has its own caret
blink (system) and drag-scroll timer.

### 15.12 Emergency handling
`__try/__except` around the dispatch of each message in WinMain: on an exception, write synchronous backups of all
modified in-memory documents (direct FileSys calls, no thread), write the session with `clean_exit=0`, show "Pica hit an
internal error. Your unsaved work was backed up and will be restored at the next start.", and exit.

## 16. Error and edge-case policy
| Situation | Behaviour |
|---|---|
| Out of memory during an edit | Edit refused, document unchanged, status "Not enough memory: close some tabs" |
| Out of memory on open | Message; for files > 4 MB offer "Open on demand (slower, uses little memory)" |
| Save fails (disk full, access, sharing) | Message box with the reason and what was kept ("Your file is unchanged."); document stays modified |
| Read-only file | Opens read-only with a bar; saving asks to clear the attribute or Save As |
| File locked by another program | Load still works (share read+write); save uses the in-place fallback or reports |
| File deleted while open | Bar; doc stays, marked modified |
| File > 4 GB - 2 | Refused with a message |
| Binary file | Hex view + bar; text view shows control pictures |
| Undecodable bytes | `xHH` cells; preserved; conversion reports them |
| Typing unrepresentable char | Refused + bar (12.4) |
| Regex too complex | Search stops with a status message, nothing changed |
| Replace All cancelled | Nothing applied |
| Very long line | Windowed layout (14.2) |
| UTF-16 misaligned edit | Refused (7.3) with a status note |
| Crash | Emergency backups (15.12); next start restores |
| Power loss | Backups (<= 5-30 s old) + session (alternating) restore; saves are temp+rename so files are old or new, never empty |
| Settings file corrupt | Bad lines ignored, defaults used, file rewritten on the next save of settings (unknown lines kept) |
| Missing language files | Built-in RCDATA used; a broken override file is skipped and named in the status bar once |
| Message boxes | Only for user-initiated actions that failed or need a decision; everything else is a status note or bar |

## 17. Test strategy

Debug from text logs first; screenshots only for visuals.

### 17.1 Mac unit tests (core)
- `tests/Makefile` (GNU make, Apple clang): strict build `clang++ -std=c++98 -pedantic -Wall -Wextra -Werror -Wshadow
  -fno-exceptions -fno-rtti -DE_DEBUG -Isrc/core`, run at `-O2`, then an ASan + UBSan build
  (`-fsanitize=address,undefined -fno-sanitize-recover=all`) run, plus `tools/lint.py`.
- Targets: `make -C tests` (= `check`: lint + both runs of every suite), `make -C tests <suite>` (one suite, both
  builds; only the sources that suite needs, listed per suite in the Makefile, so parallel agents are not blocked by
  each other's work in progress), `make -C tests mac` (C++11 extras in `tests/mac/`), `make -C tests data`
  (regenerate `tests/data/` with Python).
- Framework `tests/tf.h`: `TF_CHECK(cond)`, `TF_EQ_U(a,b)`, `TF_EQ_S(a,b)`, `TF_EQ_MEM(a,b,n)`, `tf_log`. Runner
  `tests/t_main.cpp`: `coretest [suite...] [-q] [-seed N] [-long] [-data DIR] [-tmp DIR]`; prints one line per failed
  check and a summary `coretest: N checks, M failed`; exit code = failed suites. Leak check: `e_alloc_count()` equal
  before and after each suite.
- Suites (one file each, `tests/t_<suite>.cpp`): estr, emem, ptree, undo, doc, marks, codec, detect, ucd, layout,
  lexer, regex, search, fif, hexfmt, edtype, edlines, edhex, ini, config, session, fileops, winenc (Win32 only),
  adversarial (Wave 4).
- Required styles of tests:
  - **Differential random tests**: piece tree + undo vs a flat byte-array model (insert/delete/undo/redo/save-point,
    `check()` after each op, line queries vs brute force), >= 200,000 ops by default, 2,000,000 with `-long`;
    UTF-16 aligned variants; `SynthSource` documents near 2^31 and 2^32 - 2 bytes for offset arithmetic.
  - **Round trips**: decode -> encode is byte-exact for random byte strings in every codec; SBCS tables vs Python
    vectors; UTF-8 invalid corpus (overlongs, surrogates, > U+10FFFF, truncated) vs expected markers.
  - **Regex**: a table of (pattern, text, expected match/groups) >= 300 cases; pathological patterns `(a*)*b`,
    `(a|aa)*c`, `(x+x+)+y` on 100 KB inputs finish under 1 s (Pike) or hit the budget (backtracker);
    `tests/mac/rxfuzz.cpp` compares with `std::regex` (ECMAScript) on generated patterns from the shared subset.
  - **Lexer convergence**: after random edits, incremental re-lex == full re-lex, for every language.
  - **File ops with fault injection** (`FaultFs`): every step of the save sequence failing in turn leaves either the
    old or the new content at the target path, never a partial file; temp files are reported; backups survive torn
    writes (truncate the newest file at every byte boundary class: header, data, trailer); A/B session survives a
    damaged newer file.
  - **Edit commands**: golden before/after tables (text + selection) for every ed_* and hx_* function, in ANSI,
    UTF-8 and UTF-16 LE documents, with CRLF and LF.

### 17.2 VC6 console test (`coretest.exe`)
`nmake test` builds `coretest.exe` from the **same** `tests/t_*.cpp` + core sources + `src/win/fs_win32.cpp` +
`src/win/winenc.cpp` (no `fs_posix.cpp`, no `tests/mac/`) and runs `coretest -q -tmp C:\PICATEST\tmp`. File-op suites
also run against `-tmp M:\dev\editor\tests\tmp` (Samba semantics). Check counts must match the Mac run except
suites marked platform-specific.

### 17.3 Selftest (`pica.exe -selftest`)
- `pica.exe -selftest [name...] -cfg C:\PICATEST [-log file] [-fixdir dir]` runs scripted scenarios against the real
  windows (visible: `ShowWindow` twice), writes `<cfg>\selftest.log` (`PASS name`, `FAIL name: detail`, summary line
  `selftest: N passed, M failed`), exits with the failure count. Run with `w98 run --show '...' -t 600`.
- Each win module owns a function `void st_<module>(StCtx* c)` in its own `.cpp` (declared in `selftest.h`, stubs in
  Wave 0): st_frame, st_textview, st_edit, st_hexview, st_search, st_fif, st_dialogs, st_docio, st_session, st_watch,
  st_largefile. Wave 3 adds cross-module scenarios in `selftest.cpp` (st_scenarios).
- `StCtx` helpers (selftest.cpp, Wave 0 basics, completed in Wave 3): `st_fixture(name, bytes, n)` writes a file under
  `<cfg>\fixtures` (or `-fixdir`), `st_open(path)`, `st_cmd(IDM_...)` (through `app_command`), `st_keys("^s",
  "+{RIGHT}", "abc")` (posts WM_KEYDOWN/WM_CHAR through the message loop with a faked keyboard state, so
  accelerators are exercised), `st_pump(ms)`, `st_wait_idle(timeout)` (I/O worker, indexer, FiF and highlighter idle),
  `st_answer(qid, button)` (queues answers for `ui_ask`), `st_file_bytes(path, EBytes*)`, `ST_CHECK(c, cond, fmt,
  ...)`.
- Required scenarios (by the end of Wave 3): open each fixture kind (ANSI, UTF-8 with/without BOM, UTF-16 LE/BE,
  CR-only, mixed EOL, binary, invalid UTF-8, empty, 20 MB on-demand); type/undo/redo/save and compare bytes on disk;
  toggle hex, edit nibbles in both modes, undo across views; find/replace/replace-all/regex errors; find in files over
  a fixture tree on C: and M:; session save + restart simulation (sessmgr re-init from files); backup + simulated crash
  (kill backups mid-write via FaultFs on the I/O worker); external change (fixture rewritten by the test) -> silent
  reload and the bar; read-only file; locked file (a second handle held by the test); every accelerator in 15.5 maps to
  its command.

### 17.4 Manual checks over VNC (lock held; no timing while connected)
Normal Win32 windows accept VNC mouse/keyboard: menus, dialogs, tab drag, splitter, IME-less typing, context menus,
Options pages. Screenshots with `samba/shot86.sh` for visual review (colours, fonts, hex layout, bars). Each Wave 2-4
task lists the screenshots it must take and look at.

### 17.5 Benchmarks (`pica.exe -bench`)
Window visible, no VNC. Writes `<cfg>\bench.txt`: full repaint (text, hex) median/worst over 60 repaints, 1-line
scroll, page down, keystroke, open 1/10/100 MB (SynthSource files written to `C:\PICATEST\bench`), find literal/regex
in 10 MB, replace-all 10,000 matches, startup. Each line compares with the 3.2 budget and prints OK/OVER.

## 18. Folder layout and build

### 18.1 Layout
```
dev/editor/
  Makefile               NMAKE (VC6): all, test, release, clean, per-object targets      (CRLF)
  STATUS.md  README.md (Wave 4)  .gitignore
  docs/                  DESIGN.md TASKS.md HANDOFF.md research/
  res/                   appinfo.h resource.h pica.rc menu.rc2 dlg_file.rc2 dlg_find.rc2 dlg_goto.rc2 dlg_opts.rc2
                         dlg_misc.rc2 ids_file.h ids_find.h ids_opts.h ids_misc.h toolbar.bmp pica.ico           (CRLF)
  src/core/              *.h *.cpp (section 4.1 list), cptables.cpp, ucdtables.cpp (generated, committed)
  src/langs/             cpp.lng rc.lng make.lng bat.lng ini.lng asm.lng python.lng js.lng css.lng diff.lng
                         (generic) + html.lng xml.lng md.lng (metadata for the hand-written lexers)   (CRLF)
  src/help/help.md       in-app help (embedded)                                           (CRLF)
  src/win/               *.h *.cpp (section 4.1 list)
  tests/                 Makefile tf.h t_main.cpp t_<suite>.cpp fs_posix.cpp data/ tmp/ (ignored) mac/
  tools/                 lint.py gen_cptables.py gen_ucd.py make_icons.py gen_testdata.py mkzip.sh
  obj/                   VC6 objects (ignored)       release/   (Wave 4)
  probe/                 phase-1 probe (read-only)
```

### 18.2 NMAKE targets (run with `~/Documents/86box/samba/w98 build editor [target]`)
| Target | Result |
|---|---|
| `nmake` / `nmake all` | `pica.exe` (objects in `obj\`) |
| `nmake test` | `coretest.exe`, then runs `coretest.exe -q -tmp C:\PICATEST\tmp` |
| `nmake obj\<name>.obj` | compiles one source (per-file /W3 check during parallel waves) |
| `nmake t_<suite>` | builds `obj\t_<suite>.exe` from that suite's sources only (same lists as the Mac Makefile) and runs it |
| `nmake release` | clean rebuild of `pica.exe`, copies it with `README.TXT` (CRLF) into `release\Pica-1.0\` |
| `nmake clean` | deletes `obj\*`, `*.exe`, `*.res` |
| `nmake DEBUG=1 ...` | `/Zi /Od /DE_DEBUG` (assertions on) |
Flags: `CFLAGS = /nologo /W3 /O2 /G6 /MT /DWIN32 /D_WINDOWS /Isrc\core /Isrc\win /Ires`. Link: `user32.lib gdi32.lib
kernel32.lib comctl32.lib comdlg32.lib shell32.lib advapi32.lib imm32.lib /subsystem:windows`. Resources:
`rc /r /fo obj\pica.res res\pica.rc` (`/i res`). Path-specific inference rules `{src\core\}.cpp{obj\}.obj:` and
`{src\win\}.cpp{obj\}.obj:` with `/Foobj\`. The zip is made on the Mac: `tools/mkzip.sh` -> `release/pica-1.0.zip`.
On the Mac: `make -C tests` (17.1). Generators: `python3 tools/gen_cptables.py`, `python3 tools/gen_ucd.py`,
`python3 tools/make_icons.py` (Pillow), outputs committed.

### 18.3 Version and name
`res/appinfo.h`: `#define APP_NAME "Pica"`, `APP_EXE "pica.exe"`, `APP_INI "pica.ini"`, `APP_VERSION "1.0"`,
`APP_VERSION_RC 1,0,0,0`, `APP_CLASS_FRAME APP_NAME "Frame"`, `APP_RAWFMT APP_NAME ".RawBytes"`. The rc file uses it for
VERSIONINFO and captions. Renaming = this header + `APP = pica` in the Makefile + `tools/mkzip.sh`.

---

## Appendix A: product name
Chosen: **Pica** (typewriter "pica" = 10 characters per inch, the classic monospace pitch). Short, 8.3-safe, not a known
editor or Win98 product. Alternatives considered: **Kyna** (from Finnish "kyna", pen; ASCII spelling loses the
umlaut), **Duplex** (two views of one document; generic word), **Quire**, **Tessera**, **Byteline**, **Ledger**
(taken by accounting software), **Nibble** (too close to QBasic Nibbles), **Hexpad** (several existing tools).

## Appendix B: default colours (COLORREF RGB; "system" = system colour)
| Style | Colour | Bold | Style | Colour | Bold |
|---|---|---|---|---|---|
| DEFAULT | system window text on window | - | TAG | 0,0,160 | - |
| COMMENT | 0,128,0 | - | ATTR | 160,0,0 | - |
| STRING | 128,0,0 | - | ENTITY | 128,0,128 | - |
| CHAR | 128,0,0 | - | HEADING | 0,0,160 | yes |
| NUMBER | 0,128,128 | - | EMPH | 128,0,0 | - |
| KEYWORD1 (statements) | 0,0,255 | yes | CODE | 0,112,112 | - |
| KEYWORD2 (types) | 128,0,128 | - | DIFF_ADD | 0,128,0 | - |
| KEYWORD3 (library/extra) | 0,96,160 | - | DIFF_DEL | 192,0,0 | - |
| KEYWORD4 (user) | 160,80,0 | - | DIFF_HUNK | 128,0,128 | yes |
| PREPROC | 160,80,0 | - | DIFF_HEADER | 0,0,160 | yes |
| OPERATOR | 0,0,128 | - | INVALID | 192,0,0 on 255,224,224 | - |
| LABEL | 128,0,128 | yes | SECTION (INI) | 0,0,160 | yes |
| VARIABLE | 160,80,0 | - | KEY (INI) | 0,96,160 | - |

| UI colour | Default | UI colour | Default |
|---|---|---|---|
| MARK_BG | 255,240,120 | GUTTER_BG | system button face |
| BRACE_BG | 200,230,255 | GUTTER_FG | 128,128,128 |
| BRACE_BAD_FG | 255,0,0 | BOOKMARK | 0,0,192 |
| WS_FG | 176,176,176 | HEX_OFFSET | 0,0,128 |
| CTRL_FG / CTRL_BG | 96,96,96 / 224,224,224 | HEX_MODIFIED | 255,0,0 |
| BAD_FG / BAD_BG | 160,0,0 / 255,208,208 | HEX_MIRROR_BG | 210,210,255 |
| HEX_BOM | 160,160,160 | ISEARCH_FAIL | 255,200,200 |
Selection = system highlight colours. Notification bar = system info background/text (COLOR_INFOBK/COLOR_INFOTEXT).

## Appendix C: settings (`pica.ini`) and the Config struct
Keys in [General], [Editor], [Files] and [Hex] map to `Config` fields of the same name (u8 for booleans, u16/i32 for
numbers, char arrays for strings, `EncSpec new_enc`). [Window] keys map to `win_<key>` fields, [Find] keys to
`find_<key>`, colours to `StyleColor style[ST_COUNT]` and `u32 ui[UIC_COUNT]`, lists to `mru[CFG_MRU_MAX][260]`,
`find_hist[CFG_HIST_MAX][256]`, `repl_hist`, `fif_dirs`, `fif_masks` (same shape).
| Section / key | Default | Range / values |
|---|---|---|
| [General] restore_session | 1 | 0/1 |
| backup_enabled / backup_seconds | 1 / 5 | 0/1 / 2-300 |
| ask_untitled_on_exit | 0 | |
| single_instance | 1 | |
| mru_count | 10 | 0-10 |
| check_disk / reload_silent / poll_seconds | 1 / 1 / 3 | / / 1-60 |
| [Editor] font_face | Courier New | <= 31 chars |
| font_pt / font_px | 10 / 0 | font_px > 0 means a raster pixel height (Fixedsys) |
| tab_width / use_spaces / use_lang_tabs | 4 / 0 / 1 | 1-16 |
| auto_indent / line_numbers / wrap_default / show_ws / brace_match | 1 / 1 / 0 / 0 / 1 | |
| [Files] new_enc / new_eol | ansi / crlf | enc tokens / crlf, lf, cr |
| detect_utf8 / nfo_oem / binary_hex | 1 / 1 / 1 | |
| ondemand_mb / large_mb | 16 / 8 | 4-64 / 1-64 |
| keep_bak | 0 | |
| [Hex] hex_overwrite / inspector_on / inspector_be | 1 / 0 / 0 | |
| [Colors] s_<style> (e.g. s_comment=008000,0) / u_<ui> | Appendix B | RRGGBB hex, bold flag; "sys" = system |
| [Window] x, y, w, h, max, toolbar, statusbar, results_h, find_x, find_y | 40,40,760,540,0,1,1,150,-1,-1 | |
| [Find] case, word, wrap, mode, dotall, dir_up, fif_sub | 0,0,1,0,0,0,1 | |
| [Recent] f1..f10 | - | paths |
| [FindHistory] h1..h15, [ReplaceHistory] h1..h15, [FifFolders] h1..h15, [FifMasks] h1..h15 | - | strings (ANSI) |
