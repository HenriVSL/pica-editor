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

## T07 Frame shell (wave 2, agent 86box-d1-T07, 2026-11-10)
### Built
- `src/win/main.cpp`: command line (15.7, one parser for our own and for a second instance's arguments; relative
  paths resolved against the sender's folder), single instance (named mutex + `FindWindowA(APP_CLASS_FRAME)`, block
  `"PICA1\0<cwd>\0<args...>\0\0"` sent with `WM_COPYDATA` / `SendMessageTimeout`, re-posted as
  `WM_APP_COPYDATA_OPEN`), settings load/save through `Ini` + `file_save` (`main_save_config_to/load_config_from`),
  `lang_init` from RCDATA (`cpp.lng`... names) + overrides `<cfg_dir>\langs\*.lng` then `<exe_dir>\langs\*.lng`
  (cfg wins), errors shown as a 15 s status note, message loop with SEH guard around dispatch AND idle work, idle
  work (`frame_idle` first, then highlighter, then `sessmgr_idle`), startup "new 1" via `docio_new` when nothing opened.
- `src/win/frame.cpp`: frame window, flat toolbar (15 buttons, image list from `IDB_TOOLBAR`, magenta mask, string-table
  tooltips, check buttons for Hex/Wrap/ShowWS, state synced from `cmd_state` on idle), layout (toolbar, tab bar,
  notification bar, view, isearch bar, results panel + 5 px splitter, status bar), splitter drag
  (`cfg.win_results_h`), title cache, MRU and Window lists rebuilt on WM_INITMENUPOPUP, exit flow, placement,
  WM_QUERYENDSESSION/ENDSESSION, drag and drop, frame commands (`cmd_view_toolbar/statusbar/results`,
  `cmd_win_*`, `cmd_tab_*`), `st_frame`.
- `src/win/tabs.cpp`: Tab lifecycle with a per-tab DocListener (labels/title/`sessmgr_doc_changed` refreshed in the
  idle slice), activation (detach old -> attach new view), owner-drawn `SysTabControl32` (close box on the active tab
  = `DrawFrameControl` caption close, middle-click close, drag reorder, right-click menu, path tooltips).
- `src/win/statusbar.cpp`: 9 parts with measured widths (separate width sets for text and hex view), texts per 15.3,
  notes, click targets (Go To, Syntax/Encoding/Line endings popups = the main-menu submenus, INS/OVR, TEXT/HEX).
- `src/win/notifybar.cpp`: per-tab notification bar (info colours, warning/info icon, text, up to 4 buttons, close box).
- `src/win/progress.cpp`: `UiProgress` modal window (after 500 ms, Cancel/Esc, frame disabled, paints pumped) and
  status-bar progress (`progress_status_*`, shown in the flags part as "Saving 20%").
- `src/win/frame_int.h`: private API of these modules (+ test hooks). `src/win/util.cpp`: unchanged from T00.
- `tools/make_icons.py`: hand-made 16-colour pixel art (grids) for the 15 toolbar buttons and the 32/16 app icon;
  `--preview` writes `docs/shots/t07_icons.png`. `res/toolbar.bmp`, `res/pica.ico` regenerated. `res/menu.rc2`: no
  change needed.

### Interface notes
- **Tabs.** `tab_new(doc)` takes ownership of `doc` **also on failure** (deletes it and returns NULL).
  `app_add_tab(t, activate)` inserts the tab bar item, attaches the document listener and calls `watch_tab_added(t)`;
  `app_remove_tab(t)` detaches the views if needed, removes the item, calls `watch_tab_removed(t)` and activates a
  neighbour. **Don't call watch_tab_added/removed yourself.** Then `tab_free(t)`. `docio_close` (T09) should do
  `sessmgr_tab_closed(t); app_remove_tab(t); tab_free(t);`. While `docio_close` returns E_NOTIMPL the frame closes
  tabs directly (`frame_close_tab`).
- **Labels/title refresh themselves** on document changes (listener -> idle). After changing tab fields that are not
  document state (`path`, `untitled`, `saving`, `save_pct`, `indexing`, `index_pct`, `lang`) call
  `app_update_tab(t)` (label, title, status). `app_on_doc_changed(t)` also works (label + `sessmgr_doc_changed`).
- **Views (T08/T10).** The frame owns showing, hiding and positioning both views. `app_activate(i)` detaches both
  views (`*_attach(w, NULL)`; save the view state into the old tab there) and attaches the new tab's view. **View
  switch (T10 `cmd_view_hex`): set `t->view`, then call `app_activate(app_active_index())`**: the frame sees the
  changed view and re-attaches. Views notify with `WM_APP_CARET` (status bar) and `WM_APP_DOCSTATE` (title, label,
  status). The status bar reads `CaretInfo` (line 0-based, col 1-based, `sel_len`, `sel_lines`, `ch`/`has_ch`);
  mode = `ci.overwrite` OR `t->tv.overwrite` (text) / `!t->hv.caret.insert` (hex). Focus moves to the active view on
  activation only when the focus was in the frame (a Find dialog keeps it).
- **isearch bar / results panel (T11).** The frame keeps the isearch window's own height (create it with its final
  height) and lays it out above the results panel when visible; the results panel gets the full width,
  `cfg.win_results_h` high, with the frame's splitter above it. **Call `app_layout()` after showing or hiding either**
  (`cmd_view_results` already does).
- **Notification bar.** `notifybar_show` replaces whatever bar the tab had; `auto_hide_ms` = 0 after show (set
  `t->notify.auto_hide_ms` afterwards for auto-hide, checked every second). A click hides the bar first, then calls
  `fn(t, kind, button_id, ctx)` from `WM_APP_NOTIFY` (posted, so the callback may close the tab); `button_id 0` = the
  close box or auto-hide. Clicks on a bar that was replaced meanwhile are ignored. `notifybar_hide(t, kind)` doesn't
  call the callback.
- **Progress.** One `UiProgress` window at a time (a nested one never shows). `progress()` throttles nothing: call it
  about every 64 KB; it pumps paint messages only. Status progress handles are > 0.
- **MRU menu** shows `cfg.mru[i]` for i < `cfg.mru_count` as `IDM_MRU_FIRST + i` (so `cmd_mru_open(i)` indexes
  `cfg.mru` directly); long paths compacted to 60 chars, `&` escaped. Window menu: first 30 tabs, check on the active.
- **Tab context menu:** `IDM_TAB_*` act on the clicked tab (`frame_ctx_tab`), else the active tab. Copy Full Path
  = CF_TEXT; Open Containing Folder = `explorer.exe /select,"path"` (logged only in selftest mode).
- Private frame message `WM_APP_TABCLOSE` (= WM_APP + 0x40, frame_int.h): posted by the tab bar to close a tab.
- `st_frame` hold mode for screenshots: if `<cfg>\st_frame.hold` exists (seconds, default 60), it first shows a demo
  (3 tabs, one modified, a notification bar) for that long, then runs the checks.

### Interface changes / deviations from DESIGN
- `tab_new` frees `doc` on failure (was a leak). No header signature changed; frame_int.h is private (rewritten).
- Tab bar is hidden when no tab is open; then both views are hidden and the frame shows the app-workspace colour.
- Selection part shows `Sel N (k lines)` for multi-line selections (DESIGN: `Sel 3 lines`); `Sel 0` when empty.
- Language overrides are read from `<cfg_dir>\langs` and `<exe_dir>\langs` (cfg first, same name wins).
- Startup without files/session opens "new 1" (`docio_new`) like Notepad++.

### Requests for files I don't own
- `src/win/tab.h` comment of `tab_new`: "takes ownership of doc (also on failure: doc is deleted, NULL returned)".
- none else required.

### Gotchas
- **TBSTYLE_FLAT toolbars are transparent**: without a rebar the frame's background shows through; the toolbar is
  subclassed to erase with COLOR_BTNFACE (the frame also paints the band above the view).
- **`w98 ps` columns are PID, PARENT, EXE**: killing the second column killed agent98 once (restarted over VNC with
  Start > Run `C:\AGENT98\agent98.exe`). Use `awk '{print $1}'`.
