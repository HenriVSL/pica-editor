// isearch.cpp - incremental search bar, Ctrl+I (DESIGN 11.5). Owner: T11.
// A strip above the status bar: "Find: [______] 3/17 [ ] Match case [ ] Highlight all [x]". Typing searches
// literally from where the bar opened (wrapping); Enter/F3 next, Shift+Enter/Shift+F3 previous, Esc closes and keeps
// the selection, Ctrl+I again = next. Not found: pink field + beep. Count and Highlight all for documents <= 1 MB.
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "render.h"
#include "fifworker.h"
#include "fifworker_int.h"
#include "estr.h"
#include "ids_find.h"

enum { BAR_H = 26, SMALL_DOC = 1u << 20, HL_MAX = 10000 };

static HWND g_bar = 0, g_edit = 0, g_label = 0, g_count = 0, g_case = 0, g_hl = 0, g_close = 0;
static WNDPROC g_edit_proc = 0;
static u32 g_tab_id = 0;
static epos g_start = 0;
static int g_fail = 0;
static u32 g_marks_tab = 0;          // tab whose RangeMarks the bar set (Highlight all), 0 = none
static HBRUSH g_fail_brush = 0;
static COLORREF g_fail_rgb = 0;

static Tab* bar_tab()
{
  return app_active();
}

static Tab* tab_by_id(u32 id)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) if (app_tab(i)->id == id) return app_tab(i);
  return 0;
}

static void clear_marks()
{
  Tab* t;
  if (!g_marks_tab) return;
  t = tab_by_id(g_marks_tab);
  g_marks_tab = 0;
  if (!t) return;
  t->marks.clear();
  if (t == app_active()) {
    if (t->view == VIEW_HEX) { if (g_app.hexview) hexview_invalidate(g_app.hexview); }
    else if (g_app.textview) textview_invalidate(g_app.textview);
  }
}

static void set_fail(int on)
{
  if (on == g_fail) return;
  g_fail = on;
  if (g_edit) InvalidateRect(g_edit, 0, TRUE);
}

static int compile_bar(Tab* t, Searcher* s, Vec<ecp>* pat)
{
  char buf[RX_MAX_PATTERN + 1];
  SearchOpts o;
  SearchError err;
  GetWindowTextA(g_edit, buf, sizeof(buf));
  pat->clear();
  ui_ansi_to_ecp(buf, pat);
  if (pat->empty()) return E_SYNTAX;
  o.mode = SM_NORMAL;
  o.match_case = (u8)(SendMessageA(g_case, BM_GETCHECK, 0, 0) == BST_CHECKED);
  o.whole_word = 0;
  o.dot_all = 0;
  return s->compile(pat->data(), pat->size(), o, t->doc->enc(), &err);
}

// Count "i/N" and Highlight all (small documents only).
static void update_count(Tab* t, Searcher* s, epos cur_pos, int found)
{
  Vec<MarkRange> v;
  char text[40];
  u32 i, k = 0, idx = 0;
  int hl = SendMessageA(g_hl, BM_GETCHECK, 0, 0) == BST_CHECKED;
  epos lo = t->doc->text_origin(), hi = t->doc->length();
  text[0] = 0;
  if (hi - lo <= SMALL_DOC && s->find_all(t->doc->bytes(), t->doc->codec(), lo, hi, &v, 200000, 0) >= 0) {
    for (i = 0; i < v.size(); i++) if (v[i].len) v[k++] = v[i];
    v.resize(k);
    for (i = 0; i < k; i++) if (v[i].pos == cur_pos) { idx = i + 1; break; }
    if (found && idx) es_fmt(text, sizeof(text), "%u/%u", idx, k);
    else es_fmt(text, sizeof(text), "%u found", k);
    if (hl) {
      t->marks.set(v.data(), k > HL_MAX ? (u32)HL_MAX : k);
      g_marks_tab = t->id;
      if (t->view == VIEW_HEX) { if (g_app.hexview) hexview_invalidate(g_app.hexview); }
      else if (g_app.textview) textview_invalidate(g_app.textview);
    }
  }
  if (!hl) clear_marks();
  SetWindowTextA(g_count, text);
}

