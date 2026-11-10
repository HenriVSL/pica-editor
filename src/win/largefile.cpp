// largefile.cpp - on-demand documents and the indexer thread (DESIGN 4.3, 13.8, 14.1). Owner after Wave 0: T14.
//
// An on-demand document starts as Document::init_source_indexing (pieces over the file, no line counts). This module
// runs file_index on its own thread (own read handle, 64 KB reads, below-normal priority) and posts WM_APP_INDEX:
// wParam = job id, lParam = percent (0..100) or LF_DONE. Meanwhile the hex view works read-only, the text view shows
// the first screen, edits are refused (Document::editable), and the tab shows "Indexing X... 43% [Cancel]" in its
// notification bar, "(indexing 43%)" in its label and "Indexing 43%" in the status bar. On completion the UI thread
// calls Document::finish_indexing (EV_INDEXED: full text view, editing enabled). Cancel closes the tab.
//
// Thread rules (4.3): the job is allocated and freed by the UI thread; the worker writes only job->res/rc/pct and
// posts messages; the UI thread frees a job only after its thread has exited (WaitForSingleObject).
#include <windows.h>
#include <process.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "largefile.h"
#include "watch_int.h"
#include "docio.h"
#include "docio_int.h"
#include "views.h"
#include "notifybar.h"
#include "selftest.h"
#include "fileops.h"
#include "config.h"
#include "estr.h"
#include "resource.h"

enum { LF_DONE = 1000, LF_BTN_CANCEL = 1 };

struct LfJob {
  u32 id, tabid;
  char path[260];
  BreakSpec bs;
  volatile i32 cancel;
  volatile i32 pct;
  HANDLE th;
  IndexResult res;          // written by the worker before it posts LF_DONE
  int rc;
  epos expect;              // document length when indexing started (edits are refused meanwhile)
  u32 start_ms, goto_line, last_pct;
  DocPlace place; u8 has_place, has_saved_bar, pad[2];
  NotifyState saved_bar;    // the bar the tab had before (read-only, binary...), shown again when indexing ends
};
static Vec<LfJob*> g_jobs;
static u32 g_next_job = 1;

static LfJob* job_by_id(u32 id)
{
  u32 i;
  for (i = 0; i < g_jobs.size(); i++) if (g_jobs[i]->id == id) return g_jobs[i];
  return 0;
}

static LfJob* job_by_tab(u32 tabid)
{
  u32 i;
  for (i = 0; i < g_jobs.size(); i++) if (g_jobs[i]->tabid == tabid) return g_jobs[i];
  return 0;
}

static void job_unlink(LfJob* j)
{
  u32 i;
  for (i = 0; i < g_jobs.size(); i++) if (g_jobs[i] == j) { g_jobs.erase(i, 1); return; }
}

int largefile_running(Tab* t)
{
  return t && job_by_tab(t->id) != 0;
}

// ---- worker ----

class LfProgress : public ProgressCb {
public:
  LfProgress(LfJob* j) : j_(j), last_(0xFFFFFFFFu) {}
  int progress(u32 done, u32 total)
  {
    u32 pct = total ? (u32)(((u64)done * 100u) / total) : 0;
    if (pct > 100) pct = 100;
    if (pct != last_) {
      last_ = pct;
      InterlockedExchange((LONG*)&j_->pct, (LONG)pct);
      if (g_app.frame) PostMessageA(g_app.frame, WM_APP_INDEX, (WPARAM)j_->id, (LPARAM)pct);
    }
    return j_->cancel ? 1 : 0;
  }
private:
  LfJob* j_; u32 last_;
};

static unsigned __stdcall lf_thread(void* p)
{
  LfJob* j = (LfJob*)p;
  LfProgress prog(j);
  j->rc = file_index(fs_os(), j->path, j->bs, &j->res, &prog, &j->cancel);
  if (g_app.frame) PostMessageA(g_app.frame, WM_APP_INDEX, (WPARAM)j->id, (LPARAM)LF_DONE);
  return 0;
}

// ---- the indexing bar ----

static void bar_text(Tab* t, u32 pct, char* out, u32 cap)
{
  char name[280];
  tab_title(t, name, sizeof(name));
  es_fmt(out, cap, "Indexing %s... %u%%  (the text view shows the start of the file; editing starts when this is done)",
         name, pct);
}

