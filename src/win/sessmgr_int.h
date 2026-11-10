// sessmgr_int.h - private to T13's modules: sessmgr.cpp and iothread.cpp (DESIGN 13.5-13.7, 15.12).
// Not a shared header: other modules use sessmgr.h / iothread.h.
#ifndef PICA_SESSMGR_INT_H
#define PICA_SESSMGR_INT_H

#include <windows.h>
#include "etypes.h"

struct IoJob;
class FileSys;

// ---- sessmgr.cpp ----
// iothread.cpp hands every finished BACKUP / SESSION / DELETE job here (UI thread, exactly once; the job and its
// data are freed by iothread.cpp afterwards).
void sessmgr_io_done(IoJob* j);

// ---- iothread.cpp ----
// Job conventions used by sessmgr:
//   IOJ_BACKUP:  path = backup folder, meta = header, data = bytes (MemSource copy or SnapshotSource),
//                seq = Document::change_seq() at the snapshot, ctx = submit order (u32).
//   IOJ_SESSION: path/path2 = session1.ini/session2.ini, bytes = text, last_which = slot of the newest copy,
//                seq = session seq; done: which = slot written.
//   IOJ_DELETE:  meta.tabid != 0 -> every backup generation of that tab in folder 'path' (backup_delete_tab);
//                meta.tabid == 0 -> the single file 'path'.
void iothread_set_fs(FileSys* fs);       // tests: file system of the worker (NULL = fs_os()); only with an empty queue
int  iothread_emergency_stop(u32 timeout_ms);   // cancel all jobs, wait for the worker (1 = stopped); no hand-over

#endif
