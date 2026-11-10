// edhex.cpp - hex view edits (DESIGN 6.18, 9.2, 7.3). Owner: T10.
//
// Positions are byte offsets. The selection is a byte range [lo, hi); in the hex view sel.caret == c->pos after
// every call. Return codes (nothing is changed on failure):
//   E_OK, E_READONLY / E_BUSY (document not editable), E_ACCESS (edit inside the BOM [0, origin)),
//   E_ALIGN (UTF-16: would leave or need an odd byte, e.g. typing after a dangling odd byte), E_RANGE (nothing to
//   delete at the caret, or a character the encoding can't store), E_NOMEM.
// UTF-16 rules (7.3): every edit covers whole 2-byte units. Overwriting one nibble rewrites its unit; insert mode
// inserts a unit "n0 00" on the first nibble and fills its bytes with the next nibbles (HexCaret.unit_end marks the
// end of that freshly inserted unit; the view resets it to 0 whenever the caret is moved by navigation).
#include "edops.h"
#include <string.h>

static int hx_unit(Document* d)
{
  u8 k = d->enc().kind;
  return (k == EK_UTF16LE || k == EK_UTF16BE) ? 2 : 1;
}

static int hx_check_editable(Document* d)
{
  if (d->read_only()) return E_READONLY;
  if (!d->editable()) return E_BUSY;
  return E_OK;
}

static void hx_set_caret(HexCaret* c, Selection* sel, epos pos, u8 nibble)
{
  c->pos = pos;
  c->nibble = nibble;
  sel->anchor = sel->caret = pos;
}

// Aligns a byte range for deletion: UTF-16 lo down to a unit start, hi up to a unit end (or the document end).
static void hx_align_range(Document* d, int u, epos* lo, epos* hi)
{
  epos len = d->length();
  if (*hi > len) *hi = len;
  if (*lo > *hi) *lo = *hi;
  if (u == 2) {
    *lo &= ~(epos)1;
    if (*hi & 1) *hi = (*hi + 1 > len) ? len : *hi + 1;
  }
}

static Selection hx_sel_at(epos pos) { Selection s; s.anchor = s.caret = pos; return s; }

// Replaces the selection [lo, hi) (aligned, BOM-protected) by ins; caret goes to lo + caret_off.
static int hx_replace_sel(Document* d, HexCaret* c, Selection* sel, const u8* ins, epos n, epos caret_off,
                          u8 nibble, u32 kind, u32 now_ms)
{
  int u = hx_unit(d), rc;
  epos lo = sel_lo(*sel), hi = sel_hi(*sel), origin = d->text_origin();
  hx_align_range(d, u, &lo, &hi);
  if (lo < origin) {
    if (n != 0 || hi <= origin) return E_ACCESS;
    lo = origin;                       // deleting a selection that starts in the BOM keeps the BOM (origin is even
  }                                    // for UTF-16: 0 or 2)
  d->begin_edit(kind, *sel, now_ms);
  rc = d->replace(lo, hi - lo, ins, n);
  if (rc == E_OK) {
    hx_set_caret(c, sel, lo + caret_off, nibble);
    c->unit_end = (n != 0) ? lo + n : 0;
  }
  d->end_edit(rc == E_OK ? *sel : hx_sel_at(sel->caret));
  return rc;
}

int hx_type_nibble(Document* d, HexCaret* c, Selection* sel, u8 v, u32 now_ms)
{
  int u, rc;
  epos len, pos, origin, upos;
  u8 unit[2];
  Selection before;
  if (!d || !c || !sel) return E_RANGE;
  if ((rc = hx_check_editable(d)) != E_OK) return rc;
  v &= 0x0F;
  u = hx_unit(d);
  len = d->length();
  origin = d->text_origin();
  if (!sel_empty(*sel)) {
    unit[0] = (u8)(v << 4); unit[1] = 0;
    return hx_replace_sel(d, c, sel, unit, (epos)u, 0, 1, UK_HEXTYPE, now_ms);
  }
  pos = c->pos;
  if (pos > len) pos = len;
  if (pos < origin) return E_ACCESS;
  if (u == 2 && pos == len && (len & 1)) return E_ALIGN;  // nothing can follow a dangling odd byte
  upos = (u == 2) ? (pos & ~(epos)1) : pos;
  before = hx_sel_at(pos);
  if (c->nibble == 0 && c->insert && !(u == 2 && c->unit_end == pos + 1 && pos < len)) {
    // insert a new byte (unit) at the caret; the first nibble fills its high half
    unit[0] = (u8)(v << 4); unit[1] = 0;
    d->begin_edit(UK_HEXTYPE, before, now_ms);
    rc = d->replace(upos, 0, unit, (epos)u);
    if (rc == E_OK) { hx_set_caret(c, sel, upos, 1); c->unit_end = upos + (epos)u; }
    d->end_edit(rc == E_OK ? *sel : before);
    return rc;
  }
  if (pos >= len) {
    // overwrite at the end appends (nibble 1 at the end can't happen: the caret is on an existing byte)
    if (u == 2 && (len & 1)) return E_ALIGN;
    unit[0] = (u8)(v << 4); unit[1] = 0;
    d->begin_edit(UK_HEXTYPE, before, now_ms);
    rc = d->replace(len, 0, unit, (epos)u);
    if (rc == E_OK) { hx_set_caret(c, sel, len, 1); c->unit_end = len + (epos)u; }
    d->end_edit(rc == E_OK ? *sel : before);
    return rc;
  }
  // rewrite one nibble of an existing byte (its whole unit for UTF-16)
  if (u == 2 && upos + 2 > len) return E_ALIGN;           // the dangling odd byte of a UTF-16 file
  if (d->read(upos, unit, (epos)u) != E_OK) return E_RANGE;
  {
    u8* b = &unit[pos - upos];
    *b = (c->nibble == 0) ? (u8)((v << 4) | (*b & 0x0F)) : (u8)((*b & 0xF0) | v);
  }
  d->begin_edit(UK_HEXTYPE, before, now_ms);
  rc = d->replace(upos, (epos)u, unit, (epos)u);
  if (rc == E_OK) {
    epos ue = c->unit_end;
    if (c->nibble == 0) hx_set_caret(c, sel, pos, 1);
    else hx_set_caret(c, sel, pos + 1, 0);
    c->unit_end = ue;                                      // still inside (or at the end of) a fresh unit
  }
  d->end_edit(rc == E_OK ? *sel : before);
  return rc;
}

