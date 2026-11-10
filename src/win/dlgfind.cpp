// dlgfind.cpp - Find / Replace / Find in Files dialog and the search commands (DESIGN 11.1-11.4, 11.6). Owner: T11.
// The search state (pattern, replacement, options) lives in g_fs so F3 and Ctrl+F3 work without the dialog. Every
// operation works on a Tab (op_*), shows its result in the dialog's status line (or as a status bar note while the
// dialog is hidden) and never opens a message box for "not found".
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string.h>
#include <stdarg.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "progress.h"
#include "render.h"
#include "selftest.h"
#include "fifworker.h"
#include "fifworker_int.h"
#include "estr.h"
#include "edops.h"
#include "hexfmt.h"
#include "resource.h"
#include "ids_find.h"

enum { PAGE_FIND = 0, PAGE_REPLACE = 1, PAGE_FIF = 2 };
enum { TIMER_VALIDATE = 1, PAT_MAX = RX_MAX_PATTERN, MARK_MAX = 10000 };

struct FindState {
  Vec<ecp> pat, rep;
  SearchOpts opts;
  u8 wrap, up, insel, inited;
  u32 scope_tab, scope_seq; epos scope_lo, scope_hi; u8 scope_on;   // In selection scope for Find Next / Replace
  u32 last_tab, last_seq; epos last_pos, last_len; u8 last_valid;   // the match we selected last
};
static FindState g_fs;
static HWND g_dlg = 0;
static int g_page = PAGE_FIND;
static int g_status_err = 0;
static int g_automove = 0;
static char g_status[400];
// The pattern field may show '?' for characters the ANSI code page can't hold (prefill, Ctrl+F3): while the field
// still holds exactly the text we put there, the real characters are used.
static char g_field_text[PAT_MAX + 1];
static Vec<ecp> g_field_pat;

static void read_controls();
static void update_enables();

// ---------------------------------------------------------------------------------------------------------------
// helpers

static void fs_init()
{
  if (g_fs.inited) return;
  g_fs.inited = 1;
  g_fs.opts.mode = (u8)(g_app.cfg.find_mode <= SM_HEX ? g_app.cfg.find_mode : SM_NORMAL);
  g_fs.opts.match_case = g_app.cfg.find_case;
  g_fs.opts.whole_word = g_app.cfg.find_word;
  g_fs.opts.dot_all = g_app.cfg.find_dotall;
  g_fs.wrap = g_app.cfg.find_wrap;
  g_fs.up = g_app.cfg.find_dir_up;
  g_fs.insel = 0;
}

static HWND view_of(Tab* t)
{
  if (!t) return 0;
  return t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
}

static void invalidate_view(Tab* t)
{
  if (t != app_active()) return;
  if (t->view == VIEW_HEX) { if (g_app.hexview) hexview_invalidate(g_app.hexview); }
  else if (g_app.textview) textview_invalidate(g_app.textview);
}

void search_select(Tab* t, epos anchor, epos caret, int scroll_mode)
{
  if (!t) return;
  // The view first: it compares Tab::sel with the new selection to repaint the old range.
  if (t == app_active()) {
    if (t->view == VIEW_HEX) { if (g_app.hexview) hexview_set_selection(g_app.hexview, anchor, caret, scroll_mode); }
    else if (g_app.textview) textview_set_selection(g_app.textview, anchor, caret, scroll_mode);
  }
  t->sel.anchor = anchor;
  t->sel.caret = caret;
}

void search_ecp_to_utf8(const ecp* s, u32 n, char* out, u32 cap)
{
  u32 i, k = 0;
  if (!out || cap == 0) return;
  for (i = 0; s && i < n; i++) {
    char b[4];
    ecp c = s[i];
    u32 len;
    if (ECP_IS_BAD(c) || (c >= 0xD800u && c <= 0xDFFFu) || c == 0x0D || c == 0x0A || c == 0) c = ECP_REPLACEMENT;
    len = es_utf8_encode(c, b);
    if (k + len + 1 > cap) break;
    memcpy(out + k, b, len);
    k += len;
  }
  out[k] = 0;
}

void search_header(const ecp* pat, u32 n, char* out, u32 cap)
{
  char p[420];
  u32 m = n > 100 ? 100 : n;
  search_ecp_to_utf8(pat, m, p, sizeof(p));
  es_fmt(out, cap, "Search \"%s%s\"", p, n > m ? "..." : "");
}

static void status_text(int err, const char* text)
{
  es_copy(g_status, sizeof(g_status), text);
  g_status_err = err;
  selftest_log("find: %s%s", err ? "[error] " : "", text);
  if (g_dlg) {                                     // also while hidden: a reopened dialog must not show old text
    SetDlgItemTextA(g_dlg, IDC_FIND_STATUS, text);
    InvalidateRect(GetDlgItem(g_dlg, IDC_FIND_STATUS), 0, TRUE);
  }
  if ((!g_dlg || !IsWindowVisible(g_dlg)) && text[0]) app_status_note("%s", text);
}

static void status(int err, const char* fmt, ...)
{
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  status_text(err, buf);
}

static const char* edit_error(int rc)
{
  switch (rc) {
  case E_READONLY: return "The document is read-only";
  case E_BUSY: return "The document is busy (saving or indexing)";
  case E_NOMEM: return "Out of memory";
  case E_TOOBIG: return "The result would be too big";
  case E_ALIGN: return "The edit doesn't fit the UTF-16 byte alignment";
  default: return e_errname(rc);
  }
}

static const char* search_error(int rc)
{
  switch (rc) {
  case E_BUDGET: return "Pattern too complex (backtracking limit)";
  case E_CANCEL: return "Cancelled";
  case E_NOMEM: return "Out of memory";
  case E_IO: return "Could not read the document";
  default: return e_errname(rc);
  }
}

static int tab_alive(Tab* t)
{
  return t && app_tab_index(t) >= 0;
}

static Tab* tab_by_id(u32 id)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) if (app_tab(i)->id == id) return app_tab(i);
  return 0;
}

static int cr_mode_of(Document* d)
{
  const BreakSpec& bs = d->tree()->break_spec();
  return bs.b0 == 0x0D || bs.b1 == 0x0D;
}

static epos next_char(Tab* t, epos p)
{
  if (g_fs.opts.mode == SM_HEX || t->view == VIEW_HEX) return p + 1;
  if (p >= t->doc->length()) return p + 1;
  return t->doc->char_next(p);
}

static int last_is(Tab* t, epos pos, epos len)
{
  return g_fs.last_valid && g_fs.last_tab == t->id && g_fs.last_seq == t->doc->change_seq() && g_fs.last_pos == pos &&
         g_fs.last_len == len;
}

static void set_last(Tab* t, epos pos, epos len)
{
  g_fs.last_valid = 1; g_fs.last_tab = t->id; g_fs.last_seq = t->doc->change_seq();
  g_fs.last_pos = pos; g_fs.last_len = len;
}

static void base_range(Tab* t, epos* lo, epos* hi)
{
  *lo = g_fs.opts.mode == SM_HEX ? 0 : t->doc->text_origin();
  *hi = t->doc->length();
}

// Range for Count / Mark All / Find All / Replace All: the selection when "In selection" is on and it isn't empty.
static int op_range(Tab* t, epos* lo, epos* hi)
{
  base_range(t, lo, hi);
  if (g_fs.insel && !sel_empty(t->sel)) {
    epos a = sel_lo(t->sel), b = sel_hi(t->sel);
    if (a > *lo) *lo = a;
    if (b < *hi) *hi = b;
    return 1;
  }
  return 0;
}

// In selection for Find Next / Replace: the scope is the selection the search started from; it survives while the
// selection is the match we selected and the document didn't change behind our back. Returns 1 for a new scope.
static int scope_update(Tab* t)
{
  Selection s = t->sel;
  if (g_fs.scope_on && g_fs.scope_tab == t->id && g_fs.scope_seq == t->doc->change_seq() &&
      (sel_empty(s) ? last_is(t, s.caret, 0) : last_is(t, sel_lo(s), sel_hi(s) - sel_lo(s))))
    return 0;
  if (!sel_empty(s)) {
    g_fs.scope_on = 1; g_fs.scope_tab = t->id; g_fs.scope_seq = t->doc->change_seq();
    g_fs.scope_lo = sel_lo(s); g_fs.scope_hi = sel_hi(s);
    return 1;
  }
  g_fs.scope_on = 0;
  return 0;
}

static void format_compile_error(const SearchError& e, char* out, u32 cap)
{
  if (e.code == E_NOMEM) { es_copy(out, cap, "Out of memory"); return; }
  if (g_fs.opts.mode == SM_HEX) { es_fmt(out, cap, "Invalid hex pattern: %s", e.msg); return; }
  if (g_fs.opts.mode == SM_NORMAL || e.code == E_TOOBIG) { es_copy(out, cap, e.msg); return; }
  es_fmt(out, cap, "%s error at character %u: %s", g_fs.opts.mode == SM_REGEX ? "Regex" : "Pattern", e.pos + 1, e.msg);
}

