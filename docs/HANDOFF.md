# Pica handoff notes

Collected from every finished task, for the tasks that come later. `docs/DESIGN.md` is the spec and `docs/TASKS.md`
the plan; this file records what was actually built and learned while building.

**How to add your notes:** when your task is done, append one section at the end of this file, in a single edit.
Never change other sections (several agents append in the same wave). Use the template below. Keep it factual and
short: later agents read every section.

```
## Txx <title> (wave N, agent 86box-d1-Txx, date)
### Built
- files and what they do now (one line each)
### Interface notes
- how to use what you built; non-obvious behaviour; performance numbers
### Interface changes / deviations from DESIGN
- anything that differs from DESIGN 6 / 15.1 and why (or "none")
### Requests for files I don't own
- file, exact change, why (the overseer applies these between waves)
### Gotchas
- VC6, Win98, clang or test-harness surprises worth knowing
### Tests
- commands run and results (Mac suites, VC6 suites/objects, selftests, screenshots taken)
### Open issues
- known bugs, unfinished parts, ideas for later waves
```

---

## Probe (phase 1): probe/RESULTS.md
Direct ExtTextOutW per colour run with ETO_OPAQUE: 4-5 ms per full 34-row repaint, 0.4 ms per keystroke; compatible
bitmap +3 ms; DIB sections 20+ ms (never). ScrollWindowEx for line scrolls. Win98 stubs: GetCharABCWidthsW,
CharLowerW/UpperW, GetCharWidth32*, GetTextMetricsW, GetTextExtentExPointW, DrawTextW, CompareStringW, IsTextUnicode,
CreateFileW. Never call GetTextExtentExPoint (50 ms per 1000 chars). ExtTextOutW + Courier New draws Cyrillic and
Polish with any charset. M: share: 1.1 MB/s read, 0.4 MB/s write, no change notifications for Mac-side edits (poll).
MoveFile/DeleteFile fail on files anyone holds open; FILE_SHARE_DELETE is an invalid parameter on 9x. A UTF-8 BOM
breaks cl, rc, nmake and batch files. In-memory threshold 16 MB (50 MB paged heavily); malloc caps at 128 MB, use
VirtualAlloc for big blocks.


