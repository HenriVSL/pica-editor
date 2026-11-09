// winenc.h - Windows code pages: system ACP/OEMCP, DBCS providers, input locale, ANSI <-> ecp (DESIGN 8.3, 12).
// Implemented in winenc.cpp (T02). Owner after Wave 0: overseer.
#ifndef PICA_WINENC_H
#define PICA_WINENC_H

#include "etypes.h"
#include "emem.h"

int  winenc_init();                      // after codec_init + enc_set_system: DBCS providers (IsValidCodePage only)
u16  winenc_input_cp();                  // code page of the current input locale (WM_CHAR bytes)
void winenc_input_changed();             // call on WM_INPUTLANGCHANGE
int  winenc_cp_valid(u16 cp);            // IsValidCodePage
int  winenc_ansi_to_ecp(const char* s, u32 n, Vec<ecp>* out);              // system ACP -> code points (appends)
int  winenc_ecp_to_ansi(const ecp* s, u32 n, EBytes* out, u32* lost);     // -> ACP bytes, '?' for lost chars
int  winenc_mb_to_ecp(u16 cp, const char* s, u32 n, Vec<ecp>* out);        // CF_TEXT / WM_CHAR bytes in code page cp
int  winenc_ecp_to_mb(u16 cp, const ecp* s, u32 n, EBytes* out, u32* lost);

#endif