// Compiles g_fs for the tab's encoding; shows the error (red) and beeps when it fails.
static int prepare(Tab* t, Searcher* s)
{
  SearchError err;
  char msg[200];
  int rc;
  fs_init();
  if (!t || !t->doc) { status(0, "No document is open"); return E_STATE; }
  if (g_fs.pat.empty()) { status(0, "Type something to find"); MessageBeep(MB_OK); return E_SYNTAX; }
  err.code = 0; err.pos = 0; err.msg[0] = 0;
  rc = s->compile(g_fs.pat.data(), g_fs.pat.size(), g_fs.opts, t->doc->enc(), &err);
  if (rc != E_OK) {
    if (!err.msg[0]) es_copy(err.msg, sizeof(err.msg), e_errname(rc));
    format_compile_error(err, msg, sizeof(msg));
    status(1, "%s", msg);
    MessageBeep(MB_OK);
  }
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// moving the dialog away from the match (DESIGN 11.4)

static void avoid_match(Tab* t, epos pos)
{
  RECT dr, vr, wa;
  HWND v = view_of(t);
  const RenderFonts* f = render_fonts();
  int lh = f && f->ch > 0 ? f->ch : 16, my, h, ny;
  long row;
  RECT pr;
  if (!g_dlg || !IsWindowVisible(g_dlg) || !v || !IsWindowVisible(v)) return;
  GetWindowRect(g_dlg, &dr);
  GetWindowRect(v, &vr);
  if (t->view != VIEW_HEX && textview_pos_rect(v, pos, &pr)) {
    my = pr.top;                                   // exact (word wrap and scrolling included)
    lh = pr.bottom - pr.top;
    if (pr.right < dr.left || pr.left > dr.right) return;   // the match is beside the dialog
  } else {
    if (t->view == VIEW_HEX) row = (long)(pos / 16) - (long)t->hv.top_row + 1;
    else row = (long)t->doc->line_of(pos) - (long)t->tv.top_line;
    my = vr.top + (int)row * lh;
    if (row < 0 || my + lh > vr.bottom) my = (vr.top + vr.bottom) / 2;   // not on screen yet: assume it is centred
    if (vr.right < dr.left || vr.left > dr.right) return;
  }
  if (my + lh < dr.top || my > dr.bottom) return;
  h = dr.bottom - dr.top;
  SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
  ny = my - h - 8;
  if (ny < wa.top) {
    ny = my + lh + 8;
    if (ny + h > wa.bottom) return;
  }
  g_automove = 1;
  SetWindowPos(g_dlg, 0, dr.left, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  g_automove = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// operations

static int op_find(Tab* t, int backward)
{
  Searcher s;
  RxMatch m;
  Document* d;
  epos lo, hi, from, slo, shi;
  Selection sel;
  const char* wrapped = 0;
  int r, scoped = 0, fresh = 0;
  if (prepare(t, &s) != E_OK) return E_SYNTAX;
  d = t->doc;
  base_range(t, &lo, &hi);
  if (g_fs.insel) {
    fresh = scope_update(t);
    if (g_fs.scope_on) {
      lo = emax(lo, g_fs.scope_lo);
      hi = emin(hi, g_fs.scope_hi);
      scoped = 1;
    }
  }
  sel = t->sel;
  slo = sel_lo(sel); shi = sel_hi(sel);
  if (fresh) { slo = shi = backward ? hi : lo; sel.anchor = sel.caret = slo; }   // a new scope starts at its end
  {
    UiProgress prog("Find", "Searching...", 1);
    if (!backward) {
      from = shi;
      if (sel_empty(sel) && last_is(t, slo, 0)) from = next_char(t, slo);   // after an empty match
      if (from < lo) from = lo;
      r = from <= hi ? s.find(d->bytes(), d->codec(), lo, hi, from, 0, &m, &prog) : 0;
      if (r == 0 && g_fs.wrap && from > lo) {
        r = s.find(d->bytes(), d->codec(), lo, hi, lo, 0, &m, &prog);
        if (r == 1) wrapped = scoped ? "Wrapped to the start of the selection" : "Wrapped to the top";
      }
    } else {
      from = slo;
      if (from > hi) from = hi;
      if (from < lo) from = lo;
      r = s.find(d->bytes(), d->codec(), lo, hi, from, 1, &m, &prog);
      if (r == 0 && g_fs.wrap && from < hi) {
        r = s.find(d->bytes(), d->codec(), lo, hi, hi, 1, &m, &prog);
        if (r == 1) wrapped = scoped ? "Wrapped to the end of the selection" : "Wrapped to the bottom";
      }
    }
  }
  if (r < 0) { status(r != E_CANCEL, "%s", search_error(r)); return r; }
  if (r == 0) {
    status(0, scoped ? "Not found in the selection" : "Not found");
    MessageBeep(MB_OK);
    return 0;
  }
  set_last(t, m.pos, m.len);
  if (scoped) g_fs.scope_seq = d->change_seq();
  search_select(t, m.pos, m.pos + m.len, SCROLL_MIN);
  avoid_match(t, m.pos);
  status(0, "%s", wrapped ? wrapped : "");
  return 1;
}

static int op_count(Tab* t)
{
  Searcher s;
  epos lo, hi;
  u32 n = 0;
  int r, insel;
  if (prepare(t, &s) != E_OK) return E_SYNTAX;
  insel = op_range(t, &lo, &hi);
  {
    UiProgress prog("Count", "Counting...", 1);
    r = s.count(t->doc->bytes(), t->doc->codec(), lo, hi, &n, &prog);
  }
  if (r < 0) { status(r != E_CANCEL, "%s", search_error(r)); return r; }
  status(0, "%u match%s%s", n, n == 1 ? "" : "es", insel ? " in the selection" : "");
  return (int)n;
}

static int op_mark(Tab* t)
{
  Searcher s;
  Vec<MarkRange> v;
  epos lo, hi;
  u32 i, k = 0;
  int r, insel;
  if (prepare(t, &s) != E_OK) return E_SYNTAX;
  insel = op_range(t, &lo, &hi);
  {
    UiProgress prog("Mark All", "Searching...", 1);
    r = s.find_all(t->doc->bytes(), t->doc->codec(), lo, hi, &v, 1000000, &prog);
  }
  if (r < 0) { status(r != E_CANCEL, "%s", search_error(r)); return r; }
  for (i = 0; i < v.size(); i++) if (v[i].len) v[k++] = v[i];   // empty matches can't be shown
  v.resize(k);
  t->marks.set(v.data(), k);
  invalidate_view(t);
  if (k > MARK_MAX) status(0, "%u of %u%s matches marked (the limit is %u)", (u32)MARK_MAX, k, r == 1 ? "+" : "",
                           (u32)MARK_MAX);
  else status(0, "%u marked%s", k, insel ? " in the selection" : "");
  return (int)k;
}

class FindAllSink : public FifSink {
public:
  Tab* t; u32 n;
  FindAllSink(Tab* tab) : t(tab), n(0) {}
  int on_file(const char* path, u32 index) { (void)index; return results_add_file(path, t->id, t->doc->change_seq()) < 0; }
  int on_hit(const FifHit& h)
  {
    if (results_add_hit(h) < 0) return 1;
    if (++n % 500 == 0) results_flush();
    return 0;
  }
  int on_progress(u32 done, u32 seen, const char* current) { (void)done; (void)seen; (void)current; return 0; }
  void on_done(int result, u32 searched, u32 fwh, u32 hits) { (void)result; (void)searched; (void)fwh; (void)hits; }
};

static int op_find_all(Tab* t)
{
  Searcher s;
  epos lo, hi;
  u32 hits = 0;
  int r, insel;
  char head[600], name[280], tail[120];
  if (prepare(t, &s) != E_OK) return E_SYNTAX;
  if (s.opts().mode == SM_HEX) { status(0, "Find All shows lines of text; use Mark All in the hex view"); return E_STATE; }
  insel = op_range(t, &lo, &hi);
  fif_abandon();
  tab_title(t, name, sizeof(name));
  search_header(g_fs.pat.data(), g_fs.pat.size(), head, sizeof(head));
  es_cat(head, sizeof(head), " (searching...)");
  results_begin(head);
  {
    FindAllSink sink(t);
    const char* display = (!t->untitled && t->path[0]) ? t->path : name;
    r = fif_search_source(t->doc->bytes(), t->doc->codec(), lo, hi, cr_mode_of(t->doc), s, 0, display, &sink,
                          50000, &hits, 0);
  }
  search_header(g_fs.pat.data(), g_fs.pat.size(), head, sizeof(head));
  es_fmt(tail, sizeof(tail), " in %s%s (%u hit%s%s)", name, insel ? " (selection)" : "", hits, hits == 1 ? "" : "s",
         r == E_TOOBIG ? ", stopped at the limit" : r < 0 ? ", search failed" : "");
  es_cat(head, sizeof(head), tail);
  results_finish(head);
  if (r < 0 && r != E_TOOBIG) { status(1, "%s", search_error(r)); return r; }
  if (hits == 0) { status(0, "Not found"); MessageBeep(MB_OK); }
  else status(0, "%u hit%s listed in Find Results", hits, hits == 1 ? "" : "s");
  return (int)hits;
}

class DocEditSink : public EditSink {
public:
  Document* d;
  DocEditSink(Document* doc) : d(doc) {}
  int apply(epos pos, epos del, const u8* ins, epos n) { return d->replace(pos, del, ins, n); }
};

// Replace All in one tab: one undo group; a failing edit undoes the group. Returns E_* and fills *nrep / *unrep.
static int replace_all_in(Tab* t, int use_sel, u32* nrep, u32* unrep, int* insel_out)
{
  Searcher s;
  Document* d = t->doc;
  epos lo, hi, old_len, new_len;
  Selection before = t->sel, after;
  int rc, insel = 0;
  SearchError err;
  *nrep = 0; *unrep = 0;
  if (!d->editable()) return d->read_only() ? E_READONLY : E_BUSY;
  rc = s.compile(g_fs.pat.data(), g_fs.pat.size(), g_fs.opts, d->enc(), &err);
  if (rc != E_OK) return rc;
  if (use_sel) insel = op_range(t, &lo, &hi);
  else base_range(t, &lo, &hi);
  if (insel_out) *insel_out = insel;
  old_len = d->length();
  {
    DocEditSink sink(d);
    UiProgress prog("Replace All", "Replacing...", 1);
    d->begin_edit(UK_OTHER, before, app_now_ms());
    rc = search_replace_all(s, d->bytes(), d->codec(), lo, hi, g_fs.rep.data(), g_fs.rep.size(), &sink, &prog, nrep,
                            unrep);
    new_len = d->length();
    if (insel) { after.anchor = lo; after.caret = hi + new_len - old_len; }
    else {
      epos p = before.caret;
      if (p > hi) p = p + new_len - old_len;         // after the replaced range: shift
      else if (p > lo) p = lo;                       // inside it: go to its start
      if (p > new_len) p = new_len;
      after.anchor = after.caret = p;
    }
    d->end_edit(after);
  }
  if (rc < 0 && *nrep > 0) {                         // an edit failed half way: undo the group
    Selection u;
    d->undo(&u);
    *nrep = 0;
  }
  if (*nrep > 0) {
    if (t == app_active()) search_select(t, after.anchor, after.caret, SCROLL_NONE);
    else t->sel = after;
    app_on_doc_changed(t);
  }
  return rc;
}

static int op_replace_all(Tab* t)
{
  u32 n = 0, unrep = 0;
  int rc, insel = 0;
  Searcher probe;
  if (prepare(t, &probe) != E_OK) return E_SYNTAX;
  rc = replace_all_in(t, 1, &n, &unrep, &insel);
  if (rc == E_CANCEL) { status(0, "Cancelled; nothing replaced"); return rc; }
  if (rc == E_TOOBIG) { status(1, "Too many matches (over 1,000,000); nothing replaced"); return rc; }
  if (rc < 0) { status(1, "Nothing replaced: %s", rc == E_BUDGET ? search_error(rc) : edit_error(rc)); return rc; }
  if (n == 0) { status(0, insel ? "Not found in the selection" : "Not found"); MessageBeep(MB_OK); return 0; }
  if (unrep) status(0, "%u replaced%s; %u character%s became '?'", n, insel ? " in the selection" : "", unrep,
                    unrep == 1 ? "" : "s");
  else status(0, "%u replaced%s", n, insel ? " in the selection" : "");
  return (int)n;
}

static int op_replace(Tab* t)
{
  Searcher s;
  RxMatch m;
  Document* d;
  epos lo, hi, slo, shi;
  Selection sel;
  int r, replaced = 0;
  u32 unrep = 0;
  if (prepare(t, &s) != E_OK) return E_SYNTAX;
  d = t->doc;
  sel = t->sel;
  slo = sel_lo(sel); shi = sel_hi(sel);
  base_range(t, &lo, &hi);
  if (g_fs.insel) {
    scope_update(t);
    if (g_fs.scope_on) { lo = emax(lo, g_fs.scope_lo); hi = emin(hi, g_fs.scope_hi); }
  }
  if ((!sel_empty(sel) || last_is(t, slo, 0)) && slo >= lo && shi <= hi) {
    r = s.find(d->bytes(), d->codec(), lo, hi, slo, 0, &m, 0);
    if (r < 0) { status(1, "%s", search_error(r)); return r; }
    if (r == 1 && m.pos == slo && m.pos + m.len == shi) {
      EBytes out;
      Selection after;
      int rc;
      if (!d->editable()) { status(1, "%s", edit_error(d->read_only() ? E_READONLY : E_BUSY)); return E_READONLY; }
      rc = s.expand(g_fs.rep.data(), g_fs.rep.size(), d->bytes(), d->codec(), m, &out, &unrep);
      if (rc != E_OK) { status(1, "%s", edit_error(rc)); return rc; }
      after.anchor = after.caret = g_fs.up ? m.pos : m.pos + out.size();
      d->begin_edit(UK_OTHER, sel, app_now_ms());
      rc = d->replace(m.pos, m.len, out.data(), out.size());
      d->end_edit(after);
      if (rc != E_OK) { status(1, "Could not replace: %s", edit_error(rc)); return rc; }
      if (g_fs.scope_on && g_fs.scope_tab == t->id) {
        g_fs.scope_hi = g_fs.scope_hi + out.size() - m.len;
        g_fs.scope_seq = d->change_seq();
      }
      search_select(t, after.anchor, after.caret, SCROLL_NONE);
      if (m.len == 0) set_last(t, after.caret, 0);   // don't replace the same empty match again
      else g_fs.last_valid = 0;
      if (g_fs.scope_on) g_fs.scope_seq = d->change_seq();
      app_on_doc_changed(t);
      replaced = 1;
    }
  }
  r = op_find(t, g_fs.up);
  if (replaced && r == 0) status(0, "Replaced; no more matches");
  else if (replaced && unrep) status(0, "Replaced; %u character%s became '?'", unrep, unrep == 1 ? "" : "s");
  return replaced;
}

static int op_replace_tabs()
{
  int i, n = app_tab_count(), changed = 0, skipped = 0;
  u32 total = 0;
  char q[120];
  EStr detail;
  Searcher probe;
  Tab* a = app_active();
  if (n == 0) { status(0, "No document is open"); return 0; }
  if (prepare(a, &probe) != E_OK) return E_SYNTAX;
  es_fmt(q, sizeof(q), "Replace in all %d open document%s?", n, n == 1 ? "" : "s");
  if (ui_ask(Q_REPLACE_ALL_TABS, q, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    char name[280];
    u32 nr = 0, unrep = 0;
    int rc;
    if (!t || !t->doc) continue;
    tab_title(t, name, sizeof(name));
    if (!t->doc->editable() || t->indexing || t->saving) { skipped++; continue; }
    rc = replace_all_in(t, 0, &nr, &unrep, 0);
    if (rc == E_CANCEL) { detail.appendf("%s%s: cancelled", detail.len() ? ", " : "", name); break; }
    if (rc < 0) { skipped++; detail.appendf("%s%s: %s", detail.len() ? ", " : "", name, edit_error(rc)); continue; }
    if (nr) { changed++; total += nr; detail.appendf("%s%s: %u", detail.len() ? ", " : "", name, nr); }
  }
  {
    char sk[60];
    sk[0] = 0;
    if (skipped) es_fmt(sk, sizeof(sk), "; %d skipped (read-only or busy)", skipped);
    status(0, "%u replaced in %d of %d document%s%s%s%s", total, changed, n, n == 1 ? "" : "s", sk,
           detail.len() ? " - " : "", detail.c_str());
  }
  return (int)total;
}

// ---------------------------------------------------------------------------------------------------------------
// dialog

static void place(int id, int x, int y, int w, int h, int show)
{
  RECT r;
  HWND c = GetDlgItem(g_dlg, id);
  if (!c) return;
  r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
  MapDialogRect(g_dlg, &r);
  SetWindowPos(c, 0, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(c, show ? SW_SHOWNA : SW_HIDE);
  EnableWindow(c, show ? TRUE : FALSE);
}

static int checked(int id) { return IsDlgButtonChecked(g_dlg, id) == BST_CHECKED; }
static void check(int id, int on) { CheckDlgButton(g_dlg, id, on ? BST_CHECKED : BST_UNCHECKED); }

static int dlg_mode()
{
  if (checked(IDC_FIND_EXTENDED)) return SM_EXTENDED;
  if (checked(IDC_FIND_REGEX)) return SM_REGEX;
  if (checked(IDC_FIND_HEX)) return SM_HEX;
  return SM_NORMAL;
}

static void set_mode(int mode)
{
  CheckRadioButton(g_dlg, IDC_FIND_NORMAL, IDC_FIND_HEX, IDC_FIND_NORMAL + mode);
}

static void layout_page(int page)
{
  enum { R0 = 24, BX = 268, BW = 74, BH = 14, BS = 17 };
  int oy = R0 + 18 * (page == PAGE_FIND ? 1 : page == PAGE_REPLACE ? 2 : 3);
  int fif = page == PAGE_FIF, mx = fif ? 10 : 66, mw = fif ? 248 : 192, defid;
  RECT r;
  DWORD st, ex;
  if (page != g_page && !(page == PAGE_FIF && fif_running())) status(0, "");   // a result of another page
  g_page = page;
  place(IDC_FIND_WHAT_LBL, 10, R0 + 2, 50, 8, 1);
  place(IDC_FIND_WHAT, 62, R0, 180, 140, 1);
  place(IDC_FIND_WHAT_MENU, 244, R0, 14, 13, 1);
  place(IDC_FIND_REPL_LBL, 10, R0 + 20, 50, 8, page == PAGE_REPLACE);
  place(IDC_FIND_REPL, 62, R0 + 18, 180, 140, page == PAGE_REPLACE);
  place(IDC_FIND_REPL_MENU, 244, R0 + 18, 14, 13, page == PAGE_REPLACE);
  place(IDC_FIND_MASKS_LBL, 10, R0 + 20, 50, 8, fif);
  place(IDC_FIND_MASKS, 62, R0 + 18, 180, 140, fif);
  place(IDC_FIND_DIR_LBL, 10, R0 + 38, 50, 8, fif);
  place(IDC_FIND_DIR, 62, R0 + 36, 180, 140, fif);
  place(IDC_FIND_BROWSE, 244, R0 + 36, 14, 13, fif);
  place(IDC_FIND_CASE, 10, oy, 56, 10, 1);
  place(IDC_FIND_WORD, 68, oy, 56, 10, 1);
  place(IDC_FIND_WRAP, 126, oy, 58, 10, !fif);
  place(IDC_FIND_INSEL, 186, oy, 56, 10, !fif);
  place(IDC_FIND_SUBDIRS, 126, oy, 58, 10, fif);
  place(IDC_FIND_DIRGRP, 10, oy + 14, 52, 36, !fif);
  place(IDC_FIND_UP, 16, oy + 24, 40, 10, !fif);
  place(IDC_FIND_DOWN, 16, oy + 36, 40, 10, !fif);
  place(IDC_FIND_MODEGRP, mx, oy + 14, mw, 36, 1);
  place(IDC_FIND_NORMAL, mx + 6, oy + 24, 44, 10, 1);
  place(IDC_FIND_EXTENDED, mx + 52, oy + 24, 46, 10, 1);
  place(IDC_FIND_REGEX, mx + 100, oy + 24, 40, 10, 1);
  place(IDC_FIND_HEX, mx + 142, oy + 24, 46, 10, !fif);
  place(IDC_FIND_DOTALL, mx + 6, oy + 36, 120, 10, 1);
  place(IDC_FIND_STATUS, 10, oy + 54, 248, 9, 1);
  place(IDC_FIND_NEXT, BX, R0, BW, BH, !fif);
  place(IDC_FIND_COUNT, BX, R0 + BS, BW, BH, page == PAGE_FIND);
  place(IDC_FIND_ALL, BX, R0 + 2 * BS, BW, BH, page == PAGE_FIND);
  place(IDC_FIND_MARK, BX, R0 + 3 * BS, BW, BH, page == PAGE_FIND);
  place(IDC_FIND_REPLACE, BX, R0 + BS, BW, BH, page == PAGE_REPLACE);
  place(IDC_FIND_REPLALL, BX, R0 + 2 * BS, BW, BH, page == PAGE_REPLACE);
  place(IDC_FIND_REPLTABS, BX, R0 + 3 * BS, BW, BH, page == PAGE_REPLACE);
  place(IDC_FIND_FIFGO, BX, R0, BW, BH, fif);
  place(IDC_FIND_STOP, BX, R0 + BS, BW, BH, fif);
  place(IDC_FIND_CLOSE, BX, fif ? R0 + 2 * BS : R0 + 4 * BS, BW, BH, 1);
  place(IDC_FIND_TAB, 4, 4, 340, oy + 66, 1);
  if (fif && dlg_mode() == SM_HEX) set_mode(SM_NORMAL);
  // resize the dialog to the page
  r.left = 0; r.top = 0; r.right = 348; r.bottom = oy + 74;
  MapDialogRect(g_dlg, &r);
  st = (DWORD)GetWindowLongA(g_dlg, GWL_STYLE);
  ex = (DWORD)GetWindowLongA(g_dlg, GWL_EXSTYLE);
  AdjustWindowRectEx(&r, st, FALSE, ex);
  SetWindowPos(g_dlg, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  SendDlgItemMessageA(g_dlg, IDC_FIND_TAB, TCM_SETCURSEL, (WPARAM)page, 0);
  SetWindowTextA(g_dlg, page == PAGE_FIND ? "Find" : page == PAGE_REPLACE ? "Replace" : "Find in Files");
  defid = fif ? IDC_FIND_FIFGO : IDC_FIND_NEXT;
  SendMessageA(g_dlg, DM_SETDEFID, (WPARAM)defid, 0);
  // the button styles follow the default id
  SendDlgItemMessageA(g_dlg, IDC_FIND_NEXT, BM_SETSTYLE, fif ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
  SendDlgItemMessageA(g_dlg, IDC_FIND_FIFGO, BM_SETSTYLE, fif ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
  update_enables();
  InvalidateRect(g_dlg, 0, TRUE);
}

static void update_enables()
{
  Tab* t = app_active();
  int mode, fif = g_page == PAGE_FIF, hexok;
  if (!g_dlg) return;
  mode = dlg_mode();
  hexok = !fif && ((t && t->view == VIEW_HEX) || mode == SM_HEX);
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_HEX), hexok);
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_WHAT_MENU), mode == SM_REGEX);
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_REPL_MENU), g_page == PAGE_REPLACE && mode == SM_REGEX);
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_DOTALL), mode == SM_REGEX);
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_CASE), mode != SM_HEX);
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_WORD), mode != SM_HEX);
  if (fif) EnableWindow(GetDlgItem(g_dlg, IDC_FIND_STOP), fif_running());
}

