// tabs.cpp - Tab lifecycle, the tab list, activation (view attach), and the tab bar control: owner-drawn
// SysTabControl32 with a close box on the active tab, middle-click close, drag reorder, path tooltips and the
// context menu (DESIGN 15.1, 15.2). Owner: T07.
#include <windows.h>
#include <commctrl.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "ui.h"
#include "cmds.h"
#include "views.h"
#include "notifybar.h"
#include "sessmgr.h"
#include "watch.h"
#include "selftest.h"
#include "frame_int.h"
#include "estr.h"
#include "resource.h"

#ifndef TCS_FOCUSNEVER
#define TCS_FOCUSNEVER 0x8000
#endif

// ---- document listener per tab: label/title refresh and app_on_doc_changed, deferred to the idle slice ----

class TabWatch : public DocListener {
public:
  Tab* t; Document* d; int changed; int modified;
  void doc_changed(Document* doc, const EditEvent& e) {
    (void)doc; (void)e;
    changed = 1;
    frame_ui_dirty();
  }
};
static Vec<TabWatch*> g_watch;

static TabWatch* watch_of(Tab* t)
{
  u32 i;
  for (i = 0; i < g_watch.size(); i++) if (g_watch[i]->t == t) return g_watch[i];
  return 0;
}

static void watch_attach(Tab* t)
{
  TabWatch* w;
  if (!t || !t->doc || watch_of(t)) return;
  w = new TabWatch;
  if (!w) return;
  w->t = t; w->d = t->doc; w->changed = 0; w->modified = t->doc->modified() ? 1 : 0;
  if (g_watch.push(w) != E_OK || t->doc->add_listener(w) != E_OK) {
    if (g_watch.size() && g_watch.back() == w) g_watch.pop();
    delete w;
  }
}

static void watch_detach(Tab* t)
{
  u32 i;
  for (i = 0; i < g_watch.size(); i++) {
    TabWatch* w = g_watch[i];
    if (w->t != t) continue;
    if (w->d) w->d->remove_listener(w);
    g_watch.erase(i, 1);
    delete w;
    return;
  }
}

// ---- Tab lifecycle ----

static void tab_zero(Tab* t)
{
  t->id = 0; t->doc = 0; t->hl = 0; t->lang = 0;
  t->lang_forced = t->enc_forced = t->untitled = 0; t->view = VIEW_TEXT;
  t->untitled_no = 0; t->path[0] = 0;
  t->sel.anchor = t->sel.caret = 0;
  memset(&t->tv, 0, sizeof(t->tv));
  memset(&t->hv, 0, sizeof(t->hv));
  memset(&t->disk, 0, sizeof(t->disk));
  t->disk_missing = t->shared_ro = t->change_pending = t->ondemand = 0;
  t->fsrc = 0;
  t->large = t->indexing = t->saving = t->ro_attr = 0;
  t->index_pct = t->save_pct = 0;
  memset(&t->notify, 0, sizeof(t->notify));
  t->backup_gen = t->backup_seq = t->dirty_since_ms = 0;
  t->has_backup = t->backup_note_shown = 0; t->pad[0] = t->pad[1] = 0;
  t->io_pending = 0;
}

Tab* tab_new(Document* doc)
{
  Tab* t = new Tab;
  if (!t) { if (doc) delete doc; return 0; }
  tab_zero(t);
  t->hl = new Highlighter;
  if (!t->hl) { delete t; if (doc) delete doc; return 0; }
  t->doc = doc;
  t->id = g_app.next_tabid++;
  t->tv.tab_width = (u8)g_app.cfg.tab_width;
  t->tv.use_spaces = g_app.cfg.use_spaces;
  t->tv.wrap = g_app.cfg.wrap_default;
  t->hv.caret.insert = (u8)(g_app.cfg.hex_overwrite ? 0 : 1);
  if (doc) {
    t->lines.bind(doc, t->hl);
    t->bookmarks.attach(doc);
    t->marks.attach(doc);
    t->hl->attach(&t->lines, 0);
    watch_attach(t);
  }
  return t;
}

void tab_free(Tab* t)
{
  if (!t) return;
  watch_detach(t);
  t->marks.detach();
  t->bookmarks.detach();
  t->lines.unbind();
  if (t->hl) { t->hl->detach(); delete t->hl; }
  if (t->doc) delete t->doc;
  delete t;
}

