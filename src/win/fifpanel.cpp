// fifpanel.cpp - Find Results panel and the result commands (DESIGN 11.6). Owner: T11.
// The panel is a child of the frame (the frame lays it out under the editor with a splitter): a caption strip
// (status text, [Stop], [x]) over a second TextView in panel mode showing a read-only UTF-8 Document:
//   Search "Blt" (17 hits in 6 files of 230 searched)
//   M:\dev\runner\game.cpp (3 hits)
//       120: <line text>
// Hits on the same line share one result line (several highlights). Highlights are RangeMarks (first 10,000 hits).
// F4 / Shift+F4 walk the hits, double-click or Enter opens one.
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "docio.h"
#include "clip.h"
#include "fifworker.h"
#include "fifworker_int.h"
#include "selftest.h"
#include "estr.h"
#include "ids_find.h"

enum { CAP_H = 22, MARK_MAX = 10000 };

struct ResFile { u32 rline; u32 tab_id; u32 change_seq; u32 nhits; u32 path_off; };
struct ResHit  { u32 file; u32 rline; epos rpos; u32 rlen; u32 line, col; epos pos, len; };

static Vec<ResFile> g_files;
static Vec<ResHit> g_hits;
static EBytes g_paths;
static Tab* g_rtab = 0;
static HWND g_panel = 0, g_view = 0, g_label = 0, g_stop = 0, g_close = 0;
static int g_cur = -1;
static u32 g_marked = 0;
static int g_running = 0;
static epos g_last_line_start = 0;     // start of the last appended hit line (merging)
static u32 g_last_prefix = 0;          // bytes of "    120: " on that line
static char g_last_text[1025];         // its hit text

class ResHandler : public PanelHandler {
public:
  void panel_open(epos pos);
  int  panel_key(UINT vk);
};
static ResHandler g_handler;

// ---- the results document ----

static int doc_edit(epos pos, epos del, const char* s, u32 n)
{
  Document* d;
  int rc;
  if (!g_rtab) return E_STATE;
  d = g_rtab->doc;
  d->set_read_only(false);
  rc = d->replace(pos, del, (const u8*)s, n);
  d->set_read_only(true);
  return rc;
}

static int doc_append(const char* s, u32 n)
{
  return doc_edit(g_rtab->doc->length(), 0, s, n);
}

// Replaces the text of one line (no line break inside) and shifts the stored hit offsets after it.
static void rewrite_line(u32 rline, const char* text)
{
  Document* d = g_rtab->doc;
  epos a, b, n = es_len(text);
  u32 i;
  i32 delta;
  if (rline >= d->line_count()) return;
  a = d->line_start(rline);
  b = d->line_end(rline);
  if (doc_edit(a, b - a, text, n) != E_OK) return;
  delta = (i32)n - (i32)(b - a);
  if (delta == 0) return;
  i = g_hits.size();
  while (i-- > 0 && g_hits[i].rpos > a) g_hits[i].rpos = (epos)((i32)g_hits[i].rpos + delta);
  if (g_last_line_start > a) g_last_line_start = (epos)((i32)g_last_line_start + delta);
}

static void ansi_to_utf8(const char* s, char* out, u32 cap)
{
  Vec<ecp> v;
  ui_ansi_to_ecp(s, &v);
  search_ecp_to_utf8(v.data(), v.size(), out, cap);
}

static void file_line_text(const ResFile& f, char* out, u32 cap)
{
  char p[800];
  ansi_to_utf8((const char*)g_paths.data() + f.path_off, p, sizeof(p));
  es_fmt(out, cap, "%s (%u hit%s)", p, f.nhits, f.nhits == 1 ? "" : "s");
}

static void close_file_line()
{
  char line[900];
  if (g_files.empty()) return;
  file_line_text(g_files.back(), line, sizeof(line));
  rewrite_line(g_files.back().rline, line);
}

static void set_label(const char* text)
{
  if (g_label) SetWindowTextA(g_label, text);
}

Tab* results_tab() { return g_rtab; }
u32 results_hit_count() { return g_hits.size(); }
u32 results_file_count() { return g_files.size(); }
int results_current() { return g_cur; }

