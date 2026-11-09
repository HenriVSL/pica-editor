// isearch.cpp - incremental search bar (DESIGN 11.5). STUB from T00. Owner after Wave 0: T11.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "fifworker.h"

HWND isearch_create(HWND parent) { (void)parent; return 0; }
void isearch_show() { ui_notimpl("isearch_show"); }
void isearch_hide() {}

// ---- command handlers ----
void cmd_search_isearch(void) { ui_notimpl("cmd_search_isearch"); }