static void lf_bar_fn(Tab* t, int kind, int button_id, void* ctx)
{
  char name[280];
  (void)kind; (void)ctx;
  if (!t || button_id != LF_BTN_CANCEL) return;          // [x] only hides the bar; indexing goes on
  tab_title(t, name, sizeof(name));
  largefile_cancel(t);
  docio_close(t, 0);
  app_status_note("Closed %s (indexing cancelled)", name);
}

static void show_bar(Tab* t, u32 pct)
{
  char text[300];
  NotifyButton b[1];
  memset(b, 0, sizeof(b));
  b[0].id = LF_BTN_CANCEL;
  es_copy(b[0].text, sizeof(b[0].text), "Cancel");
  bar_text(t, pct, text, sizeof(text));
  notifybar_show(t, NB_INDEXING, text, b, 1, lf_bar_fn, 0);
}

static void update_bar(Tab* t, u32 pct)
{
  if (t->notify.kind != NB_INDEXING) return;            // closed with [x] or replaced
  bar_text(t, pct, t->notify.text, sizeof(t->notify.text));
  if (t == app_active() && g_app.notifybar) InvalidateRect(g_app.notifybar, 0, FALSE);
}

static void end_bar(Tab* t, LfJob* j)
{
  if (t->notify.kind == NB_INDEXING) notifybar_hide(t, NB_INDEXING);
  if (j->has_saved_bar && t->notify.kind == NB_NONE)
    notifybar_show(t, j->saved_bar.kind, j->saved_bar.text, j->saved_bar.buttons, j->saved_bar.nbuttons,
                   j->saved_bar.fn, j->saved_bar.ctx);
}

// ---- API ----

int largefile_begin(Tab* t)
{
  return largefile_begin_ex(t, 0, 0);
}

int largefile_begin_ex(Tab* t, u32 goto_line, const DocPlace* place)
{
  LfJob* j;
  unsigned tid = 0;
  const char* path;
  if (!t || !t->doc || !t->doc->indexing()) return E_STATE;
  if (job_by_tab(t->id)) return E_BUSY;
  path = t->fsrc && t->fsrc->path() ? t->fsrc->path() : t->path;
  if (!path || !path[0]) return E_STATE;
  j = new LfJob;
  if (!j) return E_NOMEM;
  j->id = g_next_job++;
  j->tabid = t->id;
  es_copy(j->path, sizeof(j->path), path);
  codec_break_spec(t->doc->enc(), t->doc->eol_style() == EOL_CR ? 1 : 0, &j->bs);
  j->cancel = 0;
  j->pct = 0;
  j->th = 0;
  j->rc = E_STATE;
  j->expect = t->doc->length();
  j->start_ms = app_now_ms();
  j->goto_line = goto_line;
  j->last_pct = 0;
  j->has_place = 0;
  if (place) { j->place = *place; j->has_place = 1; }
  else memset(&j->place, 0, sizeof(j->place));
  j->has_saved_bar = 0;
  memset(&j->saved_bar, 0, sizeof(j->saved_bar));
  if (t->notify.kind != NB_NONE && t->notify.kind != NB_INDEXING) { j->saved_bar = t->notify; j->has_saved_bar = 1; }
  if (g_jobs.push(j) != E_OK) { delete j; return E_NOMEM; }
  j->th = (HANDLE)_beginthreadex(0, 0, lf_thread, j, CREATE_SUSPENDED, &tid);
  if (!j->th) { job_unlink(j); delete j; return E_NOMEM; }
  SetThreadPriority(j->th, THREAD_PRIORITY_BELOW_NORMAL);   // the UI stays responsive on the single CPU
  t->indexing = 1;
  t->index_pct = 0;
  show_bar(t, 0);
  app_update_tab(t);
  selftest_log("indexing %s (%u bytes) started", j->path, j->expect);
  ResumeThread(j->th);
  return E_OK;
}

