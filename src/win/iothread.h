// iothread.h - the I/O worker thread (DESIGN 4.3, 13.3, 13.5, 13.6). Implemented in iothread.cpp (T09 save jobs,
// T13 backup/session/delete jobs). Owner after Wave 0: overseer.
#ifndef PICA_IOTHREAD_H
#define PICA_IOTHREAD_H

#include <windows.h>
#include "etypes.h"
#include "emem.h"
#include "esource.h"
#include "fileops.h"
#include "session.h"

enum IoJobKind { IOJ_SAVE_TEMP = 1, IOJ_BACKUP = 2, IOJ_SESSION = 3, IOJ_DELETE = 4 };
struct IoJob {                 // allocated with new by the submitter; owned by the queue; freed by iothread_on_done
  u32 id;                      // set by iothread_submit
  u32 kind;                    // IoJobKind
  u32 tabid;                   // 0 = not tied to a tab
  volatile i32 cancel;         // set with InterlockedExchange; jobs check it between 64 KB chunks
  int result;                  // E_* when done
  char path[260];              // SAVE_TEMP: target file; BACKUP: backup folder; SESSION: session1.ini; DELETE: file
  char path2[260];             // SESSION: session2.ini
  ByteSource* data;            // SAVE_TEMP/BACKUP: snapshot to write (owned by the job)
  SaveResult save;             // SAVE_TEMP: temp file name etc.
  BackupMeta meta;             // BACKUP
  EBytes* bytes;               // SESSION: file contents (owned by the job)
  int last_which, which;       // SESSION: A/B slot written last / now
  u32 seq;                     // SAVE_TEMP: undo seq at snapshot time; SESSION: session seq
  void* ctx;                   // submitter's context (e.g. SaveDone callback data)
  volatile u32 done_bytes, total_bytes;   // progress (read by the UI thread)
};
int  iothread_start();
void iothread_stop();                         // waits for the current job; queued jobs are cancelled
u32  iothread_submit(IoJob* job);             // returns the job id; 0 = failed (job deleted)
void iothread_cancel_tab(u32 tabid);
int  iothread_wait_tab(u32 tabid, u32 timeout_ms);   // 1 = no jobs left for the tab, 0 = timeout
int  iothread_flush(u32 timeout_ms);          // 1 = queue empty
void iothread_on_done(WPARAM w, LPARAM l);    // WM_APP_IODONE: hand the job to docio/sessmgr, then free it

#endif
