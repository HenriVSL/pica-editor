// edops.h - editing operations (DESIGN 6.18): edtype.cpp (T08), edlines.cpp (T12), edhex.cpp (T10).
#ifndef PICA_EDOPS_H
#define PICA_EDOPS_H

#include "etypes.h"
#include "doc.h"
#include "lexer.h"

struct EdLang { ecp lc[8]; u32 lc_len; ecp bo[4]; u32 bo_len; ecp bc[4]; u32 bc_len;
                u8 tab_width; u8 use_spaces; char braces[8]; };
struct EdCtx  { Document* doc; Selection sel; EdLang lang; u8 overwrite; u8 auto_indent; u8 pad[2]; u32 now_ms; };
void ed_lang_from(const LangInfo* li, u8 tab_width, u8 use_spaces, EdLang* out);  // li may be NULL
// edtype.cpp: typing and deleting; all update e->sel; E_OK, E_READONLY, E_BUSY, E_ALIGN, E_NOMEM, E_RANGE (unrepresentable)
int  ed_type(EdCtx* e, ecp c);                    // replaces the selection; overwrite replaces the next char (not the EOL)
int  ed_newline(EdCtx* e);                        // doc EOL + copied indent (auto_indent); breaks undo merging
int  ed_backspace(EdCtx* e);  int ed_delete(EdCtx* e);
int  ed_delete_word(EdCtx* e, int forward);
int  ed_insert_text(EdCtx* e, const ecp* s, u32 n, u32* replaced);  // paste: CR/LF/CRLF -> doc EOL; unrepresentable -> '?' counted
int  ed_insert_bytes(EdCtx* e, const u8* p, epos n);                 // raw paste (same encoding)
epos ed_word_left(Document* d, epos pos);  epos ed_word_right(Document* d, epos pos);
void ed_word_at(Document* d, epos pos, Selection* out);
epos ed_smart_home(Document* d, epos pos);        // first non-blank; at it -> line start
// edlines.cpp: line commands (act on all lines touched by the selection)
int  ed_duplicate(EdCtx* e);                      // selection non-empty: duplicate the selection; else the line
int  ed_delete_lines(EdCtx* e);
int  ed_move_lines(EdCtx* e, int down);
int  ed_toggle_comment(EdCtx* e);                 // line comment if the language has one, else block comment
int  ed_change_case(EdCtx* e, int upper);         // selection, else the char at the caret
int  ed_indent(EdCtx* e, int outdent);            // Tab / Shift+Tab on a multi-line selection
int  ed_match_brace(Document* d, epos pos, Highlighter* hl, const char* braces, epos* brace, epos* match);
// edhex.cpp: hex view edits (HexCaret.pos is a byte offset)
struct HexCaret { epos pos; u8 nibble; u8 pane; u8 insert; u8 pad; epos unit_end; };   // pane 0 hex, 1 chars
int  hx_type_nibble(Document* d, HexCaret* c, Selection* sel, u8 v, u32 now_ms);
int  hx_type_char(Document* d, HexCaret* c, Selection* sel, ecp ch, u32 now_ms);
int  hx_write(Document* d, HexCaret* c, Selection* sel, const u8* p, epos n);   // paste in the current mode
int  hx_delete(Document* d, HexCaret* c, Selection* sel, int backspace);

#endif
