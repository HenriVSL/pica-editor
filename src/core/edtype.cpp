// edtype.cpp - typing and deleting, word movement, ed_lang_from (DESIGN 6.18). STUB from T00. Owner after Wave 0: T08.
#include "edops.h"
#include <string.h>

void ed_lang_from(const LangInfo* li, u8 tab_width, u8 use_spaces, EdLang* out)
{
  (void)li;
  memset(out, 0, sizeof(*out));
  out->tab_width = tab_width; out->use_spaces = use_spaces;
}
int ed_type(EdCtx* e, ecp c) { (void)e; (void)c; return E_NOTIMPL; }
int ed_newline(EdCtx* e) { (void)e; return E_NOTIMPL; }
int ed_backspace(EdCtx* e) { (void)e; return E_NOTIMPL; }
int ed_delete(EdCtx* e) { (void)e; return E_NOTIMPL; }
int ed_delete_word(EdCtx* e, int forward) { (void)e; (void)forward; return E_NOTIMPL; }
int ed_insert_text(EdCtx* e, const ecp* s, u32 n, u32* replaced) { (void)e; (void)s; (void)n; if (replaced) *replaced = 0; return E_NOTIMPL; }
int ed_insert_bytes(EdCtx* e, const u8* p, epos n) { (void)e; (void)p; (void)n; return E_NOTIMPL; }
epos ed_word_left(Document* d, epos pos) { (void)d; return pos; }
epos ed_word_right(Document* d, epos pos) { (void)d; return pos; }
void ed_word_at(Document* d, epos pos, Selection* out) { (void)d; out->anchor = out->caret = pos; }
epos ed_smart_home(Document* d, epos pos) { (void)d; return pos; }
