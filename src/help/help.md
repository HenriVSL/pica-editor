# Pica Help

Pica is a text editor and hex editor in one, made for Windows 98. Every file
opens in a tab. You can look at the same file as text or as bytes, and switch
between the two at any time without changing a single byte.

This help is a normal (read-only) tab: scroll it, search it with Ctrl+F, and
close it with Ctrl+W.

Contents:

- Views: text and hex
- Keyboard shortcuts
- Regular expressions
- Encodings and line endings
- The hex view
- Files, settings and backups
- Large files
- Command line
- Tips


## Views: text and hex

- **Text view** (the default) shows the file as text with syntax colouring, line
  numbers and bookmarks.
- **Hex view** (Ctrl+Shift+H, the Hex toolbar button, or a click on TEXT/HEX in
  the status bar) shows the same bytes as offsets, hex numbers and characters.
  Edits in one view show up in the other, and Undo works across both.
- Switching views never changes the file. The caret keeps its place: in the text
  view it moves to the start of the character it was in.
- Binary files (files with NUL bytes) open in the hex view; a bar at the top
  offers "Show as text".

The status bar shows, from left to right: position (click for Go To), selection,
the character or byte at the caret, syntax, encoding, line endings, INS/OVR and
TEXT/HEX. Click the syntax, encoding or line-ending fields for a menu; click
INS/OVR or TEXT/HEX to toggle them.


## Keyboard shortcuts

### Files and tabs

| Keys                        | Command       |
|-----------------------------|---------------|
| Ctrl+N                      | New document  |
| Ctrl+O                      | Open          |
| Ctrl+S                      | Save          |
| Ctrl+Alt+S                  | Save As       |
| Ctrl+Shift+S                | Save All      |
| Ctrl+W or Ctrl+F4           | Close the tab |
| Ctrl+Tab or Ctrl+PgDn       | Next tab      |
| Ctrl+Shift+Tab or Ctrl+PgUp | Previous tab  |
| Alt+F4                      | Exit          |
| F1                          | This help     |

Middle-click a tab to close it; drag tabs to reorder them; right-click a tab for
Close Others, Copy Full Path and Open Containing Folder.

### Editing

| Keys                    | Command                               |
|-------------------------|---------------------------------------|
| Ctrl+Z or Alt+Backspace | Undo                                  |
| Ctrl+Y or Ctrl+Shift+Z  | Redo                                  |
| Ctrl+X or Shift+Del     | Cut                                   |
| Ctrl+C or Ctrl+Ins      | Copy                                  |
| Ctrl+V or Shift+Ins     | Paste                                 |
| Ctrl+A                  | Select all                            |
| Ins                     | Insert / overwrite                    |
| Ctrl+D                  | Duplicate the line (or the selection) |
| Ctrl+L                  | Cut the line(s) to the clipboard      |
| Ctrl+Shift+L            | Delete the line(s)                    |
| Ctrl+Shift+Up / Down    | Move the line(s) up / down            |
| Ctrl+Q                  | Toggle a line comment                 |
| Ctrl+Shift+U            | UPPERCASE                             |
| Ctrl+U                  | lowercase                             |
| Tab / Shift+Tab         | Indent / unindent the selected lines  |

Line commands work on every line the selection touches; a selection that ends at
the very start of a line does not include that line.

### Search

| Keys                    | Command                                          |
|-------------------------|--------------------------------------------------|
| Ctrl+F                  | Find                                             |
| Ctrl+H                  | Replace                                          |
| Ctrl+Shift+F            | Find in Files                                    |
| F3 / Shift+F3           | Find next / previous                             |
| Ctrl+F3 / Ctrl+Shift+F3 | Find the selected word next / previous           |
| Ctrl+I                  | Incremental search (Enter = next, Esc = close)   |
| F4 / Shift+F4           | Next / previous result in the Find Results panel |
| Ctrl+G                  | Go to line (text view) or offset (hex view)      |
| Ctrl+B or Ctrl+]        | Go to the matching brace                         |
| Ctrl+F2                 | Toggle a bookmark                                |
| F2 / Shift+F2           | Next / previous bookmark                         |

F3 and Shift+F3 also work while the Find dialog has the focus. Esc closes the
dialog.

### Moving around in the text view

| Keys                      | Action                                          |
|---------------------------|-------------------------------------------------|
| Arrows                    | Character / line                                |
| Ctrl+Left / Right         | Word                                            |
| Home                      | First non-blank character, then column 1        |
| End                       | End of the line (wrapped: end of the row first) |
| Ctrl+Home / Ctrl+End      | Start / end of the document                     |
| PgUp / PgDn               | One screen                                      |
| Ctrl+Up / Ctrl+Down       | Scroll without moving the caret                 |
| Shift + any of these      | Extend the selection                            |
| Backspace / Del           | Delete the character before / after             |
| Ctrl+Backspace / Ctrl+Del | Delete a word                                   |
| Enter                     | New line, keeping the indentation               |
| Esc                       | Remove the Mark All highlights                  |