void largefile_cancel(Tab* t)
{
  LfJob* j;
  if (!t) return;
  j = job_by_tab(t->id);
  if (!j) { t->indexing = 0; return; }
  InterlockedExchange((LONG*)&j->cancel, 1);
  job_unlink(j);
  if (j->th) {
    // file_index checks the flag between 64 KB reads; a hung network read is the only way to wait long
    if (WaitForSingleObject(j->th, 30000) == WAIT_OBJECT_0) { CloseHandle(j->th); delete j; }
    else selftest_log("indexer thread did not stop in 30 s (job leaked)");
  } else {
    delete j;
  }
  t->indexing = 0;
  t->index_pct = 0;
  if (t->notify.kind == NB_INDEXING) notifybar_hide(t, NB_INDEXING);
  app_update_tab(t);
}

static void place_caret(Tab* t, LfJob* j)
{
  Document* d = t->doc;
  if (j->has_place) {
    docio_place_set(t, j->place);
  } else if (j->goto_line > 1) {
    epos p = d->line_start(j->goto_line - 1);
    if (p < d->text_origin()) p = d->text_origin();
    t->sel.anchor = t->sel.caret = p;
  } else {
    if (t->sel.caret > d->length() || t->sel.anchor > d->length() || t->sel.caret < d->text_origin())
      t->sel.anchor = t->sel.caret = d->text_origin();
  }
}

void largefile_on_msg(WPARAM w, LPARAM l)
{
  LfJob* j = job_by_id((u32)w);
  Tab* t;
  char name[280];
  int rc;
  if (!j) return;                                        // cancelled meanwhile
  t = docio_tab_by_id(j->tabid);
  if (l != LF_DONE) {
    u32 pct = (u32)l;
    if (!t || pct == j->last_pct) return;
    j->last_pct = pct;
    t->index_pct = pct;
    update_bar(t, pct);
    app_update_tab(t);
    return;
  }
  WaitForSingleObject(j->th, INFINITE);                  // the worker has posted its last message
  CloseHandle(j->th);
  j->th = 0;
  job_unlink(j);
  if (!t) { delete j; return; }
  t->indexing = 0;
  t->index_pct = 0;
  rc = j->rc;
  if (rc == E_OK && j->res.size != t->doc->length()) rc = E_STATE;    // the file changed while we indexed it
  if (rc == E_OK) rc = t->doc->finish_indexing(j->res.block_breaks.data(), j->res.block_breaks.size(), j->res.eol);
  tab_title(t, name, sizeof(name));
  if (rc == E_OK) {
    u32 ms = app_now_ms() - j->start_ms;
    end_bar(t, j);
    place_caret(t, j);
    docio_view_refresh(t);
    app_on_doc_changed(t);
    app_status_note("Indexed %s: %u lines in %u.%u s", name, t->doc->line_count(), ms / 1000, (ms % 1000) / 100);
    selftest_log("indexing %s done: %u lines, %u ms", j->path, t->doc->line_count(), ms);
    delete j;
    return;
  }
  if (t->notify.kind == NB_INDEXING) notifybar_hide(t, NB_INDEXING);
  app_update_tab(t);
  delete j;
  if (rc == E_CANCEL) return;
  if (rc == E_STATE)
    ui_error("%s changed on disk while Pica was indexing it, so the tab was closed. Open the file again.", name);
  else
    ui_error("%s could not be read to the end (%s), so the tab was closed.", name, e_errname(rc));
  docio_close(t, 0);
}

// ---- one-time note: unsaved changes to on-demand files are not backed up (14.1) ----

class LfNote : public DocListener {
public:
  LfNote(Tab* t) : t_(t) {}
  void doc_changed(Document* d, const EditEvent& e)
  {
    (void)e;
    if (t_->backup_note_shown || !d->modified()) return;
    t_->backup_note_shown = 1;
    if (g_app.cfg.backup_enabled)
      app_status_note("Unsaved changes to files over %u MB are not backed up: save often",
                      (u32)(docio_ondemand_threshold() / (1024u * 1024u)));
  }
  Tab* t_;
};
struct LfNoteRec { u32 tabid; Document* d; LfNote* l; };
static Vec<LfNoteRec> g_notes;

void largefile_tab_added(Tab* t)
{
  u32 i;
  LfNoteRec r;
  if (!t || !t->doc || !t->ondemand || t->backup_note_shown) return;
  for (i = 0; i < g_notes.size(); i++) if (g_notes[i].tabid == t->id) return;
  r.tabid = t->id;
  r.d = t->doc;
  r.l = new LfNote(t);
  if (!r.l) return;
  if (g_notes.push(r) != E_OK) { delete r.l; return; }
  t->doc->add_listener(r.l);
}