## T00 Skeleton (wave 0, agent 86box-d1-T00, 2026-11-09)
### Built
- Folder layout 18.1, `.gitignore` (obj/, tests/build/, tests/tmp/, release/*/, *.exe, ...).
- `res/`: `appinfo.h`, `resource.h` (all IDM_* 40000-40999, IDR/IDA/IDB/IDI, RCDATA ids), `pica.rc` (VERSIONINFO from
  appinfo.h, RCDATA for the 13 `.lng` files + `help.md`, tooltip string table), `menu.rc2` (full menu 15.4,
  `IDR_TABMENU`, `IDA_MAIN` 48 accelerators per 15.5, `IDA_FIND`), `dlg_*.rc2` placeholder dialogs + `ids_*.h`,
  `toolbar.bmp` (15 x 16x16, 4 bpp, magenta key) and `pica.ico` (32 + 16) from `tools/make_icons.py`.
- `src/core`: all headers of DESIGN 6 (+ `edops.h docbind.h marks.h fsys.h`), 34 `.cpp` files. Working: `etypes`,
  `emem` (VirtualAlloc declared by hand, no windows.h), `estr`, `MemSource`/`ViewSource`, minimal `codec` (strict UTF-8,
  cp1252, enc_* helpers, codec_decode_buf/encode_str/prev, CharReader), minimal `detect` (BOMs, NUL -> binary, pure
  ASCII, exact EOL counts), minimal `ucd` (ASCII/Latin-1 case + word class, width 1, drawable ranges),
  `config_defaults` (Appendix B/C values), `FaultFs` (forwarding + injection), `session_init/free`. Everything else:
  stubs.
- `src/win`: all 15.1 headers (`app.h tab.h views.h cmds.h ui.h progress.h docio.h sessmgr.h watch.h largefile.h
  iothread.h fifworker.h selftest.h bench.h winenc.h clip.h render.h notifybar.h dialogs.h`) + private `frame_int.h`
  (T07's modules), 32 `.cpp`. Working: WinMain (`main.cpp`: command line 15.7, cfg dir 13.1, settings load/save path,
  message loop with MsgWaitForMultipleObjects + watch handles, SEH-guarded dispatch, `app_dispatch` routing order of
  15.5), frame (`frame.cpp`: menu, accelerators, WM_* routing to every module hook, exit flow, placement),
  `statusbar.cpp` (9 parts, transient notes), `tabs.cpp` (tab list, tab_new/free basics), `util.cpp` (ui_ask + answer
  queue, ui_error, paths, ANSI helpers), `cmdtable.cpp` (complete table, 88 entries), `selftest.cpp` (framework,
  helpers, framework self-check), `fs_win32.cpp` (basic working FileSys).
- `tests/`: `tf.h`, `t_main.cpp` (runner), 24 `t_<suite>.cpp` (estr + emem real, others empty), `fs_posix.cpp` (working,
  Win98 semantics for move/make_temp), `Makefile` (GNU), `mac/README.md`.
- `tools/`: `lint.py`, `make_icons.py`, `gen_make.py` (generates BOTH Makefiles from one table; edit the table, rerun).

### Interface notes
- **Stub conventions.** Core stubs return `E_NOTIMPL` (or 0/NULL/empty, e.g. `Document::line_count()` = 1). Win
  int-returning stubs return `E_NOTIMPL`; callers treat any `< 0` as failure and `E_NOTIMPL` as "fall back" (e.g.
  `sessmgr_exit()` -> frame asks `docio_close(t, 1)` per tab; `dlg_convert_loss` -> `ui_ask(Q_CONVERT_LOSS)`).
  `cmd_*` stubs call `ui_notimpl("cmd_x")` = status note "Not implemented yet (cmd_x)"; `cmdstate_*` stubs return 0;
  `st_*` stubs call `st_skip(c, "not implemented")` = log line `SKIP st_x (not implemented)`.
- **Private members.** Every class got a minimal `private:` block marked "private state: Txx may change freely". The
  owning task may replace those private members entirely (public declarations need the overseer).
- **Per-suite source lists** (identical in `Makefile` and `tests/Makefile`; FS = `tests/fs_posix.cpp` on the Mac,
  `src/win/fs_win32.cpp` on VC6). Every suite also links `t_main.cpp` (as `tm_<suite>`, `-DTF_SUITE=<suite>`) and its
  `t_<suite>.cpp`:

  | suite | core modules |
  |---|---|
  | estr | etypes emem estr |
  | emem | + esource |
  | ptree | + esource ptree |
  | undo | + esource ptree undo |
  | doc | etypes emem estr esource ptree undo ucd ucdtables codec cptables detect doc |
  | marks | doc list + marks |
  | codec | etypes emem estr esource ucd ucdtables codec cptables |
  | detect | codec list + detect |
  | ucd | etypes emem estr ucd ucdtables |
  | layout | codec list + layout |
  | lexer | etypes emem estr ucd ucdtables lexer lexgen lexhtml lexmd |
  | regex | codec list + regex rxvm rxbt rxpre |
  | search | regex list + hexfmt search |
  | fif | search list + detect fif FS |
  | hexfmt | codec list + hexfmt |
  | edtype | doc list + lexer list + docbind edtype |
  | edlines | edtype list + edlines |
  | edhex | edtype list + edhex |
  | ini | etypes emem estr ini |
  | config | ini + codec list + config |
  | session | ini + codec list + session |
  | fileops | codec list + detect ptree ini session faultfs fileops FS |
  | winenc (VC6 only) | codec list + src/win/winenc.cpp |
  | adversarial | all core + FS |
  So `edlines`/`edhex` may call `edtype.cpp` functions (ed_lang_from, word helpers); `doc` may call codec + detect.
- **Build targets.** Mac: `make -C tests <suite>` builds in its own folder `tests/build/su_<suite>/{strict,asan}` (no
  races between parallel agents) and runs both; `make -C tests` = lint + coretest strict + ASan/UBSan. VC6:
  `w98 build editor t_<suite>` (builds `obj\t_<suite>.exe`, runs it with `-q -tmp C:\PICATEST\tmp -data tests\data`),
  `w98 build editor obj\<name>.obj`, `test`, `testm` (fileops + fif against `M:\dev\editor\tests\tmp`), `DEBUG=1`.
  The NMAKE file uses batch inference rules (`::`) and inline response files (`@<<`), both fine on VC6 nmake.
- **Test framework** (`tests/tf.h`): suites are `void suite_<name>(TF* tf)`; the macros `TF_CHECK`, `TF_EQ_U`,
  `TF_EQ_I`, `TF_EQ_S`, `TF_EQ_MEM` use the parameter named `tf`. Also `tf_log`, `tf_fail(tf, __FILE__, __LINE__,
  fmt, ...)`, deterministic `tf_rand`/`tf_rand_below` (seeded per suite from `-seed`), `tf_path(tf, name, out, cap)`
  (file in `-tmp`, created by the runner), `tf->long_run`, `tf->data_dir`. The runner checks `e_alloc_count()` before
  and after each suite (leaks fail the suite). `e_test_fail_after(n)`: n = 1 (or 0) fails the very next
  e_alloc/e_realloc/e_valloc, one-shot; n < 0 disables. `new` is not covered (and aborts on OOM under clang).
- **Selftest** (`selftest.h`): `pica.exe -selftest [names] -cfg DIR [-log FILE] [-fixdir DIR]`; names: framework frame
  textview docio hexview search fif edit dialogs session watch largefile scenarios (also `st_x`). Log lines `PASS
  st_x (N checks)`, `FAIL st_x: M of N checks failed` (+ indented check details), `SKIP st_x (why)`, summary
  `selftest: N passed, M failed (K skipped)`. `ST_CHECK(c, cond, "msg")` (fixed message) and `st_checkf(c, cond, fmt,
  ...)`. Unanswered `ui_ask` questions fail the running test. `selftest_log()` lines (indented) record status notes,
  questions, error boxes. `st_keys` basics work (`^` `+` `%` prefixes, `{NAME}` keys, typed chars via VkKeyScan,
  faked keyboard state + posted WM_KEYDOWN/UP so accelerators fire); T15 completes it and `st_wait_idle`.
  `st_framework` checks: frame/status bar (9 parts), command table (handlers, ranges, no overlaps), every menu item
  (main + tab menu) and accelerator maps to a command, AltGr-safe accelerators, trace-only routing of every id
  through `app_command`, status notes, the answer queue, a fixture round trip through `fs_os()`, codec init.
- **Commands** (`cmds.h`, `cmdtable.cpp`): `CmdEntry {id, count, fn | fni(int), st | sti(int), name}`; range commands
  take the index in their range: `cmd_mru_open(i)`, `cmd_lang_set(i)` + `cmdstate_lang(i)` (IDM_LANG_FIRST + i, 40
  slots), `cmd_enc_reload_cp(i)` + `cmdstate_enc_reload_cp(i)` (20 slots; menu order 1250 1251 1253 1254 1257 437
  850 852 866 KOI8-R), `cmd_win_activate(i)` + `cmdstate_win(i)` (30 slots). `cmd_update_menu(popup)` is called on
  WM_INITMENUPOPUP (system menu skipped). `cmd_set_trace(fn, trace_only)` hooks `app_command` (T15 audit). Handler
  owners = the file that defines the stub (filecmds/doccmds T09, editcmds T12 incl. go to/brace/bookmarks/options,
  dlgfind/isearch/fifpanel T11, textview T08 view toggles, hexview T10, frame T07 toolbar/statusbar/results/window/
  tab-bar commands, cmdtable T15 help). `cmdstate_hastab` (cmdtable.cpp) is shared: grey with no tab.
- **Exit:** `cmd_file_exit` posts WM_CLOSE; `frame_close()` runs `sessmgr_exit()` (E_NOTIMPL -> `docio_close(t,1)`
  per tab, 0 = cancel), saves placement into `g_app.cfg`, `app_save_config()`, DestroyWindow. Selftest/bench never
  write settings.
- **Tabs:** `tab_new(doc)` takes ownership of `doc`, creates the Highlighter, binds DocLines/marks, assigns
  `g_app.next_tabid++`, view defaults from Config; it does NOT add to the bar: call `app_add_tab(t, activate)`.
  `app_remove_tab(t)` removes it from the bar (activates a neighbour) without freeing; then `tab_free(t)`.
- **Language RCDATA:** `IDR_LANG_FIRST + i` in menu order cpp rc make bat ini asm python html js css xml md diff
  (`IDR_LANG_COUNT` 13), `IDR_HELP` 220. `lang_init` is not called yet (T07 builds the RCDATA LangSource).
- **Minimal codec specifics (T02 replaces):** `enc_parse/enc_token` tokens ansi oem utf8 utf8bom utf16le utf16be
  (with BOM) `utf16le-nobom` `utf16be-nobom` `cpNNNN`; labels "Windows-125x", "OEM 866", "KOI8-R", "ISO-8859-N";
  the UTF-8 encoder returns 0 for BADUNIT markers; `codec_get` returns only UTF-8 and cp1252.
- `es_fmt` also supports `%ld/%lu/%lx`, `%.Ns`, `*` width/precision; it returns the length written (truncated).
  `EStr::appendf` formats at most 1023 chars per call. `es_utf8_encode(ECP_BADBYTE(b))` writes the raw byte b.

### Interface changes / deviations from DESIGN
1. `EolStyle` and `EolCounts` (6.7) are defined in `codec.h` (doc.h includes it) so `detect.h`/`fileops.h` don't
   need doc.h; `codec.h` forward-declares `struct BreakSpec` (ptree.h) instead of including ptree.h.
2. `etypes.h` `#undef`s `E_NOTIMPL` before the enum (winerror.h defines it as an HRESULT macro). Rule (lint-checked):
   in src/win and tests, `#include <windows.h>` comes before any project header.
3. `SaveHooks::after_replace(const char* /*old_copy*/)`: parameter name commented out (unused-parameter error).
4. `ST_CHECK(c, cond, msg)` takes a fixed message (VC6 has no variadic macros); printf-style via `st_checkf`.
5. Added: `LineMarks()/~LineMarks()`, `RangeMarks()/~RangeMarks()`, `~SynthSource()`; private members in every class;
   `FaultFs` lists all forwarded methods; `Config` has `pad_*` fields; `StyleColor` has no background, so INVALID's
   255,224,224 background is fixed in render (T08).
6. Range-command handlers/state functions take `int i` (see Commands). Document > Tabs handlers are
   `cmd_doc_tabw2/4/8`, `cmd_doc_tabspaces` (+ `cmdstate_doc_*`) so `cmd_tab_*` stays the tab-bar context commands.
7. `clip.h` follows DESIGN 15.1 (`clip_copy(Tab*, const Selection&, int hex_pane)`, `clip_get/clip_free/
   clip_has_data`, `ClipData {has_raw, enc, raw, text}`), not the `clip_copy_text/clip_copy_hex` names in TASKS T12.
8. Additions to shared win headers: app.h `app_add_tab/app_remove_tab/app_tab_index/app_dispatch/app_idle_work/
   app_save_config`, timer/child-id/status-part enums; sessmgr.h `sessmgr_app_activated`, `sessmgr_timer`;
   selftest.h `selftest_set_paths`; fifworker.h `fif_running`, `results_create`, `results_visible`,
   `isearch_create`, `isearch_hide`; notifybar.h `notifybar_create/update/height`, `NotifyState` in Tab;
   docio.h `OpenOpts` fields + `docio_default_opts`; ui.h `Q_*` question ids, `ui_notimpl`, answer-queue functions,
   `ui_file_exists/ui_dir_exists`; views.h `CaretInfo`, `PanelHandler`, `SCROLL_*`; cmds.h `CmdEntry` + routing API +
   `cmd_help_show`; iothread.h `IoJob` fields (T09 may extend at the end).
9. Toolbar tooltip strings use the command id as string id (no separate IDS_* range).
10. The selftest summary line appends "(K skipped)".

### Requests for files I don't own
- none

### Gotchas
- **VM A's clock is ~14 h behind the Mac** (VM showed 9:27 AM at 23:53 EET), so every object looks older than its
  source and nmake recompiles everything a target needs on every run. Safe (never stale), just slower: full `pica.exe`
  ~70 s, `nmake test` ~90 s, one suite 10-30 s. Don't "fix" it by setting the VM clock ahead (that would make builds
  stale).
- windows.h macros that break identifiers: `E_NOTIMPL` (handled), `min`/`max`, `near`, `far`, `small` (rpcndr.h),
  `interface`, `DELETE`. Don't use them as names.
- NMAKE: never end a line (even a comment) with a backslash; command.com's 127-char limit is avoided because every
  long cl/link line uses `@<<` response files. `!IF !EXIST(obj)` + `!IF [mkdir obj]` creates obj\.
- Per-suite builds compile the dependencies' *current* sources (e.g. the doc suite compiles codec.cpp): keep your own
  files compiling at all times (write whole files, run your suite before leaving a file half-edited).
- clang `-Wshadow` + `-Wextra`: mark unused parameters `(void)p;`; don't name a derived-class parameter like an
  inherited field (Codec has `kind unit max_len cp`... fields: use `out`/`c` for parameters).
- Vec<T> is memcpy-based: POD elements only. `SessionTab` holds a Vec, so it is allocated with `new` (session_free
  deletes them).

### Tests
- Mac: `make -C tests` -> lint 164 files 0 findings; strict and ASan/UBSan coretest: 1239 checks, 0 failed. Per-suite
  targets (estr emem ptree codec fileops doc edlines ...) build and pass, also four in parallel. `make -C tests mac`
  ("no programs yet"), `make -C tests data` (prints that gen_testdata.py is T02's).
- VC6 (VM A): `w98 build editor` -> pica.exe with **zero warnings**; `w98 build editor test` -> coretest 1239 checks, 0
  failed; `t_estr` 157 checks; `t_winenc`, `t_fileops`, `t_edlines`, `obj\frame.obj` build clean.
- `pica.exe -selftest -cfg C:\PICATEST` -> exit 0; log: `PASS st_framework (4180 checks)`, 12 SKIP lines,
  `selftest: 1 passed, 0 failed (12 skipped)`.
- Screenshot `docs/shots/t00_shell.png`: frame with all menus and the 9-part status bar showing "Not implemented yet
  (cmd_file_open)" after Ctrl+O (VNC). Document > Encoding > Reload as submenus open correctly (Properties greyed with
  no tab); File > Exit (Alt+F, X) ends the process.

### Open issues
- Window placement is saved through `config_store` + `Ini::write` + `file_save`: it only persists once T05
  (`file_read_all`, `file_save`) and T06 (`Ini`, `config_load/store`) land. T07 should verify it.
- Not yet in the shell (T07): toolbar, tab bar, notification bar, status texts, single instance (WM_COPYDATA),
  `lang_init` from RCDATA, MRU/Window menu lists, timers other than TIMER_POLL. The flags part is narrow at 760 px
  (the note is cut off).
- `winenc_init` and the ANSI helpers are stubs; `ui_ansi_to_ecp/ui_ecp_to_ansi` fall back to the minimal codec.

## T02 Codecs, detection, Unicode tables, winenc (wave 1, agent 86box-d1-T02, 2026-11-10)
### Built
- `src/core/codec.cpp`: strict UTF-8 (ASCII fast paths in `codec_decode_buf`, `codec_encode_str`, `CharReader`,
  `codec_prev`), UTF-16 LE/BE (pairs; lone surrogate = one `ECP_BADUNIT`, 2 bytes; final odd byte = one `ECP_BADBYTE`),
  SBCS from generated tables, provider codecs (`DbcsProvider`), enc_* helpers, labels, tokens, `codec_prev`,
  `CharReader`. New private header `codec_tables.h` (SbcsTable layout).
- `src/core/cptables.cpp` (generated by `tools/gen_cptables.py` from Python 3.9.6 codecs): 38 tables = the DESIGN 6.9 list
  plus OEM 720/737/775/858. High-half decode table (0 = undefined) + reverse table sorted by code point; static, so
  `codec_init` only wires pointers.
- `src/core/detect.cpp`: DESIGN 6.10 rules 1-6, EOL counting, `detect_is_binary` (also recognises the BOM-less UTF-16
  NUL pattern and UTF-32 BOMs).
- `src/core/ucd.cpp` + `ucdtables.cpp` (generated by `tools/gen_ucd.py`, Unicode 13.0 from Python's unicodedata;
  private header `ucd_tables.h`): width, simple fold/upper/lower (Latin-1 lookup tables, then binary search over
  stride runs), word class; fixed White_Space and WGL4 drawable lists.
- `src/win/winenc.cpp` + private `winenc_int.h`: providers built eagerly from MultiByteToWideChar (MB_ERR_INVALID_CHARS,
  falls back to flags 0 if refused), lead bytes from IsDBCSLeadByteEx, duplicates resolved with WideCharToMultiByte;
  registered for 932/936/949/950 and for the system ACP/OEMCP when the core has no table, only if IsValidCodePage.
  Input-locale code page, ANSI/CF_TEXT conversions.
- `tools/gen_testdata.py` -> `tests/data/codec/`: `sbcs.txt` (Python decode vectors), `cp932.bin` (test provider),
  `utf8.txt` / `utf16.txt` (1,500 / 3,000 random cases decoded by Python surrogateescape / surrogatepass),
  `ucd_blocks.txt` (per-256-code-point checksums of width/fold/upper/lower/word). All generators are deterministic
  (rerun = identical files).
- Tests: `t_codec.cpp`, `t_detect.cpp`, `t_ucd.cpp`, `t_winenc.cpp` (VC6 only).

### Interface notes
- **Bad markers.** Every codec encodes `ECP_BADBYTE(b)` to the byte b (also in UTF-16 and DBCS). `ECP_BADUNIT(u)`
  encodes to the unit u only in the UTF-16 codecs (own byte order); in UTF-8/SBCS/DBCS it is unrepresentable (0).
  Undecodable input: UTF-8 one marker per bad byte (overlong C0 AF = 2 markers, E0 80 AF = 3, ED A0 80 = 3, cut E2 82
  at eof = 2); SBCS undefined byte; DBCS bad pair = lead byte alone (trail decoded on its own), lead at eof = bad.
- **decode() with eof = 0** returns 0 for an incomplete UTF-8 sequence, a lone UTF-16 byte, a high surrogate without its
  next unit, or a DBCS lead byte at the end of `avail`.
- **codec_prev(s, c, pos, floor)**: self-synchronising codecs use one read of at most 2 x max_len bytes; UTF-16 with
  `(pos - floor)` odd returns `pos - 1` (the odd trailing byte); pos inside a character returns `pos - unit`; pos is
  clamped to `size()`. DBCS (self_sync 0) decodes forward from floor, so pass a line start as floor.
- **CharReader** now copies up to 256 bytes per refill with `ByteSource::read` (never holds a `get()` pointer between
  calls, so other code may use the same source in between). The object is ~280 bytes; re-`init` after the source
  changes. A character cut by `end` decodes as bad bytes.
- `codec_encode_str(subst = 0)` stops at the first unrepresentable char: returns `E_RANGE`, `*unrepresentable = 1`, out
  holds the bytes before it. With subst, an unrepresentable subst falls back to '?'.
- `codec_register_dbcs(cp, NULL)` **unregisters** (used by tests); registering again replaces. A provider without lead
  bytes is exposed as an SBCS codec (`kind EK_SBCS`, self_sync 1, ascii_compat if 0x00-0x7F are identity), and
  `codec_get(enc_cp(cp))` finds it. `enc_cp` reports `EK_DBCS` for 932/936/949/950/1361 and registered DBCS providers.
  At most 8 providers; returns E_RANGE for cp 0/65001/1200/1201.
- `codec_get` works before `codec_init` (lazy table wiring), but call `codec_init` on the UI thread first as specified.
- **Labels** (`enc_label`): ANSI, OEM, UTF-8, UTF-8 BOM, UTF-16 LE / UTF-16 BE (with BOM) and **"UTF-16 LE no BOM" /
  "UTF-16 BE no BOM"** (detected BOM-less files), Windows-874/125x, OEM 437/720/737/775/850-869, KOI8-R, KOI8-U,
  ISO-8859-1..9/15, Shift-JIS, GBK, Korean 949, Big5, else "Code page N". The ANSI/OEM aliases follow
  `enc_set_system`. Tokens: as T00 plus `cp65001` parses as utf8; "cp" must be followed by digits.
- `enc_cp_name(u16 cp, char* buf, u32 cap)` exists in codec.cpp (the label without the ANSI/OEM aliasing, for messages
  like "can't be stored in ANSI (Windows-1252)" in 12.4); see requests.
- **detect**: examines all bytes it is given (caller passes the 64 KB sample). In a partial sample (`whole_file = 0`) a
  CR in the last code unit is not counted, and a UTF-8 sequence cut by the end is allowed (it does not count as the
  required multi-byte sequence). BOM-less UTF-16 needs `utf16_nobom` and >= 16 bytes; its EOLs are counted in units.
  A UTF-8 BOM wins even if the body is invalid UTF-8. UTF-32 BOM: `binary = utf32 = 1`, enc = `o.ansi`, bom_len 0.
- **ucd**: Unicode 13.0. width: 0 = Mn/Me/Cf (except U+00AD and the visible Arabic number signs) + U+1160-11FF;
  2 = EAW W/F + unassigned CJK ideograph blocks; else 1 (fast path < U+0300). fold = simple folding (U+1E9E -> U+00DF,
  U+0130 unchanged); upper/lower simple (U+0130 lower = 'i', U+1F80 upper = U+1F88). Bad markers: width 1, map to
  themselves, not word/space/drawable. word = L*, M*, Nd, Pc.
- **winenc**: `winenc_mb_to_ecp/ecp_to_mb(cp, ...)` use the core codec when one exists (undefined bytes ->
  `ECP_BADBYTE`, which encodes back exactly; clip.cpp should map bad markers to U+FFFD/'?' itself if it wants that),
  else MultiByteToWideChar/WideCharToMultiByte (E_UNSUPPORTED for a code page that isn't installed). Lost characters
  (incl. BADUNIT) become '?' and are counted. The ANSI helpers use `GetACP()`. `winenc_input_cp()` = the keyboard
  layout's LOCALE_IDEFAULTANSICODEPAGE (falls back to the ACP), cached until `winenc_input_changed()`.
  `winenc_init` is idempotent. Provider memory comes from HeapAlloc (not e_alloc; never freed while registered).
- VM A facts: ACP 1252, OEMCP 437, input cp 1252; 932/936/949/950 **not installed**; 20 of the 38 table code pages are
  installed and all agree with MultiByteToWideChar except 5 ISO-8859-7 bytes (Python = 2003 revision: A1/A2 quotes,
  A4 euro, A5 drachma, AA; Win98 = 1987). Pica keeps the 2003 table.
- Speed (Mac, M-series, strict -O2): t_codec 0.08 s, t_ucd 0.3 s. VM A: `w98 build editor t_codec` 12.7 s including the
  build (34 s before the random runs were rebalanced), t_ucd run 9 s, t_detect/t_winenc 10-18 s including the build.

### Interface changes / deviations from DESIGN
- `detect` `eol_mixed` = CRLF and LF both > 0 (the Document rule of 7.2: lone CRs count only in CR mode, where they are the
  only kind), not "more than one of the three counters > 0" as 6.10 says. Otherwise a CRLF file with one stray CR
  would show "Mixed".
- 4 extra SBCS tables (720, 737, 775, 858); `codec_register_dbcs(cp, NULL)` unregisters; provider without lead bytes =
  SBCS codec; BOM-less UTF-16 labels add "no BOM".
- CharReader private state grew (see notes). No public signature changed.

### Requests for files I don't own
- `src/core/codec.h`: add after `enc_label`:
  `const char* enc_cp_name(u16 cp, char* buf, u32 cap);   // "Windows-1252", "OEM 866", "KOI8-R" (no ANSI/OEM alias)`
  (already defined in codec.cpp; needed for the 12.4 messages "... can't be stored in ANSI (Windows-1252)").
