// notifybar.cpp - per-tab notification bar (DESIGN 15.2): one bar per tab (state in Tab::notify), shown under the
// tab bar while its tab is active. Info colours, an icon, the text, up to 4 buttons and a close box. A button
// click posts WM_APP_NOTIFY to the frame, which hides the bar and then runs the callback. Owner: T07.
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "notifybar.h"
#include "selftest.h"
#include "frame_int.h"
#include "estr.h"

#define NB_CLASS APP_NAME "NotifyBar"
enum { NB_BTN_ID0 = 100, NB_BTN_H = 21, NB_GAP = 6, NB_CLOSE_W = 14, NB_CLOSE_H = 12 };

static HWND g_bar = 0;
static HWND g_btn[NB_MAX_BUTTONS];
static int g_shown = 0;
static int g_height = 28;
static Tab* g_for = 0;                      // tab whose bar the window shows
static u32 g_click_shown_ms = 0;            // shown_ms of the bar a click belongs to (stale clicks are ignored)
static int g_close_down = 0;

static int warning_kind(int kind)
{
  return kind == NB_CHANGED || kind == NB_MISSING || kind == NB_CONVERT || kind == NB_READONLY ||
         kind == NB_SHARED_RO || kind == NB_BINARY;
}

static void close_rect(RECT* r)
{
  RECT rc;
  GetClientRect(g_bar, &rc);
  r->right = rc.right - 6;
  r->left = r->right - NB_CLOSE_W;
  r->top = (rc.bottom - 1 - NB_CLOSE_H) / 2;
  r->bottom = r->top + NB_CLOSE_H;
}

static int text_right()
{
  int i, x;
  RECT r;
  close_rect(&r);
  x = r.left - NB_GAP;
  for (i = 0; i < NB_MAX_BUTTONS; i++) {
    if (!g_btn[i]) continue;
    {
      RECT br;
      GetWindowRect(g_btn[i], &br);
      MapWindowPoints(0, g_bar, (POINT*)&br, 2);
      if (br.left - 10 < x) x = br.left - 10;
    }
  }
  return x;
}

static void layout_buttons()
{
  RECT r, rc;
  HDC dc;
  HFONT old;
  int i, x, y;
  if (!g_bar) return;
  GetClientRect(g_bar, &rc);
  close_rect(&r);
  x = r.left - NB_GAP;
  y = (rc.bottom - 1 - NB_BTN_H) / 2;
  dc = GetDC(g_bar);
  old = (HFONT)SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
  for (i = NB_MAX_BUTTONS - 1; i >= 0; i--) {
    char text[64];
    SIZE sz;
    int w;
    if (!g_btn[i]) continue;
    GetWindowTextA(g_btn[i], text, sizeof(text));
    if (!GetTextExtentPoint32A(dc, text, (int)es_len(text), &sz)) sz.cx = 60;
    w = sz.cx + 20;
    if (w < 75) w = 75;
    x -= w;
    MoveWindow(g_btn[i], x, y, w, NB_BTN_H, TRUE);
    x -= NB_GAP;
  }
  SelectObject(dc, old);
  ReleaseDC(g_bar, dc);
}

static void destroy_buttons()
{
  int i;
  for (i = 0; i < NB_MAX_BUTTONS; i++) {
    if (g_btn[i]) DestroyWindow(g_btn[i]);
    g_btn[i] = 0;
  }
}