// Search from 'from' (forward) or before it (backward), wrapping once.
static int bar_find(Tab* t, epos from, int backward, int count)
{
  Searcher s;
  Vec<ecp> pat;
  RxMatch m;
  epos lo, hi;
  int r;
  if (!t || !t->doc) return 0;
  if (compile_bar(t, &s, &pat) != E_OK) {
    set_fail(0);
    SetWindowTextA(g_count, "");
    clear_marks();
    search_select(t, g_start, g_start, SCROLL_MIN);
    return 0;
  }
  lo = t->doc->text_origin();
  hi = t->doc->length();
  if (from < lo) from = lo;
  if (from > hi) from = hi;
  r = s.find(t->doc->bytes(), t->doc->codec(), lo, hi, from, backward, &m, 0);
  if (r == 0) r = s.find(t->doc->bytes(), t->doc->codec(), lo, hi, backward ? hi : lo, backward, &m, 0);
  if (r == 1) {
    search_select(t, m.pos, m.pos + m.len, SCROLL_CENTER);
    set_fail(0);
  } else {
    set_fail(1);
    MessageBeep(MB_OK);
  }
  if (count) update_count(t, &s, r == 1 ? m.pos : EPOS_NONE, r == 1);
  return r == 1;
}

static void bar_changed()
{
  Tab* t = bar_tab();
  if (!t) return;
  if (t->id != g_tab_id) { g_tab_id = t->id; g_start = sel_lo(t->sel); }
  bar_find(t, g_start, 0, 1);
}

void isearch_next(int backward)
{
  Tab* t = bar_tab();
  epos from;
  if (!t || !g_edit) return;
  if (t->id != g_tab_id) { g_tab_id = t->id; g_start = sel_lo(t->sel); }
  if (GetWindowTextLengthA(g_edit) == 0) return;
  if (backward) from = sel_lo(t->sel);
  else from = sel_empty(t->sel) ? t->doc->char_next(t->sel.caret) : sel_hi(t->sel);
  bar_find(t, from, backward, 1);
}

int isearch_focused()
{
  HWND f = GetFocus();
  return g_bar && IsWindowVisible(g_bar) && f && (f == g_edit || IsChild(g_bar, f));
}

