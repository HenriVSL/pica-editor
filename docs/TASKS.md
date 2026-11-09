# Pica 1.0: implementation tasks

Read `docs/DESIGN.md` first; section numbers below refer to it. Each task is one focused Opus agent session
(roughly 1,000-3,000 lines of code plus tests). Waves run in order; tasks inside a wave run in parallel. The overseer
(session 86box-d1) commits to git after each wave and runs the full builds.

## Rules for every task

### Files and ownership
- Project folder: `~/Documents/86box/Shared/dev/editor/` (= `M:\dev\editor`). Never touch other projects in
  `Shared/dev/`. `probe/` and `docs/research/` are read-only.
- **Edit only the files your task owns** (listed per task). Shared files (Makefiles, resource script, IDs, interface
  headers) are created in Wave 0 and then belong to the task that owns them in each wave. If you need a change in a
  file you don't own, don't make it: describe the exact change in your HANDOFF section and in your final report (the
  overseer applies it between waves or assigns it).
- Headers: you may add members at the **end** of a class/struct you own, new private helpers in your `.cpp` files, and
  new private headers named `src/core/<module>_*.h` or `src/win/<module>_*.h` for your own module. Changing a
  signature declared in DESIGN section 6 or 15.1 needs the overseer; if you must, record it under "Interface changes"
  in HANDOFF.md.
- **No new compilation units**: the Makefiles list every `.cpp` (Wave 0). Put code in the files you own.
- Don't edit `docs/DESIGN.md` or `docs/TASKS.md`. Append your section to `docs/HANDOFF.md` (that file is shared
  append-only: add your section at the end in one edit, never change other sections).
- Agents don't commit to git.

### Code rules
- VC6 dialect: DESIGN 5.3 (loop variables declared before the loop, no STL, no `long long` outside `etypes.h`, no
  in-class static const initialisers, ASCII-only sources without BOM, CRLF for `res/*`, `Makefile`, `*.lng`, `help.md`).
  `python3 tools/lint.py` checks most of this.
- Core (`src/core`) never includes `windows.h` or `src/win` headers.
- Win98 API rules: DESIGN 3.1 and 5.3 item 9 (stub list, no FILE_SHARE_DELETE, ANSI windows, ExtTextOutW).
- No exceptions, no RTTI; functions return E_* codes; out of memory never crashes (DESIGN 5.1).
- Every modal question goes through `ui_ask` (so selftests can answer it); every user-visible failure has a clear
  message.

### Build and test
- Mac first: `make -C tests <suite>` for your suites (strict clang + ASan/UBSan), then `make -C tests` (all) when you
  finish. Fix every warning.
- VC6: compile your own files with `~/Documents/86box/samba/w98 build editor obj\<name>.obj` and run your suites with
  `w98 build editor t_<suite>`. During a parallel wave the full `nmake` / `nmake test` may fail because another task
  is mid-edit; that's expected. Your acceptance needs your objects warning-free at /W3 and your suites passing on
  VC6; do a full build at the end if the tree is green and report the result either way.
- Win-layer tasks verify in the VM: `pica.exe -selftest <your st_ function> -cfg C:\PICATEST` via
  `w98 run --show 'M:\dev\editor\pica.exe -selftest textview -cfg C:\PICATEST' -t 600`, then read
  `C:\PICATEST\selftest.log` (`w98 get` or `w98 sh 'type C:\PICATEST\selftest.log'`). Then manual checks over VNC
  (`~/Documents/86box/samba/vnc98.sh`) and screenshots with `~/Documents/86box/samba/shot86.sh <file.png>` (read the
  PNG). VNC slows rendering about 7x: never measure timing while connected.
- No `pica.exe` may be running when you link (`w98 ps`, `w98 kill <pid>` for processes you started).

### The VM lock (mandatory, VM A only)
- **Every** use of VM A, including builds, `w98 sh/put/get/run/start`, VNC and screenshots, happens under the lock:
  ```sh
  ~/Documents/86box/samba/vmlock wait 86box-d1-<TASK> "what you are doing" 1800
  ... short batch of VM work ...
  ~/Documents/86box/samba/vmlock release 86box-d1-<TASK>
  ```
  `<TASK>` is your task ID (e.g. `86box-d1-T03`). Use only `~/Documents/86box/samba/` tools (VM A), never `samba/vmb/`.
- Keep holds short (aim for under 10 minutes; `vmlock touch` if a run is long); release while you edit code.
  Batch your VM work: Mac tests first, then one VM round.
- Before releasing: kill every program you started, close VNC, make sure no dialog or pica window is left open.
- agent98 serves one connection at a time: no parallel `w98` calls.
- If the user holds the lock (play-testing), wait; never use the VM while someone else holds it.

### Finishing a task
1. Your suites pass on the Mac (strict + sanitizers) and on VC6; your objects build at /W3 with zero warnings.
2. Selftest functions you own pass (win tasks); required screenshots taken and checked.
3. Append to `docs/HANDOFF.md`: what you built, interface notes and deviations, gotchas, test commands and results,
   open issues, requested changes to files you don't own.
4. Final report to the overseer: the same in short form.

## Waves

```
Wave 0:  T00 skeleton (all headers, stubs, Makefiles, resources, test harness, app shell)
Wave 1:  T01 buffer | T02 codecs | T03 regex + search | T04 lexers | T05 file I/O | T06 layout, hex format, ini/config/session
Wave 2:  T07 frame shell | T08 text view | T09 files + document commands | T10 hex view | T11 search UI + find in files | T12 edit commands + dialogs
Wave 3:  T13 session, backup, recovery | T14 disk events + large files | T15 selftest suite, shortcuts, help
Wave 4:  T16 core QA | T17 win QA + performance   then   T18 release
```
Milestones for the user: after Wave 2 a usable editor (open, edit, save, hex, search); after Wave 3 the complete 1.0
feature set; after Wave 4 the release zip. The user play-tests whole milestones (no mid-wave interruptions).