static void build_for(Tab* t)
{
  u32 i;
  destroy_buttons();
  g_for = t;
  for (i = 0; i < t->notify.nbuttons && i < NB_MAX_BUTTONS; i++) {
    g_btn[i] = CreateWindowExA(0, "BUTTON", t->notify.buttons[i].text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, 75, NB_BTN_H, g_bar, (HMENU)(NB_BTN_ID0 + i), g_app.hinst, 0);
    if (g_btn[i]) SendMessageA(g_btn[i], WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
  }
  layout_buttons();
  InvalidateRect(g_bar, 0, TRUE);
}

static void set_shown(int on)
{
  if (on == g_shown) return;
  g_shown = on;
  if (!on) { destroy_buttons(); g_for = 0; }
  ShowWindow(g_bar, on ? SW_SHOWNA : SW_HIDE);
  app_layout();
}

static void focus_view()
{
  Tab* t = app_active();
  HWND v = t && t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
  if (v && IsWindowVisible(v)) SetFocus(v);
  else if (g_app.frame) SetFocus(g_app.frame);
}

static void post_click(Tab* t, int button_id)
{
  g_click_shown_ms = t->notify.shown_ms;
  PostMessageA(g_app.frame, WM_APP_NOTIFY, (WPARAM)button_id, (LPARAM)t->id);
}

static void paint(HDC dc)
{
  RECT rc, r;
  HICON ic;
  HFONT old;
  Tab* t = g_for;
  GetClientRect(g_bar, &rc);
  FillRect(dc, &rc, GetSysColorBrush(COLOR_INFOBK));
  r = rc; r.top = r.bottom - 1;
  FillRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
  if (!t) return;
  ic = (HICON)LoadImageA(0, MAKEINTRESOURCEA(warning_kind(t->notify.kind) ? 32515 : 32516), IMAGE_ICON, 16, 16,
                         LR_SHARED);
  if (!ic) ic = LoadIconA(0, (LPCSTR)(warning_kind(t->notify.kind) ? IDI_EXCLAMATION : IDI_ASTERISK));
  if (ic) DrawIconEx(dc, 6, (rc.bottom - 1 - 16) / 2, ic, 16, 16, 0, 0, DI_NORMAL);
  old = (HFONT)SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_INFOTEXT));
  r.left = 28; r.top = 0; r.right = text_right(); r.bottom = rc.bottom - 1;
  DrawTextA(dc, t->notify.text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(dc, old);
  close_rect(&r);
  DrawFrameControl(dc, &r, DFC_CAPTION, DFCS_CAPTIONCLOSE | DFCS_FLAT | (g_close_down ? DFCS_PUSHED : 0));
}

static LRESULT CALLBACK bar_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    paint(dc);
    EndPaint(w, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_SIZE:
    layout_buttons();
    InvalidateRect(w, 0, TRUE);
    return 0;
  case WM_COMMAND:
    if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) >= NB_BTN_ID0 && LOWORD(wp) < NB_BTN_ID0 + NB_MAX_BUTTONS) {
      Tab* t = g_for;
      u32 i = LOWORD(wp) - NB_BTN_ID0;
      if (t && i < t->notify.nbuttons) post_click(t, t->notify.buttons[i].id);
      focus_view();
      return 0;
    }
    break;
  case WM_LBUTTONDOWN: {
    RECT r;
    POINT p;
    p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
    close_rect(&r);
    if (PtInRect(&r, p)) { g_close_down = 1; SetCapture(w); InvalidateRect(w, &r, FALSE); }
    return 0;
  }
  case WM_LBUTTONUP: {
    RECT r;
    POINT p;
    p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
    if (!g_close_down) return 0;
    g_close_down = 0;
    ReleaseCapture();
    close_rect(&r);
    InvalidateRect(w, &r, FALSE);
    if (PtInRect(&r, p)) notifybar_close_click();
    return 0;
  }
  case WM_CAPTURECHANGED:
    if (g_close_down && (HWND)lp != w) { g_close_down = 0; InvalidateRect(w, 0, FALSE); }
    break;
  default:
    break;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