- `src/core/codec.h` comment of `codec_register_dbcs`: "p == NULL unregisters; a provider without lead bytes acts as an
  SBCS code page".
- `src/win/winenc.h`: nothing required; optionally note that `winenc_mb_to_ecp` returns bad markers for undefined bytes.

### Gotchas
- Python 3.9's `unicodedata.east_asian_width` returns 'F' for **unassigned** code points; gen_ucd.py ignores EAW for Cn.
- A DBCS round trip is not byte-exact for duplicate mappings (cp932 has 396 code points with two byte pairs, e.g.
  FA5C and ED40 -> U+7E8A), and re-encoding a duplicate next to a bad lead byte can change how that lead pairs up. Pica
  never re-encodes untouched bytes, so this only matters for conversions; the tests check per character.
- `MultiByteToWideChar(1, ...)` is CP_OEMCP and 2 is CP_MACCP: code page numbers 0-3 are pseudo code pages.
- ISO-8859-7 differs between Python and Win98 (above).

### Tests
- Mac: `make -C tests codec detect ucd`: strict and ASan/UBSan both pass (codec 813 checks, detect 154, ucd 80; the
  random round trips aggregate into few checks). `-long -seed 7`: pass. `python3 tools/lint.py` on my 14 files: 0
  findings. `make -C tests data` + both generators: byte-identical output.
- Dependent suites with my code: hexfmt 1173, config 150, session 414, layout/regex/search (still empty) pass; doc,
  fileops, edtype currently fail to build in other tasks' in-progress files (doc.cpp `Document::View`, fileops.cpp
  shadow, lexhtml.cpp unused variable), not in codec/detect/ucd.
- VC6 (VM A): `obj\codec.obj cptables.obj detect.obj ucd.obj ucdtables.obj winenc.obj` zero warnings; `t_codec` 813,
  `t_detect` 154, `t_ucd` 80, `t_winenc` 48 checks, 0 failed. Full `w98 build editor` / `test`: stop in `src\core\doc.cpp`
  (T01 mid-edit: C2248 'tree_' private), not in T02 files.
- Coverage: every SBCS table round-trips all 256 bytes, matches the Python vectors and its reverse table over the BMP;
  UTF-8 invalid corpus + 1,500 Python cases; UTF-16 3,000 Python cases; cp932 test provider: every pair vs the table;
  100,000 random strings per run over all codecs (UTF-8 30k, UTF-16 LE/BE 20k each, SBCS 15k, DBCS 15k; `-long`: 400k
  each): decode -> encode byte-exact, CharReader over 1-3-byte chunks, codec_prev at every boundary (with and without a
  floor); 57 detection table cases + 64 KB samples; ucd spot checks, fold idempotence over all code points, 4,352 block
  checksums vs Python.

### Open issues
- DBCS display/IME is untestable here (no DBCS code page installed on VM A); the WinProvider build path was exercised
  only with 1251/1252 (no lead bytes).
- No statistical 1251-vs-1252 guessing (ARCH 3.4: 1.1).

## T06 Layout, hex formatting, INI, config, session model (wave 1, agent 86box-d1-T06, 2026-11-10)
### Built
- `src/core/layout.cpp`: LineLayout (all DESIGN 6.12 kinds, show-whitespace glyphs, hit testing both ways, word/char
  wrap) and LongLineIndex (checkpoints, progress/cancel, chunk-straddling decode). `lay_ctrl_name`.
- `src/core/layout_int.h` (new private header): `lay_set_props(width, drawable)` test hook (NULL = uc_width /
  uc_is_drawable). Tests only.
- `src/core/hexfmt.cpp`: offset/byte text, `hex_char_glyph`, `hex_parse_bytes`, `hex_parse_pattern`,
  `goto_parse_offset`, `goto_parse_line`, `inspect_bytes`.
