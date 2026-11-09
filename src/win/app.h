// app.h - the application object and frame-level functions (DESIGN 15.1). Owner after Wave 0: overseer.
// Implemented by: main.cpp (g_app, app_now_ms, app_dispatch, app_idle_work, app_save_config), tabs.cpp (tab list and
// app_activate/app_update_tab/app_on_doc_changed), frame.cpp (app_update_title, app_layout, app_apply_config),
// statusbar.cpp (app_status_note, app_update_status), cmdtable.cpp (app_command).
#ifndef PICA_APP_H
#define PICA_APP_H

#include <windows.h>
#include "etypes.h"
#include "emem.h"
#include "config.h"
#include "appinfo.h"

struct Tab;

enum {                                   // window messages (15.1)
  WM_APP_CARET = WM_APP + 1,             // view -> frame: caret/selection moved (update status bar)
  WM_APP_DOCSTATE,                       // view -> frame: modified/overwrite changed (title, tab label)
  WM_APP_IODONE,                         // I/O worker -> UI: job finished (iothread_on_done)
  WM_APP_INDEX,                          // indexer -> UI: progress/done (largefile_on_msg)
  WM_APP_FIF,                            // FiF worker -> UI: hits/progress/done (fif_on_msg)
  WM_APP_COPYDATA_OPEN,                  // second instance -> first: open files (posted after WM_COPYDATA)
  WM_APP_NOTIFY                          // notification bar button pressed (wParam = button id)
};
enum { TIMER_POLL = 1, TIMER_BACKUP = 2, TIMER_SESSION = 3, TIMER_NOTE = 4 };   // frame timers (15.11)
enum {                                   // child window ids of the frame
  ID_TEXTVIEW = 100, ID_HEXVIEW, ID_RESULTS, ID_TABBAR, ID_TOOLBAR, ID_STATUSBAR, ID_NOTIFYBAR, ID_ISEARCH
};
enum {                                   // status bar parts (15.3)
  SB_PART_POS = 0, SB_PART_SEL, SB_PART_CHAR, SB_PART_LANG, SB_PART_ENC, SB_PART_EOL, SB_PART_MODE, SB_PART_VIEW,
  SB_PART_FLAGS, SB_PARTS
};

struct App {
  HINSTANCE hinst;
  HWND frame, toolbar, tabbar, statusbar, notifybar, isearch, results;
  HWND textview, hexview;                // the two main views (one shown at a time)
  HWND find_dlg;                         // modeless Find dialog or NULL (message routing 15.5)
  HACCEL accel, accel_find;              // IDA_MAIN, IDA_FIND
  Config cfg;
  char cfg_dir[260];                     // settings, session, backups (13.1); no trailing backslash
  char exe_dir[260];                     // folder of pica.exe (langs\ overrides); no trailing backslash
  Vec<Tab*> tabs;                        // tab bar order
  int active;                            // index into tabs, -1 if none
  int selftest;                          // -selftest given (ui_ask answers come from the queue)
  int bench, nosession, multi;           // -bench, -nosession, -multi
  u32 next_tabid, next_untitled;
  int exiting;                           // exit flow running
};
extern App g_app;

int  app_tab_count();
Tab* app_tab(int i);
Tab* app_active();                       // NULL if no tab
int  app_active_index();
void app_activate(int i);
void app_command(int id);                // routes through cmdtable.cpp
void app_status_note(const char* fmt, ...);   // transient note in the flags part (5 s)
void app_update_title();
void app_update_status();
void app_update_tab(Tab* t);             // tab label (modified marker, saving/indexing suffix)
void app_layout();                       // reposition the frame's children
void app_apply_config();                 // after Options OK/Apply
void app_on_doc_changed(Tab* t);         // a document changed (title, tab label, session/backup hooks)
u32  app_now_ms();                       // GetTickCount
// T00 additions (needed by several modules)
int  app_add_tab(Tab* t, int activate);  // append to the tab bar; E_NOMEM
void app_remove_tab(Tab* t);             // remove from the tab bar (does not free)
int  app_tab_index(Tab* t);              // -1 if not in the bar
void app_dispatch(MSG* m);               // one message: Find dialog accelerators, IsDialogMessage, IDA_MAIN, dispatch
int  app_idle_work();                    // one idle slice (highlighter, backups); 1 = more work pending
int  app_save_config();                  // writes <cfg_dir>\pica.ini

#endif