static void fill_combo(int id, char (*list)[256], u32 n)
{
  HWND cb = GetDlgItem(g_dlg, id);
  char keep[PAT_MAX + 1];
  u32 i;
  if (!cb) return;
  GetWindowTextA(cb, keep, sizeof(keep));
  SendMessageA(cb, CB_RESETCONTENT, 0, 0);
  for (i = 0; i < n; i++) if (list[i][0]) SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)list[i]);
  SetWindowTextA(cb, keep);
}

static void fill_histories()
{
  fill_combo(IDC_FIND_WHAT, g_app.cfg.find_hist, CFG_HIST_MAX);
  fill_combo(IDC_FIND_REPL, g_app.cfg.repl_hist, CFG_HIST_MAX);
  fill_combo(IDC_FIND_MASKS, g_app.cfg.fif_masks, CFG_HIST_MAX);
  fill_combo(IDC_FIND_DIR, g_app.cfg.fif_dirs, CFG_HIST_MAX);
}

static void hist_add(char (*list)[256], int id, const char* text)
{
  if (!text[0]) return;
  config_hist_add(list, CFG_HIST_MAX, text);
  if (g_dlg) fill_combo(id, list, CFG_HIST_MAX);
}

static void load_controls()
{
  fs_init();
  check(IDC_FIND_CASE, g_fs.opts.match_case);
  check(IDC_FIND_WORD, g_fs.opts.whole_word);
  check(IDC_FIND_WRAP, g_fs.wrap);
  check(IDC_FIND_INSEL, g_fs.insel);
  check(IDC_FIND_DOTALL, g_fs.opts.dot_all);
  check(IDC_FIND_SUBDIRS, g_app.cfg.find_fif_sub);
  CheckRadioButton(g_dlg, IDC_FIND_UP, IDC_FIND_DOWN, g_fs.up ? IDC_FIND_UP : IDC_FIND_DOWN);
  set_mode(g_fs.opts.mode);
}