- `src/core/ini.cpp`: tolerant Ini (order, comments, unknown and garbage lines kept verbatim; CRLF write).
- `src/core/config.cpp`: `config_load/store` (table-driven, every Appendix C key), colours, MRU/history lists.
- `src/core/session.cpp`: `session_to_ini/from_ini/validate` (13.6 format, `[end] ok=1` sentinel).
- Tests: `tests/t_layout.cpp t_hexfmt.cpp t_ini.cpp t_config.cpp t_session.cpp`.

### Interface notes
- **LineLayout (T08).** `build()` clears and lays out; it stops early (see `bytes()`, a T06 addition) at an incomplete
  char when `eof == 0`, and at 65535 glyphs (LayChar.glyph is u16; a 64 KB window of tabs can hit it). Codec NULL =
  one byte per char. Every glyph is one cell, except LK_WIDE (1 glyph, 2 cells) and LK_ZERO (1 glyph, 0 cells), so
  ExtTextOutW lpDx = width*cellw per char split over its glyphs. Kinds:
  - LK_TAB: `width` spaces, first glyph U+00BB when show_ws; width from the ABSOLUTE cell (start_cell matters).
  - LK_SPACE: ' ' or U+00B7. LK_CTRL: C0 + DEL names, plus (extension) C1 U+0080-009F names "PAD".."APC"
    (`lay_ctrl_name` covers 0x80-0x9F; returns NULL for non-controls).
  - LK_BAD: "xHH" / "uHHHH". LK_NODRAW: narrow non-drawable (glyph = the code unit) and every cp > U+FFFF (glyph
    U+FFFD, width 1).
  - **LK_WIDE is used for every width-2 char, drawable or not** (core can't know about fonts): render uses the CJK
    font if present, else a 2-cell box. Narrow non-drawable = LK_NODRAW.
  - LK_ZERO: `cell` = the base char's cell (draw overlapping), width 0. A zero-width char with no preceding char in
    the same build is laid out as a normal 1-cell char (LK_CHAR/LK_NODRAW).
- Caret/hit test: `cell_of_offset(off)` for an offset inside a char returns that char's cell; for an LK_ZERO char it
  returns the cell after its base. `offset_of_cell(cell, nearest)` takes a cell BOUNDARY (caller rounds the pixel:
  `(x + cellw/2) / cellw` for nearest) and only returns cursor stops (char starts that are not LK_ZERO, or `bytes()`):
  never between a base and its marks. nearest: a boundary in the middle of a multi-cell char (tab, ctrl, wide) goes to
  the nearer end (exact middle -> next).
- `wrap_rows(w, word, rows)`: char indices; rows never start at LK_ZERO, a row always holds >= 1 char (+ marks) even
  if wider than w; word mode breaks after space/tab, after `- , . ; : ! ? ) ] } / \ | & + = * % >`, en/em dash,
  ellipsis, U+00BB and after any LK_WIDE; trailing spaces/tabs hang past the edge (the row ends after them); no
  opportunity -> hard break. Tabs keep their line-relative widths in continuation rows. width 0 = 1.
- **LongLineIndex.** First checkpoint (start, 0); then the first char boundary >= 4096 bytes after the previous
  checkpoint that is not a zero-width char, so `LineLayout::build(p + cp_pos - start, ..., cp_cell, eof)` from any
  checkpoint gives exactly the cells of a full layout (tested on 1 MB lines, UTF-8 and UTF-16, odd chunk sizes).
  Progress every 256 KB (and decoding in <= 64 KB pieces), nonzero = E_CANCEL; errors clear the index. `at_offset`
  needs start <= pos <= end (E_RANGE), empty index = E_STATE. Cells saturate at 0xFFFFFFFF. ~8 bytes per 4 KB.
- **hexfmt (T10/T11/T12).** `hex_char_glyph`: '.' for C0/DEL, C1, bad bytes, non-drawable, width != 1; non-SBCS codecs
  show ASCII only. `hex_parse_bytes/pattern`: tokens split by blanks, CR/LF, ',' ';'; optional `0x` prefix or `h`
  suffix; digits taken in pairs in written order (`0x4D5A` = 4D 5A); a single digit only with prefix/suffix (`0x4`,
  `4h`); odd counts and empty input are E_SYNTAX (so pasting "abc" as hex fails and falls back to text). Pattern: `?`
  is a nibble wildcard (`??` byte, `D?` mask F0), mask 0xFF/0xF0/0x0F/0x00; mask must be non-NULL. Returns E_OK, not
  a count.
- `goto_parse_offset`: blanks ignored, case-insensitive; hex default; `#123` / `123d` decimal (a trailing d after
  decimal digits only; `ed` is hex ED); `+N`/`-N` relative to cur; `end`, `end+-N`; `N%` decimal 0-100 of size. Valid
  result 0..size inclusive. Errors E_SYNTAX/E_RANGE with texts like "Offset beyond end of file (size 0x1F40)",
  "Offset before the start of the file", "Number too large (max 0xFFFFFFFF)"; `*out` untouched on error.
- `goto_parse_line`: cur_line and `*line` are **0-based**, the text is 1-based; `*col` = user column - 1 (0 when no
  column given; the caller interprets it as a char column and clamps to the line end). Syntax `120`, `120:5`, `+10`,
  `-5`, `end`, `end-N`, `:7` (column on the current line). Out of range is an error ("Line beyond end of file (500
  lines)", "Lines start at 1", "Columns start at 1"), not clamped. Decimal only.
- `inspect_bytes`: fields "-" when short; `avail` = min(avail, 8); bin = first byte. Floats: NaN -> "NaN", infinities
  "+Inf"/"-Inf", negative zero "-0", else sprintf %.9g / %.17g with the exponent normalised to 2+ digits (VC6 prints
  3); identical strings on VC6 and clang (checked incl. denormals, 1e300, 0.1).
- **Ini.** Sections/keys case-insensitive; duplicate sections are all searched (first key wins), new keys go after the
  last key line of the LAST occurrence, new sections are appended after a blank line; keys before any header are
  section "" (also NULL). Values: no inline comments (paths and masks contain ';'); one pair of surrounding quotes is
  stripped, and `set` quotes values with leading/trailing blanks or surrounding quotes, so every string round-trips;
  CR/LF in values become spaces. `set` rejects bad names (E_SYNTAX). NUL bytes become blanks; leading UTF-8 BOMs are
  dropped. `get` pointers stay valid until the next mutation. `get_i`: decimal, E_RANGE input clamps to lo/hi, junk
  -> def. `get_x`: es_parse_u32 base 0; `set_x` writes `0x...`. `get_b`: 1/0, yes/no, true/false, on/off.
  `remove` removes all occurrences (E_NOTFOUND if none); `remove_section("")` drops the global part.
  `section_count/section(i)` list header occurrences in order (duplicates included). `write` clears `out` first.
  Lookups are O(sections + keys in the section): fine for 1000-tab sessions.
- **Config.** `config_store` writes every key (also colours: `s_<style>=RRGGBB,bold` or `sys,0`; `u_<ui>=RRGGBB` or
  `sys`) and removes stale list keys (f/hN beyond the list); unknown keys stay. `config_load` clamps (ranges not in
  Appendix C: font_pt 6-72, font_px 0-64, window x/y +-32000, w 200-16000, h 150-16000, results_h 40-4000, find mode
  0-3); bad tokens keep the default; colour text may start with '#'. List loading compacts empty entries.
  `config_mru_add` dedupes case-insensitively; `config_hist_add` exactly (case matters for searches); both move to the
  front and drop the oldest.
- **Session (T13).** `session_to_ini` removes existing `[Session]`, `[TabN]`, `[end]` sections first (other sections
  stay), writes `[end] ok=1` last. Bookmarks are 0-based line numbers (LineMarks values), comma-separated.
  `disk_mtime` = 16 hex digits hi:lo. `session_from_ini` requires: `s` initialised (it frees old tabs), sentinel ok=1,
  version=1, count <= 4096, every `[TabK]` with a unique id != 0, and a path unless untitled; otherwise E_FORMAT with
  `s` empty. Values are tolerant: clamped (tab_width 1-16, positions <= EPOS_MAX) or defaulted; bookmarks sorted/
  deduped; `active >= count` -> 0; `next_tabid`/`next_untitled` are raised past the highest id / untitled_no.
  `session_validate` = full parse (what ab_read accepts also loads), `*seq` = the session's seq.

### Interface changes / deviations from DESIGN
- layout.h: added `u32 LineLayout::bytes() const` (public, after wrap_rows). New private header `layout_int.h`.
- ini.h: private members replaced; added `struct IniLine` (private helper type) before the class.
- C1 controls as control pictures and "LK_WIDE for all wide chars" (above) are interpretations of 6.12.

### Requests for files I don't own
- none

### Gotchas
- **Test helper class names must be unique across tests/*.cpp**: coretest links all suites into one program, and two
  `class ChunkSource` (t_codec.cpp had one) silently break each other under -O2 (ODR, inline methods merged). I use
  `Lay*`/`Hex*` prefixes; per-suite builds don't show this, only `make -C tests coretest`.
- VC6 can't stringize a string literal containing `\"` inside a macro argument (C2017 in TF_EQ_S(x, "\"q\"")): put
  such literals in a variable first.
- macOS SDK marks `sprintf` deprecated (-Werror): hexfmt wraps its one call in `#pragma clang diagnostic` (guarded by
  `__clang__`, invisible to VC6).
- `offsetof(Config, field)` in static tables works on both compilers (Config is POD).

### Tests
- Mac `make -C tests layout hexfmt ini config session`: strict + ASan/UBSan all pass (layout 21056 checks, hexfmt
  1173, ini ~4790, config 150, session 414), also seeds 2-20, 77, 1234, 99999. Full `make -C tests coretest`
  (all suites, both builds): 39775 checks, 0 failed. Lint: my files clean (the 13 `.lng` LF findings are T04's).
- VC6 (VM A): `obj\layout.obj hexfmt.obj ini.obj config.obj session.obj` zero warnings; `t_layout t_hexfmt t_ini
  t_config t_session` build with zero warnings and pass (same check counts as the Mac). Full `w98 build editor`
  currently fails in doc.cpp (T01 mid-edit, C2248 tree_ private), not in T06 files.
- Real-table checks (T02's ucd and cp437) ran and passed once T02's tables landed.

### Open issues
- Word-wrap break rules are simple (no UAX #14); CJK breaks after every wide char.
- LongLineIndex has no incremental update: T08 rebuilds it after edits inside the line (14.2).

## T05 File I/O: FileSys, load, FileSource, index, safe save, backups, A/B files (wave 1, agent 86box-d1-T05, 2026-11-10)
### Built
- `src/core/fileops.cpp`: `file_load` (memory: share read+write, 64 KB reads into one `e_valloc` block, progress/cancel;
  on-demand: `FileSource` deny-write, falls back to share-all with `shared_ro = 1`; `detect` on the first 64 KB;
  `force_enc` recomputes BOM length/EOL counts in the forced encoding), `FileSource` (LRU cache of 64 KB blocks,
  `close`/`reopen`), `file_index`, the safe save (`file_write_temp` steps 1-3, `file_replace` steps 4-9, `file_save`),
  `file_read_all`, `file_write_direct`, `ab_read`/`ab_write`, backup header write/parse, `backup_write/list/open/
  delete_tab`, `backup_file_name`.
- `src/core/faultfs.cpp`: FaultFs with one-shot (`fail_on`), persistent (`fail_from`), simulated power loss
  (`crash_after`), torn writes/copies, lying short writes, late failures (op done, error reported), fake free space,
  a "locked" path.
- `src/win/fs_win32.cpp`: refined (SetLastError before SetFilePointer/GetFileSize, own `copy` loop, stat fallbacks,
  more error codes, function pointers looked up at static init: thread-safe).
- `tests/fs_posix.cpp`: Win98 semantics emulated: move/remove of a file we hold open -> E_ACCESS, remove of a
  read-only file -> E_ACCESS, share-mode conflicts -> E_SHARING (open-handle table), birth time as ctime.
- `tests/t_fileops.cpp`: the suite (see Tests).

### Interface notes
- **Hooks decide the mode of `file_replace`.** `hooks != NULL` means an on-demand save over the file the document's
  FileSource reads: no in-place fallback (a locked target gives E_SHARING with the temp file kept), `keep_bak`
  ignored, and the old copy is kept for the source. `hooks == NULL` = in-memory semantics (in-place fallback, old
  copy deleted or renamed to `<path>.bak`). For **Save As of an on-demand document to another path pass NULL hooks**
  (the source is a different file; passing hooks would reopen the source on the other file's old copy; `reopen`
  refuses a file of a different size or with a different cached block, but don't rely on that).
- Hook calls: `before_replace()` only when an existing target is replaced (not for new files), right before step 6.
  Then exactly one `after_replace(p)`, where **p is the file that now holds the original bytes**: `r->old_copy`
  after success (the tab owns it: delete it on close/reload, it is held open deny-write by the source), the target
  path after a failure that left/put the original back, `r->old_copy` if the rollback could not restore it. A typical
  hook: `before: src->close()`, `after: src->reopen(p)` (handle the error: the doc then reads only cached blocks and
  `err()` = E_STATE).
- On-demand saves read the document through a SnapshotSource over a **private second FileSource**
  (`FS_READ | same share mode as the document's source`): **delete it before `file_replace`** (Win98 can't rename a
  file anyone holds open).
- Pass the **same SaveResult** from `file_write_temp` (worker) to `file_replace` (UI): `r->st` holds the temp file's
  stat and `file_replace` refuses (E_IO) if the temp file vanished or changed size meanwhile.
- SaveResult after a failure: `temp` = a kept temp file (complete new content when `failed_step >= 4`; the caller
  names it in the message and deletes it when no longer wanted), `old_copy` = a file holding the complete old
  content (only set when verified), `stray` (new field) = a `~P*.TMP` we may have left behind. `failed_step`: 1 folder
  or space, 2 temp write, 3 verify, 4 target state (read-only -> E_ACCESS, temp removed), 6 move aside / in-place, 7
  final rename (rolled back). `in_place = 1` when the fallback overwrote the target.
- Reported-failure double check: a move/remove that returns an error is re-checked with stat (SMB redirectors can
  report errors for calls that happened); nothing is deleted unless its state is confirmed.
- `file_load`: `ondemand_threshold` 0 = 16 MB; size <= threshold loads into memory. `cache_blocks` 0 = 64. If the file
  shrank during the load, the bytes read are kept and `r->st.size_lo` is the size read (so polling reloads). `st` is
  taken before opening (any later change is seen by polling).
- `file_index`: own handle with FS_SHARE_ALL (works next to the deny-write FileSource). `block_breaks[i]` = break
  units starting in 64 KB block i of the file (what `PieceTree::set_counts` wants). **EolCounts use the detect_eol
  semantics in both LF and CR mode** (CRLF pairs, bare LFs, lone CRs, in code units; CR mode with no LF: cr = breaks).
  Progress per 64 KB block (callers throttle). C: 10 MB in 160 ms.
- Backups: `backup_write` takes the generation from `m.gen` (use last + 1 mod 256, keep `m.seq` increasing); it
  refuses with **E_EXISTS** to overwrite a *valid* backup of the same name (a torn leftover is replaced), creates the
  folder if missing, verifies the size, then deletes **all other** generations of that tab (torn leftovers too).
  `backup_list` returns the newest valid generation per tab id, sorted by tab id (newest = higher seq, then the later
  8-bit gen, wrapping); files whose name and header ids disagree are ignored. `backup_open` returns a MemSource
  (caller deletes). Header parse is strict about structure, tolerant of unknown keys and unknown `enc=` tokens (ANSI).
- `ab_read`: validator result `>= 0` = valid; newest by seq (tie: file 1); E_NOTFOUND if neither exists, E_FORMAT if
  none is valid. `ab_write` writes the file that is *not* `last_which` (0 -> file 1), checks the size, returns the new
  `which`; on failure `which` stays `last_which` (keep using it).
- `fs_os()` objects are globals constructed at CRT startup (no lazy statics). `FileSys::copy` overwrites in place
  (keeps the target's attributes/ctime) and flushes; both ends shared read+write.

### Interface changes / deviations from DESIGN
- `SaveResult` gained `char stray[260]` at the end. FaultFs gained public test members at the end (extra kinds
  `F_STAT F_ATTRS F_CTIME F_SPACE F_FIND F_MKDIR`, `F_ALL_KINDS`, `fail_from`, `crash_after`, `total_calls`,
  `set_torn_writes`, `short_write_on`, `set_fail_late`, `set_free_space`, `set_locked`). FileSource private state
  replaced (cache). No declared signature changed.
- On-demand vs in-memory is chosen by `hooks != NULL` (DESIGN 13.3 doesn't say how); see Interface notes.
- `<path>.bak` = full name + ".bak" (`foo.txt.bak`).
- `tests/fs_posix.cpp` exports `u32 fs_posix_open_count()` (Mac-only test helper, not in fsys.h).

### Requests for files I don't own
- none

### Gotchas
- Probe confirmed in the suite: moving/deleting a file we hold open fails with E_ACCESS on C: and E_SHARING on M:;
  a deny-write handle makes opens for writing fail with E_SHARING; a second reader with share-all works. Saving an
  on-demand doc without closing its source fails cleanly (target intact, leftovers named).
- In-place fallback (target locked by another program) is not crash-atomic: a failure or power loss during the
  overwrite can damage the target; the old version is then in `r->old_copy` (`~POxxxx.TMP`, same folder). A power
  loss between the two renames of a normal save leaves the target missing, the old file as `~POxxxx.TMP` and the new
  one as `~PCxxxx.TMP` (sub-millisecond window on C:; T13/T14 could look for such pairs at startup).
- `fs_posix.cpp` open-handle table has no locking (core tests are single-threaded).

### Tests
- Mac: `make -C tests fileops` strict + ASan/UBSan: **5213 checks, 0 failed** (578+ faulty saves: every call of 13
  kinds failing once and from then on with each error kind, power loss after every call, short writes, late
  failures, over 6 scenarios: new, existing, .bak, on-demand with hooks, locked, on-demand locked; torn backups at every
  truncation length; A/B damaged at every length + power loss at every call; FileSource random reads with caches 1, 2,
  64; file_index vs brute force for 6 break specs incl. CR/LF split by block boundaries). Mutation check: breaking the
  rollback, in-place restore, temp verify, hooks, A/B target choice, move/remove double checks, trailer check or the
  index carry each makes the suite fail. `make -C tests coretest`: 87028 checks, 0 failed; `lint` has 1 finding in
  `src/core/ptree.cpp:50` (T01, not mine). fif and adversarial suites (they link fs_posix.cpp) still pass.
- VC6 (VM A): `obj\fileops.obj`, `obj\faultfs.obj`, `obj\fs_win32.obj`, `t_fileops` zero warnings at /W3.
  `t_fileops` on C:\PICATEST\tmp: **5220 checks, 0 failed** (+7 Win32-only: hidden attribute and creation time kept);
  on M:\dev\editor\tests\tmp: **3285 checks, 0 failed** (smaller matrix on network drives; 72 s).
- Timings (VM A): C: 10 MB save new 1650 ms, over existing 1590 ms, load 170 ms, index 160 ms. M: 1 MB save new
  2700 ms, over existing 2360 ms, load 930 ms.
- Full VC6 `nmake` not run: the tree isn't green (ptree.cpp lint finding, Wave 1 agents mid-edit).

### Open issues
- Startup check for `~PO*.TMP`/`~PC*.TMP` pairs next to a missing file (after a crash between the renames) is not done
  anywhere yet (T13/T14 candidate).
- `free_space` failures are ignored (the save proceeds); on-demand `keep_bak` is ignored (the old copy belongs to the
  source; the tab could rename it to `.bak` when it closes).

## T04 Lexers and language files (wave 1, agent 86box-d1-T04, 2026-11-10)
### Built
- `src/core/lexer.cpp`: language registry (`lang_init/shutdown/count/at/find/for_file/parse`), the find-results lexer
  (`lang_results`), and the incremental `Highlighter` (gap buffer of u16 start states, convergence, idle work to the
  target, synchronous catch-up, dirty and "painted while unknown" tracking).
- `src/core/lexgen.cpp`: the generic data-driven lexer (`GenLexer`), the keyword hash table (`KwTable`) and the
  `.lng` parser (`lng_parse`).
- `src/core/lexhtml.cpp`: HTML/XML lexer (tags, attributes, values, entities, comments, CDATA, `<?xml ?>`, doctype;
  `<script>`/`<style>` bodies lexed by the js/css generic definitions, sub-state in bits 0-9, HTML mode in bits 10-14).
- `src/core/lexmd.cpp`: Markdown lexer (ATX/setext headings, lists, quotes, fenced + indented code, inline code,
  emphasis, links, autolinks, inline HTML, multi-line `<!-- -->`, tables, hr).
- `src/core/lexer_int.h` (new private header): shared declarations of the lexers and the parser.
- `src/langs/*.lng` (13 files, CRLF, ASCII): cpp rc make bat ini asm python html js css xml md diff. Complete lists:
  C89-C++20 + VC6/MS extensions (`__declspec`, `__int64`, `__stdcall`...), ~300 Win32/COM/DirectX/MM names; RC
  statements, controls, style constants; NMAKE + GNU make; COMMAND.COM/CMD commands; MASM/TASM/NASM directives and
  8086-PII + x87 + MMX + 3DNow! + SSE instructions and registers; Python 2+3 keywords, builtins, exceptions; ES3 + later
  JS words, builtins, DOM; CSS1-3 properties/values/at-rules/pseudo-classes; HTML 4.01 + HTML5 + legacy elements and
  attributes. **The format is documented at the top of `src/langs/cpp.lng`.**
- `tests/t_lexer.cpp`: suite `lexer` (see Tests).

### Interface notes
- **Highlighter contract for T08/T01.** `lines_changed(first, removed, added)`: first = line containing the edit start,
  removed/added = line breaks removed/inserted (EditEvent `line`, `lines_removed`, `lines_added`); line `first` is
  always treated as changed. `EV_RESET`/`EV_INDEXED` -> `lines_reset()`. If the counts don't match `lp_count()` the
  highlighter resets itself (never crashes, just re-lexes).
- **The view must call `set_target(last visible line)`** (on paint/scroll). `work()` lexes only until that line's start
  state is known, plus one non-destructive probe of the next line (that probe is what lets an undone edit / a closed
  `/*` converge immediately). It does not lex the rest of the file in the background. `attach` keeps the target.
- `style_line(line, ...)` catches up synchronously when the line is at most 200 lines past `valid_upto()` (worst case
  ~2 ms on VM A), else draws ST_DEFAULT, returns 0 and remembers the row; `take_dirty` reports it once its state is
  known, together with every line whose start state changed. Plain text (`lang == NULL` / no lexer) and disabled:
  `style_line` fills ST_DEFAULT and returns 1, `work` returns 0, `state_known` is true, `valid_upto() == lp_count()`.
  `set_enabled(false)` frees the state buffer (2 bytes/line + 64 KB scratch); enabling again re-lexes from the top.
- Lexers return states `< 0x7FFF` (the highlighter uses bit 15 and 0x7FFF). Generic states use only bits 0-9: bits
  0-2 mode (1 block comment, 2 string continued by an escape at EOL, 3 multi-line string, 4 `//...\` continuation),
  bit 3 inside a preprocessor line, bits 4-5 comment/string index, bits 6-9 nesting depth. `(state & 7) == 1` means
  "inside a block comment" for generic languages (T12 brace matching / toggle comment can use it; or simply check the
  style at the position via `style_line`).
- **Registry.** `lang_at(i)` order = built-in source order (cpp rc make bat ini asm python html js css xml md diff, the
  RCDATA/menu order), then languages added by override files (max 40 total). An override file with the same base name
  (case-insensitive, path stripped) replaces the built-in; if it is malformed the built-in is used and the error is
  reported. `lang_init` returns E_OK (even with override errors; check `errors`), E_FORMAT if a built-in failed, or
  E_NOMEM. Error lines look like `rc.lng: line 2: unknown key 'bogus' (using the built-in definition)\n`.
  `lang_init` first calls `lang_shutdown`: all earlier `LangInfo*` become invalid (call it once at startup).
- `lang_for_file(name)`: path stripped; whole names (`names=`, DOS wildcards, e.g. `makefile.*`) first, then the last
  extension; later (user-added) languages win ties. `.dsp` -> make, `.dsw`/`.cfg`/`.reg`/`.inf`/`config.sys` -> ini,
  `.inc` -> asm, `SConstruct` -> python, `.manifest`/`.svg` -> xml, `.txt`/`.nfo`/`.log` -> NULL.
- `LangInfo` comment tokens for Ctrl+Q: cpp/rc/js `//` (+ `/* */`), css `/* */` only, html/xml/md `<!-- -->` only, bat
  `REM ` (with the trailing space), make/python `#`, ini/asm `;`, diff none. `use_tabs`: make 1, python 0 (tab_width
  4), others 2. `braces`: generic default `()[]{}`, html `<>{}()`, xml `<>`, diff empty.
