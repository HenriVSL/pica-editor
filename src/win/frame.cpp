// frame.cpp - the main window: class, creation, toolbar, child layout with the results splitter, WM_* routing,
// title, menu lists (recent files, Window), exit flow, window placement, frame commands and st_frame
// (DESIGN 15.2, 15.4, 15.6, 15.11). Owner: T07.
#include <windows.h>
#include <commctrl.h>
#include <string.h>
#include "app.h"
#include "render_int.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "docio.h"
#include "sessmgr.h"
#include "watch.h"
#include "largefile.h"
#include "iothread.h"
#include "fifworker.h"
#include "notifybar.h"
#include "progress.h"
#include "selftest.h"
#include "winenc.h"
#include "frame_int.h"
#include "config.h"
#include "lexer.h"
#include "estr.h"
#include "resource.h"

static LRESULT CALLBACK frame_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp);

static int g_ui_dirty = 1;
static Tab* g_ctx = 0;                       // tab of the tab-bar context menu command
static RECT g_split;                         // results splitter (client coordinates), empty when hidden
static int g_split_drag = 0, g_split_grab = 0;
static char g_title[400];
static int g_top_h = 0;                      // height of the toolbar/tab bar/notification band (button face)

// ---- toolbar (15.6) ----

struct TbDef { int id; int image; int check; };
static const TbDef k_tb[] = {
  { IDM_FILE_NEW, 0, 0 }, { IDM_FILE_OPEN, 1, 0 }, { IDM_FILE_SAVE, 2, 0 }, { IDM_FILE_SAVEALL, 3, 0 }, { 0, 0, 0 },
  { IDM_EDIT_CUT, 4, 0 }, { IDM_EDIT_COPY, 5, 0 }, { IDM_EDIT_PASTE, 6, 0 }, { 0, 0, 0 },
  { IDM_EDIT_UNDO, 7, 0 }, { IDM_EDIT_REDO, 8, 0 }, { 0, 0, 0 },
  { IDM_SEARCH_FIND, 9, 0 }, { IDM_SEARCH_REPLACE, 10, 0 }, { IDM_SEARCH_FIF, 11, 0 }, { 0, 0, 0 },
  { IDM_VIEW_HEX, 12, 1 }, { 0, 0, 0 },
  { IDM_VIEW_WRAP, 13, 1 }, { IDM_VIEW_SHOWWS, 14, 1 }
};
enum { TB_COUNT = sizeof(k_tb) / sizeof(k_tb[0]) };

static WNDPROC g_tb_old = 0;

// TBSTYLE_FLAT toolbars are transparent: give ours a solid button-face background (no rebar behind it).
static LRESULT CALLBACK toolbar_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  if (msg == WM_ERASEBKGND) {
    RECT rc;
    GetClientRect(w, &rc);
    FillRect((HDC)wp, &rc, GetSysColorBrush(COLOR_BTNFACE));
    return 1;
  }
  return CallWindowProcA(g_tb_old, w, msg, wp, lp);
}

