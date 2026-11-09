# Text + hex editor: status

Working title TBD. Folder `dev/editor` = `M:\dev\editor`. Owner session **86box-d1**, **VM A**
(`samba/w98`, `samba/vmlock`, `samba/shot86.sh`). Started 2026-11-09.

A robust Notepad++-style text editor for Windows 98 SE with hex editing built in. VC6, Win32 + GDI.

## User decisions (2026-11-09)
- **All our own code**: no Scintilla or other editing component. We write the buffer, rendering,
  undo, syntax colouring, search and the hex view ourselves.
- **Focused 1.0**: tabs, open/save with encodings (ANSI / UTF-8 / UTF-16, CRLF/LF), syntax colouring for
  common languages, find/replace with regex + find in files, session restore, and a solid hex view of the
  same file (edit, insert/overwrite, go to, byte search). Macros, split view, plugins etc. later, if wanted.
- Process (as for Retro Strike / Lähiö): research + probe -> `docs/DESIGN.md` for user review -> task waves
  built by sub-agents (`docs/TASKS.md`, notes in `docs/HANDOFF.md`) -> user tests complete milestones.

## Phase 1: research + probe (in progress)
| Task | Output | State |
|---|---|---|
| Architecture research (buffer, big files, Win98 Unicode/encodings, regex, undo, lexing) | `docs/research/ARCH.md` | done |
| Feature / UX research (Notepad++, hex editors, period editors) | `docs/research/UX.md` | done |
| GDI text rendering + file I/O probe on VM A | `probe/RESULTS.md` | running |