| ID | Title | Wave | Depends on | Size |
|---|---|---|---|---|
| T00 | Skeleton: headers, stubs, build, resources, harness, app shell | 0 | - | L |
| T01 | Buffer: piece tree, undo, Document, marks | 1 | T00 | L |
| T02 | Codecs, detection, Unicode tables, winenc | 1 | T00 | L |
| T03 | Regex engine and search engine | 1 | T00 | L |
| T04 | Lexers and language files | 1 | T00 | M |
| T05 | File I/O: FileSys, load, on-demand source, index, safe save, backups, A/B files | 1 | T00 | L |
| T06 | Layout, hex formatting, INI, config, session model | 1 | T00 | M |
| T07 | Frame shell: main loop, frame, tabs, status bar, notify bar, progress, toolbar | 2 | Wave 1 | L |
| T08 | Text view, renderer, typing ops | 2 | Wave 1 | L |
| T09 | Files and document commands: open/save/revert/close, I/O worker, encodings, EOL | 2 | Wave 1 | L |
| T10 | Hex view, hex edit ops, data inspector | 2 | Wave 1 | M |
| T11 | Search UI: Find/Replace dialog, incremental search, find in files + results panel | 2 | Wave 1 | L |
| T12 | Edit commands, clipboard, line ops, Go To, Options, misc dialogs | 2 | Wave 1 | L |
| T13 | Session, backups, crash recovery, exit flow | 3 | Wave 2 | M |
| T14 | Disk events (change detection, reload, locks) and large-file mode | 3 | Wave 2 | M |
| T15 | Selftest suite, key injection, shortcut audit, help text | 3 | Wave 2 | M |
| T16 | Core QA: adversarial tests and fixes | 4 | Wave 3 | M |
| T17 | Win QA, bug bash and performance pass on VM A | 4 | Wave 3 | M |
| T18 | Release: README, help review, release folder and zip | 4b | T16, T17 | S |

---

## T00 Skeleton (Wave 0)

**Owns:** everything it creates (list below). After Wave 0, ownership passes to the tasks named in "Later owner".

**Inputs:** DESIGN 4, 5, 6 (copy the declarations verbatim), 10.2, 15, 17, 18, Appendix B/C.

**Deliverables**
1. Folder layout DESIGN 18.1; `.gitignore` adds `obj/`, `tests/build/`, `tests/tmp/`, `release/*/`, `*.exe`.
2. `res/appinfo.h` (18.3).
3. **All core headers** (DESIGN 6.1-6.20) with the declarations exactly as specified, plus `src/core/edops.h`,
   `docbind.h`, `marks.h`, `fsys.h`. Config struct fields per Appendix C.
4. **Core `.cpp` files**, one per module in DESIGN 4.1 (`etypes emem estr esource ptree undo doc docbind marks codec
   cptables detect ucd ucdtables layout lexer lexgen lexhtml lexmd regex rxvm rxbt rxpre search fif hexfmt edtype
   edlines edhex ini config session faultfs fileops`). Working implementations: `etypes.cpp`, `emem.cpp`,
   `estr.cpp` (complete, with tests), `MemSource`/`ViewSource`, a **minimal codec** (UTF-8 strict decode/encode and
   cp1252 SBCS via a hand-written 0x80-0x9F table; `codec_get` returns these; enc_* helpers), a **minimal detect**
   (BOM only + EOL counting), a **minimal ucd** (ASCII/Latin-1 case and word class, width 1). Everything else:
   stubs returning `E_NOTIMPL` (or empty values) that compile and link.
5. **Win headers** (DESIGN 15.1: `app.h tab.h views.h cmds.h ui.h progress.h docio.h sessmgr.h watch.h largefile.h
   iothread.h fifworker.h selftest.h bench.h winenc.h clip.h render.h notifybar.h dialogs.h`, with every
   cross-module function listed in DESIGN 15.1) and **stub `.cpp` files**:
   `main frame tabs statusbar notifybar progress util cmdtable filecmds editcmds doccmds docio iothread textview
   render hexview clip winenc fs_win32 dlgfind isearch fifpanel fifworker dlggoto dlgopts dlgmisc sessmgr watch
   largefile selftest bench`. Every `cmd_*` handler stub shows "Not implemented yet" in the status bar; every
   `st_*` selftest stub logs `SKIP st_x (not implemented)`.
6. **App shell that works**: WinMain (InitCommonControlsEx, `codec_init`, `enc_set_system(GetACP(), GetOEMCP())`,
   config dir per 13.1 with `-cfg`), frame window with the full menu and accelerators (15.4, 15.5), an empty status
   bar with the 9 parts, `app_command` dispatching through `cmdtable.cpp` (complete table: every IDM in 15.4), Exit
   works, window placement saved. `-selftest` runs `st_frame` stub + a framework self-check, writes
   `<cfg>\selftest.log`, exits with the failure count.
7. **Resources**: `res/resource.h` (all IDM_* in 40000-40999 per 15.4, IDA_MAIN, IDR_MENU, IDB_TOOLBAR, IDI_APP, RCDATA
   ids for the 13 language files and `help.md`, string table ids for toolbar tooltips), `res/pica.rc` (includes
   `menu.rc2`, `dlg_file.rc2`, `dlg_find.rc2`, `dlg_goto.rc2`, `dlg_opts.rc2`, `dlg_misc.rc2`; VERSIONINFO from appinfo.h;
   RCDATA entries for `src/langs/*.lng` and `src/help/help.md`), `res/menu.rc2` (menus + accelerators complete),
   dialog fragments with **empty placeholder dialogs** and their id headers `ids_file.h` (range 2000-2099),
   `ids_find.h` (2100-2299), `ids_opts.h` (2300-2599), `ids_misc.h` (2600-2899; Go To is in misc), placeholder
   `toolbar.bmp` and `pica.ico` from `tools/make_icons.py` (simple shapes are fine), placeholder `.lng` files (one
   line comment each) and `help.md` (title only).
8. **Build files**: root NMAKE `Makefile` (18.2: all, test, `t_<suite>`, `obj\<name>.obj`, release, clean, DEBUG=1;
   per-suite source lists), `tests/Makefile` (17.1: check, `<suite>`, mac, data; per-suite source lists identical in
   content to the NMAKE ones), `tests/tf.h`, `tests/t_main.cpp` (suite table, options), one `tests/t_<suite>.cpp` per
   suite with an empty `suite_<name>(TF*)` (estr/emem tests filled in), `tests/fs_posix.cpp` (**working** POSIX
   FileSys with Win98 semantics where cheap: `move` fails if the target exists, `make_temp` makes `~PCxxxx.TMP`),
   `tests/mac/` with a README. Suites (one `t_<suite>.cpp` each): estr, emem, ptree, undo, doc, marks, codec, detect,
   ucd, layout, lexer, regex, search, fif, hexfmt, edtype, edlines, edhex, ini, config, session, fileops, winenc
   (VC6 only), adversarial.
9. `tools/lint.py`: for-loop declarations (`for\s*\(\s*(const\s+)?(unsigned\s+|signed\s+)?(int|long|char|short|u8|u16|u32|i32|epos|ecp|size_t)\b`),
   banned tokens in core/win/tests (`std::`, `#include <vector>` etc., `long long` outside etypes.h, `snprintf`,
   `nullptr`, `FILE_SHARE_DELETE`, `GetTextExtentExPoint`, `GetCharWidth32`, `CharLowerW`, `CharUpperW`,
   `GetTextMetricsW`, `DrawTextW`, `CreateFileW`, `windows.h` in core), non-ASCII bytes and BOMs in sources, CRLF for
   the CRLF file list. Exit code 1 on findings, prints file:line.

