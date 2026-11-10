// progress.cpp - modal progress window (UiProgress) and status-bar progress for background jobs (DESIGN 15.10).
// UiProgress appears 500 ms after construction: text, bar, Cancel. While it is up the frame is disabled; paint
// messages for every window and input for the progress window are pumped, other input is dropped. Owner: T07.
#include <windows.h>
#include <commctrl.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "progress.h"
#include "selftest.h"
#include "frame_int.h"
#include "estr.h"

#define PR_CLASS APP_NAME "Progress"
enum { PR_TEXT = 10, PR_BAR = 11, PR_W = 340, PR_H_CANCEL = 118, PR_H = 88, PR_RANGE = 1000 };

static HWND g_prog_wnd = 0;                 // the visible UiProgress window (only one at a time)

static LRESULT CALLBACK prog_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_COMMAND:
    if (LOWORD(wp) == IDCANCEL) {
      int* flag = (int*)GetWindowLongA(w, GWL_USERDATA);
      HWND b = GetDlgItem(w, IDCANCEL);
      if (flag && b && IsWindowEnabled(b)) {
        *flag = 1;
        EnableWindow(b, FALSE);
        SetDlgItemTextA(w, PR_TEXT, "Cancelling...");
      }
      return 0;
    }
    break;
  case WM_CLOSE:
    SendMessageA(w, WM_COMMAND, IDCANCEL, 0);
    return 0;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

static void register_class()
{
  static int done = 0;
  WNDCLASSA wc;
  if (done) return;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = prog_proc;
  wc.hInstance = g_app.hinst;
  wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = PR_CLASS;
  if (RegisterClassA(&wc)) done = 1;
}

UiProgress::UiProgress(const char* title, const char* text, int can_cancel)
  : can_cancel_(can_cancel), cancelled_(0), start_ms_(GetTickCount()), wnd_(0)
{
  es_copy(title_, sizeof(title_), title ? title : APP_NAME);
  es_copy(text_, sizeof(text_), text ? text : "");
}

UiProgress::~UiProgress()
{
  if (wnd_) {
    HWND focus = (HWND)GetPropA(wnd_, APP_NAME "Focus");
    RemovePropA(wnd_, APP_NAME "Focus");
    if (g_app.frame) EnableWindow(g_app.frame, TRUE);          // enable the owner first: no activation flicker
    DestroyWindow(wnd_);
    if (g_prog_wnd == wnd_) g_prog_wnd = 0;
    wnd_ = 0;
    if (focus && IsWindow(focus)) SetFocus(focus);
    else if (g_app.frame) SetFocus(g_app.frame);
  }
}