void largefile_tab_removed(Tab* t)
{
  u32 i;
  if (!t) return;
  for (i = 0; i < g_notes.size(); i++) {
    if (g_notes[i].tabid != t->id) continue;
    if (t->doc == g_notes[i].d) t->doc->remove_listener(g_notes[i].l);
    delete g_notes[i].l;
    g_notes.erase(i, 1);
    return;
  }
}

// =================================================================================================================
// selftest (st_largefile): a 20 MB fixture opens on demand and indexes on the thread (hex view usable, edits refused
// meanwhile), save + reopen byte-exact, cancel closes the tab, a file growing past the threshold reloads into
// on-demand mode and back, timings. The 100 MB timing run (DESIGN 3.2) runs when <cfg_dir>\st_big exists (it writes
// 100 MB to the fixture folder: ~16 s on C:).

enum { ST_MB = 1024u * 1024u };

static int st_write_synth(StCtx* c, const char* name, epos size, u32 seed, int kind, char* path, u32 cap, u32* lines)
{
  SynthSource src(size, seed, kind);
  FileSys* fs = fs_os();
  FsFile f;
  epos off = 0;
  u32 nl = 0;
  int rc;
  ui_path_join(c->fixdir, name, path, cap);
  rc = fs->open(path, FS_WRITE, &f);
  if (rc != E_OK) { st_checkf(c, 0, "create %s: %s", path, e_errname(rc)); return rc; }
  while (off < size && rc == E_OK) {
    epos avail = 0;
    const u8* p = src.get(off, &avail);
    u32 i;
    if (!p || !avail) { rc = E_IO; break; }
    if (avail > size - off) avail = size - off;
    for (i = 0; i < avail; i++) if (p[i] == '\n') nl++;
    rc = fs->write(f, p, avail);
    off += avail;
  }
  if (rc == E_OK) rc = fs->flush(f);
  fs->close(f);
  if (rc != E_OK) st_checkf(c, 0, "write %s: %s", path, e_errname(rc));
  if (lines) *lines = nl;
  return rc;
}

static void st_rm(const char* path)
{
  FsStat st;
  if (fs_os()->stat(path, &st) == E_OK && st.exists) fs_os()->remove(path);
}

static u32 st_count_temps(const char* dir)
{
  FsFind f;
  u32 n = 0;
  if (fs_os()->find_first(dir, &f) == E_OK) {
    do {
      if ((es_nicmp(f.name, "~PC", 3) == 0 || es_nicmp(f.name, "~PO", 3) == 0) && !(f.attrs & FSA_DIR)) n++;
    } while (fs_os()->find_next(&f) == E_OK);
    fs_os()->find_close(&f);
  }
  return n;
}

// Pumps until t has finished indexing (or the tab is gone); returns the elapsed ms, 0xFFFFFFFF on timeout.
static u32 st_wait_indexed(StCtx* c, Tab* t, u32 timeout_ms)
{
  u32 start = GetTickCount();
  u32 id = t->id;
  for (;;) {
    Tab* x = docio_tab_by_id(id);
    if (!x || (!x->indexing && !x->doc->indexing())) return GetTickCount() - start;
    if (GetTickCount() - start >= timeout_ms) return 0xFFFFFFFFu;
    st_pump(c, 20);
  }
}

static int st_doc_equals_file(StCtx* c, Document* d, const char* path)
{
  FileSys* fs = fs_os();
  FsFile f;
  u8* a;
  u8* b;
  u32 lo = 0, hi = 0, off = 0;
  int same = 1;
  (void)c;
  if (fs->open(path, FS_READ | FS_SHARE_ALL, &f) != E_OK) return 0;
  if (fs->size(f, &lo, &hi) != E_OK || hi || lo != d->length()) { fs->close(f); return 0; }
  a = (u8*)e_valloc(65536);
  b = (u8*)e_valloc(65536);
  if (!a || !b) same = 0;
  while (same && off < lo) {
    u32 n = lo - off, got = 0;
    if (n > 65536) n = 65536;
    if (fs->read_at(f, off, 0, a, n, &got) != E_OK || got != n || d->read(off, b, n) != E_OK || memcmp(a, b, n) != 0)
      same = 0;
    off += n;
  }
  if (a) e_vfree(a, 65536);
  if (b) e_vfree(b, 65536);
  fs->close(f);
  return same;
}

