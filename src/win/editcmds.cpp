// editcmds.cpp - Edit menu, line, bookmark, go to, brace and Options commands (DESIGN 15.4). STUB from T00. Owner after Wave 0: T12.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "clip.h"
#include "dialogs.h"
#include "selftest.h"



// ---- selftest ----
void st_edit(StCtx* c) { st_skip(c, "not implemented"); }

// ---- command handlers ----
void cmd_edit_undo(void) { ui_notimpl("cmd_edit_undo"); }
void cmd_edit_redo(void) { ui_notimpl("cmd_edit_redo"); }
void cmd_edit_cut(void) { ui_notimpl("cmd_edit_cut"); }
void cmd_edit_copy(void) { ui_notimpl("cmd_edit_copy"); }
void cmd_edit_paste(void) { ui_notimpl("cmd_edit_paste"); }
void cmd_edit_delete(void) { ui_notimpl("cmd_edit_delete"); }
void cmd_edit_selectall(void) { ui_notimpl("cmd_edit_selectall"); }
void cmd_line_dup(void) { ui_notimpl("cmd_line_dup"); }
void cmd_line_del(void) { ui_notimpl("cmd_line_del"); }
void cmd_line_up(void) { ui_notimpl("cmd_line_up"); }
void cmd_line_down(void) { ui_notimpl("cmd_line_down"); }
void cmd_edit_comment(void) { ui_notimpl("cmd_edit_comment"); }
void cmd_edit_upper(void) { ui_notimpl("cmd_edit_upper"); }
void cmd_edit_lower(void) { ui_notimpl("cmd_edit_lower"); }
void cmd_edit_indent(void) { ui_notimpl("cmd_edit_indent"); }
void cmd_edit_unindent(void) { ui_notimpl("cmd_edit_unindent"); }
void cmd_edit_overwrite(void) { ui_notimpl("cmd_edit_overwrite"); }
void cmd_search_goto(void) { ui_notimpl("cmd_search_goto"); }
void cmd_search_brace(void) { ui_notimpl("cmd_search_brace"); }
void cmd_bm_toggle(void) { ui_notimpl("cmd_bm_toggle"); }
void cmd_bm_next(void) { ui_notimpl("cmd_bm_next"); }
void cmd_bm_prev(void) { ui_notimpl("cmd_bm_prev"); }
void cmd_bm_clear(void) { ui_notimpl("cmd_bm_clear"); }
void cmd_tools_options(void) { ui_notimpl("cmd_tools_options"); }
UINT cmdstate_edit_undo(void) { return 0; }
UINT cmdstate_edit_redo(void) { return 0; }
UINT cmdstate_edit_cut(void) { return 0; }
UINT cmdstate_edit_copy(void) { return 0; }
UINT cmdstate_edit_paste(void) { return 0; }
UINT cmdstate_edit_delete(void) { return 0; }
UINT cmdstate_edit_lineops(void) { return 0; }
UINT cmdstate_edit_overwrite(void) { return 0; }
UINT cmdstate_search_brace(void) { return 0; }
UINT cmdstate_bm(void) { return 0; }