void results_begin(const char* header)
{
  Document* d;
  EStr s;
  if (!g_rtab) return;
  d = g_rtab->doc;
  s.append(header);
  s.append("\n");
  doc_edit(0, d->length(), s.c_str(), s.len());
  d->clear_undo();
  g_rtab->marks.clear();
  g_files.clear();
  g_hits.clear();
  g_paths.clear();
  g_cur = -1;
  g_marked = 0;
  g_last_line_start = 0;
  g_last_prefix = 0;
  g_last_text[0] = 0;
  if (g_view) textview_set_selection(g_view, 0, 0, SCROLL_MIN);
  g_rtab->sel.anchor = g_rtab->sel.caret = 0;
  set_label("Find Results");
  results_show(1);
}

void results_set_running(int on)
{
  g_running = on;
  if (g_stop) EnableWindow(g_stop, on);
}

int results_add_file(const char* path, u32 tab_id, u32 change_seq)
{
  ResFile f;
  char p[800];
  EStr s;
  if (!g_rtab) return E_STATE;
  close_file_line();
  f.rline = g_rtab->doc->line_count() - 1;
  f.tab_id = tab_id;
  f.change_seq = change_seq;
  f.nhits = 0;
  f.path_off = g_paths.size();
  if (g_paths.append(path, es_len(path) + 1) != E_OK) return E_NOMEM;
  ansi_to_utf8(path, p, sizeof(p));
  s.append(p);
  s.append("\n");
  if (doc_append(s.c_str(), s.len()) != E_OK) return E_NOMEM;
  if (g_files.push(f) != E_OK) return E_NOMEM;
  g_last_text[0] = 0;
  return E_OK;
}

static u32 utf8_bytes_of_chars(const char* s, u32 len, u32 chars)
{
  u32 i = 0, n = 0;
  while (i < len && n < chars) {
    i++;
    while (i < len && ((u8)s[i] & 0xC0) == 0x80) i++;
    n++;
  }
  return i;
}

int results_add_hit(const FifHit& h)
{
  ResHit r;
  u32 off, blen;
  epos line_start;
  if (!g_rtab || g_files.empty()) return E_STATE;
  off = utf8_bytes_of_chars(h.text, h.text_len, h.hl_start);
  blen = utf8_bytes_of_chars(h.text + off, h.text_len - off, h.hl_len);
  if (!g_hits.empty() && g_hits.back().file == g_files.size() - 1 && g_hits.back().line == h.line &&
      g_last_text[0] && strcmp(g_last_text, h.text) == 0) {
    r.rline = g_hits.back().rline;                  // same line, same text: one result line, another highlight
    line_start = g_last_line_start;
  } else {
    char head[24];
    EStr s;
    es_fmt(head, sizeof(head), "    %u: ", h.line + 1);
    s.append(head);
    s.appendn(h.text, h.text_len);
    s.append("\n");
    r.rline = g_rtab->doc->line_count() - 1;
    line_start = g_rtab->doc->length();
    if (doc_append(s.c_str(), s.len()) != E_OK) return E_NOMEM;
    g_last_line_start = line_start;
    g_last_prefix = es_len(head);
    es_copy(g_last_text, sizeof(g_last_text), h.text);
  }
  r.file = g_files.size() - 1;
  r.rpos = line_start + g_last_prefix + off;
  r.rlen = blen;
  r.line = h.line; r.col = h.col; r.pos = h.pos; r.len = h.len;
  if (g_hits.push(r) != E_OK) return E_NOMEM;
  g_files.back().nhits++;
  return E_OK;
}

void results_flush()
{
  if (!g_rtab) return;
  if (g_marked < MARK_MAX && g_hits.size() > g_marked) {
    Vec<MarkRange> v;
    u32 i, n = emin(g_hits.size(), (u32)MARK_MAX);
    if (v.reserve(n) == E_OK) {
      for (i = 0; i < n; i++) {
        MarkRange m;
        if (!g_hits[i].rlen) continue;
        m.pos = g_hits[i].rpos;
        m.len = g_hits[i].rlen;
        v.push(m);
      }
      g_rtab->marks.set(v.data(), v.size());
      g_marked = n;
    }
  }
  g_rtab->doc->clear_undo();
  if (g_view) textview_invalidate(g_view);
}