- `lang_parse` (tests/tools): the lexer it creates is owned by the registry and freed by `lang_shutdown`.
- `lang_results()` is static (works without `lang_init`): `Search ...` header HEADING, file lines SECTION,
  indented `NNN:` NUMBER, the rest DEFAULT.
- Styles used: see each lexer's file header. Unknown HTML element/attribute names (not in html.lng lists; `data-*`,
  `aria-*` and `x:y` names count as known) are styled KEYWORD4 as a typo hint; XML never does this.
- **Throughput** (`t_lexer` speed lines, whole-document lex through the Highlighter): VM A (PII-233, VC6 /O2): C/C++
  118,000 lines/s (3.1 MB/s, ~8 us per line), HTML 155,000, Python 156,000, JS 133,000, asm 229,000, Markdown 429,000
  lines/s. Mac (M-series, -O2): ~15M lines/s.

### Interface changes / deviations from DESIGN
- `lexer.h`: added public `debug_state(line, &state)` and `debug_lexed()` at the end of `Highlighter`'s public part
  (tests/diagnostics), replaced the private members, and clarified the `lines_changed` argument meaning in a comment.
  No declared signature changed.
- New private header `src/core/lexer_int.h` (allowed `<module>_*.h`); no new compilation units.
- The ARCH 7.2 `.ini`-style format is extended (keys listed in cpp.lng); keyword classes are 1-4 as DESIGN says.