- `w98 run 'x.bat'` with VCVARS32 gives "Out of environment space"; use `w98 sh 'call ...VCVARS32.BAT > NUL' ...`.
- During a parallel wave the full link can fail on other tasks' in-progress files. I tested with a private exe:
  `obj\t07\` holds the HEAD (stub) versions of the other Wave 2 win files, `obj\t07\cl.rsp` / `link.rsp` link them with
  the shared objects into `obj\t07\pica.exe` (build output only, ignored by git; delete freely).

### Tests
- VC6 (VM A): `obj\main.obj frame.obj tabs.obj statusbar.obj notifybar.obj progress.obj util.obj` zero warnings at /W3.
  Full `w98 build editor` stopped in `src\win\fifworker.cpp(45,300)` (T11 mid-edit: C2664 InterlockedDecrement on a
  volatile long) and the shared `textview.obj` lacked `st_textview` (T08 mid-edit) at that moment, so the selftest ran
  on the private link above.
- `pica.exe -selftest framework frame -cfg C:\PICATEST`: `PASS st_framework (4180 checks)`, `PASS st_frame (114
  checks)`. st_frame covers: lang_init (13 languages, no errors), toolbar buttons and state vs cmd_state, 3 tabs
  (labels, modified marker, title, tooltips), activation and selection sync, status texts (lang/enc/EOL/mode/view/
  pos/sel, RO flag, read-only title), status click routing + popup submenus + hit test, drag reorder with mouse
  messages, tabbar_move, Window menu (entries, check, text), MRU menu (escape, compaction, placeholder), Next/Prev/
  wrap, Window-menu activation, Ctrl+Tab / Ctrl+Shift+Tab through the accelerator table, toolbar/status bar/results
  toggles and layout, notification bar (buttons, callback args, per-tab follow, close box, auto-hide, hide by kind,
  view below the bar), Copy Full Path (clipboard), Open Folder, middle-click close, close box, Close Others,
  every command id routed, UiProgress (500 ms, Cancel, frame disabled/enabled), status progress, settings round trip
  through file_save (placement, bars, unknown keys kept), copydata block parsing (relative/root/absolute paths,
  -n/-hex/-ro, -cfg skipped), WM_COPYDATA refused/accepted, cleanup (empty title, tab bar hidden).
- Manual (VNC + screenshots): `docs/shots/t07_frame.png` (3 tabs, modified tab with close box, notification bar,
  status parts), `docs/shots/t07_vnc_checks.png` (Encoding popup from the status bar, tab context menu, File menu with
  the empty MRU placeholder, middle-click close + path tooltip), `docs/shots/t07_icons.png` (icon art). Tab drag
  reorder over VNC works; Window menu lists tabs with a check. Second `pica.exe -cfg C:\PT07 -n5 C:\PT07\hello.txt`
  handed the file to the running instance and exited (docio stub answered "Could not open ..."). Window position and
  toolbar visibility persisted across a restart (`C:\PT07\pica.ini` [Window] x=0 y=68 toolbar=0).
- Mac: `make -C tests` -> lint 176 files 0 findings, coretest 145,550 checks, 0 failed (strict + ASan).

### Open issues
- With real docio (T09): re-check middle-click/close-box closing of modified tabs (save prompt) and the single
  instance hand-off opening a tab; with T08's view: the view top below the notification bar (st_frame checks it when
  a visible text view exists) and focus after activation.
- The status-bar popup menus are not opened by st_frame (a popup menu is modal); checked over VNC.
- No "More windows..." entry beyond 30 tabs in the Window menu (Ctrl+Tab and the tab bar still reach them).

## T09 Files and document commands (wave 2, agent 86box-d1-T09, 2026-11-10)
### Built
- `src/win/iothread.cpp`: I/O worker thread (`_beginthreadex`), FIFO of job nodes allocated at submit (the worker never
  allocates), states QUEUED -> RUNNING -> DONE -> handed to the UI thread exactly once (WM_APP_IODONE message *or* a
  synchronous wait, whichever comes first), cancel per tab, throttled progress. Worker side runs all four kinds:
  SAVE_TEMP (`file_write_temp`), BACKUP (`backup_write`), SESSION (`ab_write`), DELETE (`fs->remove`); completion of
  BACKUP/SESSION/DELETE is a stub in `io_dispatch` for T13.
- `src/win/docio.cpp`: open (13.2: full path, never twice, tab reuse of an untouched "new N", encoding overrides incl.
  the `.nfo` rule, locked-file Retry, out of memory -> offer on demand, binary -> hex view + bar, read-only bar with
  [Edit anyway], shared-RO bar, large mode, on-demand open with `largefile_begin` and a synchronous indexing fallback
  while it returns E_NOTIMPL), new, async save via the worker + `file_replace` on completion, `docio_save_wait`,
  Save As (path change, language re-detect, MRU), reload / reload-as / revert, close with prompts, `st_docio`.
- `src/win/filecmds.cpp`: File menu: Open (multi-select, filters built from the language registry), Save, Save As
  (hook template with Encoding + Line endings combos), Save All, Close / Close All / Close Others, Revert, MRU open /
  clear, Exit; `cmdstate_*`.
- `src/win/doccmds.cpp`: Document menu: Syntax (auto / plain / language, forced flag, dynamic items for override
  languages >= 13, "(Auto) - colouring off: large file" text), Encoding reload-as (5 + 10 code pages) and convert with
  the loss prompt, Line endings, Tabs, Read-only, Properties; `doccmd_offer_utf8`; radio/grey states.
- `src/win/docio_int.h` (new private header for docio/filecmds/doccmds/iothread).
- `res/dlg_file.rc2`: Save As template (230x40 dlu, Encoding and Line endings combos in two rows). `res/ids_file.h` unchanged.
- `src/core/codec.h`: `enc_cp_name` declared; `codec_register_dbcs` comment updated (the two allowed edits).
### Interface notes
- **Save path.** `docio_save` (UI thread): busy/indexing -> E_BUSY; questions first (read-only target: Clear?
  Yes/No(=E_READONLY -> caller runs Save As)/Cancel; "changed by another program since Pica read it" when the
  target's size/mtime differ from `t->disk`), then Save-As conversions (`doccmd_convert` / `doccmd_convert_eol`, one
  undo step each, loss prompt), then the attribute change, then the snapshot (`Document::snapshot`; on-demand: a
  private `FileSource` on `t->fsrc->path()`, which after the first save is the old copy, not the target). The doc is
  `set_busy(true)`, `t->saving = 1` until completion. Completion (`docio_io_done`): deletes the snapshot and the
  private reader, then `file_replace` with the same SaveResult; hooks only when `t->fsrc` reads the target itself
  (else NULL hooks, also for Save As). Success: `set_saved_seq(seq at snapshot)`, `t->disk = sr.st`, path change ->
  watch_tab_removed/added, language re-detect unless forced, MRU; then `watch_tab_saved`, `sessmgr_tab_saved`, the
  SaveDone callback. Failure: one message box (`Q_SAVE_FAILED`) saying why, what was kept (temp / old copy paths, or
  "Your file on disk is unchanged" / "Nothing was written") and that the changes are still in the editor; the doc
  stays modified. A failed in-place fallback whose target still equals the old copy byte for byte deletes the
  redundant old copy + temp and says "unchanged".
- `docio_save_wait` / `docio_wait_save(t)` (private): modal UiProgress (Cancel = cancel the tab's jobs), processes the
  completion synchronously, returns the final result. `docio_close` waits for a running save first; then asks
  "Save changes?" (Yes = `filecmd_save(t, 1)`, Save As for untitled; a failed/cancelled save keeps the tab), cancels
  and waits up to 30 s for the tab's other jobs (T13 backups), `sessmgr_tab_closed`, `watch_tab_removed`, removes,
  frees, then deletes the on-demand old copy.
- **On-demand old copy**: after a save over the source file the FileSource reads `~POxxxx.TMP`; docio keeps the
  name in a private per-tab table and deletes it on close / reload. Later saves of that tab use NULL hooks.
- **Reload** (`docio_reload(t, as, undoable)`): untitled -> `reinterpret` with the BOM of `as` if the bytes start
  with it; named -> `file_load` (forced encoding: `as`, else the encoding forced earlier for this tab, else the .nfo
  rule) then `reload_as_edit` (undoable, <= 4 MB, same encoding and break mode, not read-only; `set_saved` after) or
  `reset_source`. Caret kept by (line, char column). `doccmd_reload_as` asks `Q_RELOAD_DISCARD` for modified named
  files. `docio_revert` asks `Q_REVERT` when modified.
- **Convert** (`doccmd_convert(t, to, report)`): refused for on-demand (E_TOOBIG), busy, read-only; `check_convert`,
  then `dlg_convert_loss(label, rep, first_line)` (label like "ANSI (Windows-1252)", first_line 1-based; GOTO moves
  the caret to the first lossy char), falling back to `ui_ask(Q_CONVERT_LOSS)` OK/Cancel while T12's returns
  E_NOTIMPL. `doccmd_offer_utf8(t, msg)`: bar with [Convert to UTF-8] [Dismiss]; if msg contains "pasted" the first
  button is [Undo and convert to UTF-8] (undoes only if the undo seq is still the one at offer time).
- **Selftest helpers** (private, `docio_int.h`): `filecmd_selftest_saveas(path)` answers the next Save As dialog;
  without it the dialog is "cancelled" in selftest mode (common dialogs can't be scripted). `cmd_file_open` does
  nothing in selftest mode.
- Private question id `Q_DOCIO_CHANGED_ON_DISK = Q_USER + 1` (OK/Cancel).
- `iothread_on_done(0, id)` = progress note of a running job; `(id, 0)` = done. `iothread_flush` /
  `iothread_wait_tab` hand finished jobs over synchronously while waiting.
### Interface changes / deviations from DESIGN
- None in shared headers. New private header `src/win/docio_int.h`.
- Revert is enabled only for modified named files; Save is grey for unmodified named files (unless the file is
  missing on disk).
- Save As combo: an encoding outside the five save encodings (OEM, code pages, UTF-16 without BOM) and mixed line
  endings get a "(keep)" entry that is the default, so Save As never changes them silently.
- Extra safety prompt not in DESIGN: saving over a file whose size/mtime changed since we read it asks first.
### Requests for files I don't own
- none required. (T07: tab label `(saving)` from `t->saving`, `t->save_pct` - `app_update_tab(t)` is called on every
  change; MRU menu from `g_app.cfg.mru`.)
### Gotchas
- VC6: `InterlockedExchange/Decrement` take `LONG*`; cast `volatile` fields (`(LONG*)&j->cancel`).
- Win98/Samba: a target held open deny-write by another program makes both MoveFile and the in-place copy fail;
  `file_replace` then keeps old copy + temp (it can't know the target is intact) - docio verifies and cleans up.
- During a parallel wave the tree may not link; I linked a scratch `obj\pica9.exe` with `obj\t09.mak` (in obj\,
  ignored) to run the selftest against the current objects.
### Tests
- VC6 (VM A): `obj\iothread.obj docio.obj doccmds.obj filecmds.obj` and `obj\pica.res` (my rc2) zero warnings at
  /W3 (the RC2182 warnings in the full build come from `res\dlg_opts.rc2`, T12). Full `w98 build editor` linked
  pica.exe at the end of my run. `python3 tools/lint.py`: 0 findings.
- `pica.exe -selftest docio -cfg C:\PICATEST`: **PASS st_docio (238 checks)**; with `-fixdir M:\dev\editor\tests\tmp\docio`:
  **PASS (232 checks)** (on-demand part skipped on network folders). Covers: 11 fixture kinds (ANSI, UTF-8 with/without
  BOM, UTF-16 LE/BE, CR-only, mixed, binary -> hex view, invalid UTF-8 -> ANSI, empty, .nfo -> OEM 437) detected
  encoding/origin/EOL/view, unmodified save byte-exact, edit + save == document bytes == exact expected bytes, undo
  past the save point + close (No), reopen detects the same; Save As ANSI/CRLF -> UTF-8/LF exact bytes, source
  untouched, tab moved, MRU; loss prompt Cancel (nothing written, nothing changed) / OK ('?'); reload-as unmodified
  (no question) / modified (Cancel, OK); forced encoding kept by revert; revert OK/Cancel; untitled reinterpret with
  BOM; convert to UTF-8 BOM + undo; EOL -> LF + undo; read-only target Cancel / No (E_READONLY) / Yes (attribute
  cleared, saved); save into a missing folder (message, still modified, editable); target held deny-write by another
  handle (fails, file unchanged, no temp/old copies left); close Cancel/Yes; untitled close Yes -> Save As; new-tab
  numbering and Config defaults; tab reuse; 2 MB async save (busy, edits and a second save refused, then saved
  byte-exact); 5 MB on-demand open (indexed, line count), two saves byte-exact (hooks, then NULL hooks over the old
  copy), convert refused, old copy deleted on close. Full `pica.exe -selftest`: 8 passed, 1 failed (st_edit, T12), 4
  skipped.
- VNC on M: (2 MB file): Ctrl+S shows `big.txt * (saving)` and "Saving big.txt 37%" while the other tab takes keys;
  the file on disk = original + the typed byte; no temp files left. Screenshots checked: `docs/shots/t09_encoding_menu.png`
  (UTF-8 radio), `t09_convert_loss.png` (T12's dialog with my text: 20 characters, line 2), `t09_saveas.png` (two
  combos, defaults = document), `t09_saving.png`.
### Open issues
- Reload keeps the Document's internal in-memory/on-demand mode (`reset_source`); a file that crosses the threshold
  between open and reload keeps the old mode's convert rules until reopened (T14).
- Synchronous indexing of on-demand files until T14's `largefile_begin` exists (modal progress, Cancel closes the tab).
- T13: completion handling of BACKUP/SESSION/DELETE jobs in `io_dispatch` (iothread.cpp).

## T10 Hex view, hex edit ops, data inspector, view sync (wave 2, agent 86box-d1-T10, 2026-11-10)
### Built
- `src/core/edhex.cpp`: `hx_type_nibble`, `hx_type_char`, `hx_write`, `hx_delete` (DESIGN 9.2, 7.3): overwrite and
  insert, selection replaced first, UTF-16 whole units, BOM protection, dangling odd byte, undo grouping.
- `src/win/hexview.cpp`: the HexView window (ruler, offset / hex / char panes, colours, caret, keys, mouse, wheel,
  context menu, scroll scaling above 32767 rows, ScrollWindowEx, horizontal scroll for narrow windows), the data
  inspector strip with a clickable [LE]/[BE] toggle, modified-byte tracking, the `hexview_*` API, `cmd_view_hex` (view
  switch with selection mapping and screen-row keeping), `cmd_view_inspector`, both `cmdstate_*`, `st_hexview` and a
  screenshot demo mode.
- `tests/t_edhex.cpp`: suite `edhex` (golden table of 87 scripted cases in ANSI / OEM / UTF-8 / UTF-8 BOM / UTF-16 LE /
  LE no BOM / BE, each also undone and redone; bad markers; undo merging; 1,500 random ops per encoding with
  invariants; busy; OOM).
### Interface notes
- **State lives in the Tab.** The view keeps no private caret: `t->sel` (byte range, `sel.caret == hv.caret.pos`),
  `t->hv.caret` (pos, nibble, pane 0 hex / 1 chars, insert, unit_end) and `t->hv.top_row`. Other modules may change
  `t->sel` / `t->hv.caret` directly and then call `hexview_set_selection` (or `hexview_invalidate`); the view
  re-validates on every use (clamps to the length; if `hv.caret.pos != sel.caret` it takes `sel.caret`, nibble 0).
  The caret shape follows `t->hv.caret.insert` even when another module toggles it (editcmds' Ins command).
- **edhex return codes:** E_OK; E_READONLY / E_BUSY (not editable); **E_ACCESS = edit inside the BOM `[0, origin)`**
  (view note "The BOM is changed with Document > Encoding"); E_ALIGN = UTF-16 would need or leave an odd byte (typing at
  or after a dangling odd byte, pasting an odd number of bytes); E_RANGE = nothing to delete there (Del at the end,
  insert-mode Backspace at 0) or a character the encoding can't store; E_NOMEM. Nothing changes on failure.
- **edhex semantics:** overwrite rewrites one nibble (UTF-16: its whole unit, one replace); at the end it appends
  `n0` (UTF-16 `n0 00`). Insert: the first nibble inserts `n0` (UTF-16 a unit `n0 00` at the unit boundary at or before
  the caret), the second nibble fills the low half; UTF-16 then fills the unit's second byte (`unit_end` = end of the
  fresh unit; views reset it to 0 on any caret move). Nibble 1 always rewrites the low half (also in insert mode).
  Characters: encoded in the document codec; overwrite replaces as many bytes as the character encodes to (extending at
  the end); UTF-16 positions round down to the unit. `hx_write` = paste in the current mode (insert, or overwrite N bytes
  extending at the end), selection replaced, one UK_OTHER group. `hx_delete`: selection (UTF-16 widened to units;
  a selection starting in the BOM keeps the BOM, e.g. Select All + Del), else Del = byte/unit at the caret; Backspace in
  insert mode = the byte/unit before the caret (a half-typed byte counts as typed, so "4 Backspace" removes it);
  Backspace in overwrite mode only moves left (returns E_OK, document untouched).
- **Undo groups:** nibbles/chars are UK_HEXTYPE (merge per DESIGN 6.6: same caret, <= 2 s); consecutive Del / Backspace
  merge as UK_DELETE / UK_BACKSPACE (no clock in `hx_delete`: they merge while the caret stays put); paste and selection
  deletes are UK_OTHER. Undo across views works (one Document history).
- **View switch (`cmd_view_hex`):** text -> hex: flushes the text view (`textview_attach(NULL)`), hex selection = the
  same byte range, caret row kept at the text caret's screen row (`line_of(caret) - t->tv.top_line`) if it was visible,
  else centred. Hex -> text: caret snapped to its character start, low end down, high end up (`snap_char`/`char_next`,
  never below the origin), `t->tv.top_line` set so the caret keeps its screen row, then `textview_set_selection(...,
  SCROLL_MIN)`. Bytes never change. The view shows itself on attach and hides on `hexview_attach(w, NULL)`.
- **Modified (red) bytes:** add-buffer bytes (`PSpan.src != 0`) while `doc->modified()`; bytes appended to the add buffer
  before the last detected save are not red. A save is detected as modified -> unmodified with an unchanged
  `undo_seq` (checked after every hex edit, on paint and by a 1 s timer while attached); EV_RESET with new bytes
  (reload) resets it. Saves done while the tab was in text view are only seen as "unmodified at attach" (then every
  current byte counts as saved). Undo to the save point shows nothing red (not modified).
- **hexview_caret_info:** pos, sel_len, ch = byte at the caret (has_ch 0 at the end), overwrite, view = VIEW_HEX,
  line = row, col = column + 1.
- **Inspector:** `Config.inspector_on` (View > Data Inspector toggles it and calls `hexview_set_inspector`),
  `Config.inspector_be` (clicking [LE]/[BE]). Values via `inspect_bytes` for up to 8 bytes at the caret, "-" when short.
- **Fonts:** uses `render_fonts()->normal` when render.cpp provides one, else creates its own from Config
  (font_face / font_pt / font_px). `hexview_config_changed` re-reads fonts, colours and the inspector flag.
- **Performance (VM A, no VNC, Courier New 10, 28 rows, 1 MB document):** full repaint 3.8 ms (budget 12), one-row
  scroll (ScrollWindowEx + exposed row) 0.37 ms.
- **Selftest demo mode:** if `<cfg>\hexshow.txt` exists with "<mode> <seconds>", `st_hexview` ends by leaving a demo tab
  on screen for that long (mode 1 binary + modified bytes + selection + inspector, 2 the same in insert mode, 3 an OEM
  .nfo). Used for the screenshots; delete the file afterwards.
### Interface changes / deviations from DESIGN
- Rows = `length / 16 + 1` (the row holding the append position), not `ceil(length / 16) + 1` (which adds an empty row
  when the length is not a multiple of 16).
- E_ACCESS is the BOM-protection code of the hx_* functions (DESIGN names no code).
- The hex view is never a panel: `hexview_set_panel_handler` is a no-op.
- No declared signature changed.
### Requests for files I don't own
- `src/win/selftest.cpp` (T15): `st_keys("{LEFT}")` sends Shift+5, because VK_LEFT (0x25) equals '%' and the code
  remaps any vk equal to '+' '^' '%' '{' '}' through VkKeyScan (also hits {F12} = 0x7B '{'). Fix: do the remap only
  for the names PLUS/CARET/PERCENT/LBRACE/RBRACE (compare the name, not the vk). `st_hexview` works around it with a
  direct WM_KEYDOWN.
### Gotchas
- `ui_ansi_to_ecp` wants a NUL-terminated string.
- T07's `app_activate` detaches every view before attaching the new one, so a view must not reset per-switch state in
  its `attach(NULL)` path (cmd_view_hex's screen-row hint survives the detach).
- The Edit menu commands in the hex view are T12's (`editcmds.cpp` calls `hx_*` on `t->hv.caret`/`t->sel`, then
  `hexview_set_selection`); `hexview.cpp` has a private copy of that logic only for `st_hexview`.
- `cp -p`/restored files keep old mtimes: make won't rebuild (touch them).
### Tests
- Mac: `make -C tests edhex` strict + ASan/UBSan: 515 checks, 0 failed; mutation check (UTF-16 unit size forced to 1)
  fails 20+ golden cases. Full `make -C tests`: lint 176 files 0 findings, coretest 145,550 checks, 0 failed.
- VC6: `obj\hexview.obj`, `obj\edhex.obj` zero warnings at /W3; `t_edhex` 515 checks, 0 failed.
- `st_hexview`: PASS (75 checks) on a private build (Wave 1 commit + my two files, `tests\tmp\t10`), because the shared
  tree was mid-edit by the other Wave 2 agents.
  Later the shared tree linked (`w98 build editor`, 11:15): `pica.exe -selftest hexview` **PASS (78 checks)** with the
  real clip.cpp (hex text pastes as bytes, char-pane text pastes encoded, RawBytes with NUL paste exactly); full repaint
  3.5 ms (25 rows), line scroll 0.45 ms. (The same build printed RC2182 duplicate-control-id warnings from
  `res/dlg_opts.rc2`, T12's file.)
- Screenshots (`docs/shots/`): `t10_hex_binary.png` (binary file, modified bytes red in both panes, selection in both
  panes, caret column in the ruler, inspector), `t10_vnc_check.png` (VNC: click a byte, type `ab`, Tab, `Q`: bytes
  red, caret in the char pane, mirror byte in the hex pane), `t10_hex_nfo_oem.png` (OEM .nfo: CP437 box drawing and
  shades in the char column). `t10_hex_insert.png` (integrated build: insert mode, 2 px caret,
  inserted byte red, status bar `Offset 0x00000059 (89) | Sel 0 | Byte 0x61 97 | ... | INS | HEX`, toolbar Hex View down).
### Open issues
- Saves made while the tab shows the text view are not observed (see Modified bytes); a per-tab saved watermark in Tab
  (or a Document save counter) would make this exact.
- C0 controls in an OEM file show as '.' (hexfmt rule), not the CP437 smileys.
- The char column draws with the editor font; raster fonts (Fixedsys) can't draw CP437 graphics outside their code page.
- Ideas: offsets in decimal toggle, bytes-per-row option, editable inspector, "Paste Write" vs "Paste Insert" commands.

## T08 Text view, renderer, typing ops (wave 2, agent 86box-d1-T08, 2026-11-10)
### Built
- `src/core/edtype.cpp`: `ed_type` (insert/overwrite; Tab with "insert spaces" = spaces to the next display tab stop via
  LineLayout), `ed_newline` (doc EOL + the line's leading blanks before the caret), `ed_backspace/ed_delete` (whole
  EOLs, one bad byte/unit per char, never below the origin), `ed_delete_word`, `ed_insert_text` (CR/LF/CRLF -> doc EOL,
  unrepresentable -> '?' counted), `ed_insert_bytes`, `ed_word_left/right`, `ed_word_at`, `ed_smart_home`,
  `ed_lang_from`.
- `tests/t_edtype.cpp` (suite `edtype`): 63 golden cases x 6 document kinds (ANSI / UTF-8 BOM / UTF-16 LE BOM x CRLF /
  LF) incl. undo + redo of each, tab stops, errors, bad bytes, lone CR, combining marks, CR mode, BOM protection,
  mixed EOLs, undo grouping, `ed_lang_from`, and a differential random test against a code-point model (1,200 ops
  per kind, 6,000 with `-long`).
- `src/win/render.cpp` + new private `src/win/render_int.h`: fonts (normal, bold, CJK found with EnumFontFamiliesEx,
  Courier New fallback for raster fonts), colour tables (Appendix B, system colours), `render_run` / `render_fill` with
  a per-DC state cache.
- `src/win/textview.cpp`: the TextView class (main view and results panel), painting, caret, keyboard, IME, mouse,
  wheel, context menu, scrolling, wrap, long lines, indexing placeholder, DocListener, `textview_*` API,
  `cmd_view_wrap/showws/linenum` + states, `st_textview` (+ a screenshot demo mode).
- `tools/gen_make.py` (+ both Makefiles regenerated): T01's request (marks suite links `docbind lexer lexgen lexhtml
  lexmd`; `-DT01_DOCBIND` added to the test flags: Mac `CPPFLAGS`, VC6 `TCFLAGS`), and `layout` added to the `ed`
  group (edtype/edlines/edhex suites) because `ed_type` measures tab stops with LineLayout.

### Interface notes
- **The view keeps no document state.** Selection = `Tab::sel`, scroll/wrap/overwrite/desired column = `Tab::tv`
  (`hscroll` in cells, `desired_cell` 0xFFFFFFFF = unset). `textview_attach(w, NULL)` is therefore a valid "flush";
  attaching shows the main view (`SW_SHOW`), detaching hides it (results panel: never hidden by attach).
- **Tab::tv is the effective per-tab setting** for tab width / insert spaces: the view builds `EdCtx` with
  `ed_lang_from(t->lang, ...)` for comment tokens/braces and then overrides `tab_width/use_spaces` with `t->tv`. So
  whoever sets `t->lang` should copy the language defaults into `t->tv` (see requests). `ed_lang_from` itself applies
  `LangInfo.tab_width` (non-zero) and `use_tabs` (0/1) over the passed values; `braces` defaults to `()[]{}` with no
  language.
- After changing `Tab::tv` (wrap, tab width, overwrite), `t->lang`/highlighter, or fonts/colours: call
  `textview_invalidate(g_app.textview)` (one view) or `textview_config_changed()` (re-inits fonts from Config if
  face/pt/px changed, refreshes colours, invalidates every text view incl. the results panel). View toggles:
  `cmd_view_showws/linenum` change `g_app.cfg.show_ws/line_numbers` (global), `cmd_view_wrap` the active tab's
  `tv.wrap`.
- `textview_set_selection(w, a, c, mode)`: clamps to [origin, length] and snaps to char starts. SCROLL_MIN = minimal
  scroll; SCROLL_CENTER = centre only if the caret is not already fully visible; SCROLL_NONE = no scroll.
- Edits made elsewhere (commands, Replace All, undo in T12) shift `Tab::sel` like marks while the tab is attached;
  commands that know the new selection should still call `textview_set_selection` afterwards.
- Frame notifications are coalesced through a private posted message: `WM_APP_CARET` (caret/selection),
  `app_on_doc_changed(t)` + `WM_APP_DOCSTATE` (when `modified()` flips; also on Ins). The results panel sends none.
- `textview_caret_info`: `col` = display cell + 1 (tabs, wide chars, control pictures count); `sel_lines` does not
  count a last line the selection only touches at its start; `has_ch` = 0 at the EOL.
- Brace match: one call `ed_match_brace(d, caret, hl, braces, &b, &m)` (T12 checks the char before the caret, then
  the one at it): rc 1 = matched pair, 0 = unmatched brace (BRACE_BAD_FG), E_NOTFOUND / other errors = nothing.
  Off in large mode (DESIGN 8.6 says "limited to 2,000 lines"; the signature has no limit) and with a selection.
- Highlighter: `set_target(last visible line)` on every paint; styles via `style_line` per painted line (first
  LEX_LINE_MAX chars, only when the segment starts at the line start); `take_dirty` after each paint and from a 30 ms
  view timer that also runs `work()` while lexing is pending (main.cpp's idle work needs no change).
- **Render API conventions** (T10 uses it too): `render_run` caches font/colours per DC (4 DCs, CS_OWNDC). If you call
  SelectObject/SetTextColor/SetBkColor on such a DC yourself, call `render_fill(dc, NULL, 0)` first (NULL rect =
  drop the cache). `RunAttr.font`: 0 normal, 1 CJK, 2 = Courier New fallback (raster fonts; `RF_*` in render_int.h).
  `box` draws a 1 px outline inside the cell rect. `bold_ok` = bold has the same average width and height (Courier New
  10 yes, Fixedsys no); bold and non-ASCII runs are drawn with lpDx so glyphs stay on the grid. Default font: Config
  `font_face/font_pt/font_px` (Terminal gets OEM_CHARSET, others DEFAULT_CHARSET); `render_init` is called by the
  first `textview_create` and by `textview_config_changed`.
- Painting: one ExtTextOutW per run of equal attributes, ETO_OPAQUE on the run's cells, gutter/gap/rest of row filled
  separately: no flicker, no back buffer. Combining marks are drawn over their base (base advance moved to the mark).
  Wide chars: CJK font if found, else a drawable WGL4 glyph with a 2-cell dx, else a boxed `?` (VM A has no CJK font).
- Long lines (> 64 KB): a LongLineIndex per line (LRU of 8, built with UiProgress above 4 MB, outside WM_PAINT only;
  dropped by any edit at or before the line end); the first window from the line start needs no index. The horizontal
  range grows with the widest line laid out (index total for long lines). Wrap on long lines = fixed cell rows.
- Line lookups: the view keeps its own cache of line starts/ends around the viewport, found by scanning bytes from one
  known start and updated in place for edits that don't change the line count, so a paint costs one tree lookup.
- Indexing (on-demand) documents: first screen drawn by scanning from the origin, no numbers, no caret, no scrolling.
- Keys (8.3): Left/Right step over a whole CRLF; Left/Right with a selection collapse to its end; Home = smart home (in
  wrap mode first to the display row start); End in wrap mode goes to the row end first (caret shown at the end of that
  row), again to the line end; PgUp/PgDn move caret and view by a screen minus one row; Ctrl+Up/Down scroll; Tab with
  a multi-line selection and Shift+Tab call T12's `ed_indent`; Ins toggles `tv.overwrite`; Esc clears Mark All marks.
  WM_CHAR: bytes from `winenc_input_cp()`, DBCS lead bytes buffered, Ctrl without Alt ignored (AltGr types).
  WM_IME_CHAR handled directly; the composition window follows the caret while composing.
- Mouse: click/shift-click/drag with autoscroll (50 ms timer, speed by distance), double-click word (drag extends by
  words), triple-click line, gutter click/drag selects lines, marker cell toggles a bookmark, wheel =
  SPI_GETWHEELSCROLLLINES, right-click moves the caret when outside the selection, context menu per 8.4 (panel: Copy,
  Select All). Panel: Enter / double-click -> `PanelHandler::panel_open(pos)`, keys go to `panel_key` first.

### Interface changes / deviations from DESIGN
- New private header `src/win/render_int.h` (render module): `render_init_from_config`, `render_glyph_font`,
  `render_is_raster`, `render_refresh_colors`, `render_font_desc`, `RF_*`. No declared signature changed.
- `RunAttr.font = 2` (fallback font for raster fonts) is an extra value of the existing field.
- `render_fill(dc, NULL, bg)` drops the DC cache (convention, see above).
- Brace matching off in large mode (no line limit parameter in `ed_match_brace`).
- `ed_type('\t')` honours `EdLang.use_spaces` (spaces to the next tab stop); edtype links `layout`.

### Requests for files I don't own
- T07/T09 (wherever `t->lang` is set: open, Save As re-detect, Document > Syntax, session restore): when
  `g_app.cfg.use_lang_tabs`, copy the language defaults into the tab: `if (lang->tab_width) t->tv.tab_width =
  lang->tab_width; if (lang->use_tabs != 2) t->tv.use_spaces = !lang->use_tabs;`, then
  `textview_invalidate(g_app.textview)` if the tab is active (the view takes tab width / insert spaces from `Tab::tv`).
- T09 (Document > Tabs commands) and T12 (`cmd_edit_overwrite`): after changing `t->tv.tab_width/use_spaces/
  overwrite`, call `textview_invalidate(g_app.textview)` (repaint + caret shape).
- T17 / overseer: add the line-start cache inside PieceTree (T01's open issue). Measured on VM A: 1000 random
  `Document::line_start` calls = 225 ms (225 us each) and 1000 `line_of` = 109 ms on a fresh 93 KB, 3000-line
  document. The text view avoids them (own cache), but commands (go to line, brace match, line ops, search) pay ~0.2 ms
  per lookup.

### Gotchas
- `make` doesn't track flag changes: after the `-DT01_DOCBIND` change, stale `t_marks.o` objects had to be deleted
  (`rm -rf tests/build/su_marks`, `tests/build/{strict,asan}/t_marks.o`) before the DocLines test ran (98 -> 116 checks).
- UiProgress pumps paint messages: never build an index (or anything with progress) inside WM_PAINT; the view paints
  a plain background while `building` and repaints afterwards.
- Courier New on Win98 has no combining-mark glyphs for some marks (U+030A shows a box); U+0301 is drawn over the base.
- WM_KEYDOWN for keys the accelerator table owns never reaches the view (Ctrl+Shift+Up/Down, Shift+Del, Ctrl+Ins...).

### Tests
- Mac: `make -C tests edtype` strict + ASan/UBSan: 19,989 checks, 0 failed (mutation check: removing the whole-EOL
  Backspace rule fails 30+ cases). `make -C tests marks`: 116 checks (DocLines test now included). Full `make -C tests`
  (lint 176 files 0 findings, coretest strict + ASan): 145,550 checks, 0 failed.
- VC6 (VM A): `obj\render.obj`, `obj\textview.obj`, `obj\edtype.obj` zero warnings at /W3; `t_edtype` 19,989 checks,
  `t_marks` 116 checks, 0 failed. Full `w98 build editor` at the end: pica.exe links, zero warnings
  (the tree was green at 11:24).
- Selftest `pica.exe -selftest textview -cfg C:\PICATEST`: `PASS st_textview (56 checks)` (caret/typing/selection/
  undo/overwrite/word keys/auto-indent/desired column, mouse click/double/triple/gutter/bookmark, wrap with End and
  Down, OEM unrepresentable typing refused + e-acute accepted, UTF-8 caret over multibyte/bad/wide chars, timings,
  5 MB line). An earlier run on a privately linked exe (T11 files mid-edit) gave the same result.
- Timings on VM A (no VNC, Courier New 10, 26 visible rows, 3000-line C file, C++ colouring): full repaint 5.6 ms (budget 12), keystroke
  0.7 ms (edit + undo record + relex + row repaint + caret; 1.6 ms including the frame's status/title update; budget 3),
  1-line scroll 2.0 ms, page scroll 5.6 ms, End on a 5 MB single line 1.44 s with the progress window (index build,
  once), Home back 22 ms.
- Screenshots (`docs/shots/`): `t08_c_file.png` (C file coloured), `t08_wrap.png`, `t08_whitespace.png` (CRLF / LF /
  lone CR, tabs, control pictures), `t08_utf8.png` (Cyrillic, Polish, invalid bytes, CJK placeholder, combining,
  emoji, C1), `t08_fixedsys.png`, `t08_selection.png` (multi-line selection with EOL cells, Mark All, bookmarks),
  `t08_vnc_drag.png` (VNC: typed text coloured live, drag selection with "Sel 311 (4 lines)"), `t08_vnc_menu.png`
  (context menu). VNC also checked: click, typing, Enter with indent, double-click word, right-click caret move.

### Open issues
- No back-buffer fallback needed so far (no flicker seen); ScrollWindowEx is used for line scrolls and small
  horizontal scrolls, full repaint for jumps.
- Word wrap uses T06's simple break rules; long lines (> 64 KB) wrap at fixed cell counts.
- IME is best effort (no DBCS code page on VM A to test); the composition window is positioned, the font is not set.
- Unfocused selection uses the same highlight colours as focused.

## T11 Search UI: Find/Replace dialog, incremental search, find in files (wave 2, agent 86box-d1-T11, 2026-11-10)
### Built
- `src/core/fif.cpp`: masks (`;`/space/`,` separated, `!x` excludes files, `!\dir` / `!/dir` excludes folders),
  sorted depth-first walk (files first, case-insensitive name order, hidden/system folders skipped), per-file read
  (whole up to 16 MB, else a 4 x 256 KB block cache over the open handle: matches across "windows" are found),
  binary skip (NUL in the first 4 KB unless UTF-16), detect on the first 64 KB, one compiled Searcher per encoding,
  line-based hits (`fif_search_source`), max hits, cancel flag + on_progress/on_hit stop, on_skip for unreadable files.
- `src/core/fif_int.h` (new private header): `fif_test_limits` (tests force the block-cache path), `fif_dir_excluded`.
- `tests/t_fif.cpp`: suite `fif` (see Tests).
- `src/win/fifworker.cpp`: worker thread (`_beginthreadex`, below-normal priority) running `fif_run(fs_os())`, batches
  of up to 50 hits / 150 ms posted as `WM_APP_FIF`; run ids drop batches of a stopped or replaced run; ref-counted
  thread context so a run that doesn't stop within 2 s is abandoned safely; at most 48 batches in flight
  (backpressure); final header with counts, cancelled / stopped at N hits / folder not found / N unreadable files /
  "(N open files have unsaved changes; disk versions searched)".
- `src/win/fifpanel.cpp`: results panel (child of the frame: caption strip with status, [Stop], [x]; a panel
  TextView on a read-only UTF-8 Document with `lang_results()`); hit table, RangeMarks highlights, F4 / Shift+F4
  (wrap with a note), Enter / double-click open (a file line opens its first hit), Esc stops a running search or
  closes the panel, Ctrl+C copies the selected lines (`clip_copy` over whole lines). `st_fif`.
- `src/win/dlgfind.cpp`: modeless Find / Replace / Find in Files dialog (one template, controls placed per tab in
  code, dialog resized per tab), search state for F3 without the dialog, all `cmd_search_*` except isearch / fif /
  results, `st_search`.
- `src/win/isearch.cpp`: incremental search bar (Ctrl+I).
- `src/win/fifworker_int.h` (new private header) for the four win modules.
- `res/dlg_find.rc2`, `res/ids_find.h` (control ids 2101-2163, token menu ids 2200-2290).

### Interface notes
- **Hit semantics (FifHit):** `line`, `col` 0-based (col in characters); `pos`/`len` (new) = match bytes in the file,
  BOM included, so for an unmodified document they are Document offsets. `text` = UTF-8, <= 256 characters: the whole
  line if the match starts within the first 200 characters, else a window starting 40 characters before the match
  with "..." (and "..." at a cut end); CR/LF/NUL inside become U+FFFD; `hl_*` clipped to the text. Lines end at LF
  (CRLF = one break; a lone CR is shown, not a break) or, in CR mode (detect says EOL_CR), at CR, matching the
  Document's line numbering. Empty matches are hits (regex `^` lists every line); after an empty match the next
  search starts one character later.
- **fif_run results:** E_OK, E_CANCEL, E_TOOBIG (stopped at max_hits, default 50,000), E_NOTFOUND (folder missing or
  not a folder), pattern errors (validated once before the walk), E_NOMEM. `on_done` is called exactly once in every
  case. `searched` counts every file opened (binary files included); unreadable files go to `on_skip` (not counted).
- **Find All in a document** = `fif_search_source(doc->bytes(), ...)` into the same panel; header
  `Search "pat" in name (N hits)` (starts with "Search " so the results lexer styles it; DESIGN wrote `"pat" in name:`).
  Its hits remember the tab id and `change_seq`: they open exactly while the document is unchanged, else by line/col.
  FiF hits open exactly while the target document is unmodified, else by line/col (approximation).
- **Dialog behaviour:** Find Next honours the direction radio; F3 / Shift+F3 are always next / previous (also while the
  dialog has focus, through IDA_FIND). "In selection": Count / Mark All / Find All / Replace All use the current
  selection; Find Next / Replace keep a scope (the selection the search started from, starting at its start; it lives
  while the selection is our last match and the document didn't change otherwise; our own replacements adjust it).
  Replace replaces the selection only if it is exactly a match (an empty match counts), then finds the next; it never
  replaces the same empty match twice. Replace All = one undo group (`UK_OTHER`); a failing edit undoes the group.
  Replace in all tabs asks `Q_REPLACE_ALL_TABS` once, compiles per document encoding, skips read-only / busy /
  indexing / saving tabs, status `N replaced in K of M documents; S skipped - a.c: 3, b.h: 2`.
- Live validation 200 ms after an edit (and on mode changes): compile errors in red: `Regex error at character N:
  <T03 message>`, `Pattern error at character N: ...` (Extended), `Invalid hex pattern: ...`. Find buttons stay enabled
  and show the error. Not found: status text + `MessageBeep(MB_OK)`, never a box. Status goes to the dialog's line
  while it is visible, else to `app_status_note`.
- Prefill: single-line selection or the word at the caret (`ed_word_at`); a multi-line selection ticks In selection;
  in the hex view a selection of <= 64 bytes is put in as hex bytes and the mode switches to Hex bytes. The combo is
  ANSI: characters the code page can't hold show as '?' but the real text is searched as long as the field isn't
  edited (same for Ctrl+F3). Hex bytes radio is enabled only in the hex view (or when it is the current mode).
- `>` buttons (Regex mode only): token menus for find and replace + "Regular expression help..." (IDM_HELP_REGEX).
  `...` = SHBrowseForFolder starting at the folder in the field. FiF defaults: folder = the active file's folder (each
  Ctrl+Shift+F), masks = last used, else `*.ext` list from the active tab's language, else `*.*`.
- Options and histories live in `g_app.cfg` (find_* fields, find_hist / repl_hist / fif_dirs / fif_masks,
  win_find_x/y = the user's position; the automatic move away from a match is not remembered).
- Auto-move estimates the match row from `render_fonts()->ch` and `Tab::tv.top_line` / `hv.top_row` (assumes
  centred when the line is off the view; inexact with word wrap).
- Incremental bar: searches literally from where it opened (sel_lo at Ctrl+I), wraps; Enter/F3 next, Shift+Enter /
  Shift+F3 previous (cmd_search_next/prev route to the bar while its edit has the focus), Ctrl+I in the bar = next,
  Esc closes (selection kept, its highlights removed). "i/N" or "N found" and Highlight all only for documents
  <= 1 MB. Not found: `UIC_ISEARCH_FAIL` background + beep. Height 26 px (frame uses the window height).

### Interface changes / deviations from DESIGN
- `fif.h`: `FifHit` gained `epos pos, len` at the end; `FifSink` gained `virtual void on_skip(const char*, int)` (default
  no-op) at the end; new function `fif_search_source(...)` (Find All in open documents uses it). No signature changed.
- Find All header text (above); hits on the same line share one result line (several highlights); F4 walks hits, not
  lines. Highlights: the first 10,000 hits (RangeMarks limit).
- Files larger than 16 MB are searched through a block cache instead of "1 MB windows overlapping by 64 KB" (same
  memory bound, no missed matches at window edges).

### Requests for files I don't own
- `src/win/fifworker.h` (overseer) + `src/win/main.cpp` (T07), `app_dispatch` before `TranslateAccelerator(IDA_MAIN)`:
  declare `int isearch_translate(MSG* m); int results_translate(MSG* m);` (both implemented) and call
  `if (isearch_translate(m) || results_translate(m)) return;`. Without it Ctrl+C/V/X/Z/A typed in the incremental
  search box act on the document, and Ctrl+C / Ctrl+A in the results panel copy from / select the active document.
- `src/win/frame.cpp` (T07) `frame_close`: call `fif_stop()` before DestroyWindow (a running search's batches are
  freed by the sender when PostMessage fails, so this is cosmetic).
- T15 `st_wait_idle`: also wait while `fif_running()`.
- T08 / overseer (optional): `int textview_pos_rect(HWND, epos, RECT* screen)` would let the Find dialog's auto-move be
  exact (now estimated from line height and top line).

### Gotchas
- VC6's `InterlockedIncrement/Exchange` take `LONG*`: volatile counters need a cast.
- The tab control comes last in the dialog template (bottom of the z-order, WS_CLIPSIBLINGS) or it paints over the
  controls. Hidden controls are also disabled so their mnemonics don't fire.
- tests/tmp persists between runs: fixture trees are rewritten with fixed names every run (`fif1`, `fif2`).

### Tests
- Mac: `make -C tests fif` strict + ASan/UBSan: 7,459 checks, 0 failed; `-long` seeds 2, 3, 99: 36,259 checks each,
  0 failed. Covers masks (25 cases + folder excludes), a fixture tree (order, hidden/system folders via a wrapping
  FileSys, binary skip, ANSI/UTF-8 BOM/UTF-16 LE+BE/CR mode/empty files, NUL after 4 KB), exact hit fields, whole
  word, case, regex anchors, empty matches, Extended CRLF, multi-line matches, non-ASCII patterns per encoding, max
  hits (also inside a file), cancel/stop, missing folder, bad/unsupported patterns, long-line windows and clipping,
  random texts vs a reference (UTF-8, UTF-16 LE/BE, 1-7 byte chunked spans), the block-cache path vs in-memory, OOM at
  every allocation (no leaks). Full `make -C tests` (lint + coretest strict + ASan): 145,550 checks, 0 failed.
- `python3 tools/lint.py` on all T11 files: 0 findings.
- VC6 (VM A): `obj\fif.obj fifworker.obj fifpanel.obj dlgfind.obj isearch.obj` zero warnings at /W3; `t_fif` 7,459
  checks, 0 failed on C:\PICATEST\tmp and on M:\dev\editor\tests\tmp. Full `w98 build editor` links pica.exe (the only
  warnings were RC2182 in T12's in-progress `res/dlg_opts.rc2`).
- Selftests: `pica.exe -selftest search fif frame -cfg C:\PICATEST`: PASS st_search (60 checks), PASS st_fif (29),
  PASS st_frame (117, T07's, uses the panel); `-selftest fif -fixdir M:\dev\editor\tests\tmp\stfix`: PASS. st_fif
  also opened a closed file through T09's docio_open.
- VNC (pica on fif.cpp): Find Next + auto-move away from the match, Find All into the panel with highlights, red live
  regex error, Replace tab, token menu, Find in Files over `M:\dev\editor\src` (44 hits in 20 files, < 2 s), over
  `M:\dev` with progress in the panel caption ("Searching... 236 files ...") while the dialog stayed responsive, Stop
  -> "cancelled"; incremental bar found (1/3), not found (pink), Highlight all (Alt+H). Screenshots in `docs/shots/`:
  `t11_find_tab, t11_find_all, t11_regex_error, t11_replace_tab, t11_token_menu, t11_fif_running (FiF tab +
  results), t11_fif_tab_results, t11_fif_progress, t11_fif_cancelled, t11_isearch_found, t11_isearch_notfound,
  t11_isearch_highlight`.

### Open issues
- The two `*_translate` hooks above are not called yet (main.cpp): editing keys in the incremental search box and
  Ctrl+C in the results panel go to the main accelerators until then.
- Dialog auto-move is an estimate (line height x rows from the top line); with word wrap or a scrolled view it may
  still cover the match. The dialog keeps its keyboard focus; Ctrl+F / Ctrl+H inside the dialog don't switch tabs
  (IsDialogMessage eats them); click the tabs or use the menu.
- The regex token menu has 26 entries (tall on 800x600); could be split into submenus.
- Find All over a document > 50,000 hits stops at the limit (same as FiF). Highlights cover the first 10,000 hits.
- FiF opens a hit exactly only while the target document is unmodified; after edits it goes by line/column.
- Results of a document closed after Find All can't be opened (status note).

## T12 Edit commands, clipboard, line ops, Go To, Options, misc dialogs (wave 2, agent 86box-d1-T12, 2026-11-10)
### Built
- `src/core/edlines.cpp`: `ed_duplicate`, `ed_delete_lines`, `ed_move_lines`, `ed_toggle_comment`, `ed_change_case`,
  `ed_indent`, `ed_match_brace`; plus `ed_lines_touched/ed_lines_range` and display-column helpers `ed_col_of` /
  `ed_pos_of_col` (new private header `src/core/edlines_int.h`, also used by editcmds/dlggoto).
- `tests/t_edlines.cpp` (suite `edlines`): golden before/after tables (text + selection) for every command in six
  variants (ANSI, UTF-8, UTF-16 LE with BOM x CRLF, LF), undo/redo round trip per case, brace matching with a real
  Highlighter, columns, read-only/busy refusal, out-of-memory rollback at every allocation, random move down/up.
- `src/win/clip.cpp`: clipboard (RawBytes + CF_UNICODETEXT + CF_TEXT on copy; RawBytes > Unicode > ANSI on paste).
- `src/win/editcmds.cpp`: all `cmd_edit_*`, `cmd_line_*`, `cmd_bm_*`, `cmd_search_goto/brace`, `cmd_tools_options`,
  their `cmdstate_*`, extra `cmd_line_cut` (Ctrl+L), and `st_edit`.
- `src/win/dlggoto.cpp` (Go To, text + hex mode, live preview, red errors), `src/win/dlgopts.cpp` (Options sheet, 5
  pages), `src/win/dlgmisc.cpp` (exit prompt, convert-loss prompt, Properties, About, `st_dialogs`), private header
  `src/win/dialogs_int.h` (eval/modeless twins for the selftest).
- `res/dlg_goto.rc2`, `res/dlg_opts.rc2`, `res/dlg_misc.rc2`, `res/ids_opts.h`, `res/ids_misc.h`: real templates.

### Interface notes
- **Line commands = Notepad++ (Scintilla) exactly** (checked against Scintilla `Editor.cxx` and Notepad++
  `doBlockComment` sources):
  - Ctrl+D = SCI_SELECTIONDUPLICATE: selection duplicated right after itself and stays selected; no selection = the
    caret line copied below (EOL + text inserted at the line end), caret unchanged. Empty doc -> one more line.
  - Ctrl+Shift+Up/Down = MoveSelectedLines: whole touched lines move; afterwards the **moved lines are selected**
    (caret at their start, anchor after their EOL) even if nothing was selected before - that is what Scintilla
    does. Last line without EOL, empty last line and "block just above the last line" follow Scintilla's EOL moves.
  - Ctrl+L = SCI_LINECUT (VC6 too): touched lines go to the clipboard, then are deleted. Ctrl+Shift+L = LINEDELETE
    (no clipboard). Deleting the last line keeps the EOL before it (an empty last line remains), caret at col 0.
  - Ctrl+Q = Notepad++ toggle single-line comment: each touched non-blank line is toggled on its own; `tok` or
    `tok ` right after the indentation is removed (case-insensitive, so `rem` works for BAT), otherwise `tok ` is
    inserted at the line's indentation; selection adjusted with N++'s rules. Block-only languages (CSS, HTML, XML,
    Markdown) wrap each line `/* line */` / `<!-- line -->` ("advanced mode"); no syntax -> E_UNSUPPORTED (status
    note). Pica extra: a word token (REM) does not match the start of a longer word (REMARK).
  - "Touched lines": a non-empty selection ending at column 0 of a line does not touch that line (N++ rule for
    comment/move; used for delete/cut/indent too).
  - Indent/Unindent (Tab/Shift+Tab on lines, Edit menu) = Scintilla Indent: indentation in columns +- tab width,
    rewritten as tabs+spaces (or spaces); empty lines are not indented; multi-line selections end up as whole
    lines (Scintilla's caret/anchor rule). Single line: text positions shift, positions inside the old indentation
    go to the end of the new one.
  - Ctrl+U / Ctrl+Shift+U: selection kept (higher end moves by the byte-size difference); without a selection the
    char at the caret changes and the caret moves past it (VC6). Chars the encoding can't store, and bad markers,
    stay. Only the differing bytes are replaced (UTF-16: whole units).
- **`ed_match_brace` returns 1 (matched: *brace, *match), 0 (brace without partner: *match = EPOS_NONE),
  E_NOTFOUND (no brace before/at pos)**. Priority: the char before pos, then the char at pos (N++). Only the same
  brace kind is counted (Scintilla). Braces styled ST_COMMENT/ST_STRING/ST_CHAR are ignored at the caret and while
  scanning; far lines are lexed on demand (`set_target` + `work`, <= 40,000 lines). Scan limit 20,000 lines each way;
  lines > 1 MB use a 1 MB window without styles; lines without either brace byte are skipped by a raw-byte prefilter.
  Ctrl+B moves the caret to the match position (N++ GOTOPOS).
- **Selection contract (views):** commands read `Tab::sel`, then call `textview_set_selection` /
  `hexview_set_selection` (and set `t->sel` themselves too). Views must keep `Tab::sel` current. The hex view caret
  (`t->hv.caret.pos`) is set to the new caret before `hexview_set_selection`.
- **Clipboard (`clip.h`):** `clip_copy(t, sel, hex_pane)`: `hex_pane` nonzero = hex text ("4D 5A 90", 16 per line);
  0 = decoded text (text view and the hex char pane). Text formats use CRLF line breaks (LF/CR documents too, so
  Win98 Notepad shows lines) and U+FFFD for bad bytes; RawBytes = header (`"PRB1"`, size, EncSpec) + exact bytes.
  E_NOTFOUND for an empty selection, E_TOOBIG > 16 MB, E_BUSY if the clipboard stays locked (20 x 10 ms retries).
  `clip_get`: RawBytes (has_raw, enc, raw + decoded text) > CF_UNICODETEXT (surrogate pairs joined, lone ones ->
  U+FFFD, stops at NUL) > CF_TEXT (system ACP via winenc). Text paste: same-encoding RawBytes go in byte-exact
  (`ed_insert_bytes`; E_ALIGN falls back to text); otherwise text with bad markers -> U+FFFD, EOLs normalised by
  `ed_insert_text`; '?' replacements -> `doccmd_offer_utf8(t, "N pasted characters became '?' (not in X).")`.
  Hex paste: RawBytes, else hex text in the hex pane (`hex_parse_bytes`), else the text encoded (`?` subst), via
  `hx_write` in the current mode.
- **Dialogs:** `dlg_goto` (1 moved). Text mode: `goto_parse_line`, the column is a **display column** (tabs/wide chars,
  same as the status bar "Col"), past the line end -> line end. Hex: `goto_parse_offset`, preview
  `= 0x00001F40 (8,000) of 81,920 bytes`. `dlg_options(page)` (1 = something applied); each page has a grey
  explanation line per option (IDC_OPT_HINT). OK/Apply commit **once** (dirty flag) by copying every Config field
  before `win_x`, then `app_save_config()` + `app_apply_config()`; tabs whose tab width / insert-spaces equal the old
  global value follow the new one. `dlg_exit_prompt` returns EXIT_SAVE (save_flags[i] = ticked; unticked ones are
  to be discarded), EXIT_KEEP (only offered when `backup_enabled`), EXIT_DISCARD, EXIT_CANCEL; it lists exactly the
  tabs passed (the caller filters untitled ones per 15.9); on-demand tabs get "(not kept: too large)" when backups
  are on. `dlg_convert_loss(label, rep, first_line)`: first_line is **1-based** (as doccmds passes it).
  Properties shows path, size (+ size on disk if different), dates, encoding, EOL counts + mixed + style, lines,
  language, state. About: name/version/build date/"all our own code"/cfg_dir.
- **Selftest answers:** exit prompt `Q_EXIT` (IDYES Save Selected, IDNO Discard, IDRETRY Keep, IDCANCEL Cancel);
  convert loss `Q_CONVERT_LOSS` (IDYES/IDOK Convert, IDNO/IDRETRY Go to First, IDCANCEL). The real dialogs open and
  are answered from the queue. `<cfg>\st_shots.txt` present = st_dialogs keeps every dialog up 7 s (screenshots).

### Interface changes / deviations from DESIGN
- No declared signature changed. New private headers `src/core/edlines_int.h`, `src/win/dialogs_int.h`; new exported
  function `void cmd_line_cut(void)` in editcmds.cpp (not yet in cmds.h, see requests).
- **Ctrl+L cuts** the line(s) to the clipboard (Notepad++ SCI_LINECUT / VC6 LineCut) instead of only deleting
  (DESIGN 15.5 maps Ctrl+L and Ctrl+Shift+L both to Delete Line). Until the accelerator table changes, `cmd_line_del`
  tells them apart with GetKeyState: Ctrl down + Shift up = cut; menu and Ctrl+Shift+L = delete.
- Ctrl+Shift+L deletes all touched lines (Scintilla deletes only the caret line); "touched" excludes a final line
  where a non-empty selection ends at column 0 (Scintilla's LINECUT would include it).
- Ctrl+Shift+Up/Down leave the moved lines selected (Scintilla behaviour; DESIGN didn't say).
- Go To columns are display columns (status bar "Col"), not character counts (T06's note suggested characters).
- Options hints use ids IDC_OPT_HINT0..9 (unique per page) because rc warns RC2182 about duplicate control ids.

### Requests for files I don't own
- `res/resource.h`: `#define IDM_LINE_CUT 40120`. `res/menu.rc2`: accelerators `"L", IDM_LINE_CUT, VIRTKEY, CONTROL`
  and keep `"L", IDM_LINE_DEL, VIRTKEY, CONTROL, SHIFT` (drop the plain Ctrl+L -> IDM_LINE_DEL line); Line submenu:
  `MENUITEM "Cu&t Line\tCtrl+L", IDM_LINE_CUT` and `"De&lete Line\tCtrl+Shift+L", IDM_LINE_DEL`.
  `src/win/cmds.h`: `void cmd_line_cut(void);` under editcmds. `src/win/cmdtable.cpp`:
  `{ IDM_LINE_CUT, 1, cmd_line_cut, 0, cmdstate_edit_lineops, 0, "cmd_line_cut" }`. (cmd_line_del then always sees
  Shift for its accelerator, so it stays correct without further change.)