static void text_to_ecp(const char* s, Vec<ecp>* out)
{
  out->clear();
  ui_ansi_to_ecp(s, out);
  if (out->size() > PAT_MAX) out->resize(PAT_MAX);
}

static void read_controls()
{
  char buf[PAT_MAX + 1];
  if (!g_dlg) return;
  fs_init();
  GetDlgItemTextA(g_dlg, IDC_FIND_WHAT, buf, sizeof(buf));
  if (g_field_text[0] && strcmp(buf, g_field_text) == 0) {
    g_fs.pat.clear();
    g_fs.pat.insert(0, g_field_pat.data(), g_field_pat.size());
  } else {
    g_field_text[0] = 0;
    text_to_ecp(buf, &g_fs.pat);
  }
  GetDlgItemTextA(g_dlg, IDC_FIND_REPL, buf, sizeof(buf));
  text_to_ecp(buf, &g_fs.rep);
  g_fs.opts.mode = (u8)dlg_mode();
  g_fs.opts.match_case = (u8)checked(IDC_FIND_CASE);
  g_fs.opts.whole_word = (u8)checked(IDC_FIND_WORD);
  g_fs.opts.dot_all = (u8)checked(IDC_FIND_DOTALL);
  g_fs.wrap = (u8)checked(IDC_FIND_WRAP);
  g_fs.up = (u8)checked(IDC_FIND_UP);
  if (g_fs.insel != (u8)checked(IDC_FIND_INSEL)) g_fs.scope_on = 0;
  g_fs.insel = (u8)checked(IDC_FIND_INSEL);
  g_app.cfg.find_mode = g_fs.opts.mode; g_app.cfg.find_case = g_fs.opts.match_case;
  g_app.cfg.find_word = g_fs.opts.whole_word; g_app.cfg.find_dotall = g_fs.opts.dot_all;
  g_app.cfg.find_wrap = g_fs.wrap; g_app.cfg.find_dir_up = g_fs.up;
  g_app.cfg.find_fif_sub = (u8)checked(IDC_FIND_SUBDIRS);
}

