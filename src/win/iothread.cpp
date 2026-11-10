// iothread.cpp - I/O worker thread (DESIGN 4.3, 13.3, 13.5, 13.6). T09: queue, worker, SAVE_TEMP jobs, completion
// hand-over; T13 adds the completion handling of BACKUP/SESSION/DELETE (the worker side below already runs them).
// Owner after Wave 0: T09 (T13 in Wave 3).
//
// Jobs live in one FIFO list of nodes (allocated at submit, so the worker never allocates): QUEUED -> RUNNING ->
// DONE -> handed to the UI thread (iothread_on_done / iothread_process_done) -> freed. The worker posts
// WM_APP_IODONE (wParam = job id, lParam 0) when a job is DONE, and (wParam 0, lParam = job id) as throttled progress.
// A DONE job is handed over exactly once, whichever comes first: the posted message or a synchronous wait
// (docio_save_wait, iothread_wait_tab, iothread_flush). Messages for jobs already handed over are ignored.
#include <windows.h>
#include <process.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "iothread.h"
#include "docio_int.h"
#include "fsys.h"
#include "fileops.h"
#include "estr.h"

enum { NODE_QUEUED = 0, NODE_RUNNING = 1, NODE_DONE = 2 };
struct IoNode { IoJob* job; IoNode* next; int state; };

static CRITICAL_SECTION g_cs;
static int g_cs_ready = 0;
static HANDLE g_thread = 0;
static HANDLE g_wake = 0;                 // auto-reset: work queued or stop requested
static volatile LONG g_stop = 0;
static IoNode* g_head = 0;                // FIFO: oldest first
static u32 g_next_id = 1;

static void io_lock() { EnterCriticalSection(&g_cs); }
static void io_unlock() { LeaveCriticalSection(&g_cs); }

// ---- worker side ----

class IoProgress : public ProgressCb {    // worker thread: records progress, posts a throttled note to the UI
public:
  IoProgress(IoJob* j) : job_(j), last_ms_(0) {}
  int progress(u32 done, u32 total)
  {
    u32 now = GetTickCount();
    job_->done_bytes = done;
    job_->total_bytes = total;
    if (now - last_ms_ >= 200) {
      last_ms_ = now;
      if (g_app.frame) PostMessageA(g_app.frame, WM_APP_IODONE, 0, (LPARAM)job_->id);
    }
    return job_->cancel ? 1 : 0;
  }
private:
  IoJob* job_; u32 last_ms_;
};

static void io_run(IoJob* j)
{
  FileSys* fs = fs_os();
  IoProgress cb(j);
  if (j->cancel) { j->result = E_CANCEL; return; }
  switch (j->kind) {
  case IOJ_SAVE_TEMP:
    j->total_bytes = j->data ? j->data->size() : 0;
    j->result = j->data ? file_write_temp(fs, j->path, j->data, &cb, &j->cancel, &j->save) : E_STATE;
    break;
  case IOJ_BACKUP:                        // T13 owns scheduling and completion; the write itself is plain
    j->result = j->data ? backup_write(fs, j->path, j->meta, j->data, &cb, &j->cancel) : E_STATE;
    break;
  case IOJ_SESSION:
    j->result = j->bytes ? ab_write(fs, j->path, j->path2, j->last_which, j->bytes->data(), j->bytes->size(), &j->which)
                         : E_STATE;
    break;
  case IOJ_DELETE:
    j->result = fs->remove(j->path);
    break;
  default:
    j->result = E_UNSUPPORTED;
    break;
  }
}

static unsigned __stdcall io_main(void* arg)
{
  (void)arg;
  for (;;) {
    IoNode* n;
    WaitForSingleObject(g_wake, INFINITE);
    for (;;) {
      IoJob* j = 0;
      io_lock();
      for (n = g_head; n; n = n->next) if (n->state == NODE_QUEUED) break;
      if (n) { n->state = NODE_RUNNING; j = n->job; }
      io_unlock();
      if (!j) break;
      if (g_stop) InterlockedExchange((LONG*)&j->cancel, 1);
      io_run(j);
      io_lock();
      n->state = NODE_DONE;
      io_unlock();
      if (g_app.frame) PostMessageA(g_app.frame, WM_APP_IODONE, (WPARAM)j->id, 0);
    }
    if (g_stop) break;
  }
  return 0;
}

// ---- UI side ----

static void io_job_free(IoJob* j)
{
  if (!j) return;
  if (j->data) { delete j->data; j->data = 0; }
  if (j->bytes) { delete j->bytes; j->bytes = 0; }
  delete j;
}

static void io_dispatch(IoJob* j)
{
  switch (j->kind) {
  case IOJ_SAVE_TEMP:
    docio_io_done(j);                      // frees j->data and its ctx
    break;
  default:
    // T13: hand BACKUP / SESSION / DELETE results to sessmgr here (generation bookkeeping, A/B slot, retries).
    break;
  }
  io_job_free(j);
}

// Removes the first DONE node (with the given id, or any when id == 0) from the list; NULL if none.
static IoJob* io_take_done(u32 id, u32 tabid, int by_tab)
{
  IoNode* n;
  IoNode** link;
  IoJob* j = 0;
  io_lock();
  for (link = &g_head; (n = *link) != 0; link = &n->next) {
    if (n->state != NODE_DONE) continue;
    if (id && n->job->id != id) continue;
    if (by_tab && n->job->tabid != tabid) continue;
    *link = n->next;
    j = n->job;
    delete n;
    break;
  }
  io_unlock();
  return j;
}

