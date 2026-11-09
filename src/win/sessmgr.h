// sessmgr.h - session, backups, recovery, exit flow (DESIGN 13.5-13.7, 15.12). Implemented in sessmgr.cpp (T13).
// Owner after Wave 0: overseer.
#ifndef PICA_SESSMGR_H
#define PICA_SESSMGR_H

#include <windows.h>
#include "etypes.h"

struct Tab;
void sessmgr_startup();                  // after the frame is visible: restore session + backups (13.7)
int  sessmgr_restore_from(const char* dir);   // selftest: restore from another cfg dir into fresh tabs
void sessmgr_doc_changed(Tab* t);        // an edit happened (backup debounce)
void sessmgr_state_changed();            // tabs opened/closed/reordered/activated, view state (session debounce)
void sessmgr_tab_saved(Tab* t);          // delete its backups, write the session
void sessmgr_tab_closed(Tab* t);
int  sessmgr_exit();                     // 1 = exit now, 0 = cancelled; E_NOTIMPL until T13 (caller prompts per file)
void sessmgr_emergency();                // SEH guard: synchronous backups + session (clean_exit=0)
void sessmgr_idle();                     // idle slice: backup scheduling
// T00 additions
void sessmgr_app_activated(int active);  // WM_ACTIVATEAPP (backup on deactivate, 13.5)
void sessmgr_timer(UINT id);             // TIMER_BACKUP / TIMER_SESSION from the frame

#endif
