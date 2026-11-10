# Pica (text + hex editor): status

**Pica** (`pica.exe`; name kept in `res/appinfo.h` so it is easy to change). Folder `dev/editor` = `M:\dev\editor`.
Owner session **86box-d1**, **VM A** (`samba/w98`, `samba/vmlock`, `samba/shot86.sh`). Started 2026-11-09.

A robust Notepad++-style text editor for Windows 98 SE with hex editing built in. VC6, Win32 + GDI, all our own code.

## User decisions (2026-11-09)
- **All our own code**: no Scintilla or other editing component. We write the buffer, rendering,
  undo, syntax colouring, search and the hex view ourselves.
- **Focused 1.0**: tabs, open/save with encodings (ANSI / UTF-8 / UTF-16, CRLF/LF), syntax colouring for
  common languages, find/replace with regex + find in files, session restore, and a solid hex view of the
  same file (edit, insert/overwrite, go to, byte search). Macros, split view, plugins etc. later, if wanted.
- Process (as for Retro Strike / Lähiö): research + probe -> `docs/DESIGN.md` for user review -> task waves
  built by sub-agents (`docs/TASKS.md`, notes in `docs/HANDOFF.md`) -> user tests complete milestones.
- 2026-11-09 night: complete the app overnight without waiting for decisions; the user reviews in the morning.

## Phase 1: research + probe (done)
| Task | Output | State |
|---|---|---|
| Architecture research (buffer, big files, Win98 Unicode/encodings, regex, undo, lexing) | `docs/research/ARCH.md` | done |
| Feature / UX research (Notepad++, hex editors, period editors) | `docs/research/UX.md` | done |
| GDI text rendering + file I/O probe on VM A | `probe/RESULTS.md` | done |

## Phase 2: design (done)
- [`docs/DESIGN.md`](docs/DESIGN.md): the spec. Scope (UX must list + word wrap, show whitespace, brace match,
  Ctrl+I, bookmarks, line commands, go to, read-only data inspector), decisions with reasons, probe-based budgets,
  architecture (portable core + win layer, threads), exact core interfaces, document model, text/hex view behaviour,
  rendering, search, encodings, files/backup/session formats, large files, menus/shortcuts/options, error policy,
  tests, build.
- [`docs/TASKS.md`](docs/TASKS.md): 19 tasks in 5 waves with owned files, dependencies, acceptance tests, agent rules
  (VM lock for every VM use, file ownership, VC6 dialect, zero warnings, HANDOFF notes).
- [`docs/HANDOFF.md`](docs/HANDOFF.md): template; agents append a section per task.

Key choices: piece tree over the file's bytes (one document, text + hex views, one undo); direct ExtTextOutW drawing
(4-5 ms full repaint on the VM), Courier New 10 pt; in memory up to 16 MB, read on demand above; saves on a worker
thread with temp + rename; backups in generation files and the session in two alternating files (no renames in the
crash path); polling for changes (Mac-side edits on M: never notify); new files ANSI + CRLF without BOM.

## Phase 3: build (in progress, overnight 2026-11-09/10; user asked for autonomous completion)
| Wave | Tasks | State |
|---|---|---|
| 0 | T00 skeleton: headers, stubs, Makefiles (VC6 + Mac), resources, test harness, app shell | done (commit a2c2572) |
| 1 | T01 buffer, T02 codecs, T03 regex + search, T04 lexers, T05 file I/O, T06 layout/hex format/ini/session (core, Mac-tested) | done (112,926 checks Mac + VC6) |
| 2 | T07 frame, T08 text view, T09 files + document commands, T10 hex view, T11 search UI + find in files, T12 edit commands + dialogs | done (selftest 9/9, 0 warnings) |
| 3 | T13 session/backup/recovery, T14 disk events + large files, T15 selftest suite + shortcuts + help, I1 integration of Wave 2 requests | done (selftest 13/13) |
| 4 | T16 core QA, T17 win QA + performance, then T18 release (README, zip) | planned |

Milestones for the user: after Wave 2 a usable editor; after Wave 3 the full 1.0; after Wave 4 the release zip.
