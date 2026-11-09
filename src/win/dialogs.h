// dialogs.h - dialogs other modules call (DESIGN 15.1, 15.8, 15.9). dlgmisc.cpp: exit prompt, convert loss,
// properties, about; dlggoto.cpp: Go To; dlgopts.cpp: Options (all T12). Owner after Wave 0: overseer.
#ifndef PICA_DIALOGS_H
#define PICA_DIALOGS_H

#include <windows.h>
#include "etypes.h"
#include "doc.h"

struct Tab;
enum { EXIT_SAVE = 1, EXIT_KEEP, EXIT_DISCARD, EXIT_CANCEL };
enum { CONVERT_OK = 1, CONVERT_GOTO, CONVERT_CANCEL };
int  dlg_exit_prompt(Tab** tabs, u32 n, u8* save_flags);   // save_flags[i] = 1: save tab i (EXIT_SAVE)
int  dlg_convert_loss(const char* enc_label, const ConvertReport& r, u32 first_line);
void dlg_properties(Tab* t);
void dlg_about();
int  dlg_goto(Tab* t, int hex_mode);     // 1 = moved, 0 = cancelled
int  dlg_options(int page);              // 1 = settings applied
// Stubs (Wave 0) return E_NOTIMPL from the int functions: callers fall back to ui_ask.

#endif
