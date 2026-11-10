// hexview.cpp - hex view window, data inspector, view switching and sync (DESIGN 9). Owner: T10.
//
// One HexView per window (the frame creates one). All caret/selection state lives in the Tab (t->sel and
// t->hv.caret, t->hv.top_row) so other modules (edit commands, find, go to, session) can read and set it; the view
// re-validates it on every use. Layout per row (cells of the fixed font):
//   0..7 offset | 10 + 3*i (+1 after byte 7) hex bytes | 60 + i chars        (76 cells)
// The first row of the client area is the ruler; the optional data inspector strip takes 3 rows at the bottom.
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "selftest.h"
#include "render.h"
#include "clip.h"
#include "hexfmt.h"
#include "edops.h"
#include "winenc.h"
#include "estr.h"
#include "resource.h"

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

static int hexview_command(HWND w, int id);    // hex semantics of the Edit commands (selftest; editcmds.cpp has its own)

enum {
  HX_CELLS = 76, HX_HEX0 = 10, HX_CHR0 = 60, HX_MARGIN = 4,
  HX_TIMER_DRAG = 1, HX_TIMER_SAVE = 2,
  HX_SCROLL_MAX = 32767,
  INSP_ROWS = 3, INSP_PAD = 3
};
enum { BF_VALID = 1, BF_MOD = 2, BF_BOM = 4, BF_SEL = 8, BF_MIRROR = 16, BF_ERR = 32 };

static const char k_hex_class[] = APP_NAME "HexView";

// ---- modified-byte tracking (per tab): add-buffer bytes appended before the last detected save are not red ----
struct HxSaved { u32 tabid; Document* doc; u32 wm_src; u32 wm_fill; u32 seq; u8 mod; u8 pad[3]; };
static Vec<HxSaved> g_saved;

static void hx_add_end(Document* d, u32* src, u32* fill)
{
  AddBuffer* ab = d->tree()->add_buffer();
  u32 n = ab ? ab->chunk_count() : 0;
  *src = n;
  *fill = n ? ab->chunk_fill((u16)n) : 0;
}

static HxSaved* hx_saved_find(Tab* t, int create)
{
  u32 i;
  HxSaved s;
  for (i = 0; i < g_saved.size(); i++)
    if (g_saved[i].tabid == t->id && g_saved[i].doc == t->doc) return &g_saved[i];
  if (!create) return 0;
  // prune entries of closed tabs
  for (i = g_saved.size(); i > 0; i--) {
    u32 k, found = 0;
    for (k = 0; k < g_app.tabs.size(); k++)
      if (g_app.tabs[k]->id == g_saved[i - 1].tabid && g_app.tabs[k]->doc == g_saved[i - 1].doc) found = 1;
    if (!found) g_saved.erase(i - 1, 1);
  }
  memset(&s, 0, sizeof(s));
  s.tabid = t->id; s.doc = t->doc;
  s.mod = (u8)(t->doc->modified() ? 1 : 0);
  s.seq = t->doc->undo_seq();
  if (!s.mod) hx_add_end(t->doc, &s.wm_src, &s.wm_fill);   // unmodified: every byte in the tree is on disk
  if (g_saved.push(s) != E_OK) return 0;
  return &g_saved.back();
}

// Detects a save (modified -> unmodified without an undo/redo step). Returns 1 if the colouring changed.
static int hx_track(Tab* t)
{
  HxSaved* s;
  int mod, changed = 0;
  u32 seq;
  if (!t || !t->doc) return 0;
  s = hx_saved_find(t, 1);
  if (!s) return 0;
  mod = t->doc->modified() ? 1 : 0;
  seq = t->doc->undo_seq();
  if (s->mod && !mod) {
    if (s->seq == seq) hx_add_end(t->doc, &s->wm_src, &s->wm_fill);
    changed = 1;
  } else if (!s->mod && mod) changed = 1;
  s->mod = (u8)mod;
  s->seq = seq;
  return changed;
}

// ---- the view ----
class HexView : public DocListener {
public:
  HWND w; Tab* t; Document* doc;            // doc = document we listen to
  HFONT font; int own_font; int cw, ch, ascent;
  int cx, cy, rows_vis, data_top, data_bottom, hscroll, vis_cells;
  int insp_on, is_panel;
  int dragging, drag_dir; epos drag_anchor; int drag_pane;
  int wheel_acc;
  int focused, caret_shown, caret_ins;
  u8 lead; u8 pad[3];                       // DBCS lead byte typed in the char pane
  RECT insp_toggle;
  epos last_len;
  HexView() : w(0), t(0), doc(0), font(0), own_font(0), cw(8), ch(16), ascent(12), cx(0), cy(0), rows_vis(1),
              data_top(16), data_bottom(16), hscroll(0), vis_cells(HX_CELLS), insp_on(0), is_panel(0), dragging(0),
              drag_dir(0), drag_anchor(0), drag_pane(0), wheel_acc(0), focused(0), caret_shown(0), caret_ins(0), lead(0),
              last_len(0)
  { pad[0] = pad[1] = pad[2] = 0; insp_toggle.left = insp_toggle.top = insp_toggle.right = insp_toggle.bottom = 0; }
  ~HexView() {}
  void doc_changed(Document* d, const EditEvent& e);
};

static int g_switch_from_text = 0;          // cmd_view_hex: the next attach keeps the caret's screen row
static HexView* hv_get(HWND w) { return w ? (HexView*)GetWindowLongA(w, GWL_USERDATA) : 0; }

static COLORREF hv_ui(int uic)
{
  u32 c = g_app.cfg.ui[uic];
  if (c == COLOR_SYSDEFAULT) return GetSysColor(COLOR_WINDOWTEXT);
  return (COLORREF)(c & 0x00FFFFFFu);
}

static epos hv_len(HexView* v) { return (v->t && v->t->doc) ? v->t->doc->length() : 0; }
static u32 hv_total_rows(HexView* v) { return (u32)(hv_len(v) / HEX_BPR) + 1; }
static u32 hv_max_top(HexView* v)
{
  u32 n = hv_total_rows(v);
  return n > (u32)v->rows_vis ? n - (u32)v->rows_vis : 0;
}
static int hv_byte_cell(u32 i) { return HX_HEX0 + (int)i * 3 + (i >= 8 ? 1 : 0); }
static int hv_cell_x(HexView* v, int cell) { return HX_MARGIN + (cell - v->hscroll) * v->cw; }

// keeps t->sel and t->hv.caret consistent with the document (other modules may have changed either)
static void hv_validate(HexView* v)
{
  Tab* t = v->t;
  epos len;
  if (!t || !t->doc) return;
  len = t->doc->length();
  if (t->sel.anchor > len) t->sel.anchor = len;
  if (t->sel.caret > len) t->sel.caret = len;
  if (t->hv.caret.pos != t->sel.caret) {
    t->hv.caret.pos = t->sel.caret;
    t->hv.caret.nibble = 0;
    t->hv.caret.unit_end = 0;
  }
  if (t->hv.caret.pos >= len) t->hv.caret.nibble = 0;
  if (t->hv.caret.pane > 1) t->hv.caret.pane = 0;
  if (t->hv.top_row > hv_max_top(v)) t->hv.top_row = hv_max_top(v);
}

// ---- fonts and layout ----

