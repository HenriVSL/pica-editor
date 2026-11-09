// docio.cpp - open/new/save/reload/revert/close (DESIGN 13.2-13.4). STUB from T00. Owner after Wave 0: T09 (T14 in Wave 3).
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "selftest.h"

void docio_default_opts(OpenOpts* o)
{
  o->force_enc = 0; o->hex = 0; o->read_only = 0; o->no_activate = 0;
  o->enc = enc_ansi(); o->line = 0; o->from_session = 0; o->pad[0] = o->pad[1] = o->pad[2] = 0;
}
int docio_open(const char* path, const OpenOpts* o, Tab** out) { (void)path; (void)o; if (out) *out = 0; return E_NOTIMPL; }
int docio_new(Tab** out) { if (out) *out = 0; return E_NOTIMPL; }
int docio_save(Tab* t, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null, SaveDone cb, void* ctx)
{ (void)t; (void)path_or_null; (void)enc_or_null; (void)eol_or_null; (void)cb; (void)ctx; return E_NOTIMPL; }
int docio_save_wait(Tab* t, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null)
{ (void)t; (void)path_or_null; (void)enc_or_null; (void)eol_or_null; return E_NOTIMPL; }
int docio_reload(Tab* t, const EncSpec* as_or_null, int undoable) { (void)t; (void)as_or_null; (void)undoable; return E_NOTIMPL; }
int docio_revert(Tab* t) { (void)t; return E_NOTIMPL; }
int docio_close(Tab* t, int ask) { (void)t; (void)ask; return E_NOTIMPL; }

// ---- selftest ----
void st_docio(StCtx* c) { st_skip(c, "not implemented"); }
