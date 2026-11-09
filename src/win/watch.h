// watch.h - change detection (DESIGN 13.9). Implemented in watch.cpp (T14). Owner after Wave 0: overseer.
#ifndef PICA_WATCH_H
#define PICA_WATCH_H

#include <windows.h>
#include "etypes.h"

struct Tab;
u32  watch_handles(HANDLE* out, u32 max);   // change-notification handles for MsgWaitForMultipleObjects
void watch_signalled(u32 i);                // handle i fired
void watch_poll(int all);                   // TIMER_POLL / activation: stat files (all = every tab)
void watch_tab_added(Tab* t);
void watch_tab_removed(Tab* t);
void watch_tab_saved(Tab* t);               // our own save: record the new disk state

#endif