Tab* tab_find_path(const char* path)
{
  u32 i;
  if (!path || !path[0]) return 0;
  for (i = 0; i < g_app.tabs.size(); i++) {
    Tab* t = g_app.tabs[i];
    if (!t->untitled && t->path[0] && ui_path_equal(t->path, path)) return t;
  }
  return 0;
}

Tab* tab_by_id(u32 id)
{
  u32 i;
  for (i = 0; i < g_app.tabs.size(); i++) if (g_app.tabs[i]->id == id) return g_app.tabs[i];
  return 0;
}

void tab_title(Tab* t, char* out, u32 cap)
{
  if (t->untitled || !t->path[0]) es_fmt(out, cap, "new %u", t->untitled_no);
  else es_copy(out, cap, ui_path_name(t->path));
}

void tab_label(Tab* t, char* out, u32 cap)
{
  char name[280];
  tab_title(t, name, sizeof(name));
  es_copy(out, cap, name);
  if (t->doc && t->doc->modified()) es_cat(out, cap, " *");
  if (t->saving) es_cat(out, cap, " (saving)");
  if (t->indexing) {
    char pct[32];
    es_fmt(pct, sizeof(pct), " (indexing %u%%)", t->index_pct);
    es_cat(out, cap, pct);
  }
}

int app_tab_count() { return (int)g_app.tabs.size(); }
Tab* app_tab(int i) { return (i >= 0 && i < (int)g_app.tabs.size()) ? g_app.tabs[(u32)i] : 0; }
Tab* app_active() { return app_tab(g_app.active); }
int app_active_index() { return g_app.active; }

int app_tab_index(Tab* t)
{
  u32 i;
  for (i = 0; i < g_app.tabs.size(); i++) if (g_app.tabs[i] == t) return (int)i;
  return -1;
}

// ---- activation ----

static Tab* g_shown_tab = 0;         // tab the views were last attached for, and its view
static int g_shown_view = -1;

static int focus_in_frame()
{
  HWND f = GetFocus();
  if (!f) return GetActiveWindow() == g_app.frame;
  return f == g_app.frame || IsChild(g_app.frame, f);
}

static void detach_views()
{
  if (g_app.textview) textview_attach(g_app.textview, 0);
  if (g_app.hexview) hexview_attach(g_app.hexview, 0);
  g_shown_tab = 0;
  g_shown_view = -1;
}

void app_activate(int i)
{
  Tab* t;
  int refocus;
  if (i < 0 || i >= (int)g_app.tabs.size()) i = g_app.tabs.size() ? 0 : -1;
  refocus = focus_in_frame();
  g_app.active = i;
  t = app_active();
  if (t != g_shown_tab || !t || (int)t->view != g_shown_view) {
    detach_views();                  // views save their state into the old tab
    if (t) {
      if (t->view == VIEW_HEX) { if (g_app.hexview) hexview_attach(g_app.hexview, t); }
      else if (g_app.textview) textview_attach(g_app.textview, t);
      g_shown_tab = t;
      g_shown_view = t->view;
    }
  }
  if (g_app.tabbar && (int)SendMessageA(g_app.tabbar, TCM_GETCURSEL, 0, 0) != i)
    SendMessageA(g_app.tabbar, TCM_SETCURSEL, (WPARAM)i, 0);
  notifybar_update();
  statusbar_layout();
  app_layout();
  if (refocus && t) {
    HWND v = t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
    if (v && IsWindowVisible(v)) SetFocus(v);
  }
  if (g_app.tabbar) InvalidateRect(g_app.tabbar, 0, FALSE);   // the close box moves with the selection
  app_update_title();
  app_update_status();
  frame_ui_dirty();
  sessmgr_state_changed();
}

static int insert_item(int i)
{
  TCITEMA it;
  char label[400];
  Tab* t = app_tab(i);
  if (!g_app.tabbar || !t) return -1;
  tab_label(t, label, sizeof(label));
  es_cat(label, sizeof(label), "      ");                     // room for the close box (drawn by tabbar_draw)
  it.mask = TCIF_TEXT | TCIF_PARAM;
  it.pszText = label;
  it.lParam = (LPARAM)t->id;
  return (int)SendMessageA(g_app.tabbar, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&it);
}