Mouse: double-click selects a word, triple-click a line; click or drag in the
line-number margin to select lines; click the bookmark cell (left of the
numbers) to toggle a bookmark; right-click for the edit menu.

### Keys in the hex view

| Keys                   | Action                                              |
|------------------------|-----------------------------------------------------|
| 0-9, A-F               | Type a nibble (hex pane)                            |
| Characters             | Type a character (character pane)                   |
| Tab                    | Switch between the hex pane and the character pane  |
| Ins                    | Insert / overwrite (overwrite is the default)       |
| Left / Right           | Nibble (hex pane) or byte (character pane)          |
| Up / Down, PgUp / PgDn | Row / screen                                        |
| Home / End             | Start / end of the row                              |
| Ctrl+Home / Ctrl+End   | Start / end of the file                             |
| Ctrl+Left / Right      | Previous / next 8-byte group                        |
| Shift + any move       | Select bytes                                        |
| Del                    | Delete the selection or the byte at the caret       |
| Backspace              | Insert mode: delete the byte before; else move left |

AltGr characters (for example @ { [ ] } \ | ~ on Finnish or German keyboards)
always type normally; Pica has no shortcuts that use them.


## Regular expressions

Pick **Regex** in the Find dialog (the **>** button next to the field lists the
tokens). Matching is line by line unless you tick ". matches newline".

| Pattern                   | Matches                                     |
|---------------------------|---------------------------------------------|
| `.`                       | Any character except a line break           |
| `\d \w \s`                | Digit, word char, space (`\D \W \S` = not)  |
| `[abc] [^a-z] [\w-]`      | Character classes                           |
| `[[:alpha:]] [[:digit:]]` | POSIX classes (also alnum, space, upper...) |
| `^ $`                     | Start / end of a line                       |
| `\A \z`                   | Start / end of the document                 |
| `\b \B \< \>`             | Word boundary, not one, word start / end    |
| `( ) (?: )`               | Group, group without a number               |
| `* + ? {n} {n,} {n,m}`    | Repeat (`*?` `+?`: shortest match)          |
| `\1` - `\9`               | The text of group 1-9 again                 |
| `(?= ) (?! )`             | Followed by / not followed by               |
| `(?i) (?s)`               | At the start: ignore case / `.` matches EOL |
| `\R`                      | Any line break (CRLF, LF or CR)             |
| `\t \n \r \xHH \uHHHH`    | Tab, LF, CR, character codes                |
| `\. \* \\ \[`             | The character itself                        |

A bar between alternatives matches either one: `cat|dog`.

Not supported (you get a clear error): lookbehind `(?<= )`, atomic groups,
possessive quantifiers, named groups, recursion and `\p{...}`. Backreferences
and lookahead see the current line plus up to 64 KB after it.

**Replace with** (Regex mode): `$0`-`$9` or `\0`-`\9` or `${n}` insert a group,
`$$` is a dollar sign, `\n \r \t \\` work as above, and `\U` (upper), `\L`
(lower), `\E` (stop), `\u`, `\l` (next character only) change case. Example:
find `(\w+)=(\w+)`, replace with `$2=$1` swaps both sides.

**Extended** mode is plain text with the escapes `\n \r \t \0 \\ \xHH \uHHHH
\dNNN`. **Hex bytes** (hex view) finds byte patterns like `DE AD ?? EF` (`??` =
any byte).

Replace All is one Undo step. Find All lists every match in the Find Results
panel (F4 walks through them).


## Encodings and line endings

The encoding is how characters are stored as bytes. Pica detects it when it
opens a file and shows it in the status bar:

| Name           | What it is                                                 |
|----------------|------------------------------------------------------------|
| ANSI           | Windows code page (1252 in the West): 1 byte per character |
| OEM            | DOS code page (437): box drawing, .nfo files, DOS programs |
| UTF-8          | All of Unicode; "UTF-8 BOM" starts with a 3-byte signature |
| UTF-16 LE / BE | Two bytes per character, with a signature (BOM)            |

- **Document > Encoding > Reload as** reads the file again with another encoding
  (use it when the text looks wrong). Nothing in the file changes.
- **Document > Encoding > ANSI / UTF-8 / ...** converts the text to that
  encoding; the next save writes it. If some characters can't be stored (for
  example Cyrillic in ANSI), Pica tells you how many and where, before anything
  changes. Converting is one Undo step.
- New documents are ANSI with CRLF line endings, which every Win98 program
  understands. Pica never adds a BOM unless you pick an encoding "with BOM".
  Change the defaults in Tools > Options > Files.
- Typing a character the encoding can't store beeps and offers to convert the
  document to UTF-8.

