// cmdtable.cpp - command routing: IDM_* -> handler and menu state (DESIGN 15.1, 15.4). T00: complete table.
// Also the Help commands (T15). Owner after Wave 0: T15.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "resource.h"

static const CmdEntry g_cmds[] = {
  { IDM_FILE_NEW, 1, cmd_file_new, 0, 0, 0, "cmd_file_new" },
  { IDM_FILE_OPEN, 1, cmd_file_open, 0, 0, 0, "cmd_file_open" },
  { IDM_FILE_REVERT, 1, cmd_file_revert, 0, cmdstate_file_revert, 0, "cmd_file_revert" },
  { IDM_FILE_SAVE, 1, cmd_file_save, 0, cmdstate_file_save, 0, "cmd_file_save" },
  { IDM_FILE_SAVEAS, 1, cmd_file_saveas, 0, cmdstate_hastab, 0, "cmd_file_saveas" },
  { IDM_FILE_SAVEALL, 1, cmd_file_saveall, 0, cmdstate_file_saveall, 0, "cmd_file_saveall" },
  { IDM_FILE_CLOSE, 1, cmd_file_close, 0, cmdstate_hastab, 0, "cmd_file_close" },
  { IDM_FILE_CLOSEALL, 1, cmd_file_closeall, 0, cmdstate_hastab, 0, "cmd_file_closeall" },
  { IDM_FILE_CLOSEOTHERS, 1, cmd_file_closeothers, 0, cmdstate_file_closeothers, 0, "cmd_file_closeothers" },
  { IDM_FILE_EXIT, 1, cmd_file_exit, 0, 0, 0, "cmd_file_exit" },
  { IDM_MRU_CLEAR, 1, cmd_mru_clear, 0, cmdstate_mru_clear, 0, "cmd_mru_clear" },
  { IDM_MRU_FIRST, 10, 0, cmd_mru_open, 0, 0, "cmd_mru_open" },
  { IDM_EDIT_UNDO, 1, cmd_edit_undo, 0, cmdstate_edit_undo, 0, "cmd_edit_undo" },
  { IDM_EDIT_REDO, 1, cmd_edit_redo, 0, cmdstate_edit_redo, 0, "cmd_edit_redo" },
  { IDM_EDIT_CUT, 1, cmd_edit_cut, 0, cmdstate_edit_cut, 0, "cmd_edit_cut" },
  { IDM_EDIT_COPY, 1, cmd_edit_copy, 0, cmdstate_edit_copy, 0, "cmd_edit_copy" },
  { IDM_EDIT_PASTE, 1, cmd_edit_paste, 0, cmdstate_edit_paste, 0, "cmd_edit_paste" },
  { IDM_EDIT_DELETE, 1, cmd_edit_delete, 0, cmdstate_edit_delete, 0, "cmd_edit_delete" },
  { IDM_EDIT_SELECTALL, 1, cmd_edit_selectall, 0, cmdstate_hastab, 0, "cmd_edit_selectall" },
  { IDM_LINE_DUP, 1, cmd_line_dup, 0, cmdstate_edit_lineops, 0, "cmd_line_dup" },
  { IDM_LINE_DEL, 1, cmd_line_del, 0, cmdstate_edit_lineops, 0, "cmd_line_del" },
  { IDM_LINE_UP, 1, cmd_line_up, 0, cmdstate_edit_lineops, 0, "cmd_line_up" },
  { IDM_LINE_DOWN, 1, cmd_line_down, 0, cmdstate_edit_lineops, 0, "cmd_line_down" },
  { IDM_EDIT_COMMENT, 1, cmd_edit_comment, 0, cmdstate_edit_lineops, 0, "cmd_edit_comment" },
  { IDM_EDIT_UPPER, 1, cmd_edit_upper, 0, cmdstate_edit_lineops, 0, "cmd_edit_upper" },
  { IDM_EDIT_LOWER, 1, cmd_edit_lower, 0, cmdstate_edit_lineops, 0, "cmd_edit_lower" },
  { IDM_EDIT_INDENT, 1, cmd_edit_indent, 0, cmdstate_edit_lineops, 0, "cmd_edit_indent" },
  { IDM_EDIT_UNINDENT, 1, cmd_edit_unindent, 0, cmdstate_edit_lineops, 0, "cmd_edit_unindent" },
  { IDM_EDIT_OVERWRITE, 1, cmd_edit_overwrite, 0, cmdstate_edit_overwrite, 0, "cmd_edit_overwrite" },
  { IDM_SEARCH_FIND, 1, cmd_search_find, 0, cmdstate_hastab, 0, "cmd_search_find" },
  { IDM_SEARCH_NEXT, 1, cmd_search_next, 0, cmdstate_hastab, 0, "cmd_search_next" },
  { IDM_SEARCH_PREV, 1, cmd_search_prev, 0, cmdstate_hastab, 0, "cmd_search_prev" },
  { IDM_SEARCH_SELNEXT, 1, cmd_search_selnext, 0, cmdstate_hastab, 0, "cmd_search_selnext" },
  { IDM_SEARCH_SELPREV, 1, cmd_search_selprev, 0, cmdstate_hastab, 0, "cmd_search_selprev" },
  { IDM_SEARCH_ISEARCH, 1, cmd_search_isearch, 0, cmdstate_hastab, 0, "cmd_search_isearch" },
  { IDM_SEARCH_REPLACE, 1, cmd_search_replace, 0, cmdstate_hastab, 0, "cmd_search_replace" },
  { IDM_SEARCH_FIF, 1, cmd_search_fif, 0, 0, 0, "cmd_search_fif" },
  { IDM_SEARCH_RESNEXT, 1, cmd_search_resnext, 0, cmdstate_search_results, 0, "cmd_search_resnext" },
  { IDM_SEARCH_RESPREV, 1, cmd_search_resprev, 0, cmdstate_search_results, 0, "cmd_search_resprev" },
  { IDM_SEARCH_CLEARMARKS, 1, cmd_search_clearmarks, 0, cmdstate_search_clearmarks, 0, "cmd_search_clearmarks" },
  { IDM_SEARCH_GOTO, 1, cmd_search_goto, 0, cmdstate_hastab, 0, "cmd_search_goto" },
  { IDM_SEARCH_BRACE, 1, cmd_search_brace, 0, cmdstate_search_brace, 0, "cmd_search_brace" },
  { IDM_BM_TOGGLE, 1, cmd_bm_toggle, 0, cmdstate_bm, 0, "cmd_bm_toggle" },
  { IDM_BM_NEXT, 1, cmd_bm_next, 0, cmdstate_bm, 0, "cmd_bm_next" },
  { IDM_BM_PREV, 1, cmd_bm_prev, 0, cmdstate_bm, 0, "cmd_bm_prev" },
  { IDM_BM_CLEAR, 1, cmd_bm_clear, 0, cmdstate_bm, 0, "cmd_bm_clear" },
  { IDM_VIEW_HEX, 1, cmd_view_hex, 0, cmdstate_view_hex, 0, "cmd_view_hex" },
  { IDM_VIEW_INSPECTOR, 1, cmd_view_inspector, 0, cmdstate_view_inspector, 0, "cmd_view_inspector" },
  { IDM_VIEW_WRAP, 1, cmd_view_wrap, 0, cmdstate_view_wrap, 0, "cmd_view_wrap" },
  { IDM_VIEW_SHOWWS, 1, cmd_view_showws, 0, cmdstate_view_showws, 0, "cmd_view_showws" },
  { IDM_VIEW_LINENUM, 1, cmd_view_linenum, 0, cmdstate_view_linenum, 0, "cmd_view_linenum" },
  { IDM_VIEW_TOOLBAR, 1, cmd_view_toolbar, 0, cmdstate_view_toolbar, 0, "cmd_view_toolbar" },
  { IDM_VIEW_STATUSBAR, 1, cmd_view_statusbar, 0, cmdstate_view_statusbar, 0, "cmd_view_statusbar" },
  { IDM_VIEW_RESULTS, 1, cmd_view_results, 0, cmdstate_view_results, 0, "cmd_view_results" },
  { IDM_LANG_AUTO, 1, cmd_lang_auto, 0, cmdstate_lang_auto, 0, "cmd_lang_auto" },
  { IDM_LANG_PLAIN, 1, cmd_lang_plain, 0, cmdstate_lang_plain, 0, "cmd_lang_plain" },
  { IDM_LANG_FIRST, 40, 0, cmd_lang_set, 0, cmdstate_lang, "cmd_lang_set" },
  { IDM_ENC_RELOAD_ANSI, 1, cmd_enc_reload_ansi, 0, cmdstate_enc_reload, 0, "cmd_enc_reload_ansi" },
  { IDM_ENC_RELOAD_OEM, 1, cmd_enc_reload_oem, 0, cmdstate_enc_reload, 0, "cmd_enc_reload_oem" },
  { IDM_ENC_RELOAD_UTF8, 1, cmd_enc_reload_utf8, 0, cmdstate_enc_reload, 0, "cmd_enc_reload_utf8" },
  { IDM_ENC_RELOAD_UTF16LE, 1, cmd_enc_reload_utf16le, 0, cmdstate_enc_reload, 0, "cmd_enc_reload_utf16le" },
  { IDM_ENC_RELOAD_UTF16BE, 1, cmd_enc_reload_utf16be, 0, cmdstate_enc_reload, 0, "cmd_enc_reload_utf16be" },
  { IDM_ENC_RELOAD_CP_FIRST, 20, 0, cmd_enc_reload_cp, 0, cmdstate_enc_reload_cp, "cmd_enc_reload_cp" },
  { IDM_ENC_CONV_ANSI, 1, cmd_enc_conv_ansi, 0, cmdstate_enc_conv_ansi, 0, "cmd_enc_conv_ansi" },
  { IDM_ENC_CONV_UTF8, 1, cmd_enc_conv_utf8, 0, cmdstate_enc_conv_utf8, 0, "cmd_enc_conv_utf8" },
  { IDM_ENC_CONV_UTF8BOM, 1, cmd_enc_conv_utf8bom, 0, cmdstate_enc_conv_utf8bom, 0, "cmd_enc_conv_utf8bom" },
  { IDM_ENC_CONV_UTF16LE, 1, cmd_enc_conv_utf16le, 0, cmdstate_enc_conv_utf16le, 0, "cmd_enc_conv_utf16le" },
  { IDM_ENC_CONV_UTF16BE, 1, cmd_enc_conv_utf16be, 0, cmdstate_enc_conv_utf16be, 0, "cmd_enc_conv_utf16be" },
  { IDM_EOL_CRLF, 1, cmd_eol_crlf, 0, cmdstate_eol_crlf, 0, "cmd_eol_crlf" },
  { IDM_EOL_LF, 1, cmd_eol_lf, 0, cmdstate_eol_lf, 0, "cmd_eol_lf" },
  { IDM_EOL_CR, 1, cmd_eol_cr, 0, cmdstate_eol_cr, 0, "cmd_eol_cr" },
  { IDM_TAB_2, 1, cmd_doc_tabw2, 0, cmdstate_doc_tabw2, 0, "cmd_doc_tabw2" },
  { IDM_TAB_4, 1, cmd_doc_tabw4, 0, cmdstate_doc_tabw4, 0, "cmd_doc_tabw4" },
  { IDM_TAB_8, 1, cmd_doc_tabw8, 0, cmdstate_doc_tabw8, 0, "cmd_doc_tabw8" },
  { IDM_TAB_SPACES, 1, cmd_doc_tabspaces, 0, cmdstate_doc_tabspaces, 0, "cmd_doc_tabspaces" },
  { IDM_DOC_READONLY, 1, cmd_doc_readonly, 0, cmdstate_doc_readonly, 0, "cmd_doc_readonly" },
  { IDM_DOC_PROPERTIES, 1, cmd_doc_properties, 0, cmdstate_hastab, 0, "cmd_doc_properties" },
  { IDM_TOOLS_OPTIONS, 1, cmd_tools_options, 0, 0, 0, "cmd_tools_options" },
  { IDM_WIN_NEXT, 1, cmd_win_next, 0, cmdstate_win_cycle, 0, "cmd_win_next" },
  { IDM_WIN_PREV, 1, cmd_win_prev, 0, cmdstate_win_cycle, 0, "cmd_win_prev" },
  { IDM_WIN_FIRST, 30, 0, cmd_win_activate, 0, cmdstate_win, "cmd_win_activate" },
  { IDM_HELP_KEYS, 1, cmd_help_keys, 0, 0, 0, "cmd_help_keys" },
  { IDM_HELP_REGEX, 1, cmd_help_regex, 0, 0, 0, "cmd_help_regex" },
  { IDM_HELP_ABOUT, 1, cmd_help_about, 0, 0, 0, "cmd_help_about" },
  { IDM_TAB_CLOSE, 1, cmd_tab_close, 0, 0, 0, "cmd_tab_close" },
  { IDM_TAB_CLOSEOTHERS, 1, cmd_tab_closeothers, 0, 0, 0, "cmd_tab_closeothers" },
  { IDM_TAB_COPYPATH, 1, cmd_tab_copypath, 0, 0, 0, "cmd_tab_copypath" },
  { IDM_TAB_OPENFOLDER, 1, cmd_tab_openfolder, 0, 0, 0, "cmd_tab_openfolder" },
};