- `src/win/cmdtable.cpp` (T15): `cmd_help_about` should call `dlg_about()` (still `ui_notimpl`).
- `src/win/textview.cpp` (T08): `ed_match_brace` already checks the char before pos, then at pos; drop the second
  call with `char_prev(c)` (~line 1190: it can highlight a brace two characters away) and use `rc == E_NOTFOUND`.
  (Sent to the overseer for relay during the wave.)
- `src/win/sessmgr.cpp` (T13): `dlg_exit_prompt` lists exactly the tabs passed; filter untitled tabs per 15.9
  (backups on and !ask_untitled_on_exit) before calling; EXIT_SAVE: save ticked, discard unticked.

### Gotchas
- RC warns RC2182 for duplicate control ids inside one dialog (only -1 is exempt), so shared "class" ids for
  statics don't work; use a small id range instead.
- PropSheet in PSH_MODELESS mode: after OK/Cancel the sheet stays alive until DestroyWindow; check
  `PropSheet_GetCurrentPageHwnd(sheet) == NULL`. PSN_APPLY goes to every created page: commit once (dirty flag).
- The VM lock is not a queue (10 s polling); with six agents expect 15-30 minute waits.
- vncdo keys go to the foreground window: after killing a program, click the target window first.
- The lexer never sees more than LEX_LINE_MAX chars: brace scans style only that prefix.

