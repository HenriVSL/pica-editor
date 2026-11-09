// frame.cpp - the main window: class, creation, layout, WM_* routing, exit flow, window placement
// (DESIGN 15.2, 15.4, 15.11). T00: working shell with the menu, accelerators and status bar; T07 adds the toolbar,
// tab bar, notification bar host, results splitter, title details, Window/MRU menu lists, drag and drop.
// Owner after Wave 0: T07.
#include <windows.h>
#include <commctrl.h>
#include "app.h"
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
#include "selftest.h"
#include "winenc.h"
#include "frame_int.h"
#include "estr.h"
#include "resource.h"

static LRESULT CALLBACK frame_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp);

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
  wc.hbrBackground = (HBRUSH)(COLOR_APPWORKSPACE + 1);
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
  } else {
    ShowWindow(g_app.frame, c->win_max ? SW_SHOWMAXIMIZED : show_cmd);
  }
  UpdateWindow(g_app.frame);
  app_update_title();
  app_update_status();
  return g_app.frame;
}

void app_layout()
{
  RECT rc;
  int top = 0, bottom;
  if (!g_app.frame) return;
  GetClientRect(g_app.frame, &rc);
  bottom = rc.bottom;
  if (g_app.statusbar && IsWindowVisible(g_app.statusbar)) {
    RECT sr;
    SendMessageA(g_app.statusbar, WM_SIZE, 0, 0);
    GetWindowRect(g_app.statusbar, &sr);
    bottom -= sr.bottom - sr.top;
    statusbar_layout();
  }
  if (g_app.textview) MoveWindow(g_app.textview, 0, top, rc.right, bottom - top, TRUE);
  if (g_app.hexview) MoveWindow(g_app.hexview, 0, top, rc.right, bottom - top, TRUE);
}

void app_update_title()
{
  char title[400], name[280];
  Tab* t = app_active();
  if (!g_app.frame) return;
  if (!t) {
    SetWindowTextA(g_app.frame, APP_NAME);
    return;
  }
  tab_title(t, name, sizeof(name));
  es_fmt(title, sizeof(title), "%s%s - %s%s", name, t->doc && t->doc->modified() ? " *" : "", APP_NAME,
         t->doc && t->doc->read_only() ? " [Read-only]" : "");
  SetWindowTextA(g_app.frame, title);
}

void app_apply_config()
{
  if (g_app.textview) textview_config_changed(g_app.textview);
  if (g_app.hexview) hexview_config_changed(g_app.hexview);
  app_layout();
  app_update_status();
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

void frame_close()
{
  int r, i;
  if (g_app.exiting) return;
  g_app.exiting = 1;
  r = sessmgr_exit();
  if (r == 0) { g_app.exiting = 0; return; }
  if (r < 0) {                                   // sessmgr not implemented yet: ask per tab
    for (i = app_tab_count() - 1; i >= 0; i--) {
      if (docio_close(app_tab(i), 1) == 0) { g_app.exiting = 0; return; }
    }
  }
  save_placement();
  app_save_config();                             // E_NOTIMPL until T05/T06 land
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
    rc = docio_open(path, 0, &t);
    if (rc < 0) app_status_note("Could not open %s (%s)", path, e_errname(rc));
  }
  DragFinish(drop);
}

static LRESULT CALLBACK frame_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_CREATE:
    g_app.frame = w;
    statusbar_create(w);
    g_app.notifybar = notifybar_create(w);
    g_app.textview = textview_create(w, ID_TEXTVIEW, 0);
    g_app.hexview = hexview_create(w, ID_HEXVIEW, 0);
    g_app.results = results_create(w);
    g_app.isearch = isearch_create(w);
    SetTimer(w, TIMER_POLL, 3000, 0);
    return 0;
  case WM_SIZE:
    app_layout();
    return 0;
  case WM_COMMAND:
    if (LOWORD(wp) >= 40000 && LOWORD(wp) <= 40999) { app_command(LOWORD(wp)); return 0; }
    break;
  case WM_INITMENUPOPUP:
    if (!HIWORD(lp)) cmd_update_menu((HMENU)wp);
    return 0;
  case WM_TIMER:
    if (wp == TIMER_NOTE) statusbar_note_expired();
    else if (wp == TIMER_POLL) watch_poll(0);
    else if (wp == TIMER_BACKUP || wp == TIMER_SESSION) sessmgr_timer((UINT)wp);
    return 0;
  case WM_ACTIVATEAPP:
    sessmgr_app_activated(wp ? 1 : 0);
    if (wp) watch_poll(1);
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
  case WM_DROPFILES:
    open_dropped((HDROP)wp);
    return 0;
  case WM_APP_CARET:
    app_update_status();
    return 0;
  case WM_APP_DOCSTATE:
    app_update_title();
    app_update_status();
    return 0;
  case WM_APP_IODONE:
    iothread_on_done(wp, lp);
    return 0;
  case WM_APP_INDEX:
    largefile_on_msg(wp, lp);
    return 0;
  case WM_APP_FIF:
    fif_on_msg(wp, lp);
    return 0;
  case WM_CLOSE:
    frame_close();
    return 0;
  case WM_DESTROY:
    KillTimer(w, TIMER_POLL);
    KillTimer(w, TIMER_NOTE);
    g_app.frame = 0;
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

// ---- selftest (T07 fills it) ----
void st_frame(StCtx* c) { st_skip(c, "not implemented"); }

// ---- command handlers owned by the frame (T07) ----
void cmd_view_toolbar(void) { ui_notimpl("cmd_view_toolbar"); }
void cmd_view_statusbar(void) { ui_notimpl("cmd_view_statusbar"); }
void cmd_view_results(void) { ui_notimpl("cmd_view_results"); }
void cmd_win_next(void) { ui_notimpl("cmd_win_next"); }
void cmd_win_prev(void) { ui_notimpl("cmd_win_prev"); }
void cmd_win_activate(int i) { (void)i; ui_notimpl("cmd_win_activate"); }
void cmd_tab_close(void) { ui_notimpl("cmd_tab_close"); }
void cmd_tab_closeothers(void) { ui_notimpl("cmd_tab_closeothers"); }
void cmd_tab_copypath(void) { ui_notimpl("cmd_tab_copypath"); }
void cmd_tab_openfolder(void) { ui_notimpl("cmd_tab_openfolder"); }
UINT cmdstate_view_toolbar(void) { return 0; }
UINT cmdstate_view_statusbar(void) { return 0; }
UINT cmdstate_view_results(void) { return 0; }
UINT cmdstate_win_cycle(void) { return 0; }
UINT cmdstate_win(int i) { (void)i; return 0; }