static HWND toolbar_create(HWND parent)
{
  TBBUTTON b[TB_COUNT];
  HIMAGELIST il;
  HBITMAP bmp;
  u32 i;
  HWND tb = CreateWindowExA(0, TOOLBARCLASSNAMEA, "",
                            WS_CHILD | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP |
                            (g_app.cfg.win_toolbar ? WS_VISIBLE : 0),
                            0, 0, 0, 0, parent, (HMENU)ID_TOOLBAR, g_app.hinst, 0);
  if (!tb) return 0;
  SendMessageA(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  il = ImageList_Create(16, 16, ILC_COLORDDB | ILC_MASK, TOOLBAR_BUTTONS, 0);
  bmp = (HBITMAP)gdi_track(LoadBitmapA(g_app.hinst, MAKEINTRESOURCEA(IDB_TOOLBAR)));
  if (il && bmp) ImageList_AddMasked(il, bmp, RGB(255, 0, 255));
  gdi_delete(bmp);
  if (il) SendMessageA(tb, TB_SETIMAGELIST, 0, (LPARAM)il);
  memset(b, 0, sizeof(b));
  for (i = 0; i < TB_COUNT; i++) {
    if (!k_tb[i].id) { b[i].fsStyle = TBSTYLE_SEP; continue; }
    b[i].iBitmap = k_tb[i].image;
    b[i].idCommand = k_tb[i].id;
    b[i].fsState = TBSTATE_ENABLED;
    b[i].fsStyle = (BYTE)(k_tb[i].check ? TBSTYLE_CHECK : TBSTYLE_BUTTON);
    b[i].iString = -1;
  }
  SendMessageA(tb, TB_ADDBUTTONS, TB_COUNT, (LPARAM)b);
  SendMessageA(tb, TB_AUTOSIZE, 0, 0);
  g_tb_old = (WNDPROC)SetWindowLongA(tb, GWL_WNDPROC, (LONG)toolbar_proc);
  g_app.toolbar = tb;
  return tb;
}

void frame_toolbar_update()
{
  u32 i;
  if (!g_app.toolbar) return;
  for (i = 0; i < TB_COUNT; i++) {
    int id = k_tb[i].id, st, en, ck;
    UINT s;
    if (!id) continue;
    s = cmd_state(id);
    st = (int)SendMessageA(g_app.toolbar, TB_GETSTATE, (WPARAM)id, 0);
    if (st < 0) continue;
    en = (s & MF_GRAYED) ? 0 : 1;
    ck = (s & MF_CHECKED) ? 1 : 0;
    if (((st & TBSTATE_ENABLED) ? 1 : 0) != en) SendMessageA(g_app.toolbar, TB_ENABLEBUTTON, (WPARAM)id, MAKELONG(en, 0));
    if (k_tb[i].check && ((st & TBSTATE_CHECKED) ? 1 : 0) != ck)
      SendMessageA(g_app.toolbar, TB_CHECKBUTTON, (WPARAM)id, MAKELONG(ck, 0));
  }
}

int frame_toolbar_state(int id)
{
  if (!g_app.toolbar) return -1;
  return (int)SendMessageA(g_app.toolbar, TB_GETSTATE, (WPARAM)id, 0);
}

// ---- deferred UI refresh ----

void frame_ui_dirty()
{
  g_ui_dirty = 1;
}

void frame_idle()
{
  tabs_idle();
  if (!g_ui_dirty) return;
  g_ui_dirty = 0;
  frame_toolbar_update();
  app_update_title();
}

void frame_ctx_tab(Tab* t)
{
  g_ctx = t;
}

static Tab* ctx_tab()
{
  if (g_ctx && app_tab_index(g_ctx) >= 0) return g_ctx;
  return app_active();
}

// ---- window class and creation ----

int frame_register(HINSTANCE h)
{
  WNDCLASSA wc;
  wc.style = 0;
  wc.lpfnWndProc = frame_proc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = h;
  wc.hIcon = LoadIconA(h, MAKEINTRESOURCEA(IDI_APP));
  wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
  wc.hbrBackground = 0;                        // WM_ERASEBKGND paints the empty area and the splitter
  wc.lpszMenuName = MAKEINTRESOURCEA(IDR_MENU);
  wc.lpszClassName = APP_CLASS_FRAME;
  return RegisterClassA(&wc) != 0;
}

HWND frame_create(int show_cmd)
{
  Config* c = &g_app.cfg;
  int x = c->win_x, y = c->win_y, cw = c->win_w, ch = c->win_h;
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  if (cw < 320 || ch < 200 || cw > sw + 64 || ch > sh + 64) { cw = 760; ch = 540; }
  if (x < -32 || y < -32 || x > sw - 64 || y > sh - 64) { x = 40; y = 40; }
  g_app.accel = LoadAcceleratorsA(g_app.hinst, MAKEINTRESOURCEA(IDA_MAIN));
  g_app.accel_find = LoadAcceleratorsA(g_app.hinst, MAKEINTRESOURCEA(IDA_FIND));
  g_app.frame = CreateWindowExA(WS_EX_ACCEPTFILES, APP_CLASS_FRAME, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, cw, ch, 0, 0, g_app.hinst, 0);
  if (!g_app.frame) return 0;
  if (g_app.selftest || g_app.bench) {          // visible window (hidden windows make GDI free: fake numbers)
    ShowWindow(g_app.frame, SW_SHOWNORMAL);
    ShowWindow(g_app.frame, SW_SHOWNORMAL);
  } else if (c->win_max && (show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOWDEFAULT || show_cmd == SW_SHOW)) {
    ShowWindow(g_app.frame, SW_SHOWMAXIMIZED);
  } else {
    ShowWindow(g_app.frame, show_cmd);
  }
  app_layout();
  UpdateWindow(g_app.frame);
  app_update_title();
  app_update_status();
  return g_app.frame;
}

// ---- layout (15.2) ----

static int win_h(HWND w)
{
  RECT r;
  if (!w || !GetWindowRect(w, &r)) return 0;
  return r.bottom - r.top;
}

void frame_splitter_rect(RECT* r)
{
  *r = g_split;
}

void app_layout()
{
  RECT rc, old_split = g_split;
  int y = 0, bottom, w, h;
  Tab* t = app_active();
  HWND show = 0;
  if (!g_app.frame || IsIconic(g_app.frame)) return;
  GetClientRect(g_app.frame, &rc);
  w = rc.right;
  bottom = rc.bottom;
  if (g_app.toolbar && g_app.cfg.win_toolbar) {
    SendMessageA(g_app.toolbar, TB_AUTOSIZE, 0, 0);
    y += win_h(g_app.toolbar);
  }
  h = tabbar_height();
  if (g_app.tabbar) {
    if (h) SetWindowPos(g_app.tabbar, 0, 0, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else ShowWindow(g_app.tabbar, SW_HIDE);
  }
  y += h;
  h = notifybar_height();
  if (h && g_app.notifybar) {
    SetWindowPos(g_app.notifybar, 0, 0, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    y += h;
  }
  if (g_app.statusbar && g_app.cfg.win_statusbar) {
    SendMessageA(g_app.statusbar, WM_SIZE, 0, 0);
    bottom -= win_h(g_app.statusbar);
    statusbar_layout();
  }
  SetRectEmpty(&g_split);
  if (g_app.results && results_visible()) {
    int rh = g_app.cfg.win_results_h, room = bottom - y - FRAME_SPLIT_H - 40;
    if (rh > room) rh = room;
    if (rh < 40) rh = 40;
    SetWindowPos(g_app.results, 0, 0, bottom - rh, w, rh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetRect(&g_split, 0, bottom - rh - FRAME_SPLIT_H, w, bottom - rh);
    bottom -= rh + FRAME_SPLIT_H;
  }
  if (g_app.isearch && IsWindowVisible(g_app.isearch)) {
    int ih = win_h(g_app.isearch);
    SetWindowPos(g_app.isearch, 0, 0, bottom - ih, w, ih, SWP_NOZORDER | SWP_NOACTIVATE);
    bottom -= ih;
  }
  if (y != g_top_h) {
    RECT band;
    SetRect(&band, 0, 0, w, y > g_top_h ? y : g_top_h);
    g_top_h = y;
    InvalidateRect(g_app.frame, &band, TRUE);
  }
  if (bottom < y) bottom = y;
  if (t) show = t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
  if (g_app.textview && g_app.textview != show) ShowWindow(g_app.textview, SW_HIDE);
  if (g_app.hexview && g_app.hexview != show) ShowWindow(g_app.hexview, SW_HIDE);
  if (show) SetWindowPos(show, 0, 0, y, w, bottom - y, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  if (!EqualRect(&old_split, &g_split)) {
    InvalidateRect(g_app.frame, &old_split, TRUE);
    InvalidateRect(g_app.frame, &g_split, TRUE);
  }
  if (!show) InvalidateRect(g_app.frame, 0, TRUE);       // the empty workspace
}

void app_update_title()
{
  char title[400], name[280];
  Tab* t = app_active();
  if (!g_app.frame) return;
  if (!t) {
    es_copy(title, sizeof(title), APP_NAME);
  } else {
    tab_title(t, name, sizeof(name));
    es_fmt(title, sizeof(title), "%s%s - %s%s", name, t->doc && t->doc->modified() ? " *" : "", APP_NAME,
           t->doc && t->doc->read_only() ? " [Read-only]" : "");
  }
  if (strcmp(title, g_title) == 0) return;
  es_copy(g_title, sizeof(g_title), title);
  SetWindowTextA(g_app.frame, title);
}

void app_apply_config()
{
  if (g_app.textview) textview_config_changed(g_app.textview);
  if (g_app.hexview) hexview_config_changed(g_app.hexview);
  if (g_app.toolbar) ShowWindow(g_app.toolbar, g_app.cfg.win_toolbar ? SW_SHOWNA : SW_HIDE);
  if (g_app.statusbar) ShowWindow(g_app.statusbar, g_app.cfg.win_statusbar ? SW_SHOWNA : SW_HIDE);
  if (g_app.frame) {
    int s = g_app.cfg.poll_seconds;
    if (s < 1) s = 1;
    if (s > 60) s = 60;
    SetTimer(g_app.frame, TIMER_POLL, (UINT)s * 1000, 0);
  }
  app_layout();
  app_update_status();
  frame_ui_dirty();
}

// ---- menus: recent files and the Window list (rebuilt on WM_INITMENUPOPUP) ----

static int menu_pos_of(HMENU m, int id)
{
  int i, n = GetMenuItemCount(m);
  for (i = 0; i < n; i++) if (GetMenuItemID(m, i) == (UINT)id) return i;
  return -1;
}

static void menu_escape(const char* in, char* out, u32 cap)
{
  u32 k = 0;
  while (*in && k + 2 < cap) {
    if (*in == '&') out[k++] = '&';
    out[k++] = *in++;
  }
  out[k] = 0;
}

static void compact_path(const char* path, char* out, u32 cap, u32 max)
{
  const char* name = ui_path_name(path);
  const char* p;
  u32 n = es_len(path), head;
  if (n <= max) { es_copy(out, cap, path); return; }
  head = 0;                                   // keep "C:\first\" (or "\\server\share\") then "...\" + the tail
  for (p = path; *p; p++) {
    if (*p == '\\' && p > path + 2) { head = (u32)(p - path) + 1; break; }
  }
  if (head && head + 4 + es_len(name) <= max) {
    char tail[260];
    const char* t = path + n;
    u32 room = max - head - 4;
    while (t > path + head && (u32)(path + n - t) < room) t--;
    while (*t && *t != '\\') t++;               // start the tail at a folder boundary
    if (!*t) t = name - 1;
    es_copy(tail, sizeof(tail), t);
    es_copy(out, cap, path);
    if (head < cap) out[head] = 0;
    es_cat(out, cap, "...");
    es_cat(out, cap, tail);
    return;
  }
  es_copy(out, cap, "...\\");
  es_cat(out, cap, name);
}

static void rebuild_mru(HMENU m)
{
  int i, pos, n = 0;
  for (i = IDM_MRU_FIRST; i <= IDM_MRU_LAST; i++) while (DeleteMenu(m, (UINT)i, MF_BYCOMMAND)) {}
  pos = menu_pos_of(m, IDM_MRU_CLEAR);
  if (pos < 0) return;
  for (i = 0; i < CFG_MRU_MAX && i < g_app.cfg.mru_count; i++) {
    char shortp[300], esc[600], text[620];
    if (!g_app.cfg.mru[i][0]) continue;
    compact_path(g_app.cfg.mru[i], shortp, sizeof(shortp), 60);
    menu_escape(shortp, esc, sizeof(esc));
    if (n < 9) es_fmt(text, sizeof(text), "&%d %s", n + 1, esc);
    else es_fmt(text, sizeof(text), "1&0 %s", esc);
    InsertMenuA(m, (UINT)(pos + n), MF_BYPOSITION | MF_STRING, (UINT)(IDM_MRU_FIRST + i), text);
    n++;
  }
  if (!n) InsertMenuA(m, (UINT)pos, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_MRU_FIRST, "(no recent files)");
}

static int mru_shown()
{
  int i, n = 0;
  for (i = 0; i < CFG_MRU_MAX && i < g_app.cfg.mru_count; i++) if (g_app.cfg.mru[i][0]) n++;
  return n;
}

static void rebuild_winlist(HMENU m)
{
  int i, n = app_tab_count();
  for (i = IDM_WIN_FIRST; i <= IDM_WIN_LAST; i++) while (DeleteMenu(m, (UINT)i, MF_BYCOMMAND)) {}
  for (i = 0; i < n && i <= IDM_WIN_LAST - IDM_WIN_FIRST; i++) {
    char label[400], esc[800], text[820];
    tab_label(app_tab(i), label, sizeof(label));
    menu_escape(label, esc, sizeof(esc));
    if (i < 9) es_fmt(text, sizeof(text), "&%d %s", i + 1, esc);
    else if (i == 9) es_fmt(text, sizeof(text), "1&0 %s", esc);
    else es_fmt(text, sizeof(text), "%d %s", i + 1, esc);
    AppendMenuA(m, MF_STRING, (UINT)(IDM_WIN_FIRST + i), text);
  }
  if (!n) AppendMenuA(m, MF_STRING | MF_GRAYED, IDM_WIN_FIRST, "(no documents)");
}

void frame_rebuild_menu(HMENU popup)
{
  int has_mru = menu_pos_of(popup, IDM_MRU_CLEAR) >= 0;
  int has_win = menu_pos_of(popup, IDM_WIN_NEXT) >= 0;
  if (has_mru) rebuild_mru(popup);
  if (has_win) rebuild_winlist(popup);
  cmd_update_menu(popup);
  if (has_mru && !mru_shown()) EnableMenuItem(popup, IDM_MRU_FIRST, MF_BYCOMMAND | MF_GRAYED);
  if (has_win && !app_tab_count()) EnableMenuItem(popup, IDM_WIN_FIRST, MF_BYCOMMAND | MF_GRAYED);
}

// ---- closing tabs, exit ----

int frame_close_tab(Tab* t, int ask)
{
  int rc;
  if (!t || app_tab_index(t) < 0) return E_STATE;
  rc = docio_close(t, ask);
  if (rc != E_NOTIMPL) return rc;
  // docio not implemented yet (Wave 2 in progress): close directly
  if (ask && t->doc && t->doc->modified()) {
    char name[280], q[400];
    tab_title(t, name, sizeof(name));
    es_fmt(q, sizeof(q), "%s has unsaved changes. Close it anyway?", name);
    if (ui_ask(Q_SAVE_CHANGES, q, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
  }
  sessmgr_tab_closed(t);
  app_remove_tab(t);
  tab_free(t);
  return 1;
}

static void save_placement()
{
  WINDOWPLACEMENT wp;
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(g_app.frame, &wp)) return;
  g_app.cfg.win_x = wp.rcNormalPosition.left;
  g_app.cfg.win_y = wp.rcNormalPosition.top;
  g_app.cfg.win_w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
  g_app.cfg.win_h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
  g_app.cfg.win_max = wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0;
}

// Runs the exit questions (sessmgr, or per tab while sessmgr is a stub). 1 = go ahead, 0 = cancelled.
static int exit_questions()
{
  int r, i;
  r = sessmgr_exit();
  if (r == 0) return 0;
  if (r < 0) {
    for (i = app_tab_count() - 1; i >= 0; i--) {
      if (frame_close_tab(app_tab(i), 1) == 0) return 0;
    }
  }
  return 1;
}

void frame_close()
{
  if (g_app.exiting) return;
  g_app.exiting = 1;
  if (!exit_questions()) { g_app.exiting = 0; return; }
  save_placement();
  app_save_config();
  fif_stop();                                     // a running Find in Files must not post into a dead frame
  DestroyWindow(g_app.frame);
}

static void open_dropped(HDROP drop)
{
  char path[MAX_PATH];
  UINT i, n = DragQueryFileA(drop, 0xFFFFFFFF, 0, 0);
  for (i = 0; i < n; i++) {
    Tab* t = 0;
    int rc;
    if (!DragQueryFileA(drop, i, path, sizeof(path))) continue;
    if (ui_dir_exists(path)) { app_status_note("%s is a folder", path); continue; }
    rc = docio_open(path, 0, &t);
    if (rc < 0) app_status_note("Could not open %s (%s)", path, e_errname(rc));
  }
  DragFinish(drop);
  SetForegroundWindow(g_app.frame);
}

// ---- notifications ----

static LRESULT on_notify(NMHDR* h)
{
  HWND tabtip = g_app.tabbar ? (HWND)SendMessageA(g_app.tabbar, TCM_GETTOOLTIPS, 0, 0) : 0;
  if (h->code == TTN_NEEDTEXTA) {
    TOOLTIPTEXTA* tt = (TOOLTIPTEXTA*)h;
    if (tabtip && h->hwndFrom == tabtip) {
      tt->hinst = 0;
      tt->lpszText = (LPSTR)tabbar_tooltip((int)h->idFrom);
    } else if (h->idFrom >= 40000 && h->idFrom <= 40999) {
      tt->hinst = g_app.hinst;
      tt->lpszText = MAKEINTRESOURCEA(h->idFrom);
    }
    return 0;
  }
  if (h->code == TTN_NEEDTEXTW) {
    static WCHAR wbuf[400];
    TOOLTIPTEXTW* tt = (TOOLTIPTEXTW*)h;
    char text[400];
    text[0] = 0;
    if (tabtip && h->hwndFrom == tabtip) es_copy(text, sizeof(text), tabbar_tooltip((int)h->idFrom));
    else if (h->idFrom >= 40000 && h->idFrom <= 40999) LoadStringA(g_app.hinst, (UINT)h->idFrom, text, sizeof(text));
    if (!MultiByteToWideChar(CP_ACP, 0, text, -1, wbuf, 400)) wbuf[0] = 0;
    tt->hinst = 0;
    tt->lpszText = wbuf;
    return 0;
  }
  if (g_app.tabbar && h->hwndFrom == g_app.tabbar && h->code == TCN_SELCHANGE) {
    int i = (int)SendMessageA(g_app.tabbar, TCM_GETCURSEL, 0, 0);
    if (i >= 0 && i != g_app.active) app_activate(i);
    return 0;
  }
  if (g_app.statusbar && h->hwndFrom == g_app.statusbar && h->code == NM_CLICK) {
    DWORD mp = GetMessagePos();
    POINT p;
    p.x = (short)LOWORD(mp); p.y = (short)HIWORD(mp);
    ScreenToClient(g_app.statusbar, &p);
    statusbar_click(statusbar_part_at(p.x));
    return 0;
  }
  return 0;
}

// ---- the window procedure ----

static LRESULT CALLBACK frame_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_CREATE: {
    int s = g_app.cfg.poll_seconds;
    g_app.frame = w;
    toolbar_create(w);
    tabbar_create(w);
    statusbar_create(w);
    notifybar_create(w);
    g_app.textview = textview_create(w, ID_TEXTVIEW, 0);
    g_app.hexview = hexview_create(w, ID_HEXVIEW, 0);
    if (g_app.textview) ShowWindow(g_app.textview, SW_HIDE);
    if (g_app.hexview) ShowWindow(g_app.hexview, SW_HIDE);
    g_app.results = results_create(w);
    g_app.isearch = isearch_create(w);
    if (s < 1) s = 1;
    if (s > 60) s = 60;
    SetTimer(w, TIMER_POLL, (UINT)s * 1000, 0);
    SetTimer(w, TIMER_BACKUP, 1000, 0);
    return 0;
  }
  case WM_SIZE:
    if (wp != SIZE_MINIMIZED) app_layout();
    return 0;
  case WM_ERASEBKGND: {
    RECT rc;
    HDC dc = (HDC)wp;
    GetClientRect(w, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_APPWORKSPACE));
    if (g_top_h > 0) {                         // behind the flat (transparent) toolbar and the tab strip
      RECT r = rc;
      r.bottom = g_top_h;
      FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    }
    if (!IsRectEmpty(&g_split)) {
      RECT r = g_split;
      FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
      r.bottom = r.top + 1;
      FillRect(dc, &r, GetSysColorBrush(COLOR_BTNHIGHLIGHT));
      r.top = g_split.bottom - 1; r.bottom = g_split.bottom;
      FillRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
    }
    return 1;
  }
  case WM_GETMINMAXINFO: {
    MINMAXINFO* mm = (MINMAXINFO*)lp;
    mm->ptMinTrackSize.x = 320;
    mm->ptMinTrackSize.y = 220;
    return 0;
  }
  case WM_SETCURSOR:
    if ((HWND)wp == w && LOWORD(lp) == HTCLIENT && !IsRectEmpty(&g_split)) {
      POINT p;
      GetCursorPos(&p);
      ScreenToClient(w, &p);
      if (PtInRect(&g_split, p)) { SetCursor(LoadCursorA(0, (LPCSTR)IDC_SIZENS)); return TRUE; }
    }
    break;
  case WM_LBUTTONDOWN: {
    POINT p;
    p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
    if (PtInRect(&g_split, p)) {
      g_split_drag = 1;
      g_split_grab = p.y - g_split.top;
      SetCapture(w);
      SetCursor(LoadCursorA(0, (LPCSTR)IDC_SIZENS));
    }
    return 0;
  }
  case WM_MOUSEMOVE:
    if (g_split_drag) {
      RECT rc;
      int y = (short)HIWORD(lp) - g_split_grab, bottom;
      GetClientRect(w, &rc);
      bottom = rc.bottom - (g_app.statusbar && g_app.cfg.win_statusbar ? win_h(g_app.statusbar) : 0);
      g_app.cfg.win_results_h = bottom - y - FRAME_SPLIT_H;
      if (g_app.cfg.win_results_h < 40) g_app.cfg.win_results_h = 40;
      app_layout();
      SetCursor(LoadCursorA(0, (LPCSTR)IDC_SIZENS));
      return 0;
    }
    break;
  case WM_LBUTTONUP:
    if (g_split_drag) {
      g_split_drag = 0;
      ReleaseCapture();
      if (g_app.results) g_app.cfg.win_results_h = win_h(g_app.results);
      return 0;
    }
    break;
  case WM_CAPTURECHANGED:
    if ((HWND)lp != w) g_split_drag = 0;
    break;
  case WM_COMMAND:
    if (LOWORD(wp) >= 40000 && LOWORD(wp) <= 40999) {
      app_command(LOWORD(wp));
      frame_ui_dirty();
      return 0;
    }
    break;
  case WM_NOTIFY:
    return on_notify((NMHDR*)lp);
  case WM_DRAWITEM:
    if (wp == ID_TABBAR) { tabbar_draw((const DRAWITEMSTRUCT*)lp); return TRUE; }
    break;
  case WM_INITMENUPOPUP:
    if (!HIWORD(lp)) frame_rebuild_menu((HMENU)wp);
    return 0;
  case WM_TIMER:
    if (wp == TIMER_NOTE) statusbar_note_expired();
    else if (wp == TIMER_POLL) watch_poll(0);
    else if (wp == TIMER_BACKUP) { sessmgr_timer((UINT)wp); notifybar_tick(); frame_ui_dirty(); }
    else if (wp == TIMER_SESSION) sessmgr_timer((UINT)wp);
    return 0;
  case WM_ACTIVATEAPP:
    sessmgr_app_activated(wp ? 1 : 0);
    if (wp) { watch_poll(1); frame_ui_dirty(); }
    break;
  case WM_SETFOCUS: {
    Tab* t = app_active();
    HWND v = t && t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
    if (v && IsWindowVisible(v)) SetFocus(v);
    return 0;
  }
  case WM_INPUTLANGCHANGE:
    winenc_input_changed();
    break;
  case WM_SYSCOLORCHANGE:
    if (g_app.toolbar) SendMessageA(g_app.toolbar, msg, wp, lp);
    if (g_app.tabbar) SendMessageA(g_app.tabbar, msg, wp, lp);
    if (g_app.statusbar) SendMessageA(g_app.statusbar, msg, wp, lp);
    app_apply_config();
    InvalidateRect(w, 0, TRUE);
    break;
  case WM_SETTINGCHANGE:
    app_layout();
    break;
  case WM_DROPFILES:
    open_dropped((HDROP)wp);
    return 0;
  case WM_COPYDATA:
    return main_on_copydata((HWND)wp, (const COPYDATASTRUCT*)lp);
  case WM_APP_COPYDATA_OPEN:
    main_on_copydata_open(wp, lp);
    return 0;
  case WM_APP_CARET:
    app_update_status();
    frame_ui_dirty();
    return 0;
  case WM_APP_DOCSTATE: {
    Tab* t = app_active();
    if (t) app_update_tab(t);
    app_update_title();
    app_update_status();
    frame_ui_dirty();
    return 0;
  }
  case WM_APP_IODONE:
    iothread_on_done(wp, lp);
    frame_ui_dirty();
    return 0;
  case WM_APP_INDEX:
    largefile_on_msg(wp, lp);
    frame_ui_dirty();
    return 0;
  case WM_APP_FIF:
    fif_on_msg(wp, lp);
    return 0;
  case WM_APP_NOTIFY:
    notifybar_on_msg(wp, lp);
    frame_ui_dirty();
    return 0;
  case WM_APP_TABCLOSE: {
    Tab* t = tab_by_id((u32)wp);
    if (t) frame_close_tab(t, 1);
    return 0;
  }
  case WM_QUERYENDSESSION:
    if (g_app.selftest || g_app.bench) return TRUE;
    if (!exit_questions()) return FALSE;
    g_app.exiting = 1;
    return TRUE;
  case WM_ENDSESSION:
    if (wp && !g_app.selftest && !g_app.bench) { save_placement(); app_save_config(); }
    else if (!wp) g_app.exiting = 0;
    return 0;
  case WM_CLOSE:
    frame_close();
    return 0;
  case WM_DESTROY:
    KillTimer(w, TIMER_POLL);
    KillTimer(w, TIMER_BACKUP);
    KillTimer(w, TIMER_SESSION);
    KillTimer(w, TIMER_NOTE);
    g_app.frame = 0;
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

// ---- command handlers owned by the frame ----

static void show_bar(HWND bar, i32* flag)
{
  *flag = *flag ? 0 : 1;
  if (bar) ShowWindow(bar, *flag ? SW_SHOWNA : SW_HIDE);
  app_layout();
  if (*flag) app_update_status();
}

void cmd_view_toolbar(void) { show_bar(g_app.toolbar, &g_app.cfg.win_toolbar); }
void cmd_view_statusbar(void) { show_bar(g_app.statusbar, &g_app.cfg.win_statusbar); }

void cmd_view_results(void)
{
  if (!g_app.results) return;
  results_show(results_visible() ? 0 : 1);
  app_layout();
}

void cmd_win_next(void)
{
  int n = app_tab_count();
  if (n < 2) return;
  app_activate((g_app.active + 1) % n);
}

void cmd_win_prev(void)
{
  int n = app_tab_count();
  if (n < 2) return;
  app_activate((g_app.active + n - 1) % n);
}

void cmd_win_activate(int i)
{
  if (i >= 0 && i < app_tab_count() && i != g_app.active) app_activate(i);
}

void cmd_tab_close(void)
{
  Tab* t = ctx_tab();
  if (t) frame_close_tab(t, 1);
}

void cmd_tab_closeothers(void)
{
  Tab* keep = ctx_tab();
  int i;
  if (!keep) return;
  frame_ctx_tab(0);
  for (i = app_tab_count() - 1; i >= 0; i--) {
    Tab* t = app_tab(i);
    if (t == keep) continue;
    if (frame_close_tab(t, 1) == 0) break;      // the user cancelled
  }
  i = app_tab_index(keep);
  if (i >= 0 && i != g_app.active) app_activate(i);
}

void cmd_tab_copypath(void)
{
  Tab* t = ctx_tab();
  HGLOBAL g;
  char* p;
  u32 n;
  if (!t || t->untitled || !t->path[0]) return;
  n = es_len(t->path) + 1;
  if (!OpenClipboard(g_app.frame)) { app_status_note("The clipboard is in use by another program"); return; }
  EmptyClipboard();
  g = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, n);
  p = g ? (char*)GlobalLock(g) : 0;
  if (p) {
    memcpy(p, t->path, n);
    GlobalUnlock(g);
    if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
  } else if (g) {
    GlobalFree(g);
  }
  CloseClipboard();
  app_status_note("Copied %s", t->path);
}

void cmd_tab_openfolder(void)
{
  Tab* t = ctx_tab();
  char args[600], dir[300];
  if (!t || t->untitled || !t->path[0]) return;
  if (ui_file_exists(t->path)) {
    es_fmt(args, sizeof(args), "/select,\"%s\"", t->path);
  } else {
    ui_path_dir(t->path, dir, sizeof(dir));
    es_fmt(args, sizeof(args), "\"%s\"", dir);
  }
  if (g_app.selftest) { selftest_log("open folder: explorer.exe %s", args); return; }
  if ((int)ShellExecuteA(g_app.frame, "open", "explorer.exe", args, 0, SW_SHOWNORMAL) <= 32)
    app_status_note("Could not open the folder of %s", t->path);
}

UINT cmdstate_view_toolbar(void) { return g_app.cfg.win_toolbar ? MF_CHECKED : 0; }
UINT cmdstate_view_statusbar(void) { return g_app.cfg.win_statusbar ? MF_CHECKED : 0; }
UINT cmdstate_view_results(void)
{
  if (!g_app.results) return MF_GRAYED;
  return results_visible() ? MF_CHECKED : 0;
}
UINT cmdstate_win_cycle(void) { return app_tab_count() < 2 ? MF_GRAYED : 0; }
UINT cmdstate_win(int i) { return i == g_app.active ? MF_CHECKED : 0; }

// ---- selftest (17.3) ----

static int g_nb_calls = 0, g_nb_button = -1, g_nb_kind = -1;
static Tab* g_nb_tab = 0;
static void* g_nb_ctx = 0;
static Vec<int> g_st_trace;

static void st_nb_fn(Tab* t, int kind, int button, void* ctx)
{
  g_nb_calls++;
  g_nb_tab = t; g_nb_kind = kind; g_nb_button = button; g_nb_ctx = ctx;
}

static void st_trace(int id) { g_st_trace.push(id); }

static Tab* st_make_tab(const char* path, const char* text, int modified, const char* lang)
{
  Document* d = new Document;
  Tab* t;
  if (!d) return 0;
  if (d->init_empty(enc_ansi(), EOL_CRLF) != E_OK) { delete d; return 0; }
  if (text && text[0]) d->replace(0, 0, (const u8*)text, es_len(text));
  if (!modified) d->set_saved();
  t = tab_new(d);
  if (!t) { delete d; return 0; }
  if (path) es_copy(t->path, sizeof(t->path), path);
  else { t->untitled = 1; t->untitled_no = g_app.next_untitled++; }
  if (lang) { t->lang = lang_find(lang); t->hl->attach(&t->lines, t->lang); }
  if (app_add_tab(t, 1) != E_OK) { tab_free(t); return 0; }
  return t;
}

static int item_rect_ok(int i, RECT* r)
{
  return i >= 0 && g_app.tabbar && SendMessageA(g_app.tabbar, TCM_GETITEMRECT, (WPARAM)i, (LPARAM)r) != 0;
}

static int st_item_text(int i, char* out, u32 cap)
{
  TCITEMA it;
  out[0] = 0;
  it.mask = TCIF_TEXT;
  it.pszText = out;
  it.cchTextMax = (int)cap;
  return g_app.tabbar && SendMessageA(g_app.tabbar, TCM_GETITEMA, (WPARAM)i, (LPARAM)&it) != 0;
}

static int st_labels_match(StCtx* c)
{
  int i, ok = 1, n = app_tab_count();
  if ((int)SendMessageA(g_app.tabbar, TCM_GETITEMCOUNT, 0, 0) != n) return 0;
  for (i = 0; i < n; i++) {
    char item[400], label[400];
    st_item_text(i, item, sizeof(item));
    tab_label(app_tab(i), label, sizeof(label));
    if (strncmp(item, label, es_len(label)) != 0) { ok = 0; st_checkf(c, 0, "tab %d: item \"%s\" label \"%s\"", i, item, label); }
  }
  return ok;
}

static void st_click(HWND w, UINT down, UINT up, const RECT* r)
{
  LPARAM at = MAKELPARAM((r->left + r->right) / 2, (r->top + r->bottom) / 2);
  SendMessageA(w, down, down == WM_LBUTTONDOWN ? MK_LBUTTON : 0, at);
  SendMessageA(w, up, 0, at);
}

static HMENU st_popup_with(int id, int* pos)
{
  HMENU m = GetMenu(g_app.frame);
  int i, n = m ? GetMenuItemCount(m) : 0;
  for (i = 0; i < n; i++) {
    HMENU sub = GetSubMenu(m, i);
    if (sub && menu_pos_of(sub, id) >= 0) { if (pos) *pos = i; return sub; }
  }
  return 0;
}

static int st_count_ids(HMENU m, int lo, int hi)
{
  int i, n = GetMenuItemCount(m), k = 0;
  for (i = 0; i < n; i++) {
    int id = (int)GetMenuItemID(m, i);
    if (id >= lo && id <= hi) k++;
  }
  return k;
}

static void st_frame_settings(StCtx* c)
{
  char path[300];
  Config* back = (Config*)e_alloc(sizeof(Config));
  i32 save[8];
  int rc;
  if (!back) { ST_CHECK(c, 0, "no memory"); return; }
  save[0] = g_app.cfg.win_x; save[1] = g_app.cfg.win_y; save[2] = g_app.cfg.win_w; save[3] = g_app.cfg.win_h;
  save[4] = g_app.cfg.win_max; save[5] = g_app.cfg.win_toolbar; save[6] = g_app.cfg.win_statusbar;
  save[7] = g_app.cfg.win_results_h;
  g_app.cfg.win_x = 123; g_app.cfg.win_y = 45; g_app.cfg.win_w = 640; g_app.cfg.win_h = 480; g_app.cfg.win_max = 1;
  g_app.cfg.win_toolbar = 0; g_app.cfg.win_statusbar = 1; g_app.cfg.win_results_h = 222;
  ui_path_join(g_app.cfg_dir, "st_frame.ini", path, sizeof(path));
  DeleteFileA(path);
  rc = main_save_config_to(path);
  st_checkf(c, rc == E_OK, "settings saved through file_save (%s)", e_errname(rc));
  rc = main_load_config_from(path, back);
  st_checkf(c, rc == E_OK, "settings read back (%s)", e_errname(rc));
  st_checkf(c, back->win_x == 123 && back->win_y == 45 && back->win_w == 640 && back->win_h == 480 && back->win_max == 1,
            "placement round trip: %d,%d %dx%d max %d", back->win_x, back->win_y, back->win_w, back->win_h, back->win_max);
  st_checkf(c, back->win_toolbar == 0 && back->win_statusbar == 1 && back->win_results_h == 222,
            "bars round trip: toolbar %d statusbar %d results_h %d", back->win_toolbar, back->win_statusbar,
            back->win_results_h);
  // a second save keeps unknown keys
  {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    DWORD wr;
    static const char extra[] = "[Custom]\r\nmine=42\r\n";
    if (h != INVALID_HANDLE_VALUE) {
      SetFilePointer(h, 0, 0, FILE_END);
      WriteFile(h, extra, sizeof(extra) - 1, &wr, 0);
      CloseHandle(h);
    }
    g_app.cfg.win_x = 77;
    rc = main_save_config_to(path);
    st_checkf(c, rc == E_OK, "second save (%s)", e_errname(rc));
    {
      EBytes b;
      int found = st_file_bytes(c, path, &b) == E_OK && b.push(0) == E_OK && strstr((const char*)b.data(), "mine=42") != 0;
      st_checkf(c, found, "unknown key kept in the settings file");
    }
  }
  DeleteFileA(path);
  g_app.cfg.win_x = save[0]; g_app.cfg.win_y = save[1]; g_app.cfg.win_w = save[2]; g_app.cfg.win_h = save[3];
  g_app.cfg.win_max = save[4]; g_app.cfg.win_toolbar = save[5]; g_app.cfg.win_statusbar = save[6];
  g_app.cfg.win_results_h = save[7];
  e_free(back);
}

static void st_frame_copydata(StCtx* c)
{
  static const char block[] = "PICA1\0C:\\WORK\0-n12\0a.txt\0-hex\0sub\\b.bin\0D:\\abs.txt\0\\root.txt\0-ro\0-cfg\0x\0z.c\0";
  Vec<MainFileArg> f;
  int rc = main_parse_block(block, sizeof(block), &f);
  st_checkf(c, rc == E_OK && f.size() == 5, "copydata block parsed: %s, %u files", e_errname(rc), f.size());
  if (f.size() == 5) {
    st_checkf(c, es_icmp(f[0].path, "C:\\WORK\\a.txt") == 0 && f[0].line == 12 && !f[0].hex, "arg 1: %s line %u",
              f[0].path, f[0].line);
    st_checkf(c, es_icmp(f[1].path, "C:\\WORK\\sub\\b.bin") == 0 && f[1].hex && !f[1].line, "arg 2: %s hex %d",
              f[1].path, f[1].hex);
    st_checkf(c, es_icmp(f[2].path, "D:\\abs.txt") == 0 && !f[2].hex, "arg 3: %s", f[2].path);
    st_checkf(c, es_icmp(f[3].path, "C:\\root.txt") == 0, "arg 4: %s", f[3].path);
    st_checkf(c, es_icmp(f[4].path, "C:\\WORK\\z.c") == 0 && f[4].ro, "arg 5: %s ro %d (-cfg x skipped)", f[4].path, f[4].ro);
  }
  ST_CHECK(c, main_parse_block("PICAX\0\0\0", 8, &f) == E_FORMAT, "bad block refused");
  // through WM_COPYDATA: refused unless accepted, then the file goes to docio_open
  {
    char path[300], blk[600], cwd[300];
    COPYDATASTRUCT cds;
    u32 n = 0;
    LRESULT r;
    Tab* t;
    if (st_fixture(c, "copydata.txt", "copied\r\n", 8, path, sizeof(path)) != E_OK) return;
    ui_path_dir(path, cwd, sizeof(cwd));
    memcpy(blk, "PICA1", 6); n = 6;
    es_copy(blk + n, sizeof(blk) - n, cwd); n += es_len(cwd) + 1;
    es_copy(blk + n, sizeof(blk) - n, "copydata.txt"); n += 13;
    blk[n++] = 0;
    cds.dwData = 0x41434950; cds.cbData = n; cds.lpData = blk;
    r = SendMessageA(g_app.frame, WM_COPYDATA, 0, (LPARAM)&cds);
    ST_CHECK(c, r == FALSE, "WM_COPYDATA refused in selftest mode");
    main_accept_copydata_in_selftest(1);
    r = SendMessageA(g_app.frame, WM_COPYDATA, 0, (LPARAM)&cds);
    main_accept_copydata_in_selftest(0);
    ST_CHECK(c, r == TRUE, "WM_COPYDATA accepted");
    st_pump(c, 100);
    t = tab_find_path(path);
    if (t) {
      ST_CHECK(c, t == app_active(), "handed-over file is the active tab");
      frame_close_tab(t, 0);
    } else {
      char note[300];
      statusbar_get(SB_PART_FLAGS, note, sizeof(note));
      st_checkf(c, strstr(note, "copydata.txt") != 0, "handed-over file reached docio_open (note \"%s\")", note);
    }
    DeleteFileA(path);
  }
}

static void st_frame_progress(StCtx* c)
{
  char flags[300];
  int h;
  {
    UiProgress p("Pica selftest", "Testing the progress window...", 1);
    ST_CHECK(c, p.progress(0, 4) == 0 && !progress_window_visible(), "no window before 500 ms");
    Sleep(550);
    p.progress(1, 4);
    ST_CHECK(c, progress_window_visible(), "progress window after 500 ms");
    ST_CHECK(c, !IsWindowEnabled(g_app.frame), "frame disabled while the progress window is up");
    if (progress_window()) {
      HWND b = GetDlgItem(progress_window(), IDCANCEL);
      ST_CHECK(c, b != 0, "Cancel button");
      if (b) SendMessageA(b, BM_CLICK, 0, 0);
    }
    ST_CHECK(c, p.progress(2, 4) != 0 && p.cancelled(), "Cancel reported");
  }
  ST_CHECK(c, !progress_window_visible(), "progress window gone");
  ST_CHECK(c, IsWindowEnabled(g_app.frame), "frame enabled again");
  statusbar_note_expired();
  h = progress_status_begin("Saving");
  progress_status_step(h, 20);
  statusbar_get(SB_PART_FLAGS, flags, sizeof(flags));
  st_checkf(c, strstr(flags, "Saving 20%") != 0, "status progress shown (\"%s\")", flags);
  progress_status_end(h);
  statusbar_get(SB_PART_FLAGS, flags, sizeof(flags));
  st_checkf(c, strstr(flags, "Saving") == 0, "status progress removed (\"%s\")", flags);
}

static void st_frame_notify(StCtx* c, Tab* a, Tab* other)
{
  NotifyButton bt[2];
  RECT vr, nr;
  bt[0].id = 1; es_copy(bt[0].text, sizeof(bt[0].text), "Reload");
  bt[1].id = 2; es_copy(bt[1].text, sizeof(bt[1].text), "Keep my version");
  app_activate(app_tab_index(a));
  g_nb_calls = 0;
  notifybar_show(a, NB_CHANGED, "alpha.c was changed by another program.", bt, 2, st_nb_fn, (void*)0x1234);
  st_pump(c, 30);
  ST_CHECK(c, notifybar_height() > 0 && IsWindowVisible(g_app.notifybar), "bar visible");
  ST_CHECK(c, notifybar_button(0) != 0 && notifybar_button(1) != 0 && notifybar_button(2) == 0, "two buttons");
  if (g_app.textview && IsWindowVisible(g_app.textview)) {
    GetWindowRect(g_app.textview, &vr);
    GetWindowRect(g_app.notifybar, &nr);
    st_checkf(c, vr.top >= nr.bottom, "view below the bar (view top %ld, bar bottom %ld)", vr.top, nr.bottom);
  }
  if (notifybar_button(1)) SendMessageA(notifybar_button(1), BM_CLICK, 0, 0);
  st_pump(c, 50);
  st_checkf(c, g_nb_calls == 1 && g_nb_button == 2 && g_nb_kind == NB_CHANGED && g_nb_tab == a && g_nb_ctx == (void*)0x1234,
            "button callback: calls %d button %d kind %d", g_nb_calls, g_nb_button, g_nb_kind);
  ST_CHECK(c, notifybar_height() == 0 && !IsWindowVisible(g_app.notifybar) && a->notify.kind == NB_NONE,
           "bar hidden after the click");
  // one bar per tab: follows activation
  notifybar_show(a, NB_MISSING, "alpha.c was deleted.", bt, 1, st_nb_fn, 0);
  app_activate(app_tab_index(other));
  ST_CHECK(c, notifybar_height() == 0, "bar hidden for a tab without one");
  app_activate(app_tab_index(a));
  ST_CHECK(c, notifybar_height() > 0 && notifybar_button(0) && !notifybar_button(1), "bar back with its tab");
  notifybar_close_click();
  st_pump(c, 50);
  st_checkf(c, g_nb_calls == 2 && g_nb_button == 0 && g_nb_kind == NB_MISSING, "close box callback (button %d)", g_nb_button);
  ST_CHECK(c, notifybar_height() == 0, "bar closed");
  // auto-hide and notifybar_hide
  notifybar_show(a, NB_INFO, "Saved.", 0, 0, st_nb_fn, 0);
  a->notify.auto_hide_ms = 1;
  Sleep(20);
  notifybar_tick();
  st_checkf(c, g_nb_calls == 3 && notifybar_height() == 0, "auto-hide (calls %d)", g_nb_calls);
  notifybar_show(a, NB_READONLY, "alpha.c is read-only.", 0, 0, 0, 0);
  notifybar_hide(a, NB_CHANGED);
  ST_CHECK(c, notifybar_height() > 0, "hide of another kind keeps the bar");
  notifybar_hide(a, NB_READONLY);
  ST_CHECK(c, notifybar_height() == 0, "hide of its kind removes the bar");
}

// Manual visual check: if <cfg>\st_frame.hold exists (contents: seconds, default 60), show a demo state (3 tabs,
// one modified, notification bar, status texts) and pump messages that long before the automated checks run.
// I1: editing keys typed into the incremental search box stay in the box (isearch_translate in app_dispatch).
// Without the hook Ctrl+X/V/Z reach the main accelerators and cut/paste/undo in the document.
static void st_frame_translate(StCtx* c)
{
  Tab* t = st_make_tab(0, "hello world", 0, 0);
  char box[64];
  u8 bytes[16];
  HWND e;
  if (!t || !g_app.isearch) { ST_CHECK(c, 0, "translate: setup"); if (t) frame_close_tab(t, 0); return; }
  SetForegroundWindow(g_app.frame);
  isearch_show();
  st_pump(c, 50);
  e = GetFocus();
  ST_CHECK(c, e && IsChild(g_app.isearch, e), "incremental search box has the focus");
  st_keys(c, "wor");
  st_keys(c, "^a");
  st_keys(c, "^x");
  st_pump(c, 50);
  box[0] = 0;
  if (e) GetWindowTextA(e, box, sizeof(box));
  st_checkf(c, box[0] == 0, "Ctrl+A, Ctrl+X cut the box text (box \"%s\")", box);
  st_keys(c, "^v");
  st_keys(c, "^v");
  st_pump(c, 50);
  box[0] = 0;
  if (e) GetWindowTextA(e, box, sizeof(box));
  st_checkf(c, strcmp(box, "worwor") == 0, "Ctrl+V twice pasted into the box (box \"%s\")", box);
  st_keys(c, "^z");
  st_pump(c, 50);
  memset(bytes, 0, sizeof(bytes));
  st_checkf(c, t->doc->length() == 11 && t->doc->read(0, bytes, 11) == E_OK && memcmp(bytes, "hello world", 11) == 0 &&
            !t->doc->modified(), "document untouched by Ctrl+X/V/Z in the box (length %u)", (u32)t->doc->length());
  isearch_hide();
  st_pump(c, 20);
  frame_close_tab(t, 0);
}

#ifndef SB_ISSIMPLE
#define SB_ISSIMPLE (WM_USER + 14)
#endif

// I1: a note too long for the last part fills the whole status bar until it expires; a short one stays in the part.
static void st_frame_notes(StCtx* c)
{
  char text[300];
  int vis = g_app.statusbar && IsWindowVisible(g_app.statusbar);
  if (!vis) { ST_CHECK(c, 0, "notes: status bar visible"); return; }
  statusbar_note_ms(5000, "Short note");
  st_checkf(c, !SendMessageA(g_app.statusbar, SB_ISSIMPLE, 0, 0), "short note stays in the last part");
  statusbar_get(SB_PART_FLAGS, text, sizeof(text));
  st_checkf(c, strcmp(text, "Short note") == 0, "short note text \"%s\"", text);
  statusbar_note_ms(5000, "UTF-16 edits take longer on this document: a long note that cannot fit in the last part");
  st_checkf(c, SendMessageA(g_app.statusbar, SB_ISSIMPLE, 0, 0) != 0, "long note fills the whole bar");
  statusbar_note_expired();
  st_checkf(c, !SendMessageA(g_app.statusbar, SB_ISSIMPLE, 0, 0), "parts back after the note expired");
}

static void st_frame_hold(StCtx* c)
{
  char path[300];
  EBytes b;
  u32 secs = 60;
  Tab* t[3];
  NotifyButton bt[2];
  int i;
  ui_path_join(g_app.cfg_dir, "st_frame.hold", path, sizeof(path));
  if (!ui_file_exists(path)) return;
  if (st_file_bytes(c, path, &b) == E_OK && b.push(0) == E_OK) es_parse_u32(es_trim((char*)b.data()), &secs, 10);
  if (secs > 600) secs = 600;
  t[0] = st_make_tab("C:\\PICATEST\\fixtures\\main.cpp", "int main() { return 0; }\r\n", 1, "cpp");
  t[1] = st_make_tab("C:\\PICATEST\\fixtures\\Makefile", "all: pica.exe\r\n", 0, "make");
  t[2] = st_make_tab(0, "", 0, 0);
  app_activate(app_tab_index(t[0]));
  bt[0].id = 1; es_copy(bt[0].text, sizeof(bt[0].text), "Reload");
  bt[1].id = 2; es_copy(bt[1].text, sizeof(bt[1].text), "Keep my version");
  if (t[0]) notifybar_show(t[0], NB_CHANGED, "main.cpp was changed by another program.", bt, 2, 0, 0);
  statusbar_note_ms(secs * 1000, "Demo for the screenshot (st_frame.hold)");
  st_pump(c, secs * 1000);
  for (i = 2; i >= 0; i--) if (t[i] && app_tab_index(t[i]) >= 0) frame_close_tab(t[i], 0);
}

void st_frame(StCtx* c)
{
  int base = app_tab_count(), i, n, pos;
  st_frame_hold(c);
  char text[400], want[400];
  Tab* a;
  Tab* b;
  Tab* u;
  Tab* d;
  RECT r;
  HMENU m;

  // languages (lang_init from RCDATA), toolbar, bars
  st_checkf(c, lang_count() >= IDR_LANG_COUNT && lang_find("cpp") != 0, "languages: %u", lang_count());
  st_checkf(c, main_lang_errors()[0] == 0, "language errors: %s", main_lang_errors());
  st_checkf(c, g_app.toolbar && (int)SendMessageA(g_app.toolbar, TB_BUTTONCOUNT, 0, 0) == TB_COUNT, "toolbar buttons");
  ST_CHECK(c, g_app.tabbar && g_app.notifybar && g_app.statusbar, "frame children");

  // three tabs: one modified, one saved, one untitled
  a = st_make_tab("C:\\PICATEST\\fixtures\\alpha.c", "int x;\r\n", 1, "cpp");
  b = st_make_tab("C:\\PICATEST\\fixtures\\beta.txt", "beta\r\n", 0, 0);
  u = st_make_tab(0, "", 0, 0);
  if (!a || !b || !u) { ST_CHECK(c, 0, "test tabs created"); return; }
  st_checkf(c, app_tab_count() == base + 3 && app_active() == u, "3 tabs, last active (count %d)", app_tab_count());
  ST_CHECK(c, (int)SendMessageA(g_app.tabbar, TCM_GETCURSEL, 0, 0) == app_active_index(), "tab bar selection");
  ST_CHECK(c, IsWindowVisible(g_app.tabbar) && tabbar_height() > 0, "tab bar visible");
  ST_CHECK(c, st_labels_match(c), "tab labels");
  tab_label(a, text, sizeof(text));
  st_checkf(c, strcmp(text, "alpha.c *") == 0, "modified label \"%s\"", text);
  GetWindowTextA(g_app.frame, text, sizeof(text));
  es_fmt(want, sizeof(want), "new %u - " APP_NAME, u->untitled_no);
  st_checkf(c, strcmp(text, want) == 0, "title \"%s\" (want \"%s\")", text, want);
  st_checkf(c, strcmp(tabbar_tooltip(app_tab_index(a)), a->path) == 0, "path tooltip");
  ST_CHECK(c, strstr(tabbar_tooltip(app_tab_index(u)), "not saved") != 0, "untitled tooltip");

  // activation, title, status bar
  app_activate(app_tab_index(a));
  st_pump(c, 20);
  GetWindowTextA(g_app.frame, text, sizeof(text));
  st_checkf(c, strcmp(text, "alpha.c * - " APP_NAME) == 0, "title \"%s\"", text);
  ST_CHECK(c, (int)SendMessageA(g_app.tabbar, TCM_GETCURSEL, 0, 0) == app_tab_index(a), "selection follows activation");
  statusbar_get(SB_PART_LANG, text, sizeof(text));
  st_checkf(c, a->lang && strcmp(text, a->lang->name) == 0, "language part \"%s\"", text);
  statusbar_get(SB_PART_ENC, text, sizeof(text));
  st_checkf(c, strcmp(text, "ANSI") == 0, "encoding part \"%s\"", text);
  statusbar_get(SB_PART_EOL, text, sizeof(text));
  st_checkf(c, strcmp(text, "CRLF") == 0, "EOL part \"%s\"", text);
  statusbar_get(SB_PART_MODE, text, sizeof(text));
  st_checkf(c, strcmp(text, "INS") == 0, "mode part \"%s\"", text);
  statusbar_get(SB_PART_VIEW, text, sizeof(text));
  st_checkf(c, strcmp(text, "TEXT") == 0, "view part \"%s\"", text);
  statusbar_get(SB_PART_POS, text, sizeof(text));
  st_checkf(c, strncmp(text, "Ln ", 3) == 0, "position part \"%s\"", text);
  statusbar_get(SB_PART_SEL, text, sizeof(text));
  st_checkf(c, strncmp(text, "Sel ", 4) == 0, "selection part \"%s\"", text);
  app_activate(app_tab_index(b));
  statusbar_get(SB_PART_LANG, text, sizeof(text));
  st_checkf(c, strcmp(text, "Plain text") == 0, "plain text part \"%s\"", text);
  b->doc->set_read_only(true);
  app_update_tab(b);
  GetWindowTextA(g_app.frame, text, sizeof(text));
  st_checkf(c, strstr(text, "[Read-only]") != 0, "read-only title \"%s\"", text);
  statusbar_note_expired();
  statusbar_get(SB_PART_FLAGS, text, sizeof(text));
  st_checkf(c, strstr(text, "RO") != 0, "RO flag \"%s\"", text);
  b->doc->set_read_only(false);
  app_update_tab(b);

  // status bar click targets
  g_st_trace.clear();
  cmd_set_trace(st_trace, 1);
  statusbar_click(SB_PART_POS);
  statusbar_click(SB_PART_MODE);
  statusbar_click(SB_PART_VIEW);
  cmd_set_trace(0, 0);
  st_checkf(c, g_st_trace.size() == 3 && g_st_trace[0] == IDM_SEARCH_GOTO && g_st_trace[1] == IDM_EDIT_OVERWRITE &&
            g_st_trace[2] == IDM_VIEW_HEX, "status clicks route (%u)", g_st_trace.size());
  m = statusbar_popup_menu(SB_PART_LANG);
  ST_CHECK(c, m && menu_pos_of(m, IDM_LANG_AUTO) >= 0, "Syntax popup");
  m = statusbar_popup_menu(SB_PART_ENC);
  ST_CHECK(c, m && menu_pos_of(m, IDM_ENC_CONV_UTF8) >= 0, "Encoding popup");
  m = statusbar_popup_menu(SB_PART_EOL);
  ST_CHECK(c, m && menu_pos_of(m, IDM_EOL_LF) >= 0, "Line endings popup");
  ST_CHECK(c, statusbar_popup_menu(SB_PART_POS) == 0, "no popup for the position part");
  if (SendMessageA(g_app.statusbar, SB_GETRECT, SB_PART_ENC, (LPARAM)&r))
    st_checkf(c, statusbar_part_at(r.left + 2) == SB_PART_ENC, "part hit test");

  // drag reorder with mouse messages: drag the first test tab onto the last one
  {
    RECT r0, r2;
    int from = app_tab_index(a), to = app_tab_index(u);
    if (item_rect_ok(from, &r0) && item_rect_ok(to, &r2)) {
      int y = (r0.top + r0.bottom) / 2;
      SendMessageA(g_app.tabbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(r0.left + 8, y));
      st_pump(c, 10);
      SendMessageA(g_app.tabbar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(r0.left + 20, y));
      SendMessageA(g_app.tabbar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM((r2.left + r2.right) / 2, y));
      SendMessageA(g_app.tabbar, WM_LBUTTONUP, 0, MAKELPARAM((r2.left + r2.right) / 2, y));
      st_pump(c, 10);
      st_checkf(c, app_tab_index(a) == to && app_active() == a, "drag reorder: alpha at %d (want %d), active %d",
                app_tab_index(a), to, app_active_index());
      ST_CHECK(c, st_labels_match(c), "labels after the drag");
    } else {
      ST_CHECK(c, 0, "tab rectangles");
    }
    ST_CHECK(c, tabbar_move(app_tab_index(a), base) == E_OK && app_tab_index(a) == base && app_active() == a,
             "tabbar_move back");
    ST_CHECK(c, tabbar_move(0, 99) == E_RANGE, "tabbar_move range");
  }

  // Window menu and recent files
  m = st_popup_with(IDM_WIN_NEXT, &pos);
  if (m) {
    SendMessageA(g_app.frame, WM_INITMENUPOPUP, (WPARAM)m, MAKELPARAM(pos, FALSE));
    n = st_count_ids(m, IDM_WIN_FIRST, IDM_WIN_LAST);
    st_checkf(c, n == app_tab_count(), "Window menu entries %d (tabs %d)", n, app_tab_count());
    ST_CHECK(c, (GetMenuState(m, (UINT)(IDM_WIN_FIRST + app_active_index()), MF_BYCOMMAND) & MF_CHECKED) != 0,
             "active tab checked");
    GetMenuStringA(m, (UINT)(IDM_WIN_FIRST + app_tab_index(a)), text, sizeof(text), MF_BYCOMMAND);
    st_checkf(c, strstr(text, "alpha.c *") != 0, "Window menu text \"%s\"", text);
  } else {
    ST_CHECK(c, 0, "Window menu");
  }
  m = st_popup_with(IDM_MRU_CLEAR, &pos);
  if (m) {
    static char saved[CFG_MRU_MAX][260];
    i32 saved_count = g_app.cfg.mru_count;
    memcpy(saved, g_app.cfg.mru, sizeof(saved));
    memset(g_app.cfg.mru, 0, sizeof(g_app.cfg.mru));
    g_app.cfg.mru_count = 10;
    es_copy(g_app.cfg.mru[0], 260, "C:\\a\\one.txt");
    es_copy(g_app.cfg.mru[1], 260, "C:\\b&c\\two.txt");
    es_copy(g_app.cfg.mru[2], 260, "C:\\Program Files\\Microsoft Visual Studio\\VC98\\Include\\very_long_name.h");
    SendMessageA(g_app.frame, WM_INITMENUPOPUP, (WPARAM)m, MAKELPARAM(pos, FALSE));
    st_checkf(c, st_count_ids(m, IDM_MRU_FIRST, IDM_MRU_LAST) == 3, "3 recent files");
    GetMenuStringA(m, IDM_MRU_FIRST, text, sizeof(text), MF_BYCOMMAND);
    st_checkf(c, strcmp(text, "&1 C:\\a\\one.txt") == 0, "MRU 1 \"%s\"", text);
    GetMenuStringA(m, IDM_MRU_FIRST + 1, text, sizeof(text), MF_BYCOMMAND);
    st_checkf(c, strcmp(text, "&2 C:\\b&&c\\two.txt") == 0, "MRU 2 \"%s\"", text);
    GetMenuStringA(m, IDM_MRU_FIRST + 2, text, sizeof(text), MF_BYCOMMAND);
    st_checkf(c, strstr(text, "...") != 0 && strstr(text, "very_long_name.h") != 0 && es_len(text) <= 66,
              "MRU 3 compacted \"%s\"", text);
    ST_CHECK(c, !(GetMenuState(m, IDM_MRU_FIRST, MF_BYCOMMAND) & MF_GRAYED), "MRU entry enabled");
    memset(g_app.cfg.mru, 0, sizeof(g_app.cfg.mru));
    SendMessageA(g_app.frame, WM_INITMENUPOPUP, (WPARAM)m, MAKELPARAM(pos, FALSE));
    ST_CHECK(c, st_count_ids(m, IDM_MRU_FIRST, IDM_MRU_LAST) == 1 &&
             (GetMenuState(m, IDM_MRU_FIRST, MF_BYCOMMAND) & MF_GRAYED), "empty list placeholder");
    memcpy(g_app.cfg.mru, saved, sizeof(saved));
    g_app.cfg.mru_count = saved_count;
    SendMessageA(g_app.frame, WM_INITMENUPOPUP, (WPARAM)m, MAKELPARAM(pos, FALSE));
  } else {
    ST_CHECK(c, 0, "File menu");
  }

  // Window commands and Ctrl+Tab
  app_activate(base);
  app_command(IDM_WIN_NEXT);
  ST_CHECK(c, app_active_index() == base + 1, "next tab");
  app_command(IDM_WIN_PREV);
  app_command(IDM_WIN_PREV);
  ST_CHECK(c, app_active_index() == app_tab_count() - 1, "previous tab wraps");
  app_command(IDM_WIN_FIRST + base);
  ST_CHECK(c, app_active_index() == base, "Window menu activation");
  SetForegroundWindow(g_app.frame);
  SetFocus(g_app.frame);
  st_keys(c, "^{TAB}");
  st_checkf(c, app_active_index() == base + 1, "Ctrl+Tab (active %d)", app_active_index());
  st_keys(c, "^+{TAB}");
  st_checkf(c, app_active_index() == base, "Ctrl+Shift+Tab (active %d)", app_active_index());

  // toolbar / status bar toggles
  {
    int y0 = 0, y1 = 0;
    GetWindowRect(g_app.tabbar, &r); y0 = r.top;
    app_command(IDM_VIEW_TOOLBAR);
    GetWindowRect(g_app.tabbar, &r); y1 = r.top;
    ST_CHECK(c, !g_app.cfg.win_toolbar && !IsWindowVisible(g_app.toolbar) && y1 < y0, "toolbar hidden, tab bar moved up");
    ST_CHECK(c, (cmd_state(IDM_VIEW_TOOLBAR) & MF_CHECKED) == 0, "toolbar unchecked");
    app_command(IDM_VIEW_TOOLBAR);
    GetWindowRect(g_app.tabbar, &r);
    ST_CHECK(c, g_app.cfg.win_toolbar && IsWindowVisible(g_app.toolbar) && r.top == y0, "toolbar back");
    app_command(IDM_VIEW_STATUSBAR);
    ST_CHECK(c, !g_app.cfg.win_statusbar && !IsWindowVisible(g_app.statusbar), "status bar hidden");
    app_command(IDM_VIEW_STATUSBAR);
    ST_CHECK(c, g_app.cfg.win_statusbar && IsWindowVisible(g_app.statusbar), "status bar back");
    if (g_app.results) {
      app_command(IDM_VIEW_RESULTS);
      frame_splitter_rect(&r);
      ST_CHECK(c, results_visible() && !IsRectEmpty(&r), "results panel + splitter");
      app_command(IDM_VIEW_RESULTS);
      frame_splitter_rect(&r);
      ST_CHECK(c, !results_visible() && IsRectEmpty(&r), "results panel hidden");
    }
  }

  // toolbar state follows cmd_state
  frame_toolbar_update();
  for (i = 0; i < TB_COUNT; i++) {
    int st;
    if (!k_tb[i].id) continue;
    st = frame_toolbar_state(k_tb[i].id);
    st_checkf(c, st >= 0 && ((st & TBSTATE_ENABLED) ? 1 : 0) == ((cmd_state(k_tb[i].id) & MF_GRAYED) ? 0 : 1),
              "toolbar button %d state %d", k_tb[i].id, st);
  }

  // notification bar
  st_frame_notify(c, a, b);

  // tab context commands
  frame_ctx_tab(a);
  app_command(IDM_TAB_COPYPATH);
  frame_ctx_tab(0);
  if (OpenClipboard(g_app.frame)) {
    HANDLE h = GetClipboardData(CF_TEXT);
    const char* p = h ? (const char*)GlobalLock(h) : 0;
    st_checkf(c, p && strcmp(p, a->path) == 0, "Copy Full Path (\"%s\")", p ? p : "");
    if (p) GlobalUnlock(h);
    CloseClipboard();
  }
  frame_ctx_tab(a);
  app_command(IDM_TAB_OPENFOLDER);                 // selftest: logged only
  frame_ctx_tab(0);

  // middle-click closes a tab, the close box closes the active tab
  n = app_tab_count();
  if (item_rect_ok(app_tab_index(b), &r)) st_click(g_app.tabbar, WM_MBUTTONDOWN, WM_MBUTTONUP, &r);
  st_pump(c, 50);
  st_checkf(c, app_tab_count() == n - 1 && app_tab_index(b) < 0, "middle-click close (count %d)", app_tab_count());
  app_activate(app_tab_index(u));
  n = app_tab_count();
  if (tabbar_close_rect(app_active_index(), &r)) st_click(g_app.tabbar, WM_LBUTTONDOWN, WM_LBUTTONUP, &r);
  else ST_CHECK(c, 0, "close box rectangle");
  st_pump(c, 50);
  st_checkf(c, app_tab_count() == n - 1 && app_tab_index(u) < 0, "close box (count %d)", app_tab_count());

  // Close Others keeps only the context tab
  d = st_make_tab(0, "", 0, 0);
  b = st_make_tab("C:\\PICATEST\\fixtures\\gamma.ini", "[x]\r\n", 0, "ini");
  ST_CHECK(c, d && b && app_active() == b, "two more tabs");
  if (base == 0) {
    frame_ctx_tab(a);
    app_command(IDM_TAB_CLOSEOTHERS);
    frame_ctx_tab(0);
    st_checkf(c, app_tab_count() == 1 && app_active() == a, "Close Others (count %d)", app_tab_count());
    ST_CHECK(c, (cmd_state(IDM_WIN_NEXT) & MF_GRAYED) != 0, "Next Tab greyed with one tab");
  }

  // every command id routes through app_command (trace only)
  g_st_trace.clear();
  cmd_set_trace(st_trace, 1);
  n = 0;
  for (i = 0; i < (int)cmd_count(); i++) {
    const CmdEntry* e = cmd_at((u32)i);
    int k;
    for (k = 0; k < e->count; k++) { app_command(e->id + k); n++; }
  }
  cmd_set_trace(0, 0);
  st_checkf(c, (int)g_st_trace.size() == n && n >= 150, "%u of %d ids routed", g_st_trace.size(), n);
  g_st_trace.release();

  st_frame_progress(c);
  st_frame_settings(c);
  st_frame_copydata(c);
  st_frame_translate(c);
  st_frame_notes(c);

  // cleanup: close every tab this test made (no questions)
  for (i = app_tab_count() - 1; i >= base; i--) frame_close_tab(app_tab(i), 0);
  st_checkf(c, app_tab_count() == base, "tabs closed (count %d)", app_tab_count());
  GetWindowTextA(g_app.frame, text, sizeof(text));
  if (base == 0) {
    st_checkf(c, strcmp(text, APP_NAME) == 0, "empty title \"%s\"", text);
    ST_CHECK(c, tabbar_height() == 0 && !IsWindowVisible(g_app.tabbar), "tab bar hidden with no tabs");
  }
}