static CmdTraceFn g_trace = 0;
static int g_trace_only = 0;

u32 cmd_count()
{
  return (u32)(sizeof(g_cmds) / sizeof(g_cmds[0]));
}

const CmdEntry* cmd_at(u32 i)
{
  return i < cmd_count() ? &g_cmds[i] : 0;
}

const CmdEntry* cmd_find(int id, int* index)
{
  u32 i;
  for (i = 0; i < cmd_count(); i++) {
    const CmdEntry* e = &g_cmds[i];
    if (id >= e->id && id < e->id + e->count) {
      if (index) *index = id - e->id;
      return e;
    }
  }
  return 0;
}

void cmd_set_trace(CmdTraceFn fn, int trace_only)
{
  g_trace = fn;
  g_trace_only = fn ? trace_only : 0;
}

void app_command(int id)
{
  int idx = 0;
  const CmdEntry* e = cmd_find(id, &idx);
  if (g_trace) g_trace(id);
  if (g_trace_only) return;
  if (!e) { app_status_note("Unknown command %d", id); return; }
  if (e->fn) e->fn();
  else if (e->fni) e->fni(idx);
}

UINT cmd_state(int id)
{
  int idx = 0;
  const CmdEntry* e = cmd_find(id, &idx);
  if (!e) return 0;
  if (e->st) return e->st();
  if (e->sti) return e->sti(idx);
  return 0;
}

void cmd_update_menu(HMENU popup)
{
  int i, n = GetMenuItemCount(popup);
  for (i = 0; i < n; i++) {
    UINT id = GetMenuItemID(popup, i);
    UINT st;
    if (id == 0xFFFFFFFF || id == 0) continue;     // submenu or separator
    st = cmd_state((int)id);
    EnableMenuItem(popup, (UINT)i, MF_BYPOSITION | ((st & MF_GRAYED) ? MF_GRAYED : MF_ENABLED));
    CheckMenuItem(popup, (UINT)i, MF_BYPOSITION | ((st & MF_CHECKED) ? MF_CHECKED : MF_UNCHECKED));
  }
}

UINT cmdstate_hastab(void)
{
  return app_active() ? 0 : MF_GRAYED;
}

void cmd_help_show(int section) { (void)section; ui_notimpl("cmd_help_show"); }

// ---- Help commands (T15) ----
void cmd_help_keys(void) { ui_notimpl("cmd_help_keys"); }
void cmd_help_regex(void) { ui_notimpl("cmd_help_regex"); }
void cmd_help_about(void) { ui_notimpl("cmd_help_about"); }
