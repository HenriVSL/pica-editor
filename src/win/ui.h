// ui.h - modal questions, errors, string and path helpers (DESIGN 15.1). Implemented in util.cpp (T07).
// Owner after Wave 0: overseer.
#ifndef PICA_UI_H
#define PICA_UI_H

#include <windows.h>
#include "etypes.h"
#include "emem.h"

enum UiQuestion {          // question ids: selftests queue answers per id (st_answer); add new ones before Q_USER
  Q_ANY = -1,              // ui_answer_push: matches any question
  Q_GENERIC = 0,
  Q_SAVE_CHANGES,          // "Save changes to X?"            YES/NO/CANCEL
  Q_RELOAD_DISCARD,        // "Reloading discards your changes"  OK/CANCEL
  Q_REVERT,                // "Discard all changes to X?"     OK/CANCEL
  Q_LOCKED_RETRY,          // "X is locked by another program" RETRY/CANCEL
  Q_CLEAR_READONLY,        // "X is read-only. Clear and save?" YES/NO/CANCEL
  Q_REPLACE_ALL_TABS,      // "Replace in all N open documents?" OK/CANCEL
  Q_CONVERT_LOSS,          // fallback for dlg_convert_loss
  Q_OPEN_ONDEMAND,         // out of memory: open on demand?   YES/NO
  Q_OVERWRITE_FILE,        // Save As onto an existing file (if the dialog doesn't ask)
  Q_SAVE_FAILED,           // save failed message (OK)
  Q_RELOAD_CHANGED,        // file changed on disk (modal fallback)
  Q_EXIT,                  // fallback exit question
  Q_TOO_BIG,               // file too big (OK)
  Q_ERROR,                 // ui_error in selftest mode is logged under this id
  Q_USER = 100             // modules may use Q_USER + n privately
};
// All modal questions go through here. buttons = MB_OK / MB_OKCANCEL / MB_YESNO / MB_YESNOCANCEL / MB_RETRYCANCEL
// (MB_ICON* and MB_DEFBUTTON* allowed). Returns IDOK / IDCANCEL / IDYES / IDNO / IDRETRY. In selftest mode the answer
// comes from the queue (ui_answer_push); an unexpected question is logged as a failure and answered IDCANCEL.
int  ui_ask(int qid, const char* text, int buttons);
void ui_error(const char* fmt, ...);            // message box (selftest: logged, no box)
void ui_notimpl(const char* what);              // status note "Not implemented yet (what)" - used by stubs
void ui_answer_push(int qid, int button);       // selftest answer queue (FIFO)
void ui_answer_clear();
u32  ui_answer_pending();
int  ui_unexpected_questions();                 // questions asked with no queued answer (selftest checks)
// strings in the system ANSI code page (window texts, paths)
int  ui_ansi_to_ecp(const char* s, Vec<ecp>* out);             // appends; E_NOMEM
int  ui_ecp_to_ansi(const ecp* s, u32 n, char* out, u32 cap);  // unrepresentable -> '?'; returns how many
// paths (ANSI, backslashes)
const char* ui_path_name(const char* path);                    // file name part
void ui_path_dir(const char* path, char* out, u32 cap);        // folder without trailing backslash ("C:\" stays)
void ui_path_join(const char* dir, const char* name, char* out, u32 cap);
int  ui_path_full(const char* in, char* out, u32 cap);         // GetFullPathNameA; E_RANGE, E_NOTFOUND
int  ui_path_equal(const char* a, const char* b);              // same file: case-insensitive, short names
int  ui_file_exists(const char* path);
int  ui_dir_exists(const char* path);

#endif
