// fifpanel.cpp - find-in-files results panel and result commands (DESIGN 11.6). STUB from T00. Owner after Wave 0: T11.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "fifworker.h"
#include "selftest.h"

HWND results_create(HWND parent) { (void)parent; return 0; }
void results_show(int on) { (void)on; }
int results_visible() { return 0; }

// ---- selftest ----
void st_fif(StCtx* c) { st_skip(c, "not implemented"); }

// ---- command handlers ----
void cmd_search_fif(void) { ui_notimpl("cmd_search_fif"); }
void cmd_search_resnext(void) { ui_notimpl("cmd_search_resnext"); }
void cmd_search_resprev(void) { ui_notimpl("cmd_search_resprev"); }
UINT cmdstate_search_results(void) { return 0; }