// Puts a pattern into the Find what field (ANSI); unrepresentable characters show as '?' but stay searchable.
static void set_find_field(const ecp* s, u32 n)
{
  char buf[PAT_MAX + 1];
  int lost;
  if (n > PAT_MAX) n = PAT_MAX;
  lost = ui_ecp_to_ansi(s, n, buf, sizeof(buf));
  g_field_pat.clear();
  if (lost > 0) { g_field_pat.insert(0, s, n); es_copy(g_field_text, sizeof(g_field_text), buf); }
  else g_field_text[0] = 0;
  if (g_dlg) {
    SetDlgItemTextA(g_dlg, IDC_FIND_WHAT, buf);
    SendDlgItemMessageA(g_dlg, IDC_FIND_WHAT, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
  }
}

// Selected text (or the word at the caret) of a tab, decoded; 0 if none or it spans lines.
static int selection_text(Tab* t, int use_word, Vec<ecp>* out, int* multiline)
{
  Document* d = t->doc;
  Selection s = t->sel;
  epos lo, hi;
  u8 buf[PAT_MAX * 4];
  ecp cps[PAT_MAX + 4];
  u32 n, used = 0, i;
  out->clear();
  *multiline = 0;
  if (sel_empty(s)) {
    if (!use_word || t->view == VIEW_HEX) return 0;
    ed_word_at(d, s.caret, &s);
    if (sel_empty(s)) return 0;
  }
  lo = sel_lo(s); hi = sel_hi(s);
  if (d->line_of(lo) != d->line_of(hi)) { *multiline = 1; return 0; }
  if (hi - lo > sizeof(buf)) return 0;
  if (d->read(lo, buf, hi - lo) != E_OK) return 0;
  n = codec_decode_buf(d->codec(), buf, hi - lo, 1, cps, PAT_MAX + 4, &used);
  if (n == 0 || n > PAT_MAX) return 0;
  for (i = 0; i < n; i++) if (cps[i] == 0x0D || cps[i] == 0x0A) { *multiline = 1; return 0; }
  out->insert(0, cps, n);
  return 1;
}

static void prefill(Tab* t, int page)
{
  Vec<ecp> v;
  int multi = 0;
  if (!t || !t->doc) return;
  if (t->view == VIEW_HEX && !sel_empty(t->sel) && page != PAGE_FIF && sel_hi(t->sel) - sel_lo(t->sel) <= 64) {
    u8 b[64];
    char text[200];
    epos lo = sel_lo(t->sel), n = sel_hi(t->sel) - lo, i;
    if (t->doc->read(lo, b, n) == E_OK) {
      text[0] = 0;
      for (i = 0; i < n; i++) {
        char hx[3];
        hex_byte_text(b[i], hx);
        hx[2] = 0;
        if (i) es_cat(text, sizeof(text), " ");
        es_cat(text, sizeof(text), hx);
      }
      g_field_text[0] = 0;
      SetDlgItemTextA(g_dlg, IDC_FIND_WHAT, text);
      SendDlgItemMessageA(g_dlg, IDC_FIND_WHAT, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
      set_mode(SM_HEX);
      check(IDC_FIND_INSEL, 0);
    }
    return;
  }
  if (selection_text(t, 1, &v, &multi)) {
    set_find_field(v.data(), v.size());
    check(IDC_FIND_INSEL, 0);
  } else if (multi && page != PAGE_FIF) {
    check(IDC_FIND_INSEL, 1);                       // DESIGN 11.4: multi-line selection ticks In selection
    g_fs.scope_on = 0;
  }
}

static void prefill_fif(Tab* t)
{
  char masks[260], dir[260];
  GetDlgItemTextA(g_dlg, IDC_FIND_MASKS, masks, sizeof(masks));
  if (!masks[0]) {
    if (g_app.cfg.fif_masks[0][0]) es_copy(masks, sizeof(masks), g_app.cfg.fif_masks[0]);
    else if (t && t->lang && t->lang->exts[0]) {
      const char* p = t->lang->exts;
      masks[0] = 0;
      while (*p) {
        char ext[40];
        u32 k = 0;
        while (*p && *p != ';' && k + 1 < sizeof(ext)) ext[k++] = *p++;
        ext[k] = 0;
        if (*p == ';') p++;
        if (!k) continue;
        if (masks[0]) es_cat(masks, sizeof(masks), ";");
        es_cat(masks, sizeof(masks), "*.");
        es_cat(masks, sizeof(masks), ext);
      }
    } else es_copy(masks, sizeof(masks), "*.*");
    SetDlgItemTextA(g_dlg, IDC_FIND_MASKS, masks);
  }
  if (t && !t->untitled && t->path[0]) {
    ui_path_dir(t->path, dir, sizeof(dir));
    SetDlgItemTextA(g_dlg, IDC_FIND_DIR, dir);
  } else {
    GetDlgItemTextA(g_dlg, IDC_FIND_DIR, dir, sizeof(dir));
    if (!dir[0]) {
      if (g_app.cfg.fif_dirs[0][0]) es_copy(dir, sizeof(dir), g_app.cfg.fif_dirs[0]);
      else GetCurrentDirectoryA(sizeof(dir), dir);
      SetDlgItemTextA(g_dlg, IDC_FIND_DIR, dir);
    }
  }
}

// Live validation (200 ms after an edit): regex/extended/hex errors in red.
static void validate_now()
{
  Searcher s;
  SearchError err;
  char msg[200];
  Tab* t = app_active();
  int rc;
  read_controls();
  if (g_fs.pat.empty() || g_fs.opts.mode == SM_NORMAL) {
    if (g_status_err) status(0, "");
    return;
  }
  err.code = 0; err.pos = 0; err.msg[0] = 0;
  rc = s.compile(g_fs.pat.data(), g_fs.pat.size(), g_fs.opts, t ? t->doc->enc() : enc_utf8(0), &err);
  if (rc != E_OK) {
    if (!err.msg[0]) es_copy(err.msg, sizeof(err.msg), e_errname(rc));
    format_compile_error(err, msg, sizeof(msg));
    status(1, "%s", msg);
  } else if (g_status_err) {
    status(0, "");
  }
}

struct TokItem { const char* label; const char* ins; int back; int group; };
// Find tokens are grouped into submenus so the menu fits an 800x600 screen.
static const char* const k_tok_groups[] = { "&Characters", "&Anchors", "&Repeat", "&Groups", "&Look-ahead and flags" };
static const TokItem k_find_tok[] = {
  { ".\tAny character", ".", 0, 0 }, { "\\d\tDigit", "\\d", 0, 0 }, { "\\w\tWord character", "\\w", 0, 0 },
  { "\\s\tWhitespace", "\\s", 0, 0 }, { "\\D \\W \\S\tNot a digit / word char / space", "\\W", 0, 0 },
  { "[...]\tAny of these characters", "[]", 1, 0 }, { "[^...]\tNone of these characters", "[^]", 1, 0 },
  { "\\t\tTab", "\\t", 0, 0 }, { "\\R\tLine break (CRLF, LF or CR)", "\\R", 0, 0 },
  { "\\x{HHHH}\tCharacter code", "\\x{}", 1, 0 },
  { "^\tStart of line", "^", 0, 1 }, { "$\tEnd of line", "$", 0, 1 }, { "\\b\tWord boundary", "\\b", 0, 1 },
  { "*\t0 or more times", "*", 0, 2 }, { "+\t1 or more times", "+", 0, 2 }, { "?\t0 or 1 time", "?", 0, 2 },
  { "{n,m}\tn to m times", "{1,3}", 0, 2 }, { "*?\t0 or more, as few as possible", "*?", 0, 2 },
  { "+?\t1 or more, as few as possible", "+?", 0, 2 },
  { "(...)\tGroup (\\1 in replace)", "()", 1, 3 }, { "(?:...)\tGroup without a number", "(?:)", 1, 3 },
  { "|\tOr", "|", 0, 3 }, { "\\1\tText of group 1 again", "\\1", 0, 3 },
  { "(?=...)\tFollowed by", "(?=)", 1, 4 }, { "(?!...)\tNot followed by", "(?!)", 1, 4 },
  { "(?i)\tIgnore case from here", "(?i)", 0, 4 }
};
static const TokItem k_repl_tok[] = {
  { "$0\tThe whole match", "$0", 0, 0 }, { "$1\tGroup 1", "$1", 0, 0 }, { "$2\tGroup 2", "$2", 0, 0 },
  { "$3\tGroup 3", "$3", 0, 0 }, { "\\n\tNew line (LF)", "\\n", 0, 0 }, { "\\r\\n\tNew line (CRLF)", "\\r\\n", 0, 0 },
  { "\\t\tTab", "\\t", 0, 0 }, { "\\U...\\E\tUPPER CASE", "\\U\\E", 2, 0 }, { "\\L...\\E\tlower case", "\\L\\E", 2, 0 },
  { "\\u\tNext character upper case", "\\u", 0, 0 }, { "\\l\tNext character lower case", "\\l", 0, 0 },
  { "$$\tA dollar sign", "$$", 0, 0 }
};

// Builds the token popup (find: one submenu per group; replace: flat, 12 entries) and returns it.
static HMENU token_popup(int field, const TokItem* items, u32 n)
{
  HMENU m = CreatePopupMenu();
  u32 i, g;
  if (!m) return 0;
  if (field == IDC_FIND_WHAT) {
    for (g = 0; g < sizeof(k_tok_groups) / sizeof(k_tok_groups[0]); g++) {
      HMENU sub = CreatePopupMenu();
      if (!sub) continue;
      for (i = 0; i < n; i++) if (items[i].group == (int)g) AppendMenuA(sub, MF_STRING, IDM_TOK_FIRST + i, items[i].label);
      AppendMenuA(m, MF_POPUP, (UINT)sub, k_tok_groups[g]);   // m owns sub from here
    }
  } else {
    for (i = 0; i < n; i++) AppendMenuA(m, MF_STRING, IDM_TOK_FIRST + i, items[i].label);
  }
  AppendMenuA(m, MF_SEPARATOR, 0, 0);
  AppendMenuA(m, MF_STRING, IDM_TOK_HELP, "Regular expression &help...");
  return m;
}

static void token_menu(int field, int button)
{
  const TokItem* items = field == IDC_FIND_WHAT ? k_find_tok : k_repl_tok;
  u32 n = field == IDC_FIND_WHAT ? sizeof(k_find_tok) / sizeof(k_find_tok[0]) : sizeof(k_repl_tok) / sizeof(k_repl_tok[0]);
  HMENU m = token_popup(field, items, n);
  RECT r;
  int id;
  if (!m) return;
  GetWindowRect(GetDlgItem(g_dlg, button), &r);
  id = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, r.left, r.bottom, 0,
                           g_dlg, 0);
  DestroyMenu(m);
  if (id == IDM_TOK_HELP) { app_command(IDM_HELP_REGEX); return; }
  if (id >= IDM_TOK_FIRST && id < (int)(IDM_TOK_FIRST + n)) {
    HWND cb = GetDlgItem(g_dlg, field);
    char text[PAT_MAX + 1], out[PAT_MAX + 1];
    DWORD sel = (DWORD)SendMessageA(cb, CB_GETEDITSEL, 0, 0);
    u32 s = LOWORD(sel), e = HIWORD(sel), len, caret;
    const TokItem& it = items[id - IDM_TOK_FIRST];
    GetWindowTextA(cb, text, sizeof(text));
    len = es_len(text);
    if (s > len) s = len;
    if (e > len || e < s) e = s;
    if (len - (e - s) + es_len(it.ins) > PAT_MAX) { MessageBeep(MB_OK); return; }
    memcpy(out, text, s);
    out[s] = 0;
    es_cat(out, sizeof(out), it.ins);
    caret = es_len(out) - (u32)it.back;
    es_cat(out, sizeof(out), text + e);
    SetWindowTextA(cb, out);
    SetFocus(cb);
    SendMessageA(cb, CB_SETEDITSEL, 0, MAKELPARAM(caret, caret));
    SetTimer(g_dlg, TIMER_VALIDATE, 200, 0);
  }
}

static int CALLBACK browse_cb(HWND w, UINT msg, LPARAM lp, LPARAM data)
{
  (void)lp;
  if (msg == BFFM_INITIALIZED && data) SendMessageA(w, BFFM_SETSELECTIONA, TRUE, data);
  return 0;
}

static void browse_folder()
{
  BROWSEINFOA bi;
  char disp[MAX_PATH], cur[MAX_PATH], path[MAX_PATH];
  LPITEMIDLIST pidl;
  GetDlgItemTextA(g_dlg, IDC_FIND_DIR, cur, sizeof(cur));
  memset(&bi, 0, sizeof(bi));
  bi.hwndOwner = g_dlg;
  bi.pszDisplayName = disp;
  bi.lpszTitle = "Folder to search:";
  bi.ulFlags = BIF_RETURNONLYFSDIRS;
  bi.lpfn = browse_cb;
  bi.lParam = (LPARAM)cur;
  pidl = SHBrowseForFolderA(&bi);
  if (!pidl) return;
  if (SHGetPathFromIDListA(pidl, path)) SetDlgItemTextA(g_dlg, IDC_FIND_DIR, path);
  {
    IMalloc* mal = 0;
    if (SHGetMalloc(&mal) == NOERROR && mal) { mal->Free(pidl); mal->Release(); }
  }
}

static int op_fif()
{
  Searcher s;
  SearchError err;
  char dir[300], full[300], masks[260], pat[PAT_MAX + 1], msg[200];
  FifQuery* q;
  int rc;
  u32 i;
  read_controls();
  if (g_fs.pat.empty()) { status(0, "Type something to find"); MessageBeep(MB_OK); return E_SYNTAX; }
  if (g_fs.opts.mode == SM_HEX) g_fs.opts.mode = SM_NORMAL;
  err.code = 0; err.pos = 0; err.msg[0] = 0;
  rc = s.compile(g_fs.pat.data(), g_fs.pat.size(), g_fs.opts, enc_utf8(0), &err);
  if (rc != E_OK) {
    if (!err.msg[0]) es_copy(err.msg, sizeof(err.msg), e_errname(rc));
    format_compile_error(err, msg, sizeof(msg));
    status(1, "%s", msg);
    MessageBeep(MB_OK);
    return rc;
  }
  GetDlgItemTextA(g_dlg, IDC_FIND_DIR, dir, sizeof(dir));
  GetDlgItemTextA(g_dlg, IDC_FIND_MASKS, masks, sizeof(masks));
  es_trim(dir);
  es_trim(masks);
  if (!dir[0]) { status(1, "Choose a folder to search"); return E_STATE; }
  if (ui_path_full(dir, full, sizeof(full)) != E_OK) es_copy(full, sizeof(full), dir);
  {
    u32 n = es_len(full);
    if (n > 3 && (full[n - 1] == '\\' || full[n - 1] == '/')) full[n - 1] = 0;
  }
  if (!ui_dir_exists(full)) { status(1, "Folder not found: %s", full); MessageBeep(MB_OK); return E_NOTFOUND; }
  if (!masks[0]) es_copy(masks, sizeof(masks), "*.*");
  q = (FifQuery*)e_alloc(sizeof(FifQuery));
  if (!q) { status(1, "Out of memory"); return E_NOMEM; }
  memset(q, 0, sizeof(*q));
  es_copy(q->folder, sizeof(q->folder), full);
  es_copy(q->masks, sizeof(q->masks), masks);
  q->subfolders = (u8)checked(IDC_FIND_SUBDIRS);
  q->opts = g_fs.opts;
  for (i = 0; i < g_fs.pat.size() && i < RX_MAX_PATTERN; i++) q->pattern[i] = g_fs.pat[i];
  q->pattern_len = i;
  q->det.utf8_nobom = g_app.cfg.detect_utf8;
  q->det.utf16_nobom = 1;
  q->det.ansi = enc_ansi();
  q->det.default_eol = (EolStyle)(g_app.cfg.new_eol <= EOL_CR ? g_app.cfg.new_eol : EOL_CRLF);
  q->max_hits = 50000;
  GetDlgItemTextA(g_dlg, IDC_FIND_WHAT, pat, sizeof(pat));
  hist_add(g_app.cfg.find_hist, IDC_FIND_WHAT, pat);
  hist_add(g_app.cfg.fif_masks, IDC_FIND_MASKS, masks);
  hist_add(g_app.cfg.fif_dirs, IDC_FIND_DIR, full);
  SetDlgItemTextA(g_dlg, IDC_FIND_DIR, full);
  rc = fif_start(*q);
  e_free(q);
  if (rc == E_OK) status(0, "Searching...");
  else status(1, "Could not start the search (%s)", e_errname(rc));
  return rc;
}

static void dlg_hide()
{
  Tab* t = app_active();
  HWND v = view_of(t);
  if (!g_dlg) return;
  KillTimer(g_dlg, TIMER_VALIDATE);
  ShowWindow(g_dlg, SW_HIDE);
  if (g_app.frame) SetActiveWindow(g_app.frame);
  if (v && IsWindowVisible(v)) SetFocus(v);
}

static void remember_pos()
{
  RECT r;
  if (g_automove || !g_dlg || !IsWindowVisible(g_dlg) || IsIconic(g_dlg)) return;
  GetWindowRect(g_dlg, &r);
  g_app.cfg.win_find_x = r.left;
  g_app.cfg.win_find_y = r.top;
}

static void history_from_fields()
{
  char buf[PAT_MAX + 1];
  GetDlgItemTextA(g_dlg, IDC_FIND_WHAT, buf, sizeof(buf));
  hist_add(g_app.cfg.find_hist, IDC_FIND_WHAT, buf);
  if (g_page == PAGE_REPLACE) {
    GetDlgItemTextA(g_dlg, IDC_FIND_REPL, buf, sizeof(buf));
    hist_add(g_app.cfg.repl_hist, IDC_FIND_REPL, buf);
  }
}

static void dlg_command(int id, int code)
{
  Tab* t = app_active();
  switch (id) {
  case IDC_FIND_NEXT: read_controls(); history_from_fields(); op_find(t, g_fs.up); break;
  case IDC_FIND_COUNT: read_controls(); history_from_fields(); op_count(t); break;
  case IDC_FIND_ALL: read_controls(); history_from_fields(); op_find_all(t); break;
  case IDC_FIND_MARK: read_controls(); history_from_fields(); op_mark(t); break;
  case IDC_FIND_REPLACE: read_controls(); history_from_fields(); op_replace(t); break;
  case IDC_FIND_REPLALL: read_controls(); history_from_fields(); op_replace_all(t); break;
  case IDC_FIND_REPLTABS: read_controls(); history_from_fields(); op_replace_tabs(); break;
  case IDC_FIND_FIFGO: op_fif(); break;
  case IDC_FIND_STOP: fif_stop(); break;
  case IDC_FIND_BROWSE: browse_folder(); break;
  case IDC_FIND_WHAT_MENU: token_menu(IDC_FIND_WHAT, IDC_FIND_WHAT_MENU); break;
  case IDC_FIND_REPL_MENU: token_menu(IDC_FIND_REPL, IDC_FIND_REPL_MENU); break;
  case IDC_FIND_CLOSE: case IDCANCEL: dlg_hide(); break;
  case IDOK: dlg_command(g_page == PAGE_FIF ? IDC_FIND_FIFGO : IDC_FIND_NEXT, 0); break;
  case IDC_FIND_WHAT: case IDC_FIND_REPL:
    if (code == CBN_EDITCHANGE || code == CBN_SELCHANGE) SetTimer(g_dlg, TIMER_VALIDATE, 200, 0);
    break;
  case IDC_FIND_NORMAL: case IDC_FIND_EXTENDED: case IDC_FIND_REGEX: case IDC_FIND_HEX: case IDC_FIND_CASE:
  case IDC_FIND_WORD: case IDC_FIND_DOTALL:
    update_enables();
    SetTimer(g_dlg, TIMER_VALIDATE, 50, 0);
    break;
  case IDC_FIND_INSEL: g_fs.scope_on = 0; break;
  default: break;
  }
}

static BOOL CALLBACK find_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_INITDIALOG: {
    TCITEMA ti;
    static const char* names[3] = { "Find", "Replace", "Find in Files" };
    int i;
    g_dlg = w;
    memset(&ti, 0, sizeof(ti));
    ti.mask = TCIF_TEXT;
    for (i = 0; i < 3; i++) {
      ti.pszText = (char*)names[i];
      SendDlgItemMessageA(w, IDC_FIND_TAB, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&ti);
    }
    SendDlgItemMessageA(w, IDC_FIND_WHAT, CB_LIMITTEXT, PAT_MAX, 0);
    SendDlgItemMessageA(w, IDC_FIND_REPL, CB_LIMITTEXT, PAT_MAX, 0);
    SendDlgItemMessageA(w, IDC_FIND_MASKS, CB_LIMITTEXT, 255, 0);
    SendDlgItemMessageA(w, IDC_FIND_DIR, CB_LIMITTEXT, 255, 0);
    fill_histories();
    load_controls();
    return FALSE;
  }
  case WM_COMMAND:
    dlg_command(LOWORD(wp), HIWORD(wp));
    return TRUE;
  case WM_NOTIFY: {
    NMHDR* nm = (NMHDR*)lp;
    if (nm->idFrom == IDC_FIND_TAB && nm->code == TCN_SELCHANGE) {
      int page = (int)SendDlgItemMessageA(w, IDC_FIND_TAB, TCM_GETCURSEL, 0, 0);
      layout_page(page < 0 ? 0 : page);
      if (page == PAGE_FIF) prefill_fif(app_active());
    }
    return TRUE;
  }
  case WM_TIMER:
    if (wp == TIMER_VALIDATE) { KillTimer(w, TIMER_VALIDATE); validate_now(); }
    return TRUE;
  case WM_CTLCOLORSTATIC:
    if ((HWND)lp == GetDlgItem(w, IDC_FIND_STATUS)) {
      HDC dc = (HDC)wp;
      SetTextColor(dc, g_status_err ? RGB(204, 0, 0) : GetSysColor(COLOR_BTNTEXT));
      SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
      return (BOOL)GetSysColorBrush(COLOR_BTNFACE);
    }
    return FALSE;
  case WM_ACTIVATE:
    if (LOWORD(wp) != WA_INACTIVE) update_enables();
    return FALSE;
  case WM_MOVE:
    remember_pos();
    return FALSE;
  case WM_CLOSE:
    dlg_hide();
    return TRUE;
  case WM_DESTROY:
    g_dlg = 0;
    g_app.find_dlg = 0;
    return FALSE;
  default:
    return FALSE;
  }
}

