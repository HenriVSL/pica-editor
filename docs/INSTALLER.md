# Pica installer (setup.exe)

`setup/` builds **setup.exe**, our own VC6 installer. It is one file: it embeds `pica.exe`, `uninst.exe` and, when
they exist at build time, `README.TXT` and `LICENSE.TXT`. No MSI, no third-party installer, Win98-safe APIs only.

## What it does

- **Wizard** (Win98 property-sheet wizard): Welcome (says "Upgrade Pica 0.9 -> 1.0", "Reinstall" or "Replace ...
  with ..." when Pica is already installed) -> Folder and options -> File types -> Installing -> Finish
  ("Start Pica now", or "Restart Windows now" when files were in use).
- **Folder**: default `<ProgramFilesDir>\Pica` (`C:\Program Files\Pica`). On an upgrade the folder is fixed to the
  installed one (to move Pica, uninstall it first). Browse adds `\Pica` unless the chosen folder already ends with it.
- **Options**: Start menu shortcut (`Programs\Pica.lnk`, default on), desktop shortcut (default off),
  "Open with Pica" in the right-click menu of every file (`HKCR\*\shell\Pica`, default on).
- **File types** (ticked by default: .txt .log .ini .c .cpp .h .asm .py .md .mak):
  - Text .txt .log .ini .nfo; Web .htm .html .xml (unticked: they open in the browser now); Source .c .cpp .h .hpp
    .rc .asm .py .css .md .mak .diff: a ticked type gets its own ProgID `Pica.<ext>` (`HKCR\.txt = Pica.txt`), so double-click opens Pica.
    `Pica.<ext>` copies the previous ProgID's name ("Text Document"; else "<EXT> File") and all its other verbs
    (print, printto, ...), so Explorer's Type column, the New menu (`.txt\ShellNew` is never touched) and Print
    stay as they were; only `open` and the icon are Pica's. Setups before this used one shared `Pica.Document`;
    an upgrade migrates it (the saved previous values are reused) and deletes `Pica.Document`.
  - Scripts and setup files .bat .cmd .reg .inf .vbs .js: Windows' own `open` verb stays (double-click still runs
    them); a ticked type makes Pica its `edit` verb (`HKCR\<ProgID>\shell\edit\command`; if the ProgID has no Edit
    verb, one called "Edit with Pica" is added). A script type that nothing runs (no ProgID, e.g. .cmd on Win98) is
    opened by Pica instead.