static void st_ondemand_20mb(StCtx* c)
{
  char path[300];
  Tab* t;
  u32 lines = 0, ms, t0, i;
  epos size = 20u * ST_MB + 123u;
  int rc, saw_indexing = 0;
  u8 head[16];
  if (st_write_synth(c, "lf20.txt", size, 7, SYN_TEXT_CRLF, path, sizeof(path), &lines) != E_OK) return;
  t0 = GetTickCount();
  rc = docio_open(path, 0, &t);
  ms = GetTickCount() - t0;
  st_checkf(c, rc == E_OK && t, "20 MB open (%s)", e_errname(rc));
  if (!t) { st_rm(path); return; }
  selftest_log("20 MB: docio_open returned after %u ms", ms);
  st_checkf(c, t->ondemand && t->fsrc && t->large, "20 MB opens on demand (large mode)");
  saw_indexing = t->indexing && t->doc->indexing();
  st_checkf(c, saw_indexing, "indexing runs in the background after open");
  if (saw_indexing) {
    st_checkf(c, t->notify.kind == NB_INDEXING && t->notify.nbuttons == 1, "indexing bar with Cancel");
    // edits refused, save refused, hex view usable
    st_checkf(c, t->doc->replace(0, 0, (const u8*)"x", 1) == E_BUSY, "edits refused while indexing");
    st_checkf(c, docio_save(t, 0, 0, 0, 0, 0) == E_BUSY, "save refused while indexing");
    t0 = GetTickCount();
    t->view = VIEW_HEX;
    app_activate(app_tab_index(t));
    if (g_app.hexview) { InvalidateRect(g_app.hexview, 0, FALSE); UpdateWindow(g_app.hexview); }
    ms = GetTickCount() - t0;
    selftest_log("20 MB: hex view shown during indexing in %u ms", ms);
    st_checkf(c, t->doc->read(size - 16, head, 16) == E_OK, "bytes at the end readable during indexing");
    t->sel.anchor = t->sel.caret = size / 2;
    if (g_app.hexview) hexview_set_selection(g_app.hexview, size / 2, size / 2, SCROLL_CENTER);
    st_pump(c, 30);
    st_checkf(c, t->hv.top_row > 0 && t->indexing, "hex view scrolls to the middle while indexing");
    t->view = VIEW_TEXT;
    app_activate(app_tab_index(t));
    st_pump(c, 30);
  }
  ms = st_wait_indexed(c, t, 120000);
  st_checkf(c, ms != 0xFFFFFFFFu, "20 MB indexing finished");
  selftest_log("20 MB: indexing finished %u ms after the checks above", ms);
  st_checkf(c, !t->indexing && !t->doc->indexing() && t->notify.kind != NB_INDEXING, "indexing state cleared");
  st_checkf(c, t->doc->line_count() == lines + 1, "line count %u (want %u)", t->doc->line_count(), lines + 1);
  st_checkf(c, t->doc->editable(), "editable after indexing");
  // edit + save + reopen byte-exact
  rc = t->doc->replace(1000, 0, (const u8*)"LARGE1", 6);
  st_checkf(c, rc == E_OK && t->doc->modified(), "edit after indexing (%s)", e_errname(rc));
  rc = t->doc->replace(t->doc->length(), 0, (const u8*)"END\r\n", 5);
  t0 = GetTickCount();
  rc = docio_save_wait(t, 0, 0, 0);
  ms = GetTickCount() - t0;
  selftest_log("20 MB: save took %u ms", ms);
  st_checkf(c, rc == E_OK && !t->doc->modified(), "20 MB on-demand save (%s)", e_errname(rc));
  st_checkf(c, st_doc_equals_file(c, t->doc, path), "saved file == document");
  docio_close(t, 0);
  st_checkf(c, st_count_temps(c->fixdir) == 0, "old copy deleted on close");
  rc = docio_open(path, 0, &t);
  if (rc == E_OK && t) {
    u8 buf[6];
    st_wait_indexed(c, t, 120000);
    st_checkf(c, t->doc->read(1000, buf, 6) == E_OK && memcmp(buf, "LARGE1", 6) == 0 &&
              t->doc->length() == size + 11 && t->doc->line_count() == lines + 2, "reopen: edits on disk, lines");
    // cancel: reopen and cancel through the bar; the tab closes
    docio_close(t, 0);
  }
  rc = docio_open(path, 0, &t);
  if (rc == E_OK && t) {
    u32 id = t->id;
    st_checkf(c, t->indexing && t->notify.kind == NB_INDEXING, "reopen indexes again");
    if (t->notify.kind == NB_INDEXING && t->notify.fn) {
      NotifyFn fn = t->notify.fn;
      void* ctx = t->notify.ctx;
      notifybar_hide(t, NB_NONE);
      fn(t, NB_INDEXING, LF_BTN_CANCEL, ctx);
    }
    st_pump(c, 50);
    st_checkf(c, docio_tab_by_id(id) == 0, "Cancel on the indexing bar closes the tab");
    st_checkf(c, g_jobs.size() == 0, "no indexer job left");
  }
  // closing a tab while it indexes stops the thread
  rc = docio_open(path, 0, &t);
  if (rc == E_OK && t) {
    st_checkf(c, docio_close(t, 1) == 1 && g_jobs.size() == 0, "close while indexing stops the indexer");
  }
  for (i = 0; i < 20 && fs_os()->remove(path) != E_OK; i++) st_pump(c, 50);
  st_checkf(c, !ui_file_exists(path), "20 MB fixture removed");
}