HWND notifybar_create(HWND parent)
{
  WNDCLASSA wc;
  TEXTMETRICA tm;
  HDC dc;
  HFONT old;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = bar_proc;
  wc.hInstance = g_app.hinst;
  wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
  wc.lpszClassName = NB_CLASS;
  RegisterClassA(&wc);                         // fails harmlessly if already registered
  g_bar = CreateWindowExA(0, NB_CLASS, "", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 100, 28, parent,
                          (HMENU)ID_NOTIFYBAR, g_app.hinst, 0);
  dc = GetDC(0);
  old = (HFONT)SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
  if (GetTextMetricsA(dc, &tm)) g_height = (int)tm.tmHeight + 14;
  SelectObject(dc, old);
  ReleaseDC(0, dc);
  if (g_height < NB_BTN_H + 7) g_height = NB_BTN_H + 7;
  g_app.notifybar = g_bar;
  return g_bar;
}

void notifybar_show(Tab* t, int kind, const char* text, const NotifyButton* b, u32 n, NotifyFn fn, void* ctx)
{
  NotifyState* s;
  u32 i;
  if (!t) return;
  s = &t->notify;
  s->kind = kind == NB_NONE ? NB_INFO : kind;
  es_copy(s->text, sizeof(s->text), text ? text : "");
  if (n > NB_MAX_BUTTONS) n = NB_MAX_BUTTONS;
  s->nbuttons = 0;
  for (i = 0; i < n && b; i++) {
    if (!b[i].id) continue;
    s->buttons[s->nbuttons].id = b[i].id;
    es_copy(s->buttons[s->nbuttons].text, sizeof(s->buttons[0].text), b[i].text);
    s->nbuttons++;
  }
  s->fn = fn;
  s->ctx = ctx;
  s->shown_ms = GetTickCount();
  if (s->shown_ms == 0) s->shown_ms = 1;
  s->auto_hide_ms = 0;
  selftest_log("notify bar (kind %d): %s", s->kind, s->text);
  if (t == app_active() && g_bar) {
    build_for(t);
    set_shown(1);
  }
}

void notifybar_hide(Tab* t, int kind)
{
  if (!t) return;
  if (kind != NB_NONE && t->notify.kind != kind) return;
  t->notify.kind = NB_NONE;
  t->notify.fn = 0;
  t->notify.ctx = 0;
  if (g_bar && (t == g_for || t == app_active())) set_shown(0);
}

void notifybar_update()
{
  Tab* t = app_active();
  if (!g_bar) return;
  if (t && t->notify.kind != NB_NONE) {
    if (g_for != t || !g_shown) build_for(t);
    set_shown(1);
  } else {
    set_shown(0);
  }
}

int notifybar_height()
{
  return g_bar && g_shown ? g_height : 0;
}

void notifybar_on_msg(WPARAM w, LPARAM l)
{
  Tab* t = tab_by_id((u32)l);
  NotifyFn fn;
  void* ctx;
  int kind;
  if (!t || t->notify.kind == NB_NONE || t->notify.shown_ms != g_click_shown_ms) return;   // stale click
  fn = t->notify.fn;
  ctx = t->notify.ctx;
  kind = t->notify.kind;
  notifybar_hide(t, NB_NONE);
  if (fn) fn(t, kind, (int)w, ctx);
}

void notifybar_close_click()
{
  Tab* t = g_for ? g_for : app_active();
  if (!t || t->notify.kind == NB_NONE) return;
  post_click(t, 0);
}

void notifybar_tick()
{
  u32 i, now = GetTickCount();
  for (i = 0; i < g_app.tabs.size(); i++) {
    Tab* t = g_app.tabs[i];
    NotifyState* s = &t->notify;
    if (s->kind == NB_NONE || !s->auto_hide_ms || now - s->shown_ms < s->auto_hide_ms) continue;
    {
      NotifyFn fn = s->fn;
      void* ctx = s->ctx;
      int kind = s->kind;
      notifybar_hide(t, NB_NONE);
      if (fn) fn(t, kind, 0, ctx);
    }
    return;                                     // the callback may have changed the tab list
  }
}

HWND notifybar_button(int index)
{
  if (index >= 0 && index < NB_MAX_BUTTONS) return g_btn[index];
  return 0;
}