**Later owner map** (the task that owns each file in its wave): core files as listed in T01-T06 and T08-T12; win files
as listed in T07-T15. `Makefile`, `tests/Makefile`, `tests/t_main.cpp`, `tests/tf.h`, `res/pica.rc`,
`res/resource.h`, `src/win/*.h` shared headers: **overseer only** after Wave 0 (requests via HANDOFF), except where a
task below lists them.

**Acceptance**
- `make -C tests` passes on the Mac (estr/emem suites real, others empty) with zero warnings; `python3 tools/lint.py`
  clean.
- `w98 build editor` builds `pica.exe` with zero warnings; `w98 build editor test` builds and runs `coretest.exe`;
  `w98 build editor t_estr` works.
- `pica.exe` starts, shows the window with all menus and the status bar, every menu item routes to its stub (status
  note), Exit works; `pica.exe -selftest -cfg C:\PICATEST` writes the log and exits 0.
- Screenshot of the shell window checked.
- HANDOFF section: the per-suite source lists, the stub conventions, anything in DESIGN 6 that didn't compile as
  written and how it was fixed.

---

## T01 Buffer: piece tree, undo, Document, marks (Wave 1)

**Owns:** `src/core/esource.cpp` (adds SynthSource), `ptree.cpp`, `undo.cpp`, `doc.cpp`, `docbind.cpp`, `marks.cpp`;
`tests/t_ptree.cpp`, `t_undo.cpp`, `t_doc.cpp`, `t_marks.cpp`.

**Inputs:** DESIGN 6.4-6.8, 7, 4.3 (SnapshotSource rules), ARCH.md 1, 6.

**Deliverables**
- PieceTree as a treap with split/merge over a node pool (32-bit indices, free list, deterministic seeded priorities),
  subtree sums of length and breaks, pieces <= 64 KB, unit-2 alignment, AddBuffer with a fixed chunk table, `span`,
  `break_pos`, `breaks_before` in O(log n) + one piece scan, `set_break_spec` recount, `init_uncounted/set_counts`,
  `snapshot`, `check`, `dump`. SnapshotSource.
- UndoLog with groups, merge rule (6.6), state ops (kind 3), save point incl. `set_save_point_seq`, memory accounting.
- Document: everything in 6.7 including EOL counting windows (7.2), char stepping (7.1, DBCS fallback via line start),
  alignment rule (7.3), `replace_text`, `convert`/`check_convert`/`reinterpret`/`convert_eol` (7.5, using the codec
  interface; tests use the minimal codec until T02 lands), `reload_as_edit`, `reset_source`, busy/read-only/indexing
  flags, listeners, `bytes()` adaptor.
- DocLines (docbind), LineMarks and RangeMarks with edit adjustment.

**Acceptance**
- Differential random test vs a flat model (17.1): 200,000 mixed ops (insert/delete/undo/redo/save point) with
  `check()` after every op in the default run; 2,000,000 with `-long`; same for unit-2 documents; line queries vs brute
  force; SynthSource documents of 3.9 GB for offset arithmetic (init + edits near 2^31 and 2^32-2).
- EOL counts exact after random edits in CRLF/LF/mixed/CR documents (compare with a full recount).
- Undo restores bytes and selection; modified flag clears when undoing to the save point; redo-branch discard
  invalidates the save point; async `set_saved_seq`.
- SnapshotSource reads the same bytes as the document while the document keeps being edited (single-threaded
  simulation: snapshot, edit, compare).
- OOM: `e_test_fail_after(k)` for k = 1..N inside `replace` leaves the document unchanged (verified by `check()` and
  content compare).
- Performance smoke (Mac, report numbers): 1,000,000 single-byte inserts at random positions in a 10 MB document.
- VC6: `t_ptree t_undo t_doc t_marks` pass.

---

## T02 Codecs, detection, Unicode tables, winenc (Wave 1)

**Owns:** `src/core/codec.cpp`, `cptables.cpp`, `detect.cpp`, `ucd.cpp`, `ucdtables.cpp`; `src/win/winenc.cpp`;
`tools/gen_cptables.py`, `tools/gen_ucd.py`, `tools/gen_testdata.py`; `tests/t_codec.cpp`, `t_detect.cpp`,
`t_ucd.cpp`, `t_winenc.cpp`, `tests/data/codec/`.

**Inputs:** DESIGN 6.9-6.11, 7.1, 12.1, ARCH.md 3, probe RESULTS 9.

**Deliverables**
- Codecs: strict UTF-8 (ASCII fast path in `codec_decode_buf`), UTF-16 LE/BE with surrogates and bad units, SBCS via
  generated tables (decode table + sorted reverse table, undefined bytes = bad markers), DBCS through
  `DbcsProvider`; `codec_prev`, `CharReader`, `codec_encode_str` with substitution, enc_* helpers, labels and tokens
  (all code pages listed in 6.9 get a label, e.g. "Windows-1251", "OEM 866", "KOI8-R").
- `detect` and `detect_eol` per 6.10 rules.
- ucd: width, simple fold, upper/lower, word, space, drawable (6.11), generated tables.
- winenc (VC6 only): system ACP/OEMCP setup, DBCS provider via `IsDBCSLeadByteEx`/MultiByteToWideChar built eagerly
  at registration (only when `IsValidCodePage`), input-locale code page (`winenc_input_cp()`, refreshed on
  WM_INPUTLANGCHANGE), helpers `winenc_ansi_to_ecp` / `winenc_ecp_to_ansi` for UI strings and paths, CF_TEXT <->
  ecp conversion helpers for clip.cpp.

**Acceptance**
- Every SBCS table round-trips all 256 bytes (decode -> encode == byte, undefined bytes included) and matches Python
  vectors; random byte strings round-trip byte-exact in every codec (100,000 strings).
- UTF-8 invalid corpus (overlongs C0 AF / E0 80 AF, surrogates ED A0 80, > U+10FFFF, truncated sequences, lone
  continuation bytes) yields the expected bad-marker sequences; `codec_prev` agrees with forward decoding at every
  position of random buffers.
- DBCS logic tested on the Mac with a test provider built from Python cp932 vectors.
- Detection table >= 40 cases (BOMs, UTF-32, BOM-less UTF-16 LE/BE, binary, pure ASCII, valid/invalid UTF-8, truncated
  at sample end, CR-only, mixed EOL).
- ucd spot checks (fold of 'A', 'Ä', 'Ж', 'ẞ'; width of U+4E2D = 2, U+0301 = 0).
- VC6: suites pass; `t_winenc` checks the ACP is 1252, round-trips ANSI strings and that 932 is reported unsupported on
  VM A.

---

## T03 Regex engine and search engine (Wave 1)