static void hv_fonts(HexView* v)
{
  const RenderFonts* rf = render_fonts();
  HDC dc;
  TEXTMETRICA tm;
  HFONT old;
  if (v->own_font && v->font) DeleteObject(v->font);
  v->font = 0; v->own_font = 0;
  if (rf && rf->normal && rf->cw > 0 && rf->ch > 0) {
    v->font = rf->normal;
    v->cw = rf->cw; v->ch = rf->ch; v->ascent = rf->ascent;
    return;
  }
  dc = GetDC(v->w);
  {
    int h = g_app.cfg.font_px > 0 ? g_app.cfg.font_px
                                  : -MulDiv(g_app.cfg.font_pt > 0 ? g_app.cfg.font_pt : 10, GetDeviceCaps(dc, LOGPIXELSY), 72);
    v->font = CreateFontA(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN,
                          g_app.cfg.font_face[0] ? g_app.cfg.font_face : "Courier New");
  }
  if (!v->font) v->font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
  else v->own_font = 1;
  old = (HFONT)SelectObject(dc, v->font);
  GetTextMetricsA(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(v->w, dc);
  v->cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
  v->ch = tm.tmHeight > 0 ? tm.tmHeight : 16;
  v->ascent = tm.tmAscent;
}

static int hv_insp_height(HexView* v) { return v->insp_on ? INSP_ROWS * v->ch + 2 * INSP_PAD + 1 : 0; }

static void hv_update_scroll(HexView* v)
{
  SCROLLINFO si;
  u32 total = hv_total_rows(v), top = v->t ? v->t->hv.top_row : 0;
  memset(&si, 0, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
  if (!v->t) { si.nMax = 0; si.nPage = 1; }
  else if (total <= HX_SCROLL_MAX) {
    si.nMin = 0; si.nMax = (int)total - 1; si.nPage = (UINT)v->rows_vis; si.nPos = (int)top;
  } else {
    u32 maxtop = hv_max_top(v);
    u32 page = (u32)(((u64)v->rows_vis * (HX_SCROLL_MAX + 1)) / total);
    if (page < 1) page = 1;
    si.nMin = 0; si.nMax = HX_SCROLL_MAX; si.nPage = page;
    si.nPos = maxtop ? (int)(((u64)top * (u64)(HX_SCROLL_MAX + 1 - page)) / maxtop) : 0;
  }
  SetScrollInfo(v->w, SB_VERT, &si, TRUE);
  memset(&si, 0, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0; si.nMax = HX_CELLS; si.nPage = (UINT)(v->vis_cells > 0 ? v->vis_cells : 1); si.nPos = v->hscroll;
  SetScrollInfo(v->w, SB_HORZ, &si, TRUE);
}

static void hv_layout(HexView* v)
{
  RECT rc;
  int avail;
  GetClientRect(v->w, &rc);
  v->cx = rc.right; v->cy = rc.bottom;
  v->data_top = v->ch;
  v->data_bottom = v->cy - hv_insp_height(v);
  if (v->data_bottom < v->data_top) v->data_bottom = v->data_top;
  avail = v->data_bottom - v->data_top;
  v->rows_vis = avail / v->ch;
  if (v->rows_vis < 1) v->rows_vis = 1;
  v->vis_cells = (v->cx - HX_MARGIN) / (v->cw > 0 ? v->cw : 8);
  if (v->vis_cells >= HX_CELLS + 1) v->hscroll = 0;
  else if (v->hscroll > HX_CELLS + 1 - v->vis_cells) v->hscroll = HX_CELLS + 1 - v->vis_cells;
  if (v->hscroll < 0) v->hscroll = 0;
  hv_validate(v);
  hv_update_scroll(v);
}

// ---- caret ----

static void hv_caret_cell(HexView* v, int* cell)
{
  HexCaret* c = &v->t->hv.caret;
  u32 i = (u32)(c->pos % HEX_BPR);
  if (c->pane == 0) *cell = hv_byte_cell(i) + (c->nibble ? 1 : 0);
  else *cell = HX_CHR0 + (int)i;
}

static void hv_make_caret(HexView* v);

static void hv_place_caret(HexView* v)
{
  int cell, x, y;
  u32 row;
  if (!v->focused) return;
  if (!v->t) { if (v->caret_shown) { HideCaret(v->w); v->caret_shown = 0; } return; }
  if ((v->t->hv.caret.insert ? 1 : 0) != v->caret_ins) { DestroyCaret(); hv_make_caret(v); return; }  // Ins toggled elsewhere
  hv_caret_cell(v, &cell);
  row = (u32)(v->t->hv.caret.pos / HEX_BPR);
  x = hv_cell_x(v, cell);
  if (row < v->t->hv.top_row || row >= v->t->hv.top_row + (u32)v->rows_vis + 1) y = -2 * v->ch;
  else y = v->data_top + (int)(row - v->t->hv.top_row) * v->ch;
  if (y + v->ch > v->data_bottom && y >= 0) y = -2 * v->ch;      // hidden under the inspector strip
  SetCaretPos(x, y);
  if (!v->caret_shown) { ShowCaret(v->w); v->caret_shown = 1; }
}

static void hv_make_caret(HexView* v)
{
  int insert;
  if (!v->focused) return;
  insert = (v->t && v->t->hv.caret.insert) ? 1 : 0;
  v->caret_ins = insert;
  CreateCaret(v->w, 0, insert ? 2 : v->cw, v->ch);
  v->caret_shown = 0;
  hv_place_caret(v);
}

static void hv_notify(HexView* v)
{
  HWND p = GetParent(v->w);
  if (p && !v->is_panel) SendMessageA(p, WM_APP_CARET, 0, 0);
}

// ---- invalidation ----

static void hv_inval_rows(HexView* v, epos lo, epos hi)   // byte range [lo, hi] (inclusive end row)
{
  u32 r0, r1, top;
  RECT rc;
  if (!v->t) return;
  top = v->t->hv.top_row;
  r0 = (u32)(lo / HEX_BPR); r1 = (u32)(hi / HEX_BPR);
  if (r1 < top || r0 >= top + (u32)v->rows_vis + 1) return;
  if (r0 < top) r0 = top;
  if (r1 > top + (u32)v->rows_vis) r1 = top + (u32)v->rows_vis;
  rc.left = 0; rc.right = v->cx;
  rc.top = v->data_top + (int)(r0 - top) * v->ch;
  rc.bottom = v->data_top + (int)(r1 - top + 1) * v->ch;
  if (rc.bottom > v->data_bottom) rc.bottom = v->data_bottom;
  if (rc.top < rc.bottom) InvalidateRect(v->w, &rc, FALSE);
}

static void hv_inval_decor(HexView* v)    // ruler (caret column) and inspector
{
  RECT rc;
  rc.left = 0; rc.right = v->cx; rc.top = 0; rc.bottom = v->data_top;
  InvalidateRect(v->w, &rc, FALSE);
  if (v->insp_on) {
    rc.top = v->data_bottom; rc.bottom = v->cy;
    InvalidateRect(v->w, &rc, FALSE);
  }
}

// ---- scrolling ----

static void hv_scroll_to(HexView* v, u32 top)
{
  u32 old;
  int d;
  if (!v->t) return;
  if (top > hv_max_top(v)) top = hv_max_top(v);
  old = v->t->hv.top_row;
  if (top == old) return;
  v->t->hv.top_row = top;
  d = (top > old) ? (int)(top - old > 0x7FFF ? 0x7FFF : top - old) : -(int)(old - top > 0x7FFF ? 0x7FFF : old - top);
  if (d < v->rows_vis && -d < v->rows_vis && IsWindowVisible(v->w)) {
    RECT area;
    area.left = 0; area.right = v->cx; area.top = v->data_top; area.bottom = v->data_bottom;
    if (v->caret_shown) HideCaret(v->w);
    ScrollWindowEx(v->w, 0, -d * v->ch, &area, &area, 0, 0, SW_INVALIDATE);
    // the partial row at the bottom edge is redrawn too
    area.top = v->data_top + v->rows_vis * v->ch - v->ch;
    if (area.top < v->data_top) area.top = v->data_top;
    InvalidateRect(v->w, &area, FALSE);
    if (v->caret_shown) ShowCaret(v->w);
  } else {
    RECT area;
    area.left = 0; area.right = v->cx; area.top = v->data_top; area.bottom = v->data_bottom;
    InvalidateRect(v->w, &area, FALSE);
  }
  hv_update_scroll(v);
  hv_place_caret(v);
  UpdateWindow(v->w);
}

static void hv_hscroll_to(HexView* v, int h)
{
  int maxh = HX_CELLS + 1 - v->vis_cells;
  if (maxh < 0) maxh = 0;
  if (h > maxh) h = maxh;
  if (h < 0) h = 0;
  if (h == v->hscroll) return;
  v->hscroll = h;
  InvalidateRect(v->w, 0, FALSE);
  hv_update_scroll(v);
  hv_place_caret(v);
}

static void hv_ensure_visible(HexView* v, epos pos, int center)
{
  u32 row, top;
  int cell;
  if (!v->t) return;
  row = (u32)(pos / HEX_BPR);
  top = v->t->hv.top_row;
  if (center) {
    if (row < top || row >= top + (u32)v->rows_vis)
      hv_scroll_to(v, row > (u32)v->rows_vis / 2 ? row - (u32)v->rows_vis / 2 : 0);
  } else if (row < top) hv_scroll_to(v, row);
  else if (row >= top + (u32)v->rows_vis) hv_scroll_to(v, row - (u32)v->rows_vis + 1);
  if (pos == v->t->hv.caret.pos) {
    hv_caret_cell(v, &cell);
    if (cell < v->hscroll) hv_hscroll_to(v, cell > 2 ? cell - 2 : 0);
    else if (cell >= v->hscroll + v->vis_cells) hv_hscroll_to(v, cell - v->vis_cells + 3);
  }
}

// ---- caret / selection changes ----

// Moves the caret; extend keeps the anchor (selection in whole bytes). scroll: 0 none, 1 minimal, 2 centre.
static void hv_set_caret(HexView* v, epos pos, u8 nibble, int extend, int scroll)
{
  Tab* t = v->t;
  epos len, olo, ohi, nlo, nhi;
  if (!t) return;
  len = t->doc->length();
  if (pos > len) pos = len;
  if (pos == len) nibble = 0;
  olo = sel_lo(t->sel); ohi = sel_hi(t->sel);
  if (t->hv.caret.pos < olo) olo = t->hv.caret.pos;
  if (t->hv.caret.pos > ohi) ohi = t->hv.caret.pos;
  if (!extend) t->sel.anchor = pos;
  t->sel.caret = pos;
  t->hv.caret.pos = pos;
  t->hv.caret.nibble = extend ? 0 : nibble;
  t->hv.caret.unit_end = 0;
  v->lead = 0;
  nlo = sel_lo(t->sel); nhi = sel_hi(t->sel);
  hv_inval_rows(v, olo < nlo ? olo : nlo, ohi > nhi ? ohi : nhi);
  hv_inval_decor(v);
  if (scroll) hv_ensure_visible(v, pos, scroll == 2);
  hv_place_caret(v);
  hv_notify(v);
}

// ---- painting ----

struct HvRow {
  WCHAR txt[HX_CELLS];
  COLORREF fg[HX_CELLS], bg[HX_CELLS];
};

// Byte flags for one row (16 bytes from base); returns the number of bytes in the row.
static u32 hv_row_bytes(HexView* v, epos base, u8* b, u8* fl)
{
  Tab* t = v->t;
  Document* d = t->doc;
  epos len = d->length(), origin = d->text_origin(), lo = sel_lo(t->sel), hi = sel_hi(t->sel);
  u32 n, i = 0;
  int modified = d->modified() ? 1 : 0;
  HxSaved* sv = hx_saved_find(t, 0);
  AddBuffer* ab = d->tree()->add_buffer();
  memset(fl, 0, HEX_BPR);
  if (base >= len) return 0;
  n = (len - base > HEX_BPR) ? HEX_BPR : (u32)(len - base);
  while (i < n) {
    PSpan s;
    u32 take, k;
    int is_mod = 0;
    epos off0 = 0;
    if (d->span(base + i, &s) != E_OK || !s.p || s.len == 0) break;
    take = (s.len > (epos)(n - i)) ? n - i : (u32)s.len;
    if (modified && s.src != 0) {
      is_mod = 1;
      if (sv && ab && ab->chunk(s.src)) {
        off0 = (epos)(s.p - ab->chunk(s.src));
        if (s.src < sv->wm_src) is_mod = 0;
        else if (s.src == sv->wm_src && off0 + take <= sv->wm_fill) is_mod = 0;
      }
    }
    for (k = 0; k < take; k++) {
      b[i + k] = s.p[k];
      fl[i + k] = BF_VALID;
      if (is_mod) {
        if (!(sv && s.src == sv->wm_src && off0 + k < sv->wm_fill)) fl[i + k] |= BF_MOD;
      }
    }
    i += take;
  }
  for (; i < n; i++) { b[i] = 0; fl[i] = BF_ERR; }
  for (i = 0; i < n; i++) {
    epos p = base + i;
    if (p < origin) fl[i] |= BF_BOM;
    if (p >= lo && p < hi) fl[i] |= BF_SEL;
    if (p == t->hv.caret.pos && lo == hi) fl[i] |= BF_MIRROR;
  }
  return n;
}

static void hv_build_row(HexView* v, u32 row, HvRow* r)
{
  static const char k_hex[] = "0123456789ABCDEF";
  Tab* t = v->t;
  Document* d = t->doc;
  COLORREF wfg = GetSysColor(COLOR_WINDOWTEXT), wbg = GetSysColor(COLOR_WINDOW);
  COLORREF sfg = GetSysColor(COLOR_HIGHLIGHTTEXT), sbg = GetSysColor(COLOR_HIGHLIGHT);
  COLORREF mod = hv_ui(UIC_HEX_MODIFIED), bom = hv_ui(UIC_HEX_BOM), mir = hv_ui(UIC_HEX_MIRROR_BG);
  const Codec* sbcs = (d->enc().kind == EK_SBCS) ? d->codec() : 0;
  u8 b[HEX_BPR], fl[HEX_BPR];
  epos base = (epos)row * HEX_BPR;
  u32 n, i;
  int k;
  char off[9];
  for (k = 0; k < HX_CELLS; k++) { r->txt[k] = ' '; r->fg[k] = wfg; r->bg[k] = wbg; }
  if (row >= hv_total_rows(v)) return;
  hex_offset_text(base, off);
  for (k = 0; k < 8; k++) { r->txt[k] = (WCHAR)off[k]; r->fg[k] = hv_ui(UIC_HEX_OFFSET); }
  n = hv_row_bytes(v, base, b, fl);
  for (i = 0; i < n; i++) {
    int hc = hv_byte_cell(i), cc = HX_CHR0 + (int)i;
    COLORREF fg = wfg, bg = wbg;
    if (fl[i] & BF_BOM) fg = bom;
    else if (fl[i] & BF_MOD) fg = mod;
    if (fl[i] & BF_ERR) { r->txt[hc] = '?'; r->txt[hc + 1] = '?'; r->txt[cc] = '?'; }
    else {
      r->txt[hc] = (WCHAR)k_hex[b[i] >> 4];
      r->txt[hc + 1] = (WCHAR)k_hex[b[i] & 15];
      r->txt[cc] = (WCHAR)hex_char_glyph(b[i], sbcs);
    }
    if (fl[i] & BF_SEL) {
      r->fg[hc] = r->fg[hc + 1] = r->fg[cc] = sfg;
      r->bg[hc] = r->bg[hc + 1] = r->bg[cc] = sbg;
      if (i + 1 < n && (fl[i + 1] & BF_SEL)) {   // join the gap to the next selected byte
        int g;
        for (g = hc + 2; g < hv_byte_cell(i + 1); g++) { r->fg[g] = sfg; r->bg[g] = sbg; }
      }
    } else {
      r->fg[hc] = r->fg[hc + 1] = r->fg[cc] = fg;
      if (fl[i] & BF_MIRROR) {
        // the byte under the caret is outlined in the other pane
        if (t->hv.caret.pane == 0) r->bg[cc] = mir;
        else { r->bg[hc] = mir; r->bg[hc + 1] = mir; }
      }
    }
  }
}

static void hv_draw_cells(HexView* v, HDC dc, int y, const WCHAR* txt, const COLORREF* fg, const COLORREF* bg)
{
  int c0 = v->hscroll, cell, x;
  RECT rc;
  COLORREF wbg = GetSysColor(COLOR_WINDOW);
  rc.top = y; rc.bottom = y + v->ch;
  // left margin
  rc.left = 0; rc.right = HX_MARGIN;
  SetBkColor(dc, wbg);
  ExtTextOutA(dc, 0, y, ETO_OPAQUE, &rc, 0, 0, 0);
  cell = c0;
  while (cell < HX_CELLS) {
    int e = cell + 1;
    while (e < HX_CELLS && fg[e] == fg[cell] && bg[e] == bg[cell]) e++;
    x = hv_cell_x(v, cell);
    if (x >= v->cx) break;
    rc.left = x; rc.right = hv_cell_x(v, e);
    SetTextColor(dc, fg[cell]);
    SetBkColor(dc, bg[cell]);
    ExtTextOutW(dc, x, y, ETO_OPAQUE | ETO_CLIPPED, &rc, txt + cell, (UINT)(e - cell), 0);
    cell = e;
  }
  x = hv_cell_x(v, HX_CELLS);
  if (x < HX_MARGIN) x = HX_MARGIN;
  if (x < v->cx) {
    rc.left = x; rc.right = v->cx;
    SetBkColor(dc, wbg);
    ExtTextOutA(dc, x, y, ETO_OPAQUE, &rc, 0, 0, 0);
  }
}

static void hv_paint_ruler(HexView* v, HDC dc)
{
  static const char k_hex[] = "0123456789ABCDEF";
  WCHAR txt[HX_CELLS];
  COLORREF fg[HX_CELLS], bg[HX_CELLS];
  COLORREF rfg = hv_ui(UIC_HEX_OFFSET), rbg = GetSysColor(COLOR_BTNFACE), mir = hv_ui(UIC_HEX_MIRROR_BG);
  const char* lab = "Offset(h)";
  int k, cur = -1;
  u32 i;
  for (k = 0; k < HX_CELLS; k++) { txt[k] = ' '; fg[k] = rfg; bg[k] = rbg; }
  for (k = 0; lab[k]; k++) txt[k] = (WCHAR)lab[k];
  if (v->t) cur = (int)(v->t->hv.caret.pos % HEX_BPR);
  for (i = 0; i < HEX_BPR; i++) {
    int hc = hv_byte_cell(i);
    txt[hc] = '0'; txt[hc + 1] = (WCHAR)k_hex[i];
    txt[HX_CHR0 + i] = (WCHAR)k_hex[i];
    if ((int)i == cur) { bg[hc] = bg[hc + 1] = bg[HX_CHR0 + i] = mir; }
  }
  hv_draw_cells(v, dc, 0, txt, fg, bg);
  {
    // the margin and the area right of the ruler in the ruler colour
    RECT rc;
    rc.top = 0; rc.bottom = v->ch; rc.left = 0; rc.right = HX_MARGIN;
    SetBkColor(dc, rbg);
    ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &rc, 0, 0, 0);
    rc.left = hv_cell_x(v, HX_CELLS); if (rc.left < HX_MARGIN) rc.left = HX_MARGIN;
    rc.right = v->cx;
    if (rc.left < rc.right) ExtTextOutA(dc, rc.left, 0, ETO_OPAQUE, &rc, 0, 0, 0);
  }
}

// Inspector texts (also used by the selftest): three rows of "label value" pairs.
static void hv_inspect(HexView* v, InspectOut* out, int* be)
{
  u8 b[8];
  u32 n = 0;
  epos len, pos;
  *be = g_app.cfg.inspector_be ? 1 : 0;
  memset(b, 0, sizeof(b));
  if (v->t && v->t->doc) {
    len = v->t->doc->length();
    pos = v->t->hv.caret.pos;
    if (pos < len) {
      n = (len - pos > 8) ? 8 : (u32)(len - pos);
      if (v->t->doc->read(pos, b, n) != E_OK) n = 0;
    }
  }
  inspect_bytes(b, n, *be, out);
}

static int hv_insp_text(HexView* v, int line, char* out, u32 cap)    // selftest + paint
{
  InspectOut o;
  int be;
  hv_inspect(v, &o, &be);
  if (line == 0) return es_fmt(out, cap, "int8 %s  uint8 %s  int16 %s  uint16 %s  int32 %s  uint32 %s", o.i8, o.u8_,
                               o.i16, o.u16_, o.i32_, o.u32_);
  if (line == 1) return es_fmt(out, cap, "int64 %s  uint64 %s", o.i64, o.u64_);
  return es_fmt(out, cap, "float %s  double %s  bin %s  %s", o.f32, o.f64, o.bin, be ? "[BE]" : "[LE]");
}

static void hv_paint_inspector(HexView* v, HDC dc)
{
  RECT rc;
  int line;
  COLORREF wbg = GetSysColor(COLOR_WINDOW), wfg = GetSysColor(COLOR_WINDOWTEXT), lab = GetSysColor(COLOR_GRAYTEXT);
  COLORREF tog = hv_ui(UIC_HEX_OFFSET);
  if (!v->insp_on) return;
  rc.left = 0; rc.right = v->cx; rc.top = v->data_bottom; rc.bottom = v->data_bottom + 1;
  SetBkColor(dc, GetSysColor(COLOR_BTNSHADOW));
  ExtTextOutA(dc, 0, rc.top, ETO_OPAQUE, &rc, 0, 0, 0);
  rc.top = rc.bottom; rc.bottom = v->cy;
  SetBkColor(dc, wbg);
  ExtTextOutA(dc, 0, rc.top, ETO_OPAQUE, &rc, 0, 0, 0);
  if (!v->t) return;
  for (line = 0; line < INSP_ROWS; line++) {
    char buf[200];
    int y = v->data_bottom + 1 + INSP_PAD + line * v->ch, x = HX_MARGIN, i = 0, n;
    n = hv_insp_text(v, line, buf, sizeof(buf));
    // words alternate: label value label value ... ; the [LE]/[BE] toggle is drawn as a link
    while (i < n) {
      int s = i, is_label, is_toggle;
      while (i < n && buf[i] != ' ') i++;
      is_toggle = (buf[s] == '[');
      // count words before s to know label/value parity
      {
        int k, words = 0;
        for (k = 0; k < s; k++) if (buf[k] != ' ' && (k == 0 || buf[k - 1] == ' ')) words++;
        is_label = (words % 2) == 0;
      }
      SetBkColor(dc, wbg);
      SetTextColor(dc, is_toggle ? tog : (is_label ? lab : wfg));
      ExtTextOutA(dc, x, y, 0, 0, buf + s, (UINT)(i - s), 0);
      if (is_toggle) {
        v->insp_toggle.left = x; v->insp_toggle.right = x + (i - s) * v->cw;
        v->insp_toggle.top = y; v->insp_toggle.bottom = y + v->ch;
      }
      x += (i - s) * v->cw;
      while (i < n && buf[i] == ' ') { i++; x += v->cw; }
    }
  }
}

static void hv_paint(HexView* v)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(v->w, &ps);
  HFONT old = (HFONT)SelectObject(dc, v->font);
  int y;
  u32 row;
  SetBkMode(dc, OPAQUE);
  if (!v->t) {
    SetBkColor(dc, GetSysColor(COLOR_WINDOW));
    ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &ps.rcPaint, 0, 0, 0);
    SelectObject(dc, old);
    EndPaint(v->w, &ps);
    return;
  }
  hx_track(v->t);
  hv_validate(v);
  if (ps.rcPaint.top < v->data_top) hv_paint_ruler(v, dc);
  {
    HRGN clip = CreateRectRgn(0, v->data_top, v->cx, v->data_bottom);
    if (clip) { SelectClipRgn(dc, clip); DeleteObject(clip); }
    for (y = v->data_top, row = v->t->hv.top_row; y < v->data_bottom; y += v->ch, row++) {
      HvRow r;
      if (y + v->ch <= ps.rcPaint.top || y >= ps.rcPaint.bottom) continue;
      hv_build_row(v, row, &r);
      hv_draw_cells(v, dc, y, r.txt, r.fg, r.bg);
    }
    SelectClipRgn(dc, 0);
  }
  if (v->insp_on && ps.rcPaint.bottom > v->data_bottom) hv_paint_inspector(v, dc);
  SelectObject(dc, old);
  EndPaint(v->w, &ps);
}