static int dlg_ensure()
{
  if (g_dlg) return 1;
  g_dlg = CreateDialogParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_FIND), g_app.frame, find_proc, 0);
  g_app.find_dlg = g_dlg;
  return g_dlg != 0;
}

static void dlg_position()
{
  RECT dr, fr, wa;
  int x = g_app.cfg.win_find_x, y = g_app.cfg.win_find_y, w, h;
  GetWindowRect(g_dlg, &dr);
  w = dr.right - dr.left; h = dr.bottom - dr.top;
  SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
  if (x == -1 && y == -1) {
    GetWindowRect(g_app.frame, &fr);
    x = fr.right - w - 40;
    y = fr.top + 90;
  }
  if (x + w > wa.right) x = wa.right - w;
  if (y + h > wa.bottom) y = wa.bottom - h;
  if (x < wa.left) x = wa.left;
  if (y < wa.top) y = wa.top;
  g_automove = 1;
  SetWindowPos(g_dlg, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  g_automove = 0;
}

void finddlg_open(int page)
{
  Tab* t = app_active();
  int was_visible;
  if (!dlg_ensure()) { ui_error("Could not open the Find dialog."); return; }
  was_visible = IsWindowVisible(g_dlg);
  if (was_visible && GetActiveWindow() == g_dlg) {
    // Ctrl+F / Ctrl+H / Ctrl+Shift+F pressed inside the dialog (IDA_FIND): switch the page like a tab click and keep
    // what the user typed.
    if (page != g_page) {
      layout_page(page);
      if (page == PAGE_FIF) prefill_fif(t);
    }
    SetFocus(GetDlgItem(g_dlg, IDC_FIND_WHAT));
    SendDlgItemMessageA(g_dlg, IDC_FIND_WHAT, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
    SetTimer(g_dlg, TIMER_VALIDATE, 200, 0);
    return;
  }
  layout_page(page);
  if (page == PAGE_FIF) prefill_fif(t);
  prefill(t, page);
  if (!was_visible) {
    dlg_position();
    status(0, "");
    ShowWindow(g_dlg, SW_SHOW);
  }
  update_enables();
  SetActiveWindow(g_dlg);
  SetFocus(GetDlgItem(g_dlg, IDC_FIND_WHAT));
  SendDlgItemMessageA(g_dlg, IDC_FIND_WHAT, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
  SetTimer(g_dlg, TIMER_VALIDATE, 200, 0);
}

void finddlg_fif_state(int running)
{
  if (!g_dlg) return;
  EnableWindow(GetDlgItem(g_dlg, IDC_FIND_STOP), g_page == PAGE_FIF && running);
  if (!running && g_page == PAGE_FIF && IsWindowVisible(g_dlg)) {
    char text[160];
    int r = fif_last_result();
    es_fmt(text, sizeof(text), "%u hit%s in %u file%s%s", results_hit_count(), results_hit_count() == 1 ? "" : "s",
           results_file_count(), results_file_count() == 1 ? "" : "s",
           r == E_CANCEL ? " (cancelled)" : r == E_TOOBIG ? " (stopped at the hit limit)" :
           r == E_NOTFOUND ? " (folder not found)" : "");
    status(0, "%s", text);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// commands

static void find_next_cmd(int backward)
{
  Tab* t;
  if (isearch_focused()) { isearch_next(backward); return; }
  t = app_active();
  if (!t) return;
  fs_init();
  if (g_dlg && IsWindowVisible(g_dlg)) read_controls();
  if (g_fs.pat.empty()) { finddlg_open(PAGE_FIND); return; }
  op_find(t, backward);
}

static void find_sel_cmd(int backward)
{
  Tab* t = app_active();
  Vec<ecp> v;
  int multi = 0;
  if (!t) return;
  fs_init();
  if (!selection_text(t, 1, &v, &multi)) {
    status(0, multi ? "Find Selection works on a selection inside one line" : "No word at the caret");
    MessageBeep(MB_OK);
    return;
  }
  g_fs.pat.clear();
  g_fs.pat.insert(0, v.data(), v.size());
  g_fs.opts.mode = SM_NORMAL;
  g_app.cfg.find_mode = SM_NORMAL;
  set_find_field(v.data(), v.size());
  if (g_dlg) set_mode(SM_NORMAL);
  {
    char buf[PAT_MAX + 1];
    ui_ecp_to_ansi(v.data(), v.size(), buf, sizeof(buf));
    hist_add(g_app.cfg.find_hist, IDC_FIND_WHAT, buf);
  }
  if (g_field_text[0] && g_dlg) {                    // field shows '?': keep the real text bound to it
    char buf[PAT_MAX + 1];
    GetDlgItemTextA(g_dlg, IDC_FIND_WHAT, buf, sizeof(buf));
    es_copy(g_field_text, sizeof(g_field_text), buf);
  }
  g_fs.insel = 0;
  if (g_dlg) check(IDC_FIND_INSEL, 0);
  op_find(t, backward);
}

void cmd_search_find(void) { finddlg_open(PAGE_FIND); }
void cmd_search_replace(void) { finddlg_open(PAGE_REPLACE); }
void cmd_search_next(void) { find_next_cmd(0); }
void cmd_search_prev(void) { find_next_cmd(1); }
void cmd_search_selnext(void) { find_sel_cmd(0); }
void cmd_search_selprev(void) { find_sel_cmd(1); }

void cmd_search_clearmarks(void)
{
  Tab* t = app_active();
  if (!t) return;
  t->marks.clear();
  invalidate_view(t);
  app_status_note("Marks cleared");
}

UINT cmdstate_search_clearmarks(void)
{
  Tab* t = app_active();
  return (t && t->marks.count()) ? 0 : MF_GRAYED;
}

// ---------------------------------------------------------------------------------------------------------------
// selftest (st_search): find / replace / regex error / count / mark all / replace all undo / in selection /
// replace in open tabs / Find All into the results panel / Ctrl+F3 / hex bytes / incremental search bar

static Tab* st_tab(StCtx* c, const char* name, const void* bytes, u32 n, const EncSpec& enc, EolStyle eol)
{
  Document* d = new Document;
  MemSource* src;
  Tab* t;
  int rc;
  if (!d) { ST_CHECK(c, 0, "new Document"); return 0; }
  src = new MemSource;
  if (!src || src->copy_from((const u8*)bytes, n) != E_OK) { ST_CHECK(c, 0, "MemSource"); delete src; delete d; return 0; }
  rc = d->init_source(src, enc, enc.bom ? (enc.kind == EK_UTF8 ? 3 : 2) : 0, eol, 0);
  if (rc != E_OK) { st_checkf(c, 0, "init_source: %s", e_errname(rc)); delete d; return 0; }
  t = tab_new(d);
  if (!t) { ST_CHECK(c, 0, "tab_new"); delete d; return 0; }
  es_copy(t->path, sizeof(t->path), name);
  t->untitled = 0;
  if (app_add_tab(t, 1) != E_OK) { tab_free(t); return 0; }
  return t;
}

static void st_close(Tab* t)
{
  if (!t || !tab_alive(t)) return;
  app_remove_tab(t);
  tab_free(t);
}

static void st_set(const char* pat, const char* rep, int mode, int mcase, int word)
{
  fs_init();
  g_fs.pat.clear(); g_fs.rep.clear();
  es_utf8_to_ecp(pat, es_len(pat), &g_fs.pat);
  if (rep) es_utf8_to_ecp(rep, es_len(rep), &g_fs.rep);
  g_fs.opts.mode = (u8)mode; g_fs.opts.match_case = (u8)mcase; g_fs.opts.whole_word = (u8)word; g_fs.opts.dot_all = 0;
  g_fs.wrap = 1; g_fs.up = 0; g_fs.insel = 0; g_fs.scope_on = 0; g_fs.last_valid = 0;
  g_field_text[0] = 0;
}

// bench.cpp: Find Next (op 0) or Replace All (op 1) on t with this pattern (UTF-8), the way the dialog runs them.
int finddlg_bench(Tab* t, const char* pat, const char* rep, int mode, int match_case, int op)
{
  if (!t) return E_STATE;
  st_set(pat, rep ? rep : "", mode, match_case, 0);
  return op == 0 ? op_find(t, 0) : op_replace_all(t);
}

static int st_doc_is(Tab* t, const char* want)
{
  EBytes b;
  u32 n = t->doc->length();
  if (b.resize(n) != E_OK) return 0;
  if (n && t->doc->read(0, b.data(), n) != E_OK) return 0;
  return n == es_len(want) && memcmp(b.data(), want, n) == 0;
}

static int st_sel_is(Tab* t, epos a, epos c) { return t->sel.anchor == a && t->sel.caret == c; }
static int st_status_has(const char* s) { return strstr(g_status, s) != 0; }

void st_search(StCtx* c)
{
  static const char text[] = "alpha beta\r\nGamma alpha\r\nalphabet alpha.\r\n";
  Tab* t = st_tab(c, "C:\\st_search.txt", text, sizeof(text) - 1, enc_cp(1252), EOL_CRLF);
  Tab* t2;
  Tab* t3;
  Selection u;
  int r;
  if (!t) return;
  // Find Next / wrap / backward
  st_set("alpha", 0, SM_NORMAL, 1, 0);
  search_select(t, 0, 0, SCROLL_NONE);
  r = op_find(t, 0);
  st_checkf(c, r == 1 && st_sel_is(t, 0, 5), "find 1: r %d sel %u-%u", r, t->sel.anchor, t->sel.caret);
  op_find(t, 0);
  st_checkf(c, st_sel_is(t, 18, 23), "find 2: sel %u-%u", t->sel.anchor, t->sel.caret);
  op_find(t, 0); op_find(t, 0);
  ST_CHECK(c, st_sel_is(t, 34, 39), "find 4 (alpha.)");
  op_find(t, 0);
  ST_CHECK(c, st_sel_is(t, 0, 5) && st_status_has("Wrapped to the top"), "wrap to top");
  op_find(t, 1);
  ST_CHECK(c, st_sel_is(t, 34, 39) && st_status_has("Wrapped to the bottom"), "backward wraps to the bottom");
  op_find(t, 1);
  ST_CHECK(c, st_sel_is(t, 25, 30), "backward: alphabet");
  g_fs.wrap = 0;
  search_select(t, 34, 39, SCROLL_NONE);
  r = op_find(t, 0);
  ST_CHECK(c, r == 0 && st_status_has("Not found") && st_sel_is(t, 34, 39), "no wrap: not found, selection kept");
  // whole word / case
  st_set("alpha", 0, SM_NORMAL, 1, 1);
  ST_CHECK(c, op_count(t) == 3 && st_status_has("3 matches"), "whole word count 3");
  st_set("ALPHA", 0, SM_NORMAL, 0, 0);
  ST_CHECK(c, op_count(t) == 4, "case-insensitive count 4");
  st_set("ALPHA", 0, SM_NORMAL, 1, 0);
  ST_CHECK(c, op_count(t) == 0 && st_status_has("0 matches"), "match case count 0");
  // regex error in the dialog: red status, Find Next shows it and keeps the selection
  finddlg_open(PAGE_FIND);
  ST_CHECK(c, g_dlg != 0 && IsWindowVisible(g_dlg), "dialog open");
  if (g_dlg) {
    set_mode(SM_REGEX);
    SetDlgItemTextA(g_dlg, IDC_FIND_WHAT, "(ab|c");
    g_field_text[0] = 0;
    validate_now();
    ST_CHECK(c, g_status_err && st_status_has("Regex error at character"), "live validation: red regex error");
    search_select(t, 3, 3, SCROLL_NONE);
    dlg_command(IDC_FIND_NEXT, BN_CLICKED);
    ST_CHECK(c, g_status_err && st_sel_is(t, 3, 3), "Find Next with a bad regex: error, selection unchanged");
    SetDlgItemTextA(g_dlg, IDC_FIND_WHAT, "(?<=a)b");
    validate_now();
    ST_CHECK(c, g_status_err && st_status_has("not supported"), "unsupported construct named");
    SetDlgItemTextA(g_dlg, IDC_FIND_WHAT, "G\\w+");
    validate_now();
    ST_CHECK(c, !g_status_err, "valid regex clears the error");
    dlg_command(IDC_FIND_NEXT, BN_CLICKED);
    ST_CHECK(c, st_sel_is(t, 12, 17), "regex Find Next from the dialog");
    set_mode(SM_EXTENDED);
    SetDlgItemTextA(g_dlg, IDC_FIND_WHAT, "a\\q");
    validate_now();
    ST_CHECK(c, g_status_err, "extended: invalid escape is an error");
    layout_page(PAGE_REPLACE);
    layout_page(PAGE_FIF);
    ST_CHECK(c, IsWindowVisible(GetDlgItem(g_dlg, IDC_FIND_DIR)) && !IsWindowVisible(GetDlgItem(g_dlg, IDC_FIND_REPL)),
             "Find in Files page shows its controls");
    layout_page(PAGE_FIND);
    set_mode(SM_NORMAL);
    dlg_hide();
  }
  // Mark All / Clear Marks
  st_set("alpha", 0, SM_NORMAL, 1, 0);
  ST_CHECK(c, op_mark(t) == 4 && t->marks.count() == 4 && st_status_has("4 marked"), "mark all 4");
  ST_CHECK(c, cmdstate_search_clearmarks() == 0, "clear marks enabled");
  cmd_search_clearmarks();
  ST_CHECK(c, t->marks.count() == 0 && cmdstate_search_clearmarks() == MF_GRAYED, "clear marks");
  // Replace All + one undo step
  st_set("alpha", "omega", SM_NORMAL, 1, 0);
  ST_CHECK(c, op_replace_all(t) == 4 && st_status_has("4 replaced"), "replace all 4");
  ST_CHECK(c, st_doc_is(t, "omega beta\r\nGamma omega\r\nomegabet omega.\r\n"), "replace all bytes");
  ST_CHECK(c, t->doc->undo(&u) == E_OK && st_doc_is(t, text), "replace all is one undo step");
  // In selection (line 2 only)
  search_select(t, 12, 25, SCROLL_NONE);
  st_set("alpha", "X", SM_NORMAL, 1, 0);
  g_fs.insel = 1;
  ST_CHECK(c, op_count(t) == 1 && st_status_has("in the selection"), "count in selection");
  ST_CHECK(c, op_replace_all(t) == 1, "replace all in selection");
  ST_CHECK(c, st_doc_is(t, "alpha beta\r\nGamma X\r\nalphabet alpha.\r\n"), "in-selection bytes");
  ST_CHECK(c, st_sel_is(t, 12, 21), "selection covers the replaced range");
  t->doc->undo(&u);
  // In selection Find Next stays inside the scope and wraps inside it
  search_select(t, 25, 41, SCROLL_NONE);
  st_set("alpha", 0, SM_NORMAL, 1, 0);
  g_fs.insel = 1;
  op_find(t, 0);
  ST_CHECK(c, st_sel_is(t, 25, 30), "scoped find 1");
  op_find(t, 0);
  ST_CHECK(c, st_sel_is(t, 34, 39), "scoped find 2");
  op_find(t, 0);
  ST_CHECK(c, st_sel_is(t, 25, 30) && st_status_has("start of the selection"), "scoped wrap");
  // Extended: CRLF -> LF
  st_set("\\r\\n", "\\n", SM_EXTENDED, 1, 0);
  ST_CHECK(c, op_replace_all(t) == 3 && st_doc_is(t, "alpha beta\nGamma alpha\nalphabet alpha.\n"), "extended CRLF -> LF");
  t->doc->undo(&u);
  // Regex groups and case operators
  st_set("(\\w+) (\\w+)", "\\U$2\\E-$1", SM_REGEX, 1, 0);
  op_replace_all(t);
  ST_CHECK(c, st_doc_is(t, "BETA-alpha\r\nALPHA-Gamma\r\nALPHA-alphabet.\r\n"), "regex groups + \\U");
  t->doc->undo(&u);
  // empty matches: ^ gets a prefix on every line
  st_set("^", "> ", SM_REGEX, 1, 0);
  op_replace_all(t);
  ST_CHECK(c, st_doc_is(t, "> alpha beta\r\n> Gamma alpha\r\n> alphabet alpha.\r\n> "), "^ prefixes every line");
  t->doc->undo(&u);
  // Replace one by one
  st_set("alpha", "A", SM_NORMAL, 1, 1);
  search_select(t, 0, 0, SCROLL_NONE);
  op_replace(t);                                    // first press only selects
  ST_CHECK(c, st_sel_is(t, 0, 5), "replace: first press selects the match");
  op_replace(t);
  ST_CHECK(c, st_doc_is(t, "A beta\r\nGamma alpha\r\nalphabet alpha.\r\n") && st_sel_is(t, 14, 19),
           "replace: replaced and found the next");
  op_replace(t); op_replace(t);
  ST_CHECK(c, st_doc_is(t, "A beta\r\nGamma A\r\nalphabet A.\r\n"), "replace: whole words only");
  ST_CHECK(c, st_status_has("no more matches"), "replace: no more matches");
  while (t->doc->can_undo()) t->doc->undo(&u);
  ST_CHECK(c, st_doc_is(t, text), "undo all replaces");
  // unrepresentable replacement characters become '?'
  st_set("beta", "\\u0416x", SM_EXTENDED, 1, 0);
  op_replace_all(t);
  ST_CHECK(c, st_doc_is(t, "alpha ?x\r\nGamma alpha\r\nalphabet alpha.\r\n") && st_status_has("became '?'"),
           "unrepresentable -> '?' counted");
  t->doc->undo(&u);
  // read-only document
  t->doc->set_read_only(true);
  st_set("alpha", "z", SM_NORMAL, 1, 0);
  ST_CHECK(c, op_replace_all(t) == E_READONLY && st_status_has("read-only") && st_doc_is(t, text), "read-only refused");
  t->doc->set_read_only(false);
  // Replace All in open tabs: asks once, skips the read-only tab
  t2 = st_tab(c, "C:\\st_search2.txt", "alpha alpha\r\n", 13, enc_cp(1252), EOL_CRLF);
  t3 = st_tab(c, "C:\\st_search3.txt", "alpha\r\n", 7, enc_cp(1252), EOL_CRLF);
  if (t2 && t3) {
    t3->doc->set_read_only(true);
    st_set("alpha", "b", SM_NORMAL, 1, 1);
    st_answer(c, Q_REPLACE_ALL_TABS, IDOK);
    r = op_replace_tabs();
    st_checkf(c, r == 5, "replace in tabs: %d replaced", r);
    ST_CHECK(c, st_doc_is(t2, "b b\r\n") && st_doc_is(t3, "alpha\r\n"), "replace in tabs bytes; read-only skipped");
    ST_CHECK(c, st_status_has("skipped"), "replace in tabs reports skipped tabs");
    ST_CHECK(c, t2->doc->undo(&u) == E_OK && st_doc_is(t2, "alpha alpha\r\n"), "one undo group per tab");
    t->doc->undo(&u);
    st_answer(c, Q_REPLACE_ALL_TABS, IDCANCEL);
    ST_CHECK(c, op_replace_tabs() == 0 && st_doc_is(t2, "alpha alpha\r\n"), "replace in tabs cancelled");
  }
  st_close(t3);
  st_close(t2);
  app_activate(app_tab_index(t));
  // Find All -> results panel; opening a hit selects it
  st_set("alpha", 0, SM_NORMAL, 1, 0);
  r = op_find_all(t);
  ST_CHECK(c, r == 4 && results_hit_count() == 4 && results_file_count() == 1, "find all: 4 hits in 1 file");
  if (results_tab()) {
    Document* rd = results_tab()->doc;
    EBytes b;
    b.resize(rd->length());
    rd->read(0, b.data(), rd->length());
    b.push(0);
    {
      const char* hd = "Search \"alpha\" in st_search.txt (4 hits)";
      ST_CHECK(c, strstr((const char*)b.data(), hd) != 0, "find all header");
    }
    ST_CHECK(c, strstr((const char*)b.data(), "\n    2: Gamma alpha\n") != 0, "find all hit line");
    ST_CHECK(c, results_tab()->marks.count() == 4, "find all highlights");
    search_select(t, 0, 0, SCROLL_NONE);
    ST_CHECK(c, results_open_hit(2, 0) == E_OK && st_sel_is(t, 25, 30), "open hit 3 selects 'alphabet'");
  }
  // Ctrl+F3: the word at the caret / the selection
  search_select(t, 6, 10, SCROLL_NONE);              // "beta"
  st_set("zzz", 0, SM_REGEX, 1, 0);
  cmd_search_selnext();
  ST_CHECK(c, g_fs.opts.mode == SM_NORMAL && g_fs.pat.size() == 4 && st_sel_is(t, 6, 10), "Ctrl+F3 uses the selection");
  // hex bytes
  st_set("61 6C ?? 68", 0, SM_HEX, 0, 0);
  search_select(t, 1, 1, SCROLL_NONE);
  op_find(t, 0);
  ST_CHECK(c, st_sel_is(t, 18, 22), "hex bytes with a wildcard");
  st_set("4G", 0, SM_HEX, 0, 0);
  ST_CHECK(c, op_find(t, 0) == E_SYNTAX && g_status_err, "bad hex pattern");
  // incremental search bar
  isearch_show();
  if (g_app.isearch && GetDlgItem(g_app.isearch, IDC_IS_EDIT)) {
    HWND ed = GetDlgItem(g_app.isearch, IDC_IS_EDIT);
    search_select(t, 0, 0, SCROLL_NONE);
    isearch_show();                                  // restart from the caret
    SetWindowTextA(ed, "Gam");
    st_pump(c, 20);
    ST_CHECK(c, st_sel_is(t, 12, 15), "isearch: typing finds 'Gam'");
    SetWindowTextA(ed, "alpha");
    st_pump(c, 20);
    ST_CHECK(c, st_sel_is(t, 0, 5), "isearch: searches from where the bar opened");
    isearch_next(0);
    ST_CHECK(c, st_sel_is(t, 18, 23), "isearch: next");
    isearch_next(1);
    ST_CHECK(c, st_sel_is(t, 0, 5), "isearch: previous");
    SetWindowTextA(ed, "zzz");
    st_pump(c, 20);
    ST_CHECK(c, st_sel_is(t, 0, 5), "isearch: not found keeps the selection");
    isearch_hide();
    ST_CHECK(c, !IsWindowVisible(g_app.isearch), "isearch closed");
  } else {
    ST_CHECK(c, 0, "isearch bar missing");
  }
  results_show(0);
  st_close(t);
}