static void pump_progress(HWND wnd)
{
  MSG m;
  int n;
  // input: only for the progress window and its children (Cancel button, Esc); everything else is dropped
  for (n = 0; n < 64 && PeekMessageA(&m, 0, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE); n++) {
    if (m.hwnd == wnd || IsChild(wnd, m.hwnd)) {
      if (!IsDialogMessageA(wnd, &m)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
  }
  for (n = 0; n < 64 && PeekMessageA(&m, 0, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE); n++) {
    if (m.hwnd == wnd || IsChild(wnd, m.hwnd)) DispatchMessageA(&m);
  }
  for (n = 0; n < 64 && PeekMessageA(&m, 0, WM_NCMOUSEMOVE, WM_NCMBUTTONDBLCLK, PM_REMOVE); n++) {
    if (m.hwnd == wnd) DispatchMessageA(&m);
  }
  for (n = 0; n < 16 && PeekMessageA(&m, wnd, WM_TIMER, WM_TIMER, PM_REMOVE); n++) DispatchMessageA(&m);
  for (n = 0; n < 64 && PeekMessageA(&m, 0, WM_PAINT, WM_PAINT, PM_REMOVE); n++) DispatchMessageA(&m);
}

int UiProgress::progress(u32 done, u32 total)
{
  u32 now = GetTickCount();
  if (!wnd_) {
    RECT fr;
    int h = can_cancel_ ? PR_H_CANCEL : PR_H, x, y;
    HWND focus;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND c;
    if (now - start_ms_ < 500 || g_prog_wnd) return cancelled_;
    register_class();
    if (g_app.frame && GetWindowRect(g_app.frame, &fr)) {
      x = fr.left + (fr.right - fr.left - PR_W) / 2;
      y = fr.top + (fr.bottom - fr.top - h) / 2;
    } else {
      x = (GetSystemMetrics(SM_CXSCREEN) - PR_W) / 2;
      y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    }
    focus = GetFocus();
    wnd_ = CreateWindowExA(WS_EX_DLGMODALFRAME, PR_CLASS, title_, WS_POPUP | WS_CAPTION | WS_CLIPCHILDREN,
                           x, y, PR_W, h, g_app.frame, 0, g_app.hinst, 0);
    if (!wnd_) return cancelled_;
    SetWindowLongA(wnd_, GWL_USERDATA, (LONG)&cancelled_);
    SetPropA(wnd_, APP_NAME "Focus", (HANDLE)focus);
    c = CreateWindowExA(0, "STATIC", text_, WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX, 12, 12,
                        PR_W - 30, 16, wnd_, (HMENU)PR_TEXT, g_app.hinst, 0);
    if (c) SendMessageA(c, WM_SETFONT, (WPARAM)font, 0);
    c = CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE, 12, 32, PR_W - 30, 16, wnd_, (HMENU)PR_BAR,
                        g_app.hinst, 0);
    if (c) SendMessageA(c, PBM_SETRANGE, 0, MAKELPARAM(0, PR_RANGE));
    if (can_cancel_) {
      c = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          (PR_W - 6 - 75) / 2, 58, 75, 23, wnd_, (HMENU)IDCANCEL, g_app.hinst, 0);
      if (c) SendMessageA(c, WM_SETFONT, (WPARAM)font, 0);
    }
    if (g_app.frame) EnableWindow(g_app.frame, FALSE);
    ShowWindow(wnd_, SW_SHOWNORMAL);
    UpdateWindow(wnd_);
    if (can_cancel_ && GetDlgItem(wnd_, IDCANCEL)) SetFocus(GetDlgItem(wnd_, IDCANCEL));
    else SetFocus(wnd_);
    g_prog_wnd = wnd_;
    selftest_log("progress window: %s", text_);
  }
  if (total) {
    u32 pos = (u32)(((u64)(done > total ? total : done) * PR_RANGE) / total);
    SendDlgItemMessageA(wnd_, PR_BAR, PBM_SETPOS, (WPARAM)pos, 0);
  }
  pump_progress(wnd_);
  return cancelled_;
}

void UiProgress::set_text(const char* text)
{
  es_copy(text_, sizeof(text_), text ? text : "");
  if (wnd_ && !cancelled_) SetDlgItemTextA(wnd_, PR_TEXT, text_);
}

int UiProgress::cancelled() const
{
  return cancelled_;
}

int progress_window_visible()
{
  return g_prog_wnd && IsWindowVisible(g_prog_wnd);
}

HWND progress_window()
{
  return g_prog_wnd;
}

// ---- status-bar progress (background jobs) ----

struct StatusProg { int h; u32 pct; char label[40]; };
enum { SP_MAX = 8 };
static StatusProg g_sp[SP_MAX];
static int g_sp_next = 1;

int progress_status_begin(const char* label)
{
  int i;
  for (i = 0; i < SP_MAX; i++) {
    if (g_sp[i].h) continue;
    g_sp[i].h = g_sp_next++;
    if (g_sp_next <= 0) g_sp_next = 1;
    g_sp[i].pct = 0;
    es_copy(g_sp[i].label, sizeof(g_sp[i].label), label ? label : "Working");
    app_update_status();
    return g_sp[i].h;
  }
  return SP_MAX + 1000;                          // table full: a valid handle that shows nothing
}

void progress_status_step(int h, u32 pct)
{
  int i;
  if (pct > 100) pct = 100;
  for (i = 0; i < SP_MAX; i++) {
    if (g_sp[i].h != h || g_sp[i].pct == pct) continue;
    g_sp[i].pct = pct;
    app_update_status();
  }
}

void progress_status_end(int h)
{
  int i;
  for (i = 0; i < SP_MAX; i++) {
    if (g_sp[i].h != h) continue;
    g_sp[i].h = 0;
    app_update_status();
  }
}

int progress_status_text(char* out, u32 cap)
{
  int i, n = 0;
  out[0] = 0;
  for (i = 0; i < SP_MAX; i++) {
    char one[64];
    if (!g_sp[i].h) continue;
    es_fmt(one, sizeof(one), "%s %u%%", g_sp[i].label, g_sp[i].pct);
    if (n) es_cat(out, cap, ", ");
    es_cat(out, cap, one);
    n++;
  }
  return n;
}