### Requests for files I don't own
- T07 (`main.cpp`/frame): build the built-in `LangSource` from `IDR_LANG_FIRST + i` RCDATA with names `cpp.lng`,
  `rc.lng`, ... (any name works as long as the base name matches the override file name), the override `LangSource`
  from `<cfg dir>\langs\*.lng`, call `lang_init` once before tabs are created, and show a non-empty `errors` string
  to the user (notification or message box). RCDATA NUL padding is fine (parsing stops at a NUL); a UTF-8 BOM is
  skipped.
- T09 (Document > Syntax menu): the static menu covers the 13 built-ins (`lang_at(0..12)` matches it); languages added
  by override files (`lang_count() > 13`) need dynamic items `IDM_LANG_FIRST + i` (40 slots reserved) with
  `lang_at(i)->name`.
- T08: `DocLines` (T01) + view: call `set_target` as described; after `work()` returns, repaint the `take_dirty` range;
  `ed_lang_from` can copy `line_comment`, `block_open/close`, `braces`, `tab_width`, `use_tabs` straight from LangInfo.
- T13 (session): store `LangInfo::id` (max 15 chars) in `SessionTab::lang`; restore with `lang_find(id)`.

### Gotchas
- `case=0` folds keywords and comment/string tokens (ASCII only). Keyword lists are added after the whole file is read,
  so `case=` may come anywhere.
- Inside `.lng` values `\s` is a space, `\t` a tab, `\\` a backslash (e.g. `comment=REM\s`, `line=diff\s diff_header`);
  a lone `\` is literal (`string=" " \`).
- Mac clang marks `sprintf` deprecated (error under -Werror); the speed log uses integer formatting.
- C++ raw strings `R"(...)"` are not supported (styled as an ordinary string); JS `${...}` inside template strings stays
  string-coloured; Markdown emphasis does not span lines (per-line lexer).

### Tests
- Mac: `make -C tests lexer` -> strict and ASan/UBSan: 47,162 checks, 0 failed (also seeds 2 3 7 99 12345, and `-long`:
  149,350 checks). Covers: golden style arrays for all 13 languages + results (10-20 lines each, every rule; produced
  by the lexers and reviewed line by line); registry order/ids/fields, 41 `lang_for_file` cases; overrides (replace,
  malformed, binary, duplicate id, new language, extension ties, html sub-languages after override); 37 malformed
  `.lng` texts (each E_SYNTAX + message, with line numbers); 1,500 mutated real `.lng` files; every lexer on random
  text with random/garbage start states and 40,000-char lines (state range, styles NULL == styles, cap at
  LEX_LINE_MAX, no overflow); convergence: 2,000 random edits per language (10,000 with `-long`) with a simulated
  30-row viewport, scrolling, partial idle work, dirty repaints, compared against a full re-lex (states and painted
  styles) plus whole-document checks every 97 edits; the 10,000-line `/*` test (re-lexes <= 60 lines in budgeted
  slices, `*/` removal converges to all 10,000 lines valid with <= 60 lines lexed); 100,000-char line through the
  Highlighter for every language; OOM in `lang_init` at every allocation (no leaks) and in `attach`/`lines_changed`.
  Mutation check: breaking any of 8 Highlighter rules (flag checks, dirty marking, probe, sentinel states) makes the
  suite fail.
- `python3 tools/lint.py` on my files: 0 findings (the full lint currently fails on T01's `src/core/ptree.cpp:50`).
- VC6 (VM A): `obj\lexer.obj lexgen.obj lexhtml.obj lexmd.obj t_lexer.obj` zero warnings at /W3; `w98 build editor
  t_lexer` -> 47,162 checks, 0 failed (same count as the Mac); verbose run 14 s.
- Full builds at the end: Mac `make -C tests coretest` (strict + ASan): 87,636 checks, 0 failed; VC6 `w98 build
  editor` stops in `src\core\doc.cpp(87) C2248 'tree_'` (T01 mid-edit), not in lexer files.

### Open issues
- Lines are lexed only up to the viewport target; brace matching across far lines (T12) gets unknown states for lines
  past `valid_upto()` unless it raises the target or uses `style_line` (which catches up only 200 lines).
- Ideas for later: CSS `#hex` colours as NUMBER, `${}` expressions in JS templates, more languages as data only
  (Pascal: `block_comment={ }` + `(* *)` + `number=pascal`; BASIC: `number=basic`, `comment_word=rem`; SQL, Lua).


