// fifworker_int.h - private to T11's win modules: dlgfind.cpp, isearch.cpp, fifpanel.cpp, fifworker.cpp.
#ifndef PICA_FIFWORKER_INT_H
#define PICA_FIFWORKER_INT_H

#include <windows.h>
#include "etypes.h"
#include "emem.h"
#include "fif.h"
#include "search.h"

struct Tab;

// ---- fifpanel.cpp: the results document and its hit table ----
void results_begin(const char* header);                  // clears, writes the header line, shows the panel
void results_set_running(int on);                        // Stop button and caption state
int  results_add_file(const char* path, u32 tab_id, u32 change_seq);   // tab_id != 0: hits of an open tab (Find All)
int  results_add_hit(const FifHit& h);                   // hit of the last added file
void results_flush();                                    // after a batch: marks, undo memory, repaint
void results_progress(u32 done, u32 seen, const char* current);        // caption text while running
void results_finish(const char* header);                 // final header line (also closes the last file line)
void results_note(const char* text);                     // caption text (idle)
u32  results_hit_count();
u32  results_file_count();
int  results_open_hit(u32 i, int focus_editor);          // opens/activates the file and selects the match; E_*
int  results_current();                                  // index of the current hit (F4), -1 if none
Tab* results_tab();                                      // the read-only results Tab (not in the tab bar)
int  results_translate(MSG* m);                          // Ctrl+C / Ctrl+A in the panel (see HANDOFF)
int  results_is_open();                                  // panel visible

// ---- fifworker.cpp ----
u32  fif_last_skipped();                                 // files that could not be read in the last run
int  fif_last_result();                                  // on_done result of the last finished run (1 = running/none)
u32  fif_runs_finished();                                // counts finished runs (selftests wait on it)
void fif_abandon();                                      // cancel and drop the running search (its batches are ignored)

// ---- dlgfind.cpp ----
void finddlg_open(int page);                             // 0 Find, 1 Replace, 2 Find in Files (prefilled)
void finddlg_fif_state(int running);                     // Stop button in the dialog
void search_select(Tab* t, epos anchor, epos caret, int scroll_mode);  // selection in the tab's view
void search_ecp_to_utf8(const ecp* s, u32 n, char* out, u32 cap);      // bad markers -> U+FFFD, truncates
void search_header(const ecp* pat, u32 n, char* out, u32 cap);         // "Search \"pat\"" (pattern cut at 100 chars)

// ---- isearch.cpp ----
int  isearch_focused();                                  // the bar's edit has the focus
void isearch_next(int backward);                         // Enter / F3 / Ctrl+I in the bar
int  isearch_translate(MSG* m);                          // editing keys stay in the bar's edit (see HANDOFF)

#endif
