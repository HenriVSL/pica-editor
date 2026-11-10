// watch_int.h - private to T14's modules: watch.cpp, largefile.cpp, docio.cpp (DESIGN 13.9, 14.1).
// Not a shared header: other modules use watch.h / largefile.h / docio.h.
#ifndef PICA_WATCH_INT_H
#define PICA_WATCH_INT_H

#include <windows.h>
#include "etypes.h"
#include "fsys.h"
#include "docio_int.h"

struct Tab;

// ---- docio.cpp ----
enum {
  RL_QUIET = 1,            // no message boxes: failures only return the code (change detection retries later)
  RL_ASK_LOSSY = 2         // a modified document whose reload can't be undone asks Q_RELOAD_DISCARD first
};
// docio_reload with flags. Keeps caret (line, column) and scroll. On-demand results index on the indexer thread
// (the caret place is restored when indexing finishes). Returns E_OK, E_CANCEL (user said no) or an error.
int  docio_reload_ex(Tab* t, const EncSpec* as_or_null, int undoable, int flags);
epos docio_ondemand_threshold();          // min(Config.ondemand_mb, totalPhys / 8), 4..64 MB

// ---- largefile.cpp ----
// Starts the indexer thread for an indexing document. goto_line (1-based, 0 = none) or place (may be NULL) is applied
// to the caret when indexing finishes. E_OK, E_STATE (not indexing), E_NOMEM (thread could not start).
int  largefile_begin_ex(Tab* t, u32 goto_line, const DocPlace* place);
int  largefile_running(Tab* t);          // 1 while t has an indexer job
void largefile_tab_added(Tab* t);        // watch_tab_added: one-time "not backed up" note listener (on-demand)
void largefile_tab_removed(Tab* t);      // watch_tab_removed

// ---- watch.cpp (selftest hooks) ----
void watch_test_set_inactive(int on);    // pretend Pica is not the active application (queueing test)
int  watch_test_state(Tab* t, int* has_candidate, int* bar_kind);   // 1 if the tab is watched
u32  watch_test_folder_count();

#endif
