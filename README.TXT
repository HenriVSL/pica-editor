PICA 1.0
========

A text editor and hex editor in one, for Windows 98.

Pica opens every file in a tab and shows it as text with syntax colouring,
or as bytes in a hex view. Both views edit the same document with one undo
history, and switching between them never changes a single byte. Saving is
safe, unsaved work is backed up while you type, and after a crash or a
power cut the next start brings your tabs back.

Pica is small (one exe under 800 KB, no DLLs or runtime of its own to
install) and quick on a Pentium II.


REQUIREMENTS
------------
- Windows 98 or 98 SE (tested on 98 SE). Other Windows versions are
  untested.
- Pentium-class PC; Pica was tuned on a Pentium II 233 MHz with 128 MB RAM.
- About 1 MB of disk space, plus room for backups of unsaved work.


INSTALL
-------
There are two ways. Both install the same pica.exe.

1. Setup (pica-1.0-setup.exe)

   Run it and follow the wizard:
   - Folder: C:\Program Files\Pica by default.
   - Options: Start menu shortcut (on), desktop shortcut (off),
     "Open with Pica" in the right-click menu of every file (on).
   - File types: ticked types open in Pica when you double-click them.
     Ticked by default: .txt .log .ini .c .cpp .h .asm .py .md .mak.
     Explorer keeps their names ("Text Document"), their Print command and
     the New menu. For scripts (.bat .cmd .reg .inf .vbs .js) double-click
     still runs them; Pica becomes their Edit command.
   - "Start Pica now" on the last page.

   Setup remembers the previous file associations and puts them back when
   you uninstall.

   Upgrading: run the setup of the newer version. It finds the installed
   copy, says "Upgrade Pica 1.0 -> 1.x" and replaces only the program
   files. Your settings, session, backups and language files are never
   touched. If Pica is running, Setup asks you to close it first (it never
   closes Pica for you, so your unsaved work is saved properly). Running
   the same version again repairs the install. An older setup over a newer
   Pica asks first.

   Uninstall: Control Panel > Add/Remove Programs > Pica 1.0 (or
   uninst.exe in the Pica folder). It asks whether to delete your settings,
   session and backups too (default: keep them).

   Silent install for the curious: pica-1.0-setup.exe /S installs with the
   defaults; /D=<folder> (last on the line) picks the folder;
   /TYPES=none|all|default|.txt,.c picks the file types; /LOG=<file>
   writes a log. "pica-1.0-setup.exe /UNINSTALL /S" removes Pica silently
   (add /PURGE to delete the settings as well).

2. Portable (pica-1.0.zip)

   Unzip it to C:\ (you get C:\PICA) or anywhere else, and run pica.exe.
   Nothing goes into the registry. To uninstall, delete the folder. To
   upgrade, unzip the new version over the old one: the zip holds no
   settings, so yours are kept.

   No file associations or right-click menu in this case, but you can drop
   files on the window, or drag them onto pica.exe or a shortcut to it.


WHERE YOUR SETTINGS LIVE
------------------------
Pica keeps everything in its settings folder:

  - the folder of pica.exe, when Pica can write there (on Windows 98 that
    includes C:\PICA and C:\Program Files\Pica);
  - otherwise C:\WINDOWS\Application Data\Pica;
  - or the folder you give with -cfg <folder>.

  pica.ini          settings, window position, recent files, search history
  session1.ini      open tabs, carets, bookmarks (two copies, so a crash
  session2.ini      while writing one never loses both)
  backup\*.BAK      unsaved changes, written a few seconds after you stop
                    typing
  Pica Help.md      the help text, written when you open Help
  langs\*.lng       your own syntax colouring files (optional, next to
                    pica.exe)

Help > About Pica shows the settings folder.


USING PICA
----------
Press F1 for the keyboard shortcuts and the regular expression cheat sheet
(Help opens as a read-only tab you can search with Ctrl+F).

  Ctrl+Shift+H    switch between the text view and the hex view
  Ctrl+F / Ctrl+H Find / Replace (Normal, Extended and Regex modes)
  Ctrl+Shift+F    Find in Files (results panel; F4 = next result)
  Ctrl+I          incremental search
  Ctrl+G          go to a line (text view) or an offset (hex view)
  Ctrl+D, Ctrl+L, Ctrl+Shift+Up/Down, Ctrl+Q
                  duplicate, cut, move and comment lines
  Ctrl+F2, F2     bookmarks
  Tools > Options all settings, each with a one-line explanation