- **Upgrade in place**: replaces only the program files (`pica.exe`, `uninst.exe`, `README.TXT`, `LICENSE.TXT`).
  It never touches `pica.ini`, `session1.ini`, `session2.ini`, `backup\`, `langs\` or anything else in the folder
  (Pica keeps its settings next to the exe when that folder is writable, which on Win98 includes Program Files;
  DESIGN 13.1). Same version = repair. An older setup over a newer Pica asks first (silent: refused, exit 4, unless
  `/FORCE`). The stored choices (shortcuts, context menu, types) are the wizard's defaults on an upgrade.
- **Pica running**: Setup asks to close it and offers Retry/Cancel; it never kills Pica (its session and backups must
  be written). Silent mode doesn't wait: exit 2, nothing changed.
- **Files in use**: all new files are first written next to their targets as `PICA.NEW`, `UNINST.NEW`, ... (a full
  disk changes nothing), then swapped in. A file that can't be deleted (in use) stays as `*.NEW` and
  `WININIT.INI [rename]` gets `NUL=<old>` + `<old>=<new>` (8.3 paths); Setup asks to restart (exit 3).
- **Uninstall** (`uninst.exe` in the install folder, Add/Remove Programs, or `setup.exe /uninstall`): asks, refuses
  while Pica runs, asks "Also delete your settings, session and backups?" (default **No**), restores the previous
  associations (only those that still point at Pica), removes Pica's keys, shortcuts and program files. Kept settings
  keep the folder; with "Yes" (or `/PURGE`) `pica.ini`, `session1/2.ini`, `backup\*` are deleted too, also from
  `Application Data\Pica` and `<windows>\Pica`, and the folder goes if it is empty (`langs\` and files the user
  created are never deleted). Files Pica regenerates (`Pica Help.md`, written when Help is opened, and `~pica.tst`)
  are not settings: every uninstall deletes them, so they never keep the folder alive. The installed `uninst.exe` copies itself to `%TEMP%\PICAUN.EXE`, runs that copy and
  waits for it; the copy deletes `uninst.exe` once the original has exited and schedules its own deletion with
  `WININIT.INI` (`NUL=...PICAUN.EXE`).
- **Log**: every run writes `%TEMP%\PICASET.LOG` (or `/LOG=<file>`): each file, registry change and decision.

## Switches

| Switch | Meaning |
|---|---|
| `/S` | silent: no UI; defaults, or the stored choices of the installed copy |
| `/D=<folder>` | install folder; must be the **last** switch; no quotes needed |
| `/TYPES=<list>` | `default`, `none`, `all`, a list `.txt,.c` (replaces the set) or `+.bat,-.ini` (changes it) |
| `/STARTMENU=0\|1`, `/DESKTOP=0\|1`, `/CONTEXT=0\|1` | the three options |
| `/RUN` | start Pica when the silent install is done |
| `/FORCE` | silent downgrade allowed |
| `/LOG=<file>` | log file |
| `/UNINSTALL` | uninstall (`uninst.exe` always does); with `/S` silent; `/PURGE` also deletes settings |
| `/_LOCKTEST` | test only: holds the old files open so the WININIT.INI path runs |

Exit codes: 0 ok, 1 error / bad switch / cancelled, 2 Pica is running, 3 done but restart needed, 4 newer version
installed (silent without `/FORCE`). `uninst.exe` returns the code of its temp copy.

## Registry

| Key | Values |
|---|---|
| `HKLM\Software\Pica` | `InstallDir`, `Version`, `Files` (installed program files, `;`), `Types` (ticked extensions, `;`), `StartMenu`, `Desktop`, `ContextMenu` (DWORD), `StartMenuLnk`, `DesktopLnk` (shortcut paths) |
| `HKLM\Software\Pica\Previous\Open` | per extension, what `HKCR\.ext` held before: `=<ProgID>`, `!` (key without default), `-` (no key) |
| `HKLM\Software\Pica\Previous\Edit` | per ProgID, what `shell\edit\command` held: `=<command>`, `!` (no command value), `-` (no edit verb: ours) |
| `HKLM\Software\Pica\Previous\Created` | HKCR keys Setup created (`HKCR\.md`, `HKCR\*\shell`...): deleted again once empty |
| `HKCR\Pica.<ext>` (one per ticked type) | name copied from the previous ProgID, `DefaultIcon = <dir>\pica.exe,0`, `shell\open\command = "<dir>\pica.exe" "%1"`, the previous ProgID's other verbs copied; rebuilt on every run, deleted on uninstall |
| `HKCR\.ext` (ticked text/source/web types) | default value `Pica.<ext>` |
| `HKCR\<ProgID>\shell\edit\command` (ticked script types) | `"<dir>\pica.exe" "%1"` |
| `HKCR\*\shell\Pica` | "Open with &Pica", `command = "<dir>\pica.exe" "%1"` |
| `HKLM\...\CurrentVersion\Uninstall\Pica` | `DisplayName "Pica 1.0"`, `DisplayVersion`, `UninstallString "<dir>\uninst.exe"`, `DisplayIcon`, `InstallLocation`, `NoModify`, `NoRepair` |

Re-running Setup applies the chosen state idempotently: a newly ticked type is saved and associated, an unticked
one restored, an entry that someone else changed since is left alone (and its record dropped). After every change:
`SHChangeNotify(SHCNE_ASSOCCHANGED)`. Explorer starts one `pica.exe` per selected file; Pica's single instance hands
them to the running window.

## Building

```sh
# after pica.exe is built (root Makefile):
~/Documents/86box/samba/w98 build 'editor\setup'          # -> setup\setup.exe, zero warnings at /W3
~/Documents/86box/samba/w98 build 'editor\setup' shtest   # -> setup\obj\shtest.exe (test helper)
python3 tools/patch_ver.py pica.exe setup/obj/pica09.exe 0.9
~/Documents/86box/samba/w98 build 'editor\setup' old      # -> setup\obj\setup09.exe (installs "0.9")
```
`setup/Makefile` variables: `PICA=..\pica.exe`, `README=..\README.TXT`, `LICENSE=..\LICENSE.TXT` (point README at
the release copy if T18 keeps it elsewhere: `nmake README=..\release\Pica-1.0\README.TXT`). The payload version
shown by Setup is read from the embedded pica.exe's version resource, so a setup always tells the truth about what
it installs. `setup.exe` and `obj\uninst.exe` link the same two objects (`setup.cpp` UI/flow, `inst.cpp` work);
`uninst.exe` has no payload and therefore always uninstalls.

## Testing (VM A)

`setup\obj\shtest.exe` drives the shell like Explorer: `exec <verb> <file>` (ShellExecuteEx), `waitpica <title>`,
`close`, `ver <file>`, `assoc <.ext>...`, `key HKCR|HKLM <path>`, `waitfile`, `wininit`. See the T19 section of
`docs/HANDOFF.md` for the test matrix and results. For a clean-registry check export `HKEY_CLASSES_ROOT` and the
Uninstall key with `regedit /e` before installing and after uninstalling and compare.

## Release (T18)

Ship both: the portable zip (`pica-1.0.zip`, unpack anywhere, settings next to the exe) and `setup.exe`.
Build the release setup with `w98 build 'editor\setup' clean` then `w98 build 'editor\setup'` after the final
`pica.exe` (and README.TXT) exist: the Mac/VM clock difference once kept nmake from rebuilding `obj\setup.res`, so a
stale payload is possible without `clean`. Check the payload: the wizard title shows the embedded pica.exe version.

## Known limits

- A file replaced through WININIT.INI comes back with its 8.3 name in capitals (`PICA.EXE`); harmless.
- The temp copy `%TEMP%\PICAUN.EXE` stays until the next restart (WININIT.INI deletes it).
- The verbs copied into `Pica.<ext>` are a snapshot: a program installed later that adds a verb to `txtfile` won't
  show it on .txt until Setup runs again (repair) or Pica is uninstalled. Shell extensions (`shellex`) of the old
  ProgID are not copied.
- No disk-space check (Pica is under 1 MB). Installing into a different folder while Pica is installed is refused
  (uninstall first).