int hx_type_char(Document* d, HexCaret* c, Selection* sel, ecp ch, u32 now_ms)
{
  u8 enc[4];
  u32 n;
  int u, rc;
  epos len, pos, origin, del;
  Selection before;
  if (!d || !c || !sel) return E_RANGE;
  if ((rc = hx_check_editable(d)) != E_OK) return rc;
  n = d->codec()->encode(ch, enc);
  if (n == 0) return E_RANGE;
  u = hx_unit(d);
  if (!sel_empty(*sel)) return hx_replace_sel(d, c, sel, enc, n, n, 0, UK_HEXTYPE, now_ms);
  len = d->length();
  origin = d->text_origin();
  pos = c->pos;
  if (pos > len) pos = len;
  if (pos < origin) return E_ACCESS;
  if (u == 2 && pos == len && (len & 1)) return E_ALIGN;
  if (u == 2) pos &= ~(epos)1;
  if (c->insert) del = 0;
  else {
    del = len - pos;
    if (del > n) del = n;
  }
  before = hx_sel_at(c->pos > len ? len : c->pos);
  d->begin_edit(UK_HEXTYPE, before, now_ms);
  rc = d->replace(pos, del, enc, n);
  if (rc == E_OK) { hx_set_caret(c, sel, pos + n, 0); c->unit_end = 0; }
  d->end_edit(rc == E_OK ? *sel : before);
  return rc;
}

int hx_write(Document* d, HexCaret* c, Selection* sel, const u8* p, epos n)
{
  int u, rc;
  epos len, pos, origin, del;
  Selection before;
  if (!d || !c || !sel) return E_RANGE;
  if ((rc = hx_check_editable(d)) != E_OK) return rc;
  if (n == 0) return E_OK;
  u = hx_unit(d);
  if (!sel_empty(*sel)) return hx_replace_sel(d, c, sel, p, n, n, 0, UK_OTHER, 0);
  len = d->length();
  origin = d->text_origin();
  pos = c->pos;
  if (pos > len) pos = len;
  if (pos < origin) return E_ACCESS;
  if (u == 2 && pos == len && (len & 1)) return E_ALIGN;
  if (u == 2) pos &= ~(epos)1;
  if (c->insert) del = 0;
  else {
    del = len - pos;
    if (del > n) del = n;
  }
  before = hx_sel_at(c->pos > len ? len : c->pos);
  d->begin_edit(UK_OTHER, before, 0);
  rc = d->replace(pos, del, p, n);
  if (rc == E_OK) { hx_set_caret(c, sel, pos + n, 0); c->unit_end = 0; }
  d->end_edit(rc == E_OK ? *sel : before);
  return rc;
}

int hx_delete(Document* d, HexCaret* c, Selection* sel, int backspace)
{
  int u, rc;
  epos len, pos, origin, start, del;
  Selection before;
  u32 kind;
  if (!d || !c || !sel) return E_RANGE;
  len = d->length();
  pos = c->pos;
  if (pos > len) pos = len;
  if (backspace && !c->insert && sel_empty(*sel)) {
    // overwrite mode: Backspace only moves left, it never shifts data
    if (c->nibble) { hx_set_caret(c, sel, pos, 0); c->unit_end = 0; return E_OK; }
    if (pos == 0) return E_RANGE;
    hx_set_caret(c, sel, pos - 1, 0);
    c->unit_end = 0;
    return E_OK;
  }
  if ((rc = hx_check_editable(d)) != E_OK) return rc;
  if (!sel_empty(*sel)) return hx_replace_sel(d, c, sel, 0, 0, 0, 0, UK_OTHER, 0);
  u = hx_unit(d);
  origin = d->text_origin();
  if (backspace) {
    epos eff = pos + ((c->nibble && pos < len) ? 1 : 0);   // a half-typed byte counts as typed
    if (eff == 0) return E_RANGE;
    start = eff - 1;
    kind = UK_BACKSPACE;
  } else {
    if (pos >= len) return E_RANGE;
    start = pos;
    kind = UK_DELETE;
  }
  if (u == 2) start &= ~(epos)1;
  if (start < origin) return E_ACCESS;
  del = len - start;
  if (del > (epos)u) del = (epos)u;
  before = hx_sel_at(pos);
  d->begin_edit(kind, before, 0);
  rc = d->replace(start, del, 0, 0);
  if (rc == E_OK) { hx_set_caret(c, sel, start, 0); c->unit_end = 0; }
  d->end_edit(rc == E_OK ? *sel : before);
  return rc;
}
