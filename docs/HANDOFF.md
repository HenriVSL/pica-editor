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