void results_progress(u32 done, u32 seen, const char* current)
{
  char text[400];
  (void)seen;
  es_fmt(text, sizeof(text), "Searching... %u file%s, %u hit%s: %s", done, done == 1 ? "" : "s", g_hits.size(),
         g_hits.size() == 1 ? "" : "s", current);
  set_label(text);
}

void results_note(const char* text) { set_label(text); }

void results_finish(const char* header)
{
  if (!g_rtab) return;
  close_file_line();
  rewrite_line(0, header);
  results_flush();
  set_label(g_hits.size() ? "Find Results (F4 next, Shift+F4 previous, Enter opens)" :
            strstr(header, ", cancelled") ? "Find Results: cancelled" : "Find Results: nothing found");
}

// ---- opening hits ----

static Tab* find_tab_id(u32 id)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) if (app_tab(i)->id == id) return app_tab(i);
  return 0;
}

static void panel_mark_current(u32 i)
{
  Document* d;
  epos a, b;
  if (!g_rtab || !g_view) return;
  d = g_rtab->doc;
  if (g_hits[i].rline >= d->line_count()) return;
  a = d->line_start(g_hits[i].rline);
  b = d->line_end(g_hits[i].rline);
  textview_set_selection(g_view, b, a, SCROLL_MIN);
  g_rtab->sel.anchor = b;
  g_rtab->sel.caret = a;
}

int results_open_hit(u32 i, int focus_editor)
{
  ResHit h;
  ResFile f;
  Tab* t = 0;
  Document* d;
  epos a, c;
  int idx, exact, rc;
  if (i >= g_hits.size()) return E_RANGE;
  h = g_hits[i];
  f = g_files[h.file];
  if (f.tab_id) {
    t = find_tab_id(f.tab_id);
    if (!t) { app_status_note("The document of this result is closed"); return E_NOTFOUND; }
  } else {
    const char* path = (const char*)g_paths.data() + f.path_off;
    t = tab_find_path(path);
    if (!t) {
      rc = docio_open(path, 0, &t);
      if (rc < 0 || !t) {
        app_status_note("Could not open %s (%s)", path, e_errname(rc < 0 ? rc : E_NOTFOUND));
        return rc < 0 ? rc : E_NOTFOUND;
      }
    }
  }
  idx = app_tab_index(t);
  if (idx >= 0 && idx != app_active_index()) app_activate(idx);
  d = t->doc;
  exact = f.tab_id ? d->change_seq() == f.change_seq : !d->modified();
  if (exact && h.pos <= d->length() && h.len <= d->length() - h.pos) {
    a = h.pos;
    c = h.pos + h.len;
  } else if (h.line < d->line_count()) {          // the document changed: line and column instead
    epos end = d->line_end(h.line);
    u32 k;
    a = d->line_start(h.line);
    for (k = 0; k < h.col && a < end; k++) a = d->char_next(a);
    c = h.len <= d->length() - a ? a + h.len : d->length();
    c = d->snap_char(c);
    if (c < a) c = a;
  } else {
    a = c = d->length();
  }
  search_select(t, a, c, SCROLL_CENTER);
  g_cur = (int)i;
  panel_mark_current(i);
  if (focus_editor) {
    HWND v = t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
    if (v) SetFocus(v);
  }
  return E_OK;
}

// The hit at a position of the results document: the one under pos on a hit line, the first hit of a file line.
static int hit_at(epos pos)
{
  Document* d;
  u32 line, lo = 0, hi, i;
  if (!g_rtab || g_hits.empty()) return -1;
  d = g_rtab->doc;
  line = d->line_of(pos);
  hi = g_hits.size();
  while (lo < hi) { u32 mid = (lo + hi) / 2; if (g_hits[mid].rline < line) lo = mid + 1; else hi = mid; }
  if (lo < g_hits.size() && g_hits[lo].rline == line) {
    for (i = lo; i < g_hits.size() && g_hits[i].rline == line; i++)
      if (pos >= g_hits[i].rpos && pos <= g_hits[i].rpos + g_hits[i].rlen) return (int)i;
    return (int)lo;
  }
  for (i = 0; i < g_files.size(); i++) {
    if (g_files[i].rline == line) {
      u32 k;
      for (k = 0; k < g_hits.size(); k++) if (g_hits[k].file == i) return (int)k;
    }
  }
  return -1;
}

