// hexview.cpp - hex view window and data inspector (DESIGN 9). STUB from T00. Owner after Wave 0: T10.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "views.h"
#include "selftest.h"

HWND hexview_create(HWND parent, int id, int is_panel) { (void)parent; (void)id; (void)is_panel; return 0; }
void hexview_attach(HWND w, Tab* t) { (void)w; (void)t; }
void hexview_set_selection(HWND w, epos anchor, epos caret, int scroll_mode) { (void)w; (void)anchor; (void)caret; (void)scroll_mode; }
void hexview_ensure_visible(HWND w, epos pos, int center) { (void)w; (void)pos; (void)center; }
void hexview_invalidate(HWND w) { (void)w; }
void hexview_config_changed(HWND w) { (void)w; }
void hexview_caret_info(HWND w, CaretInfo* out) { (void)w; memset(out, 0, sizeof(*out)); }
void hexview_set_panel_handler(HWND w, PanelHandler* h) { (void)w; (void)h; }
void hexview_set_inspector(HWND w, int on) { (void)w; (void)on; }

// ---- selftest ----
void st_hexview(StCtx* c) { st_skip(c, "not implemented"); }

// ---- command handlers ----
void cmd_view_hex(void) { ui_notimpl("cmd_view_hex"); }
void cmd_view_inspector(void) { ui_notimpl("cmd_view_inspector"); }
UINT cmdstate_view_hex(void) { return 0; }
UINT cmdstate_view_inspector(void) { return 0; }
