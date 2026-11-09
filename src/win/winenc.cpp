// winenc.cpp - Windows code pages, DBCS providers, input locale (DESIGN 8.3, 12). STUB from T00. Owner after Wave 0: T02.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "winenc.h"

int winenc_init() { return E_OK; }
u16 winenc_input_cp() { return (u16)GetACP(); }
void winenc_input_changed() {}
int winenc_cp_valid(u16 cp) { return IsValidCodePage(cp) ? 1 : 0; }
int winenc_ansi_to_ecp(const char* s, u32 n, Vec<ecp>* out) { (void)s; (void)n; (void)out; return E_NOTIMPL; }
int winenc_ecp_to_ansi(const ecp* s, u32 n, EBytes* out, u32* lost) { (void)s; (void)n; (void)out; if (lost) *lost = 0; return E_NOTIMPL; }
int winenc_mb_to_ecp(u16 cp, const char* s, u32 n, Vec<ecp>* out) { (void)cp; (void)s; (void)n; (void)out; return E_NOTIMPL; }
int winenc_ecp_to_mb(u16 cp, const ecp* s, u32 n, EBytes* out, u32* lost) { (void)cp; (void)s; (void)n; (void)out; if (lost) *lost = 0; return E_NOTIMPL; }