### Tests
- Mac: `make -C tests edlines` strict + ASan/UBSan: 4,643 checks, 0 failed (also seeds 2, 7, 99; `-long` 15,043).
  Full `make -C tests`: lint 176 files 0 findings, coretest 145,550 checks, 0 failed (both builds).
- VC6 (VM A): `obj\edlines.obj clip.obj editcmds.obj dlggoto.obj dlgopts.obj dlgmisc.obj` zero warnings at /W3;
  `w98 build editor t_edlines` 4,643 checks, 0 failed. Full pica.exe built from a private copy of the tree
  (tests/tmp/t12, since removed) with zero warnings, rc included (after the RC2182 fix).
- Selftest on that build, `pica.exe -selftest -cfg C:\PICATEST` (all): st_framework, st_frame, st_textview, st_docio,
  st_hexview, st_search, st_fif, **st_edit (249 checks), st_dialogs (59 checks)** all PASS; 4 SKIP (Wave 3);
  `selftest: 9 passed, 0 failed`. (An earlier integrated build failed one st_edit check: my test expected Undo enabled
  after an undo/redo/undo sequence; fixed to check Redo.)
- VNC: Pica -> Win98 Notepad copy (ANSI text with e-acute and O-slash arrives intact) and Notepad -> Pica paste
  ("Hello from Notepad" pasted at the end, Ctrl+S, bytes on disk checked). Note: Win98 Notepad has no Ctrl+A.
- Screenshots (read and checked) in `docs/shots/`: t12_goto_text, t12_goto_error (red, Go disabled), t12_goto_hex,
  t12_opt_general/editor/files/colours/hex, t12_exit_prompt, t12_convert_loss, t12_properties, t12_about,
  t12_clip_to_notepad, t12_clip_from_notepad.

### Open issues
- Copy/cut with an empty selection does nothing (Notepad++ default); VS-style "copy the whole line" is not done.
- Brace highlighting on every caret move in a huge file with an unmatched brace scans up to 20,000 lines (with the
  raw prefilter: only lines containing that brace kind are decoded); T08 already skips it in large mode.
- The Options sheet title bar shows the Win98 "?" context-help button (PSH_NOCONTEXTHELP needs comctl32 5.80).
- About shows the VM's build date (__DATE__ from VM A's clock, ~14 h behind).

## I1 Integration of Wave 2 cross-file requests (wave 3, agent 86box-d1-I1, 2026-11-10)
### Built
- **T11 key hooks:** `fifworker.h` declares `isearch_translate(MSG*)` / `results_translate(MSG*)`; `main.cpp`
  `app_dispatch` calls `if (isearch_translate(m) || results_translate(m)) return;` before the main accelerators (the
  find-dialog branch is unchanged). Ctrl+A/X/V/Z typed in the incremental search box now edit the box, not the document.
- **T11:** `frame_close` calls `fif_stop()` after the exit questions/config save, before `DestroyWindow`.
- **T08 language tabs helper: `int tab_apply_lang_tabs(Tab* t)`** (tab.h, tabs.cpp). Call it after setting `t->lang`.
  With `cfg.use_lang_tabs` on and a language set, it copies `LangInfo.tab_width` (1-16) and `use_tabs` (0 spaces,
  1 tabs, 2 = global `cfg.use_spaces`) into `t->tv` and calls `textview_invalidate(g_app.textview)` if `t` is the
  active text-view tab. Returns 1 when `t->tv` changed. None of my files sets `t->lang` outside selftest helpers: the
  Syntax menu (doccmds) goes through `docio_set_lang` (T14), which still has the same logic inline. T14 can replace it
  with the helper.
- **Document > Tabs** (doccmds `tabs_changed`) now calls `textview_invalidate(g_app.textview)` instead of
  `textview_config_changed` (same repaint, without re-creating the fonts). `cmd_edit_overwrite` already invalidated.
- **`textview_pos_rect(HWND, epos, RECT* screen)`** (views.h, textview.cpp): screen rectangle of the character at pos
  (wide characters 2 cells, one cell at a line end), wrap rows included (affinity 0: a position at a wrap point is at
  the start of the next row). Returns 1 when the row is on screen (x is not clipped when scrolled sideways), else 0
  with an empty rect. Not used yet; meant for the Find dialog's auto-move (dlgfind.cpp).