// ---- edits ----

static void hv_edit_failed(HexView* v, int rc, int typing_char)
{
  Tab* t = v->t;
  char lab[40];
  MessageBeep(MB_OK);
  switch (rc) {
  case E_READONLY: app_status_note("The document is read-only (Document > Read-Only)"); break;
  case E_BUSY:
    if (t && t->indexing) app_status_note("Wait until indexing finishes");
    else if (t && t->saving) app_status_note("Saving: try again in a moment");
    else app_status_note("The document is busy");
    break;
  case E_ACCESS: app_status_note("The BOM is changed with Document > Encoding"); break;
  case E_ALIGN: app_status_note("UTF-16 edits take whole 2-byte units (delete the odd trailing byte first)"); break;
  case E_RANGE:
    if (typing_char && t) app_status_note("This character can't be stored in %s", enc_label(t->doc->enc(), lab, sizeof(lab)));
    break;
  case E_NOMEM: app_status_note("Out of memory: close some tabs"); break;
  case E_TOOBIG: app_status_note("The document would become too large"); break;
  default: app_status_note("Edit failed (%s)", e_errname(rc)); break;
  }
}

static void hv_after_edit(HexView* v, int rc, int typing_char)
{
  if (rc != E_OK) { hv_edit_failed(v, rc, typing_char); return; }
  hx_track(v->t);                               // observe the state after the group closed (save detection)
  hv_validate(v);
  hv_inval_decor(v);
  hv_ensure_visible(v, v->t->hv.caret.pos, 0);
  hv_place_caret(v);
  hv_notify(v);
  app_on_doc_changed(v->t);
}

void HexView::doc_changed(Document* d, const EditEvent& e)
{
  epos len = d->length();
  if ((e.flags & EV_RESET) && (e.removed || e.inserted) && t) {
    HxSaved* s = hx_saved_find(t, 0);
    if (s) { hx_add_end(d, &s->wm_src, &s->wm_fill); s->mod = (u8)(d->modified() ? 1 : 0); s->seq = d->undo_seq(); }
  }
  if (!w) return;
  if ((e.flags & (EV_RESET | EV_INDEXED)) || e.removed != e.inserted || len != last_len) {
    epos from = (e.flags & EV_RESET) ? 0 : e.pos;
    RECT rc;
    u32 row = (u32)(from / HEX_BPR), top = t ? t->hv.top_row : 0;
    rc.left = 0; rc.right = cx; rc.bottom = data_bottom;
    rc.top = row <= top ? data_top : (row - top > (u32)rows_vis + 1 ? data_bottom : data_top + (int)(row - top) * ch);
    if (rc.top < rc.bottom) InvalidateRect(w, &rc, FALSE);
    last_len = len;
    hv_validate(this);
    hv_update_scroll(this);
  } else {
    hv_inval_rows(this, e.pos, e.pos + (e.inserted ? e.inserted - 1 : 0));
  }
  hv_inval_decor(this);
}

static void hv_type_nibble(HexView* v, u8 nib)
{
  Tab* t = v->t;
  int rc = hx_type_nibble(t->doc, &t->hv.caret, &t->sel, nib, app_now_ms());
  hv_after_edit(v, rc, 0);
}

static void hv_type_char(HexView* v, ecp c)
{
  Tab* t = v->t;
  int rc = hx_type_char(t->doc, &t->hv.caret, &t->sel, c, app_now_ms());
  hv_after_edit(v, rc, 1);
}

static void hv_delete(HexView* v, int backspace)
{
  Tab* t = v->t;
  epos before = t->hv.caret.pos;
  u32 seq = t->doc->change_seq();
  int rc = hx_delete(t->doc, &t->hv.caret, &t->sel, backspace);
  if (rc == E_RANGE) { MessageBeep(MB_OK); return; }
  if (rc == E_OK && seq == t->doc->change_seq()) {
    // overwrite-mode Backspace only moved the caret
    hv_inval_rows(v, t->hv.caret.pos, before);
    hv_inval_decor(v);
    hv_ensure_visible(v, t->hv.caret.pos, 0);
    hv_place_caret(v);
    hv_notify(v);
    return;
  }
  hv_after_edit(v, rc, 0);
}

static void hv_toggle_insert(HexView* v)
{
  if (!v->t) return;
  v->t->hv.caret.insert = (u8)!v->t->hv.caret.insert;
  v->t->hv.caret.unit_end = 0;
  if (v->focused) { DestroyCaret(); hv_make_caret(v); }
  hv_notify(v);
  {
    HWND p = GetParent(v->w);
    if (p && !v->is_panel) SendMessageA(p, WM_APP_DOCSTATE, 0, 0);
  }
}

// ---- keyboard ----

