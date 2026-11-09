// dlgmisc.cpp - exit prompt, convert-loss prompt, Properties, About (DESIGN 15.9). STUB from T00. Owner after Wave 0: T12.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "dialogs.h"
#include "selftest.h"

int dlg_exit_prompt(Tab** tabs, u32 n, u8* save_flags) { (void)tabs; (void)n; (void)save_flags; return E_NOTIMPL; }
int dlg_convert_loss(const char* enc_label, const ConvertReport& r, u32 first_line) { (void)enc_label; (void)r; (void)first_line; return E_NOTIMPL; }
void dlg_properties(Tab* t) { (void)t; ui_notimpl("dlg_properties"); }
void dlg_about() { ui_notimpl("dlg_about"); }

// ---- selftest ----
void st_dialogs(StCtx* c) { st_skip(c, "not implemented"); }