int app_add_tab(Tab* t, int activate)
{
  int was_empty = g_app.tabs.size() == 0;
  if (!t) return E_STATE;
  if (g_app.tabs.push(t) != E_OK) return E_NOMEM;
  watch_attach(t);
  insert_item((int)g_app.tabs.size() - 1);
  watch_tab_added(t);
  if (was_empty) app_layout();                                  // the tab bar appears
  if (activate || g_app.active < 0) app_activate((int)g_app.tabs.size() - 1);
  else { frame_ui_dirty(); sessmgr_state_changed(); }
  return E_OK;
}

void app_remove_tab(Tab* t)
{
  int i = app_tab_index(t);
  if (i < 0) return;
  if (t == g_shown_tab || i == g_app.active) detach_views();
  g_app.tabs.erase((u32)i, 1);
  if (g_app.tabbar) SendMessageA(g_app.tabbar, TCM_DELETEITEM, (WPARAM)i, 0);
  watch_tab_removed(t);
  frame_ctx_tab(0);
  if (g_app.active > i) {
    g_app.active--;
    if (g_app.tabbar) SendMessageA(g_app.tabbar, TCM_SETCURSEL, (WPARAM)g_app.active, 0);
    frame_ui_dirty();
    sessmgr_state_changed();
  } else if (g_app.active == i) {
    int n = (int)g_app.tabs.size();
    g_app.active = -1;
    app_activate(n == 0 ? -1 : (i < n ? i : n - 1));
  } else {
    frame_ui_dirty();
    sessmgr_state_changed();
  }
  if (g_app.tabs.size() == 0) app_layout();                     // the tab bar disappears
}

void app_update_tab(Tab* t)
{
  TCITEMA it;
  char label[400];
  int i = app_tab_index(t);
  TabWatch* w = watch_of(t);
  if (i < 0) return;
  if (w) { w->modified = t->doc && t->doc->modified() ? 1 : 0; }
  if (g_app.tabbar) {
    char old[400];
    old[0] = 0;
    it.mask = TCIF_TEXT;
    it.pszText = old;
    it.cchTextMax = sizeof(old);
    SendMessageA(g_app.tabbar, TCM_GETITEMA, (WPARAM)i, (LPARAM)&it);
    tab_label(t, label, sizeof(label));
    es_cat(label, sizeof(label), "      ");
    if (strcmp(old, label) != 0) {
      it.mask = TCIF_TEXT;
      it.pszText = label;
      SendMessageA(g_app.tabbar, TCM_SETITEMA, (WPARAM)i, (LPARAM)&it);
    }
    InvalidateRect(g_app.tabbar, 0, FALSE);
  }
  if (t == app_active()) {
    app_update_title();
    app_update_status();
  }
  frame_ui_dirty();
}

void app_on_doc_changed(Tab* t)
{
  TabWatch* w = watch_of(t);
  if (w) w->changed = 0;
  app_update_tab(t);
  sessmgr_doc_changed(t);
}

void tabs_idle()
{
  u32 i;
  for (i = 0; i < g_watch.size(); i++) {
    TabWatch* w = g_watch[i];
    Tab* t = w->t;
    int mod;
    if (!w->changed) continue;
    w->changed = 0;
    if (app_tab_index(t) < 0) continue;
    mod = t->doc && t->doc->modified() ? 1 : 0;
    sessmgr_doc_changed(t);
    if (mod != w->modified) app_update_tab(t);                  // label "*" / title
    else if (t == app_active()) app_update_status();            // EOL counts, size
  }
}

// ---- tab bar control ----

static WNDPROC g_tab_old = 0;
static int g_tab_h = 0;
static int g_drag_idx = -1, g_dragging = 0, g_drag_x = 0, g_drag_y = 0;
static int g_close_down = 0, g_close_hot = 0;

int tabbar_height()
{
  return (g_app.tabbar && g_app.tabs.size()) ? g_tab_h : 0;
}

void tabbar_rebuild()
{
  u32 i;
  if (!g_app.tabbar) return;
  SendMessageA(g_app.tabbar, WM_SETREDRAW, FALSE, 0);
  SendMessageA(g_app.tabbar, TCM_DELETEALLITEMS, 0, 0);
  for (i = 0; i < g_app.tabs.size(); i++) insert_item((int)i);
  if (g_app.active >= 0) SendMessageA(g_app.tabbar, TCM_SETCURSEL, (WPARAM)g_app.active, 0);
  SendMessageA(g_app.tabbar, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_app.tabbar, 0, TRUE);
}