static void hv_key(HexView* v, UINT vk)
{
  Tab* t = v->t;
  HexCaret* c;
  int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
  epos pos, len, lo, hi;
  u32 page;
  if (!t) return;
  hv_validate(v);
  c = &t->hv.caret;
  pos = c->pos; len = t->doc->length();
  lo = sel_lo(t->sel); hi = sel_hi(t->sel);
  page = (u32)(v->rows_vis > 1 ? v->rows_vis - 1 : 1) * HEX_BPR;
  switch (vk) {
  case VK_LEFT:
    if (ctrl) { epos g = (pos % 8 == 0 && c->nibble == 0) ? (pos >= 8 ? pos - 8 : 0) : pos - pos % 8; hv_set_caret(v, g, 0, shift, 1); }
    else if (shift) { if (pos) hv_set_caret(v, pos - 1, 0, 1, 1); }
    else if (lo != hi) hv_set_caret(v, lo, 0, 0, 1);
    else if (c->pane == 0 && c->nibble) hv_set_caret(v, pos, 0, 0, 1);
    else if (pos) hv_set_caret(v, pos - 1, (u8)(c->pane == 0 ? 1 : 0), 0, 1);
    break;
  case VK_RIGHT:
    if (ctrl) { epos g = pos - pos % 8 + 8; hv_set_caret(v, g > len ? len : g, 0, shift, 1); }
    else if (shift) { if (pos < len) hv_set_caret(v, pos + 1, 0, 1, 1); }
    else if (lo != hi) hv_set_caret(v, hi, 0, 0, 1);
    else if (c->pane == 0 && c->nibble == 0 && pos < len) hv_set_caret(v, pos, 1, 0, 1);
    else if (pos < len) hv_set_caret(v, pos + 1, 0, 0, 1);
    break;
  case VK_UP:
    if (ctrl) { if (t->hv.top_row) hv_scroll_to(v, t->hv.top_row - 1); }
    else if (pos >= HEX_BPR) hv_set_caret(v, pos - HEX_BPR, shift ? 0 : c->nibble, shift, 1);
    else if (shift) hv_set_caret(v, 0, 0, 1, 1);
    break;
  case VK_DOWN:
    if (ctrl) hv_scroll_to(v, t->hv.top_row + 1);
    else if (len - pos >= HEX_BPR) hv_set_caret(v, pos + HEX_BPR, shift ? 0 : c->nibble, shift, 1);
    else if (pos / HEX_BPR < len / HEX_BPR) hv_set_caret(v, len, 0, shift, 1);   // last row: to the end
    else if (shift) hv_set_caret(v, len, 0, 1, 1);
    break;
  case VK_PRIOR: {
    u32 top = t->hv.top_row, rows = page / HEX_BPR;
    epos np = pos >= page ? pos - page : pos % HEX_BPR;
    hv_scroll_to(v, top > rows ? top - rows : 0);
    hv_set_caret(v, np, shift ? 0 : c->nibble, shift, 1);
    break;
  }
  case VK_NEXT: {
    u32 rows = page / HEX_BPR;
    epos np = (len - pos >= page) ? pos + page : len;
    hv_scroll_to(v, t->hv.top_row + rows);
    hv_set_caret(v, np, shift ? 0 : c->nibble, shift, 1);
    break;
  }
  case VK_HOME:
    if (ctrl) hv_set_caret(v, 0, 0, shift, 1);
    else hv_set_caret(v, pos - pos % HEX_BPR, 0, shift, 1);
    break;
  case VK_END:
    if (ctrl) hv_set_caret(v, len, 0, shift, 1);
    else {
      epos e = pos - pos % HEX_BPR + HEX_BPR - 1;
      if (shift) e++;                             // Shift+End selects through the row's last byte
      hv_set_caret(v, e >= len ? len : e, 0, shift, 1);
    }
    break;
  case VK_TAB:
    c->pane = (u8)!c->pane;
    c->nibble = 0;
    c->unit_end = 0;
    v->lead = 0;
    hv_inval_rows(v, pos, pos);
    hv_ensure_visible(v, pos, 0);
    hv_place_caret(v);
    hv_notify(v);
    break;
  case VK_INSERT:
    if (!shift && !ctrl) hv_toggle_insert(v);
    break;
  case VK_DELETE:
    if (!shift && !ctrl) hv_delete(v, 0);
    break;
  case VK_BACK:
    hv_delete(v, 1);
    break;
  default:
    break;
  }
}

static void hv_char(HexView* v, UINT ch)
{
  Tab* t = v->t;
  if (!t) return;
  if (ch < 0x20 || ch == 0x7F) return;          // controls: keys are handled in WM_KEYDOWN
  if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return;
  hv_validate(v);
  if (t->hv.caret.pane == 0) {
    int d = -1;
    if (ch >= '0' && ch <= '9') d = (int)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') d = (int)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') d = (int)(ch - 'A' + 10);
    if (d < 0) { MessageBeep(MB_OK); app_status_note("Type hex digits 0-9, A-F here (Tab switches to the text column)"); return; }
    hv_type_nibble(v, (u8)d);
    return;
  }
  {
    // char pane: the byte is in the input locale's code page (DBCS: lead byte first)
    u16 cp = winenc_input_cp();
    char mb[3];
    u32 n = 0;
    Vec<ecp> out;
    if (v->lead) { mb[0] = (char)v->lead; mb[1] = (char)ch; n = 2; v->lead = 0; }
    else if (ch < 0x100 && ch >= 0x80 && IsDBCSLeadByteEx(cp, (BYTE)ch)) { v->lead = (u8)ch; return; }
    else { mb[0] = (char)ch; n = 1; }
    mb[n] = 0;
    if (winenc_mb_to_ecp(cp, mb, n, &out) < 0 || out.size() == 0) {
      if (ui_ansi_to_ecp(mb, &out) < 0 || out.size() == 0) { MessageBeep(MB_OK); return; }
    }
    hv_type_char(v, out[0]);
  }
}

// ---- mouse ----

// Client point -> byte position, nibble and pane. Returns 0 outside the data rows.
static int hv_hit(HexView* v, int x, int y, epos* pos, u8* nibble, u8* pane, int clamp_rows)
{
  int cell, row_off;
  u32 row, i;
  epos len = hv_len(v), p;
  if (!v->t) return 0;
  if (!clamp_rows && (y < v->data_top || y >= v->data_bottom)) return 0;
  if (y < v->data_top) row_off = -1 - (v->data_top - y - 1) / v->ch;
  else row_off = (y - v->data_top) / v->ch;
  if (row_off < 0) row = (u32)(-row_off) > v->t->hv.top_row ? 0 : v->t->hv.top_row - (u32)(-row_off);
  else row = v->t->hv.top_row + (u32)row_off;
  if (row >= hv_total_rows(v)) row = hv_total_rows(v) - 1;
  cell = (x - HX_MARGIN) >= 0 ? (x - HX_MARGIN) / v->cw + v->hscroll : v->hscroll - 1;
  *nibble = 0;
  if (cell < HX_HEX0) { *pane = 0; i = 0; }
  else if (cell < HX_CHR0 - 1) {
    *pane = 0;
    for (i = 0; i < HEX_BPR - 1; i++) if (cell < hv_byte_cell(i) + 2) break;
    if (cell >= hv_byte_cell(i)) *nibble = (u8)((cell - hv_byte_cell(i)) >= 1 ? 1 : 0);
  } else {
    *pane = 1;
    i = (cell >= HX_CHR0) ? (u32)(cell - HX_CHR0) : 0;
    if (i >= HEX_BPR) i = HEX_BPR - 1;
  }
  p = (epos)row * HEX_BPR + i;
  if (p >= len) { p = len; *nibble = 0; }
  *pos = p;
  return 1;
}

// selection while dragging: bytes touched by anchor byte a and current byte b
static void hv_drag_to(HexView* v, epos b)
{
  Tab* t = v->t;
  epos a = v->drag_anchor, len = t->doc->length();
  if (b == a) { hv_set_caret(v, a, t->hv.caret.nibble, 0, 1); return; }
  if (b >= len && len) b = len - 1;
  if (b >= a) { t->sel.anchor = a; hv_set_caret(v, b + 1 > len ? len : b + 1, 0, 1, 1); }
  else { t->sel.anchor = a + 1 > len ? len : a + 1; hv_set_caret(v, b, 0, 1, 1); }
}

static void hv_context_menu(HexView* v, int sx, int sy)
{
  struct { int id; const char* text; } items[] = {
    { IDM_EDIT_UNDO, "&Undo\tCtrl+Z" }, { IDM_EDIT_REDO, "&Redo\tCtrl+Y" }, { 0, 0 },
    { IDM_EDIT_CUT, "Cu&t\tCtrl+X" }, { IDM_EDIT_COPY, "&Copy\tCtrl+C" }, { IDM_EDIT_PASTE, "&Paste\tCtrl+V" },
    { IDM_EDIT_DELETE, "&Delete\tDel" }, { 0, 0 }, { IDM_EDIT_SELECTALL, "Select &All\tCtrl+A" }
  };
  HMENU m = CreatePopupMenu();
  u32 i;
  int id;
  if (!m) return;
  for (i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
    if (!items[i].id) AppendMenuA(m, MF_SEPARATOR, 0, 0);
    else AppendMenuA(m, MF_STRING | (cmd_state(items[i].id) & MF_GRAYED), (UINT)items[i].id, items[i].text);
  }
  id = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, sx, sy, 0, v->w, 0);
  DestroyMenu(m);
  if (id) app_command(id);
}

// ---- window procedure ----