## T01 Buffer: piece tree, undo, Document, marks (wave 1, agent 86box-d1-T01, 2026-11-10)
### Built
- `src/core/ptree.cpp`: `AddBuffer` (16384-entry chunk table allocated on first append, 256 KB `e_valloc` chunks that
  never move, refs split at chunk ends and `PT_PIECE_MAX`), `PieceTree` (treap with split/merge over a node pool:
  32-bit indices, index 0 = nil, free list, xorshift32 priorities with a fixed seed; subtree sums of length and
  breaks; consecutive typing coalesces into one add piece up to 64 KB), `SnapshotSource` (binary search + "same or
  next piece" fast path), `check()` (heap order, sums, caps, source ranges, unit-2 alignment, free list, byte recount),
  `dump()` (first 200 pieces).
- `src/core/esource.cpp`: `SynthSource` (pure function of offset/seed/kind, generated into a lazily allocated 64 KB
  block; formula in the file comment).
- `src/core/undo.cpp`: `UndoLog` (groups/ops/refs in three vectors, merge rule 6.6, state ops, unique state ids,
  save point incl. `set_save_point_seq`, `memory_bytes`).
- `src/core/doc.cpp`: `Document`, everything in 6.7 (lines, characters, replace/replace_text, undo/redo, encodings,
  EOL conversion, reload, reset, indexing, snapshot, listeners, `bytes()` view).
- `src/core/docbind.cpp`: `DocLines` (decodes a line with `CharReader` over `bytes()`; line 0 starts at the origin).
- `src/core/marks.cpp`: `LineMarks`, `RangeMarks` with edit adjustment.
- `tests/t_ptree.cpp t_undo.cpp t_doc.cpp t_marks.cpp`.

### Interface notes
- **Document is the only mutator.** `replace` outside `begin_edit/end_edit` makes its own UK_OTHER group (selection
  before = pos, after = pos + ins). `begin_edit/end_edit` nest (depth counted; only the outermost talks to the log).
  A group that recorded nothing leaves the history untouched (the redo side survives a failed command).
  `undo/redo` return E_STATE inside an open group or with nothing to do, E_READONLY / E_BUSY like edits.
- **Ownership:** `init_source`, `init_source_indexing`, `reset_source` always take the source, also on failure
  (it is deleted then). `reload_as_edit(src)` does NOT take it (it copies the bytes into the add buffer; caller
  deletes). `init_empty` creates its own MemSource (with the BOM bytes as original content: not modified).
- **Errors:** edits: E_READONLY (read-only), E_BUSY (busy or indexing), E_RANGE, E_TOOBIG, E_ALIGN (UTF-16:
  odd pos/ins/del, except deleting a dangling odd byte at the end), E_NOMEM. `convert`/`convert_eol` return E_TOOBIG
  for on-demand documents (= opened with `init_source_indexing`; `reset_source` keeps that mode), E_UNSUPPORTED if
  `codec_get(to)` is NULL. `reinterpret`/`reset_source` return E_BUSY while busy.
- **EditEvent:** one event per primitive change, fired after the change: `replace` = one event (removed +
  inserted). Undo/redo fire one event per recorded op (a replace group undoes as two events), flags EV_UNDO/EV_REDO.
  Encoding/break-mode changes (state ops in convert/convert_eol, reinterpret) fire `EV_RESET` with removed = inserted
  = 0 (bytes unchanged, lines renumbered); `reset_source` fires EV_RESET with removed = old length, inserted = new
  length; `finish_indexing` fires EV_INDEXED. `line` = line of pos before the edit; lines_removed/added = break units.
  Listeners may remove themselves during dispatch; don't edit the document from a listener.
- **EOL counts** use one classification in both modes (same as `detect_eol`, so `file_index` results can go
  straight into `finish_indexing`): an LF unit after a CR unit is a CRLF, other LF units are bare LFs, a CR unit not
  followed by LF is a lone CR. In CR mode breaks = cr + crlf. `eol_mixed()`: LF mode counts kinds among crlf/lf,
  CR mode among all three. Counts are adjusted from the 7.2 windows on every edit (exact; tested against recounts).
- **Break mode** is a Document flag (CR mode iff the eol passed to init/reset is EOL_CR, or after `convert_eol(EOL_CR)`).
  `set_eol_style` only changes the style for new breaks (`eol_bytes`), never the mode. `reinterpret` keeps the mode.
- **convert:** same kind+cp with a different BOM = lossless BOM swap (invalid bytes kept); otherwise decode from the
  origin, encode with 7.5 substitutions (invalid -> U+FFFD or '?', unrepresentable -> '?'), one group: delete all,
  state op, insert all. `check_convert` reports `first_bad` = byte offset of the first lossy character.
  `convert_eol` rewrites every CRLF / bare LF / lone CR that differs from the target (only the changed span is
  replaced), then a state op if style or mode changed; the whole thing is one undo step.
- **codec():** never NULL. For an encoding `codec_get` doesn't support (e.g. DBCS without a provider) the Document
  uses an internal lossless fallback (ASCII + bad markers; UTF-16 units without pairing). Check `codec_get(enc())`
  if you need to know.
- **Characters:** `char_next/char_prev/snap_char` clamp to [origin, length]; UTF-16 odd positions snap down to the
  unit. `snap_char` for self-synchronising codecs looks for the covering sequence (not `char_prev`); DBCS decodes
  forward from the line start (max 64 KB back).
- **bytes()** is a ByteSource over the live document with a one-span cache keyed on `PieceTree::stamp()` (changes on
  every mutation and every original->get), so pointers from it obey the usual "valid until the next get" rule.
- **set_saved / set_saved_seq** clear `always_modified` and break merging. `clear_undo` keeps the modified status.
- **Marks:** `LineMarks` toggle returns 1 (added) / 0 (removed); `next/prev` wrap; a pure insertion at the very start
  of a marked line moves the mark down with its text; lines inside a removed range collapse onto its first line;
  EV_RESET/EV_INDEXED drop marks past the end. `RangeMarks::set` copies, sorts, drops empty and overlapping ranges and
  keeps at most 10,000 (returns E_OK even when truncated); EV_RESET with changed bytes clears them, a pure state reset
  keeps them.
- **PieceTree for other users** (hex view, search, fileops): `span()` returns bytes to the end of the piece or of the
  original's current block; `breaks_before(pos)` counts complete break units ending at or before pos; unit-2 trees
  keep every piece except the last at an even length (misaligned ops from undo across a reinterpret take an O(n)
  rebuild path). `check()` recounts piece bytes only for documents up to 4 MB (structure is always checked).
- **Performance (Mac, strict -O2):** init 10 MB (count breaks) 3-6 ms; 100,000 single-byte inserts at random positions
  in a 10 MB document 119 ms (1.2 us/op); 1,000,000 inserts 3.9 s (3.9 us/op, 1.9 M pieces, cache misses);
  100,000 break_pos or breaks_before lookups on that fragmented tree ~30 ms (100k-op tree) / ~180 ms (1M-op tree).
  Memory: 36 bytes per piece node, ~20 bytes per undo op + 16 per ref.

### Interface changes / deviations from DESIGN
- Additions at the end of public sections (no signature changes): `AddBuffer::append_aligned/clear/swap/chunk_count/
  chunk_fill`, `PieceTree::reserve_nodes/count_refs/swap/stamp`, `UndoLog::reserve/in_group`.
- 6.5 says every unit-2 piece start and length is even; the implementation guarantees even lengths for all pieces but
  the last (odd-sized UTF-16 files), which is what keeps piece-local counts valid; source starts may be odd.
- Ownership/EOL-count/event conventions above fill gaps in 6.7 (not contradictions).

### Requests for files I don't own
- `tools/gen_make.py` (both Makefiles): add `docbind lexer lexgen lexhtml lexmd` to the `marks` suite sources and
  `-DT01_DOCBIND` to its compile flags (or to all suites; the macro only enables `test_docbind` in t_marks.cpp).
  Until then DocLines is covered only by a scratch run (19 checks pass with T04's current lexer).

### Gotchas
- `tools/lint.py`'s for-declaration regex also matches identifiers ending in "for(" followed by a type, e.g. a helper
  named `pieces_for(u32 n)`; rename such helpers.
- C++98 doesn't give nested classes access to the enclosing class's privates (clang allows it anyway); `Document::View`
  is declared a friend so VC6 is happy.
- Inside a convert group the origin can exceed length() for a moment (BOM set before the bytes are inserted); the
  character functions clamp, listeners see an EV_RESET in between.

### Tests
- Mac: `make -C tests ptree undo doc marks` strict + ASan/UBSan, all 0 failed (ptree 870, undo 91, doc 1239, marks 98
  checks). `-long` (2,000,000 ops): ptree and doc pass in strict (43 s / 48 s) and ASan (1:42 / 1:46). Combined
  coretest: every suite except T03's in-progress `search` passes (search hits an ASan stack overflow in
  t_search.cpp:644, not T01 code). `python3 tools/lint.py`: 0 findings.
- Differential random tests: tree 200,000 ops per unit (insert / remove / re-insert removed refs / reads / line
  queries, `check()` after every op), plus 20,000-op runs over a "choppy" source that returns 1-7 byte spans; document
  200,000 ops ANSI LF-mode and UTF-16 LE, 50,000 UTF-8 CR mode and UTF-16 BE (replace / multi-op groups / undo /
  redo / save / async save / clear, check() every op, bytes + exact EOL counts + modified/can_undo/can_redo vs a flat
  model with its own history). 3.9 GB SynthSource tree and document (indexing path) with edits at 2^31 and at
  EPOS_MAX (2^32 - 2). OOM injection over replace, undo, append and set_break_spec. Mutation checks (8 injected bugs)
  were all caught except one equivalent mutant.
- VC6 (VM A): `obj\esource.obj ptree.obj undo.obj doc.obj docbind.obj marks.obj` zero warnings; `t_undo` 91, `t_marks`
  98, `t_ptree` 870, `t_doc` 1239 checks, 0 failed (same counts as the Mac). t_ptree takes ~3.5 min and t_doc ~3 min on the
  emulated PII (the 200,000-op runs with a full check() per op). Full `w98 build editor` (pica.exe) links with zero
  warnings at the end of the task.

### Open issues
- No line-start cache yet (ARCH 1.3 suggests an LRU of line starts per 64 KB block): `break_pos` on a fresh 64 KB
  piece scans up to 64 KB. If T08/T17 measure line lookups as a hotspot, add the cache inside PieceTree (private).
- `convert_eol` and `convert` build the new text in memory: fine for in-memory documents (<= 16 MB), refused for
  on-demand ones.
- Undo across a `reinterpret` that changes the unit (ANSI <-> UTF-16) works but costs an O(document) rebuild per op.


## T03 Regex engine and search engine (wave 1, agent 86box-d1-T03, 2026-11-10)
### Built
- `src/core/regex_int.h` (new private header, regex/search modules and their tests only): `RxProg` (instructions,
  classes, literal prefix, required literal, first-char set, flags), opcodes, `rx_fold`/`rx_is_word`, `RxReader`
  (chunk-cached decoding over ByteSource + Codec, ASCII fast path), `RxPre` (Horspool filter), `RxMState`.
- `regex.cpp`: parser (pattern -> AST node pool -> program; errors with char positions), classes (sorted ranges +
  property bits + case closure; final bitmap for chars < 256), assertions, prefix / required-literal / first-char /
  anchoring analysis, `rx_compile` (also internal flags `RXF_LITERAL_` for Normal/Extended, `RXF_FORCEBT_` for tests).
- `rxvm.cpp`: `RxMatcher`, Pike VM (sparse-set thread lists, explicit-stack epsilon closure, captures per thread,
  leftmost-first priority). While no thread is alive it skips ahead: prefix Horspool (a pure literal returns straight
  from the filter), required literal (line-local patterns: jump to the line start of its next occurrence; none = no
  match), `^`-led patterns jump line to line, first-char set in a tight byte loop.
- `rxbt.cpp`: backtracker for backrefs/lookahead over a decoded window (32 K start positions + 64 K chars of room),
  explicit choice stack + undo trail, atomic lookahead (ECMAScript), step budget per call (default 10 M), frame/trail
  caps (1 M each) -> E_BUDGET. Uses the prefix, required literal, `^` and first-char checks per start too.
- `rxpre.cpp`: Horspool exact and keyed (case-insensitive: SBCS key table over all 256 bytes = exact; UTF-8 only the
  ASCII part, excluding letters a non-ASCII character folds to: k (U+212A), s (U+017F), i (U+0130/0131 if folded));
  UTF-16 alignment; encodability checks for `impossible()`.
- `search.cpp`: `Searcher` (Normal/Extended -> literal programs, Regex, Hex via hexfmt's parsers), forward/backward
  find, count, find_all, expand, `search_replace_all`, `search_parse_extended`.
- Tests: `tests/t_regex.cpp` (325 match cases run on both engines, 68 error cases with code + position, API, context,
  Unicode folding, 3,000 random patterns Pike vs backtracker, pathological timing, OOM), `tests/t_search.cpp`,
  `tests/mac/rxfuzz.cpp` (vs std::regex, plus Mac throughput).

### Interface notes
- **RxMatcher::search(s, c, lo, from, hi):** leftmost-first match starting in [from, hi] and ending <= hi.
  Assertions (`^ $ \b \< \> \A \z`, lookahead) see the text [lo, ctx): ctx = hi unless `set_context_end(e)` (new,
  e >= hi). So lo/hi act as the ends of the text: in-selection search matches `^`/`\A` at the selection start and
  `$`/`\z` at its end. `init(prog)` once per program; one matcher per thread; RxProg is read-only after compile.
- **RxMatch:** `pos/len` = whole match = `gpos[0]/glen[0]`; groups 1..9; unset = `EPOS_NONE`/0. Groups after the 9th
  don't capture (no error). Backrefs to an unset group fail (Perl/Boost; ECMAScript would match empty).
- **Syntax (DESIGN 11.1) plus:** inline flags `(?i) (?s) (?m)` (m: no-op) `(?-i)` anywhere, scoped to the enclosing
  group, and `(?i:...)`; `\Q...\E`; `(?#...)`; `\e \a \cX \x{...}`; `😀` pairs; `[[:word:] [:blank:]
  [:cntrl:] [:print:] [:graph:]]`; literal `{` when it isn't a valid quantifier, `]`/`}` literal; `[]a]` = `]` or a.
  `^`/`$` are line-wise for CR, LF and CRLF and never match between CR and LF; `.` excludes CR and LF; `\d` = ASCII
  0-9; `\w \b \< \>` use `uc_is_word`; `\s` = `uc_is_space`; case folding = `uc_fold` (simple folding).
- **Errors:** E_SYNTAX (malformed), E_UNSUPPORTED (lookbehind, atomic, possessive, recursion/subroutines, named
  groups/backrefs, `\p \P \Z \G \K \X`, `(?|`, `(?x)`; messages name the construct), E_TOOBIG (pattern > 1000 chars:
  pos 1000; program > 4000 instructions: pos of the outermost quantifier; repeat count > 1000; > 100 nested groups).
  `err.pos` = index into the pattern characters, `err.msg` is user-readable.
- **Searcher::find:** forward = leftmost match starting in [from, hi] (from clamped to lo; an empty match at `from` is
  returned: after an empty match the caller passes the next character for Find Next). Backward = the last match of
  the forward iteration with start < from and end <= from (assertions see up to hi). It scans blocks of 2 KB
  doubling, each block start moved to a line start, so it equals the iteration from lo except for overlapping
  matches crossing line starts. Wrap-around is the caller's.
- **count / find_all / replace_all** iterate: next search at the match end, after an empty match one character later
  (JS/Python: `a*` -> "-" on "baaac" = "-b--c-"; `^` -> "> " prefixes every line incl. an empty last one). find_all
  returns 1 when it stopped at `max` (else E_OK) and includes empty matches (len 0; filter for RangeMarks).
- **expand** replaces `out`'s contents. Regex: `$0-$9 ${n} $& $$ \0-\9 \n \r \t \\ \xHH \x{H..} \U \L \E \u \l`,
  any other `\c` = c, unset/missing group = empty. Group text is copied as raw bytes unless a case operator is active
  (undecodable bytes survive). Normal = literal; Extended = its escapes; Hex = hex bytes (`hex_parse_bytes`).
  Characters the encoding can't store become `?` and are counted in `*unrepresentable`.
- **search_replace_all** collects every match and expansion first (Cancel or > 1,000,000 matches: nothing applied,
  E_CANCEL / E_TOOBIG), then calls `sink->apply` back to front, skipping empty-for-empty edits. The caller opens one
  undo group around it; if the sink fails it returns that error with `*replaced` = edits already applied (undo the
  group).
- **Whole word** (all modes): the match must not be preceded or followed by a word character (Scintilla rule); in
  Regex mode this replaces DESIGN's `\b(?:...)\b` (see deviations).
- **impossible()** = a required prefix character (or all its case variants) can't be encoded in `doc_enc`
  (`codec_get(doc_enc)`; not checked when that returns NULL). find/count then return 0 at once.
- **Hex mode** uses T06's `hex_parse_pattern` syntax (`DE AD ?? EF`, nibble wildcards `4?`, `0x4D`, `4Dh`), compares
  under the bit masks; case/whole word ignored; the codec may be NULL. Compile errors: E_SYNTAX at pos 0 (the parser
  has no positions).
- **Progress/cancel:** `ProgressCb` gets bytes done of [lo, hi] (backward: distance already searched back from
  `from`), about every 64 KB of scanning and between matches of an iteration; nonzero -> E_CANCEL.
- **Throughput, VM A (PII-233), 1 MB of code-like text, count():** literal no-match 100 MB/s (case-insensitive and
  whole word the same), `lazy` with 11,700 matches 21 MB/s; `\bzeb\w+`, `(foo|bar)baz`, `\w+ing\b` 70 MB/s;
  `^\s*#` 18; `^\s*[{}]` 12; `[0-9]{3}` 9; `\d+` (11,700 matches) 7; `[a-z]+\(` 2.3; `a.*z` (8,750 line-long
  matches) 0.75; backtracker `(\w)\1x` 0.75 MB/s. Mac: 70-1700 MB/s. Classic pathological patterns (`(a*)*b
  (a|aa)*c (x+x+)+y (a?){30}a{30} .*.*.*= (?:a+)+b` and literal-free variants) on 100 KB: <= 0.4 s on the PII,
  <= 5 ms on the Mac; exponential backtracker cases stop with E_BUDGET in < 0.4 s on the PII (2 M budget).
  Counted repeats such as `(a{1,5}){1,5}[bc]` stay linear but run ~2 s per 100 KB on the PII (many live threads).

### Interface changes / deviations from DESIGN
1. `regex.h`: `RxMatcher::set_context_end(epos)` added (public, after `search`); private state replaced
   (`RxMState* st_` etc.), `struct RxMState;` forward declaration. `search.h`: Searcher private state replaced
   (RxMatcher member, hex bytes + masks, private helpers). No declared signature changed.
2. Whole word in Regex mode = "not preceded/followed by a word character", same as Normal mode, instead of
   `\b(?:...)\b` (they differ only for patterns that start or end with a non-word character; consistent across modes).
3. Inline flags work anywhere (scoped) and as `(?i:...)` (DESIGN: at the pattern start).
4. Extended `\xHH` / `\dNNN` are code points U+00HH (DESIGN: "character codes, encoded in the document encoding"):
   in a cp1252 document `\x80` is U+0080 (not representable), the euro sign is `€`.
5. More than 9 groups: the extra groups don't capture (no error).
6. Loops whose body can match empty (e.g. `(\w*?)*`, `(a|b?)+`) may give different captures or lengths in the Pike
   VM and the backtracker (the backtracker fails an empty iteration like ECMAScript; the VM drops the revisited
   state; Perl/PCRE use a third rule). Real-world patterns rarely hit this; the differential tests exclude it.

### Requests for files I don't own
- none.

### Gotchas
- **Trigraphs:** `"??)"` in a string or comment is a trigraph (`]`); VC6 converts it silently, clang warns. Write
  `"?\?)"`.
- The combined-coretest ODR clash T06 described also hit `search`: t_codec.cpp and t_search.cpp both had
  `ChunkSource`/`TestDbcs` (T01's "search crash in coretest"). Fixed on my side: my test classes are `Srch*`/`Rxt*`.
- `es_utf8_to_ecp` appends to the Vec (doesn't clear it).
- Win98 `clock()` ticks every 55 ms: time loops of >= 0.3 s.
- libc++ `std::regex` (rxfuzz reference) is wrong for multiline `^` (matches after non-newlines and at `first` with
  match_prev_avail), `\b` at the end of the input, and `\b` at the start of a lookahead; rxfuzz works around these.

### Tests
- Mac: `make -C tests regex search` strict + ASan/UBSan: 3,911 + 24,475 checks, 0 failed. `-long` with several seeds:
  regex 50,890 and search 167,795 checks per run, 0 failed (300,000 random patterns Pike vs backtracker, 0 differences).
  `make -C tests mac`: rxfuzz 10,000 cases vs std::regex ECMAScript|multiline, 0 mismatches (5,143 matched, 2,497
  match-list iterations compared through Searcher::find_all); 11 more seeds x 20,000 cases: 0 mismatches.
  `make -C tests` (lint 170 files 0 findings, full coretest strict + ASan): 112,926 checks, 0 failed.
- Search suite coverage: 16,320 cross-encoding comparisons (UTF-8 / UTF-16 LE+BE fixtures and T02's codecs / cp1252
  / fake DBCS, flat vs random 1-7 byte chunks with poisoned buffers), 7,560 backward positions vs the forward
  iteration (UTF-8 and UTF-16, texts up to 9,000 chars), 300 random Replace All vs a naive reference, Extended/Hex
  parsing, whole word, impossible(), expansion, cancel, E_TOOBIG at 1,000,001 matches, OOM at every allocation.
- VC6 (VM A): `obj\regex.obj rxvm.obj rxbt.obj rxpre.obj search.obj` zero warnings at /W3; `t_regex` 3,911 and
  `t_search` 24,475 checks, 0 failed.
- VC6 full tree at the end: `w98 build editor test` -> coretest 112,981 checks, 0 failed, no warnings;
  `w98 build editor` -> pica.exe links, no warnings.

### Open issues
- The Pike VM costs ~1 us per live thread per character on the PII; a lazy DFA (ARCH 8.1, "2.0") would speed up
  class-heavy patterns. Find in Files gets the literal filters for free (most no-match files are rejected by the
  prefix / required-literal scan).
- Backtracker window: matches longer than ~64 K characters aren't found, and at a truncated window end assertions
  treat it as the end of the text (DESIGN 11.1 documents the limit).
- Backward search can differ from the iteration from lo only for overlapping matches that cross a line start.
