// doccmds.cpp - Document menu commands and doccmd_offer_utf8 (DESIGN 12, 15.4). STUB from T00. Owner after Wave 0: T09.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "notifybar.h"

void doccmd_offer_utf8(Tab* t, const char* msg) { (void)t; app_status_note("%s", msg ? msg : ""); }

// ---- command handlers ----
void cmd_lang_auto(void) { ui_notimpl("cmd_lang_auto"); }
void cmd_lang_plain(void) { ui_notimpl("cmd_lang_plain"); }
void cmd_lang_set(int i) { (void)i; ui_notimpl("cmd_lang_set"); }
void cmd_enc_reload_ansi(void) { ui_notimpl("cmd_enc_reload_ansi"); }
void cmd_enc_reload_oem(void) { ui_notimpl("cmd_enc_reload_oem"); }
void cmd_enc_reload_utf8(void) { ui_notimpl("cmd_enc_reload_utf8"); }
void cmd_enc_reload_utf16le(void) { ui_notimpl("cmd_enc_reload_utf16le"); }
void cmd_enc_reload_utf16be(void) { ui_notimpl("cmd_enc_reload_utf16be"); }
void cmd_enc_reload_cp(int i) { (void)i; ui_notimpl("cmd_enc_reload_cp"); }
void cmd_enc_conv_ansi(void) { ui_notimpl("cmd_enc_conv_ansi"); }
void cmd_enc_conv_utf8(void) { ui_notimpl("cmd_enc_conv_utf8"); }
void cmd_enc_conv_utf8bom(void) { ui_notimpl("cmd_enc_conv_utf8bom"); }
void cmd_enc_conv_utf16le(void) { ui_notimpl("cmd_enc_conv_utf16le"); }
void cmd_enc_conv_utf16be(void) { ui_notimpl("cmd_enc_conv_utf16be"); }
void cmd_eol_crlf(void) { ui_notimpl("cmd_eol_crlf"); }
void cmd_eol_lf(void) { ui_notimpl("cmd_eol_lf"); }
void cmd_eol_cr(void) { ui_notimpl("cmd_eol_cr"); }
void cmd_doc_tabw2(void) { ui_notimpl("cmd_doc_tabw2"); }
void cmd_doc_tabw4(void) { ui_notimpl("cmd_doc_tabw4"); }
void cmd_doc_tabw8(void) { ui_notimpl("cmd_doc_tabw8"); }
void cmd_doc_tabspaces(void) { ui_notimpl("cmd_doc_tabspaces"); }
void cmd_doc_readonly(void) { ui_notimpl("cmd_doc_readonly"); }
void cmd_doc_properties(void) { ui_notimpl("cmd_doc_properties"); }
UINT cmdstate_lang_auto(void) { return 0; }
UINT cmdstate_lang_plain(void) { return 0; }
UINT cmdstate_lang(int i) { (void)i; return 0; }
UINT cmdstate_enc_reload(void) { return 0; }
UINT cmdstate_enc_reload_cp(int i) { (void)i; return 0; }
UINT cmdstate_enc_conv_ansi(void) { return 0; }
UINT cmdstate_enc_conv_utf8(void) { return 0; }
UINT cmdstate_enc_conv_utf8bom(void) { return 0; }
UINT cmdstate_enc_conv_utf16le(void) { return 0; }
UINT cmdstate_enc_conv_utf16be(void) { return 0; }
UINT cmdstate_eol_crlf(void) { return 0; }
UINT cmdstate_eol_lf(void) { return 0; }
UINT cmdstate_eol_cr(void) { return 0; }
UINT cmdstate_doc_tabw2(void) { return 0; }
UINT cmdstate_doc_tabw4(void) { return 0; }
UINT cmdstate_doc_tabw8(void) { return 0; }
UINT cmdstate_doc_tabspaces(void) { return 0; }
UINT cmdstate_doc_readonly(void) { return 0; }