static LRESULT CALLBACK hv_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  HexView* v = hv_get(w);
  switch (msg) {
  case WM_CREATE: {
    CREATESTRUCTA* cs = (CREATESTRUCTA*)lp;
    v = new HexView;
    if (!v) return -1;
    v->w = w;
    v->is_panel = cs->lpCreateParams ? 1 : 0;
    v->insp_on = g_app.cfg.inspector_on ? 1 : 0;
    SetWindowLongA(w, GWL_USERDATA, (LONG)v);
    hv_fonts(v);
    hv_layout(v);
    return 0;
  }
  case WM_DESTROY:
    if (v) {
      if (v->doc) { v->doc->remove_listener(v); v->doc = 0; }
      if (v->own_font && v->font) DeleteObject(v->font);
      SetWindowLongA(w, GWL_USERDATA, 0);
      delete v;
    }
    return 0;
  case WM_SIZE:
    if (v) { hv_layout(v); InvalidateRect(w, 0, FALSE); hv_place_caret(v); }
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    if (v) hv_paint(v);
    else { PAINTSTRUCT ps; BeginPaint(w, &ps); EndPaint(w, &ps); }
    return 0;
  case WM_SETFOCUS:
    if (v) { v->focused = 1; hv_make_caret(v); }
    return 0;
  case WM_KILLFOCUS:
    if (v) { v->focused = 0; v->caret_shown = 0; DestroyCaret(); if (v->dragging) { v->dragging = 0; ReleaseCapture(); } }
    return 0;
  case WM_GETDLGCODE:
    return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
  case WM_KEYDOWN:
    if (v) hv_key(v, (UINT)wp);
    return 0;
  case WM_CHAR:
    if (v) hv_char(v, (UINT)(wp & 0xFF));
    return 0;
  case WM_VSCROLL:
    if (v && v->t) {
      u32 top = v->t->hv.top_row, rows = (u32)(v->rows_vis > 1 ? v->rows_vis - 1 : 1);
      switch (LOWORD(wp)) {
      case SB_LINEUP: if (top) hv_scroll_to(v, top - 1); break;
      case SB_LINEDOWN: hv_scroll_to(v, top + 1); break;
      case SB_PAGEUP: hv_scroll_to(v, top > rows ? top - rows : 0); break;
      case SB_PAGEDOWN: hv_scroll_to(v, top + rows); break;
      case SB_TOP: hv_scroll_to(v, 0); break;
      case SB_BOTTOM: hv_scroll_to(v, hv_max_top(v)); break;
      case SB_THUMBTRACK: case SB_THUMBPOSITION: {
        SCROLLINFO si;
        u32 total = hv_total_rows(v);
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS | SIF_PAGE;
        GetScrollInfo(w, SB_VERT, &si);
        if (total <= HX_SCROLL_MAX) hv_scroll_to(v, (u32)si.nTrackPos);
        else {
          u32 range = HX_SCROLL_MAX + 1 - si.nPage;
          hv_scroll_to(v, range ? (u32)(((u64)(u32)si.nTrackPos * hv_max_top(v)) / range) : 0);
        }
        break;
      }
      default: break;
      }
    }
    return 0;
  case WM_HSCROLL:
    if (v) {
      switch (LOWORD(wp)) {
      case SB_LINELEFT: hv_hscroll_to(v, v->hscroll - 1); break;
      case SB_LINERIGHT: hv_hscroll_to(v, v->hscroll + 1); break;
      case SB_PAGELEFT: hv_hscroll_to(v, v->hscroll - v->vis_cells / 2); break;
      case SB_PAGERIGHT: hv_hscroll_to(v, v->hscroll + v->vis_cells / 2); break;
      case SB_THUMBTRACK: case SB_THUMBPOSITION: hv_hscroll_to(v, (int)(short)HIWORD(wp)); break;
      default: break;
      }
    }
    return 0;
  case WM_MOUSEWHEEL:
    if (v && v->t) {
      int steps;
      v->wheel_acc += (short)HIWORD(wp);
      steps = v->wheel_acc / WHEEL_DELTA;
      v->wheel_acc -= steps * WHEEL_DELTA;
      if (steps > 0) { u32 d = (u32)steps * 3; hv_scroll_to(v, v->t->hv.top_row > d ? v->t->hv.top_row - d : 0); }
      else if (steps < 0) hv_scroll_to(v, v->t->hv.top_row + (u32)(-steps) * 3);
    }
    return 0;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
    if (v) {
      int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
      epos pos; u8 nib, pane;
      SetFocus(w);
      if (!v->t) return 0;
      if (v->insp_on && y >= v->data_bottom) {
        POINT pt; pt.x = x; pt.y = y;
        if (PtInRect(&v->insp_toggle, pt)) {
          g_app.cfg.inspector_be = (u8)!g_app.cfg.inspector_be;
          hv_inval_decor(v);
        }
        return 0;
      }
      if (!hv_hit(v, x, y, &pos, &nib, &pane, 0)) return 0;
      v->t->hv.caret.pane = pane;
      if (msg == WM_LBUTTONDBLCLK) {
        epos len = v->t->doc->length();
        if (pos < len) { v->t->sel.anchor = pos; hv_set_caret(v, pos + 1, 0, 1, 1); }
        return 0;
      }
      if (wp & MK_SHIFT) {
        hv_set_caret(v, pos, 0, 1, 1);
        v->drag_anchor = v->t->sel.anchor;
      } else {
        hv_set_caret(v, pos, (u8)(pane == 0 ? nib : 0), 0, 1);
        v->drag_anchor = pos;
      }
      v->dragging = 1;
      v->drag_pane = pane;
      SetCapture(w);
    }
    return 0;
  case WM_MOUSEMOVE:
    if (v && v->dragging && v->t) {
      int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
      epos pos; u8 nib, pane;
      v->drag_dir = y < v->data_top ? -1 : (y >= v->data_bottom ? 1 : 0);
      if (v->drag_dir) SetTimer(w, HX_TIMER_DRAG, 50, 0);
      if (hv_hit(v, x, y, &pos, &nib, &pane, 1)) {
        hv_drag_to(v, pos);
      }
    }
    return 0;
  case WM_LBUTTONUP:
    if (v && v->dragging) { v->dragging = 0; v->drag_dir = 0; KillTimer(w, HX_TIMER_DRAG); ReleaseCapture(); }
    return 0;
  case WM_CAPTURECHANGED:
    if (v) { v->dragging = 0; KillTimer(w, HX_TIMER_DRAG); }
    return 0;
  case WM_TIMER:
    if (v && wp == HX_TIMER_DRAG) {
      if (!v->dragging || !v->drag_dir || !v->t) { KillTimer(w, HX_TIMER_DRAG); return 0; }
      {
        POINT pt;
        epos pos; u8 nib, pane;
        u32 top = v->t->hv.top_row;
        if (v->drag_dir < 0 && top) hv_scroll_to(v, top - 1);
        else if (v->drag_dir > 0) hv_scroll_to(v, top + 1);
        GetCursorPos(&pt);
        ScreenToClient(w, &pt);
        if (hv_hit(v, pt.x, pt.y, &pos, &nib, &pane, 1)) hv_drag_to(v, pos);
      }
    } else if (v && wp == HX_TIMER_SAVE) {
      if (v->t && hx_track(v->t)) { InvalidateRect(w, 0, FALSE); }
    }
    return 0;
  case WM_RBUTTONDOWN:
    if (v && v->t) {
      epos pos; u8 nib, pane;
      SetFocus(w);
      if (hv_hit(v, (short)LOWORD(lp), (short)HIWORD(lp), &pos, &nib, &pane, 0)) {
        if (pos < sel_lo(v->t->sel) || pos >= sel_hi(v->t->sel)) { v->t->hv.caret.pane = pane; hv_set_caret(v, pos, 0, 0, 1); }
      }
    }
    return 0;
  case WM_CONTEXTMENU:
    if (v && v->t) {
      int sx = (short)LOWORD(lp), sy = (short)HIWORD(lp);
      if (sx == -1 && sy == -1) {
        POINT pt;
        int cell;
        hv_caret_cell(v, &cell);
        pt.x = hv_cell_x(v, cell);
        pt.y = v->data_top + (int)((v->t->hv.caret.pos / HEX_BPR) - v->t->hv.top_row) * v->ch + v->ch;
        ClientToScreen(w, &pt);
        sx = pt.x; sy = pt.y;
      }
      hv_context_menu(v, sx, sy);
    }
    return 0;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

// ---- API (views.h) ----

HWND hexview_create(HWND parent, int id, int is_panel)
{
  static int registered = 0;
  if (!registered) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = hv_proc;
    wc.hInstance = g_app.hinst;
    wc.hCursor = LoadCursor(0, IDC_IBEAM);
    wc.hbrBackground = 0;
    wc.lpszClassName = k_hex_class;
    if (!RegisterClassA(&wc)) return 0;
    registered = 1;
  }
  return CreateWindowExA(WS_EX_CLIENTEDGE, k_hex_class, "", WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS,
                         0, 0, 100, 100, parent, (HMENU)id, g_app.hinst, (LPVOID)(is_panel ? 1 : 0));
}

void hexview_attach(HWND w, Tab* t)
{
  HexView* v = hv_get(w);
  if (!v) return;
  if (t && !t->doc) t = 0;
  if (v->t == t) {
    if (t) { hv_validate(v); hv_update_scroll(v); InvalidateRect(w, 0, FALSE); hv_place_caret(v); }
    g_switch_from_text = 0;
    return;
  }
  if (v->doc) { v->doc->remove_listener(v); v->doc = 0; }
  if (v->dragging) { v->dragging = 0; ReleaseCapture(); }
  KillTimer(w, HX_TIMER_SAVE);
  v->t = t;
  v->lead = 0;
  if (!t) {
    ShowWindow(w, SW_HIDE);           // (app_activate detaches every view before attaching: keep g_switch_from_text)
    return;
  }
  if (t->doc->add_listener(v) == E_OK) v->doc = t->doc;
  v->last_len = t->doc->length();
  hx_track(t);
  hv_fonts(v);
  hv_layout(v);
  if (g_switch_from_text) {
    // keep the caret on the same screen row as in the text view (if it was visible there), else centre it
    u32 line = t->doc->line_of(t->sel.caret), crow = (u32)(t->hv.caret.pos / HEX_BPR);
    u32 srow = line >= t->tv.top_line ? line - t->tv.top_line : 0xFFFFFFFFu;
    if (srow < (u32)v->rows_vis) t->hv.top_row = crow >= srow ? crow - srow : 0;
    else t->hv.top_row = crow > (u32)v->rows_vis / 2 ? crow - (u32)v->rows_vis / 2 : 0;
    if (t->hv.top_row > hv_max_top(v)) t->hv.top_row = hv_max_top(v);
    g_switch_from_text = 0;
  }
  hv_validate(v);
  hv_update_scroll(v);
  SetTimer(w, HX_TIMER_SAVE, 1000, 0);
  ShowWindow(w, SW_SHOW);
  InvalidateRect(w, 0, FALSE);
  if (v->focused) { DestroyCaret(); hv_make_caret(v); }
}

void hexview_set_selection(HWND w, epos anchor, epos caret, int scroll_mode)
{
  HexView* v = hv_get(w);
  Tab* t;
  epos len;
  if (!v || !v->t) return;
  t = v->t;
  len = t->doc->length();
  if (anchor > len) anchor = len;
  t->sel.anchor = anchor;
  hv_set_caret(v, caret, 0, 1, 0);
  InvalidateRect(w, 0, FALSE);
  if (scroll_mode == SCROLL_MIN) hv_ensure_visible(v, t->hv.caret.pos, 0);
  else if (scroll_mode == SCROLL_CENTER) hv_ensure_visible(v, t->hv.caret.pos, 1);
  hv_place_caret(v);
}

void hexview_ensure_visible(HWND w, epos pos, int center)
{
  HexView* v = hv_get(w);
  if (v) hv_ensure_visible(v, pos, center);
}

void hexview_invalidate(HWND w)
{
  HexView* v = hv_get(w);
  if (!v) return;
  hv_validate(v);
  hv_update_scroll(v);
  InvalidateRect(w, 0, FALSE);
  hv_place_caret(v);
}

void hexview_config_changed(HWND w)
{
  HexView* v = hv_get(w);
  if (!v) return;
  hv_fonts(v);
  v->insp_on = g_app.cfg.inspector_on ? 1 : 0;
  hv_layout(v);
  InvalidateRect(w, 0, FALSE);
  if (v->focused) { DestroyCaret(); hv_make_caret(v); }
}

void hexview_caret_info(HWND w, CaretInfo* out)
{
  HexView* v = hv_get(w);
  memset(out, 0, sizeof(*out));
  out->view = VIEW_HEX;
  if (!v || !v->t) return;
  hv_validate(v);
  out->pos = v->t->hv.caret.pos;
  out->line = (u32)(out->pos / HEX_BPR);
  out->col = (u32)(out->pos % HEX_BPR) + 1;
  out->sel_len = sel_hi(v->t->sel) - sel_lo(v->t->sel);
  out->overwrite = (u8)(v->t->hv.caret.insert ? 0 : 1);
  if (out->pos < v->t->doc->length()) {
    u8 b = 0;
    if (v->t->doc->read(out->pos, &b, 1) == E_OK) { out->ch = b; out->has_ch = 1; }
  }
}

void hexview_set_panel_handler(HWND w, PanelHandler* h) { (void)w; (void)h; }   // the hex view is never a panel

void hexview_set_inspector(HWND w, int on)
{
  HexView* v = hv_get(w);
  if (!v) return;
  v->insp_on = on ? 1 : 0;
  hv_layout(v);
  InvalidateRect(w, 0, FALSE);
  hv_place_caret(v);
}

// ---- edit commands in the hex view (used by the selftest; the real commands are in editcmds.cpp) ----

static int hv_paste(HexView* v)
{
  Tab* t = v->t;
  ClipData cd;
  EBytes bytes;
  int rc;
  u32 unrep = 0;
  cd.has_raw = 0;
  rc = clip_get(&cd);
  if (rc < 0) { clip_free(&cd); MessageBeep(MB_OK); if (rc != E_NOTFOUND) app_status_note("Nothing to paste (%s)", e_errname(rc)); return 1; }
  if (cd.has_raw) rc = bytes.append(cd.raw.data(), cd.raw.size());
  else {
    rc = E_SYNTAX;
    if (t->hv.caret.pane == 0 && cd.text.size()) {
      // hex text ("4D 5A 90 00") pasted into the hex pane becomes those bytes
      EBytes asc;
      u32 i, ok = 1;
      for (i = 0; i < cd.text.size() && ok; i++) { if (cd.text[i] >= 0x80) ok = 0; else asc.push((u8)cd.text[i]); }
      if (ok && hex_parse_bytes((const char*)asc.data(), asc.size(), &bytes) == E_OK) rc = E_OK;
      else bytes.clear();
    }
    if (rc != E_OK) rc = codec_encode_str(t->doc->codec(), cd.text.data(), cd.text.size(), &bytes, &unrep, '?');
  }
  clip_free(&cd);
  if (rc < 0) { hv_edit_failed(v, rc, 0); return 1; }
  if (bytes.size() > 16u * 1024u * 1024u) { MessageBeep(MB_OK); app_status_note("Clipboard too large to paste (16 MB max)"); return 1; }
  rc = hx_write(t->doc, &t->hv.caret, &t->sel, bytes.data(), bytes.size());
  hv_after_edit(v, rc, 0);
  if (rc == E_OK) {
    InvalidateRect(v->w, 0, FALSE);
    if (unrep) app_status_note("%u character(s) can't be stored in this encoding: pasted as ?", unrep);
  }
  return 1;
}