int iothread_start()
{
  unsigned tid;
  if (g_thread) return E_OK;
  if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = 1; }
  g_stop = 0;
  g_wake = CreateEventA(0, FALSE, FALSE, 0);
  if (!g_wake) return E_NOMEM;
  g_thread = (HANDLE)_beginthreadex(0, 0, io_main, 0, 0, &tid);
  if (!g_thread) { CloseHandle(g_wake); g_wake = 0; return E_NOMEM; }
  return E_OK;
}

int iothread_running()
{
  return g_thread != 0 && !g_stop;
}

void iothread_stop()
{
  IoNode* n;
  IoJob* j;
  if (!g_thread) return;
  InterlockedExchange((LONG*)&g_stop, 1);
  io_lock();
  for (n = g_head; n; n = n->next) InterlockedExchange((LONG*)&n->job->cancel, 1);
  io_unlock();
  SetEvent(g_wake);
  if (WaitForSingleObject(g_thread, 15000) != WAIT_OBJECT_0) {
    // The worker is stuck in a file call (dead network share). Leave it; the process is ending.
    return;
  }
  CloseHandle(g_thread); g_thread = 0;
  CloseHandle(g_wake); g_wake = 0;
  // Nothing may be left half done: finished saves that were never handed over delete their temp file.
  while ((j = io_take_done(0, 0, 0)) != 0) {
    if (j->kind == IOJ_SAVE_TEMP) docio_io_abandon(j);
    io_job_free(j);
  }
  io_lock();
  while (g_head) {                        // cancelled before they ran (the worker marks them DONE; defensive)
    n = g_head; g_head = n->next;
    if (n->job->kind == IOJ_SAVE_TEMP) docio_io_abandon(n->job);
    io_job_free(n->job);
    delete n;
  }
  io_unlock();
}

u32 iothread_submit(IoJob* job)
{
  IoNode* n;
  IoNode** link;
  if (!job) return 0;
  if (!g_thread || g_stop) { io_job_free(job); return 0; }
  n = new IoNode;
  if (!n) { io_job_free(job); return 0; }
  job->cancel = 0;
  job->result = E_STATE;
  job->done_bytes = job->total_bytes = 0;
  n->job = job; n->next = 0; n->state = NODE_QUEUED;
  io_lock();
  job->id = g_next_id++;
  if (g_next_id == 0) g_next_id = 1;
  for (link = &g_head; *link; link = &(*link)->next) {}
  *link = n;
  io_unlock();
  SetEvent(g_wake);
  return job->id;
}

void iothread_cancel_tab(u32 tabid)
{
  IoNode* n;
  if (!g_cs_ready) return;
  io_lock();
  for (n = g_head; n; n = n->next)
    if (n->job->tabid == tabid && n->state != NODE_DONE) InterlockedExchange((LONG*)&n->job->cancel, 1);
  io_unlock();
}

int iothread_process_done(u32 job_id)
{
  int count = 0;
  IoJob* j;
  if (!g_cs_ready) return 0;
  while ((j = io_take_done(job_id, 0, 0)) != 0) {
    io_dispatch(j);
    count++;
    if (job_id) break;
  }
  return count;
}

int iothread_job_pending(u32 job_id)
{
  IoNode* n;
  int found = 0;
  if (!g_cs_ready || !job_id) return 0;
  io_lock();
  for (n = g_head; n; n = n->next) if (n->job->id == job_id) { found = 1; break; }
  io_unlock();
  return found;
}

static int io_has_jobs(u32 tabid, int by_tab)
{
  IoNode* n;
  int found = 0;
  io_lock();
  for (n = g_head; n; n = n->next) if (!by_tab || n->job->tabid == tabid) { found = 1; break; }
  io_unlock();
  return found;
}

static int io_wait(u32 tabid, int by_tab, u32 timeout_ms)
{
  u32 start = GetTickCount();
  if (!g_cs_ready) return 1;
  for (;;) {
    IoJob* j;
    while ((j = io_take_done(0, tabid, by_tab)) != 0) io_dispatch(j);
    if (!io_has_jobs(tabid, by_tab)) return 1;
    if (GetTickCount() - start >= timeout_ms) return 0;
    Sleep(10);
  }
}

int iothread_wait_tab(u32 tabid, u32 timeout_ms)
{
  return io_wait(tabid, 1, timeout_ms);
}

int iothread_flush(u32 timeout_ms)
{
  return io_wait(0, 0, timeout_ms);
}

void iothread_on_done(WPARAM w, LPARAM l)
{
  if (w == 0) {                            // progress of a running job
    IoNode* n;
    u32 id = (u32)l, tabid = 0, done = 0, total = 0;
    int found = 0;
    if (!g_cs_ready) return;
    io_lock();
    for (n = g_head; n; n = n->next) {
      if (n->job->id == id && n->state == NODE_RUNNING) {
        found = n->job->kind == IOJ_SAVE_TEMP;
        tabid = n->job->tabid; done = n->job->done_bytes; total = n->job->total_bytes;
        break;
      }
    }
    io_unlock();
    if (found) docio_io_progress(tabid, done, total);
    return;
  }
  iothread_process_done((u32)w);
}