**Owns:** `src/core/regex.cpp`, `rxvm.cpp`, `rxbt.cpp`, `rxpre.cpp`, `search.cpp`; `tests/t_regex.cpp`,
`t_search.cpp`, `tests/mac/rxfuzz.cpp`, `tools/gen_rxcases.py` (optional).

**Inputs:** DESIGN 6.14, 6.15, 11.1-11.3, ARCH.md 8, Russ Cox's articles (cited there).

**Deliverables**
- Parser -> program compiler (11.1 syntax, error codes with positions, limits), Pike VM with captures and
  leftmost-first priority over `ByteSource` + `Codec` (decoding with `CharReader`), backtracker for backrefs and
  lookahead over a decoded window with a step budget, literal-prefix extraction and Horspool prefilter (exact and
  ASCII case-folded), case folding via `uc_fold` (ASCII-only until T02 lands; write the tests for non-ASCII folding
  and mark them as needing T02 in HANDOFF if they fail with the minimal ucd).
- Searcher: Normal / Extended (`search_parse_extended`) / Regex / Hex bytes modes, whole word, find forward/backward
  with wrap handled by the caller, count, find_all (capped), replacement expansion (11.2) with case operators,
  `search_replace_all` back to front through `EditSink`, empty-match handling, `impossible()` for unencodable literals,
  progress/cancel.

**Acceptance**
- >= 300 table cases (pattern, flags, text, expected span + groups or error code/position), including anchors at
  CRLF/LF/CR, `\b` with non-ASCII letters, lazy quantifiers, alternation priority, `{n,m}`, classes, POSIX classes,
  inline flags, `\R`, backreferences, lookahead, unsupported constructs giving clear errors.
- Pathological patterns finish in < 1 s on 100 KB (Pike) or return E_BUDGET (backtracker).
- Search over a multi-span ByteSource (pieces of random sizes, including 1-byte pieces splitting multi-byte chars)
  finds the same matches as over a flat buffer; in UTF-8, UTF-16 LE/BE and cp1252.
- Backward search == the last forward match before the position, on random texts.
- Replace all: results equal a naive reference implementation on random inputs, incl. empty matches and `$0`/`\U`.
- `make -C tests mac` runs `rxfuzz` (10,000 generated cases vs std::regex ECMAScript, shared subset) with no
  mismatches.
- Throughput on the Mac reported; on VC6 `t_regex t_search` pass.

---

## T04 Lexers and language files (Wave 1)

**Owns:** `src/core/lexer.cpp`, `lexgen.cpp`, `lexhtml.cpp`, `lexmd.cpp`; `src/langs/*.lng` (13 files);
`tests/t_lexer.cpp`.

**Inputs:** DESIGN 6.13, 8.5, Appendix B, ARCH.md 7, UX.md 8.5.

**Deliverables**
- Highlighter: per-line u16 start states in a gap array, `valid_upto`, `lines_changed` invalidation, re-lex until the
  computed state equals the stored one (convergence), idle `work`, `take_dirty`, LEX_LINE_MAX cap.
- Registry: `lang_init` from built-in sources + overrides, `lang_for_file` (extensions, whole names such as
  `makefile`, case-insensitive), `lang_results` (results-panel lexer: header line, file lines, `NNN:` line numbers).
- Generic data-driven lexer (`lexgen`): the `.lng` format from ARCH 7.2 extended as needed (document it at the top of
  `src/langs/cpp.lng` and in HANDOFF): up to 4 keyword classes, line comment(s), block comment pairs (nesting flag),
  string delimiters with escape char, multi-line strings (Python `"""`, JS template strings), preprocessor at line
  start, number formats (c, asm `0FFh`, basic `&H`), line-start rules (INI `[section]`, `key=`; BAT `:label`, `rem`,
  `::`; Makefile `target:`, `!IF`, `$(VAR)`; diff `+ - @@ ---/+++`), case-insensitive keywords, `tab_width`,
  `use_tabs` (Makefile: tabs), `braces`, comment tokens for Ctrl+Q.
- Hand-written HTML/XML lexer (tags, attributes, values, entities, comments, CDATA, `<?xml ?>`, doctype; `<script>`
  and `<style>` switch to the JS and CSS generic definitions with the sub-language in the high state bits) and
  Markdown lexer (headings, lists, code fences with state, inline code, emphasis, links, block quotes).
- The 13 language files: cpp (also .rc via rc.lng), rc, make, bat, ini (.ini .inf .reg .cfg .dsw), asm, python, js,
  css, diff + html, xml, md metadata files.

**Acceptance**
- Golden style tests per language (>= 10 lines each, covering every rule) compare style arrays.
- Convergence: for every language, random edits to sample files then incremental re-lex == full re-lex (states and
  styles), 2,000 edits per language.
- Typing `/*` at the top of a 10,000-line C file re-lexes only to the viewport target in `work` slices; closing it
  restores the states.
- Lines longer than LEX_LINE_MAX are styled up to the cap without overflow (ASan).
- Malformed override files produce an error string and are skipped, never crash.
- VC6: `t_lexer` passes.

---

## T05 File I/O (Wave 1)

**Owns:** `src/core/fileops.cpp`, `faultfs.cpp`; `src/win/fs_win32.cpp`; `tests/fs_posix.cpp`; `tests/t_fileops.cpp`.

**Inputs:** DESIGN 6.20, 13.2-13.7, 14.1, probe RESULTS 7 and 9 (file semantics).

**Deliverables**
- `fs_win32.cpp`: FileSys over the ANSI Win32 API (CreateFileA with share modes per 6.20 and never
  FILE_SHARE_DELETE; `read_at` with `SetFilePointer(lo, &hi)` and GetLastError checks; GetTempFileNameA;
  GetDiskFreeSpaceExA via GetProcAddress with GetDiskFreeSpace fallback; GetFileAttributesExA for `stat`;
  FindFirstFileA; error mapping to E_*; `is_remote` via GetDriveTypeA).
- `fs_posix.cpp` refined to the same semantics for Mac tests.
- FaultFs.
- `file_load` (in-memory read in 64 KB chunks into `e_valloc`, progress, share read+write; on-demand open with
  deny-write and fallback; detection via `detect`), FileSource (LRU block cache, `close/reopen`), `file_index` (own
  handle, per-block break counts, EOL counts, cancel), `file_write_temp` / `file_replace` / `file_save` (13.3 steps
  incl. in-place fallback and rollback, attributes and creation time), `file_read_all`, `file_write_direct`,
  `ab_read`/`ab_write`, backup header write/parse, `backup_write` (generation scheme), `backup_list`, `backup_open`,
  `backup_delete_tab`, `backup_file_name`.

**Acceptance**
- Fault injection: for every FileSys call in the save sequence, failing it (each error kind) leaves the target either
  byte-identical to the old content or to the new content, temp files are reported or removed, rollback works; tested
  for new files, existing files, read-only targets, "locked" targets (FaultFs returns E_SHARING on move).