void ResHandler::panel_open(epos pos)
{
  int i = hit_at(pos);
  if (i >= 0) results_open_hit((u32)i, 1);
}

static void copy_lines()
{
  Document* d;
  Selection s;
  epos a, b;
  u32 l0, l1;
  int rc;
  if (!g_rtab) return;
  d = g_rtab->doc;
  a = sel_lo(g_rtab->sel); b = sel_hi(g_rtab->sel);
  l0 = d->line_of(a);
  l1 = d->line_of(b > a ? b - 1 : b);
  s.anchor = d->line_start(l0);
  s.caret = d->line_next(l1);
  rc = clip_copy(g_rtab, s, 0);
  if (rc < 0) app_status_note("Could not copy (%s)", e_errname(rc));
  else app_status_note("Copied %u line%s", l1 - l0 + 1, l1 == l0 ? "" : "s");
}

int ResHandler::panel_key(UINT vk)
{
  if (vk == VK_ESCAPE) {
    if (fif_running()) fif_stop();
    else results_show(0);
    return 1;
  }
  if (vk == VK_RETURN) { panel_open(g_rtab ? g_rtab->sel.caret : 0); return 1; }
  if ((vk == 'C' || vk == VK_INSERT) && GetKeyState(VK_CONTROL) < 0) { copy_lines(); return 1; }
  return 0;
}

// Ctrl+C / Ctrl+Ins / Ctrl+A while the panel has the focus would otherwise reach the main accelerators (they act on
// the active document). main.cpp's app_dispatch should call this before TranslateAccelerator (HANDOFF request).
int results_translate(MSG* m)
{
  if (!g_view || (m->hwnd != g_view && !IsChild(g_panel, m->hwnd))) return 0;
  if (m->message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0) return 0;
  if (m->wParam == 'C' || m->wParam == VK_INSERT) { copy_lines(); return 1; }
  if (m->wParam == 'A' && g_rtab) {
    textview_set_selection(g_view, 0, g_rtab->doc->length(), SCROLL_NONE);
    g_rtab->sel.anchor = 0;
    g_rtab->sel.caret = g_rtab->doc->length();
    return 1;
  }
  return 0;
}

// ---- the panel window ----

static void layout_panel()
{
  RECT r;
  if (!g_panel) return;
  GetClientRect(g_panel, &r);
  if (g_label) MoveWindow(g_label, 6, 4, emax(10, (int)r.right - 6 - 80), CAP_H - 6, TRUE);
  if (g_stop) MoveWindow(g_stop, r.right - 74, 2, 46, CAP_H - 4, TRUE);
  if (g_close) MoveWindow(g_close, r.right - 24, 2, 20, CAP_H - 4, TRUE);
  if (g_view) MoveWindow(g_view, 0, CAP_H, r.right, emax(0, (int)r.bottom - CAP_H), TRUE);
}

static LRESULT CALLBACK panel_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_SIZE:
    layout_panel();
    return 0;
  case WM_COMMAND:
    if (LOWORD(wp) == IDC_RES_STOP) { fif_stop(); return 0; }
    if (LOWORD(wp) == IDC_RES_CLOSE) { results_show(0); return 0; }
    break;
  case WM_SETFOCUS:
    if (g_view) SetFocus(g_view);
    return 0;
  case WM_DESTROY:
    if (g_view) textview_attach(g_view, 0);
    if (g_rtab) { tab_free(g_rtab); g_rtab = 0; }
    g_panel = 0;
    break;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

