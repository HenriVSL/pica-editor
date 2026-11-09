// filecmds.cpp - File menu commands (DESIGN 15.4). STUB from T00. Owner after Wave 0: T09.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"



// ---- command handlers ----
void cmd_file_new(void) { ui_notimpl("cmd_file_new"); }
void cmd_file_open(void) { ui_notimpl("cmd_file_open"); }
void cmd_file_revert(void) { ui_notimpl("cmd_file_revert"); }
void cmd_file_save(void) { ui_notimpl("cmd_file_save"); }
void cmd_file_saveas(void) { ui_notimpl("cmd_file_saveas"); }
void cmd_file_saveall(void) { ui_notimpl("cmd_file_saveall"); }
void cmd_file_close(void) { ui_notimpl("cmd_file_close"); }
void cmd_file_closeall(void) { ui_notimpl("cmd_file_closeall"); }
void cmd_file_closeothers(void) { ui_notimpl("cmd_file_closeothers"); }
void cmd_file_exit(void) { if (g_app.frame) PostMessageA(g_app.frame, WM_CLOSE, 0, 0); }   // frame_close() runs the exit flow
void cmd_mru_clear(void) { ui_notimpl("cmd_mru_clear"); }
void cmd_mru_open(int i) { (void)i; ui_notimpl("cmd_mru_open"); }
UINT cmdstate_file_revert(void) { return 0; }
UINT cmdstate_file_save(void) { return 0; }
UINT cmdstate_file_saveall(void) { return 0; }
UINT cmdstate_file_closeothers(void) { return 0; }
UINT cmdstate_mru_clear(void) { return 0; }