// A file that crosses the on-demand threshold between open and reload changes mode (T09 open issue).
static void st_mode_switch(StCtx* c)
{
  char path[300];
  Tab* t;
  u32 lines = 0, lines2 = 0, ms;
  i32 saved = g_app.cfg.ondemand_mb;
  int rc;
  if (st_write_synth(c, "lfsw.txt", 3u * ST_MB, 3, SYN_TEXT_LF, path, sizeof(path), &lines) != E_OK) return;
  g_app.cfg.ondemand_mb = 4;
  rc = docio_open(path, 0, &t);
  st_checkf(c, rc == E_OK && t && !t->ondemand, "3 MB opens in memory (threshold 4 MB)");
  if (!t) { g_app.cfg.ondemand_mb = saved; st_rm(path); return; }
  // grows to 6 MB: reload goes on demand and indexes on the thread
  st_write_synth(c, "lfsw.txt", 6u * ST_MB, 4, SYN_TEXT_LF, path, sizeof(path), &lines2);
  rc = docio_reload_ex(t, 0, 1, RL_QUIET);
  st_checkf(c, rc == E_OK && t->ondemand && t->fsrc, "reload of a 6 MB file switches to on-demand (%s)", e_errname(rc));
  ms = st_wait_indexed(c, t, 60000);
  st_checkf(c, ms != 0xFFFFFFFFu && !t->doc->indexing() && t->doc->line_count() == lines2 + 1,
            "reloaded on-demand document indexed (%u lines)", t->doc->line_count());
  st_checkf(c, st_doc_equals_file(c, t->doc, path), "on-demand reload bytes == file");
  st_checkf(c, doccmd_convert(t, enc_utf8(1), 0) == E_TOOBIG, "convert refused after switching to on-demand");
  // shrinks to 1 MB (as an edit from the Mac side of M: would: our deny-write handle doesn't stop it there; here the
  // handle is closed first): back to memory, convert allowed again
  if (t->fsrc) t->fsrc->close();
  st_write_synth(c, "lfsw.txt", 1u * ST_MB, 5, SYN_TEXT_LF, path, sizeof(path), &lines);
  rc = docio_reload_ex(t, 0, 1, RL_QUIET);
  st_checkf(c, rc == E_OK && !t->ondemand && !t->fsrc && !t->doc->indexing() && t->doc->line_count() == lines + 1,
            "reload of a 1 MB file switches back to memory (%s)", e_errname(rc));
  st_checkf(c, st_doc_equals_file(c, t->doc, path), "in-memory reload bytes == file");
  rc = doccmd_convert(t, enc_utf8(1), 0);
  st_checkf(c, rc == E_OK, "convert allowed again (%s)", e_errname(rc));
  if (rc == E_OK) { Selection s; t->doc->undo(&s); }
  g_app.cfg.ondemand_mb = saved;
  docio_close(t, 0);
  st_rm(path);
}

