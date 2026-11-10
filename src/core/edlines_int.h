// edlines_int.h - private helpers of edlines.cpp shared with editcmds.cpp (T12).
#ifndef PICA_EDLINES_INT_H
#define PICA_EDLINES_INT_H

#include "edops.h"

// Lines touched by the selection: first..last (0-based). A non-empty selection that ends at the start of a later
// line does not touch that line (Notepad++ rule for move/comment/indent).
void ed_lines_touched(Document* d, const Selection& s, u32* first, u32* last);
// Byte range of the touched lines: [visible start of first, start of the line after last) - includes the last EOL;
// for the last line of the document it ends at length() (the EOL before it stays, as in Scintilla).
void ed_lines_range(Document* d, const Selection& s, epos* a, epos* b);
// Ctrl+L (Scintilla LINECUT) deletes the same range ed_delete_lines does; editcmds copies it to the clipboard first.

// Display column (0-based cells: tabs, wide chars, control pictures) <-> byte offset within a line.
u32  ed_col_of(Document* d, epos pos, u8 tab_width);             // lines with > 64 KB before pos: characters
epos ed_pos_of_col(Document* d, u32 line, u32 col, u8 tab_width);  // clamps to the line end

#endif