- **Status bar (overseer review):** (a) a note wider than the last part switches the bar to simple mode (`SB_SIMPLE`,
  the whole width, like Explorer's menu help) until the note expires; a short note stays in the last part. A click on
  the bar during a long note dismisses it and hits the part underneath. `statusbar_get(SB_PART_FLAGS)` still returns
  the note. (b) EOL part shows `-` for binary documents (NUL in the first 4 KB and not UTF-16, `detect_is_binary`)
  in the hex view, or whenever the counts are mixed. Cached per tab id and length, so caret moves don't re-read.
- `obj\t07\`, `obj\t07link.bat`, `obj\t07link.rsp` deleted.

### Interface changes / deviations
- **`tab_new` ownership reverted (approved by the overseer):** on failure it returns NULL and **doc stays the
  caller's**. T07 had made it delete doc, but 9 of the 10 callers (dlgfind, dlgmisc, docio x2, fifpanel x2, hexview,
  editcmds, textview) delete doc again on failure (double free on out-of-memory). Reverting fixed all of them with no
  edits outside tabs.cpp. frame.cpp's `st_make_tab`, which leaked, now deletes doc. The tab.h comment says so.
  **Don't drop the `delete d` after a failed `tab_new`.**
- New declarations: `tab_apply_lang_tabs` (tab.h), `textview_pos_rect` (views.h), the two translate hooks
  (fifworker.h; also still in T11's private fifworker_int.h, identical).

### Tests
- `python3 tools/lint.py`: 176 files, 0 findings.
- VC6 (VM A): `obj\main frame tabs statusbar doccmds editcmds textview .obj` zero warnings at /W3; full
  `w98 build editor` links pica.exe with no warnings.
- `pica.exe -selftest -cfg C:\PICAI1`: `selftest: 9 passed, 0 failed (4 skipped)`. st_frame now has 125 checks; new:
  `st_frame_translate` (isearch box: type, Ctrl+A, Ctrl+X empties the box, Ctrl+V twice = "worwor", Ctrl+Z; the
  document stays "hello world" and unmodified) and `st_frame_notes` (short note in the part, long note -> SB_ISSIMPLE,
  parts back after expiry).
- VNC (real keys): `pica.exe -cfg C:\PICAI1V hello.txt`, Ctrl+I, type "wor", Ctrl+A, Ctrl+X, Ctrl+V, Ctrl+V: box
  "worwor", document unchanged and unmodified; Ctrl+Z: box "wor". Shots: `docs/shots/i1_isearch_keys.png`,
  `i1_isearch_undo.png`.
- Status bar: `pica.exe -cfg C:\PICAI1S bin.dat` with a duplicate-id `langs\zz.lng`: the 15 s note "Language file
  problem: ZZ.LNG: duplicate language id (skipped)" spans the whole bar (`docs/shots/i1_statusbar_longnote.png`);
  after it expires: hex view parts with EOL `-` (`docs/shots/i1_statusbar_binary.png`). Fixtures in `tests/tmp/i1/`.

### Notes / requests for files I don't own
- T14 (docio.cpp `docio_set_lang`): optionally replace the inline language-tab block with `tab_apply_lang_tabs(t)`
  (keep the unconditional invalidate + `app_update_status` for the highlighter change).
- T12's textview request (single `ed_match_brace` call) was already applied in Wave 2. No change needed.
- Find dialog (dlgfind.cpp): can switch its auto-move estimate to `textview_pos_rect`.

## T13 Session, backups, crash recovery, exit flow (wave 3, agent 86box-d1-T13, 2026-11-10)
### Built
- `src/win/sessmgr.cpp`: backup scheduling and jobs (13.5), session writing (13.6: async A/B through the worker, sync
  at startup/exit/emergency), startup restore and crash recovery (13.7: backups, orphan and stale backups,
  changed-on-disk bar, missing-file note, `clean_exit`, `-nosession`, `restore_session` off), recovery of interrupted
  safe saves (~PO/~PC pairs), the exit flow (`sessmgr_exit`), `sessmgr_tab_saved/closed`, `sessmgr_emergency`,
  `st_session`.
- `src/win/iothread.cpp`: BACKUP/SESSION/DELETE completions go to `sessmgr_io_done`. IOJ_DELETE with `meta.tabid != 0`
  = `backup_delete_tab(path, tabid)`. `iothread_set_fs` (tests run FaultFs on the worker). `iothread_emergency_stop(ms)`
  (iothread_stop with a timeout and no hand-over; `iothread_start` works again afterwards).
- `src/win/sessmgr_int.h` (new private header for sessmgr.cpp and iothread.cpp): those hooks plus the job conventions.
### Interface notes
- **Folder:** `sessmgr_startup()` uses `g_app.cfg_dir`. `sessmgr_restore_from(dir)` restores from `dir` and makes it
  the live folder (`dir\session1.ini`, `session2.ini`, `backup\`). Backups and sessions run only after one of these
  calls, so other selftests never write backups.
- **One sequence counter** numbers session writes and backups (BackupMeta.seq), starting above everything on disk.
  At startup a backup older than the session is superseded and deleted in two cases: its session tab says
  `modified=0 has_backup=0` (saved, undone or discarded after the backup), or its tab id is missing from the session
  (the tab was closed). Every other backup is restored, also when both session files are lost. New tab ids start
  above every id in the session and the backup folder (also with `-nosession`), so a new tab never overwrites an old
  tab's backups.
- **Backups:** a backup is due `backup_seconds` after the last edit, at most 30 s after the first edit that has no
  backup yet, at once on WM_ACTIVATEAPP(false), and on exit. Checks run on TIMER_BACKUP (1 s); content changes are
  detected through `Document::change_seq()`. Only one backup job per tab is in flight. Documents up to 2 MB are
  **copied** into a MemSource for the worker; larger ones use a SnapshotSource. After a failure, retries back off
  5-60 s and one status note is shown. Backups are deleted after a save (`sessmgr_tab_saved`), when the document is
  back at its saved state or an untitled tab becomes empty, on close (after the session is written without the tab),
  and on Discard at exit. On-demand tabs are never backed up: one status note per tab ("too large for backups").
- **Session:** written 2 s after `sessmgr_state_changed`, every 60 s when a state hash (carets, scroll, view, wrap,
  language, modified...) changed, after a save, on deactivation, synchronously after the startup restore
  (`clean_exit=0`) and at exit (`=1`). Only one session job is in flight; a request that comes in meanwhile is written
  when it finishes. Untitled tabs go into the session only when they are modified and not empty. Named tabs that
  were discarded are written with `modified=0`.
- **Restored tabs:** a tab restored from a backup is a Document built from the backup bytes (BOM checked), marked
  always-modified, with the same tab id and generation (no rewrite until the next edit). Encoding, line ending,
  language, caret and path come from the backup; view state comes from the session. `t->disk` = the file's
  *current* stat, so T14's polling raises no second bar. Bars (NB_RECOVERED / NB_CHANGED):
  "changed on disk after your unsaved changes were backed up" [Reload from disk] [Keep my version]; crash restore
  "Recovered: Pica did not close normally..." [Save]/[Save As...] [Revert to saved]; a named file now missing gets a
  bar with [Save]. Tabs kept at a normal exit get no bar (the `*` marks them), and the status bar says "N unsaved
  documents from the last session restored." After a crash the status bar says "Pica did not close normally;
  unsaved changes were restored from backups." Missing files are counted in one note. A backup that can't be
  restored is copied to `<cfg>\recover\`. `sessmgr_tab_saved` hides NB_RECOVERED.
- **Interrupted saves:** this case applies to session tabs whose file is missing and to MRU entries that are not open.
  If `~PO*.TMP` (matching the session's recorded size and mtime exactly; for MRU entries, the only one in the
  folder) and `~PC*.TMP` (the newest) exist next to the missing file, the PC bytes open (in memory, at most 4 MB) as
  a tab with the target path, marked missing and always-modified. The bar offers [Restore this version], which moves
  PC onto the target and keeps PO. Nothing is ever renamed without that click.
- **Exit (`sessmgr_exit`):** running saves finish first. List: modified named tabs and on-demand tabs; untitled tabs
  are listed only when backups are off or `ask_untitled_on_exit` is on (empty untitled tabs never). Then
  `dlg_exit_prompt`; it falls back to a `Q_EXIT` question if the dialog fails. Save Selected uses `filecmd_save(t, 1)`
  on ticked tabs (a failed or cancelled save cancels the exit) and discards unticked ones. Keep discards only the
  on-demand tabs. Discard discards the listed tabs. With backups off, everything modified is discarded. Then final
  backups and deletes, a queue flush (after 300 ms a "Still writing backups..." UiProgress; after 10 s `Q_SM_FLUSH`
  asks Yes = wait more, No = exit now). If a kept document has no current backup, `Q_SM_UNSAVED` asks before
  losing it. Last comes the synchronous session write with `clean_exit=1`. If WM_QUERYENDSESSION was answered and
  the shutdown was then cancelled, the discard marks are reset on the next tick. Timers do nothing while the exit
  flow runs.
- **Emergency:** `iothread_emergency_stop(3000)`, then synchronous `backup_write` for every modified in-memory tab
  without a current backup, then the session with `clean_exit=0`. Each step is in its own `__try`.
- Private question ids: `Q_SM_FLUSH = Q_USER + 30`, `Q_SM_UNSAVED = Q_USER + 31`.
### Interface changes / deviations from DESIGN
- No shared header changed. New private header `sessmgr_int.h`. sessmgr.cpp includes T09's private `docio_int.h`
  (`docio_set_lang`, `docio_wait_save`, `filecmd_save`, `iothread_running`).
- Stale backups are detected with the shared sequence counter (a backup older than the session) instead of
  DESIGN's "orphans are always restored". Only backups the user explicitly gave up (save, close, discard) are
  dropped, so nothing that was never discarded is lost.
- Restored tabs keep their numbers ("new 7") instead of "recovered N".
- Crash-restored tabs get a NB_RECOVERED bar (DESIGN only has the status note). Tabs kept at a normal exit don't.
- The session's `read_only` is applied (the user's Read-only toggle survives a restart).
### Requests for files I don't own
- **`src/win/docio.cpp` (T14), important:** in `docio_reload`, call `iothread_wait_tab(t->id, 30000)` before
  `d->reinterpret(...)` and before `d->reset_source(...)`. Both free or swap the source and the add buffer that a
  running backup SnapshotSource reads (documents > 2 MB; smaller ones are copied). Without the wait, a reload during
  a backup of a big file is a use-after-free on the worker thread. `reload_as_edit` and `convert` are safe (they
  only append).
- `src/win/tab.h` (I1, optional): the backup fields are kept in sync (`has_backup`, `backup_gen`, `backup_seq` =
  change_seq of the newest backup, `dirty_since_ms`, `backup_note_shown`); `io_pending` is unused.
### Gotchas
- Changing the folder must reset the A/B slot (`g_which`); otherwise the first write in a new folder goes to
  `session2.ini`.
- `notifybar_show` replaces the binary/read-only bar that docio_open showed for a restored tab (one bar per tab).
- vncdo keys reach Pica fine for these checks; Ctrl+Tab / Ctrl+Shift+Tab switch tabs, Alt+K presses Keep.
### Tests
- Mac: clang `-fsyntax-only -Wall -Wextra -Wshadow -Wsign-compare` with a mock windows.h on both files: clean.
  `python3 tools/lint.py`: 178 files, 0 findings.
- VC6 (VM A): full `w98 build editor`: sessmgr.cpp and iothread.cpp **zero warnings**, pica.exe linked.
- `pica.exe -selftest session -cfg C:\PICATEST`: **PASS st_session (102 checks)**. Covers: round trip (4 tabs:
  selection, bookmarks, wrap, tab width, hex view and pane, forced language, encoding, EOL, active tab) through a
  restart simulation; unsaved named and untitled contents byte-exact and marked; closing deletes backups; the
  changed-on-disk bar and Reload (undoable, unmodified); `restore_session` off; a torn newest backup (FaultFs on the
  worker: data write and cleanup fail) falls back to the previous generation; a newer orphan is restored ("new 7",
  same id), stale and superseded backups are deleted, new ids are above them; an interrupted save is offered, not
  restored silently, then Restore puts PC in place and keeps PO; an unmatched PO gives no offer; exit Cancel / Keep
  (3 backups, session clean_exit=1, restored next start without the crash bar) / Save Selected (files written,
  backups deleted, untitled kept) / Discard (backup deleted, file unchanged) / untitled only (no question) /
  backups off (untitled listed, nothing kept); emergency (worker stopped, synchronous backup + session, restored and
  marked); `-nosession` writes no session.
- Full `pica.exe -selftest`: 12 passed, 1 failed. The failure is st_scenarios (T15, in progress: 3 audit checks).
- Manual on VM A (`-multi -cfg C:\PT13`): typed into `named.txt` and a new tab. After 12 s both backups existed
  (contents exactly what was typed). `w98 kill`, restart: both tabs back, the Recovered bar, and the status note
  "Pica did not close normally..." (`docs/shots/t13_crash_restore.png`). Ctrl+S on the restored file: disk bytes
  identical to the typed text, and its backup was deleted. Two modified named files + Alt+F4 showed one prompt
  listing exactly the 2 files, untitled not listed (`t13_exit_prompt.png`). Keep for Next Session: 3 backups,
  session clean_exit=1. Restart: all 3 tabs back, modified, no crash bar, "3 unsaved documents from the last session
  restored." (`t13_keep_restore.png`).
### Open issues
- Two live instances sharing one cfg dir (`-multi`) can still pick the same new tab id; ids only start above what is
  on disk at startup.
- If the user answers "exit now" while backup jobs are still running, the final synchronous session write can race
  with a queued async session write on the same A/B file (the other file keeps the previous valid copy).
- Torn backup files of tabs that no longer exist are never cleaned up (harmless: they are ignored, and a new tab with
  that id replaces them).

## T14 Disk events (change detection, reload, locked files) and large-file mode (wave 3, agent 86box-d1-T14, 2026-11-10)
### Built
- `src/win/watch.cpp`: change detection (13.9). Poll of every named tab (frame TIMER_POLL, WM_ACTIVATEAPP, a tab
  becoming active - detected in `watch_handles`, which the main loop calls every idle round), change notifications
  for up to 16 folders of open files (fixed/remote/RAM drives, removable except A:/B:; UNC paths poll only),
  1 s stability re-check on a private callback timer (`SetTimer(NULL, ...)`, runs only while candidates exist),
  quiet reload of unmodified tabs (status note "Reloaded X (changed on disk)"), the NB_CHANGED bar
  ([Reload] [Keep my version]; on-demand: [Reload] only), the NB_MISSING bar ([Save] [Close tab]) with
  always-modified, reappearing files treated as changed, own-save suppression, queueing while inactive or modal,
  quiet-reload retry while the writer still holds the file locked, `st_watch`.
- `src/win/largefile.cpp`: indexer thread per on-demand document (`file_index`, own handle, below-normal priority,
  WM_APP_INDEX wParam = job id, lParam = percent or 1000 = done), NB_INDEXING bar "Indexing X... 43%" + [Cancel]
  (closes the tab; [x] only hides the bar), tab label / status bar percent, `finish_indexing` on completion, caret
  to `-n<line>` or to the pre-reload place afterwards, the bar the tab had before (read-only, shared-RO, binary)
  shown again after indexing, cancel on close, the one-time "Unsaved changes to files over N MB are not backed up"
  note (own DocListener per on-demand tab, sets `Tab::backup_note_shown`), `st_largefile` (+ 100 MB timing).
- `src/win/docio.cpp`: asynchronous on-demand open (`largefile_begin_ex`; synchronous indexing only if the thread
  can't start), `docio_reload_ex` (flags RL_QUIET / RL_ASK_LOSSY; keeps caret line/column AND scroll; load mode
  follows the file's current size: see below), `docio_ondemand_threshold()`, `docio_set_lang` uses I1's
  `tab_apply_lang_tabs`, `docio_view_refresh` only repaints while a document indexes, st_docio waits for the indexer.
- `src/win/watch_int.h` (new private header of my three files).
- docio_int.h signatures unchanged (T13 uses them).

### Interface notes
- **Reload changes mode now (T09 open issue fixed).** Grown past the threshold -> `reset_source(empty MemSource)`
  (EV_RESET, closes the old source) + `init_source_indexing(FileSource)` (silent) + indexer thread -> EV_INDEXED: the
  same event sequence listeners see for a fresh open. Shrunk below it -> `init_source(MemSource)` (silent, leaves
  on-demand mode) + `reset_source` on the same source (EV_RESET with counts). Both use only public Document calls.
  During an async reload the caret sits at the origin; the saved place is applied when indexing ends.
- Reload does **not** re-apply the language's tab settings (`set_lang_(..., apply_tabs = 0)`): the language is
  unchanged and a Document > Tabs choice must survive files changing on disk. Open, Save As re-detect and Syntax
  apply them (docio_set_lang -> tab_apply_lang_tabs).
- Watch-triggered reloads are `docio_reload_ex(t, 0, 1, RL_QUIET)`: in-memory <= 4 MB = `reload_as_edit`, so Undo
  brings the previous text back; errors never show a box (E_SHARING/E_ACCESS/E_NOTFOUND/E_IO retry after 2 s; others
  show the bar with the reason). The bar's Reload uses RL_ASK_LOSSY: a modified document whose reload can't be undone
  (> 4 MB, on-demand, encoding change) asks Q_RELOAD_DISCARD first.
- Keep my version (and the bar's [x]) records the new disk state and marks the document always-modified (its text
  differs from the file, so Save is enabled and the exit dialog lists it).
- Missing: `Tab::disk_missing = 1`, `set_always_modified(true)`; if the file comes back and the user made no edit
  since, always-modified is cleared again and the change is handled like any other (quiet reload / bar). Close tab
  asks "Save changes?" only if the document had edits.
- "Can act" = frame enabled (no message box / UiProgress) and our process owns the foreground window (selftest: always,
  except `watch_test_set_inactive`). Queued changes set `Tab::change_pending` and are acted on by the next poll
  (1 s timer, 3 s poll or activation).
- `watch_tab_added/removed` are idempotent (app_add_tab and docio_open both call added).
- Timings (VM A, no VNC): **100 MB SynthSource file on C: (written by st_largefile): first screen 14 ms, hex view at
  the end of the file 18 ms, full index 2.2 s** (budget 1 s / 3 s). 20 MB on C:: open 8-10 ms, index ~0.5 s, save of
  the edited on-demand doc 3.5 s. **100 MB file on M: (written from the Mac): first screen 80 ms, hex view at the end
  69 ms, full index 93 s** (M: reads at ~1.1 MB/s; the tab stays usable read-only meanwhile).
  Change detection: Mac-side append to a file on M: reloaded 3.5 s after the edit; in-VM `echo >>` by command.com on
  C: 3.3 s (incl. agent98 latency); in-VM writes on C: are seen at once through the folder notification, then 1 s
  stability.

### Interface changes / deviations from DESIGN
- No shared header changed. New private header `src/win/watch_int.h`.
- 13.9 "watch_poll(all)": both values stat every named tab (cheap); `all` is ignored.
- Tab activation check lives in `watch_handles` (tabs.cpp has no hook; it runs on the next idle round).
- On-demand changed bar: [Reload] only (as 13.9 says for share-write opens), also for Mac-side edits of on-demand
  files on M: (our deny-write handle does not stop those).
- A file that changes while it is being indexed (size differs at the end) closes the tab with a message.

### Requests for files I don't own
- T13 (sessmgr): the "not backed up" note for on-demand tabs is shown by largefile.cpp and sets
  `Tab::backup_note_shown`; check that flag before showing your own, or leave the note to me.
- T15 (selftest.cpp `st_wait_idle`): treat `t->indexing` of any tab as busy (indexer thread).
- Overseer: `Document` has no reset-into-indexing call; I used reset_source(empty) + init_source_indexing (see
  above). A `Document::reset_source_indexing` would make that one call (optional).

### Gotchas
- **Win98 file times:** a file rewritten (CREATE_ALWAYS + write + close) shortly after a SetFileTime on it kept the
  SetFileTime value on C:. Real writers normally change size or mtime; st_watch stamps its same-size rewrite itself.
- `progress_window()` is in frame_int.h (frame modules only); watch.cpp uses `IsWindowEnabled(g_app.frame)`, which
  UiProgress and message boxes both clear.
- VC6 TIMERPROC takes `UINT` ids (no UINT_PTR in VC6's headers).
- During this wave the shared link failed on T13's `sessmgr_io_done`; I tested with a private exe:
  `obj\t14\mk.bat` (stub for that symbol + `obj\t14\link.rsp` over the shared objects -> `obj\t14\pica.exe`; build
  output, delete freely).
- Selftest hooks: `<cfg>\st_big` ("100" = write and time N MB in the fixture folder, or a path = time an existing file),
  `<cfg>\st_watch_ext` (a path: st_watch opens it, writes `<path>.wait` and waits 60 s for an outside edit).

### Tests
- VC6: `obj\watch.obj largefile.obj docio.obj` zero warnings at /W3. lint 0 findings. Full `w98 build editor` (shared
  tree, 12:35) links pica.exe with zero warnings.
- `pica.exe -selftest watch largefile -cfg C:\PICATEST`: **PASS st_watch (43 checks), PASS st_largefile (30 checks)**
  (34 with the 100 MB run). Full selftest on the shared build: 12 passed, 1 failed (st_scenarios, T15's, 3 audit checks of key routing,
  in progress; no watch/indexer activity in its log), 0 skipped.
- st_watch: quiet reload on size change after the 1 s stability check, caret line/column kept, undoable;
  same-size change; own save never triggers; modified tab -> bar, bar shown once, Keep (disk state recorded, still
  modified), next change -> bar, Reload + Undo brings my version back; reload_silent off -> bar for unmodified tabs;
  delete -> missing bar (stays on later polls), always-modified; recreate -> reloaded, flag cleared; Save on the
  missing bar rewrites the file; Close tab without a question; inactive -> queued, active -> acted on within 1.5 s;
  folder notification fires and checks the tab at once; folder list empties without tabs; locked writer -> no popup,
  retried, reloaded after unlock; locked open -> Retry asked again / Cancel; unlocked open asks nothing; read-only bar
  Edit anyway; reload while a real IOJ_BACKUP job over a SnapshotSource of the 3 MB modified document is still
  pending (T13's request: docio_reload_ex waits with `iothread_wait_tab(t->id, 30000)` before reinterpret /
  reset_source / init_source; E_BUSY after 30 s). Manual: Mac-side append on M: (3.5 s) and command.com append on C: (3.3 s) through `st_watch_ext`.
- st_largefile: 20 MB opens on demand in ~10 ms and indexes in the background; indexing bar with Cancel; edits and
  save refused (E_BUSY); hex view usable and scrolls to the middle while indexing; line count exact; edit + save +
  reopen byte-exact; old copy deleted on close; Cancel on the bar closes the tab and stops the thread; closing during
  indexing stops it; 3 MB in memory -> grows to 6 MB -> reload goes on demand (indexed, convert refused) -> shrinks
  to 1 MB -> back in memory (convert allowed).
- Screenshots: `docs/shots/t14_indexing.png` (20 MB on M: at 21%: bar, first screen, "(indexing 21%)" label,
  "Large Indexing 21%" status), `t14_changed_bar.png` (Mac-side edit, reload_silent off), `t14_missing_bar.png`
  (file deleted on the Mac side; tab marked `*`).
- Big test files removed (C:\PICATEST\fixtures empty; tests/tmp big files deleted).

### Open issues
- Change detection on M: depends on the poll (3 s) + 1 s stability: 1-4 s. A shorter poll for the active tab would
  make it snappier.
- On-demand reload counts nothing synchronously, but an in-memory reload of a large file on M: shows the modal
  progress window (1.1 MB/s); Cancel there turns a quiet reload into the bar.
- The 100 MB fixture takes ~80 s to write on C: (SynthSource + 6 MB/s writes), so it is opt-in (`st_big`).

## T15 Selftest suite, key injection, shortcut audit, help text (wave 3, agent 86box-d1-T15, 2026-11-10)
### Built
- `src/win/selftest.cpp`: key injection rewritten, `st_keys_modal`, `st_wait_idle` completed, `st_scenarios` (10
  end-to-end scenarios incl. the accelerator audit and the AltGr test). The framework and `st_framework` are unchanged.
- `src/win/cmdtable.cpp`: `IDM_LINE_CUT` entry; the Help commands (`cmd_help_show(section)`, `cmd_help_keys/regex`;
  `cmd_help_about` calls `dlg_about()`).
- `res/resource.h`: `IDM_LINE_CUT 40120`. `res/menu.rc2`: Ctrl+L -> `IDM_LINE_CUT`, Ctrl+Shift+L -> `IDM_LINE_DEL`,
  Line submenu "Cu&t Line\tCtrl+L" + "De&lete Line\tCtrl+Shift+L"; `IDA_FIND` gained Ctrl+Shift+F3 (Find Selection
  Backwards also works while the Find dialog has the focus, like the other F3 keys).
- `src/win/cmds.h`: `cmd_line_cut` declared; `HELP_TOP/KEYS/REGEX/ENCODINGS/HEX/FILES/LARGE` section ids for
  `cmd_help_show`.
- `src/help/help.md`: the user help (views, all shortcuts incl. text-view and hex-view keys, regex cheat sheet,
  encodings and line endings, hex view, files/settings/backups, large files, command line, tips). ASCII, CRLF, prose
  wrapped at 88 columns and tables aligned so it reads well as raw text in an 800x600 window.

### Interface notes
- **st_keys** syntax: `^` Ctrl, `+` Shift, `%` Alt for the next key; `{NAME}` keys (ENTER TAB ESC BS DEL INS HOME END
  PGUP PGDN UP DOWN LEFT RIGHT SPACE F1-F12); `{PLUS} {CARET} {PERCENT} {LBRACE} {RBRACE}` type those characters
  (resolved by name, so `{LEFT}`/`{F12}` are no longer remapped: T10's bug fixed); `{VKdd}` = any virtual key in hex
  (e.g. `^{VKDD}` = Ctrl+]); other characters use the key VkKeyScan gives (layout shift/ctrl/alt bits applied); a
  character without a key (e.g. `\xE9` on US) is posted as WM_CHAR. Messages go to the focus window (if the focus is
  not ours: the active view, focused first) and run through the real `app_dispatch` while the faked keyboard state is
  set. Alt without Ctrl = WM_SYSKEYDOWN (bit 29), Ctrl+Alt = WM_KEYDOWN, extended bit for navigation keys - as Windows.
  Remember `+` is Shift: type a plus sign with `{PLUS}`.
- **st_keys_modal(c, open_keys or NULL, cmd, dlg_keys, title, cap)** (static in selftest.cpp): for modal dialogs (Go
  To, About, Options...). The dialog keys are queued and fed from a thread timer (dispatched by the dialog's own
  loop), one message per 30 ms, only while one of our `#32770` dialogs owned by the frame is up (never into the main
  window). A dialog still open 1.2 s after the last key is cancelled and the check fails, so a broken dialog can't hang
  the run. Returns 1 if a dialog appeared, its caption in `title`. Export it in selftest.h if other st_ functions need it.