static int hexview_command(HWND w, int id)
{
  HexView* v = hv_get(w);
  Tab* t;
  int rc;
  if (!v || !v->t) return 0;
  t = v->t;
  hv_validate(v);
  switch (id) {
  case IDM_EDIT_UNDO: case IDM_EDIT_REDO: {
    Selection s;
    rc = (id == IDM_EDIT_UNDO) ? t->doc->undo(&s) : t->doc->redo(&s);
    if (rc == E_STATE) { MessageBeep(MB_OK); return 1; }
    if (rc != E_OK) { hv_edit_failed(v, rc, 0); return 1; }
    t->sel = s;
    hx_track(t);
    hv_validate(v);
    t->hv.caret.nibble = 0; t->hv.caret.unit_end = 0;
    InvalidateRect(w, 0, FALSE);
    hv_ensure_visible(v, t->hv.caret.pos, 0);
    hv_place_caret(v);
    hv_notify(v);
    app_on_doc_changed(t);
    return 1;
  }
  case IDM_EDIT_COPY: case IDM_EDIT_CUT:
    if (sel_empty(t->sel)) { MessageBeep(MB_OK); return 1; }
    rc = clip_copy(t, t->sel, t->hv.caret.pane == 0 ? 1 : 0);
    if (rc == E_TOOBIG) { MessageBeep(MB_OK); app_status_note("Selection too large to copy (16 MB max)"); return 1; }
    if (rc < 0) { MessageBeep(MB_OK); app_status_note("Copy failed (%s)", e_errname(rc)); return 1; }
    if (id == IDM_EDIT_CUT) {
      rc = hx_delete(t->doc, &t->hv.caret, &t->sel, 0);
      hv_after_edit(v, rc, 0);
    }
    return 1;
  case IDM_EDIT_PASTE:
    return hv_paste(v);
  case IDM_EDIT_DELETE:
    hv_delete(v, 0);
    return 1;
  case IDM_EDIT_SELECTALL:
    t->sel.anchor = 0;
    hv_set_caret(v, t->doc->length(), 0, 1, 0);
    InvalidateRect(w, 0, FALSE);
    return 1;
  case IDM_EDIT_OVERWRITE:
    hv_toggle_insert(v);
    return 1;
  default:
    return 0;
  }
}

// ---- commands ----

// Hex -> text selection: caret to the start of its character, low end down, high end up, all >= origin.
static void hv_map_to_text(Document* d, Selection* s)
{
  epos lo = sel_lo(*s), hi = sel_hi(*s), sl, sh;
  int caret_hi = s->caret > s->anchor;
  if (lo == hi) { s->anchor = s->caret = d->snap_char(s->caret); return; }
  sl = d->snap_char(lo);
  sh = d->snap_char(hi);
  if (sh < hi) sh = d->char_next(sh);
  if (sh < sl) sh = sl;
  if (caret_hi) { s->anchor = sl; s->caret = sh; }
  else { s->anchor = sh; s->caret = sl; }
}

void cmd_view_hex(void)
{
  Tab* t = app_active();
  int idx = app_active_index();
  if (!t || !t->doc) { MessageBeep(MB_OK); return; }
  if (t->view == VIEW_TEXT) {
    // flush the text view's state into the tab first (caret, top line)
    if (g_app.textview) textview_attach(g_app.textview, 0);
    t->hv.caret.pos = t->sel.caret;
    t->hv.caret.nibble = 0;
    t->hv.caret.unit_end = 0;
    t->view = VIEW_HEX;
    g_switch_from_text = 1;
    app_activate(idx);
    if (g_app.textview) ShowWindow(g_app.textview, SW_HIDE);
    if (g_app.hexview) { SetFocus(g_app.hexview); hexview_ensure_visible(g_app.hexview, t->sel.caret, 0); }
  } else {
    HexView* v = hv_get(g_app.hexview);
    u32 srow = 0xFFFFFFFFu;
    Selection s = t->sel;
    if (v && v->t == t) {
      u32 crow = (u32)(t->hv.caret.pos / HEX_BPR);
      if (crow >= t->hv.top_row && crow < t->hv.top_row + (u32)v->rows_vis) srow = crow - t->hv.top_row;
    }
    hv_map_to_text(t->doc, &s);
    t->sel = s;
    {
      u32 line = t->doc->line_of(s.caret);
      if (srow != 0xFFFFFFFFu) t->tv.top_line = line >= srow ? line - srow : 0;
      t->tv.top_subrow = 0;
    }
    t->view = VIEW_TEXT;
    if (g_app.hexview) hexview_attach(g_app.hexview, 0);
    app_activate(idx);
    if (g_app.textview) {
      ShowWindow(g_app.textview, SW_SHOW);
      textview_set_selection(g_app.textview, s.anchor, s.caret, SCROLL_MIN);
      SetFocus(g_app.textview);
    }
  }
  g_switch_from_text = 0;
  app_update_status();
  app_update_title();
}

void cmd_view_inspector(void)
{
  g_app.cfg.inspector_on = (u8)!g_app.cfg.inspector_on;
  if (g_app.hexview) hexview_set_inspector(g_app.hexview, g_app.cfg.inspector_on);
}

UINT cmdstate_view_hex(void)
{
  Tab* t = app_active();
  if (!t) return MF_GRAYED;
  return t->view == VIEW_HEX ? MF_CHECKED : 0;
}

UINT cmdstate_view_inspector(void) { return g_app.cfg.inspector_on ? MF_CHECKED : 0; }

// ---- selftest (st_hexview) ----

static Tab* st_hx_tab(StCtx* c, const u8* bytes, u32 n, const EncSpec& enc, epos bom, int hex)
{
  MemSource* src = new MemSource;
  Document* d = new Document;
  Tab* t;
  int rc;
  if (!src || !d) { delete src; delete d; st_checkf(c, 0, "st_hx_tab: out of memory"); return 0; }
  if (n) src->copy_from(bytes, n);
  rc = d->init_source(src, enc, bom, EOL_CRLF, 0);
  if (rc != E_OK) { delete d; st_checkf(c, 0, "st_hx_tab: init_source %s", e_errname(rc)); return 0; }
  t = tab_new(d);
  if (!t) { delete d; st_checkf(c, 0, "st_hx_tab: tab_new failed"); return 0; }
  t->untitled = 1;
  t->untitled_no = 900 + t->id;
  t->view = (u8)(hex ? VIEW_HEX : VIEW_TEXT);
  t->hv.caret.insert = 0;
  if (app_add_tab(t, 1) != E_OK) { tab_free(t); st_checkf(c, 0, "st_hx_tab: app_add_tab failed"); return 0; }
  st_pump(c, 0);
  return t;
}

static void st_hx_close(StCtx* c, Tab* t)
{
  if (!t) return;
  app_remove_tab(t);
  tab_free(t);
  st_pump(c, 0);
}

static void st_hx_parse(const char* s, EBytes* out)
{
  int hi = -1;
  out->clear();
  for (; *s; s++) {
    int v = -1;
    if (*s >= '0' && *s <= '9') v = *s - '0';
    else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
    else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
    if (v < 0) continue;
    if (hi < 0) hi = v;
    else { out->push((u8)(hi * 16 + v)); hi = -1; }
  }
}

static int st_hx_is(Tab* t, const char* hex)
{
  EBytes want, got;
  st_hx_parse(hex, &want);
  if (!t || t->doc->length() != want.size()) return 0;
  if (got.resize(want.size()) != E_OK) return 0;
  if (want.size() && t->doc->read(0, got.data(), want.size()) != E_OK) return 0;
  return want.size() == 0 || memcmp(got.data(), want.data(), want.size()) == 0;
}

static void st_hx_dump(StCtx* c, Tab* t, const char* what)
{
  char buf[200];
  u8 b[48];
  u32 n = t->doc->length() > 48 ? 48 : (u32)t->doc->length(), i, k = 0;
  t->doc->read(0, b, n);
  buf[0] = 0;
  for (i = 0; i < n && k + 4 < sizeof(buf); i++) k += (u32)es_fmt(buf + k, sizeof(buf) - k, "%02X ", b[i]);
  selftest_log("%s: bytes [%s] caret %u.%u pane %u sel %u,%u", what, buf, (u32)t->hv.caret.pos, (u32)t->hv.caret.nibble,
               (u32)t->hv.caret.pane, (u32)t->sel.anchor, (u32)t->sel.caret);
}

#define ST_HX(c, t, hex, msg) do { int ok_ = st_hx_is((t), (hex)); if (!ok_) st_hx_dump((c), (t), (msg)); \
  st_checkf((c), ok_, "%s: want [%s]", (msg), (hex)); } while (0)

static void st_hx_click(HexView* v, int cell, u32 row, UINT msg, WPARAM keys)
{
  int x = hv_cell_x(v, cell) + 1;
  int y = v->data_top + (int)(row - v->t->hv.top_row) * v->ch + 1;
  SendMessageA(v->w, msg, keys, MAKELPARAM(x, y));
}

static void st_hx_key(StCtx* c, UINT vk)
{
  SendMessageA(g_app.hexview, WM_KEYDOWN, (WPARAM)vk, 1);
  SendMessageA(g_app.hexview, WM_KEYUP, (WPARAM)vk, (LPARAM)0xC0000001u);
  st_pump(c, 0);
}

static int st_hx_contains(const char* s, const char* sub)
{
  u32 n = es_len(sub);
  for (; *s; s++) if (memcmp(s, sub, n) == 0) return 1;
  return 0;
}

static void st_hx_mapping(StCtx* c)
{
  static const u8 k_utf8[] = { 'a', 0xC3, 0xA9, 'b', 0xE2, 0x82, 0xAC, 'c' };
  static const u8 k_bom[] = { 0xEF, 0xBB, 0xBF, 'A', 'B' };
  Tab* t = st_hx_tab(c, k_utf8, sizeof(k_utf8), enc_utf8(0), 0, 0);
  CaretInfo ci;
  if (!t) return;
  t->sel.anchor = 1; t->sel.caret = 4;
  if (g_app.textview) textview_set_selection(g_app.textview, 1, 4, SCROLL_NONE);
  st_cmd(c, IDM_VIEW_HEX);
  ST_CHECK(c, t->view == VIEW_HEX, "text -> hex switches the view");
  ST_CHECK(c, IsWindowVisible(g_app.hexview), "hex view visible");
  ST_CHECK(c, cmdstate_view_hex() & MF_CHECKED, "View > Hex View checked");
  hexview_caret_info(g_app.hexview, &ci);
  st_checkf(c, ci.pos == 4 && ci.sel_len == 3 && ci.view == VIEW_HEX && ci.has_ch && ci.ch == 0xE2,
            "text -> hex keeps the byte range: pos %u sel %u ch %u", (u32)ci.pos, (u32)ci.sel_len, (u32)ci.ch);
  ST_HX(c, t, "61 C3 A9 62 E2 82 AC 63", "switching never changes a byte");
  // hex selection inside characters: low end down, high end up
  hexview_set_selection(g_app.hexview, 2, 5, SCROLL_NONE);
  st_cmd(c, IDM_VIEW_HEX);
  ST_CHECK(c, t->view == VIEW_TEXT, "hex -> text switches the view");
  st_checkf(c, t->sel.anchor == 1 && t->sel.caret == 7, "hex 2..5 -> text 1..7 (got %u..%u)", (u32)t->sel.anchor,
            (u32)t->sel.caret);
  st_cmd(c, IDM_VIEW_HEX);
  st_checkf(c, t->hv.caret.pos == 7 && sel_lo(t->sel) == 1, "back in hex: caret 7 (got %u)", (u32)t->hv.caret.pos);
  hexview_set_selection(g_app.hexview, 5, 2, SCROLL_NONE);          // caret at the low end
  st_cmd(c, IDM_VIEW_HEX);
  st_checkf(c, t->sel.anchor == 7 && t->sel.caret == 1, "hex 5..2 -> text 7..1 (got %u..%u)", (u32)t->sel.anchor,
            (u32)t->sel.caret);
  st_cmd(c, IDM_VIEW_HEX);
  hexview_set_selection(g_app.hexview, 5, 5, SCROLL_NONE);
  st_cmd(c, IDM_VIEW_HEX);
  st_checkf(c, t->sel.anchor == 4 && t->sel.caret == 4, "hex caret 5 -> text 4 (got %u)", (u32)t->sel.caret);
  ST_HX(c, t, "61 C3 A9 62 E2 82 AC 63", "still unchanged");
  st_hx_close(c, t);
  // BOM: the text caret never goes below the origin
  t = st_hx_tab(c, k_bom, sizeof(k_bom), enc_utf8(1), 3, 1);
  if (!t) return;
  hexview_set_selection(g_app.hexview, 1, 1, SCROLL_NONE);
  st_cmd(c, IDM_VIEW_HEX);
  st_checkf(c, t->sel.caret == 3 && t->sel.anchor == 3, "hex caret in the BOM -> text origin (got %u)", (u32)t->sel.caret);
  st_hx_close(c, t);
}