HWND results_create(HWND parent)
{
  WNDCLASSA wc;
  HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  Document* d;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = panel_proc;
  wc.hInstance = g_app.hinst;
  wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = APP_NAME "Results";
  RegisterClassA(&wc);
  g_panel = CreateWindowExA(0, wc.lpszClassName, "", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 400, 150, parent,
                            (HMENU)ID_RESULTS, g_app.hinst, 0);
  if (!g_panel) return 0;
  g_label = CreateWindowExA(0, "STATIC", "Find Results", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX, 0, 0,
                            10, 10, g_panel, (HMENU)IDC_RES_LABEL, g_app.hinst, 0);
  g_stop = CreateWindowExA(0, "BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 0, 0, 10, 10,
                           g_panel, (HMENU)IDC_RES_STOP, g_app.hinst, 0);
  g_close = CreateWindowExA(0, "BUTTON", "x", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_panel,
                            (HMENU)IDC_RES_CLOSE, g_app.hinst, 0);
  SendMessageA(g_label, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_stop, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_close, WM_SETFONT, (WPARAM)f, 0);
  d = new Document;
  if (d && d->init_empty(enc_utf8(0), EOL_LF) == E_OK) {
    d->set_read_only(true);
    g_rtab = tab_new(d);
    if (!g_rtab) delete d;
  } else if (d) {
    delete d;
  }
  if (g_rtab) {
    g_rtab->lang = lang_results();
    g_rtab->hl->attach(&g_rtab->lines, g_rtab->lang);
    g_rtab->tv.wrap = 0;
    es_copy(g_rtab->path, sizeof(g_rtab->path), "Find Results");
  }
  g_view = textview_create(g_panel, IDC_RES_VIEW, 1);
  if (g_view) {
    textview_set_panel_handler(g_view, &g_handler);
    if (g_rtab) textview_attach(g_view, g_rtab);
  }
  layout_panel();
  g_app.results = g_panel;
  return g_panel;
}

void results_show(int on)
{
  if (!g_panel) return;
  if ((IsWindowVisible(g_panel) ? 1 : 0) == (on ? 1 : 0)) return;
  ShowWindow(g_panel, on ? SW_SHOWNA : SW_HIDE);
  app_layout();
  if (!on) {
    Tab* t = app_active();
    HWND v = t && t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
    HWND f = GetFocus();
    if (v && (f == g_view || f == g_panel || !f)) SetFocus(v);
  }
}

int results_visible() { return g_panel && IsWindowVisible(g_panel); }
int results_is_open() { return results_visible(); }

// ---- commands ----

void cmd_search_fif(void) { finddlg_open(2); }

static void step_result(int dir)
{
  int n = (int)g_hits.size(), i;
  if (n == 0) { app_status_note("No find results"); MessageBeep(MB_OK); return; }
  if (g_cur < 0) i = dir > 0 ? 0 : n - 1;
  else i = g_cur + dir;
  if (i >= n) { i = 0; app_status_note("Wrapped to the first result"); }
  else if (i < 0) { i = n - 1; app_status_note("Wrapped to the last result"); }
  results_open_hit((u32)i, 0);
}

void cmd_search_resnext(void) { step_result(1); }
void cmd_search_resprev(void) { step_result(-1); }
UINT cmdstate_search_results(void) { return g_hits.size() ? 0 : MF_GRAYED; }

// ---------------------------------------------------------------------------------------------------------------
// selftest (st_fif): a fixture tree on the -fixdir drive (C: by default, M: with -fixdir), hit counts and the
// results text, opening hits (selection = the match), F4 / Shift+F4, cancel, a restarted search.

static Tab* st_file_tab(StCtx* c, const char* path, const EncSpec& enc, epos bom)
{
  EBytes b;
  Document* d;
  MemSource* src;
  Tab* t;
  if (st_file_bytes(c, path, &b) != E_OK) { st_checkf(c, 0, "read %s", path); return 0; }
  d = new Document;
  src = new MemSource;
  if (!d || !src || src->copy_from(b.data(), b.size()) != E_OK) { delete src; delete d; ST_CHECK(c, 0, "doc"); return 0; }
  if (d->init_source(src, enc, bom, EOL_CRLF, 0) != E_OK) { delete d; ST_CHECK(c, 0, "init_source"); return 0; }
  t = tab_new(d);
  if (!t) { delete d; return 0; }
  es_copy(t->path, sizeof(t->path), path);
  if (app_add_tab(t, 0) != E_OK) { tab_free(t); return 0; }
  return t;
}

static void st_close_tab(Tab* t)
{
  if (!t || app_tab_index(t) < 0) return;
  app_remove_tab(t);
  tab_free(t);
}

static int st_wait_run(StCtx* c, u32 before, u32 ms)
{
  u32 start = GetTickCount();
  while (fif_runs_finished() == before) {
    st_pump(c, 10);
    if (GetTickCount() - start > ms) return 0;
  }
  return 1;
}