- Torn backups: truncating the newest generation at many offsets makes `backup_list` return the previous generation;
  A/B: damaging the newer file makes `ab_read` return the older one.
- FileSource: random reads vs file contents with cache sizes 1, 2, 64; reopen keeps contents.
- `file_index` block counts equal a brute-force count; cancel works.
- VC6 `t_fileops` passes against `C:\PICATEST\tmp` **and** `M:\dev\editor\tests\tmp`; report timings (a 10 MB save on
  C: and a 1 MB save on M:). Verify on VM A that MoveFile of a file held open fails as the probe says and that the
  save sequence copes (on-demand hooks close the handle).

---

## T06 Layout, hex formatting, INI, config, session model (Wave 1)

**Owns:** `src/core/layout.cpp`, `hexfmt.cpp`, `ini.cpp`, `config.cpp`, `session.cpp`; `tests/t_layout.cpp`,
`t_hexfmt.cpp`, `t_ini.cpp`, `t_config.cpp`, `t_session.cpp`.

**Inputs:** DESIGN 6.12, 6.17, 6.19, 8.1, 9.1, 13.6, 14.2, Appendix C.

**Deliverables**
- LineLayout (tabs, control pictures with names, bad bytes/units, wide/zero-width/non-drawable, show-whitespace
  glyphs, hit testing both ways, `wrap_rows` word and char modes), LongLineIndex (checkpoints, progress).
- hexfmt: offset/byte text, char glyphs per SBCS codec, `hex_parse_bytes`, `hex_parse_pattern`, `goto_parse_offset`,
  `goto_parse_line` (all syntax in 6.17 with error texts), `inspect_bytes` (int8..uint64 LE/BE, float, double, binary;
  own 64-bit decimal formatting; floats via sprintf("%.9g"/"%.17g")).
- Ini (tolerant parse, order and comments kept, CRLF write), Config defaults/load/store/MRU/history (Appendix C),
  Session to/from INI with the `[end] ok=1` sentinel and `session_validate`.

**Acceptance**
- Layout: tables for tabs at every column, each control char, bad bytes in UTF-8, UTF-16 bad units, wide chars,
  combining marks; `cell_of_offset`/`offset_of_cell` inverse on random lines; wrap rows never split wide chars or
  combining sequences and break at spaces when possible; LongLineIndex agrees with a full layout on 1 MB lines.
- goto parsers: >= 40 cases incl. errors and range limits; inspect_bytes against known values (incl. NaN/Inf, negative
  zero, 0x8000000000000000).
- Ini: round trip preserves unknown lines and comments; garbage input never fails; config defaults equal Appendix C;
  out-of-range values clamp.
- Session: round trip of a session with 3 tabs (all fields, bookmarks); a truncated session text fails validation.
- VC6 suites pass.

---

## T07 Frame shell (Wave 2)

**Owns:** `src/win/main.cpp`, `frame.cpp`, `tabs.cpp`, `statusbar.cpp`, `notifybar.cpp`, `progress.cpp`,
`util.cpp`; `res/menu.rc2` (fixes only), `res/toolbar.bmp`, `res/pica.ico`, `tools/make_icons.py`.