static void st_hx_typing(StCtx* c)
{
  static const u8 k_init[] = { 0x00, 0x11, 0x22, 0x33 };
  Tab* t = st_hx_tab(c, k_init, sizeof(k_init), enc_ansi(), 0, 1);
  CaretInfo ci;
  u32 undos = 0;
  if (!t) return;
  SetFocus(g_app.hexview);
  hexview_set_selection(g_app.hexview, 0, 0, SCROLL_NONE);
  st_keys(c, "ab");
  ST_HX(c, t, "AB 11 22 33", "overwrite: two nibbles");
  st_checkf(c, t->hv.caret.pos == 1 && t->hv.caret.nibble == 0, "caret after a byte: %u.%u", (u32)t->hv.caret.pos,
            (u32)t->hv.caret.nibble);
  st_keys(c, "{INS}");
  hexview_caret_info(g_app.hexview, &ci);
  ST_CHECK(c, t->hv.caret.insert == 1 && ci.overwrite == 0, "Ins switches to insert mode");
  st_keys(c, "cd");
  ST_HX(c, t, "AB CD 11 22 33", "insert: a new byte");
  st_keys(c, "{INS}");
  ST_CHECK(c, t->hv.caret.insert == 0, "Ins back to overwrite");
  st_keys(c, "{DEL}");
  ST_HX(c, t, "AB CD 22 33", "Del deletes the byte at the caret");
  st_keys(c, "{BS}");
  ST_HX(c, t, "AB CD 22 33", "overwrite Backspace never shifts data");
  st_checkf(c, t->hv.caret.pos == 1, "overwrite Backspace moves left (caret %u)", (u32)t->hv.caret.pos);
  st_keys(c, "{INS}{BS}{INS}");
  ST_HX(c, t, "CD 22 33", "insert Backspace deletes the byte before the caret");
  st_checkf(c, t->hv.caret.pos == 0, "caret 0 (got %u)", (u32)t->hv.caret.pos);
  st_keys(c, "{TAB}");
  ST_CHECK(c, t->hv.caret.pane == 1, "Tab switches to the char pane");
  st_keys(c, "Z");
  ST_HX(c, t, "5A 22 33", "char pane typing writes the encoded character");
  st_keys(c, "{TAB}");
  ST_CHECK(c, t->hv.caret.pane == 0 && t->hv.caret.pos == 1, "Tab back keeps the byte");
  st_keys(c, "{END}");
  st_checkf(c, t->hv.caret.pos == 3, "End on the last row goes to the end (got %u)", (u32)t->hv.caret.pos);
  st_keys(c, "{HOME}+{RIGHT}+{RIGHT}");
  st_checkf(c, sel_lo(t->sel) == 0 && sel_hi(t->sel) == 2, "Shift+Right selects bytes (got %u..%u)", (u32)t->sel.anchor,
            (u32)t->sel.caret);
  hexview_caret_info(g_app.hexview, &ci);
  ST_CHECK(c, ci.sel_len == 2, "caret info: Sel 2 bytes");
  st_keys(c, "f");
  ST_HX(c, t, "F0 33", "typing replaces the selection");
  st_checkf(c, t->hv.caret.pos == 0 && t->hv.caret.nibble == 1, "after one nibble the caret is on the low nibble (%u.%u)",
            (u32)t->hv.caret.pos, (u32)t->hv.caret.nibble);
  st_hx_key(c, VK_LEFT);           // st_keys("{LEFT}") sends Shift+5: VK_LEFT == '%' (see HANDOFF)
  st_checkf(c, t->hv.caret.pos == 0 && t->hv.caret.nibble == 0, "Left moves by nibble (%u.%u)", (u32)t->hv.caret.pos,
            (u32)t->hv.caret.nibble);
  st_keys(c, "{RIGHT}{RIGHT}");
  st_checkf(c, t->hv.caret.pos == 1 && t->hv.caret.nibble == 0, "Right moves by nibble (%u.%u)", (u32)t->hv.caret.pos,
            (u32)t->hv.caret.nibble);
  st_keys(c, "^{END}");
  st_checkf(c, t->hv.caret.pos == 2, "Ctrl+End (got %u)", (u32)t->hv.caret.pos);
  st_keys(c, "7");
  ST_HX(c, t, "F0 33 70", "overwrite at the end appends");
  st_keys(c, "^{HOME}");
  ST_CHECK(c, t->hv.caret.pos == 0, "Ctrl+Home");
  // undo in the hex view, then across views
  hexview_command(g_app.hexview, IDM_EDIT_UNDO);
  ST_HX(c, t, "F0 33", "undo in the hex view");
  hexview_command(g_app.hexview, IDM_EDIT_REDO);
  ST_HX(c, t, "F0 33 70", "redo in the hex view");
  st_cmd(c, IDM_VIEW_HEX);
  ST_CHECK(c, t->view == VIEW_TEXT, "to text");
  while (t->doc->can_undo() && undos < 50) { Selection s; if (t->doc->undo(&s) != E_OK) break; undos++; }
  ST_HX(c, t, "00 11 22 33", "undo in the text view restores the hex edits");
  while (t->doc->can_redo()) { Selection s; if (t->doc->redo(&s) != E_OK) break; }
  ST_HX(c, t, "F0 33 70", "redo all");
  st_checkf(c, undos >= 5 && undos <= 9, "typing groups merged (%u undo steps)", undos);
  st_cmd(c, IDM_VIEW_HEX);
  // read-only
  SetFocus(g_app.hexview);
  t->doc->set_read_only(true);
  hexview_set_selection(g_app.hexview, 0, 0, SCROLL_NONE);
  st_keys(c, "1{DEL}");
  ST_HX(c, t, "F0 33 70", "read-only refuses edits");
  t->doc->set_read_only(false);
  // select all + Del
  hexview_command(g_app.hexview, IDM_EDIT_SELECTALL);
  hexview_caret_info(g_app.hexview, &ci);
  ST_CHECK(c, ci.sel_len == 3, "select all");
  hexview_command(g_app.hexview, IDM_EDIT_DELETE);
  ST_HX(c, t, "", "select all + Delete empties the document");
  st_keys(c, "4");
  ST_HX(c, t, "40", "typing into an empty document");
  st_hx_close(c, t);
}

static void st_hx_utf16(StCtx* c)
{
  static const u8 k_init[] = { 0xFF, 0xFE, 0x41, 0x00, 0x42, 0x00 };
  static const u8 k_odd[] = { 0xFF, 0xFE, 0x41, 0x00, 0x42 };
  Tab* t = st_hx_tab(c, k_init, sizeof(k_init), enc_utf16le(1), 2, 1);
  if (!t) return;
  SetFocus(g_app.hexview);
  hexview_set_selection(g_app.hexview, 2, 2, SCROLL_NONE);
  st_keys(c, "7");
  ST_HX(c, t, "FF FE 71 00 42 00", "UTF-16 nibble rewrites its unit");
  st_keys(c, "{INS}");
  hexview_set_selection(g_app.hexview, 4, 4, SCROLL_NONE);
  st_keys(c, "12");
  ST_HX(c, t, "FF FE 71 00 12 00 42 00", "UTF-16 insert adds a whole unit");
  st_checkf(c, t->hv.caret.pos == 5, "caret inside the new unit (%u)", (u32)t->hv.caret.pos);
  st_keys(c, "34");
  ST_HX(c, t, "FF FE 71 00 12 34 42 00", "the next nibbles fill the unit");
  st_keys(c, "{INS}");
  hexview_set_selection(g_app.hexview, 3, 3, SCROLL_NONE);
  st_keys(c, "{DEL}");
  ST_HX(c, t, "FF FE 12 34 42 00", "UTF-16 Del removes the unit");
  // BOM protection
  hexview_set_selection(g_app.hexview, 0, 0, SCROLL_NONE);
  st_keys(c, "1{DEL}");
  ST_HX(c, t, "FF FE 12 34 42 00", "BOM bytes are protected");
  st_hx_close(c, t);
  t = st_hx_tab(c, k_odd, sizeof(k_odd), enc_utf16le(1), 2, 1);
  if (!t) return;
  SetFocus(g_app.hexview);
  hexview_set_selection(g_app.hexview, 5, 5, SCROLL_NONE);
  st_keys(c, "9");
  ST_HX(c, t, "FF FE 41 00 42", "nothing can follow a dangling odd byte");
  hexview_set_selection(g_app.hexview, 4, 4, SCROLL_NONE);
  st_keys(c, "{DEL}");
  ST_HX(c, t, "FF FE 41 00", "the dangling byte can be deleted");
  st_hx_close(c, t);
}

static void st_hx_inspector(StCtx* c)
{
  static const u8 k_init[] = { 0xFF, 0xFE, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x3F };
  Tab* t = st_hx_tab(c, k_init, sizeof(k_init), enc_ansi(), 0, 1);
  HexView* v = hv_get(g_app.hexview);
  char buf[200];
  u8 old_on = g_app.cfg.inspector_on, old_be = g_app.cfg.inspector_be;
  if (!t || !v) return;
  st_cmd(c, IDM_VIEW_INSPECTOR);
  st_checkf(c, v->insp_on == (g_app.cfg.inspector_on ? 1 : 0) && g_app.cfg.inspector_on != old_on,
            "View > Data Inspector toggles the strip");
  if (!v->insp_on) st_cmd(c, IDM_VIEW_INSPECTOR);
  g_app.cfg.inspector_be = 0;
  hexview_set_selection(g_app.hexview, 0, 0, SCROLL_NONE);
  hv_insp_text(v, 0, buf, sizeof(buf));
  st_checkf(c, st_hx_contains(buf, "int8 -1 ") && st_hx_contains(buf, "uint16 65279 ") &&
            st_hx_contains(buf, "int16 -257 "), "inspector LE row 1: %s", buf);
  hv_insp_text(v, 2, buf, sizeof(buf));
  st_checkf(c, st_hx_contains(buf, "bin 11111111") && st_hx_contains(buf, "[LE]"), "inspector row 3: %s", buf);
  // the [LE] toggle is clickable
  {
    RECT r;
    InvalidateRect(g_app.hexview, 0, FALSE);
    UpdateWindow(g_app.hexview);
    r = v->insp_toggle;
    SendMessageA(g_app.hexview, WM_LBUTTONDOWN, 0, MAKELPARAM(r.left + 2, r.top + 2));
    SendMessageA(g_app.hexview, WM_LBUTTONUP, 0, MAKELPARAM(r.left + 2, r.top + 2));
    ST_CHECK(c, g_app.cfg.inspector_be == 1, "clicking [LE] switches to big endian");
  }
  hv_insp_text(v, 0, buf, sizeof(buf));
  st_checkf(c, st_hx_contains(buf, "uint16 65534 "), "inspector BE: %s", buf);
  g_app.cfg.inspector_be = 0;
  hexview_set_selection(g_app.hexview, 6, 6, SCROLL_NONE);
  hv_insp_text(v, 2, buf, sizeof(buf));
  st_checkf(c, st_hx_contains(buf, "float 1 "), "float at offset 6: %s", buf);
  hv_insp_text(v, 0, buf, sizeof(buf));
  st_checkf(c, st_hx_contains(buf, "int32 1065353216 "), "int32 at offset 6: %s", buf);
  hexview_set_selection(g_app.hexview, 9, 9, SCROLL_NONE);
  hv_insp_text(v, 0, buf, sizeof(buf));
  st_checkf(c, st_hx_contains(buf, "uint8 63 ") && st_hx_contains(buf, "int16 - "), "short tail: %s", buf);
  g_app.cfg.inspector_be = old_be;
  if ((g_app.cfg.inspector_on ? 1 : 0) != (old_on ? 1 : 0)) st_cmd(c, IDM_VIEW_INSPECTOR);
  st_hx_close(c, t);
}

