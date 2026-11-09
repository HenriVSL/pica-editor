// dlgfind.cpp - Find/Replace dialog and search commands (DESIGN 11.4). STUB from T00. Owner after Wave 0: T11.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "selftest.h"



// ---- selftest ----
void st_search(StCtx* c) { st_skip(c, "not implemented"); }

// ---- command handlers ----
void cmd_search_find(void) { ui_notimpl("cmd_search_find"); }
void cmd_search_next(void) { ui_notimpl("cmd_search_next"); }
void cmd_search_prev(void) { ui_notimpl("cmd_search_prev"); }
void cmd_search_selnext(void) { ui_notimpl("cmd_search_selnext"); }
void cmd_search_selprev(void) { ui_notimpl("cmd_search_selprev"); }
void cmd_search_replace(void) { ui_notimpl("cmd_search_replace"); }
void cmd_search_clearmarks(void) { ui_notimpl("cmd_search_clearmarks"); }
UINT cmdstate_search_clearmarks(void) { return 0; }