int tabbar_move(int from, int to)
{
  Tab* t;
  Tab* active = app_active();
  int n = (int)g_app.tabs.size();
  if (from < 0 || from >= n || to < 0 || to >= n) return E_RANGE;
  if (from == to) return E_OK;
  t = g_app.tabs[(u32)from];
  g_app.tabs.erase((u32)from, 1);
  g_app.tabs.insert((u32)to, &t, 1);                             // cannot fail: capacity is unchanged
  g_app.active = app_tab_index(active);
  tabbar_rebuild();
  frame_ui_dirty();
  sessmgr_state_changed();
  return E_OK;
}

static int item_rect(int i, RECT* r)
{
  return g_app.tabbar && SendMessageA(g_app.tabbar, TCM_GETITEMRECT, (WPARAM)i, (LPARAM)r) != 0;
}

int tabbar_close_rect(int i, RECT* r)
{
  RECT ir;
  int h = 11, w = 13;
  if (i < 0 || i != g_app.active || !item_rect(i, &ir)) { SetRectEmpty(r); return 0; }
  r->right = ir.right - 4;
  r->left = r->right - w;
  r->top = ir.top + (ir.bottom - ir.top - h) / 2;
  r->bottom = r->top + h;
  return 1;
}

static int hit_test(int x, int y)
{
  TCHITTESTINFO ht;
  ht.pt.x = x; ht.pt.y = y; ht.flags = 0;
  return (int)SendMessageA(g_app.tabbar, TCM_HITTEST, 0, (LPARAM)&ht);
}