- **st_wait_idle** = no idle work pending (`app_idle_work`), `!fif_running()`, `iothread_flush(0)` (queue empty;
  hands finished jobs over), no tab with `saving`, `indexing` or `io_pending` (T11/T14 requests).
- **Help tab**: the RCDATA text is written to `<cfg_dir>\Pica Help.md` (falls back to `%TEMP%` if not writable; only
  rewritten when no tab shows it and the bytes differ) and opened with `docio_open` (`read_only = 1`, `from_session = 1`
  = no MRU entry, no reuse of an empty "new 1"). An open tab named `Pica Help.md` is reused. The section heading line
  (exact text, e.g. `## Keyboard shortcuts`) becomes the top line with the caret on it (text view: detach, set
  `tv.top_line`/`sel`, attach; hex view or another tab: `app_activate`). **Keep the `##` headings in help.md in sync
  with `k_help_heading[]` in cmdtable.cpp** (sc_help checks the two used by the menu).
- `<cfg>\st_help.hold` (seconds) keeps the help tab on screen during `st_scenarios` (screenshots).

### Scenarios (st_scenarios, 457 checks)
1. save cycle: open ANSI CRLF fixture, type at the end and in line 1, Ctrl+Z/Ctrl+Y, Backspace/Left/Del, title `*`,
   Ctrl+S (async, waited), bytes on disk, Ctrl+W without a question, reopen (bytes, ANSI CRLF, no history); UTF-8 BOM LF
   file: e-acute typed (WM_CHAR), saved as C3 A9 with the BOM, Select All + Del keeps the BOM, undo to the save point.
2. hex switch: UTF-8 mixed-EOL file, caret byte kept text -> hex, nibble moves, hex -> text snaps to the character
   start, 6 switches change no byte (not modified, no undo), typed `42` (overwrite) and `z` in the char pane, Ctrl+Z
   in the text view undoes the hex edits back to the save point, Ctrl+Y redoes.
3. find/replace: Ctrl+H, type, Alt+L to Replace with, Alt+A Replace All ("3 replaced"), Esc, one Ctrl+Z undoes it,
   Ctrl+Y, F3 / F3 / Shift+F3 positions, Ctrl+F + Alt+G (Regex) + Alt+N + `(ab` = live red error, `b\w+d` + Enter finds
   "bird", Alt+M back to Normal.
4. encoding: ANSI -> UTF-8 (status bar), save, -> LF (status bar), save, Ctrl+Z twice = original encoding and bytes,
   save, -> UTF-16 LE (BOM is not text), undo; UTF-8 Cyrillic -> ANSI: loss prompt Cancel (nothing) / OK ('?'), undo.
5. line commands (.c file): Ctrl+D, Ctrl+Shift+Down (moved line selected), Ctrl+L (cut: clipboard = "alpha\r\n"),
   Ctrl+Shift+L (delete, clipboard kept), Ctrl+V, Ctrl+Shift+U / Ctrl+U (selection kept), Ctrl+Q twice (`// `),
   Ctrl+Shift+Up, undo everything back to the save point.
6. bookmarks: Ctrl+F2 x2, F2 / F2 / F2 (wrap) / Shift+F2 (wrap), marks follow an inserted line and come back on undo,
   Ctrl+F2 removes one, Clear All, F2 without bookmarks ("No bookmarks" note, caret unchanged).
7. go to (real modal dialog): `7`, `end`, `3:4`, invalid `zz` + Enter (stays) + Esc; hex view: "Go To Offset", `1A`,
   `#5`, Esc.
8. help: F1 opens the read-only Markdown tab at "## Keyboard shortcuts" as the top line, Help > Regular Expressions
   reuses it, typing changes nothing, Ctrl+W closes it, About (modal) opens and closes with Enter.
9. accelerator audit (below). 10. AltGr (below).

### Shortcut audit findings
- All 48 keys of DESIGN 15.5 (with Ctrl+L = Cut Line) reach exactly their command through the real loop
  (`cmd_set_trace(fn, 1)`: traced, nothing runs): 46 live; F4 / Shift+F4 are grey without results (the key is
  swallowed; their table entries are checked statically). `IDA_MAIN` has exactly these 48 entries, nothing extra;
  all 39 shortcuts shown in menu texts have a matching accelerator; Del / Ins / Tab reach the view.
- Fixed: Ctrl+L / Ctrl+Shift+L split (T12), Ctrl+Shift+F3 in `IDA_FIND`.
- **AltGr**: VM A could load the Finnish layout (`LoadKeyboardLayout("0000040B")`, unloaded afterwards): real AltGr
  keys typed `@ { [ ] } \ | € $ £ µ` into an ANSI document with no accelerator firing; `~` is a dead key on that
  layout (skipped). On layouts without AltGr the test simulates the Finnish and German key positions.
- **Known limit (not fixed, needs main.cpp):** Ctrl+Alt+S (Save As, the Notepad++ key) swallows AltGr+S on layouts that
  put a letter there, e.g. Polish (Programmers) s-acute. Proposed fix for `app_dispatch` (I1/T17), before
  `TranslateAcceleratorA(g_app.frame, g_app.accel, m)`: skip the accelerator when the key is a Ctrl+Alt accelerator
  key that the current layout turns into a character:
  ```
  static int altgr_char(const MSG* m)          // Ctrl+Alt+key that types a character on this layout (AltGr)
  {
    BYTE ks[256]; WORD ch = 0;
    if (m->message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) >= 0) return 0;
    if (m->wParam != 'S') return 0;            // only keys IDA_MAIN uses with Ctrl+Alt (ToAscii eats dead keys)
    if (!GetKeyboardState(ks)) return 0;
    return ToAscii((UINT)m->wParam, (UINT)((m->lParam >> 16) & 0xFF), ks, &ch, 0) == 1;
  }
  ...  if (!altgr_char(m) && g_app.accel && g_app.frame && TranslateAcceleratorA(...)) return;
  ```
  On US/Finnish layouts Ctrl+Alt+S stays Save As (ToAscii returns 0). The AltGr scenario logs what Ctrl+Alt+S does.
- UX.md 7 keys not in 1.0 (no commands exist; DESIGN scope): Ctrl+Shift+T reopen closed tab, Ctrl+J join lines,
  Ctrl+P print, zoom keys. Alt+F4 is the system close (menu text only).