**Line endings**: Windows uses CRLF, Unix LF, old Macs CR. The status bar shows
the style, or **Mixed** when a file has several (saving keeps each line as it
is). Document > Line Endings converts every line (one Undo step). View > Show
Whitespace and Line Endings draws them.


## The hex view

```
Offset(h) 00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  0123456789ABCDEF
00000000  4D 5A 90 00 03 00 00 00  04 00 00 00 FF FF 00 00  MZ..............
```

- Left: the offset of the row in hex. Middle: 16 bytes. Right: the same bytes as
  characters (`.` = not printable).
- **Overwrite** (OVR, the default) replaces bytes; **insert** (INS, press Ins)
  inserts new bytes. Typing two hex digits makes one byte.
- Changed bytes are drawn in red until you save. A BOM is shown in its own
  colour and is changed only with Document > Encoding.
- **View > Data Inspector** shows the bytes at the caret as numbers (int8 to
  double, little- or big-endian; click [LE]/[BE] to switch).
- **Ctrl+G** goes to an offset: `1F40` or `0x1F40` (hex), `#8000` or `8000d`
  (decimal), `+100`, `-20`, `end`, `end-10` or `50%`.
- **Copy** in the hex pane copies hex text ("4D 5A 90"); in the character pane
  it copies text. Pasting hex text into the hex pane pastes those bytes.
  Pica-to-Pica copies keep the exact bytes.
- In UTF-16 files bytes are edited in whole 2-byte units.


## Files, settings and backups

**Saving is safe**: Pica writes a temporary file next to yours, checks it, and
only then replaces the old file. If anything goes wrong your file stays as it
was and Pica tells you what was kept. Tick "Keep a .bak copy when saving" (Tools
> Options > Files) to keep the previous version as `name.bak`.

**Where things live** (the settings folder):

- The folder of `pica.exe`, if Pica can write there (`C:\PICA` for the
  portable zip, `C:\Program Files\Pica` after Setup: Windows 98 lets Pica
  write to both).
- Otherwise `C:\WINDOWS\Application Data\Pica`.
- The `-cfg <folder>` command line option picks another folder.

| File                    | Contents                                  |
|-------------------------|-------------------------------------------|
| `pica.ini`              | Settings, window, recent files, history   |
| `session1/2.ini`        | Open tabs, carets, bookmarks (two copies) |
| `backup\*.BAK`          | Unsaved changes (written while you work)  |
| `Pica Help.md`          | This help                                 |

**Unsaved work is never lost**: with "Remember unsaved changes" on (the
default), Pica backs up modified documents in the background. After a crash or a
power cut, the next start restores the tabs with your unsaved changes. When you
exit, Pica can keep unsaved documents for next time instead of asking you to
save them. Closing a single tab always asks.

**Changes by other programs**: Pica checks open files every few seconds. A file
you haven't edited reloads by itself; if you have edited it, a bar asks whether
to reload or keep your version. A deleted file gets a bar with Save and Close
tab.

**Read-only files** open read-only with an "Edit anyway" button. Document >
Read-Only toggles it for a tab.

**Language files**: put your own `*.lng` syntax files in a `langs` folder next
to `pica.exe` (or in the settings folder) to add or change syntax colouring.


## Large files

- Files up to 16 MB are read into memory. Bigger files (up to 4 GB) are read
  **on demand** while you scroll, so they open at once and use little memory.
  While Pica counts their lines you can already look at them; editing starts
  when the count is done.
- Files of 8 MB or more open in **large mode** (the status bar says Large): no
  syntax colouring and no word wrap, so scrolling stays fast. Document > Syntax
  can turn colouring back on.
- Very long lines (megabytes without a line break) work too; only the first
  16,384 characters of a line are coloured.
- Unsaved changes to files over 16 MB are not backed up, and their encoding
  can't be converted.
- Both limits can be changed in Tools > Options > Files.


## Command line

```
pica.exe [-n<line>] [-hex] [-ro] [-multi] [-nosession] [-cfg <folder>] [files...]
```

| Option          | Meaning                                                   |
|-----------------|-----------------------------------------------------------|
| `-n120`         | Go to line 120                                            |
| `-hex`          | Open in the hex view                                      |
| `-ro`           | Open read-only                                            |
| `-multi`        | Start another Pica (normally files go to the running one) |
| `-nosession`    | Don't restore or save the session                         |
| `-cfg <folder>` | Use another settings folder                               |

`-n`, `-hex` and `-ro` apply to the file that follows them, for example
`pica.exe -n50 main.c -hex data.bin`.


## Tips

- Drop files from Explorer onto the window to open them.
- Ctrl+F3 searches for the word under the caret without opening the Find dialog.
- Search > Find in Files searches a whole folder; use masks like `*.cpp;*.h` and
  `!*.bak` to skip files.
- Pica runs fastest from the local disk (C:). Files on network drives work, but
  saving is slower.
- Help > About Pica shows the version and the settings folder.