void tabbar_draw(const DRAWITEMSTRUCT* d)
{
  char label[400];
  RECT r = d->rcItem, cr;
  Tab* t = app_tab((int)d->itemID);
  int sel = (int)d->itemID == g_app.active;
  HFONT old;
  if (!t) return;
  FillRect(d->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
  tab_label(t, label, sizeof(label));
  old = (HFONT)SelectObject(d->hDC, GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(d->hDC, TRANSPARENT);
  SetTextColor(d->hDC, GetSysColor(t->saving || t->indexing ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
  r.left += 6;
  if (sel && tabbar_close_rect((int)d->itemID, &cr)) {
    r.right = cr.left - 2;
    OffsetRect(&cr, 0, d->rcItem.top - cr.top + (d->rcItem.bottom - d->rcItem.top - (cr.bottom - cr.top)) / 2);
    DrawFrameControl(d->hDC, &cr, DFC_CAPTION,
                     DFCS_CAPTIONCLOSE | (g_close_down && g_close_hot ? DFCS_PUSHED : 0));
  } else {
    r.right -= 4;
  }
  DrawTextA(d->hDC, label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
  SelectObject(d->hDC, old);
}

const char* tabbar_tooltip(int i)
{
  static char tip[400];
  Tab* t = app_tab(i);
  if (!t) return "";
  if (t->untitled || !t->path[0]) es_fmt(tip, sizeof(tip), "new %u (not saved yet)", t->untitled_no);
  else es_copy(tip, sizeof(tip), t->path);
  return tip;
}

static void tab_context_menu(int i, int sx, int sy)
{
  HMENU m = LoadMenuA(g_app.hinst, MAKEINTRESOURCEA(IDR_TABMENU));
  HMENU sub = m ? GetSubMenu(m, 0) : 0;
  Tab* t = app_tab(i);
  int cmd;
  if (!sub || !t) { if (m) DestroyMenu(m); return; }
  if (t->untitled || !t->path[0]) {
    EnableMenuItem(sub, IDM_TAB_COPYPATH, MF_BYCOMMAND | MF_GRAYED);
    EnableMenuItem(sub, IDM_TAB_OPENFOLDER, MF_BYCOMMAND | MF_GRAYED);
  }
  if (g_app.tabs.size() < 2) EnableMenuItem(sub, IDM_TAB_CLOSEOTHERS, MF_BYCOMMAND | MF_GRAYED);
  cmd = (int)TrackPopupMenu(sub, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                            sx, sy, 0, g_app.frame, 0);
  DestroyMenu(m);
  if (cmd) {
    frame_ctx_tab(t);
    app_command(cmd);
    frame_ctx_tab(0);
  }
}

static void post_close(Tab* t)
{
  if (t) PostMessageA(g_app.frame, WM_APP_TABCLOSE, (WPARAM)t->id, 0);
}

static LRESULT CALLBACK tab_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
  switch (msg) {
  case WM_LBUTTONDOWN: {
    RECT cr;
    POINT p;
    p.x = x; p.y = y;
    if (tabbar_close_rect(g_app.active, &cr) && PtInRect(&cr, p)) {
      g_close_down = 1; g_close_hot = 1;
      SetCapture(w);
      InvalidateRect(w, &cr, FALSE);
      return 0;
    }
    g_drag_idx = hit_test(x, y);
    g_drag_x = x; g_drag_y = y; g_dragging = 0;
    break;                                                     // the control selects the tab (TCN_SELCHANGE)
  }
  case WM_MOUSEMOVE:
    if (g_close_down) {
      RECT cr;
      POINT p;
      int hot;
      p.x = x; p.y = y;
      hot = tabbar_close_rect(g_app.active, &cr) && PtInRect(&cr, p);
      if (hot != g_close_hot) { g_close_hot = hot; InvalidateRect(w, &cr, FALSE); }
      return 0;
    }
    if (g_drag_idx >= 0 && (wp & MK_LBUTTON)) {
      int target;
      if (!g_dragging) {
        if (x - g_drag_x < 5 && g_drag_x - x < 5 && y - g_drag_y < 5 && g_drag_y - y < 5) break;
        g_dragging = 1;
        SetCapture(w);
      }
      target = hit_test(x, y < 2 ? 2 : y);
      if (target < 0) {                                        // beyond the last tab / before the first
        RECT r;
        int n = (int)g_app.tabs.size();
        if (n && item_rect(n - 1, &r) && x >= r.right) target = n - 1;
        else if (n && item_rect(0, &r) && x < r.left) target = 0;
      }
      if (target >= 0 && target != g_drag_idx) {
        tabbar_move(g_drag_idx, target);
        g_drag_idx = target;
      }
      SetCursor(LoadCursorA(0, (LPCSTR)IDC_SIZEWE));
      return 0;
    }
    break;
  case WM_LBUTTONUP:
    if (g_close_down) {
      int hit = g_close_hot;
      g_close_down = g_close_hot = 0;
      ReleaseCapture();
      InvalidateRect(w, 0, FALSE);
      if (hit) post_close(app_active());
      return 0;
    }
    if (g_dragging) {
      g_dragging = 0;
      g_drag_idx = -1;
      ReleaseCapture();
      SetCursor(LoadCursorA(0, (LPCSTR)IDC_ARROW));
      return 0;
    }
    g_drag_idx = -1;
    break;
  case WM_CAPTURECHANGED:
    if ((HWND)lp != w) {
      if (g_close_down) InvalidateRect(w, 0, FALSE);
      g_close_down = g_close_hot = 0;
      g_dragging = 0;
      g_drag_idx = -1;
    }
    break;
  case WM_MBUTTONDOWN:
    return 0;
  case WM_MBUTTONUP: {
    int i = hit_test(x, y);
    if (i >= 0) post_close(app_tab(i));
    return 0;
  }
  case WM_RBUTTONDOWN:
    return 0;
  case WM_RBUTTONUP: {
    int i = hit_test(x, y);
    POINT p;
    if (i < 0) return 0;
    p.x = x; p.y = y;
    ClientToScreen(w, &p);
    tab_context_menu(i, p.x, p.y);
    return 0;
  }
  default:
    break;
  }
  return CallWindowProcA(g_tab_old, w, msg, wp, lp);
}

HWND tabbar_create(HWND parent)
{
  RECT r;
  HWND w = CreateWindowExA(0, WC_TABCONTROLA, "",
                           WS_CHILD | WS_CLIPSIBLINGS | TCS_TABS | TCS_SINGLELINE | TCS_OWNERDRAWFIXED |
                           TCS_TOOLTIPS | TCS_FOCUSNEVER,
                           0, 0, 400, 30, parent, (HMENU)ID_TABBAR, g_app.hinst, 0);
  if (!w) return 0;
  SendMessageA(w, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
  g_app.tabbar = w;
  // strip height: the tab row only (the "page" part of the control is covered by the view below)
  r.left = 0; r.top = 0; r.right = 400; r.bottom = 200;
  SendMessageA(w, TCM_ADJUSTRECT, FALSE, (LPARAM)&r);
  g_tab_h = r.top - 1;
  if (g_tab_h < 16) g_tab_h = 22;
  g_tab_old = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)tab_proc);
  return w;
}
