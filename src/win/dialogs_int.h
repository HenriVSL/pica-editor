// dialogs_int.h - private helpers shared by T12's dialog files (dlggoto.cpp, dlgopts.cpp, dlgmisc.cpp) and their
// selftest (st_dialogs in dlgmisc.cpp). Not for other modules.
#ifndef PICA_DIALOGS_INT_H
#define PICA_DIALOGS_INT_H

#include <windows.h>
#include "etypes.h"

struct Tab;

// Go To: evaluates the input like the dialog does; 1 valid (*pos set), 0 error/empty (msg explains).
int  dlg_goto_eval(Tab* t, int hex_mode, const char* text, epos* pos, char* msg, u32 cap);
HWND dlg_goto_modeless(Tab* t, int hex_mode);     // selftest/screenshots: the same dialog, modeless (IDOK applies)
void dlg_goto_apply(Tab* t, int hex_mode, epos pos);

// Options: the property sheet created modeless (PSH_MODELESS) for the selftest; the caller destroys it when
// PropSheet_GetCurrentPageHwnd returns NULL (after OK/Cancel).
HWND dlg_options_modeless(int page);
int  dlg_options_applied();                       // applies since the last call (count), for the selftest

HWND dlg_properties_modeless(Tab* t);
HWND dlg_about_modeless();

// Number with thousands separators: 81920 -> "81,920".
void dlg_fmt_num(u32 v, char* out, u32 cap);

#endif
