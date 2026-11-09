// sessmgr.cpp - session, backups, recovery, exit flow (DESIGN 13.5-13.7, 15.12). STUB from T00. Owner after Wave 0: T13.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "sessmgr.h"
#include "selftest.h"

void sessmgr_startup() {}
int sessmgr_restore_from(const char* dir) { (void)dir; return E_NOTIMPL; }
void sessmgr_doc_changed(Tab* t) { (void)t; }
void sessmgr_state_changed() {}
void sessmgr_tab_saved(Tab* t) { (void)t; }
void sessmgr_tab_closed(Tab* t) { (void)t; }
int sessmgr_exit() { return E_NOTIMPL; }
void sessmgr_emergency() {}
void sessmgr_idle() {}
void sessmgr_app_activated(int active) { (void)active; }
void sessmgr_timer(UINT id) { (void)id; }

// ---- selftest ----
void st_session(StCtx* c) { st_skip(c, "not implemented"); }
