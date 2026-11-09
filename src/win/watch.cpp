// watch.cpp - change detection (DESIGN 13.9). STUB from T00. Owner after Wave 0: T14.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "watch.h"
#include "selftest.h"

u32 watch_handles(HANDLE* out, u32 max) { (void)out; (void)max; return 0; }
void watch_signalled(u32 i) { (void)i; }
void watch_poll(int all) { (void)all; }
void watch_tab_added(Tab* t) { (void)t; }
void watch_tab_removed(Tab* t) { (void)t; }
void watch_tab_saved(Tab* t) { (void)t; }

// ---- selftest ----
void st_watch(StCtx* c) { st_skip(c, "not implemented"); }