static void st_big_timing(StCtx* c)
{
  char marker[300], path[300];
  Tab* t;
  u32 lines = 0, t0, open_ms, hex_ms, index_ms, mb = 100, i;
  u64 free_b = 0;
  epos size;
  int rc, existing = 0;
  EBytes mk;
  ui_path_join(c->cfg_dir, "st_big", marker, sizeof(marker));
  if (!ui_file_exists(marker)) { selftest_log("100 MB timing skipped (create %s to run it)", marker); return; }
  if (st_file_bytes(c, marker, &mk) == E_OK && mk.size() > 0) {
    if (mk[0] >= '0' && mk[0] <= '9') {
      u32 v = 0;
      for (i = 0; i < mk.size() && mk[i] >= '0' && mk[i] <= '9'; i++) v = v * 10 + (mk[i] - '0');
      if (v >= 20 && v <= 1000) mb = v;
    } else {                                             // a path: time an existing file (e.g. one on M:)
      for (i = 0; i < mk.size() && i < sizeof(path) - 1 && mk[i] != '\r' && mk[i] != '\n' && mk[i] != ' '; i++)
        path[i] = (char)mk[i];
      path[i] = 0;
      existing = 1;
    }
  }
  if (existing) {
    FsStat st;
    if (fs_os()->stat(path, &st) != E_OK || !st.exists || st.size_hi) { st_checkf(c, 0, "timing file %s", path); return; }
    size = st.size_lo;
    mb = size / ST_MB;
  } else {
    size = (epos)mb * ST_MB;
    if (fs_os()->free_space(c->fixdir, &free_b) == E_OK && free_b < (u64)size + 50u * ST_MB) {
      selftest_log("%u MB timing skipped: not enough free space in %s", mb, c->fixdir);
      return;
    }
    t0 = GetTickCount();
    if (st_write_synth(c, "lfbig.txt", size, 11, SYN_TEXT_CRLF, path, sizeof(path), &lines) != E_OK) return;
    selftest_log("%u MB fixture written in %u ms", mb, GetTickCount() - t0);
  }
  t0 = GetTickCount();
  rc = docio_open(path, 0, &t);
  if (rc == E_OK && t && g_app.textview) { InvalidateRect(g_app.textview, 0, FALSE); UpdateWindow(g_app.textview); }
  open_ms = GetTickCount() - t0;
  st_checkf(c, rc == E_OK && t && t->ondemand, "%u MB opens on demand (%s)", mb, e_errname(rc));
  if (!t) { if (!existing) st_rm(path); return; }
  t0 = GetTickCount();
  t->view = VIEW_HEX;
  app_activate(app_tab_index(t));
  if (g_app.hexview) {
    hexview_set_selection(g_app.hexview, size - 1, size - 1, SCROLL_CENTER);
    InvalidateRect(g_app.hexview, 0, FALSE);
    UpdateWindow(g_app.hexview);
  }
  hex_ms = GetTickCount() - t0;
  st_checkf(c, t->indexing || open_ms + hex_ms < 1000, "hex view shown while indexing");
  index_ms = st_wait_indexed(c, t, 900000);
  st_checkf(c, index_ms != 0xFFFFFFFFu && (existing || t->doc->line_count() == lines + 1), "%u MB indexed, %u lines",
            mb, t->doc->line_count());
  selftest_log("TIMING %u MB %s: first screen %u ms, hex view at the end %u ms, indexing done %u ms later "
               "(budget on C:: first screen + hex <= 1000 ms, index <= 3000 ms)", mb, path, open_ms, hex_ms, index_ms);
  st_checkf(c, open_ms + hex_ms <= 1000, "first screen + hex view <= 1 s (%u ms)", open_ms + hex_ms);
  t->view = VIEW_TEXT;
  docio_close(t, 0);
  if (!existing) st_rm(path);
}

void st_largefile(StCtx* c)
{
  int i;
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  if (fs_os()->is_remote(c->fixdir)) selftest_log("network fixture folder: 20 MB and mode-switch parts skipped");
  else { st_ondemand_20mb(c); st_mode_switch(c); }
  st_big_timing(c);
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  st_pump(c, 20);
}