### Interface changes / deviations from DESIGN
- 15.5: Ctrl+L = Cut Line (new `IDM_LINE_CUT`), Ctrl+Shift+L = Delete Line (T12's request; Notepad++/VC6 behaviour).
- `IDA_FIND` has six entries (Ctrl+Shift+F3 added to DESIGN 15.5's five).
- `cmds.h`: `cmd_line_cut`, `HELP_*` enum. No signature changed.

### Requests for files I don't own
- `src/win/editcmds.cpp` (I1/T17): `cmd_line_del`'s `GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) >= 0`
  branch is now dead code (Ctrl+L sends IDM_LINE_CUT; Ctrl+Shift+L and the menu send IDM_LINE_DEL): make
  `cmd_line_del` call `ec_line_delete()` only, and drop the local `cmd_line_cut` prototype (now in cmds.h).
- `src/win/main.cpp` (I1/T17): the `altgr_char` guard above.
- `src/win/docio.cpp` (T14/T17): **double delete** when `tab_new` fails: `tab_new` already deletes the document on
  failure (T07), but `docio_new` (`t = tab_new(d); if (!t) { delete d; ...}`) and `docio_open` (`if (!t) { delete d;
  ui_error(...) }`) delete it again. Drop the `delete d` in both (out-of-memory path only).
- `src/win/selftest.h` (overseer, optional): declare `st_keys_modal` if other modules want modal-dialog key tests.

### Gotchas
- VK_LEFT (0x25) == '%', VK_F12 (0x7B) == '{', VK_RIGHT == '\'', VK_DOWN == '(' ...: never compare key codes with
  characters; the old remap broke `{LEFT}`/`{F12}`.
- An accelerator whose menu item is grey is swallowed by TranslateAccelerator (no WM_COMMAND), so audits need a state
  where commands are enabled (two tabs, modified named file, undo + redo, selection, clipboard data).
- Undo merging: "a", Left, Right, "b" is ONE undo step (the caret came back); use `Document::break_undo_merge` to get two.
- A modal dialog opened by a key blocks inside `st_pump`; feed it with a thread timer (`SetTimer(NULL, ...)`), not
  with more st_keys calls.
- `w98 run` waits for the whole selftest (~3 min on VM A); the shell call then outlives the Bash tool's 3-minute
  foreground limit - run it in the background.

### Tests
- `python3 tools/lint.py` on all my files: 0 findings (help.md CRLF).
- VC6 (VM A): `obj\selftest.obj`, `obj\cmdtable.obj`, `obj\pica.res` and the full `w98 build editor` link with zero
  warnings.
- `pica.exe -selftest -cfg C:\PICATEST` (all): **13 passed, 0 failed (0 skipped)**: st_framework
  4271, st_frame 125, st_textview 56, st_docio 238, st_hexview 78, st_search 60, st_fif 29, st_edit 249, st_dialogs 59,
  st_session 102, st_watch 43, st_largefile 30, **st_scenarios 457** (every scenario 0 failed). Run three times on
  the final tree; one full run takes about 3 minutes.
- Screenshot `docs/shots/t15_help_keys.png`: help tab "Pica Help.md - Pica [Read-only]" at "## Keyboard shortcuts"
  (line 41 at the top), Markdown headings coloured, aligned tables, status bar `Markdown | ANSI | CRLF | RO`.

### Open issues
- The audit can't check F4 / Shift+F4 live (needs find results); their table entries are checked.
- `~` (dead key on the Finnish layout) is not typed by the AltGr test in real-layout mode.
- Help is plain Markdown source (coloured), not rendered; links between sections are by heading text.

## T17 Win QA, bug bash and `-bench` performance pass (wave 4, agent 86box-d1-T17, 2026-11-10)
### Built
- `src/win/bench.cpp`: `pica.exe -bench -cfg C:\PICATEST` (DESIGN 17.5). Maximizes the window, writes fixtures once
  into `<cfg>\bench` (bench.c 3000 lines of C; dense.c = the probe's "dense" text, 116 chars and ~14 runs per line;
  extreme.c ~70 runs per row; replace.txt 1 MB with exactly 10,000 "needle"; SynthSource f1mb / f10mb / f100mb. The
  100 MB file is kept between runs and skipped when less than 150 MB are free) and writes `<cfg>\bench.txt`: one line
  per measurement with the 3.2 budget and OK / OVER / info, then a summary. The exit code is the number of OVER lines.
  Measured: full repaint (C file, dense, extreme, hex; median/worst of 60), Down at the bottom row, wheel, page down,
  keystroke until idle, open 1/10 MB (3 runs + memory delta), open 100 MB on demand (first screen + hex view at the
  end, full index, memory), find literal (ignore case / match case) and regex in 10 MB through the real `op_find`,
  Replace All of 10,000 matches through `op_replace_all` (+ its undo, info), GDI objects after 50 tabs, startup
  (child process `-multi -nosession`: CreateProcess -> frame visible + WaitForInputIdle) and startup + session
  restore of 10 files. Diagnostics (info lines): the core split of the 10 MB open (file_load vs
  Document::init_source) and the raw ReadFile speed of the 100 MB file.
- GDI accounting (`render_int.h`: `gdi_track`, `gdi_delete`, `gdi_live`): every font / brush / region / bitmap the
  win modules create (render, hexview, dlgopts, dlgmisc, isearch, frame) is counted. Win98 has no GetGuiResources.
- `dlgfind.cpp`: `finddlg_bench(t, pat, rep, mode, match_case, op)` (declared in `fifworker_int.h`) for the bench.
- `selftest.cpp`: scenario 11 "t17 fixes" in st_scenarios: Ctrl+H / Ctrl+Shift+F / Ctrl+F inside the Find dialog
  switch the page and keep the typed text; 30 tabs (a third in the hex view, some with notification bars) opened and
  closed leave `gdi_live()` unchanged.

### Bug list (assigned + found in the bug bash; all in win code, all fixed)
| # | Bug | Fix | Test |
|---|---|---|---|
| 1 | "Not backed up" note shown twice for on-demand tabs (sessmgr + largefile) | sessmgr checks `t->backup_note_shown` | review (both paths set the flag) |
| 2 | `cmd_line_del`: dead "Ctrl without Shift = cut" branch, stale local `cmd_line_cut` prototype | delete only; prototype dropped (cmds.h has it) | sc_lines |
| 3 | AltGr+S (Polish Programmers s-acute) swallowed by Ctrl+Alt+S Save As | T15's `altgr_char` guard before TranslateAccelerator (only keys that ToAscii turns into a printable char) | sc_altgr |
| 4 | Ctrl+F / Ctrl+H / Ctrl+Shift+F inside the Find dialog did nothing | IDA_FIND gained the three keys; `finddlg_open` from inside the active dialog only switches the page (like a tab click) and keeps the fields | sc_t17 |
| 5 | Regex token menu: 26 entries, too tall for 800x600 | five submenus (Characters, Anchors, Repeat, Groups, Look-ahead and flags); the replace menu stays flat (12) | t17_find_tokens.png |
| 6 | Find dialog moved away from an estimated row (wrong with word wrap / scrolled views) | I1's `textview_pos_rect` in the text view (exact row; no move when the match is beside the dialog); hex view keeps the row estimate | st_search |
| 7 | **Single instance race** (Explorer starts one `pica.exe "%1"` per file): `GetLastError()` was read after `FindWindowA`, which can reset it, so a process started while the first had the mutex but no window yet could miss ERROR_ALREADY_EXISTS and open a second window; the wait was only 3 s; `SMTO_ABORTIFHUNG` + 8 s let a main instance busy restoring its session look hung, so the file opened in a new window | last error read right after CreateMutex; wait up to 15 s; SMTO_NORMAL 30 s + one retry. Command-line format unchanged | VM A, no instance running: 5 back-to-back `w98 start` and a .bat with 5 `start` lines both gave 5 tabs in one process, in order |
| 8 | Files handed over by a second instance while a modal window was up (message box, progress, Options) opened tabs underneath that question | `main_on_copydata_open` queues them while the frame is disabled; a 300 ms thread timer re-posts them afterwards | manual review |
| 9 | Find dialog showed stale status text after closing and reopening ("106 hits in 29 files" from Find in Files on the Find page, or an old red error): the clear ran while the dialog was hidden and only reached the status bar | the status control is always updated; a page change clears another page's result | VNC before/after |
| 10 | Ctrl+A did nothing in the Find dialog's fields (Win98 edit controls ignore it) | `app_dispatch` selects all in an Edit of the Find dialog | VNC |
| 11 | Options sheet title bar had a "?" button that did nothing (PSH_NOCONTEXTHELP needs comctl32 5.80) | PSH_USECALLBACK: DS_CONTEXTHELP dropped in PSCB_PRECREATE, WS_EX_CONTEXTHELP in PSCB_INITIALIZED | t17_options.png |
| 12 | A font still selected into a view's CS_OWNDC could fail to delete on a font change (GDI leak per Options font change) | `free_fonts` selects a stock font into the cached DCs first | GDI count (bench, sc_t17) |
| 13 | On-demand reload used reset_source(empty) + init_source_indexing (two events; a failure left an empty tab) | T16's `Document::reset_source_indexing` (one EV_RESET; on failure the document is unchanged) | st_docio, st_watch, st_largefile |
| - | Leftover build folders | `obj\t14\` deleted; tests/tmp `fif1 fif2 i1 stfix t17` deleted (`advbak`, `pfuzz-rx-1.bin` are T16's live files). No artefacts outside `obj\` except the nmake targets pica.exe / coretest.exe | - |

### Bench (VM A, final tree with T16's init_source fix, window maximized at 800x600, 27 text rows, Courier New 10, no VNC viewer)
| Measurement | Value | Budget | |
|---|---|---|---|
| Full repaint, C file coloured | median 5.7 ms, worst 8.6 | <= 12 ms | OK |
| Full repaint, dense (116 chars, 14 runs/line) | median 12.1 ms, worst 19.0 | <= 15 ms | OK |
| Full repaint, extreme (~70 runs/row) | median 29.7 ms | - | info |
| Line scroll: Down at the bottom row | median 6.5 ms (2.9-3.7 in earlier runs) | <= 8 ms | OK |
| Line scroll: wheel 3 lines | median 6.2 ms (1.9-3.6 earlier) | <= 8 ms | OK |
| Page down | median 6.3 ms | <= 12 ms | OK |
| Keystroke (edit, relex, row, caret, status, title, until idle) | median 1.2 ms | <= 3 ms | OK |
| Hex view full repaint | median 7.9 ms (3.6-4.5 earlier) | <= 12 ms | OK |
| Open 1 MB from C: to first paint | median 53 ms (first 205) | <= 150 ms | OK |
| Open 10 MB from C: to first paint | median 391 ms (first 1979, cold cache) | <= 600 ms | OK (was 1255 before T16's fix) |
| 10 MB split (core) | file_load 114 ms, init_source 263 ms (was 1122) | - | info |
| Memory, in-memory doc | 1024 KB / 1 MB file, 10240 KB / 10 MB file | size + 3% | OK |
| Open 100 MB on demand: first screen + hex at the end | 28 + 116 ms | <= 1000 ms | OK |
| Open 100 MB on demand: full index | 17.6 s (4.4 s in the first run) | <= 3000 ms | **OVER: disk-bound** (raw ReadFile of the uncached file 9 MB/s = 11 s; 100 MB can't stay in VCACHE with 128 MB RAM) |
| Memory, on-demand doc | 128 KB | <= 8 MB | OK |
| Find next, literal, ignore case, 10 MB | 75 MB/s | >= 30 MB/s | OK |
| Find next, literal, match case | 158 MB/s | >= 30 MB/s | OK |
| Find next, regex `[xy]\d{4}[xy]` (no literal), 10 MB | 4.0 MB/s | >= 2 MB/s | OK |
| Replace All, 10,000 matches in 1 MB | 993 ms (undo 1131 ms, info) | <= 2000 ms | OK |
| GDI objects after 50 tabs | 2 before, 2 after | no growth | OK |
| Startup to window visible + idle (process load included) | median 180 ms (77 in earlier runs) | <= 300 ms | OK |
| Startup + session restore, 10 small files | median 292 ms | <= 1000 ms | OK |
The last run came right after a VM restart with VM B busy, so its scroll/hex numbers are about 2x the earlier runs;
every run was within budget except the disk-bound 100 MB index.

### Bug-bash checklist
- VNC on VM A (pica copied to `C:\PT17` so settings stay out of the project): C file coloured, hex view toggle,
  Find (all three pages, token submenus, the Ctrl+A / stale-status bugs above), Find in Files over
  `M:\dev\editor\src` (106 hits in 29 files, results panel, F4 caption), Options (5 pages), binary file -> hex view +
  bar (opened through a second instance), the exit prompt with 2 modified files (Discard left both files unchanged
  on disk), the window dragged to its minimum size (menu wraps, toolbar clips, status parts shrink; usable),
  second instance with file arguments (5 at once, see bug 7).
- Selftests (13/13, st_scenarios 525 checks): tabs open/close/switch, save cycles, close during save, undo/redo
  across text and hex edits, mixed EOL / invalid UTF-8 / UTF-16 fixtures in both views, paste incl. RawBytes with
  NUL, read-only and locked files, external changes, 20 MB on-demand edits while indexing (refused, then allowed),
  session + crash recovery, every accelerator, AltGr, GDI after 30 tabs.
- Not done (VM time): High Contrast scheme switch (WM_SYSCOLORCHANGE handling reviewed: the frame forwards it to the
  common controls and re-applies colours), Fixedsys / Lucida Console visual pass (T08 shot Fixedsys), a full disk
  (covered by FaultFs in st_session / T05 suites).

### Tests
- VC6 full `w98 build editor`: 0 warnings. `pica.exe -selftest -cfg C:\PICATEST`: 13 passed, 0 failed (0 skipped);
  st_scenarios 525 checks incl. "t17 fixes" 68. After bug 13: `-selftest docio watch largefile session` 4/4, then the
  full selftest again on the final tree: 13 passed, 0 failed. `python3 tools/lint.py`: 0 findings.
- `pica.exe -bench -cfg C:\PICATEST`: 20 OK, 1 OVER (disk-bound 100 MB index), 4 info.

### Screenshots (`docs/shots/t17_*.png`, all read and checked)
t17_text_c (C file coloured), t17_hex (hex view of the same file), t17_find_tokens (Find page with the regex token
submenus), t17_fif_results (results panel), t17_options (General page, no "?" button), t17_notify_binary (binary
bar + hex view), t17_exit_prompt (Unsaved Changes dialog). Visual review: nothing misaligned or clipped; fixed the
"?" button, the stale status line and the 26-entry token menu.

### Interface notes / changes
- New in shared win headers: `render_int.h` (`gdi_track/gdi_delete/gdi_live`; **new GDI objects in win code must use
  them**), `fifworker_int.h` (`finddlg_bench`). `res/menu.rc2`: IDA_FIND has 9 entries (Ctrl+F, Ctrl+H,
  Ctrl+Shift+F added).
- Single instance: same `"PICA1\0<cwd>\0<args>\0\0"` WM_COPYDATA block; `pica.exe "file"` from Explorer works once
  per file (T19's associations need nothing new). The settings folder is the exe folder when it is writable.

### Gotchas
- The scratchpad folder is shared by every agent of the session: use task-prefixed file names.
- vncdo `ctrl-shift-x` arrives without Shift on VM A; use the menus or toolbar for Shift shortcuts over VNC.
- After someone reboots VM A, check that `M:` answers (`w98 sh "dir M:\dev"`) before running pica from M:. A
  pica.exe started from a dead share hard-hung the whole VM (frozen clock, VNC dead) and needed a force restart.
  `kill` didn't end 86Box (it ignored SIGTERM): confirm the process is gone before starting the VM again, or two
  86Box processes run on the same VHD.

### Open issues
- 100 MB full index on the emulated disk is I/O-bound (9-25 MB/s uncached), far from the 3 s budget; only a faster
  disk helps. The first screen and hex view are usable within 150 ms meanwhile.
- The extreme colouring case (~70 runs per row) repaints in 30 ms. A per-colour batched draw (one ExtTextOutW per
  colour per row) would fix it; not done (risky this late, not a budget case).
- Undo of a 10,000-match Replace All takes ~1.1 s with no progress window.

## T19 Installer: setup.exe, upgrade, uninstall, file associations, "Open with Pica" (wave 4, agent 86box-d1-T19, 2026-11-10)
### Built
- `setup/setup.cpp`: command line, wizard (PropertySheetA PSH_WIZARD: Welcome / Folder and options / File types /
  Installing / Finish), silent install, uninstall flow (questions, temp-copy handshake), Pica-running Retry loop.
- `setup/inst.cpp`: the work: payload extraction (write `*.NEW`, then swap), WININIT.INI `[rename]`, registry
  (associations with saved previous values, ProgID, `*\shell\Pica`, Add/Remove entry), IShellLink shortcuts, settings
  purge, uninstall. Every step goes to the log (`%TEMP%\PICASET.LOG` or `/LOG=`).
- `setup/setup.h`, `setup_ids.h`, `setup.rc` (pages + RCDATA payload), `uninst.rc`, `setupver.rc2` (icon = `res/pica.ico`,
  VERSIONINFO from `res/appinfo.h`), `setup/Makefile` (CRLF; targets `all`, `old`, `shtest`, `clean`).
- `setup.exe` (921 KB) embeds `..\pica.exe`, `obj\uninst.exe` (64 KB, same objects, no payload: always uninstalls) and,
  if present at build time, `..\README.TXT` / `..\LICENSE.TXT`. The version Setup shows/stores is read from the
  embedded pica.exe's VS_FIXEDFILEINFO, not from a constant.
- `setup/shtest.cpp` -> `obj\shtest.exe`: test helper (ShellExecuteEx verbs, wait for a Pica title, close Pica,
  file version, association dump, key exists, WININIT dump, reboot). Not shipped.
- `tools/patch_ver.py`: copy of an exe with another version (0.9) for the upgrade/downgrade tests.
- `docs/INSTALLER.md`: behaviour, switches, exit codes, registry layout, build, limits, release notes.
- Screenshots: `docs/shots/t19_welcome.png`, `t19_welcome_upgrade.png` ("Upgrade Pica 0.9 -> 1.0"),
  `t19_folder_fresh.png`, `t19_folder.png` (upgrade: folder fixed), `t19_types.png`, `t19_running.png` (Retry dialog),
  `t19_finish.png`, `t19_context_menu.png` (Explorer right-click "Open with Pica"), `t19_opened_via_menu.png`,
  `t19_addremove.png`, `t19_uninstall_confirm.png`, `t19_uninstall_settings.png`, `t19_uninstall_done.png`.
### Interface notes
- Switches: `/S`, `/D=<dir>` (last, no quotes), `/TYPES=default|none|all|.txt,.c|+.bat,-.ini`, `/STARTMENU=0|1`,
  `/DESKTOP=0|1`, `/CONTEXT=0|1`, `/RUN`, `/FORCE`, `/LOG=<file>`, `/UNINSTALL [/S] [/PURGE]`, test-only `/_LOCKTEST`.
  Exit codes 0 ok, 1 error/cancel, 2 Pica running, 3 restart needed, 4 newer installed (silent).
- Registry: `HKLM\Software\Pica` (InstallDir, Version, Files, Types, StartMenu, Desktop, ContextMenu, StartMenuLnk,
  DesktopLnk) with `Previous\Open` (per .ext: `=old`, `!` no default, `-` no key), `Previous\Edit` (per ProgID: old
  edit command / `-` = verb added by us), `Previous\Created` (HKCR keys we created; deleted once empty);
  `HKCR\Pica.Document` (DefaultIcon, shell\open\command `"<dir>\pica.exe" "%1"`); `HKCR\.ext` = Pica.Document for
  ticked text/source/web types; `HKCR\<ProgID>\shell\edit\command` for ticked script types (.bat .cmd .reg .inf .vbs
  .js keep their `open`); `HKCR\*\shell\Pica` "Open with &Pica"; `HKLM\...\Uninstall\Pica`.
- Applying associations is idempotent "desired state": re-running Setup restores unticked types, adds ticked ones,
  keeps the first saved previous value, leaves entries someone else changed alone. Restores only while the entry
  still points at Pica. Deletion of HKCR keys only when empty (Win9x RegDeleteKey deletes whole trees).
- Upgrade replaces only pica.exe, uninst.exe, README.TXT, LICENSE.TXT (+ deletes files an older version listed in
  `Files` that this one doesn't ship). pica.ini, session1/2.ini, backup\, langs\ and user files are never touched.
  With Pica installed, the folder is fixed (uninstall to move it); `/D` to another folder is refused.
- Uninstall: `uninst.exe` in the folder copies itself to `%TEMP%\PICAUN.EXE`, starts it (`/UNINSTALL /_CHILD=<pid>`)
  and waits on a named event; the result code comes back through a 4-byte named file mapping. The copy deletes
  uninst.exe after the original exits, removes the folder if empty and schedules itself with `NUL=` in WININIT.INI.
  `setup.exe /uninstall` (from elsewhere) runs synchronously. "Delete settings" (default No; `/PURGE`) removes
  pica.ini, session1/2.ini, ~pica.tst, backup\* in the install dir, `Application Data\Pica` and `<windows>\Pica`.
### Interface changes / deviations from DESIGN
- None to Pica. Deviations from the T19 text: Start menu shortcut is `Programs\Pica.lnk` (no folder); script types
  without a program that runs them (.cmd on Win98) get the `open` mapping instead of an edit verb; the folder can't be
  changed on an upgrade.
### Requests for files I don't own
- None required. For T18: release builds should run `w98 build 'editor\setup' clean` then `w98 build 'editor\setup'`
  after the final pica.exe and README.TXT exist (README is picked up from `..\README.TXT`; override with
  `nmake README=..\release\Pica-1.0\README.TXT` if T18 keeps it elsewhere). Ship `setup.exe` next to the zip.
- Root `.gitignore` already covers `setup/obj/` and `setup/setup.exe` (`obj/`, `*.exe`, `*.res`).
### Gotchas
- WININIT.INI needs 8.3 paths; the file renamed through it comes back as `PICA.EXE` (capitals). Write `NUL=old`
  before `old=new`. WritePrivateProfileString can't write duplicate `NUL=` keys, so the file is edited by hand.
- **Rebooting VM A from agent98**: `ExitWindowsEx` from a console program started by agent98 does not reboot (the
  console window vetoes; with EWX_FORCE it killed Explorer and still didn't reboot). `w98 start 'rundll32.exe
  shell32.dll,SHExitWindowsEx 2'` rebooted cleanly (agent98 back after ~30 s, WININIT processed).
- nmake didn't rebuild `obj\setup.res` once although its inputs were newer (VM clock was a day behind the Mac before
  a reboot); `clean` fixes it.
- `regedit /e` HKCR exports differ only in two `"Implementing"` values (component-category cache timestamps that
  Windows rewrites at boot); everything else must be byte-identical.
### Tests (VM A, all with the lock)
- Build: `setup.cpp`, `inst.cpp`, `shtest.cpp`, rc files: zero warnings at /W3 (clean build). `python3 tools/lint.py
  setup/*.cpp setup/*.h` 0 findings; rc/rc2/Makefile/setup_ids.h CRLF, ASCII.
- Fresh install (`obj\setup09.exe /S`, 0.9): rc 0, pica.exe + uninst.exe, Start menu link, 10 default types,
  context menu. Launch checks with ShellExecuteEx: `a.txt` (default verb) -> "a.txt - Pica"; `b.c` open -> "b.c -
  Pica"; verb `Pica` on `C:\T19\long name test.txt` -> "long name test.txt - Pica" (long name with spaces arrives
  intact; all three in one process via the single instance). `run.bat` open still runs it (marker file written).