static void st_query(StCtx* c, FifQuery* q, const char* sub, const char* pat)
{
  Vec<ecp> v;
  u32 i;
  memset(q, 0, sizeof(*q));
  ui_path_join(c->fixdir, sub, q->folder, sizeof(q->folder));
  es_copy(q->masks, sizeof(q->masks), "*.*");
  q->subfolders = 1;
  q->opts.mode = SM_NORMAL; q->opts.match_case = 1;
  ui_ansi_to_ecp(pat, &v);
  for (i = 0; i < v.size(); i++) q->pattern[i] = v[i];
  q->pattern_len = v.size();
  q->det.utf8_nobom = 1; q->det.utf16_nobom = 1; q->det.ansi = enc_ansi(); q->det.default_eol = EOL_CRLF;
  q->max_hits = 50000;
}

static int st_results_have(const char* s)
{
  EBytes b;
  Document* d;
  if (!g_rtab) return 0;
  d = g_rtab->doc;
  if (b.resize(d->length() + 1) != E_OK) return 0;
  d->read(0, b.data(), d->length());
  b[d->length()] = 0;
  return strstr((const char*)b.data(), s) != 0;
}

void st_fif(StCtx* c)
{
  char dir[300], path[300], pa[300], pc[300], pu[300], want[400];
  FifQuery* q = (FifQuery*)e_alloc(sizeof(FifQuery));
  Tab* ta;
  Tab* tc;
  Tab* tu;
  u32 before, i;
  int rc;
  if (!q) { ST_CHECK(c, 0, "alloc"); return; }
  ui_path_join(c->fixdir, "fif", dir, sizeof(dir));
  CreateDirectoryA(dir, 0);
  ui_path_join(dir, "sub", path, sizeof(path));
  CreateDirectoryA(path, 0);
  ui_path_join(dir, "many", path, sizeof(path));
  CreateDirectoryA(path, 0);
  st_fixture(c, "fif\\a.cpp", "int foo;\r\nfoo();\r\n", 18, pa, sizeof(pa));
  st_fixture(c, "fif\\b.txt", "nothing\r\n", 9, 0, 0);
  st_fixture(c, "fif\\bin.dat", "\0\1foo", 5, 0, 0);
  st_fixture(c, "fif\\sub\\c.h", "  foo foo\r\n", 11, pc, sizeof(pc));
  st_fixture(c, "fif\\sub\\u.txt", "\xEF\xBB\xBF" "caf\xC3\xA9 foo\n", 13, pu, sizeof(pu));
  // the search
  st_query(c, q, "fif", "foo");
  es_copy(q->masks, sizeof(q->masks), "*.* !\\many");
  before = fif_runs_finished();
  rc = fif_start(*q);
  ST_CHECK(c, rc == E_OK && fif_running(), "fif_start");
  ST_CHECK(c, st_wait_run(c, before, 60000), "search finishes");
  st_checkf(c, fif_last_result() == E_OK, "result %d", fif_last_result());
  st_checkf(c, results_hit_count() == 5 && results_file_count() == 3, "hits %u files %u", results_hit_count(),
            results_file_count());
  ST_CHECK(c, results_visible(), "results panel shown");
  es_copy(want, sizeof(want), "Search \"foo\" (5 hits in 3 files of 5 searched)");
  ST_CHECK(c, st_results_have(want), "header line");
  es_fmt(want, sizeof(want), "%s (2 hits)\n    1: int foo;\n    2: foo();\n", pa);
  ST_CHECK(c, st_results_have(want), "a.cpp block");
  es_fmt(want, sizeof(want), "%s (2 hits)\n    1:   foo foo\n", pc);
  ST_CHECK(c, st_results_have(want), "two hits on one line share a result line");
  ST_CHECK(c, st_results_have("    1: caf\xC3\xA9 foo\n"), "UTF-8 line text");
  ST_CHECK(c, g_rtab && g_rtab->marks.count() == 5, "5 highlights");
  ST_CHECK(c, cmdstate_search_results() == 0, "result commands enabled");
  // opening hits: open tabs (independent of docio_open)
  ta = st_file_tab(c, pa, enc_ansi(), 0);
  tc = st_file_tab(c, pc, enc_ansi(), 0);
  tu = st_file_tab(c, pu, enc_utf8(1), 3);
  if (ta && tc && tu) {
    ST_CHECK(c, results_open_hit(1, 0) == E_OK && app_active() == ta && ta->sel.anchor == 10 && ta->sel.caret == 13,
             "open hit 2: a.cpp 10-13");
    cmd_search_resnext();
    ST_CHECK(c, app_active() == tc && tc->sel.anchor == 2 && tc->sel.caret == 5, "F4: c.h first foo");
    cmd_search_resnext();
    ST_CHECK(c, app_active() == tc && tc->sel.anchor == 6 && tc->sel.caret == 9, "F4: c.h second foo");
    cmd_search_resnext();
    ST_CHECK(c, app_active() == tu && tu->sel.anchor == 9 && tu->sel.caret == 12, "F4: u.txt after the BOM");
    cmd_search_resnext();
    ST_CHECK(c, app_active() == ta && ta->sel.anchor == 4 && results_current() == 0, "F4 wraps to the first hit");
    cmd_search_resprev();
    ST_CHECK(c, app_active() == tu && results_current() == 4, "Shift+F4 wraps to the last hit");
    // a modified document: line and column are used instead of the offset
    {
      Selection s0;
      s0.anchor = s0.caret = 0;
      ta->doc->begin_edit(UK_OTHER, s0, 0);
      ta->doc->replace(0, 0, (const u8*)"// x\r\n", 6);
      ta->doc->end_edit(s0);
      ST_CHECK(c, results_open_hit(1, 0) == E_OK && ta->sel.anchor == 6 && ta->sel.caret == 9,
               "modified document: line 2, column 1 (not the stale offset)");
    }
    // double-click on a file line opens its first hit
    if (g_rtab) {
      g_handler.panel_open(g_rtab->doc->line_start(g_files[1].rline));
      ST_CHECK(c, app_active() == tc && results_current() == 2, "file line opens its first hit");
    }
  }
  st_close_tab(tu);
  st_close_tab(tc);
  st_close_tab(ta);
  // a closed file opens through docio_open (when T09's docio is there)
  rc = results_open_hit(4, 0);
  if (rc == E_NOTIMPL) selftest_log("  note: docio_open not implemented yet; open-from-disk not checked");
  else {
    Tab* t = tab_find_path(pu);
    ST_CHECK(c, rc == E_OK && t && t->sel.anchor == 9 && t->sel.caret == 12, "open a closed file at the hit");
    if (t) docio_close(t, 0);
  }
  // cancel: a tree of 150 files, stopped at once
  for (i = 0; i < 150; i++) {
    char name[40], body[64];
    es_fmt(name, sizeof(name), "fif\\many\\f%03u.txt", i);
    es_fmt(body, sizeof(body), "line one\r\nfoo %u\r\n", i);
    st_fixture(c, name, body, es_len(body), 0, 0);
  }
  st_query(c, q, "fif", "foo");
  before = fif_runs_finished();
  fif_start(*q);
  fif_stop();
  ST_CHECK(c, st_wait_run(c, before, 30000), "cancelled search finishes");
  st_checkf(c, fif_last_result() == E_CANCEL, "cancel result %d", fif_last_result());
  ST_CHECK(c, st_results_have("cancelled"), "header says cancelled");
  ST_CHECK(c, !fif_running(), "not running after cancel");
  // a new search replaces a running one; only its results arrive
  st_query(c, q, "fif", "foo");
  fif_start(*q);
  st_query(c, q, "fif\\many", "line one");
  before = fif_runs_finished();
  fif_start(*q);
  ST_CHECK(c, st_wait_run(c, before, 60000), "second search finishes");
  st_checkf(c, results_hit_count() == 150 && results_file_count() == 150 && fif_last_result() == E_OK,
            "restarted search: %u hits", results_hit_count());
  es_copy(want, sizeof(want), "Search \"line one\" (150 hits in 150 files of 150 searched)");
  ST_CHECK(c, st_results_have(want), "restarted header");
  // missing folder
  st_query(c, q, "fif\\nope", "foo");
  before = fif_runs_finished();
  fif_start(*q);
  ST_CHECK(c, st_wait_run(c, before, 10000) && fif_last_result() == E_NOTFOUND && st_results_have("folder not found"),
           "missing folder reported");
  results_show(0);
  ST_CHECK(c, !results_visible(), "panel closed");
  e_free(q);
}