**Inputs:** DESIGN 4.3, 15 (all), 10.3 (fonts are T08's), Appendix B, UX.md 9.

**Deliverables**
- WinMain: command line (15.7), single instance via WM_COPYDATA, config load/save (`pica.ini` via `file_save`),
  `lang_init` with RCDATA + `langs\` overrides, message loop with `MsgWaitForMultipleObjects` (watch handles from
  `watch_handles()`), accelerator/dialog routing order (15.5), idle work dispatch (15.11), timers, SEH guard calling
  `sessmgr_emergency()` (15.12), WM_DROPFILES.
- Frame: child layout (15.2), toolbar (15.6) with real icons (16 colours, generated), tab bar owner-drawn with close
  box, middle-click close, drag reorder, tooltips, context menu commands (`IDM_TAB_*`), status bar parts and click
  targets (15.3) with popup menus built from the Document submenus, title updates, WM_INITMENUPOPUP state via
  `cmdstate_*`, Window menu list, MRU list (commands in T09), splitter for the results panel host.
- `tabs.cpp`: Tab lifecycle (`tab_new/tab_free` with listener wiring: DocLines + Highlighter, marks), activation
  (attach the right view, save/restore view state), `app_*` functions from app.h.
- Notification bar widget (per tab, buttons, callback), progress (`UiProgress` modal + status-bar progress), `ui_ask`
  with the selftest answer queue, `ui_error`, path helpers.
- Window menu / tab commands handlers: `cmd_win_*`, `cmd_tab_*`, `cmd_view_toolbar/statusbar/results`.
- `st_frame`: create/activate/close tabs, reorder, status bar texts, notification bar callback, every IDM routes.

**Acceptance**
- Selftest `st_frame` passes. Screenshots: main window with 3 tabs (one modified), notification bar visible, status
  bar with all parts; menus open (VNC). Tab drag reorder and middle-click close work over VNC.
- Second `pica.exe somefile` hands the file to the first instance.
- Window placement and toolbar/status visibility persist across restarts.
- Zero warnings; `lint.py` clean.

---

## T08 Text view, renderer, typing ops (Wave 2)

**Owns:** `src/win/textview.cpp`, `render.cpp`; `src/core/edtype.cpp`; `tests/t_edtype.cpp` (suite `edtype`).

**Inputs:** DESIGN 6.12, 6.13, 6.18 (edtype part), 7, 8, 10, 14.2, probe RESULTS 1-6, 10.

**Deliverables**
- render.cpp (10.1-10.4): fonts from Config (ChooseFont-compatible LOGFONT), bold check, CJK font discovery,
  placeholder drawing, run drawing with state caching, colour resolution (Appendix B, system defaults).
- TextView window class (instances: main view and results panel): attach/detach Tab, layout per 8.1 (gutter,
  numbers, marker cell, wrap anchor and rows, scrollbars with 32767 scaling, lazy horizontal range), painting per 8.5
  with Highlighter styles, selection, RangeMarks, brace match, show whitespace/EOL, long-line windows; caret (8.2),
  keyboard (8.3) incl. AltGr, DBCS lead-byte buffering, WM_IME_CHAR, IME window placement; mouse (8.4) incl.
  triple-click, gutter clicks, drag autoscroll, wheel; context menu; invalidation via DocListener; ScrollWindowEx
  scrolling; large-mode gates (8.6); indexing placeholder rendering (14.1); WM_APP_CARET notifications;
  `textview_*` API (15.1). View commands `cmd_view_wrap/showws/linenum`.
- edtype.cpp (core): `ed_type`, `ed_newline` (auto-indent), `ed_backspace`, `ed_delete`, `ed_delete_word`,
  `ed_insert_text`, `ed_insert_bytes`, word movement, `ed_word_at`, `ed_smart_home`, `ed_lang_from`, with Mac tests in
  `suite_edtype` (golden tables in ANSI/UTF-8/UTF-16 LE, CRLF/LF).
- `st_textview`: type text, move the caret, select, undo, wrap toggle, show whitespace, check caret info and document
  bytes.

**Acceptance**
- Mac `make -C tests edtype` passes.
- `st_textview` passes. Screenshots: C file coloured (Courier New 10), same with wrap, show whitespace with CRLF/LF/lone
  CR, a UTF-8 file with Cyrillic + Polish + an invalid byte + a CJK char (placeholder), Fixedsys font.
- Timing without VNC (temporary `-bench`-like measurement printed to the log, or T17 later): full repaint <= 12 ms,
  keystroke <= 3 ms on VM A. Report numbers.
- 5 MB single-line file scrolls horizontally and End works without freezing (> 0.5 s shows progress).

---

## T09 Files and document commands (Wave 2)

**Owns:** `src/win/docio.cpp`, `filecmds.cpp`, `doccmds.cpp`, `iothread.cpp`; `res/dlg_file.rc2`, `res/ids_file.h`.

**Inputs:** DESIGN 4.2, 4.3, 7.5, 12, 13.2-13.4, 15.4 (File, Document menus), 15.9 (Save As template), 16.

**Deliverables**
- I/O worker thread (iothread.cpp): FIFO of jobs with ids, cancel per tab, completion via WM_APP_IODONE; job kinds:
  SAVE_TEMP (T09), BACKUP, SESSION, DELETE (filled in by T13: provide the job struct fields and dispatch switch with
  T13's kinds stubbed).
- docio: open (13.2 incl. tab reuse, encoding overrides, `.nfo` rule, binary -> hex view with bar, read-only bar,
  on-demand path calling `largefile_begin(Tab*)` (stub until T14: synchronous indexing with UiProgress)), new
  (untitled numbering), save via the I/O worker with snapshot + `file_replace` on completion (13.3), `docio_save_wait`,
  Save As (path change, language re-detect, MRU), reload/revert (13.4), close with prompts (calls
  `sessmgr_tab_closed` hook, stub until T13).
- File menu commands (open dialog multi-select with filters, Save As dialog with the Encoding and Line endings combos via
  a hook template, Save All, Close/All/Others, Revert, MRU open/clear, Exit -> `sessmgr_exit()` stub that falls back to
  per-file prompts).
- Document menu commands: Syntax (auto/plain/language, forced flag), Encoding reload-as (incl. code page submenu) and
  convert with the loss prompt (`dlg_convert_loss`, T12; until then `ui_ask`), Line endings, Tabs, Read-only,
  Properties (calls `dlg_properties`, T12). `cmdstate_*` for all of them (radio marks, disabled for on-demand
  convert).
- Unrepresentable typing/paste bar actions "Convert to UTF-8" (12.4) as a function other modules call:
  `doccmd_offer_utf8(Tab*, const char* msg)`.
- `st_docio`: open every fixture kind and check detected encoding/EOL/view; save/reopen byte-exact; Save As with
  conversion; reload-as; revert; save to `-fixdir` on M:; save failure with a read-only target (message via
  `ui_ask`).

**Acceptance**
- `st_docio` passes on C: and on M: fixtures. A 2 MB save to M: does not freeze the UI (other tab scrolls while saving;
  check over VNC) and the tab shows `(saving)`.
- Saved bytes are exactly the document bytes for all fixture kinds (compare with `st_file_bytes`).
- Screenshots: Save As dialog with the two combos; Encoding menu; convert-loss prompt.

---

## T10 Hex view, hex edit ops, data inspector (Wave 2)

**Owns:** `src/win/hexview.cpp`; `src/core/edhex.cpp`; `tests/t_edhex.cpp` (suite `edhex`).

**Inputs:** DESIGN 6.17, 6.18 (edhex), 7.3, 9 (all), 10, 15.3.

**Deliverables**
- edhex.cpp: `hx_type_nibble` (overwrite/insert, UTF-16 unit handling, append at end), `hx_type_char`, `hx_write`
  (paste semantics), `hx_delete` (Del/Backspace rules, BOM protection, units), undo grouping UK_HEXTYPE. Mac tests with
  golden tables in SBCS, UTF-8 and UTF-16 LE/BE documents.
- HexView window: layout and ruler (9.1), colours (modified via `PSpan.src`, BOM, selection, mirror byte), caret and
  keys (9.2), mouse, wheel, context menu, inspector strip with LE/BE toggle, scroll scaling for files > 32767 rows,
  ScrollWindowEx, indexing/read-only states, status bar info (9.3), view switching and sync with the text view (9.5,
  `cmd_view_hex`, `cmd_view_inspector`), copy/paste in hex panes (9.6, using `clip_*` from T12's clip.h API: until it
  lands, call through the declared functions; they are stubs).
- `st_hexview`: toggle views with selection mapping in a UTF-8 file; nibble typing in overwrite and insert; Del and
  Backspace rules; UTF-16 unit rules; BOM protection; undo across views; inspector values for known bytes.

**Acceptance**
- Mac `make -C tests edhex` passes; `st_hexview` passes.
- Screenshots: hex view of a binary file with modified bytes (red), selection, inspector visible; insert mode caret and
  `INS` in the status bar; an OEM `.nfo` file showing CP437 graphics in the char column.
- Full hex repaint <= 12 ms without VNC (report).

---

## T11 Search UI and find in files (Wave 2)

**Owns:** `src/win/dlgfind.cpp`, `isearch.cpp`, `fifpanel.cpp`, `fifworker.cpp`; `src/core/fif.cpp`;
`tests/t_fif.cpp`; `res/dlg_find.rc2`, `res/ids_find.h`.

**Inputs:** DESIGN 6.15, 6.16, 11 (all), 15.5 (dialog accelerators), probe RESULTS 7 (M: costs).

**Deliverables**
- core fif.cpp: mask matching (`;`/space separated, `!` excludes), recursive walk over FileSys (skip hidden/system
  folders), per-file read (whole <= 16 MB, windows above), detection, binary skip, line-based hits with UTF-8 text and
  highlight positions, max hits, cancel, progress. Mac tests over a generated fixture tree (`tests/tmp/fif`).
- Find/Replace dialog (11.4): modeless, three tabs, history combos (Config), regex token menu, live validation with
  red status text, Find Next / Count / Find All / Mark All / Replace / Replace All / Replace All in Open Tabs, In
  selection auto-tick, direction, wrap, Hex bytes mode in hex view, auto-move away from the match, position memory,
  F3 handling while focused. Search commands: `cmd_search_*` (find, replace, next, prev, selnext, selprev, fif,
  resnext, resprev, clearmarks, isearch). Progress for long searches; Replace All as one undo group via an EditSink
  over `Document::replace`.
- Incremental search bar (11.5).
- FiF worker thread + results panel (11.6): second TextView instance (panel mode) on a read-only UTF-8 Document with
  `lang_results()`, RangeMarks for hit spans, header progress, Stop, double-click/Enter opens and selects, F4 /
  Shift+F4, Ctrl+C, Esc, splitter (frame hosts it, T07).
- `st_search`: find/replace/regex error/count/mark all/replace all undo/in selection; `st_fif`: fixture tree on C: and
  `-fixdir` M:, hit counts, opening a hit selects the match, cancel.

**Acceptance**
- Mac `make -C tests fif` passes; `st_search` and `st_fif` pass.
- Find in files over `M:\dev\editor\src` completes with progress and can be cancelled; UI stays responsive (VNC check).
- Screenshots: dialog on each tab, regex error in red, results panel with hits, incremental search bar (found and
  not-found pink).

---

## T12 Edit commands, clipboard, line ops, dialogs (Wave 2)

**Owns:** `src/win/editcmds.cpp`, `clip.cpp`, `dlggoto.cpp`, `dlgopts.cpp`, `dlgmisc.cpp`; `src/core/edlines.cpp`;
`tests/t_edlines.cpp` (T00 creates it as suite `edlines`); `res/dlg_goto.rc2`, `res/dlg_opts.rc2`,
`res/dlg_misc.rc2`, `res/ids_opts.h`, `res/ids_misc.h`.

**Inputs:** DESIGN 6.18 (edlines), 8.3, 9.6, 11, 12.2, 12.4, 15.4 (Edit menu, Search > Go To/Brace/Bookmarks, Tools),
15.8, 15.9.

**Deliverables**
- edlines.cpp: duplicate, delete lines, move lines (incl. last line without EOL), toggle comment (line/block, mixed
  selections, indentation-aware), change case (via `uc_upper/uc_lower`, bad markers untouched), indent/outdent,
  `ed_match_brace` (skips strings/comments via Highlighter styles). Mac golden tests.
- clip.cpp: copy/cut/paste per DESIGN ARCH 3.5 and 9.6: `Pica.RawBytes` private format, CF_UNICODETEXT + CF_TEXT on
  copy, paste preference, 16 MB cap; API `clip_copy_text(Tab*, Selection)`, `clip_copy_hex(Tab*, Selection, int
  hex_pane)`, `clip_get(ClipData*)`.
- Edit commands (`cmd_edit_*`, `cmd_line_*`, `cmd_bm_*`, `cmd_search_goto`, `cmd_search_brace`): work in the text
  view; in the hex view undo/redo/cut/copy/paste/delete/select all apply, line commands are disabled
  (`cmdstate_*`). Unrepresentable paste -> `doccmd_offer_utf8` (T09).
- Go To dialog (text: line[:col]; hex: offset; live preview; errors in red).
- Options property sheet (15.8, all 5 pages, explanations, ChooseFont/ChooseColor, Apply/OK -> `app_apply_config`).
- Misc dialogs: exit/close-many prompt (`dlg_exit_prompt`, 15.9), convert loss prompt (`dlg_convert_loss`),
  Properties (`dlg_properties`), About.
- `st_edit`: every line command on CRLF/LF/UTF-16 fixtures; clipboard round trips (RawBytes between documents, CF_TEXT
  from outside via SetClipboardData in the test, NUL bytes survive); bookmarks; brace match; `st_dialogs`: options
  change + apply, go to parsing, exit prompt answers via `st_answer`.

**Acceptance**
- Mac `make -C tests edlines` passes; `st_edit` and `st_dialogs` pass.
- Screenshots: each Options page, Go To dialog (text and hex mode), exit prompt, About.

---

## T13 Session, backups, crash recovery, exit flow (Wave 3)

**Owns:** `src/win/sessmgr.cpp`, `iothread.cpp` (adds BACKUP/SESSION/DELETE jobs).

**Inputs:** DESIGN 4.3, 13.4-13.7, 15.9 (exit prompt), 15.12, 16.

**Deliverables**
- Backup scheduling (13.5: idle debounce, 30 s max, on deactivate, on exit), snapshot jobs, generation handling,
  deleting backups after save/close, "not backed up" note for on-demand tabs.
- Session writing (13.6: debounce, 60 s, after save, exit; A/B files) and startup restore + recovery (13.7) incl.
  orphan backups, changed-on-disk-after-backup bar, missing-file note, `clean_exit`, `-nosession`,
  `restore_session` off.
- Exit flow: `sessmgr_exit()` with `dlg_exit_prompt` (Save Selected via `docio_save_wait`, Keep for Next Session,
  Discard, Cancel), untitled handling, flush of the I/O queue with a 10 s timeout and a "Still writing backups..."
  progress; `sessmgr_tab_closed`; `sessmgr_emergency()` synchronous backups + session for the SEH guard.
- `st_session`: write session, re-run restore in-process (`sessmgr_restore_from(dir)` into fresh tabs) and compare tab
  state; backups restore unsaved content byte-exact; torn newest backup (FaultFs on the worker) falls back to the
  previous generation; orphan backup recovered; exit prompt paths.

**Acceptance**
- `st_session` passes. Manual test: type in an untitled tab and a named file, kill pica.exe with `w98 kill` after 10 s,
  restart: both tabs come back with their unsaved text and the "did not close normally" note.
- Exit with 2 modified named files shows one prompt; Keep for Next Session restores them next start.

---

## T14 Disk events and large-file mode (Wave 3)

**Owns:** `src/win/watch.cpp`, `largefile.cpp`, `docio.cpp` (Wave 3 owner, for on-demand open/save hooks and reload
changes).

**Inputs:** DESIGN 13.2, 13.4, 13.8, 13.9, 14, 16, probe RESULTS 9 (notifications, locks).

**Deliverables**
- watch.cpp: polling timer + activation + tab activation checks with GetFileAttributesExA, change notifications for up
  to 16 folders (`watch_handles`/`watch_signalled` for the main loop), stability re-check, silent reload vs bar
  (13.9), missing/reappearing files, own-save suppression, queueing while inactive or modal.
- largefile.cpp: indexer thread for on-demand documents (progress in the tab label/status bar and the indexing bar with
  Cancel, `finish_indexing` on completion), share-write fallback read-only mode, on-demand save hooks (close/reopen
  FileSource, old copy deletion on close), the one-time "not backed up" note hook for T13, large-mode flags (13.8).
- docio.cpp changes: async on-demand open, reload for on-demand documents (`reset_source`), locked-file retry prompt,
  "Edit anyway" for read-only files, out-of-memory -> offer on-demand.
- `st_watch`: fixture rewritten by the test (size change, same-size change) -> silent reload of an unmodified tab,
  bar on a modified tab, Keep my version, delete -> missing bar, recreate -> changed; `st_largefile`: 20 MB fixture
  opens on demand, hex usable during indexing, edits blocked until indexed, save + reopen byte-exact, cancel closes.

**Acceptance**
- `st_watch` and `st_largefile` pass. Manual: edit a file **on the Mac side** (`echo >> Shared/dev/editor/tests/tmp/x.txt`)
  while it's open in Pica on M:: reload/bar within about 3 s.
- A 100 MB SynthSource file on C: (written by the selftest) shows its first screen and a usable hex view within 1 s
  and finishes indexing within the budget (report numbers without VNC).

---

## T15 Selftest suite, shortcuts, help (Wave 3)

**Owns:** `src/win/selftest.cpp`, `cmdtable.cpp`, `res/menu.rc2`, `src/help/help.md`.

**Inputs:** DESIGN 15.4, 15.5, 17.3, UX.md 7.

**Deliverables**
- Complete StCtx helpers (17.3): `st_keys` with faked keyboard state through the real message loop, `st_wait_idle`,
  fixtures on C: and `-fixdir`, answer queue, logging.
- `st_scenarios`: the cross-module scenarios required by 17.3 (open/edit/save/undo/hex/search/session cycles driven
  by keys), plus an **accelerator audit**: every key in 15.5 sent through `st_keys` reaches its command (a command
  trace hook in `app_command`, provided via cmdtable.cpp), and no AltGr combination is swallowed (type `@ { [ ] } \ |
  ~ €` with Ctrl+Alt held on the Finnish layout if available; else US + simulated AltGr messages).
- Menu/accelerator fixes found by the audit (menu.rc2, cmdtable.cpp).
- `help.md`: Keyboard Shortcuts (table from 15.5 + view keys + hex keys), Regular Expressions cheat sheet (11.1-11.2),
  Encodings and line endings explained (12), Files and backups (where settings/backups live), Large files; Help menu
  commands open it as a read-only tab and jump to the section (`cmd_help_keys`, `cmd_help_regex`, `cmd_help_about`
  calls T12's About).

**Acceptance**
- `pica.exe -selftest -cfg C:\PICATEST` runs **all** module selftests + scenarios with 0 failures (overseer reruns it
  after the wave).
- Screenshot of the help tab with Markdown colouring.

---

## T16 Core QA (Wave 4)

**Owns:** all of `src/core/*`, `tests/t_adversarial.cpp` (T00 creates it as suite `adversarial`), `tests/mac/*`,
`tests/data/*`, other `tests/t_*.cpp` (fixes only).

**Inputs:** everything; HANDOFF open issues.

**Deliverables**
- Adversarial suite: huge documents (SynthSource 3.9 GB edits at the ends), binary data through every codec and the
  layout, 5 MB lines (layout, LongLineIndex, search, lexers), mixed EOL with random lone CRs and conversions,
  invalid UTF-8 everywhere (search, case change, convert, line ops), UTF-16 odd tail, undo stress (1,000,000 random
  edits interleaved with undo/redo and save points, memory checked), crash-safe save fault matrix rerun, backups and
  session under every truncation, regex fuzz (Mac), lexer fuzz (random bytes never crash, convergence holds).
- Fix every bug found in core; each fix gets a regression test.
- Profile hot paths on the Mac (line queries, decode, Horspool, Pike VM, layout) and fix anything far from the 3.2
  budgets scaled to the VM.

**Acceptance**
- `make -C tests` and `make -C tests mac` pass; `nmake test` on VC6 passes with the same check counts; `-long` runs
  pass on the Mac.
- HANDOFF lists every bug fixed with its test name.

---

## T17 Win QA and performance (Wave 4)

**Owns:** all of `src/win/*`, `res/*` (fixes only), `src/win/bench.cpp`.

**Inputs:** everything; HANDOFF open issues; DESIGN 3.2, 17.4, 17.5.

**Deliverables**
- `-bench` (17.5) implemented; run on VM A **without VNC**; fix anything OVER budget.
- Bug bash over VNC with a checklist (write it into HANDOFF): every menu item, every dialog, tabs with 30 files,
  binary/huge/long-line/mixed-EOL/invalid-UTF-8/UTF-16 fixtures in both views, Samba M: open/save/find in files,
  external Mac edits, read-only and locked files, kill during save and during backup (restart recovers), low memory
  (open several 15 MB files), 800x600 layout at the smallest window size, system colour scheme change (e.g. "High
  Contrast Black") repaints correctly, Fixedsys and Lucida Console fonts.
- Fix bugs in win code; GDI handle check (no growth after opening/closing 50 tabs: `GetGuiResources` is not on 98, so
  count our own created objects in render.cpp and log them).

**Acceptance**
- `bench.txt` all OK (attach the numbers to HANDOFF); full selftest passes; checklist complete with screenshots of
  the key states.

---

## T18 Release (Wave 4b, after T16 and T17)

**Owns:** `README.md`, `src/help/help.md` (final review), `tools/mkzip.sh`, `Makefile` (release target only),
`release/`.

**Deliverables**
- `README.md` (GitHub-style: what Pica is, features, screenshots from `docs/shots/` (create), install (`C:\PICA`),
  command line, where settings/backups live, known limits (DESIGN 1.4, 14), building with VC6 and testing on the Mac,
  "all our own code" credits).
- `README.TXT` (CRLF, plain text) for the zip; `nmake release` produces `release\Pica-1.0\` (pica.exe, README.TXT);
  `tools/mkzip.sh` makes `release/pica-1.0.zip`.
- Final clean rebuild on VM A, full selftest, coretest; smoke test of the zip contents in a fresh folder on C: (copy,
  run, settings created next to the exe).

**Acceptance**
- Zip exists, runs from `C:\PICA` on VM A, help opens, selftest passes from the release exe.
- STATUS.md update text proposed in HANDOFF (the overseer edits STATUS.md).

---

## Notes for the overseer
- After Wave 0, check that the headers match DESIGN 6 and that per-suite builds work on both compilers before starting
  Wave 1.
- Between waves: full `make -C tests`, `nmake`, `nmake test`, `pica -selftest`, apply the cross-file requests listed in
  HANDOFF.md, commit.
- The three edit-op test files `tests/t_edtype.cpp` (T08), `tests/t_edlines.cpp` (T12), `tests/t_edhex.cpp` (T10) and
  `tests/t_adversarial.cpp` (T16) are created by T00, so no two tasks share a test file.
- If a Wave 2 task finishes early, don't let it touch other tasks' files; give it review work instead (read-only
  review of another module with findings in HANDOFF).