The status bar is clickable: position (Go To), syntax, encoding, line
endings, INS/OVR and TEXT/HEX.


FEATURES
--------
- Tabs: drag to reorder, middle-click to close, right-click for Close
  Others, Copy Full Path and Open Containing Folder. 10 recent files,
  drag-and-drop, single instance (files from Explorer open as new tabs in
  the running Pica).
- Syntax colouring: C/C++, resource scripts, makefiles, batch files, INI,
  assembly, Python, HTML, JavaScript, CSS, XML, Markdown and diffs.
- Encodings: ANSI, OEM (DOS), UTF-8 with or without BOM, UTF-16 LE/BE, and
  more code pages under Reload as (1250, 1251, 1253, 1254, 1257, 437, 850,
  852, 866, KOI8-R). Line endings CRLF / LF / CR, mixed files kept as
  they are. Bytes that aren't valid text survive load and save unchanged.
- Hex view: overwrite and insert, nibble caret, changed bytes in red,
  byte-pattern search (DE AD ?? EF), go to offset (1F40, #8000, end-10,
  50%), data inspector (int8 to double, little or big endian). Binary
  files open in the hex view.
- Search: regular expressions with groups, lookahead and \U \L case
  changes in replacements, whole word, in selection, Count, Find All,
  Mark All, Replace All (one undo step), Replace All in Open Tabs, Find in
  Files with masks (*.cpp;*.h;!*.bak).
- Editing: unlimited undo/redo, auto-indent, block indent, tabs or spaces,
  word wrap, show whitespace and line endings, brace matching (Ctrl+B),
  UPPER/lower case, line commands.
- Safe files: saves go to a temporary file first, so you never get a
  half-written file. Optional .bak copy. Files changed by another program
  reload by themselves (or ask, if you have edited them).
- Large files: up to 16 MB in memory, up to 4 GB read on demand while you
  scroll. Megabyte-long lines stay responsive.


COMMAND LINE
------------
  pica.exe [-n<line>] [-hex] [-ro] [-multi] [-nosession] [-cfg <folder>]
           [files...]

  -n120           go to line 120 in the next file
  -hex            open the next file in the hex view
  -ro             open the next file read-only
  -multi          start a separate Pica (normally files go to the running
                  one)
  -nosession      don't restore or save the session
  -cfg <folder>   use another settings folder


KNOWN LIMITS
------------
- Not in 1.0: printing, macros, plugins, split view, column selection,
  folding, autocomplete, replace in files, compare, key remapping,
  user-defined languages (beyond .lng files), proportional fonts,
  right-to-left and complex scripts.
- Fixed-width fonts only (Courier New 10 pt by default; Tools > Options
  > Editor picks another). Characters no installed font can draw show as
  a grey "?" cell (the bytes stay as they are). Right-to-left text is
  drawn left to right. Asian input methods (IME) are best effort and
  untested.
- Files over 16 MB are read on demand: their unsaved changes are not
  backed up, their encoding can't be converted, and editing starts once
  Pica has counted the lines (about 15 seconds for 100 MB on a slow
  disk; you can look at the file meanwhile).
- Files of 8 MB or more open without colouring and word wrap (Document >
  Syntax turns colouring back on). Only the first 16,384 characters of a
  line are coloured.
- Regular expressions: no lookbehind, atomic groups, named groups or
  \p{...}. Backreferences and lookahead see the current line plus up to
  64 KB after it.
- Hex view: 16 bytes per row and hex offsets only; the data inspector is
  read-only.
- Undo of a Replace All with thousands of matches can take a second
  without a progress window.
- Setup: the install folder can't be changed during an upgrade (uninstall
  first). A file that was in use during an upgrade comes back with its
  name in capitals (PICA.EXE) after the restart; that's harmless.


ABOUT
-----
Pica is all our own code: the text buffer (a piece tree), undo, the
rendering, syntax colouring, the regular expression engine, encodings,
the hex view and the installer were written from scratch in C++ for
Visual C++ 6.0, with no editing component, no libraries and no runtime
beyond Windows 98 itself. It was built and tested inside Windows 98 SE.

"Pica" is the typewriter pitch of 10 characters per inch, the classic
monospace size.

Made by Henri Lavikainen with Claude (Anthropic) coding agents, 2026.

Source code: https://github.com/HenriVSL/pica-editor


LICENSE
-------

MIT License

Copyright (c) 2026 Henri Lavikainen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