static LRESULT CALLBACK edit_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  if (msg == WM_KEYDOWN) {
    if (wp == VK_RETURN) { isearch_next(GetKeyState(VK_SHIFT) < 0); return 0; }
    if (wp == VK_ESCAPE) { isearch_hide(); return 0; }
  }
  if (msg == WM_SYSKEYDOWN && (wp == 'C' || wp == 'H')) {   // Alt+C / Alt+H: the bar's check boxes (not a dialog)
    HWND b = wp == 'C' ? g_case : g_hl;
    SendMessageA(b, BM_SETCHECK, SendMessageA(b, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageA(g_bar, WM_COMMAND, MAKEWPARAM(wp == 'C' ? IDC_IS_CASE : IDC_IS_HIGHLIGHT, BN_CLICKED), (LPARAM)b);
    return 0;
  }
  if (msg == WM_SYSCHAR && (wp == 'c' || wp == 'C' || wp == 'h' || wp == 'H')) return 0;   // no menu, no beep
  if (msg == WM_CHAR && (wp == '\r' || wp == 27 || wp == '\n')) return 0;   // no beep
  return CallWindowProcA(g_edit_proc, w, msg, wp, lp);
}

// Editing keys typed into the bar's edit must not reach the main accelerators (Ctrl+V would paste into the
// document). main.cpp's app_dispatch should call this before TranslateAccelerator(IDA_MAIN) (HANDOFF request).
int isearch_translate(MSG* m)
{
  if (!g_edit || m->hwnd != g_edit) return 0;
  if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return 0;
  if (GetKeyState(VK_CONTROL) < 0) {
    switch (m->wParam) {
    case 'C': case 'V': case 'X': case 'Z': case 'A': case VK_INSERT: case VK_BACK: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_DELETE:
      if (m->wParam == 'A') { SendMessageA(g_edit, EM_SETSEL, 0, -1); return 1; }
      TranslateMessage(m);
      DispatchMessageA(m);
      return 1;
    default: return 0;
    }
  }
  if (GetKeyState(VK_SHIFT) < 0 && (m->wParam == VK_INSERT || m->wParam == VK_DELETE)) {
    TranslateMessage(m);
    DispatchMessageA(m);
    return 1;
  }
  return 0;
}

static void layout_bar()
{
  RECT r;
  int w, x = 6, h, y;
  if (!g_bar) return;
  GetClientRect(g_bar, &r);
  w = r.right;
  h = 20;
  y = (r.bottom - h) / 2;
  MoveWindow(g_label, x, y + 3, 32, h - 4, TRUE); x += 34;
  MoveWindow(g_edit, x, y, 200, h, TRUE); x += 206;
  MoveWindow(g_count, x, y + 3, 70, h - 4, TRUE); x += 74;
  MoveWindow(g_case, x, y, 84, h, TRUE); x += 88;
  MoveWindow(g_hl, x, y, 90, h, TRUE);
  MoveWindow(g_close, w - 24, y, 20, h, TRUE);
}

static LRESULT CALLBACK bar_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_SIZE:
    layout_bar();
    return 0;
  case WM_COMMAND:
    if (LOWORD(wp) == IDC_IS_EDIT && HIWORD(wp) == EN_CHANGE) { bar_changed(); return 0; }
    if (LOWORD(wp) == IDC_IS_CLOSE) { isearch_hide(); return 0; }
    if (LOWORD(wp) == IDC_IS_CASE || LOWORD(wp) == IDC_IS_HIGHLIGHT) {
      Tab* t = bar_tab();
      if (t && GetWindowTextLengthA(g_edit) > 0) bar_find(t, sel_lo(t->sel), 0, 1);
      else if (LOWORD(wp) == IDC_IS_HIGHLIGHT) clear_marks();
      SetFocus(g_edit);
      return 0;
    }
    break;
  case WM_CTLCOLOREDIT:
    if ((HWND)lp == g_edit && g_fail) {
      u32 cfgc = g_app.cfg.ui[UIC_ISEARCH_FAIL];
      COLORREF c = cfgc == COLOR_SYSDEFAULT ? render_ui(UIC_ISEARCH_FAIL) : (COLORREF)cfgc;
      if (!g_fail_brush || c != g_fail_rgb) {
        if (g_fail_brush) DeleteObject(g_fail_brush);
        g_fail_brush = CreateSolidBrush(c);
        g_fail_rgb = c;
      }
      SetBkColor((HDC)wp, c);
      SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
      return (LRESULT)g_fail_brush;
    }
    break;
  case WM_DESTROY:
    if (g_fail_brush) { DeleteObject(g_fail_brush); g_fail_brush = 0; }
    g_bar = 0;
    break;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

HWND isearch_create(HWND parent)
{
  WNDCLASSA wc;
  HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = bar_proc;
  wc.hInstance = g_app.hinst;
  wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = APP_NAME "ISearch";
  RegisterClassA(&wc);
  g_bar = CreateWindowExA(0, wc.lpszClassName, "", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 600, BAR_H, parent,
                          (HMENU)ID_ISEARCH, g_app.hinst, 0);
  if (!g_bar) return 0;
  g_label = CreateWindowExA(0, "STATIC", "Find:", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, g_bar,
                            (HMENU)IDC_IS_LABEL, g_app.hinst, 0);
  g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10,
                           10, g_bar, (HMENU)IDC_IS_EDIT, g_app.hinst, 0);
  g_count = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, g_bar,
                            (HMENU)IDC_IS_COUNT, g_app.hinst, 0);
  g_case = CreateWindowExA(0, "BUTTON", "Match &case", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10,
                           10, g_bar, (HMENU)IDC_IS_CASE, g_app.hinst, 0);
  g_hl = CreateWindowExA(0, "BUTTON", "&Highlight all", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10,
                         10, g_bar, (HMENU)IDC_IS_HIGHLIGHT, g_app.hinst, 0);
  g_close = CreateWindowExA(0, "BUTTON", "x", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_bar,
                            (HMENU)IDC_IS_CLOSE, g_app.hinst, 0);
  SendMessageA(g_label, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_edit, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_count, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_case, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_hl, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_close, WM_SETFONT, (WPARAM)f, 0);
  SendMessageA(g_edit, EM_LIMITTEXT, 255, 0);
  g_edit_proc = (WNDPROC)SetWindowLongA(g_edit, GWL_WNDPROC, (LONG)edit_proc);
  layout_bar();
  g_app.isearch = g_bar;
  return g_bar;
}

void isearch_show()
{
  Tab* t = bar_tab();
  if (!g_bar) return;
  if (!IsWindowVisible(g_bar)) {
    ShowWindow(g_bar, SW_SHOWNA);
    app_layout();
  }
  if (t) { g_tab_id = t->id; g_start = sel_lo(t->sel); }
  set_fail(0);
  SetWindowTextA(g_count, "");
  SetFocus(g_edit);
  SendMessageA(g_edit, EM_SETSEL, 0, -1);
}

void isearch_hide()
{
  Tab* t = bar_tab();
  HWND v;
  if (!g_bar || !IsWindowVisible(g_bar)) return;
  clear_marks();
  ShowWindow(g_bar, SW_HIDE);
  app_layout();
  v = t && t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
  if (v && IsWindowVisible(v)) SetFocus(v);
  else if (g_app.frame) SetFocus(g_app.frame);
}

void cmd_search_isearch(void)
{
  if (!app_active()) return;
  if (isearch_focused()) { isearch_next(0); return; }
  isearch_show();
}
