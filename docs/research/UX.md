# UX research: text + hex editor for Windows 98 SE

Desk research for `dev/editor` (phase 1). Goal: decide **what 1.0 contains and how it behaves**,
so the result is small and polished rather than broad. Sources are linked inline; forum quotes are
paraphrased or quoted briefly. Architecture questions (buffer, big files, Unicode on Win9x, regex
engine) are in `ARCH.md`; this file only notes where a UX decision puts a requirement on them.

Contents
1. What people use and love in Notepad++, and what annoys them
2. Win98-era editors (1999-2003): what they offered, what a Win98 user expects
3. Hex editing UX
4. Find / replace UX
5. Encoding and line-ending UX
6. Session and robustness UX
7. Keyboard shortcuts for 1.0
8. Recommended 1.0 feature list (must / should / later / cut), languages
9. Main window sketch and menu structure

---

## 0. Top findings in one screen

1. **Data safety is the feature people care about most and notice least.** The loudest Notepad++
   complaints are lost sessions, empty or NUL-filled files after a crash, and lost unsaved tabs. A
   Win98 machine crashes and loses power more often than a modern PC, so robustness is the place to
   spend polish (safe save, crash-proof backup, session written often).
2. **The loved Notepad++ core is small:** tabs, syntax colouring, line numbers, find/replace with
   regex and "Extended" escapes, Find in Files with a clickable results panel, unsaved tabs that
   survive a restart, encoding/EOL shown and changeable, and a handful of line shortcuts (Ctrl+D,
   Ctrl+L, Ctrl+Shift+Up/Down). Our agreed scope already covers almost all of it.
3. **Avoid what annoys:** modal update nags at startup, cluttered toolbars and over-deep
   preferences, silent encoding guesses that turn out wrong, confusing "Encode in" vs "Convert to",
   repeated "file changed, reload?" popups, and a Replace-in-Files button with no undo.
4. **Hex view must show the real bytes on disk.** The Notepad++ HEX-Editor plugin's main defect is
   that it shows the editor's decoded/re-encoded buffer, not the file, so BOMs, UTF-16 and NUL bytes
   come out wrong. Our hex view and text view must be two views of one byte-exact document with one
   undo history.
5. **Hex defaults that people expect (HxD, UltraEdit, 010 Editor):** offset | hex | characters,
   16 bytes per row, hex offsets, **overwrite by default**, insert as an explicit mode shown loudly,
   modified bytes in red, Tab to switch panes, Ctrl+G go to offset, search by hex bytes or text.
6. **Win98 users expect a standard Win32 look:** menu bar, flat 16x16 toolbar, tabs, status bar,
   standard Open/Save/Font dialogs, a property-sheet Options dialog, system colours. Period editors
   were MDI with file tabs; tabs alone are enough.
7. **VC6 and Notepad++ agree on many keys** (Ctrl+F2/F2 bookmarks, F3, Ctrl+F3, Ctrl+G, F4 next
   result, Ctrl+U/Ctrl+Shift+U case). A Win98 hobbyist who lives in VC6 will feel at home if we use
   that overlap, plus Ctrl+I incremental search from VC6.

---

## 1. Notepad++ and friends: what people love, what they hate

### 1.1 Why people pick it over Notepad

The reasons repeat across reviews and forum threads:

- **Syntax highlighting + strong find/replace** are the two headline reasons
  ([Windows Central](https://www.windowscentral.com/software-apps/windows-11/should-you-use-notepad-or-notepad-on-windows-11),
  [dignited](https://www.dignited.com/104675/notepad-vs-notepad-plus-plus)).
- **Regex find/replace for bulk edits.** "powerful regex-based Find and Replace feature, which
  allows me to make bulk edits quickly and accurately"
  ([G2 review](https://www.g2.com/survey_responses/notepad-review-616961)).
- **Find in Files** to trace a name across many files, with filters and subfolders
  ([MakeUseOf](https://www.makeuseof.com/tag/3-handy-builtin-notepad-features-beginners-windows/),
  [N++ forum](https://community.notepad-plus-plus.org/post/95078)).
- **Tabs, and unsaved tabs that come back after a restart.** Introduced in v6.6 as "session snapshot
  and periodic backup": on exit you are not asked to save; on start the unsaved and untitled
  documents come back ([changelog via Softpedia](https://www.softpedia.com/progChangelog/Notepad-plus-plus-Changelog-13216.html),
  [forum](https://community.notepad-plus-plus.org/topic/14636/previous-session-tabs)). Many users treat
  untitled tabs as a scratchpad that never needs saving
  ([forum](https://community.notepad-plus-plus.org/post/36891)).
- **Free, fast to start, small.**
- **Column mode (Alt+drag)** for structured data, and the **Compare** plugin, are frequently named
  extras ([MakeUseOf](https://www.makeuseof.com/tag/3-handy-builtin-notepad-features-beginners-windows/)).
- **Line operations by key:** Ctrl+D duplicate, Ctrl+L delete line, Ctrl+Shift+Up/Down move line,
  Ctrl+Q comment ([planetozh](https://planetozh.com/blog/2012/10/commanding-your-text-editor-the-notepad-edition/),
  [defkey](https://defkey.com/notepad-plus-plus-shortcuts)).
- Smaller niceties people mention: double-click a word highlights all other occurrences ("smart
  highlighting"), bracket matching, Extended search mode (`\r\n`, `\t`, `\x20` without regex), the
  Mark tab, bookmarks.

**Takeaway:** the "can't live without" set is tabs + colouring + line numbers + find/replace (regex
and Extended) + Find in Files + session/unsaved-tab restore + encoding/EOL control + a few line
keys. Plugins, macros, folding, autocomplete and the document map are secondary for most users.

### 1.2 What people complain about (avoid these)

| Complaint | Evidence | Our rule |
|---|---|---|
| **Lost session / lost unsaved tabs** after update, crash, or restart | [forum: lost session after update](https://community.notepad-plus-plus.org/topic/19835/lost-session-after-update/4), [forum: crash lost auto-saved docs](https://community.notepad-plus-plus.org/topic/20548/searching-a-word-in-a-large-directory-caused-n-crash-and-loss-of-auto-saved-open-documents/3), [lost session 2025](https://community.notepad-plus-plus.org/post/100070), GitHub #6133, #16001 | Session + backups written crash-safely and often; never overwrite a good backup with a partial one |
| **Files empty (0 bytes) or full of NULs after power loss** | Reproduced in v7.3.2: "The file has zero size - totally empty" ([forum](https://community.notepad-plus-plus.org/post/23560), [NUL thread](https://community.notepad-plus-plus.org/topic/13302/fix-corrupted-txt-file-null/2?page=1)) | Safe save: write temp file, flush, then swap; keep the old file until the new one is complete |
| **No save prompt on exit surprises people** (periodic backup on) | ["Closing Notepad does not ask to save"](https://community.notepad-plus-plus.org/post/40419), [auto save thread](https://community.notepad-plus-plus.org/topic/13202/auto-save-of-unsaved-files) | Keep the feature (it is loved) but make "unsaved" impossible to miss on restore; option to ask on exit |
| **Modal update dialog at startup** "when I'm least ready for it" | [forum](https://community.notepad-plus-plus.org/topic/21945/update-dialog-when-i-m-least-ready-for-it/4), [disable auto update](https://community.notepad-plus-plus.org/post/52791) | No updater, no network at all |
| **"File modified by another program, reload?" popups**, repeated per tab, on closing, with sync folders | [forum](https://community.notepad-plus-plus.org/post/102028), [forum](https://community.notepad-plus-plus.org/topic/22136/avoid-reload-popup-when-closing-notepad/4), [forum](https://community.notepad-plus-plus.org/post/71310) | Unmodified docs reload quietly; modified docs get a non-modal bar in the tab, not a popup storm |
| **Wrong encoding guess** (ANSI vs UTF-8, odd code pages; a single NUL flips detection) | ["it will try some heuristics to guess the encoding, but it's often wrong"](https://community.notepad-plus-plus.org/topic/188/how-to-adjust-encoding-detection-algorithm-ansi-vs-utf-8-w-o-bom), [NUL case](https://community.notepad-plus-plus.org/post/58745) | Simple, predictable detection (BOM, then strict UTF-8, else ANSI); always visible; one click to reinterpret |
| **"Encode in" vs "Convert to" is confusing** | Long-time contributor: these concepts "are not easy to assimilate" ([forum](https://community.notepad-plus-plus.org/post/36799), [manual](https://npp-user-manual.org/docs/encoding)) | Name them by effect: "Reload as..." and "Save as encoding / Convert to..." (section 5) |
| **Cluttered / hard-to-find preferences**, settings moved between versions, no tooltips | ["now I cannot find it even after 15 min searching"](https://community.notepad-plus-plus.org/post/99493), [cplusplus.com thread](https://cplusplus.com/forum/lounge/77448/), [toolbar toggle buried](https://community.notepad-plus-plus.org/post/91240) | Few options; one property sheet with 4-5 pages; every option has a one-line explanation |
| **Updates break things / change UI** (8.6 tab close buttons, thicker margin, nested menus) | [forum summary](https://community.notepad-plus-plus.org/post/92032), ["new updates ... often break the things that had been working"](https://community.notepad-plus-plus.org/topic/16796/you-ve-a-lot-of-reasons-to-hate-notepad/81) | Stable layout; don't add gimmicks (the Document Peeker was called "useless" and "utterly annoying", [forum](https://community.notepad-plus-plus.org/topic/13977/notepad-7-4-2-release/18?page=1)) |
| **Replace in Files has no undo**; Find All and Replace buttons sit together | [forum](https://community.notepad-plus-plus.org/post/29994), [request: separate tabs](https://community.notepad-plus-plus.org/post/29992) | 1.0 has Find in Files only; replace-in-files later, with backups |
| **Find dialog covers the match** | [forum](https://community.notepad-plus-plus.org/post/18772), [improve find dialog](https://community.notepad-plus-plus.org/post/41323) | Dialog moves itself away from the match; incremental search bar for quick finds |
| **Large files freeze** (colouring, no-newline files, navigation stutter) | [forum](https://community.notepad-plus-plus.org/post/89068), [forum](https://community.notepad-plus-plus.org/post/68395) | Large-file mode: colouring off above a threshold, progress + cancel when loading, very long lines handled |
| **Defaults people dislike** (no close button on tabs, odd tab width) | [cplusplus.com](https://cplusplus.com/forum/lounge/77448/) | Close button on the active tab, middle-click closes, tab width 4 default but per-language |
| **Plugins breaking / plugin admin** | [forum](https://community.notepad-plus-plus.org/topic/16796/you-ve-a-lot-of-reasons-to-hate-notepad/81) | No plugin system in 1.0 |

### 1.3 Lessons from the minimalist Notepad replacements

- **Notepad2** (2004) and **Metapad** were loved because they stayed close to Notepad: small, fast,
  no install, no registry, but with syntax colouring (including BAT, INI, REG, INF, DIFF), bracket
  matching, Unicode, EOL/encoding control ([PCWorld](https://www.pcworld.com/article/447690/notepad2-review-syntax-highlighting-a-good-notepad-replacement-for-coders.html),
  [Notepad2.txt](https://www.flos-freeware.ch/doc/Notepad2.txt)). Their missing tabs were the most
  common reason people moved to Notepad++.
- **Takeaway:** "Notepad2 with tabs, session restore and a real hex view" is a good mental model
  for our 1.0. Portable (settings in an INI next to the exe or in the Windows folder), single small
  exe, instant start.

---

## 2. Win98-era editors (1999-2003)

### 2.1 What they offered

| Editor | Period features that mattered | Notes |
|---|---|---|
| **TextPad 4** (1999-2000) | MDI + **file tabs** and a **Document Selector** pane, regex with backreferences, block (column) select, bookmarks in the margin, **Clip Library** pane, compare, spell check, user tools, very configurable keys/colours | Review (TextPad 4.31, 2000) calls the interface "the very best thought-out part"; criticises nag screens, basic macros, no folding ([gnosis.cx](https://gnosis.cx/publish/programming/review_textpad.html)). Find in Files with subfolders was a registered-only feature in early versions ([readme](https://discmaster.textfiles.com/view/30678/Dominator%204.iso/win95prg/tpad32/readme.txt)) |
| **EditPlus 2** (1999-2002) | HTML/CSS/PHP/C/Java/Perl colouring, custom syntax files, **Document Selector**, **user tools with Output Window** (double-click an error line jumps to file/line), **Hex Viewer** toggled with one command, cliptext, column select, FTP upload, built-in browser preview, auto-completion | ([features](https://editplus.com/feature.html), [2.x manual](https://discmaster.textfiles.com/file/11709/ftp.editplus.com.tar/ftp.editplus.com/epp221.zip/editplus.hlp/editplus.pdf)) |
| **UltraEdit 6-9** (1998-2002) | **Hex mode toggled with Ctrl+H on the same document**, column mode, huge files, macros, FTP, file tabs, find in files | Hex typing is always overwrite; inserting/deleting bytes is a separate command (Ctrl+D dialog) ([UE wiki: Hex edit](https://wiki.ultraedit.com/Hex_edit), [Hex insert/delete](https://wiki.ultraedit.com/Hex_insert/delete)) |
| **Programmer's File Editor (PFE)** 1.01 (1999, freeware) | Standard **MDI**, very large files, **run a program and capture its output into a window**, keyboard remapping, good printing | Discontinued 1999 but kept by fans ([Wikipedia](https://en.wikipedia.org/wiki/Programmer%27s_File_Editor), [review](https://gnosis.cx/publish/programming/review_pfe.html)) |
| **PSPad** (2001+, freeware) | MDI with tabs, **integrated hex editor**, regex, FTP, macros, code explorer | PC Pro praised features but criticised slowness on large files ([Wikipedia](https://en.wikipedia.org/wiki/PSPad)) |
| **Notepad2 / Metapad** | See 1.3 | |
| **HxD 1.7.7** | Last HxD that runs on Windows 95-7 ([HxD page](https://mh-nexus.de/en/hxd/)) | The reference hex UX on our platform |

### 2.2 What mattered then and still matters

- **Tabs or a document list** to switch files fast ("Much faster than selecting on Window menu",
  EditPlus). Tabs won; MDI tiling was rarely used.
- **Run a tool, capture output, double-click to jump to the error** (PFE, TextPad, EditPlus). This
  was the most beloved programmer feature of the era. For a VC6/nmake hobbyist it is valuable, but
  it is not in the agreed scope: first candidate for 1.1.
- **Regex with backreferences**, find in files.
- **Hex view of the same document** (UltraEdit, EditPlus, PSPad). Users liked the single toggle.
- **Customisable colours and keys** were selling points; today most people keep defaults. One
  colour scheme done well is enough for 1.0.
- What dated badly: nag screens, FTP/browser integrations, clip libraries, spell checkers.

### 2.3 What a Win98 user expects (look and feel)

- **Standard Win32 controls only**, so the program looks like it belongs on Win98: menu bar with
  `&` accelerators and shortcut text, flat toolbar with 16x16 16-colour icons and tooltips
  (comctl32 4.72 on Win98 supports `TBSTYLE_FLAT`), `SysTabControl32` tabs, `msctls_statusbar32`
  with parts, common Open/Save (Explorer style, multi-select, encoding combo in the Save dialog
  through a hook template), `ChooseFont`, a property sheet for Options, standard `MessageBox`es.
- **System colours** (`COLOR_WINDOW`, `COLOR_HIGHLIGHT`, `COLOR_BTNFACE`) for the default scheme,
  so High Contrast and custom Win98 schemes work. Syntax colours on top of the window colour.
- **Fonts:** default Fixedsys or Courier New 10 pt or Lucida Console; any fixed-pitch font via
  ChooseFont. (Proportional fonts: not in 1.0.)
- **MDI vs tabs:** tabs on a single main window; no MDI child windows. Keep a **Window** menu (Win98
  users look there for the open-document list) that lists open tabs and has Next/Previous.
- **Single instance:** opening a file from Explorer while the editor runs adds a tab to the running
  window (Notepad++ default). Drag and drop files onto the window or tab bar opens them.
- **Command line:** `editor.exe file1 file2 -n<line>` so it can be used from batch files and VC6
  "Tools" menu.

---

## 3. Hex editing UX

### 3.1 What makes HxD and peers good

From the HxD feature list and changelog ([features](https://mh-nexus.de/en/hxd/),
[changelog](https://mh-nexus.de/en/hxd/changelog.php)), 010 Editor
([hex editor manual](https://sweetscape.com/010editor/manual/UsingHexEditor.htm)) and UltraEdit
([wiki](https://wiki.ultraedit.com/Hex_edit)):

- **Layout:** offset column | hex bytes | character column. Non-printable bytes show as `.`. A ruler
  of column offsets (`00 01 02 ...`) at the top (010, HxD). Bytes can be grouped (1/2/4/8 per group,
  HxD); a wider gap after 8 bytes helps reading.
- **Bytes per row:** 16 is the universal default; HxD allows a fixed number (up to 4096 since 2.0).
- **Offset base:** hex by default, decimal or octal optional; HxD labels the base in the header
  "(h)/(d)/(o)" (1.7.0). Status bar shows the caret offset; clicking it opens Go To (HxD 1.7.0).
- **Caret:** nibble-level caret in the hex pane, byte caret in the char pane; the byte under the
  caret is highlighted in the other pane (010). **Tab switches panes** (010). Ctrl+Left/Right moves by
  nibble/group (HxD).
- **Overwrite vs insert:** overwrite is the usual default because binary edits are in-place
  ([KDE Okteta maintainer](https://bugs.kde.org/show_bug.cgi?id=472751),
  [HHD Hex Editor docs](https://docs.hhdsoftware.com/hex/definitive-guide/editor-windows/simple-editing/insert-mode.html)).
  UltraEdit has *no* insert typing at all; insert/delete is an explicit command. 010 shows insert as
  a vertical-bar caret and overwrite as an underscore caret, INS/OVR in the status bar, clickable,
  and puts an asterisk next to the file size after insertions so the user sees the file grew.
  HxD 2.5 fixed paste in overwrite mode to only overwrite (earlier it deleted then inserted).
- **Modified bytes highlighted** (HxD: red and bold, configurable, since 1.5).
- **Paste vs "paste write":** HxD has Paste (insert) and Paste Write (overwrite) as separate
  commands (1.7.0).
- **Selection:** Shift+arrows/mouse, mirrored in both panes; double-click selects a word; "Select
  block" dialog with start + end or length (HxD 1.2/1.7.1); status bar shows selection length.
- **Go to:** offset in hex or dec, relative (`+`/`-`) and absolute; the dialog shows the computed
  target and gives a clear error when out of range (HxD 2.3/2.4).
- **Search:** text (ANSI, UTF-16), hex bytes, integers, floats; forward/backward/all
  (HxD 1.0-2.0). HxD 2.0 replaced a type combo box with a tab per data type.
- **Data inspector:** shows the bytes at the caret as int8/16/32/64 (signed/unsigned, LE/BE),
  float/double, date/time, char, binary; editable (HxD 2.0, 2018). Hex Editor Neo and 010 have one;
  users of modern hex editors expect it, but it is a convenience, not core.
- **Large files:** HxD opens any size instantly (memory-mapped / paged); progress windows with
  cancel for long operations. Undo that does not store whole copies (HxD 1.5).
- **Unlimited undo**, including across insert/delete.

### 3.2 Text <-> hex in one editor

| Editor | How you switch | Sync |
|---|---|---|
| UltraEdit | Ctrl+H toggles the same window between text and hex | Hex and char panes update each other on each keystroke ([wiki](https://wiki.ultraedit.com/Hex_edit)) |
| EditPlus | "Hex Viewer" toggles back and forth with one command | Viewer only in older versions |
| 010 Editor | "Edit As" per file (Text / Hex / ...), toggle button in toolbar, Edit As shown in status bar ([menu](https://www.sweetscape.com/010editor/manual/MenuView.htm)) | One document, two editor windows |
| PSPad | Opens the file in a hex editor tab | Separate document |
| N++ HEX-Editor plugin | Ctrl+Alt+Shift+H toggles a hex view over the Scintilla buffer | Reads the *decoded* buffer (see pitfalls) |

### 3.3 Pitfalls of the Notepad++ HEX-Editor plugin (do not repeat)

- **Shows the editor buffer, not the file.** A user concluded the plugin "gets the contents of the
  file from the Scintilla editor object, not from the bytes on the disk", so UTF-16 / ANSI files
  appear as UTF-8, and byte counts are wrong after autodetection
  ([forum](https://community.notepad-plus-plus.org/post/50111), [forum](https://community.notepad-plus-plus.org/post/50126)).
- **Encoding-related breakage:** UTF-16 BE BOM shows extra characters, BOM errors, Windows-1251
  issues, "HexEdit misinterprets file and saves incorrectly", hex search fails above `7F` with
  non-standard encodings, Unicode search ignores NULs
  ([open issues](https://github.com/chcg/NPP_HexEdit/issues): #80, #88, #92, #76, #118, #77).
- **Copying 0x00 bytes pastes 0x20** (#72, #120): clipboard text cannot carry NULs.
- **"HEX-Editor itself changes the file contents"**: autodetection silently "corrects" bytes
  ([SourceForge #355](https://sourceforge.net/p/npp-plugins/bugs/355)).
- **Session data loss:** v0.9.14 "makes unsaved new document content disappear on the next session"
  (#106). **Crashes** with selections and .nfo files ([forum](https://community.notepad-plus-plus.org/post/57823)).
- **No keybind to switch dump/hex pane** (#89) and insert/delete of bytes was unclear
  ([forum](https://community.notepad-plus-plus.org/post/43809)).

### 3.4 Our hex design rules (requirements for ARCH/DESIGN)

1. **One document, two views.** Each tab holds one document. The hex view shows the exact bytes
   the file would have if saved now: BOM, CR/LF, UTF-16 byte order, everything. The text view is a
   decoding of those bytes. **Switching views never changes a byte.**
2. **Lossless round trip.** If the bytes are not valid in the current encoding (bad UTF-8, odd
   UTF-16 length), the text view shows a visible placeholder glyph for each bad byte and keeps the
   original bytes; saving from text view writes them back unchanged. If the architecture cannot
   guarantee this, the text view becomes read-only for that file with a clear bar explaining why.
3. **One undo history** across both views. Undo in hex can undo a text edit and vice versa.
4. **Toggle on the same tab** (View > Hex View, toolbar button, status-bar "TEXT/HEX" field,
   keyboard Ctrl+Shift+H, see section 7). Not side by side in 1.0 (split view is deferred).
5. **Caret and selection sync on toggle:** text caret -> byte offset of the first byte of that
   character (after BOM); text selection -> byte range covering the selected characters.
   Hex caret -> the character containing that byte (snap to its start); hex selection -> characters
   touched. Scroll so the caret line is in the same screen position if possible.
6. **Binary detection:** if a file has NUL bytes in its first 64 KB and is not UTF-16 (BOM or
   strong pattern), open it in hex view, status bar says "Binary: opened in hex view". User can
   still switch to text.
7. **Overwrite is the default** in hex view, separately remembered from text view's Ins state
   (010 does this). Insert mode is toggled with Ins, shown in the status bar as a highlighted
   **INS**, the caret becomes a bar instead of a block, and the status bar shows the file size with
   `+N` after insertions. Del removes the byte under the caret in either mode (an explicit
   action, as in HxD and 010); it is undoable and the size indicator shows the change. Backspace in
   overwrite mode only moves the caret left, so it can never shift data by accident.
8. **Paste:** paste of hex text (e.g. `4D 5A 90 00`) into the hex pane is parsed as bytes; paste of
   other text into the char pane writes its bytes in the file's encoding. Paste follows the current
   mode (overwrite replaces N bytes, insert inserts). Copy offers "Copy" (bytes as raw, kept in a
   private clipboard format so NULs survive) and "Copy as hex text".
9. **Modified bytes in red** until saved. Offsets in hex by default, toggle to decimal.
10. **Layout:** `00001F40  4D 5A 90 00 03 00 00 00-04 00 00 00 FF FF 00 00  MZ..............`
    16 bytes per row with a gap/dash after 8; option 8/16/32 or "fit to window". Offset width
    grows with file size (8 hex digits up to 4 GB).
11. **Status bar in hex view:** `Offset 0x1F4A (8010)  Sel 16 bytes  Byte 0x4D 77 'M'  OVR  Size 81 920`.
12. **Go to (Ctrl+G):** one edit field accepting `1F40`, `0x1F40`, `1F40h`, `8000` (with a hex/dec
    radio), `+100`, `-20`, `end-16`; shows the resolved offset live.
13. **Find (Ctrl+F in hex view):** the same Find dialog with a "Hex bytes" search mode in addition
    to Text; text search in hex view searches the encoded bytes (option: also UTF-16LE).
    Integers/floats search: later.
14. **Data inspector:** "should" for 1.0, as a small optional panel or a status-bar tooltip:
    int8/uint8, int16/uint16, int32/uint32 (LE, with a BE checkbox), float, double, and the char.
    Read-only in 1.0.

---

## 4. Find / replace UX

### 4.1 Notepad++'s dialog: strengths and weaknesses

From the [manual](https://npp-user-manual.org/docs/searching/):

Strengths
- One modeless dialog with tabs **Find / Replace / Find in Files / Mark**; Ctrl+F, Ctrl+H,
  Ctrl+Shift+F open it on the right tab. Options: match case, whole word, wrap around, backward, in
  selection; search mode **Normal / Extended (`\n \t \x20`) / Regular expression**; ". matches
  newline".
- **Count**, **Find All in Current Document**, **Find All in All Opened Documents** send results to
  the results panel. **Mark All** highlights all matches.
- Results panel: tree of searches -> files -> lines, double-click/Enter jumps, **F4/Shift+F4**
  next/previous result, Delete removes entries, "find in these results".
- **F3 / Shift+F3** next/previous without the dialog; **Ctrl+F3** finds the word or selection under
  the caret ("Select and Find Next").
- **Incremental search** bar at the bottom (Ctrl+Alt+I) with highlight-all and an "N/M" count.
- Find in Files: directory, wildcard filters (`*.cpp *.h`, `!` to exclude), subfolders, hidden.

Weaknesses (from forum threads and the manual itself)
- **The dialog covers the text** ([forum](https://community.notepad-plus-plus.org/post/18772)).
  Workarounds are transparency sliders and moving it manually.
- **Invalid regex is only reported when you press Find Next**, and whole-word is disabled in regex
  mode with no explanation in the UI (manual).
- **Replace in Files sits next to Find All, no undo** for files not open
  ([forum](https://community.notepad-plus-plus.org/post/29994)).
- **Results accumulate** across searches (tree grows) unless an option is set; many users never
  find "purge for every search".
- Too many buttons and checkboxes on one tab; "Replace All in All Opened Documents" has no warning.
- Regex dialect (Boost) surprises users who test patterns elsewhere.

### 4.2 Recommended design for 1.0

**Find dialog** (modeless, standard dialog look, remembers position):
- Tabs: **Find**, **Replace**, **Find in Files**. (No Mark tab: "Mark all" is a button on Find.)
- Fields are combo boxes with history (last 15). The Replace tab shares the Find field text.
- Opens pre-filled with the selection (if single-line) or the word under the caret.
- Options: Match case, Whole word, Wrap around, In selection (auto-ticked when a multi-line
  selection exists), direction Up/Down radio (Win98 Notepad has it, users expect it).
- Search mode radio: **Normal / Extended (\n, \t, \xNN) / Regex**. Extended is cheap and loved
  for CRLF work.
- **Regex helper:** a small `>` button next to the field pops up a menu of common tokens that it
  inserts (`.`, `\d`, `[...]`, `^`, `$`, `(...)`, `\1`, `*?`...). VC6's Find dialog had exactly this,
  so the hobbyist knows it. A "Regex help" item opens a short cheat sheet in the help file.
- **Live validation:** if the regex is invalid, show the error as red text inside the dialog while
  typing (no message box). Whole word in regex mode stays enabled and wraps the pattern in `\b`.
- Buttons, Find tab: **Find Next**, **Count**, **Find All** (to results panel), **Mark All**,
  Close. Replace tab: **Find Next**, **Replace**, **Replace All**, **Replace All in Open Tabs**
  (asks once, with a count; undoable per tab).
- **Auto-move:** when a match would be hidden under the dialog, move the dialog away (above/below
  the match). Status line inside the dialog: "12 matches", "Not found", "Wrapped to top"
  (no message box for not-found; beep optional).
- In hex view the same dialog gains a "Hex bytes" mode.

**Find in Files tab:** "Find what", "Files" (`*.cpp;*.h;*.c` default from the current tab's
type, history), "Folder" (defaults to current file's folder, `...` browse via
`SHBrowseForFolder`), Subfolders, Match case, Whole word, mode. **Find All** only; no Replace in
Files in 1.0. Shows progress in the results panel header and can be cancelled (Esc / Stop
button). Skips binary files (NUL in first 4 KB) by default.

**Results panel** (docked at the bottom, resizable splitter, closes with Esc or its X):
- Grouped per file: `M:\dev\x\main.cpp (3 hits)` then `  120: if (foo == bar)` with the match in
  bold or coloured. A summary line: `"foo" : 17 hits in 6 files (of 230 searched)`.
- Double-click / Enter jumps (opens the file if needed). **F4 / Shift+F4** next/previous result
  (same as VC6's next error). Ctrl+C copies selected lines.
- **New search replaces the old results** (simple); keep the previous one reachable with a
  "Previous search" dropdown at most. No tree of all historic searches.

**Incremental search (Ctrl+I, as in VC6):** a thin bar above the status bar (or inside it):
`Find: [foo____] (3/17)  [x] Match case  [Highlight all]`. Type to jump; Enter / F3 next,
Shift+Enter / Shift+F3 previous; Esc closes and leaves the selection. Not-found turns the field
background pink. This answers "the dialog covers the text" for most quick searches.

**No-dialog keys:** F3, Shift+F3, Ctrl+F3 (find selection/word, puts it in the history),
Ctrl+Shift+F3 (backwards).

**Smart highlight (should):** after a double-click selects a word, other occurrences on screen get
a light background. Cheap to draw (only visible lines), and frequently praised.

---

## 5. Encoding and line-ending UX

### 5.1 What confuses users

- Two kinds of change look the same in a menu: **reinterpret** (bytes stay, characters change) vs
  **convert** (characters stay, bytes change). Notepad++ calls them "Encode in X" and "Convert to X"
  in one Encoding menu; users and even regulars find it confusing
  ([manual](https://npp-user-manual.org/docs/encoding), [forum](https://community.notepad-plus-plus.org/post/36799)).
- Wrong autodetection and code-page guesses ([forum](https://community.notepad-plus-plus.org/topic/188/how-to-adjust-encoding-detection-algorithm-ansi-vs-utf-8-w-o-bom)).
- BOM: "UTF-8" vs "UTF-8 with BOM" labels changed between versions; BOM is invisible
  ([forum](https://community.notepad-plus-plus.org/post/12362)).
- Pure-ASCII files: whether they are "ANSI" or "UTF-8" is meaningless but the label still worries
  people.
- Converting to ANSI with characters that do not fit silently writes `?`.
- Line endings: mixed EOL files silently normalised or not; Mac CR files.

### 5.2 Win98-specific facts that shape defaults

- Win98 Notepad, VC6's compiler and resource compiler, `command.com` batch files and most tools of
  the era expect **ANSI (system code page, cp1252 in the West) with CRLF**. A UTF-8 BOM at the start
  of a `.cpp`, `.bat` or `.rc` file breaks them. So **new files default to ANSI + CRLF**, and we never
  add a BOM unless the user picks "UTF-8 with BOM".
- UTF-8 and UTF-16 files still come from modern machines via the `M:` share; they must open and
  round-trip perfectly.

### 5.3 Recommended UX

**Detection (simple and predictable):** BOM (UTF-8, UTF-16 LE/BE) -> else if the file has
non-ASCII bytes and is strictly valid UTF-8 -> UTF-8 -> else if it looks like BOM-less UTF-16
(many NULs at alternating positions in text-like data) -> UTF-16 -> else ANSI. Pure ASCII files are
shown as **ANSI** (saving them is identical either way). No code-page guessing in 1.0; OEM (cp437/850,
DOS) is selectable manually.

**Status bar (always visible, clickable):** `UTF-8` / `UTF-8 BOM` / `UTF-16 LE` / `UTF-16 BE` /
`ANSI` / `OEM`, and `CRLF` / `LF` / `CR` / `Mixed`. Left-click or right-click on a field opens its
popup menu (same as the Document menu items). No double-click-to-cycle: a popup only, so nothing
changes by accident.

**Document > Encoding submenu, two clearly named groups:**
```
Encoding >
  Reload as  >  ANSI | OEM (DOS) | UTF-8 | UTF-16 LE | UTF-16 BE      (reinterpret the bytes)
  ------------------------------------------------------------
  * ANSI                                                               (save as: convert)
    UTF-8
    UTF-8 with BOM
    UTF-16 LE (BOM)
    UTF-16 BE (BOM)
```
- **Reload as** rereads the file from disk with the chosen decoding. If the document has unsaved
  edits, ask: "Reloading discards your changes to X. Continue?" (for an untitled doc, it re-decodes
  the current bytes). It never marks the doc modified.
- The **radio list** below is "the encoding this document will be saved in". Picking another one
  converts: characters stay the same, the document becomes modified, and the hex view immediately
  shows the new bytes. If some characters cannot be represented (UTF-8 -> ANSI), show a dialog:
  "14 characters cannot be stored in ANSI and will become '?'. First one at line 12. Convert anyway /
  Cancel" and offer "Go to first".
- The Save As dialog has **Encoding** and **Line endings** combo boxes (hook template), like
  Win2000+ Notepad's Encoding combo, defaulting to the document's current values.

**Line endings:** Document > Line Endings > Windows (CRLF) / Unix (LF) / Mac (CR). Picking one
converts all line breaks (one undo step). Mixed files are shown as `Mixed`; we **keep each line's
original ending** when saving unless the user converts; new lines typed get the dominant ending.
View > Show Line Endings shows `CR LF` glyphs (and Show Whitespace dots/arrows), which is the best
way to explain EOL issues.

**Per-language defaults:** `.bat/.cmd/.ini/.rc/.dsp/.mak` -> ANSI + CRLF; `.sh/.py` keep what the
file has; new untitled docs use the Options default (ANSI + CRLF).

---

## 6. Session and robustness UX

### 6.1 What Notepad++ does and where it fails

- Session: open files, active file, caret/selection, bookmarks, language per file
  ([manual](https://npp-user-manual.org/docs/session/)). Periodic backup of unsaved/untitled docs to
  `%APPDATA%\Notepad++\backup` every few seconds; on exit no save prompt
  ([forum](https://community.notepad-plus-plus.org/topic/13202/auto-save-of-unsaved-files)).
- Optional "verbose backup": a timestamped `.bak` copy on each save
  ([preferences](https://npp-user-manual.org/docs/preferences/)).
- Failure modes reported: session.xml emptied while backup files still exist (users rebuild
  session.xml by hand), backups lost on crash or Windows restart, new-version path changes not
  recognising old entries, zero-length or NUL-filled files after power loss (see 1.2).
- File status auto-detection: modal reload prompts, triggered by sync tools, on close, for many tabs
  in a row ([forum](https://community.notepad-plus-plus.org/post/102974)).
- Large files: colouring disabled above a "large file" threshold (Preferences > Performance);
  navigation stalls remain.

### 6.2 Recommended behaviour

**Safe save (must):** write `name.~tmp` in the same folder, `FlushFileBuffers`, close; then replace
the original (Win9x has no `ReplaceFile`/`MoveFileEx`, so: rename original to `name.~old`, rename
temp to name, delete `.~old`; on any failure, leave the original and report). Preserve the file's
attributes; if the original is read-only, say so and offer Save As. Never truncate the original
before the new data is safely on disk. (ARCH to confirm the exact sequence on FAT32 and SMB.)

**Session restore (must):**
- Restored per tab: path, active tab, tab order, caret, selection, first visible line, text/hex
  mode, encoding and EOL choice, syntax override, bookmarks (if we ship bookmarks).
- Written **on every tab open/close/switch and on a timer** (not only on exit), atomically
  (write `session.~tmp`, then swap; keep `session.bak` = previous good copy). On load, if
  `session.ini` is missing or corrupt, use `session.bak`.
- Missing files on restore: skip them and show one status-bar note ("2 files from last session
  were not found"), not one dialog per file.

**Unsaved-work backup (must):**
- Untitled and modified documents are backed up a few seconds after the last edit (debounced,
  e.g. 5 s idle, at most every 30 s while typing) into `backup\` next to the settings. Each backup
  written to a temp name, then swapped; the previous good backup kept until the new one is complete.
- Default: **on**. Exit does not ask about untitled docs (they come back), but for **named files with
  unsaved changes** we show a single dialog listing them with Save All / Keep for next session /
  Discard / Cancel. This keeps the loved scratchpad behaviour while avoiding the "I thought it was
  saved" surprise. Option: "Ask to save untitled documents on exit".
- On restore, modified tabs show the modified marker (`*` and red-ish tab icon) and the title bar
  says "(unsaved changes restored)". If the file on disk changed since the backup was taken
  (size/mtime differ), show the file-changed bar (below) instead of silently overlaying old edits.

**File changed on disk (must):**
- Check when the app is activated (`WM_ACTIVATEAPP`) and when a tab becomes active; no polling
  timer needed. Compare size + last-write time against what we last read or wrote. FAT stores
  times at 2-second resolution, so also remember our own save time and ignore changes we caused.
- **Unmodified document:** reload silently, keep caret/scroll, show a status-bar note "Reloaded
  (changed on disk)". The reload is one undo step, so an accidental external change can be undone.
- **Modified document:** a yellow notification bar inside that tab: "This file was changed by
  another program. [Reload] [Keep my version]". Non-modal; no popup storm.
- **Deleted/renamed file:** bar "File no longer exists on disk. [Save] [Close tab]"; tab stays open
  and marked modified.

**Large files (must, details from ARCH/probe):**
- Show a progress bar with Cancel if opening takes longer than about half a second.
- Above a threshold (start with 4-8 MB on the 128 MB VM; ARCH/probe decide): colouring off, smart
  highlight off, word wrap off, status bar shows `Large file`. Hex view should handle much larger
  files than text view.
- Very long lines (minified files, binaries opened as text) must not freeze rendering.
- Files above what we can load: offer "Open in hex view (read-only)" instead of failing.

**Other safety:**
- Read-only files open with a `[Read-only]` marker; typing shows a bar offering "Save As".
- "Revert to saved" (File menu) with confirmation.
- Settings in a plain INI file (`editor.ini` next to the exe, or in the Windows folder when the exe
  folder is not writable); never reset settings on a parse error, just ignore bad lines.

---

## 7. Keyboard shortcuts for 1.0

Principles: Windows/Win98 conventions first (Notepad, WordPad, VC6), then the Notepad++ keys
people rely on. Where VC6 and Notepad++ differ, prefer the one that does not break a Windows
convention. No user remapping in 1.0 (later).

| Action | Key | Source / note |
|---|---|---|
| New / Open / Save | Ctrl+N / Ctrl+O / Ctrl+S | Windows |
| Save As / Save All | Ctrl+Alt+S / Ctrl+Shift+S | N++ (VC6 has no Save All key; N++ users expect Ctrl+Shift+S) |
| Close tab | Ctrl+W and Ctrl+F4 | N++ / Windows MDI |
| Reopen last closed tab | Ctrl+Shift+T | N++, browsers |
| Exit | Alt+F4 | Windows |
| Print | Ctrl+P | Windows (if printing ships) |
| Undo / Redo | Ctrl+Z / Ctrl+Y (also Alt+Backspace, Ctrl+Shift+Z) | Windows |
| Cut / Copy / Paste | Ctrl+X/C/V, Shift+Del, Ctrl+Ins, Shift+Ins | Windows (Win98 users still use the Ins forms) |
| Select all | Ctrl+A | Windows |
| Duplicate line/selection | Ctrl+D | N++ |
| Delete line | Ctrl+L (and Ctrl+Shift+L) | N++ / VC6 |
| Move line up/down | Ctrl+Shift+Up / Ctrl+Shift+Down | N++ |
| Join lines | Ctrl+J | N++ |
| Toggle line comment | Ctrl+Q | N++ |
| Upper / lower case | Ctrl+Shift+U / Ctrl+U | N++ and VC6 agree |
| Indent / unindent | Tab / Shift+Tab on selection | universal |
| Go to matching brace | Ctrl+B (and Ctrl+]) | N++ / VC6 |
| Find / Replace | Ctrl+F / Ctrl+H | Windows, N++, VC6 |
| Find in Files | Ctrl+Shift+F | N++ (VC6 too) |
| Find next / previous | F3 / Shift+F3 | Windows |
| Find selection/word next / prev | Ctrl+F3 / Ctrl+Shift+F3 | N++ and VC6 agree |
| Incremental search | Ctrl+I | VC6 (N++ uses Ctrl+Alt+I; Ctrl+I there is split lines, which we don't ship) |
| Next / previous search result | F4 / Shift+F4 | N++ and VC6 agree |
| Go to line (text) / offset (hex) | Ctrl+G | N++, HxD, VC6 |
| Toggle bookmark / next / previous | Ctrl+F2 / F2 / Shift+F2 | N++ and VC6 agree |
| Next / previous tab (MRU while Ctrl held) | Ctrl+Tab / Ctrl+Shift+Tab | VC6/N++ doc switcher |
| Next / previous tab (bar order) | Ctrl+PgDn / Ctrl+PgUp | browsers, N++ |
| Go to tab 1..9 | Alt+1..9? | Skip in 1.0 (Alt+digit clashes with nothing, but rarely used) |
| Toggle hex view | Ctrl+Shift+H | Ctrl+H is Replace (Windows convention); UltraEdit's Ctrl+H would break that. Also via menu, toolbar, status bar |
| Insert / overwrite | Ins | universal |
| Hex: switch hex/char pane | Tab | 010 Editor |
| Word wrap toggle | Alt+Z? / menu | (VS Code key); menu only is fine |
| Zoom in / out / reset | Ctrl+Num+ / Ctrl+Num- / Ctrl+Num/ and Ctrl+wheel | N++ |
| Options | (menu only) | Windows |
| Help | F1 | Windows |

Deliberately unused: Ctrl+K/Ctrl+Shift+K (N++ comment variants), Alt+drag column select, Ctrl+Space
autocomplete, F5 run, F11 fullscreen. Keep them free for later versions.

---

## 8. Recommended 1.0 feature list

Ranking: **Must** = 1.0 is not shippable without it. **Should** = small, high-value, do it if the
musts are polished. **Later** = good, but defer. **Cut** = not planned.

### 8.1 Must

| Feature | Reason |
|---|---|
| Tabs (close button on active tab, middle-click close, drag to reorder, modified marker, tooltip with full path, context menu: Close, Close Others, Copy Path, Open Folder) | Core reason people switch from Notepad |
| Open/Save/Save As/Save All/Revert, recent files (10), drag-and-drop open, single instance, command line | Basics; Win98 users expect MRU in the File menu |
| Safe save (temp + swap), never 0-byte files | Top data-loss complaint |
| Encodings ANSI / OEM / UTF-8 (+BOM) / UTF-16 LE/BE, CRLF/LF/CR, status bar display, Reload as / Convert to | Agreed scope; confusion is a top complaint, so the UX must be clear |
| Lossless round trip of undecodable bytes | Prerequisite for honest hex view |
| Syntax colouring for ~10 languages, auto-selected by extension, manual override menu | Headline feature |
| Line numbers, current line + column in status bar, auto-indent (copy previous line's indent), tab width + tabs/spaces per language | Basic code editing |
| Undo/redo, unlimited (memory permitting), grouped by typing runs | Basic; shared with hex view |
| Find/Replace dialog (Normal/Extended/Regex, case, whole word, wrap, selection, direction, count, find all, mark all, replace all in open tabs) | Agreed scope |
| F3 / Shift+F3 / Ctrl+F3, Ctrl+G | Most-used search keys |
| Find in Files with results panel (F4/Shift+F4) | Agreed scope; loved |
| Session restore + unsaved/untitled backup, crash-safe | Loved feature; top failure area in N++ |
| File-changed-on-disk handling (quiet reload / inline bar) | Avoid the popup annoyance, prevent lost edits |
| Hex view of the same document: layout, overwrite default + insert, nibble caret, Tab pane switch, selection sync, Go to offset, hex/text byte search, modified-byte colour, caret/selection sync on toggle, binary auto-detect | Agreed scope; the differentiator |
| Large-file mode + load progress/cancel | Avoid freezes on a 233 MHz machine |
| Ctrl+D, Ctrl+L, Ctrl+Shift+Up/Down, Ctrl+Q, case keys | Small code, much loved |
| Options property sheet (General, Editor, Files/Encoding, Colours/Font, Backup) with explanations | Few settings, easy to find |
| Help: one page of keys + regex cheat sheet (WinHelp `.hlp` or a plain text/HTML file) | Discoverability without a cluttered UI |

### 8.2 Should

| Feature | Reason |
|---|---|
| Incremental search bar (Ctrl+I) | Fixes "dialog covers text"; VC6 users know it |
| Word wrap toggle | Logs and prose; design the renderer so it fits, but it is the costliest "should" |
| Show whitespace / line endings | Explains encoding/EOL problems visually |
| Bracket matching highlight + Ctrl+B | Cheap, expected in a code editor |
| Current-line highlight | Cheap, readability |
| Smart highlight on double-click | Frequently praised, visible lines only |
| Bookmarks (Ctrl+F2/F2) in a margin | Same keys as VC6, cheap |
| Reopen closed tab (Ctrl+Shift+T) | Recovers mistakes |
| Zoom (Ctrl+wheel) | Fixedsys/Courier at 800x600 vs 1024x768 |
| Data inspector (read-only, int/float LE/BE) | Modern hex expectation, small |
| Hex: Copy as hex text, Paste hex text, decimal offset toggle, bytes-per-row option | Small finishing touches |
| Print (plain, with optional line numbers and header) | Win98 users expect File > Print; skip colour printing |
| Status bar click targets (encoding, EOL, language, INS/OVR, TEXT/HEX, Ln/Col -> Go to) | Fast, discoverable, no menus needed |

### 8.3 Later (post-1.0 candidates, in rough order)

| Feature | Reason to defer |
|---|---|
| Run tool + capture output + jump to error (nmake/cl) | Most-loved period feature for programmers; not in agreed scope; first 1.1 candidate |
| Replace in Files | No undo for closed files; needs backups + preview to be safe |
| Column (block) selection, Alt+drag | Popular, but touches selection, rendering, clipboard, undo |
| Macro record/playback | User agreed to defer; few use it daily |
| Split view / second view of same doc (e.g. hex + text side by side) | Deferred by user |
| Compare two files | Popular N++ plugin; a separate tool works too |
| Code folding | Big renderer work, mixed value |
| Key remapping, user-defined languages, colour themes | Most users keep defaults |
| Hex: select block dialog, checksums, integer/float search, character-set views (OEM/EBCDIC), editable inspector | Nice, not core |
| Shell "Edit with ..." context menu | Needs a shell extension DLL; Send To shortcut is enough |

### 8.4 Cut list (not planned)

- **Plugins / plugin manager:** causes breakage and maintenance ([forum](https://community.notepad-plus-plus.org/topic/16796/you-ve-a-lot-of-reasons-to-hate-notepad/81)).
- **Auto-update / any network access:** the most-cited nag; also pointless on Win98.
- **Autocomplete / function list / code explorer:** IDE territory; VC6 does it.
- **Multi-cursor / multi-select editing:** modern, hard, low value for this audience.
- **Document map / document peeker / minimap:** called useless and distracting.
- **FTP, browser preview, spell checker, clip library:** period clutter.
- **Dark mode / theme gallery, transparency sliders:** system colours + one scheme.
- **Huge toolbar:** keep ~16 buttons (New, Open, Save, Save All | Cut, Copy, Paste | Undo, Redo |
  Find, Replace, Find in Files | Hex view | Word wrap, Show whitespace).
- **Hex-encoded line numbers, odd defaults, nested context menus.**

### 8.5 Languages to colour in 1.0

Chosen for a Win98 hobbyist who writes VC6 C++, batch files, makefiles, configs, and some web/script
work. Keyword + comment + string + number + preprocessor colouring; no deep parsing.

| # | Language | Extensions | Why |
|---|---|---|---|
| 1 | C / C++ | .c .cpp .cc .cxx .h .hpp .inl | Main language (VC6) |
| 2 | Resource script | .rc .rc2 | VC6 resources; C-like preprocessor |
| 3 | Makefile (NMAKE) | makefile, .mak, .mk | Every project has one; `!IF` directives, `$(VAR)`, tab-sensitive |
| 4 | Batch | .bat .cmd .btm | Win98 scripting; `REM`, `::`, labels, `%VAR%` |
| 5 | INI / config | .ini .inf .reg .cfg .dsw | Win98 configs (incl. `86box.cfg`), registry files |
| 6 | Assembly (x86, MASM/NASM) | .asm .inc .s | Retro hobby; optional but cheap |
| 7 | Python | .py .pyw | Art scripts in this workshop |
| 8 | HTML (+ embedded JS/CSS basic) | .htm .html | Period web pages, readmes |
| 9 | JavaScript | .js | Common, C-like lexer reused |
| 10 | XML | .xml .xsl .svg .manifest | Config/data files |
| 11 | Markdown (light: headings, code, emphasis) | .md | The workshop's docs are Markdown |
| 12 | Diff / patch | .diff .patch | Cheap, useful for patches (TD5) |

Candidates if cheap after the lexer framework exists: Pascal/Delphi, BASIC/VB6 (.bas .frm), SQL, CSS,
Perl, shell (.sh), Lua. Plain text (.txt, .nfo, .log) gets no colouring; `.nfo` should default to the
**OEM** code page (DOS art), a nice period touch.

---

## 9. Main window sketch and menus

### 9.1 Text view

```
+--------------------------------------------------------------------------------+
| main.cpp * - Editor                                                    [_][o][X]|
+--------------------------------------------------------------------------------+
| File  Edit  Search  View  Document  Tools  Window  Help                         |
+--------------------------------------------------------------------------------+
| [New][Open][Save][SaveAll] | [Cut][Copy][Paste] | [Undo][Redo] | [Find][Repl]   |
| [FiF] | [HEX] | [Wrap][Pilcrow]                                                 |
+--------------------------------------------------------------------------------+
| main.cpp * [x] | game.h | Makefile | new 1 |                                    |
+-----+--------------------------------------------------------------------------+
|  118|     if (dd->Blt(&dst, surf, &src, DDBLT_KEYSRC, NULL) != DD_OK)         ^|
|  119|     {                                                                    #|
| >120|         restore_surfaces();            <- current line highlighted       #|
|  121|         return false;                                                    ||
|  122|     }                                                                    v|
+-----+--------------------------------------------------------------------------+
| [!] This file was changed by another program.   [Reload]  [Keep my version]    |  <- only when needed
+--------------------------------------------------------------------------------+
| Find results: "Blt" 17 hits in 6 files                                     [x] |  <- docked, optional
|  M:\dev\runner\game.cpp (3)                                                    |
|     120:     if (dd->Blt(&dst, surf, &src, DDBLT_KEYSRC, NULL) != DD_OK)        |
+--------------------------------------------------------------------------------+
| Ln 120, Col 9 | Sel 0 | C++ | ANSI | CRLF | INS | TEXT |                        |
+--------------------------------------------------------------------------------+
```

### 9.2 Hex view (same tab, toggled)

```
+-----+--------------------------------------------------------------------------+
| Offset(h) 00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  0123456789ABCDEF    |
| 00000000  4D 5A 90 00 03 00 00 00  04 00 00 00 FF FF 00 00  MZ..............    |
| 00000010  B8 00 00 00 00 00 00 00  40 00 00 00 00 00 00 00  ........@.......    |
| 00000020  00 00 00 00 [0]0 00 00 00  00 00 00 00 00 00 00 00  ................  |  <- nibble caret
| 00000030  00 00 00 00 00 00 00 00  00 00 00 00 C8 00 00 00  ............È...    |
+--------------------------------------------------------------------------------+
| Offset 0x24 (36) | Sel 0 | Byte 0x00 0 | int16 0 int32 0 | ANSI | OVR | HEX |   |
+--------------------------------------------------------------------------------+
```
Modified bytes in red; the byte under the caret is outlined in the other pane.

### 9.3 Menus (1.0; items marked (s) are "should")

```
File      New                      Ctrl+N
          Open...                  Ctrl+O
          Reopen closed tab (s)    Ctrl+Shift+T
          Revert to saved
          ---
          Save                     Ctrl+S
          Save As...               Ctrl+Alt+S
          Save All                 Ctrl+Shift+S
          ---
          Close                    Ctrl+W
          Close All / Close Others
          ---
          Print... (s)             Ctrl+P
          ---
          1 M:\dev\editor\src\main.cpp   ... (10 recent files)
          ---
          Exit                     Alt+F4

Edit      Undo / Redo              Ctrl+Z / Ctrl+Y
          ---
          Cut / Copy / Paste / Delete
          Copy as hex text (hex view)
          Select All               Ctrl+A
          ---
          Line > Duplicate (Ctrl+D), Delete (Ctrl+L), Move Up/Down (Ctrl+Shift+Up/Down), Join (Ctrl+J)
          Toggle comment           Ctrl+Q
          Case > UPPER (Ctrl+Shift+U), lower (Ctrl+U)
          Indent / Unindent        Tab / Shift+Tab
          ---
          Insert / Overwrite       Ins

Search    Find...                  Ctrl+F
          Find Next / Previous     F3 / Shift+F3
          Find selection           Ctrl+F3
          Incremental search (s)   Ctrl+I
          Replace...               Ctrl+H
          Find in Files...         Ctrl+Shift+F
          Next / Previous result   F4 / Shift+F4
          ---
          Go to line / offset...   Ctrl+G
          Go to matching brace (s) Ctrl+B
          ---
          Bookmarks (s) > Toggle (Ctrl+F2), Next (F2), Previous (Shift+F2), Clear all

View      Hex view                 Ctrl+Shift+H   (checkmark)
          ---
          Word wrap (s)
          Show whitespace and line endings (s)
          Line numbers
          ---
          Zoom in / out / reset (s)
          ---
          Toolbar / Status bar / Find results panel

Document  Syntax > (Auto) | Plain text | C/C++ | RC | Makefile | Batch | INI | ASM | Python | HTML | JS | XML | Markdown | Diff
          Encoding > Reload as > ...  /  ANSI, UTF-8, UTF-8 with BOM, UTF-16 LE, UTF-16 BE (radio = save encoding)
          Line endings > Windows (CRLF) | Unix (LF) | Mac (CR)
          Tabs > Tab width 2/4/8, Insert spaces
          ---
          Hex > Bytes per row 8/16/32/Fit, Offsets hex/decimal, Data inspector (s)
          ---
          Read-only (toggle)
          Properties... (path, size, dates, encoding, EOL, line count)

Tools     Options...
          (later: user tools / run command)

Window    Next tab / Previous tab   Ctrl+Tab / Ctrl+Shift+Tab
          ---
          1 main.cpp  2 game.h  3 Makefile ... (open tabs, checkmark on active)

Help      Keyboard shortcuts
          Regular expressions
          About
```

### 9.4 Options dialog (property sheet, 5 pages)

- **General:** single instance, restore session, remember unsaved changes (backup) + interval,
  ask to save untitled documents on exit, recent files count.
- **Editor:** font (ChooseFont, fixed-pitch), tab width + spaces (default and per language),
  auto-indent, current-line highlight, smart highlight, caret blink.
- **Files:** new document encoding + EOL (default ANSI + CRLF), detect UTF-8 without BOM,
  `.nfo` as OEM, large-file threshold, file-changed handling (reload unmodified silently: on).
- **Colours:** one default scheme on system colours; per-token colour + bold for the shared token
  classes (comment, string, number, keyword, type, preprocessor, operator); "Reset".
- **Hex:** bytes per row, offset base, group size, modified-byte colour, default to overwrite.

Every option has a one-line explanation under it or as a tooltip.

---

## 10. Requirements this puts on ARCH / DESIGN (summary)

1. Byte-exact document model with lossless decode/encode; hex and text views share it and one undo.
2. Safe save sequence that works on Win98 FAT32 and the SMB share without `MoveFileEx`/`ReplaceFile`.
3. Crash-safe session and backup files (temp + swap + previous copy), written often.
4. Renderer that tolerates very long lines and can disable colouring per document; word wrap kept
   possible in the line-layout design.
5. Regex engine with live validation (error text and position) and backreferences; Extended-mode
   escape parser; search over the byte buffer for hex mode.
6. Change detection with size + mtime + own-save tracking (FAT 2 s resolution).
7. A private clipboard format for raw bytes (so NULs survive), plus CF_TEXT for hex text.