- Upgrade 0.9 -> 1.0 with Pica running: silent rc 2, version still 0.9; after closing Pica: rc 0, pica.exe 1.0;
  pica.ini, session1/2.ini (fc /b), langs\my.lng, notes.txt, backup\T0000A01.BAK unchanged.
- Downgrade: setup09 /S over 1.0 -> rc 4 (nothing changed); with /FORCE -> rc 0. Repair (same version) rc 0.
- `/TYPES=+.bat,+.reg,+.cmd,+.js,-.ini`: .ini restored to inifile; batfile/regfile/JSFile edit commands -> Pica
  (previous Notepad commands saved); .cmd (no ProgID on Win98) -> Pica.Document. `edit` on run.bat -> "run.bat -
  Pica"; `open` on run.bat still runs it.
- Locked files (`/_LOCKTEST`, 0.9 installed): rc 3, PICA.NEW/UNINST.NEW + WININIT.INI lines; after a reboot
  pica.exe and uninst.exe are 1.0 and the .NEW files are gone.
- Silent uninstall via the installed `uninst.exe /S` (temp-copy path): rc 0 passed back, all associations restored
  (log shows each), Pica keys, shortcut, uninst.exe deleted; kept settings + user files left the folder in place.
  HKCR export identical to the pre-install export (except the two "Implementing" timestamps); Uninstall key export
  byte-identical; `HKLM\Software\Pica` absent.
- Wizard over VNC: upgrade 0.9 -> 1.0 with .bat ticked in the GUI, Pica running -> Retry dialog -> closed Pica ->
  Retry -> Finish -> "Start Pica now" started it. Explorer right-click on `my.lng` shows "Open with Pica" (bold:
  the only verb for that type) and opens it in Pica. Add/Remove Programs lists "Pica 1.0"; Add/Remove -> confirm ->
  "delete settings" Yes -> "Pica was removed"; list refreshed without Pica.
- Final round with the final binary: fresh wizard pages + Cancel ("Quit Pica Setup?" -> exit 1); `/S /DESKTOP=1
  /D=C:\Program Files\Pica Test` (spaces) rc 0, desktop link created, b.c opens in Pica; `setup.exe /uninstall /S
  /PURGE` rc 0: folder, both links, settings gone; registry again identical to the pre-install export.
- VM left clean: no Pica folder, no Pica keys, original associations, no WININIT.INI, no PICAUN.EXE, `C:\T19`
  deleted, no Pica/setup processes. Logs and exports kept on the Mac in `setup/obj/reg/` (ignored by git).
### Open issues
- The Browse button (SHBrowseForFolder, adds `\Pica`) was not exercised over VNC.
- Not tested on the real T40 (only VM A). No print verb on Pica.Document; "New > Text Document" reads "Pica
  Document" while .txt is mapped (Notepad++ behaves the same).
### Addendum (overseer follow-up): one ProgID per type
- `HKCR\Pica.Document` is replaced by one ProgID per ticked type, `Pica.<ext>` (`.txt -> Pica.txt`). It is rebuilt on
  every run from the saved previous ProgID: name copied ("Text Document", "C Source file", "Configuration Settings";
  else "<EXT> File"), every non-`open` verb copied with its whole subtree (txtfile/inifile `print`, VC6's "&Open with
  MSDev" incl. `ddeexec`), `open` = Pica, DefaultIcon = Pica. `.ext\ShellNew` is never touched. Uninstall restores
  `HKCR\.ext` (only while it points at `Pica.<ext>` or the legacy `Pica.Document`) and deletes `Pica.<ext>`.
- Migration: an install made by the earlier setup (shared `Pica.Document`) is treated as ours; the upgrade reuses the
  saved previous values, repoints each `.ext` to `Pica.<ext>` and deletes `Pica.Document`.
- `shtest` gained `typename <file>` (SHGetFileInfo SHGFI_TYPENAME, the Type column) and `verbs <.ext>`.
- Tests (VM A): `obj\setup09_legacy.exe` (the earlier setup.exe with all three version resources patched to 0.9,
  i.e. old scheme) `/S /TYPES=+.bat` -> `.txt`/`.c` = Pica.Document; new `setup.exe /S` -> rc 0, log "HKCR\.txt
  Pica.Document -> Pica.txt", Pica.Document deleted. `verbs .txt`: open = Pica, `print` = `C:\WINDOWS\NOTEPAD.EXE /p
  %1`, ShellNew present; `.ini` print kept; `.c` keeps "Open with MSDev". Type names: a.txt "Text Document", b.c "C
  Source file". a.txt and b.c still open in Pica. Explorer right-click on a.txt shows Open with Pica / **Open** /
  Print (`docs/shots/t19_txt_menu_print.png`, details pane says "Text Document"; Print not executed). Background >
  New lists "Text Document" (`t19_new_menu.png`); clicking it created `New Text Document.txt` (0 bytes, type name
  "Text Document"). `uninst.exe /S /PURGE` rc 0: all `.ext` restored, every `Pica.<ext>` deleted; HKCR export again
  identical to the pre-install export except the two boot-time "Implementing" timestamps; Uninstall key export
  byte-identical; `HKLM\Software\Pica` absent. Build zero warnings at /W3, lint 0 findings. VM left clean (no Pica
  folder/keys, no WININIT.INI, no temp files, C:\T19 deleted, Explorer window closed).
- Limit: the copied verbs are a snapshot (a verb added to `txtfile` later appears on .txt after a repair run or
  uninstall); `shellex` handlers of the old ProgID are not copied.

## T16 Core QA: adversarial tests and fixes (wave 4, agent 86box-d1-T16, 2026-11-10)
### Built
- `src/core/ptree.cpp`: **line-start cache** inside PieceTree (design below); `break_pos`, `breaks_before` and the piece
  split of every edit (`ensure_boundary_`) use it. New private header `src/core/ptree_int.h` (test hook
  `pt_line_cache_enable(int)`, timing comparisons only).
- `src/core/doc.cpp`: **memchr EOL counting** (unit 1 and UTF-16) for `init_source`/`reset_source`/recounts and the
  edit windows; one pass at load (the piece tree already counted the break byte). The old per-unit loop stays as the
  reference (odd UTF-16 windows, tests). New private header `src/core/doc_int.h` (`doc_count_eol_test`).
  `Document::reset_source_indexing` (optional item, below).
- `src/core/lexgen.cpp`: bug fix (below).
- `tests/t_adversarial.cpp` (suite `adversarial`, 44,230 checks): 1 line cache, 2 pathological inputs, 3 regex
  bombs / limits / garbage / Replace All edges, 4 lexer garbage + convergence, 5 settings / session / .lng / hex /
  go-to corruption, 6 backup corruption, 7 undo stress, 8 codec round trips, 9 OOM sweeps, 10 EOL counting + open
  timing, 11 reset_source_indexing. Section times go to the verbose log (`-q` off).
- `tests/mac/pfuzz.cpp`: mutation fuzzer for the parsers (README in `tests/mac/README.md`); `tests/t_fif.cpp` fixture fix.

### Interface notes
- **Line-start cache.** The bytes a piece refers to never change (original immutable, add chunks append-only), so
  break counts are cached per SOURCE range, independent of the tree shape: per 64 KB-aligned block of a source
  (src, start >> 16) an entry holds the break count of each 512-byte sub-block (u16 x 128; UTF-16: one array per
  start parity, because a piece counts pairs at even offsets from its own start). Entries fill lazily from the block
  start up to the sub-block a query needs. A query over [x, y) scans at most ~512 bytes at each end and adds whole
  sub-blocks in between (ranges < 2 KB just scan). Edits and undo never invalidate anything; `clear`/`init`
  (chunk ids are reused), `set_break_spec`, `set_original` drop the table, every lookup re-checks spec and original,
  `swap` swaps it with the tree. An add-chunk sub-block is cached only once the chunk holds every byte it reads.
  Memory: 4-way set-associative, sets sized to the document when allocated (1 set = 4 entries = 2 KB for a 93 KB
  file, at most 64 sets = 131 KB covering 16 MB; LRU replacement beyond). Allocation failure just disables it.
  `PieceTree::line_cache_bytes()` (new, public, end) reports the table size.
- **Timings, VM A (VC6 /O2), random calls:** 93 KB / 3,000-line fresh document: `line_start` 330 -> 8 us, `line_of`
  165 -> 7 us; 10 MB / 330,000 lines: `line_start` 380-440 -> 8.7 us warm (16-19 us in the first 0.3 s while the
  table fills), `line_of` 190 -> 7.3 us. Mac: 4.8 -> 0.1 us. All well under the 20 us target.
- **Open speed:** `Document::init_source` of 10 MB CRLF on VM A: 1122 ms -> 263 ms (`-bench` breakdown; `file_load`
  114 ms), so "open 10 MB to first paint" is 387 ms median (budget 600, was 1255). In the suite (with a 10 MB copy):
  PieceTree::init 220 ms, init_source 330-390 ms; UTF-16 LE 20 MB 770-820 ms. The EOL counts are derived as P = CR-LF pairs,
  crlf = P, lf = LFs - P, cr = CRs - Q with Q = P - x + y (x/y = a CRLF pair crossing the window's start/end).
- **`int Document::reset_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol);`**
  (public, end of Document): `reset_source` + `init_source_indexing` in one call: busy -> E_BUSY, inside a group ->
  E_STATE (src deleted in both cases); else EV_RESET (removed = old length, inserted = new length, lines_added 0),
  undo cleared, on-demand mode, edits refused until `finish_indexing` (EV_INDEXED). docio's reload path
  (`reset_source(empty)` + `init_source_indexing`) can switch to it.
- EOL counts keep the `detect_eol` classification exactly (lone CRs, CR-LF pairs across piece/span boundaries,
  UTF-16 look-alike bytes, odd tails); `eol_counts()` after any edit still equals a full recount.

### Interface changes / deviations from DESIGN
- Additions only: `PieceTree::line_cache_bytes()`, `Document::reset_source_indexing(...)`, private members, the
  forward declarations `struct PtLineCache; struct PtLcEntry;` in ptree.h, private headers `ptree_int.h` and
  `doc_int.h`. No declared signature changed.

### Bugs found and fixed (each with its regression test)
1. `lang_parse`/`lang_init`: a keyword of exactly KW_MAX_LEN + 1 = 48 characters in a .lng file failed with E_NOMEM
   "out of memory" instead of a syntax error (`add_words` turned every `KwTable::add` error into E_NOMEM); an
   override file with such a word was reported as out of memory. Found by `adv_lng_corrupt`; test
   `adv_lng_long_keyword`.
2. Performance: `Document::line_start`/`line_of` scanned up to 64 KB per call (225/110 us on VM A). Line-start
   cache; tests `adv_cache_tree_run` (4 break specs, every edit kind, undo-style re-insert, typing into a growing add
   piece, spec changes, swap, clear + init over another source, unit-2 slow path), `adv_cache_huge` (4 GB - 2,
   blocks at 0xFFFF), `adv_cache_doc` (replace / undo / redo / convert_eol / reinterpret / convert), `adv_cache_oom`,
   `adv_cache_timing`. Mutation check: 4 of 5 injected cache bugs fail the suite (the 5th is equivalent).
3. Performance: opening 10 MB spent ~1 s in `count_eol_` (per-byte state machine over 4 KB `read` copies). memchr
   paths; tests `adv_eol_counts` (ANSI / UTF-16 LE / BE, CR mode, look-alike bytes, odd tails, 1-7-byte source
   spans, every edit, convert_eol, reinterpret), `adv_eol_windows` (direct comparison with the reference loop on
   random windows, all known-count paths; every injected x/y or pairing bug fails it), `adv_open_timing`.
4. `tests/t_fif.cpp` `test_windowed`: `s1.total > 0` failed with `-long` (the random file had no `^x.*?y$` line in
   ~1 of 5 seeds): the fixture now always contains one hit per pattern.

### What the adversarial suite covers (no other core bugs found)
- Pathological inputs (empty, BOM-only x3, all NUL, all CR, all LF, broken EOL, random bytes, invalid/overlong/
  surrogate/truncated UTF-8, UTF-16 with lone surrogates and an odd tail, a 70 KB line, controls) x 8 encodings:
  detect, line structure, layout + wrap of every line, forward/backward character walks agree, snap_char, decode ->
  encode byte-exact, search (8 patterns incl. empty matches; find_all = count; backward = last forward match),
  highlighting, 14 line/case/comment/indent/word edits then undo-all = original bytes, convert to 5 encodings
  (check_convert = convert counts) + undo, convert_eol x3 + undo, reinterpret round trip. 5 MB line (layout index,
  literal + regex search both directions, highlighting, duplicate / move line). 4 GB - 2 document: search at both
  ends, edits at EPOS_MAX (E_TOOBIG at the limit), undo.
- Regex: 21 classic bombs on 20 KB (slowest 0.55 s on VM A, E_BUDGET allowed), 27 malformed/unsupported patterns
  (error position and message), 1,001-char and 101-deep limits, 3,000 garbage patterns with NUL / bad markers /
  surrogates / U+10FFFF in all modes against invalid UTF-8 (count = find_all, ranges, backward, expand). 18 Replace All
  edge cases with exact expected output (`^`, `$` before CRLF, `a*`, unset groups, `\U \E \u`, `$$`, unrepresentable
  replacement in ANSI, CRLF -> LF, lookahead, raw bad bytes, hex mode), each undone.
- Lexers: 200 random lines per language with garbage code points and random start states (state < 0x7FFF, styles <
  ST_COUNT, NULL styles = same state); Highlighter convergence on a garbage document under random edits + undo vs a
  fresh full lex, all 13 languages.
- Settings: config text truncated at every byte + 500 mutations (values in range, strings terminated, write -> parse
  -> write stable); session truncated at every byte (accepted only when it is the whole session) + 500 mutations
  (invariants, session_validate agrees); ini.lng and diff.lng truncated at every byte, the others every 97 bytes, +150
  mutations each; 20,000 garbage hex patterns / go-to expressions.
- Backups: every byte of a backup file flipped, junk appended, trailer doubled, a data byte missing: structure damage
  is never accepted; whatever is listed opens with the right size (no checksum by design: a flipped data byte or
  header value that still parses is accepted).
- Undo stress: 100,000 ops (1,000,000 with `-long`): edits, undo, redo, multi-op groups, save points, with a random
  allocation failure before 1 op in 50 (E_NOMEM must leave the bytes unchanged) vs a flat model with its own history
  (bytes, modified, can_undo/redo; pieces <= 3/op, add buffer <= bytes inserted); full undo then redo; UndoLog memory
  80 bytes per single-op group.
- Codecs: all 38 SBCS tables + 6 Unicode encodings: every representable character from UTF-8 -> X -> UTF-8 byte-exact,
  BOM/origin, undo/redo, reinterpret both ways; lossy counts and first_bad exact; invalid source bytes -> U+FFFD.
- OOM at every allocation (sweep until the failing allocation is never reached; probed after each run), 24
  operations: replace 300 KB, undo, redo, convert, convert_eol x2, reinterpret, replace_text, Replace All x3,
  INI parse+write, config load+store, session parse+write, long-line index, layout + wrap, highlighter, lang_parse,
  move/duplicate/case/comment/indent/insert text: success = right result (and undo restores), failure = E_NOMEM with
  state unchanged and tree consistent, no leaks.
- Mac fuzz pass (`tests/mac/pfuzz.cpp`, ASan + UBSan): 4 seeds x 20 min = 54.6 million inputs over INI/config/session,
  .lng, hex + go-to, regex compile/search/expand, backup headers, detect + Document: no crash, no sanitizer report,
  no invariant violation, so `tests/data/fuzz/` stays empty (pfuzz replays files there when they exist).

### Requests for files I don't own
- T17 / `src/win/docio.cpp` (optional): the on-demand reload can call `d->reset_source_indexing(r.src, r.det.enc,
  r.det.bom_len, r.det.eol_style)` instead of `reset_source(empty)` + `init_source_indexing` (one EV_RESET, no empty
  MemSource). Signature exactly as in Interface notes.
- T17 / tab ids (`tab_new`, `g_app.next_tabid`): `backup_write` refuses tab ids > 0xFFFFF (5 hex digits in the 8.3
  name) with E_RANGE, and `session_from_ini` raises next_tabid past the highest id: after 1,048,575 tabs in the
  lifetime of a cfg folder, new tabs are never backed up. Wrap next_tabid to 1 above 0xFFFFF (skipping ids in use).
- Overseer (DESIGN 6.19, optional): `config_store` returns void, so a failed `Ini::set` (out of memory) silently writes
  a settings file with keys missing (the OOM sweep shows it: success with an incomplete INI). Returning int would let
  `app_save_config` skip the write.

### Gotchas
- Timing loops must not draw from `tf_rand` or contain checks: the number of iterations depends on the machine, and
  VC6 must report the same check counts as the Mac (the timing tests save/restore `tf->rng`).
- `e_test_fail_after` cannot tell whether the failure fired: the sweep probes it afterwards with throwaway
  allocations; prepare test inputs before arming it (a test Vec failing inside the window looks like a core bug).
- In an edit window the x/y corrections cancel between the before and after counts, so only a direct comparison
  (`doc_count_eol_test`) catches them.
- Converting to the encoding the document already has records no undo step; an empty Searcher pattern is E_SYNTAX.

### Tests
- Mac: `make -C tests` (lint 180 files 0 findings; coretest strict + ASan/UBSan): **189,780 checks, 0 failed**
  (was 145,550). `adversarial` alone 44,230 (strict and ASan identical; 8 more seeds pass); `-long`: adversarial
  427,077 checks, full coretest -long 972,032 checks with the fif fixture fixed. `make -C tests mac`: pfuzz 3 s
  (~190,000 inputs) + rxfuzz 10,000 cases, 0 mismatches. Suite time: Mac 6 s, VM A 2.7 min (undo stress 51 s).
- VC6 (VM A): `w98 build editor t_adversarial` zero warnings, **44,230 checks, 0 failed** (same as the Mac);
  `w98 build editor` (pica.exe) zero warnings; `-bench`: 20 OK, 1 OVER (below).
  `w98 build editor test`: coretest **189,835 checks, 0 failed** (Mac + the VC6-only winenc 48 and fileops 7).
  `pica.exe -selftest -cfg C:\PICATEST`: **13 passed, 0 failed (0 skipped)** with the new core.

### Open issues
- `-bench` "open 100 MB on demand: full index" is OVER (18 s vs 3 s): disk-bound in the VM (raw ReadFile of the file
  6 MB/s, 16 s), not core CPU (file_index's counting is the same memchr loop).
- Brace matching / Find All over > 64 K-char lines and the other T03/T06 open issues are unchanged.
