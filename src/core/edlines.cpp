// edlines.cpp - line commands, comments, case, indent, brace match (DESIGN 6.18). STUB from T00. Owner after Wave 0: T12.
#include "edops.h"

int ed_duplicate(EdCtx* e) { (void)e; return E_NOTIMPL; }
int ed_delete_lines(EdCtx* e) { (void)e; return E_NOTIMPL; }
int ed_move_lines(EdCtx* e, int down) { (void)e; (void)down; return E_NOTIMPL; }
int ed_toggle_comment(EdCtx* e) { (void)e; return E_NOTIMPL; }
int ed_change_case(EdCtx* e, int upper) { (void)e; (void)upper; return E_NOTIMPL; }
int ed_indent(EdCtx* e, int outdent) { (void)e; (void)outdent; return E_NOTIMPL; }
int ed_match_brace(Document* d, epos pos, Highlighter* hl, const char* braces, epos* brace, epos* match)
{
  (void)d; (void)pos; (void)hl; (void)braces;
  if (brace) *brace = EPOS_NONE;
  if (match) *match = EPOS_NONE;
  return E_NOTIMPL;
}
