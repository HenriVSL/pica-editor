// textview.cpp - text view window (DESIGN 8). STUB from T00. Owner after Wave 0: T08.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "selftest.h"

HWND textview_create(HWND parent, int id, int is_panel) { (void)parent; (void)id; (void)is_panel; return 0; }
void textview_attach(HWND w, Tab* t) { (void)w; (void)t; }
void textview_set_selection(HWND w, epos anchor, epos caret, int scroll_mode) { (void)w; (void)anchor; (void)caret; (void)scroll_mode; }
void textview_ensure_visible(HWND w, epos pos, int center) { (void)w; (void)pos; (void)center; }
void textview_invalidate(HWND w) { (void)w; }
void textview_config_changed(HWND w) { (void)w; }
void textview_caret_info(HWND w, CaretInfo* out) { (void)w; memset(out, 0, sizeof(*out)); }
void textview_set_panel_handler(HWND w, PanelHandler* h) { (void)w; (void)h; }

// ---- selftest ----
void st_textview(StCtx* c) { st_skip(c, "not implemented"); }

// ---- command handlers ----
void cmd_view_wrap(void) { ui_notimpl("cmd_view_wrap"); }
void cmd_view_showws(void) { ui_notimpl("cmd_view_showws"); }
void cmd_view_linenum(void) { ui_notimpl("cmd_view_linenum"); }
UINT cmdstate_view_wrap(void) { return 0; }
UINT cmdstate_view_showws(void) { return 0; }
UINT cmdstate_view_linenum(void) { return 0; }