static void st_hx_modified_and_mouse(StCtx* c)
{
  u8 init[64];
  u8 b[HEX_BPR], fl[HEX_BPR];
  Tab* t;
  HexView* v = hv_get(g_app.hexview);
  u32 i;
  for (i = 0; i < sizeof(init); i++) init[i] = (u8)i;
  t = st_hx_tab(c, init, sizeof(init), enc_ansi(), 0, 1);
  if (!t || !v) return;
  SetFocus(g_app.hexview);
  hexview_set_selection(g_app.hexview, 0, 0, SCROLL_NONE);
  st_keys(c, "ee");
  hv_row_bytes(v, 0, b, fl);
  ST_CHECK(c, (fl[0] & BF_MOD) && !(fl[1] & BF_MOD), "typed byte is modified (red), its neighbour is not");
  t->doc->set_saved();
  hx_track(t);
  hv_row_bytes(v, 0, b, fl);
  ST_CHECK(c, !(fl[0] & BF_MOD), "after a save the byte is no longer red");
  st_keys(c, "dd");
  hv_row_bytes(v, 0, b, fl);
  ST_CHECK(c, (fl[1] & BF_MOD) && !(fl[0] & BF_MOD), "only bytes edited after the save are red");
  ST_CHECK(c, (fl[0] & BF_VALID) && b[0] == 0xEE && b[1] == 0xDD, "row bytes read back");
  // mouse: click byte 0x15 low nibble, char pane, drag
  st_hx_click(v, hv_byte_cell(5) + 1, 1, WM_LBUTTONDOWN, MK_LBUTTON);
  st_hx_click(v, hv_byte_cell(5) + 1, 1, WM_LBUTTONUP, 0);
  st_checkf(c, t->hv.caret.pos == 0x15 && t->hv.caret.nibble == 1 && t->hv.caret.pane == 0,
            "click on a low nibble (%u.%u pane %u)", (u32)t->hv.caret.pos, (u32)t->hv.caret.nibble, (u32)t->hv.caret.pane);
  st_hx_click(v, HX_CHR0 + 3, 2, WM_LBUTTONDOWN, MK_LBUTTON);
  st_hx_click(v, HX_CHR0 + 3, 2, WM_LBUTTONUP, 0);
  st_checkf(c, t->hv.caret.pos == 0x23 && t->hv.caret.pane == 1, "click in the char pane (%u pane %u)",
            (u32)t->hv.caret.pos, (u32)t->hv.caret.pane);
  st_hx_click(v, hv_byte_cell(1), 0, WM_LBUTTONDOWN, MK_LBUTTON);
  st_hx_click(v, hv_byte_cell(4), 0, WM_MOUSEMOVE, MK_LBUTTON);
  st_hx_click(v, hv_byte_cell(4), 0, WM_LBUTTONUP, 0);
  st_checkf(c, sel_lo(t->sel) == 1 && sel_hi(t->sel) == 5, "drag selects bytes 1..4 (%u..%u)", (u32)t->sel.anchor,
            (u32)t->sel.caret);
  st_hx_click(v, hv_byte_cell(9), 3, WM_LBUTTONDBLCLK, MK_LBUTTON);
  st_hx_click(v, hv_byte_cell(9), 3, WM_LBUTTONUP, 0);
  st_checkf(c, sel_lo(t->sel) == 0x39 && sel_hi(t->sel) == 0x3A, "double-click selects the byte (%u..%u)",
            (u32)t->sel.anchor, (u32)t->sel.caret);
  st_hx_close(c, t);
}

static void st_hx_bigdoc(StCtx* c)
{
  u32 n = 1024u * 1024u, i, reps = 20;
  u8* buf = (u8*)e_alloc(n);
  Tab* t;
  HexView* v = hv_get(g_app.hexview);
  SCROLLINFO si;
  LARGE_INTEGER f, a, b;
  if (!buf || !v) { e_free(buf); st_checkf(c, 0, "bigdoc: out of memory"); return; }
  for (i = 0; i < n; i++) buf[i] = (u8)(i * 7 + (i >> 9));
  t = st_hx_tab(c, buf, n, enc_ansi(), 0, 1);
  e_free(buf);
  if (!t) return;
  hexview_set_selection(g_app.hexview, n - 1, n - 1, SCROLL_CENTER);
  memset(&si, 0, sizeof(si));
  si.cbSize = sizeof(si); si.fMask = SIF_ALL;
  GetScrollInfo(g_app.hexview, SB_VERT, &si);
  st_checkf(c, si.nMax <= HX_SCROLL_MAX && si.nPos + (int)si.nPage >= si.nMax - 1, "scroll range scaled (max %d pos %d page %u)",
            si.nMax, si.nPos, si.nPage);
  st_checkf(c, t->hv.top_row + (u32)v->rows_vis >= n / HEX_BPR, "caret row visible (top %u)", t->hv.top_row);
  SendMessageA(g_app.hexview, WM_VSCROLL, SB_TOP, 0);
  ST_CHECK(c, t->hv.top_row == 0, "SB_TOP");
  SendMessageA(g_app.hexview, WM_VSCROLL, SB_BOTTOM, 0);
  ST_CHECK(c, t->hv.top_row == hv_max_top(v), "SB_BOTTOM");
  hexview_set_selection(g_app.hexview, n / 2, n / 2 + 100, SCROLL_CENTER);
  // full repaint timing (window must be visible; no VNC)
  if (QueryPerformanceFrequency(&f) && IsWindowVisible(g_app.hexview)) {
    UpdateWindow(g_app.hexview);
    QueryPerformanceCounter(&a);
    for (i = 0; i < reps; i++) { InvalidateRect(g_app.hexview, 0, FALSE); UpdateWindow(g_app.hexview); }
    QueryPerformanceCounter(&b);
    {
      u32 us = (u32)(((b.QuadPart - a.QuadPart) * 1000000) / f.QuadPart / reps);
      selftest_log("hex view full repaint: %u.%03u ms (%d rows, %u reps, 1 MB document)", us / 1000, us % 1000,
                   v->rows_vis, reps);
      st_checkf(c, us < 100000, "full repaint %u us", us);
    }
    QueryPerformanceCounter(&a);
    for (i = 0; i < reps; i++) SendMessageA(g_app.hexview, WM_VSCROLL, SB_LINEDOWN, 0);
    QueryPerformanceCounter(&b);
    selftest_log("hex view line scroll: %u us", (u32)(((b.QuadPart - a.QuadPart) * 1000000) / f.QuadPart / reps));
  } else selftest_log("hex view repaint timing skipped (window not visible)");
  st_hx_close(c, t);
}

static void st_hx_clipboard(StCtx* c)
{
  static const u8 k_init[] = { 0x10, 0x20, 0x30, 0x00 };
  ClipData cd;
  Tab* t;
  HGLOBAL h;
  int rc;
  t = st_hx_tab(c, k_init, sizeof(k_init), enc_ansi(), 0, 1);
  if (!t) return;
  // hex text on the clipboard pastes as bytes into the hex pane
  if (OpenClipboard(g_app.frame)) {
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 8);
    if (h) { char* p = (char*)GlobalLock(h); memcpy(p, "DE AD", 6); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
  }
  cd.has_raw = 0;
  rc = clip_get(&cd);
  clip_free(&cd);
  if (rc == E_NOTIMPL) { selftest_log("clipboard checks skipped (clip.cpp not implemented yet)"); st_hx_close(c, t); return; }
  hexview_set_selection(g_app.hexview, 1, 1, SCROLL_NONE);
  t->hv.caret.pane = 0;
  hexview_command(g_app.hexview, IDM_EDIT_PASTE);
  ST_HX(c, t, "10 DE AD 00", "hex text pastes as bytes (overwrite)");
  t->hv.caret.pane = 1;
  hexview_set_selection(g_app.hexview, 4, 4, SCROLL_NONE);
  hexview_command(g_app.hexview, IDM_EDIT_PASTE);
  ST_HX(c, t, "10 DE AD 00 44 45 20 41 44", "text pastes as encoded text in the char pane");
  // raw bytes with NULs survive copy/paste
  t->hv.caret.pane = 0;
  hexview_set_selection(g_app.hexview, 2, 4, SCROLL_NONE);
  rc = hexview_command(g_app.hexview, IDM_EDIT_COPY);
  t->hv.caret.insert = 1;
  hexview_set_selection(g_app.hexview, 0, 0, SCROLL_NONE);
  hexview_command(g_app.hexview, IDM_EDIT_PASTE);
  t->hv.caret.insert = 0;
  ST_HX(c, t, "AD 00 10 DE AD 00 44 45 20 41 44", "raw bytes (with NUL) paste exactly in insert mode");
  st_hx_close(c, t);
}

// Screenshot helper: if <cfg>\hexshow.txt exists ("<mode> <seconds>"), leave a demo state on screen.
static void st_hx_show(StCtx* c)
{
  char path[300], txt[32];
  FILE* f;
  int mode = 1, secs = 20;
  u8 buf[1024];
  u32 i;
  Tab* t;
  ui_path_join(c->cfg_dir, "hexshow.txt", path, sizeof(path));
  f = fopen(path, "r");
  if (!f) return;
  memset(txt, 0, sizeof(txt));
  fgets(txt, sizeof(txt) - 1, f);
  fclose(f);
  mode = txt[0] - '0';
  secs = atoi(txt + 1);
  if (secs <= 0 || secs > 120) secs = 20;
  if (mode == 3) {
    static const char* k_nfo[] = {
      "\xC9\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBB\r\n",
      "\xBA  PICA  NFO \xB0\xB1\xB2 \xBA\r\n",
      "\xC8\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBC\r\n",
      " \xDB\xDB\xDF\xDF\xDB\xDB \xDC\xDC \xFE \xAE\xAF \x01\x02\x03\r\n" };
    u32 n = 0, k;
    for (k = 0; k < 4; k++) { u32 l = es_len(k_nfo[k]); memcpy(buf + n, k_nfo[k], l); n += l; }
    t = st_hx_tab(c, buf, n, enc_oem(), 0, 1);
  } else {
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xFF\xFF\x00\x00\xB8", 17);
    buf[0x18] = 0x40;
    memcpy(buf + 0x40, "\x0E\x1F\xBA\x0E\x00\xB4\x09\xCD\x21\xB8\x01\x4C\xCD\x21This program cannot be run in DOS mode.\r\r\n$", 0x40);
    for (i = 0x80; i < sizeof(buf); i++) buf[i] = (u8)((i * 37) ^ (i >> 3));
    memcpy(buf + 0x100, "PE\0\0\x4C\x01\x04\x00", 8);
    t = st_hx_tab(c, buf, sizeof(buf), enc_ansi(), 0, 1);
  }
  if (!t) return;
  if (!g_app.cfg.inspector_on) st_cmd(c, IDM_VIEW_INSPECTOR);
  SetFocus(g_app.hexview);
  if (mode != 3) {
    hexview_set_selection(g_app.hexview, 0x3C, 0x3C, SCROLL_NONE);
    st_keys(c, "80");
    hexview_set_selection(g_app.hexview, 0x22, 0x22, SCROLL_NONE);
    st_keys(c, "c0ffee");
    hexview_set_selection(g_app.hexview, 0x40, 0x4E, SCROLL_NONE);
    if (mode == 2) {
      hexview_set_selection(g_app.hexview, 0x58, 0x58, SCROLL_NONE);
      st_keys(c, "{INS}");
      st_keys(c, "a5");
    }
  } else hexview_set_selection(g_app.hexview, 0x13, 0x13, SCROLL_NONE);
  app_update_status();
  selftest_log("hexshow mode %d: showing for %d s", mode, secs);
  st_pump(c, (u32)secs * 1000);
  t->doc->set_saved();
  st_hx_close(c, t);
}

void st_hexview(StCtx* c)
{
  HexView* v = hv_get(g_app.hexview);
  u8 old_overwrite = g_app.cfg.hex_overwrite;
  if (!v) { st_checkf(c, 0, "no hex view window"); return; }
  if (!IsWindowVisible(g_app.frame)) { ShowWindow(g_app.frame, SW_SHOWNORMAL); ShowWindow(g_app.frame, SW_SHOWNORMAL); }
  g_app.cfg.hex_overwrite = 1;
  if (!app_active()) ST_CHECK(c, cmdstate_view_hex() == MF_GRAYED, "View > Hex View greyed without a tab");
  st_hx_mapping(c);
  st_hx_typing(c);
  st_hx_utf16(c);
  st_hx_inspector(c);
  st_hx_modified_and_mouse(c);
  st_hx_bigdoc(c);
  st_hx_clipboard(c);
  st_hx_show(c);
  g_app.cfg.hex_overwrite = old_overwrite;
}
