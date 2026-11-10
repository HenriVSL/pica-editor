// docio_int.h - private to T09's modules: docio.cpp, filecmds.cpp, doccmds.cpp, iothread.cpp (DESIGN 12, 13.2-13.4).
// Not a shared header: other modules use docio.h / iothread.h.
#ifndef PICA_DOCIO_INT_H
#define PICA_DOCIO_INT_H

#include <windows.h>
#include "etypes.h"
#include "codec.h"
#include "lexer.h"
#include "ui.h"

struct Tab;
struct IoJob;

enum {                                    // private question ids (ui.h: modules may use Q_USER + n)
  Q_DOCIO_CHANGED_ON_DISK = Q_USER + 1,   // save over a file that changed on disk since we read it   OK/CANCEL
  Q_DOCIO_ONDEMAND_CONVERT = Q_USER + 2   // (reserved)
};

// ---- iothread.cpp ----
int  iothread_process_done(u32 job_id);   // hands finished jobs (job_id, or all when 0) to their owners now; count
int  iothread_job_pending(u32 job_id);    // 1 while the job is queued, running or not yet handed over
int  iothread_running();                  // worker thread alive

// ---- docio.cpp ----
void docio_io_done(IoJob* job);           // SAVE_TEMP finished (UI thread): rename sequence, state, callback; frees ctx
void docio_io_abandon(IoJob* job);        // shutdown with a finished save not handed over: delete its temp file
void docio_io_progress(u32 tabid, u32 done, u32 total);
void docio_set_lang(Tab* t, const LangInfo* li, int forced);   // language + highlighter (large-mode gate)
void docio_sync_lang_menu();              // adds Document > Syntax items for override languages (index >= 13)
int  docio_wait_save(Tab* t);             // tab is saving: modal wait until the save has finished; its result
int  docio_lang_menu_ready();
Tab* docio_tab_by_id(u32 id);
// caret <-> (line, char column) so commands that rewrite all bytes (convert, reload) keep the caret's place
struct DocPlace { u32 line; u32 col; u32 aline; u32 acol; };
void docio_place_get(Tab* t, DocPlace* p);
void docio_place_set(Tab* t, const DocPlace& p);
void docio_view_refresh(Tab* t);          // after a document-wide change: views, title, status

// ---- doccmds.cpp ----
// Converts the document to 'to' (DESIGN 7.5/12.2): loss prompt, one undo step. E_OK, E_CANCEL (user said no), or
// an error (already reported to the user when report != 0).
int  doccmd_convert(Tab* t, const EncSpec& to, int report);
int  doccmd_convert_eol(Tab* t, int eol, int report);
int  doccmd_reload_as(Tab* t, const EncSpec& enc);       // menu Reload as: asks when modified

// ---- filecmds.cpp ----
// Save As dialog with the Encoding / Line endings combos. Returns 1 = OK (path, *enc_out / *eol_out set; *enc_set /
// *eol_set = 0 when the user kept the document's value), 0 = cancelled.
int  filecmd_saveas_dialog(Tab* t, char* path, u32 cap, EncSpec* enc_out, int* eol_out, int* enc_set, int* eol_set);
int  filecmd_save_as(Tab* t, int wait);   // dialog + save (wait: modal until done); E_OK, E_CANCEL, error
int  filecmd_save(Tab* t, int wait);      // Save (Save As for untitled / read-only "No"); E_OK, E_CANCEL, error
void filecmd_selftest_saveas(const char* path);   // selftest: answer for the next Save As dialog (else cancel)

#endif
