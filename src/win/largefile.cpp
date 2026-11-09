// largefile.cpp - on-demand documents and the indexer thread (DESIGN 14.1). STUB from T00. Owner after Wave 0: T14.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "largefile.h"
#include "selftest.h"

int largefile_begin(Tab* t) { (void)t; return E_NOTIMPL; }
void largefile_cancel(Tab* t) { (void)t; }
void largefile_on_msg(WPARAM w, LPARAM l) { (void)w; (void)l; }

// ---- selftest ----
void st_largefile(StCtx* c) { st_skip(c, "not implemented"); }
