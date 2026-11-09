// cmds.h - command handlers and menu state functions (DESIGN 15.1, 15.4). Owner after Wave 0: overseer.
// Every IDM_* in res/resource.h maps to one handler in cmdtable.cpp. Range commands (MRU, languages, code pages,
// window list) take the index within their range. State functions return MF_GRAYED and/or MF_CHECKED bits
// (0 = enabled, unchecked); they are called from WM_INITMENUPOPUP through cmd_update_menu().
#ifndef PICA_CMDS_H
#define PICA_CMDS_H

#include <windows.h>
#include "etypes.h"

// filecmds.cpp (T09)
void cmd_file_new(void);
void cmd_file_open(void);
void cmd_file_revert(void);
void cmd_file_save(void);
void cmd_file_saveas(void);
void cmd_file_saveall(void);
void cmd_file_close(void);
void cmd_file_closeall(void);
void cmd_file_closeothers(void);
void cmd_file_exit(void);
void cmd_mru_clear(void);
void cmd_mru_open(int i);
UINT cmdstate_file_revert(void);
UINT cmdstate_file_save(void);
UINT cmdstate_file_saveall(void);
UINT cmdstate_file_closeothers(void);
UINT cmdstate_mru_clear(void);

// editcmds.cpp (T12)
void cmd_edit_undo(void);
void cmd_edit_redo(void);
void cmd_edit_cut(void);
void cmd_edit_copy(void);
void cmd_edit_paste(void);
void cmd_edit_delete(void);
void cmd_edit_selectall(void);
void cmd_line_dup(void);
void cmd_line_del(void);
void cmd_line_up(void);
void cmd_line_down(void);
void cmd_edit_comment(void);
void cmd_edit_upper(void);
void cmd_edit_lower(void);
void cmd_edit_indent(void);
void cmd_edit_unindent(void);
void cmd_edit_overwrite(void);
void cmd_search_goto(void);
void cmd_search_brace(void);
void cmd_bm_toggle(void);
void cmd_bm_next(void);
void cmd_bm_prev(void);
void cmd_bm_clear(void);
void cmd_tools_options(void);
UINT cmdstate_edit_undo(void);
UINT cmdstate_edit_redo(void);
UINT cmdstate_edit_cut(void);
UINT cmdstate_edit_copy(void);
UINT cmdstate_edit_paste(void);
UINT cmdstate_edit_delete(void);
UINT cmdstate_edit_lineops(void);
UINT cmdstate_edit_overwrite(void);
UINT cmdstate_search_brace(void);
UINT cmdstate_bm(void);

// dlgfind.cpp (T11)
void cmd_search_find(void);
void cmd_search_next(void);
void cmd_search_prev(void);
void cmd_search_selnext(void);
void cmd_search_selprev(void);
void cmd_search_replace(void);
void cmd_search_clearmarks(void);
UINT cmdstate_search_clearmarks(void);

// isearch.cpp (T11)
void cmd_search_isearch(void);

// fifpanel.cpp (T11)
void cmd_search_fif(void);
void cmd_search_resnext(void);
void cmd_search_resprev(void);
UINT cmdstate_search_results(void);

// textview.cpp (T08)
void cmd_view_wrap(void);
void cmd_view_showws(void);
void cmd_view_linenum(void);
UINT cmdstate_view_wrap(void);
UINT cmdstate_view_showws(void);
UINT cmdstate_view_linenum(void);

// hexview.cpp (T10)
void cmd_view_hex(void);
void cmd_view_inspector(void);
UINT cmdstate_view_hex(void);
UINT cmdstate_view_inspector(void);

// frame.cpp (T07)
void cmd_view_toolbar(void);
void cmd_view_statusbar(void);
void cmd_view_results(void);
void cmd_win_next(void);
void cmd_win_prev(void);
void cmd_win_activate(int i);
void cmd_tab_close(void);
void cmd_tab_closeothers(void);
void cmd_tab_copypath(void);
void cmd_tab_openfolder(void);
UINT cmdstate_view_toolbar(void);
UINT cmdstate_view_statusbar(void);
UINT cmdstate_view_results(void);
UINT cmdstate_win_cycle(void);
UINT cmdstate_win(int i);

// doccmds.cpp (T09)
void cmd_lang_auto(void);
void cmd_lang_plain(void);
void cmd_lang_set(int i);
void cmd_enc_reload_ansi(void);
void cmd_enc_reload_oem(void);
void cmd_enc_reload_utf8(void);
void cmd_enc_reload_utf16le(void);
void cmd_enc_reload_utf16be(void);
void cmd_enc_reload_cp(int i);
void cmd_enc_conv_ansi(void);
void cmd_enc_conv_utf8(void);
void cmd_enc_conv_utf8bom(void);
void cmd_enc_conv_utf16le(void);
void cmd_enc_conv_utf16be(void);
void cmd_eol_crlf(void);
void cmd_eol_lf(void);
void cmd_eol_cr(void);
void cmd_doc_tabw2(void);
void cmd_doc_tabw4(void);
void cmd_doc_tabw8(void);
void cmd_doc_tabspaces(void);
void cmd_doc_readonly(void);
void cmd_doc_properties(void);
UINT cmdstate_lang_auto(void);
UINT cmdstate_lang_plain(void);
UINT cmdstate_lang(int i);
UINT cmdstate_enc_reload(void);
UINT cmdstate_enc_reload_cp(int i);
UINT cmdstate_enc_conv_ansi(void);
UINT cmdstate_enc_conv_utf8(void);
UINT cmdstate_enc_conv_utf8bom(void);
UINT cmdstate_enc_conv_utf16le(void);
UINT cmdstate_enc_conv_utf16be(void);
UINT cmdstate_eol_crlf(void);
UINT cmdstate_eol_lf(void);
UINT cmdstate_eol_cr(void);
UINT cmdstate_doc_tabw2(void);
UINT cmdstate_doc_tabw4(void);
UINT cmdstate_doc_tabw8(void);
UINT cmdstate_doc_tabspaces(void);
UINT cmdstate_doc_readonly(void);

// cmdtable.cpp (T15)
void cmd_help_keys(void);
void cmd_help_regex(void);
void cmd_help_about(void);

// cmdtable.cpp (T15): routing
UINT cmdstate_hastab(void);               // MF_GRAYED when no tab is open
struct CmdEntry {
  int id; int count;                        // count > 1: range id .. id + count - 1
  void (*fn)(void); void (*fni)(int i);     // one of them
  UINT (*st)(void); UINT (*sti)(int i);     // may both be NULL (always enabled)
  const char* name;                         // handler name (logs, selftest)
};
const CmdEntry* cmd_find(int id, int* index);   // NULL if id is not a command; *index = id - entry id
u32  cmd_count();  const CmdEntry* cmd_at(u32 i);
UINT cmd_state(int id);                         // state bits for one id
void cmd_update_menu(HMENU popup);              // applies cmd_state to every item of a popup (WM_INITMENUPOPUP)
typedef void (*CmdTraceFn)(int id);
void cmd_set_trace(CmdTraceFn fn, int trace_only);  // every app_command(id) calls fn(id) first; trace_only: skip the handler
void cmd_help_show(int section);                    // cmdtable.cpp (T15): help tab at a section (stub)

#endif
