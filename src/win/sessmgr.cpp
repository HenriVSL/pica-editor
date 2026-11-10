// sessmgr.cpp - session, backups, crash recovery, exit flow (DESIGN 13.4-13.7, 15.9, 15.12). Owner after Wave 0: T13.
//
// Model
// - Backups (13.5): one generation file per modified in-memory tab in <dir>\backup, written by the I/O worker from a
//   copy (<= 2 MB) or a SnapshotSource of the document. Due backup_seconds after the last edit, at most 30 s after the
//   first un-backed-up edit, at once on deactivation and on exit. Deleted after a save, an undo back to the saved
//   state, a close, and a Discard at exit.
// - Session (13.6): written through the worker into session1.ini / session2.ini (ab_write) 2 s after tab changes,
//   every 60 s when view state changed, after a save, and synchronously at startup (clean_exit=0) and exit (=1).
//   Only one session job is in flight; a request meanwhile is written when it finishes.
// - One sequence counter g_seq numbers session writes AND backups (BackupMeta.seq), so at startup a backup older
//   than the session is known to be superseded: a session tab with modified=0 and has_backup=0 (saved or
//   discarded after the backup), or a tab id missing from the session (closed after the backup). Such stale backups
//   are deleted; every other backup is restored (13.7), also when the session is missing or damaged.
// - Startup: also offers recovery of a safe save interrupted between its two renames (target missing, old file as
//   ~PO*.TMP and new one as ~PC*.TMP next to it), never silently: a tab with the new bytes and a bar.
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "sessmgr.h"
#include "sessmgr_int.h"
#include "selftest.h"
#include "iothread.h"
#include "docio.h"
#include "docio_int.h"
#include "dialogs.h"
#include "progress.h"
#include "views.h"
#include "notifybar.h"
#include "watch.h"
#include "fsys.h"
#include "fileops.h"
#include "session.h"
#include "ini.h"
#include "estr.h"
#include "lexer.h"

enum {
  SM_COPY_MAX = 2u * 1024u * 1024u,       // documents up to this size are copied for the worker (else snapshot)
  SM_MAX_DIRTY_MS = 30000,                // a backup at least every 30 s while edits continue
  SM_STATE_MS = 60000,                    // view state (caret, scroll) is written at most once a minute
  SM_SESSION_DEBOUNCE_MS = 2000,
  SM_FLUSH_MS = 10000,                    // exit: wait this long for the I/O queue before asking
  SM_PAIR_MAX_BYTES = 4u * 1024u * 1024u  // interrupted-save recovery loads the new copy into memory
};
enum {                                    // private question ids (ui.h: modules may use Q_USER + n)
  Q_SM_FLUSH = Q_USER + 30,               // exit: backups still being written      YES (wait) / NO (exit now)
  Q_SM_UNSAVED = Q_USER + 31              // exit: some changes could not be backed up   YES (exit) / NO (stay)
};
enum { SMB_SAVE = 1, SMB_REVERT, SMB_RELOAD, SMB_KEEP, SMB_RESTORE_PC };   // notification bar buttons

struct SmTab {                            // per-tab bookkeeping (POD: lives in a Vec)
  u32 tabid;
  u32 seen_change;                        // Document::change_seq() last seen
  u32 last_edit_ms, first_dirty_ms;       // last change; first change not covered by a backup
  u32 job, job_change, job_order;         // backup job in flight (0 = none) and what it covers
  u32 backed_change;                      // change_seq of the newest valid backup (when has_backup)
  u32 gen;                                // newest generation written or submitted (0 = none yet)
  u32 retry_ms, fail_count;               // after a failure: no new attempt before retry_ms
  u32 delete_order;                       // submit order of the newest delete job (older backups are gone)
  u32 pair_change;                        // interrupted-save recovery: change_seq when the tab was created
  u8 dirty, has_backup, discarded, restored;
  u8 note_ondemand, note_fail, pad[2];
  char pc[260], po[260];                  // interrupted-save recovery: new copy (shown) and old copy
};

static Vec<SmTab> g_sm;
static int g_on = 0;                      // backups/session active (sessmgr_startup or a selftest)
static int g_manual = 0;                  // selftest: timers and idle do nothing
static char g_dir[260], g_bdir[260], g_s1[260], g_s2[260];
static u32 g_seq = 1;                     // shared by session writes and backups
static u32 g_order = 1;                   // submit order of sessmgr jobs
static int g_which = 0;                   // A/B slot of the newest session copy (0 = none)
static u32 g_sess_job = 0;
static int g_sess_dirty = 0;
static u32 g_sess_hash = 0, g_state_check_ms = 0;
static int g_exit_done = 0;               // sessmgr_exit said yes (WM_QUERYENDSESSION may still be cancelled)
static u32 g_last_prompt_n = 0;           // selftest: tabs passed to the last exit prompt
static int g_note_sess_fail = 0;
static int g_in_exit = 0;                 // sessmgr_exit running: timers and idle submit nothing (modal pumps)

static FileSys* sm_fs() { return fs_os(); }

// ---------------------------------------------------------------------------------------------------------------
// helpers

static int sm_find(u32 tabid)
{
  u32 i;
  for (i = 0; i < g_sm.size(); i++) if (g_sm[i].tabid == tabid) return (int)i;
  return -1;
}

static SmTab* sm_get(Tab* t)             // pointer valid until the next sm_get of a new tab
{
  int i;
  SmTab z;
  if (!t) return 0;
  i = sm_find(t->id);
  if (i >= 0) return &g_sm[(u32)i];
  memset(&z, 0, sizeof(z));
  z.tabid = t->id;
  z.seen_change = t->doc ? t->doc->change_seq() : 0;
  if (g_sm.push(z) != E_OK) return 0;
  return &g_sm.back();
}

static Tab* sm_tab_by_id(u32 id)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) if (app_tab(i)->id == id) return app_tab(i);
  return 0;
}

static void sm_set_dir(const char* dir)
{
  es_copy(g_dir, sizeof(g_dir), dir);
  ui_path_join(g_dir, "backup", g_bdir, sizeof(g_bdir));
  ui_path_join(g_dir, "session1.ini", g_s1, sizeof(g_s1));
  ui_path_join(g_dir, "session2.ini", g_s2, sizeof(g_s2));
  g_which = 0;                            // unknown for this folder until a read or write (ab_write picks file 1)
}

static void sm_name(Tab* t, char* out, u32 cap)
{
  if (t->untitled || !t->path[0]) tab_title(t, out, cap);
  else es_copy(out, cap, ui_path_name(t->path));
}

static int sm_backups_on()
{
  return g_on && g_app.cfg.backup_enabled;
}

static int sm_empty_untitled(Tab* t)
{
  return t->untitled && t->doc && t->doc->length() <= t->doc->text_origin();
}

// Tracks content changes even when sessmgr_doc_changed was not called.
static void sm_track(Tab* t, SmTab* s, u32 now)
{
  u32 cs;
  if (!t->doc) return;
  cs = t->doc->change_seq();
  if (cs == s->seen_change) return;
  s->seen_change = cs;
  s->last_edit_ms = now;
  if (!s->dirty) { s->dirty = 1; s->first_dirty_ms = now; }
}

static int sm_needs_backup(Tab* t, SmTab* s)
{
  if (!sm_backups_on() || !t->doc || t->ondemand || t->doc->indexing() || s->discarded) return 0;
  if (!t->doc->modified() || sm_empty_untitled(t)) return 0;
  if (s->job) return s->job_change != t->doc->change_seq();   // newer edits than the running job
  return !s->has_backup || s->backed_change != t->doc->change_seq();
}

static int sm_should_delete(Tab* t, SmTab* s)
{
  if (!g_on || s->job || !s->has_backup) return 0;
  return !t->doc || !t->doc->modified() || sm_empty_untitled(t) || s->discarded || !g_app.cfg.backup_enabled;
}

static void sm_mirror(Tab* t, SmTab* s)
{
  t->has_backup = s->has_backup;
  t->backup_gen = s->gen;
  t->backup_seq = s->backed_change;
  t->dirty_since_ms = s->dirty ? s->first_dirty_ms : 0;
}

static void sm_now_filetime(u32* lo, u32* hi)
{
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  *lo = ft.dwLowDateTime; *hi = ft.dwHighDateTime;
}

static void sm_fill_meta(Tab* t, SmTab* s, u32 gen, BackupMeta* m)
{
  memset(m, 0, sizeof(*m));
  m->tabid = t->id;
  m->gen = gen & 0xFFu;
  m->seq = ++g_seq;
  es_copy(m->path, sizeof(m->path), t->untitled ? "" : t->path);
  m->untitled = (u8)(t->untitled || !t->path[0] ? 1 : 0);
  m->eol = (u8)t->doc->eol_style();
  m->untitled_no = t->untitled_no;
  m->enc = t->doc->enc();
  es_copy(m->lang, sizeof(m->lang), t->lang ? t->lang->id : "");
  m->caret = t->sel.caret;
  m->anchor = t->sel.anchor;
  m->disk_size = t->disk.size_lo;
  m->disk_mtime_lo = t->disk.mtime_lo;
  m->disk_mtime_hi = t->disk.mtime_hi;
  sm_now_filetime(&m->written_lo, &m->written_hi);
  (void)s;
}

// Bytes for the worker: a private copy for small documents (immune to anything the UI does to the document), a
// SnapshotSource above SM_COPY_MAX (4.3 exception 1). NULL on failure.
static ByteSource* sm_data_of(Tab* t)
{
  Document* d = t->doc;
  epos n = d->length();
  if (n <= SM_COPY_MAX) {
    MemSource* m = new MemSource();
    u8* p;
    if (!m) return 0;
    p = (u8*)e_alloc(n ? n : 1);
    if (!p) { delete m; return 0; }
    if ((n && d->read(0, p, n) != E_OK) || m->adopt(p, n, 0) != E_OK) { e_free(p); delete m; return 0; }
    return m;
  } else {
    SnapshotSource* snap = new SnapshotSource();
    if (!snap) return 0;
    if (d->snapshot(snap, 0) != E_OK) { delete snap; return 0; }
    return snap;
  }
}

static int sm_submit_backup(Tab* t, SmTab* s)
{
  IoJob* j;
  u32 id, gen = (s->gen + 1) & 0xFFu;
  j = new IoJob;
  if (!j) return E_NOMEM;
  memset(j, 0, sizeof(*j));
  j->kind = IOJ_BACKUP;
  j->tabid = t->id;
  es_copy(j->path, sizeof(j->path), g_bdir);
  sm_fill_meta(t, s, gen, &j->meta);
  j->seq = t->doc->change_seq();
  j->data = sm_data_of(t);
  if (!j->data) { delete j; return E_NOMEM; }
  s->job_order = g_order++;
  j->ctx = (void*)(size_t)s->job_order;
  id = iothread_submit(j);                 // owns j from here
  if (!id) return E_STATE;
  s->job = id;
  s->job_change = t->doc->change_seq();
  s->gen = gen;
  s->dirty = 0;
  sm_mirror(t, s);
  return E_OK;
}

// Deletes every backup generation of tab id 'tabid' (FIFO after any queued backup of that tab).
static void sm_submit_delete(u32 tabid, int tied_to_tab)
{
  IoJob* j = new IoJob;
  int i;
  if (!j) return;
  memset(j, 0, sizeof(*j));
  j->kind = IOJ_DELETE;
  j->tabid = tied_to_tab ? tabid : 0;
  es_copy(j->path, sizeof(j->path), g_bdir);
  j->meta.tabid = tabid;
  j->ctx = (void*)(size_t)g_order;
  i = sm_find(tabid);
  if (i >= 0) {
    g_sm[(u32)i].delete_order = g_order;
    g_sm[(u32)i].has_backup = 0;
  }
  g_order++;
  iothread_submit(j);
}

static void sm_backup_tab_now(Tab* t)
{
  SmTab* s = sm_get(t);
  if (!s) return;
  sm_track(t, s, app_now_ms());
  if (sm_should_delete(t, s)) { sm_submit_delete(t->id, 1); sm_mirror(t, s); return; }
  if (sm_needs_backup(t, s) && !s->job) sm_submit_backup(t, s);
}

// ---------------------------------------------------------------------------------------------------------------
// session text

static u32 sm_hash_step(u32 h, u32 v)
{
  u32 i;
  for (i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFFu; h *= 16777619u; }
  return h;
}

static u32 sm_state_hash()
{
  u32 h = 2166136261u;
  int i, n = app_tab_count();
  h = sm_hash_step(h, (u32)n);
  h = sm_hash_step(h, (u32)app_active_index());
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    const char* p;
    h = sm_hash_step(h, t->id);
    h = sm_hash_step(h, t->sel.anchor);
    h = sm_hash_step(h, t->sel.caret);
    h = sm_hash_step(h, t->tv.top_line);
    h = sm_hash_step(h, t->tv.top_subrow);
    h = sm_hash_step(h, t->hv.top_row);
    h = sm_hash_step(h, (u32)t->view | ((u32)t->tv.wrap << 8) | ((u32)t->tv.tab_width << 16) | ((u32)t->tv.use_spaces << 24));
    h = sm_hash_step(h, (u32)(size_t)t->lang ^ ((u32)t->lang_forced << 31));
    h = sm_hash_step(h, t->bookmarks.count());
    if (t->doc) {
      h = sm_hash_step(h, (u32)t->doc->modified() | ((u32)t->doc->read_only() << 1) | ((u32)t->doc->eol_style() << 2));
      h = sm_hash_step(h, (u32)t->doc->enc().kind | ((u32)t->doc->enc().cp << 8) | ((u32)t->doc->enc().bom << 24));
    }
    for (p = t->path; *p; p++) h = sm_hash_step(h, (u8)*p);
  }
  return h;
}

static int sm_include_tab(Tab* t, SmTab* s, u32 exclude_id)
{
  if (t->id == exclude_id) return 0;
  if (t->untitled || !t->path[0]) {
    if (s && s->discarded) return 0;
    return t->doc && !sm_empty_untitled(t) && t->doc->modified();
  }
  return 1;
}

static int sm_build_session(int clean_exit, u32 exclude_id, EBytes* out, u32* seq_out)
{
  Session s;
  Ini ini;
  EStr txt;
  int i, n = app_tab_count(), rc = E_OK;
  int act = app_active_index();
  session_init(&s);
  s.seq = ++g_seq;
  s.next_tabid = g_app.next_tabid;
  s.next_untitled = g_app.next_untitled;
  s.clean_exit = (u8)(clean_exit ? 1 : 0);
  s.active = 0;
  for (i = 0; i < n && rc == E_OK; i++) {
    Tab* t = app_tab(i);
    int si = sm_find(t->id);
    SmTab* sm = si >= 0 ? &g_sm[(u32)si] : 0;
    SessionTab* st;
    u32 k;
    if (!t->doc || !sm_include_tab(t, sm, exclude_id)) continue;
    st = new SessionTab;
    if (!st) { rc = E_NOMEM; break; }
    st->id = t->id;
    es_copy(st->path, sizeof(st->path), t->untitled ? "" : t->path);
    st->untitled = (u8)(t->untitled || !t->path[0] ? 1 : 0);
    st->view = t->view;
    st->hex_pane = t->hv.caret.pane;
    st->hex_insert = t->hv.caret.insert;
    st->untitled_no = t->untitled_no;
    st->enc = t->doc->enc();
    st->enc_forced = t->enc_forced;
    st->eol = (u8)t->doc->eol_style();
    st->lang_forced = t->lang_forced;
    st->wrap = t->tv.wrap;
    es_copy(st->lang, sizeof(st->lang), t->lang ? t->lang->id : "");
    st->tab_width = t->tv.tab_width;
    st->use_spaces = t->tv.use_spaces;
    st->read_only = (u8)(t->doc->read_only() ? 1 : 0);
    st->modified = (u8)(t->doc->modified() && !(sm && sm->discarded) ? 1 : 0);
    st->has_backup = (u8)(sm && sm->has_backup && !sm->discarded ? 1 : 0);
    st->disk_missing = t->disk_missing;
    st->pad[0] = st->pad[1] = 0;
    st->caret = t->sel.caret;
    st->anchor = t->sel.anchor;
    st->top_line = t->tv.top_line;
    st->top_subrow = t->tv.top_subrow;
    st->hex_top = (epos)t->hv.top_row * 16u;
    st->disk_size = t->disk.size_lo;
    st->disk_mtime_lo = t->disk.mtime_lo;
    st->disk_mtime_hi = t->disk.mtime_hi;
    for (k = 0; k < t->bookmarks.count() && rc == E_OK; k++) rc = st->bookmarks.push(t->bookmarks.at(k));
    if (i == act) s.active = s.tabs.size();
    if (rc == E_OK) rc = s.tabs.push(st);
    if (rc != E_OK) delete st;
  }
  if (rc == E_OK) rc = session_to_ini(s, &ini);
  if (rc == E_OK) rc = ini.write(&txt);
  if (rc == E_OK) { out->clear(); rc = out->append(txt.c_str(), txt.len()); }
  if (seq_out) *seq_out = s.seq;
  session_free(&s);
  return rc;
}

static int sm_write_session_sync(int clean_exit)
{
  EBytes b;
  int which = g_which, rc;
  if (!g_on || g_app.nosession) return E_OK;
  rc = sm_build_session(clean_exit, 0, &b, 0);
  if (rc == E_OK) rc = ab_write(sm_fs(), g_s1, g_s2, g_which, b.data(), b.size(), &which);
  if (rc == E_OK) { g_which = which; g_sess_hash = sm_state_hash(); g_sess_dirty = 0; }
  selftest_log("session written (clean_exit=%d): %s", clean_exit, e_errname(rc));
  return rc;
}

static void sm_submit_session(u32 exclude_id)
{
  IoJob* j;
  EBytes* b;
  u32 seq = 0;
  if (!g_on || g_app.nosession) return;
  if (g_sess_job) { g_sess_dirty = 1; return; }
  b = new EBytes();
  if (!b) { g_sess_dirty = 1; return; }
  if (sm_build_session(0, exclude_id, b, &seq) != E_OK) { delete b; g_sess_dirty = 1; return; }
  j = new IoJob;
  if (!j) { delete b; g_sess_dirty = 1; return; }
  memset(j, 0, sizeof(*j));
  j->kind = IOJ_SESSION;
  es_copy(j->path, sizeof(j->path), g_s1);
  es_copy(j->path2, sizeof(j->path2), g_s2);
  j->bytes = b;
  j->last_which = g_which;
  j->seq = seq;
  g_sess_job = iothread_submit(j);
  g_sess_dirty = g_sess_job ? 0 : 1;
  g_sess_hash = sm_state_hash();
  g_state_check_ms = app_now_ms();
}

// ---------------------------------------------------------------------------------------------------------------
// I/O completion (iothread.cpp -> here, UI thread)

void sessmgr_io_done(IoJob* j)
{
  int i;
  SmTab* s;
  Tab* t;
  if (!j) return;
  if (j->kind == IOJ_SESSION) {
    if (j->id == g_sess_job) g_sess_job = 0;
    if (j->result == E_OK) { g_which = j->which; g_note_sess_fail = 0; }
    else if (j->result != E_CANCEL && !g_note_sess_fail) {
      g_note_sess_fail = 1;
      app_status_note("Could not write the session file (%s)", e_errname(j->result));
    }
    selftest_log("session job: %s", e_errname(j->result));
    if (g_sess_dirty && g_on) sm_submit_session(0);
    return;
  }
  i = sm_find(j->kind == IOJ_DELETE ? j->meta.tabid : j->tabid);
  if (i < 0) return;
  s = &g_sm[(u32)i];
  t = sm_tab_by_id(s->tabid);
  if (j->kind == IOJ_DELETE) {
    selftest_log("backup delete tab %u: %s", j->meta.tabid, e_errname(j->result));
    return;
  }
  // IOJ_BACKUP
  if (s->job == j->id) s->job = 0;
  selftest_log("backup tab %u gen %u: %s", j->tabid, j->meta.gen, e_errname(j->result));
  if (j->result == E_OK) {
    if ((u32)(size_t)j->ctx > s->delete_order) {   // not deleted again by a later delete job
      s->has_backup = 1;
      s->backed_change = j->seq;
    }
    s->fail_count = 0;
    s->note_fail = 0;
  } else if (j->result == E_EXISTS) {
    s->retry_ms = app_now_ms();                    // a valid file with that generation exists: next one
  } else if (j->result != E_CANCEL) {
    u32 wait = 5000u << (s->fail_count < 4 ? s->fail_count : 4);
    s->fail_count++;
    s->retry_ms = app_now_ms() + (wait > 60000u ? 60000u : wait);
    if (t && !s->note_fail) {
      char name[280];
      s->note_fail = 1;
      sm_name(t, name, sizeof(name));
      app_status_note("Could not back up %s (%s); Pica keeps trying", name, e_errname(j->result));
    }
  }
  if (t) sm_mirror(t, s);
}

// ---------------------------------------------------------------------------------------------------------------
// scheduling

static void sm_check_tabs(int force)
{
  u32 now = app_now_ms();
  i32 secs = g_app.cfg.backup_seconds;
  int i, n = app_tab_count();
  if (secs < 1) secs = 1;
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    SmTab* s = sm_get(t);
    if (!s || !t->doc) continue;
    sm_track(t, s, now);
    if (sm_should_delete(t, s)) { sm_submit_delete(t->id, 1); sm_mirror(t, s); continue; }
    if (t->ondemand && t->doc->modified() && !s->note_ondemand && sm_backups_on()) {
      char name[280];
      s->note_ondemand = 1;
      if (!t->backup_note_shown) {                 // largefile.cpp may have shown its note already (once per tab)
        t->backup_note_shown = 1;
        sm_name(t, name, sizeof(name));
        app_status_note("%s is too large for backups: save it to keep your changes", name);
      }
    }
    if (!sm_needs_backup(t, s) || s->job) continue;
    if (!force) {
      if ((i32)(now - s->retry_ms) < 0) continue;
      if (!s->dirty) { s->dirty = 1; s->first_dirty_ms = s->last_edit_ms = now; }   // e.g. restored state
      if (now - s->last_edit_ms < (u32)secs * 1000u && now - s->first_dirty_ms < (u32)SM_MAX_DIRTY_MS) continue;
    }
    sm_submit_backup(t, s);
  }
}

void sessmgr_doc_changed(Tab* t)
{
  SmTab* s;
  if (!g_on || !t) return;
  s = sm_get(t);
  if (s) sm_track(t, s, app_now_ms());
}

void sessmgr_state_changed()
{
  if (!g_on || g_app.nosession || g_manual || !g_app.frame) return;
  SetTimer(g_app.frame, TIMER_SESSION, SM_SESSION_DEBOUNCE_MS, 0);
}

static void sm_exit_reset()
{
  u32 i;
  g_exit_done = 0;
  for (i = 0; i < g_sm.size(); i++) g_sm[i].discarded = 0;
}

void sessmgr_timer(UINT id)
{
  if (id == TIMER_SESSION) {
    if (g_app.frame) KillTimer(g_app.frame, TIMER_SESSION);
    if (g_on && !g_manual && !g_app.exiting && !g_in_exit) sm_submit_session(0);
    return;
  }
  if (id != TIMER_BACKUP) return;
  if (g_exit_done && !g_app.exiting) sm_exit_reset();   // the end of the Windows session was cancelled
  if (!g_on || g_manual || g_app.exiting || g_in_exit) return;
  sm_check_tabs(0);
  if (app_now_ms() - g_state_check_ms >= (u32)SM_STATE_MS) {
    g_state_check_ms = app_now_ms();
    if (g_sess_dirty || sm_state_hash() != g_sess_hash) sm_submit_session(0);
  }
}

void sessmgr_idle()
{
  if (!g_on || g_manual || g_app.exiting || g_in_exit) return;
  if (g_sess_dirty && !g_sess_job) sm_submit_session(0);
}

void sessmgr_app_activated(int active)
{
  if (!g_on || g_manual || g_app.exiting || g_in_exit || active) return;
  sm_check_tabs(1);                                 // 13.5: back up at once when the user switches away
  if (g_sess_dirty || sm_state_hash() != g_sess_hash) sm_submit_session(0);
}

void sessmgr_tab_saved(Tab* t)
{
  SmTab* s;
  if (!t) return;
  notifybar_hide(t, NB_RECOVERED);
  if (!g_on) return;
  s = sm_get(t);
  if (!s) return;
  s->discarded = 0;
  s->dirty = 0;
  s->pc[0] = s->po[0] = 0;
  if (s->has_backup || s->job || s->gen) sm_submit_delete(t->id, 1);   // 13.5: the disk file is the truth
  sm_mirror(t, s);
  sm_submit_session(0);
}

void sessmgr_tab_closed(Tab* t)
{
  int i;
  if (!t) return;
  i = sm_find(t->id);
  if (!g_on) { if (i >= 0) g_sm.erase((u32)i, 1); return; }
  sm_submit_session(t->id);                         // 13.4: the session without the tab first, then its backups
  if (i >= 0 && (g_sm[(u32)i].has_backup || g_sm[(u32)i].gen || g_sm[(u32)i].restored)) sm_submit_delete(t->id, 0);
  i = sm_find(t->id);
  if (i >= 0) g_sm.erase((u32)i, 1);
}

// ---------------------------------------------------------------------------------------------------------------
// notification bars of restored tabs

static void sm_format_time(u32 lo, u32 hi, char* out, u32 cap)
{
  FILETIME ft, lft;
  SYSTEMTIME st;
  ft.dwLowDateTime = lo; ft.dwHighDateTime = hi;
  out[0] = 0;
  if (!lo && !hi) return;
  if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return;
  es_fmt(out, cap, "%02u:%02u", (u32)st.wHour, (u32)st.wMinute);
}

static void sm_restore_pair(Tab* t);

static void sm_bar_fn(Tab* t, int kind, int id, void* ctx)
{
  SmTab* s;
  (void)kind; (void)ctx;
  if (!t || app_tab_index(t) < 0) return;
  switch (id) {
  case SMB_SAVE:
    filecmd_save(t, 0);
    break;
  case SMB_REVERT:
    docio_revert(t);
    break;
  case SMB_RELOAD:
    if (docio_reload(t, 0, 1) == E_OK) {
      t->doc->set_always_modified(false);
      app_update_tab(t);
      if (g_on) { s = sm_get(t); if (s && sm_should_delete(t, s)) { sm_submit_delete(t->id, 1); sm_mirror(t, s); } }
    }
    break;
  case SMB_KEEP:                                    // the disk state is already the current one
    app_status_note("Kept your version; saving it will replace the file on disk");
    break;
  case SMB_RESTORE_PC:
    sm_restore_pair(t);
    break;
  default:
    break;
  }
}

static void sm_show_recovered_bar(Tab* t, const BackupMeta* m, int crashed, int changed_on_disk)
{
  NotifyButton b[2];
  char text[300], name[280], when[16];
  u32 n = 0;
  sm_name(t, name, sizeof(name));
  memset(b, 0, sizeof(b));
  if (changed_on_disk) {
    es_fmt(text, sizeof(text), "%s changed on disk after your unsaved changes were backed up.", name);
    b[0].id = SMB_RELOAD; es_copy(b[0].text, sizeof(b[0].text), "Reload from disk");
    b[1].id = SMB_KEEP; es_copy(b[1].text, sizeof(b[1].text), "Keep my version");
    notifybar_show(t, NB_CHANGED, text, b, 2, sm_bar_fn, 0);
    return;
  }
  if (!crashed && !t->disk_missing) return;        // kept at a normal exit: the "*" marker is enough
  when[0] = 0;
  if (m) sm_format_time(m->written_lo, m->written_hi, when, sizeof(when));
  if (t->disk_missing)
    es_fmt(text, sizeof(text), "%s no longer exists on disk. Your unsaved changes were restored from a backup%s%s.",
           name, when[0] ? " made at " : "", when);
  else
    es_fmt(text, sizeof(text), "Recovered: Pica did not close normally. These unsaved changes were restored from a "
           "backup%s%s.", when[0] ? " made at " : "", when);
  b[0].id = SMB_SAVE; es_copy(b[0].text, sizeof(b[0].text), t->untitled ? "Save As..." : "Save");
  n = 1;
  if (!t->untitled && !t->disk_missing) {
    b[1].id = SMB_REVERT; es_copy(b[1].text, sizeof(b[1].text), "Revert to saved");
    n = 2;
  }
  notifybar_show(t, NB_RECOVERED, text, b, n, sm_bar_fn, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// restore (13.7)

static epos sm_clamp_pos(Document* d, epos p)
{
  epos org = d->text_origin(), len = d->length();
  if (p < org) p = org;
  if (p > len) p = len;
  if (!d->indexing()) p = d->snap_char(p);
  return p;
}

static int sm_untitled_used(u32 no)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) if (app_tab(i)->untitled && app_tab(i)->untitled_no == no) return 1;
  return 0;
}

// Session view state onto a freshly opened tab.
static void sm_apply_state(Tab* t, const SessionTab* st, int set_sel)
{
  Document* d = t->doc;
  u32 lines, k;
  if (!d || !st) return;
  if (st->lang_forced) {
    const LangInfo* li = st->lang[0] ? lang_find(st->lang) : 0;
    if (li || !st->lang[0]) docio_set_lang(t, li, 1);
  }
  lines = d->line_count();
  if (set_sel) {
    t->sel.anchor = sm_clamp_pos(d, st->anchor);
    t->sel.caret = sm_clamp_pos(d, st->caret);
  }
  t->hv.caret.pos = t->sel.caret;
  t->hv.caret.nibble = 0;
  t->hv.caret.pane = (u8)(st->hex_pane ? 1 : 0);
  t->hv.caret.insert = (u8)(st->hex_insert ? 1 : 0);
  t->tv.top_line = d->indexing() || st->top_line < lines ? st->top_line : (lines ? lines - 1 : 0);
  t->tv.top_subrow = st->top_subrow;
  if (!t->large) t->tv.wrap = (u8)(st->wrap ? 1 : 0);
  if (st->tab_width >= 1 && st->tab_width <= 16) t->tv.tab_width = st->tab_width;
  t->tv.use_spaces = (u8)(st->use_spaces ? 1 : 0);
  t->hv.top_row = (u32)(st->hex_top / 16u);
  t->view = (u8)(st->view == VIEW_HEX ? VIEW_HEX : VIEW_TEXT);
  if (st->bookmarks.size()) {
    Vec<u32> bm;
    for (k = 0; k < st->bookmarks.size(); k++) if (d->indexing() || st->bookmarks[k] < lines) bm.push(st->bookmarks[k]);
    t->bookmarks.set(bm.data(), bm.size());
  }
  if (st->read_only && !d->read_only()) d->set_read_only(true);
}

// A tab from a backup (13.7 step 3/4). st may be NULL (orphan). Returns the tab or NULL.
static Tab* sm_open_backup(const BackupMeta& m, const SessionTab* st, int crashed)
{
  ByteSource* src = 0;
  Document* d;
  Tab* t;
  SmTab* s;
  EncSpec enc = m.enc;
  u8 bom[4];
  u32 bl = 0, keep_id = g_app.next_tabid;
  const LangInfo* li;
  FsStat fst;
  int rc, changed = 0, forced;
  char path[260];
  rc = backup_open(sm_fs(), g_bdir, m, &src);
  if (rc != E_OK) {
    app_status_note("A backup could not be read (%s)", e_errname(rc));
    return 0;
  }
  if (enc.bom) {
    u8 got[4];
    bl = enc_bom(enc, bom);
    if (!bl || src->size() < bl || src->read(0, got, bl) != E_OK || memcmp(got, bom, bl) != 0) bl = 0;
  }
  enc.bom = (u8)(bl ? 1 : 0);
  d = new Document();
  if (!d) { delete src; return 0; }
  {
    UiProgress prog("Restoring", "Restoring unsaved changes...", 0);
    rc = d->init_source(src, enc, bl, (EolStyle)(m.eol <= EOL_CR ? m.eol : EOL_CRLF), &prog);
  }
  if (rc != E_OK) { delete d; app_status_note("A backup could not be restored (%s)", e_errname(rc)); return 0; }
  d->set_always_modified(true);
  if (!sm_tab_by_id(m.tabid) && m.tabid < keep_id) g_app.next_tabid = m.tabid;
  t = tab_new(d);                                  // owns d, also on failure
  g_app.next_tabid = keep_id;
  if (!t) return 0;
  es_copy(path, sizeof(path), m.untitled ? "" : m.path);
  if (path[0] && tab_find_path(path)) path[0] = 0; // that file is open in another tab: keep this one untitled
  memset(&fst, 0, sizeof(fst));
  if (path[0]) {
    es_copy(t->path, sizeof(t->path), path);
    t->untitled = 0;
    if (sm_fs()->stat(path, &fst) == E_OK && fst.exists) {
      t->disk = fst;
      t->disk_missing = 0;
      changed = fst.size_lo != m.disk_size || fst.size_hi != 0 || fst.mtime_lo != m.disk_mtime_lo ||
                fst.mtime_hi != m.disk_mtime_hi;
      t->ro_attr = (u8)(fst.attrs & FSA_READONLY ? 1 : 0);
    } else {
      t->disk_missing = 1;
    }
  } else {
    t->path[0] = 0;
    t->untitled = 1;
    t->untitled_no = m.untitled_no ? m.untitled_no : (st ? st->untitled_no : 0);
    if (!t->untitled_no || sm_untitled_used(t->untitled_no)) t->untitled_no = g_app.next_untitled++;
    if (t->untitled_no >= g_app.next_untitled) g_app.next_untitled = t->untitled_no + 1;
  }
  t->large = (u8)(d->length() >= (epos)g_app.cfg.large_mb * 1024u * 1024u ? 1 : 0);
  if (t->large) t->tv.wrap = 0;
  li = m.lang[0] ? lang_find(m.lang) : 0;
  if (st) forced = st->lang_forced;
  else forced = t->path[0] ? li != lang_for_file(ui_path_name(t->path)) : li != 0;
  docio_set_lang(t, li, forced);
  t->enc_forced = (u8)(st && st->enc_forced ? 1 : 0);
  if (st) sm_apply_state(t, st, 0);
  t->sel.anchor = sm_clamp_pos(d, m.anchor);
  t->sel.caret = sm_clamp_pos(d, m.caret);
  t->hv.caret.pos = t->sel.caret;
  if (!st) {
    u32 line = d->line_of(t->sel.caret);
    t->tv.top_line = line > 5 ? line - 5 : 0;
  }
  if (app_add_tab(t, 0) != E_OK) { tab_free(t); return 0; }
  s = sm_get(t);
  if (s) {
    s->restored = 1;
    if (t->id == m.tabid) { s->has_backup = 1; s->gen = m.gen; s->backed_change = d->change_seq(); }
    s->seen_change = d->change_seq();
    sm_mirror(t, s);
  }
  sm_show_recovered_bar(t, &m, crashed, changed);
  selftest_log("restored tab %u from backup gen %u (%u bytes)%s", m.tabid, m.gen, (u32)d->length(),
               changed ? ", changed on disk" : "");
  return t;
}

// ---- interrupted safe save (T05 note): target missing, ~PO*.TMP (old) + ~PC*.TMP (new) next to it ----

struct SmCand { char name[260]; u32 size; u32 mlo, mhi; };

static int sm_scan_pair(const char* target, int have_disk, u32 dsize, u32 dlo, u32 dhi, char* pc, char* po)
{
  FileSys* fs = sm_fs();
  FsFind f;
  Vec<SmCand> pcs, pos;
  char dir[260];
  int rc;
  u32 i;
  int bpc = -1, bpo = -1, npo_match = 0;
  pc[0] = po[0] = 0;
  ui_path_dir(target, dir, sizeof(dir));
  memset(&f, 0, sizeof(f));
  rc = fs->find_first(dir, &f);
  if (rc != E_OK) return 0;
  for (;;) {
    if (!(f.attrs & FSA_DIR) && (es_wildcard("~PC*.TMP", f.name) || es_wildcard("~PO*.TMP", f.name)) &&
        pcs.size() + pos.size() < 64) {
      SmCand c;
      FsStat st;
      memset(&c, 0, sizeof(c));
      ui_path_join(dir, f.name, c.name, sizeof(c.name));
      if (fs->stat(c.name, &st) == E_OK && st.exists && !st.size_hi) {
        c.size = st.size_lo; c.mlo = st.mtime_lo; c.mhi = st.mtime_hi;
        if (es_nicmp(f.name, "~PC", 3) == 0) pcs.push(c); else pos.push(c);
      }
    }
    if (fs->find_next(&f) != E_OK) break;
  }
  fs->find_close(&f);
  if (!pcs.size()) return 0;
  if (have_disk) {
    for (i = 0; i < pos.size(); i++)
      if (pos[i].size == dsize && pos[i].mlo == dlo && pos[i].mhi == dhi) { bpo = (int)i; npo_match++; }
    if (npo_match != 1) return 0;                  // the old copy must be exactly the file we knew
  } else {
    if (pos.size() != 1 || pcs.size() != 1) return 0;   // no disk state: only an unambiguous pair
    bpo = 0;
  }
  for (i = 0; i < pcs.size(); i++) {               // the newest new copy
    if (bpc < 0 || pcs[i].mhi > pcs[(u32)bpc].mhi || (pcs[i].mhi == pcs[(u32)bpc].mhi && pcs[i].mlo > pcs[(u32)bpc].mlo))
      bpc = (int)i;
  }
  if (pcs[(u32)bpc].size > SM_PAIR_MAX_BYTES) return 0;
  es_copy(pc, 260, pcs[(u32)bpc].name);
  es_copy(po, 260, pos[(u32)bpo].name);
  return 1;
}

static Tab* sm_open_pair(const char* target, const char* pc, const char* po, const SessionTab* st)
{
  OpenOpts o;
  Tab* t = 0;
  SmTab* s;
  NotifyButton b[1];
  char text[300], keep[260];
  u32 keep_id = g_app.next_tabid;
  int n0 = app_tab_count(), rc;
  docio_default_opts(&o);
  o.no_activate = 1;
  o.from_session = 1;
  if (st && st->enc_forced) { o.force_enc = 1; o.enc = st->enc; }
  if (st && !sm_tab_by_id(st->id) && st->id < keep_id) g_app.next_tabid = st->id;
  rc = docio_open(pc, &o, &t);
  g_app.next_tabid = keep_id;
  if (rc != E_OK || !t || app_tab_count() == n0) return 0;
  watch_tab_removed(t);
  es_copy(t->path, sizeof(t->path), target);
  t->untitled = 0;
  memset(&t->disk, 0, sizeof(t->disk));
  t->disk_missing = 1;
  watch_tab_added(t);
  if (!st || !st->lang_forced) docio_set_lang(t, lang_for_file(ui_path_name(target)), 0);
  if (st) sm_apply_state(t, st, 1);
  t->doc->set_always_modified(true);
  s = sm_get(t);
  if (s) {
    es_copy(s->pc, sizeof(s->pc), pc);
    es_copy(s->po, sizeof(s->po), po);
    s->pair_change = t->doc->change_seq();
    s->seen_change = t->doc->change_seq();
  }
  es_copy(keep, sizeof(keep), ui_path_name(po));
  es_fmt(text, sizeof(text), "%s was being saved when Pica or Windows stopped. This is the new version (from %s); the "
         "previous version is kept as %s.", ui_path_name(target), ui_path_name(pc), keep);
  memset(b, 0, sizeof(b));
  b[0].id = SMB_RESTORE_PC; es_copy(b[0].text, sizeof(b[0].text), "Restore this version");
  notifybar_show(t, NB_RECOVERED, text, b, 1, sm_bar_fn, 0);
  app_update_tab(t);
  selftest_log("interrupted save of %s: showing %s, old copy %s", target, pc, po);
  return t;
}

static void sm_restore_pair(Tab* t)
{
  FileSys* fs = sm_fs();
  SmTab* s = sm_get(t);
  FsStat st;
  char name[280];
  int rc;
  if (!s || !s->pc[0]) return;
  sm_name(t, name, sizeof(name));
  if (fs->stat(t->path, &st) == E_OK && st.exists) {
    ui_error("%s exists again on disk, so it was not replaced. The recovered copy is still %s.", name, s->pc);
    return;
  }
  rc = fs->move(s->pc, t->path);
  if (rc != E_OK) {
    ui_error("%s could not be restored (%s). The new version is still %s, the previous one %s.", name, e_errname(rc),
             s->pc, s->po);
    return;
  }
  if (fs->stat(t->path, &st) == E_OK && st.exists) t->disk = st;
  t->disk_missing = 0;
  if (t->doc->change_seq() == s->pair_change) t->doc->set_saved();
  else t->doc->set_always_modified(false);
  watch_tab_saved(t);
  app_status_note("Restored %s; the previous version is kept as %s", name, s->po);
  selftest_log("interrupted save restored: %s -> %s", s->pc, t->path);
  s->pc[0] = 0;
  app_update_tab(t);
  app_update_title();
  app_update_status();
}

// A named session tab without a backup: open the file (or recover an interrupted save). 1 = opened, 0 = missing.
static int sm_open_file(const SessionTab* st, Tab** out)
{
  OpenOpts o;
  FsStat fst;
  Tab* t = 0;
  u32 keep_id = g_app.next_tabid;
  int n0 = app_tab_count(), rc;
  *out = 0;
  if (sm_fs()->stat(st->path, &fst) != E_OK || !fst.exists) {
    char pc[260], po[260];
    if (sm_scan_pair(st->path, st->disk_size || st->disk_mtime_lo || st->disk_mtime_hi, st->disk_size,
                     st->disk_mtime_lo, st->disk_mtime_hi, pc, po)) {
      *out = sm_open_pair(st->path, pc, po, st);
      if (*out) return 1;
    }
    return 0;
  }
  docio_default_opts(&o);
  o.no_activate = 1;
  o.from_session = 1;
  o.hex = (u8)(st->view == VIEW_HEX ? 1 : 0);
  if (st->enc_forced) { o.force_enc = 1; o.enc = st->enc; }
  if (!sm_tab_by_id(st->id) && st->id < keep_id) g_app.next_tabid = st->id;
  rc = docio_open(st->path, &o, &t);
  g_app.next_tabid = keep_id;
  if (rc != E_OK || !t) return rc == E_NOTFOUND ? 0 : 1;   // other errors were reported by docio
  if (app_tab_count() > n0) sm_apply_state(t, st, 1);   // (an already open tab keeps its state)
  *out = t;
  return 1;
}

static void sm_refresh_active()
{
  Tab* t = app_active();
  if (!t) return;
  if (t->view == VIEW_HEX) { if (g_app.hexview) hexview_attach(g_app.hexview, t); }
  else if (g_app.textview) { textview_attach(g_app.textview, 0); textview_attach(g_app.textview, t); }
  app_update_tab(t);
  app_update_title();
  app_update_status();
}

static void sm_restore_failed_copy(const BackupMeta& m)
{
  char name[13], from[260], dir[260], to[260];
  backup_file_name(m.tabid, m.gen, name);
  ui_path_join(g_bdir, name, from, sizeof(from));
  ui_path_join(g_dir, "recover", dir, sizeof(dir));
  sm_fs()->mkdir(dir);
  ui_path_join(dir, name, to, sizeof(to));
  if (sm_fs()->copy(from, to) == E_OK)
    app_status_note("A backup could not be restored; a copy is kept as %s", to);
}

struct SmRestoreStats { u32 tabs, from_backup, missing, stale, pairs; int crashed; };

static int sm_restore(SmRestoreStats* rs)
{
  FileSys* fs = sm_fs();
  Session s;
  EBytes b;
  Vec<BackupMeta> bl;
  Vec<u8> used;
  Tab* target = 0;
  int have = 0, which = 0, rc;
  u32 i, k, maxid = 0, maxseq = 0, maxun = 0;
  memset(rs, 0, sizeof(*rs));
  session_init(&s);
  rc = ab_read(fs, g_s1, g_s2, session_validate, &b, &which);
  if (rc == E_OK) {
    Ini ini;
    if (ini.parse((const char*)b.data(), b.size()) == E_OK && session_from_ini(ini, &s) == E_OK) have = 1;
  }
  g_which = have ? which : 0;
  rc = backup_list(fs, g_bdir, &bl);
  if (rc != E_OK && rc != E_NOTFOUND) bl.clear();
  // counters: new tab ids above every id in use, the sequence above everything on disk
  if (have) {
    maxseq = s.seq;
    if (s.next_tabid) maxid = s.next_tabid - 1;
    if (s.next_untitled) maxun = s.next_untitled - 1;
    for (i = 0; i < s.tabs.size(); i++) {
      if (s.tabs[i]->id > maxid) maxid = s.tabs[i]->id;
      if (s.tabs[i]->untitled_no > maxun) maxun = s.tabs[i]->untitled_no;
    }
  }
  for (i = 0; i < bl.size(); i++) {
    if (bl[i].seq > maxseq) maxseq = bl[i].seq;
    if (bl[i].tabid > maxid) maxid = bl[i].tabid;
    if (bl[i].untitled_no > maxun) maxun = bl[i].untitled_no;
  }
  if (g_seq <= maxseq) g_seq = maxseq + 1;
  if (g_app.next_tabid <= maxid) g_app.next_tabid = maxid + 1;
  if (g_app.next_untitled <= maxun) g_app.next_untitled = maxun + 1;
  if (used.resize(bl.size()) != E_OK) { session_free(&s); return E_NOMEM; }
  rs->crashed = have ? (s.clean_exit ? 0 : 1) : (bl.size() > 0 ? 1 : 0);
  // 13.7 step 3: session tabs in order
  for (i = 0; i < s.tabs.size(); i++) {
    const SessionTab* st = s.tabs[i];
    Tab* t = 0;
    int bi = -1;
    for (k = 0; k < bl.size(); k++) if (bl[k].tabid == st->id) { bi = (int)k; break; }
    if (bi >= 0 && !st->modified && !st->has_backup && bl[(u32)bi].seq < s.seq) {
      used[(u32)bi] = 1;                           // saved or discarded after that backup: superseded
      backup_delete_tab(fs, g_bdir, st->id);
      rs->stale++;
      bi = -1;
    }
    if (bi >= 0) {
      used[(u32)bi] = 1;
      t = sm_open_backup(bl[(u32)bi], st, rs->crashed);
      if (t) rs->from_backup++;
      else sm_restore_failed_copy(bl[(u32)bi]);
    } else if (g_app.cfg.restore_session && !st->untitled && st->path[0]) {
      int n0 = app_tab_count();
      if (!sm_open_file(st, &t)) rs->missing++;
      else if (t && app_tab_count() > n0) { SmTab* sm = sm_get(t); if (sm && sm->pc[0]) rs->pairs++; }
    }
    if (t) { rs->tabs++; if (i == s.active) target = t; }
  }
  // step 4: backups whose tab is not in the session
  for (k = 0; k < bl.size(); k++) {
    Tab* t;
    if (used[k]) continue;
    if (have && bl[k].seq < s.seq) {               // the tab was closed after this backup
      backup_delete_tab(fs, g_bdir, bl[k].tabid);
      rs->stale++;
      continue;
    }
    t = sm_open_backup(bl[k], 0, 1);
    if (t) { rs->tabs++; rs->from_backup++; if (!target) target = t; }
    else sm_restore_failed_copy(bl[k]);
  }
  // interrupted saves of recent files that are not open
  for (i = 0; i < (u32)g_app.cfg.mru_count && i < (u32)CFG_MRU_MAX; i++) {
    const char* p = g_app.cfg.mru[i];
    FsStat fst;
    char pc[260], po[260];
    if (!p[0] || tab_find_path(p)) continue;
    if (fs->stat(p, &fst) != E_OK || fst.exists) continue;
    if (sm_scan_pair(p, 0, 0, 0, 0, pc, po) && sm_open_pair(p, pc, po, 0)) { rs->tabs++; rs->pairs++; }
  }
  session_free(&s);
  if (target) app_activate(app_tab_index(target));
  else if (app_active_index() < 0 && app_tab_count()) app_activate(0);
  sm_refresh_active();
  g_sess_hash = 0;                                 // write the session at the next check
  selftest_log("restore: %u tabs, %u from backups, %u missing, %u stale, %u interrupted saves, crashed=%d", rs->tabs,
               rs->from_backup, rs->missing, rs->stale, rs->pairs, rs->crashed);
  return E_OK;
}

static void sm_restore_note(const SmRestoreStats& rs)
{
  char text[400];
  text[0] = 0;
  if (rs.crashed && rs.from_backup) es_copy(text, sizeof(text), "Pica did not close normally; unsaved changes were "
                                            "restored from backups.");
  else if (rs.from_backup) es_fmt(text, sizeof(text), "%u unsaved document%s from the last session restored.",
                                  rs.from_backup, rs.from_backup == 1 ? "" : "s");
  if (rs.pairs) {
    if (text[0]) es_cat(text, sizeof(text), " ");
    es_cat(text, sizeof(text), "An interrupted save was found: see the bar above the text.");
  }
  if (rs.missing) {
    char m[120];
    es_fmt(m, sizeof(m), "%u file%s from the last session %s not found.", rs.missing, rs.missing == 1 ? "" : "s",
           rs.missing == 1 ? "was" : "were");
    if (text[0]) es_cat(text, sizeof(text), " ");
    es_cat(text, sizeof(text), m);
  }
  if (text[0]) app_status_note("%s", text);
}

int sessmgr_restore_from(const char* dir)
{
  SmRestoreStats rs;
  int rc;
  if (!dir || !dir[0]) return E_STATE;
  sm_set_dir(dir);
  g_on = 1;
  g_state_check_ms = app_now_ms();
  rc = sm_restore(&rs);
  sm_restore_note(rs);
  return rc;
}

// Only the counters (for -nosession): never reuse a tab id or sequence number that is on disk.
static void sm_scan_counters()
{
  FileSys* fs = sm_fs();
  Vec<BackupMeta> bl;
  EBytes b;
  u32 i, seq = 0, maxid = 0;
  int which = 0;
  if (ab_read(fs, g_s1, g_s2, session_validate, &b, &which) == E_OK) {
    Ini ini;
    Session s;
    session_init(&s);
    if (ini.parse((const char*)b.data(), b.size()) == E_OK && session_from_ini(ini, &s) == E_OK) {
      seq = s.seq;
      if (s.next_tabid) maxid = s.next_tabid - 1;
    }
    session_free(&s);
  }
  if (backup_list(fs, g_bdir, &bl) == E_OK)
    for (i = 0; i < bl.size(); i++) { if (bl[i].seq > seq) seq = bl[i].seq; if (bl[i].tabid > maxid) maxid = bl[i].tabid; }
  if (g_seq <= seq) g_seq = seq + 1;
  if (g_app.next_tabid <= maxid) g_app.next_tabid = maxid + 1;
}

void sessmgr_startup()
{
  SmRestoreStats rs;
  sm_set_dir(g_app.cfg_dir);
  g_on = 1;
  g_state_check_ms = app_now_ms();
  if (g_app.nosession) {                           // 13.7: no restore, no session; backups still protect edits
    sm_scan_counters();
    return;
  }
  sm_restore(&rs);
  sm_write_session_sync(0);                        // step 6: clean_exit=0 until a normal exit
  sm_restore_note(rs);
}

// ---------------------------------------------------------------------------------------------------------------
// exit (15.9)

// Waits for the I/O queue; after 300 ms with a "Still writing backups..." progress. 1 = empty, 0 = gave up.
static int sm_flush(u32 timeout_ms)
{
  u32 start;
  if (iothread_flush(300)) return 1;
  {
    UiProgress prog("Exiting", "Still writing backups...", 1);
    start = app_now_ms();
    for (;;) {
      u32 el;
      if (iothread_flush(100)) return 1;
      el = app_now_ms() - start;
      if (prog.progress(el < timeout_ms ? el : timeout_ms, timeout_ms)) return 0;
      if (el >= timeout_ms) return 0;
    }
  }
}

static void sm_discard(Tab* t)
{
  SmTab* s = sm_get(t);
  if (!s) return;
  s->discarded = 1;
  if (g_on) sm_submit_delete(t->id, 1);
  sm_mirror(t, s);
}

static int sm_exit_flow();

int sessmgr_exit()
{
  int r;
  g_in_exit = 1;
  r = sm_exit_flow();
  g_in_exit = 0;
  return r;
}

static int sm_exit_flow()
{
  Vec<Tab*> list;
  Vec<u8> flags;
  int i, n, r, backups = sm_backups_on() ? 1 : 0;
  u32 k, unsaved;
  g_last_prompt_n = 0;
  // running saves complete first (their result decides what is still modified)
  for (i = 0; i < app_tab_count(); i++) if (app_tab(i)->saving) docio_wait_save(app_tab(i));
  n = app_tab_count();
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    if (!t->doc || !t->doc->modified()) continue;
    if (t->untitled || !t->path[0]) {
      if (sm_empty_untitled(t)) continue;          // nothing to lose
      if (backups && !g_app.cfg.ask_untitled_on_exit) continue;   // kept for the next session (15.9)
    }
    if (list.push(t) != E_OK) return 0;
  }
  if (list.size()) {
    if (flags.resize(list.size()) != E_OK) return 0;
    g_last_prompt_n = list.size();
    r = dlg_exit_prompt(list.data(), list.size(), flags.data());
    if (r < 0) {                                   // the dialog could not be shown: plain question
      int a = ui_ask(Q_EXIT, "Save the changes to your modified files before exiting?",
                     MB_YESNOCANCEL | MB_ICONQUESTION);
      r = a == IDYES ? EXIT_SAVE : (a == IDNO ? EXIT_DISCARD : EXIT_CANCEL);
      for (k = 0; k < list.size(); k++) flags[k] = 1;
    }
    if (r == EXIT_CANCEL) return 0;
    if (r == EXIT_SAVE) {
      for (k = 0; k < list.size(); k++) {
        Tab* t = list[k];
        if (app_tab_index(t) < 0) continue;
        if (!flags[k]) { sm_discard(t); continue; }
        if (filecmd_save(t, 1) != E_OK || (t->doc && t->doc->modified())) {
          sm_exit_reset();                         // not saved: stay (every message was shown by the save)
          return 0;
        }
      }
    } else if (r == EXIT_KEEP) {
      for (k = 0; k < list.size(); k++) if (list[k]->ondemand || !backups) sm_discard(list[k]);   // "(not kept)"
    } else {
      for (k = 0; k < list.size(); k++) sm_discard(list[k]);
    }
  }
  if (!backups) {                                  // backups off: nothing is kept for the next session
    for (i = 0; i < app_tab_count(); i++) {
      Tab* t = app_tab(i);
      if (t->doc && t->doc->modified()) sm_discard(t);
    }
  }
  if (!g_on) { g_exit_done = 1; return 1; }
  // final backups of everything kept, deletes of everything discarded, then wait for the worker
  for (i = 0; i < app_tab_count(); i++) sm_backup_tab_now(app_tab(i));
  for (;;) {
    if (sm_flush(SM_FLUSH_MS)) break;
    if (ui_ask(Q_SM_FLUSH, "Pica is still writing backups of your unsaved changes (the disk may be slow).\n\n"
               "Keep waiting?\n\nYes: wait 10 more seconds.\nNo: exit now (changes made since the last backup may "
               "be lost).", MB_YESNO | MB_ICONWARNING) != IDYES) break;
  }
  // anything that should be kept but has no current backup?
  unsaved = 0;
  for (i = 0; i < app_tab_count(); i++) {
    Tab* t = app_tab(i);
    SmTab* s = sm_get(t);
    if (s && sm_needs_backup(t, s)) unsaved++;
  }
  if (unsaved) {
    char q[300];
    es_fmt(q, sizeof(q), "Pica could not back up %u document%s with unsaved changes (see the status bar).\n\n"
           "Exit anyway and lose those changes?", unsaved, unsaved == 1 ? "" : "s");
    if (ui_ask(Q_SM_UNSAVED, q, MB_YESNO | MB_ICONWARNING) != IDYES) { sm_exit_reset(); return 0; }
  }
  if (!g_app.nosession) {
    int rc = sm_write_session_sync(1);
    if (rc != E_OK) app_status_note("Could not write the session file (%s)", e_errname(rc));
  }
  g_exit_done = 1;
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// emergency (15.12): called from the SEH guard. Synchronous, no worker, each step guarded.

static int sm_em_backup(Tab* t)
{
  SmTab* s;
  ByteSource* data;
  BackupMeta m;
  int rc;
  if (!t || !t->doc) return E_OK;
  s = sm_get(t);
  if (!s) return E_NOMEM;
  sm_track(t, s, app_now_ms());
  s->job = 0;                                       // the worker was stopped: whatever it ran is gone or complete
  if (!g_app.cfg.backup_enabled || t->ondemand || t->doc->indexing() || !t->doc->modified() || sm_empty_untitled(t))
    return E_OK;
  if (s->has_backup && s->backed_change == t->doc->change_seq()) return E_OK;
  data = sm_data_of(t);
  if (!data) return E_NOMEM;
  s->gen = (s->gen + 1) & 0xFFu;
  sm_fill_meta(t, s, s->gen, &m);
  rc = backup_write(sm_fs(), g_bdir, m, data, 0, 0);
  if (rc == E_EXISTS) {
    s->gen = (s->gen + 1) & 0xFFu;
    m.gen = s->gen;
    rc = backup_write(sm_fs(), g_bdir, m, data, 0, 0);
  }
  delete data;
  if (rc == E_OK) { s->has_backup = 1; s->backed_change = t->doc->change_seq(); }
  return rc;
}

static int sm_em_backup_guarded(Tab* t)
{
  __try {
    return sm_em_backup(t);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return E_IO;
  }
}

static int sm_em_session()
{
  return sm_write_session_sync(0);
}

static int sm_em_session_guarded()
{
  __try {
    return sm_em_session();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return E_IO;
  }
}

void sessmgr_emergency()
{
  int i;
  if (!g_on) return;
  iothread_emergency_stop(3000);
  for (i = 0; i < app_tab_count(); i++) sm_em_backup_guarded(app_tab(i));
  sm_em_session_guarded();
}

// =================================================================================================================
// selftest (st_session): session round trip through a restart simulation (sessmgr_restore_from into fresh tabs),
// byte-exact backups, the changed-on-disk bar, restore_session off, a torn newest backup (FaultFs on the worker),
// orphan and stale backups, an interrupted safe save (~PO/~PC pair), the exit prompt paths and the emergency path.

static void sst_join(const char* a, const char* b, char* out)
{
  ui_path_join(a, b, out, 260);
}

static void sst_clear_dir(const char* dir)
{
  FileSys* fs = fs_os();
  FsFind f;
  char p[260], bdir[260];
  sst_join(dir, "backup", bdir);
  memset(&f, 0, sizeof(f));
  if (fs->find_first(bdir, &f) == E_OK) {
    for (;;) {
      if (!(f.attrs & FSA_DIR)) { sst_join(bdir, f.name, p); fs->remove(p); }
      if (fs->find_next(&f) != E_OK) break;
    }
    fs->find_close(&f);
  }
  sst_join(dir, "session1.ini", p); fs->remove(p);
  sst_join(dir, "session2.ini", p); fs->remove(p);
  CreateDirectoryA(dir, 0);
  CreateDirectoryA(bdir, 0);
}

static void sst_dir(StCtx* c, const char* name, char* out)
{
  char base[260];
  sst_join(c->cfg_dir, "st_sess", base);
  CreateDirectoryA(base, 0);
  sst_join(base, name, out);
  sst_clear_dir(out);
}

static int sst_copy_dir(const char* from, const char* to)
{
  FileSys* fs = fs_os();
  FsFind f;
  char a[260], b[260], fb[260], tb[260];
  int rc = E_OK;
  sst_clear_dir(to);
  sst_join(from, "session1.ini", a); sst_join(to, "session1.ini", b);
  if (ui_file_exists(a)) rc = fs->copy(a, b);
  sst_join(from, "session2.ini", a); sst_join(to, "session2.ini", b);
  if (rc == E_OK && ui_file_exists(a)) rc = fs->copy(a, b);
  sst_join(from, "backup", fb); sst_join(to, "backup", tb);
  memset(&f, 0, sizeof(f));
  if (rc == E_OK && fs->find_first(fb, &f) == E_OK) {
    for (;;) {
      if (!(f.attrs & FSA_DIR)) { sst_join(fb, f.name, a); sst_join(tb, f.name, b); if (fs->copy(a, b) != E_OK) rc = E_IO; }
      if (fs->find_next(&f) != E_OK) break;
    }
    fs->find_close(&f);
  }
  return rc;
}

static u32 sst_backup_count(const char* dir)
{
  Vec<BackupMeta> bl;
  char bdir[260];
  sst_join(dir, "backup", bdir);
  if (backup_list(fs_os(), bdir, &bl) != E_OK) return 0;
  return bl.size();
}

static u32 sst_file_count(const char* dir, const char* mask)
{
  FileSys* fs = fs_os();
  FsFind f;
  u32 n = 0;
  memset(&f, 0, sizeof(f));
  if (fs->find_first(dir, &f) != E_OK) return 0;
  for (;;) {
    if (!(f.attrs & FSA_DIR) && es_wildcard(mask, f.name)) n++;
    if (fs->find_next(&f) != E_OK) break;
  }
  fs->find_close(&f);
  return n;
}

static int sst_doc_is(Tab* t, const char* bytes, u32 n)
{
  EBytes b;
  if (!t || !t->doc || t->doc->length() != n) return 0;
  if (b.resize(n ? n : 1) != E_OK) return 0;
  if (n && t->doc->read(0, b.data(), n) != E_OK) return 0;
  return memcmp(b.data(), bytes, n) == 0;
}

static int sst_file_is(StCtx* c, const char* path, const char* bytes, u32 n)
{
  EBytes b;
  if (st_file_bytes(c, path, &b) != E_OK) return 0;
  return b.size() == n && (n == 0 || memcmp(b.data(), bytes, n) == 0);
}

static void sst_close_all(StCtx* c)
{
  int i;
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  iothread_flush(10000);
  st_pump(c, 10);
}

static void sst_type(Tab* t, const char* s)
{
  t->doc->replace(t->doc->length(), 0, (const u8*)s, es_len(s));
}

static Tab* sst_find_path(const char* name)
{
  int i;
  for (i = 0; i < app_tab_count(); i++)
    if (app_tab(i)->path[0] && es_icmp(ui_path_name(app_tab(i)->path), name) == 0) return app_tab(i);
  return 0;
}

static Tab* sst_find_untitled()
{
  int i;
  for (i = 0; i < app_tab_count(); i++) if (app_tab(i)->untitled) return app_tab(i);
  return 0;
}

static void sst_backup_all()
{
  int i;
  for (i = 0; i < app_tab_count(); i++) sm_backup_tab_now(app_tab(i));
  iothread_flush(10000);
}

struct SstExpect {
  char name[64]; u8 untitled, view, wrap, tab_width, use_spaces, lang_forced, modified, pad;
  EncSpec enc; int eol; const LangInfo* lang; epos anchor, caret; u32 top_line; u32 nbm, bm0, bm1;
};

static void sst_record(Tab* t, SstExpect* e)
{
  memset(e, 0, sizeof(*e));
  es_copy(e->name, sizeof(e->name), t->untitled ? "" : ui_path_name(t->path));
  e->untitled = t->untitled; e->view = t->view; e->wrap = t->tv.wrap; e->tab_width = t->tv.tab_width;
  e->use_spaces = t->tv.use_spaces; e->lang_forced = t->lang_forced;
  e->modified = (u8)(t->doc->modified() ? 1 : 0);
  e->enc = t->doc->enc(); e->eol = (int)t->doc->eol_style(); e->lang = t->lang;
  e->anchor = t->sel.anchor; e->caret = t->sel.caret; e->top_line = t->tv.top_line;
  e->nbm = t->bookmarks.count();
  e->bm0 = e->nbm > 0 ? t->bookmarks.at(0) : 0;
  e->bm1 = e->nbm > 1 ? t->bookmarks.at(1) : 0;
}

static void sst_compare(StCtx* c, Tab* t, const SstExpect* e, const char* what)
{
  if (!t) { st_checkf(c, 0, "%s: tab restored", what); return; }
  st_checkf(c, (e->untitled ? t->untitled != 0 : es_icmp(ui_path_name(t->path), e->name) == 0), "%s: path", what);
  st_checkf(c, t->view == e->view, "%s: view %u (want %u)", what, (u32)t->view, (u32)e->view);
  st_checkf(c, t->tv.wrap == e->wrap && t->tv.tab_width == e->tab_width && t->tv.use_spaces == e->use_spaces,
            "%s: wrap/tab width/spaces", what);
  st_checkf(c, enc_equal(t->doc->enc(), e->enc), "%s: encoding", what);
  st_checkf(c, (int)t->doc->eol_style() == e->eol, "%s: line endings", what);
  st_checkf(c, t->lang == e->lang && t->lang_forced == e->lang_forced, "%s: language %s (want %s)", what,
            t->lang ? t->lang->id : "-", e->lang ? e->lang->id : "-");
  st_checkf(c, t->sel.anchor == e->anchor && t->sel.caret == e->caret, "%s: selection %u-%u (want %u-%u)", what,
            (u32)t->sel.anchor, (u32)t->sel.caret, (u32)e->anchor, (u32)e->caret);
  st_checkf(c, t->tv.top_line == e->top_line, "%s: top line", what);
  st_checkf(c, t->bookmarks.count() == e->nbm && (e->nbm < 1 || t->bookmarks.at(0) == e->bm0) &&
            (e->nbm < 2 || t->bookmarks.at(1) == e->bm1), "%s: bookmarks", what);
  st_checkf(c, (t->doc->modified() ? 1 : 0) == e->modified, "%s: modified state", what);
}

// 1. round trip: two files (one modified), one in hex view with a forced language, an untitled tab
static void sst_roundtrip(StCtx* c)
{
  static const char a_bytes[] = "alpha\r\nbeta\r\ngamma\r\n";
  static const char b_bytes[] = "\xEF\xBB\xBFone\ntwo\nthree\n";
  static const char b_new[] = "\xEF\xBB\xBFone\ntwo\nthree\nfour \xC3\xA4\n";
  static const char c_bytes[] = "int main() { return 0; }\r\n";
  static const char u_text[] = "untitled text\r\nsecond line";
  static const char changed[] = "\xEF\xBB\xBF" "changed on disk\n";
  char d1[260], d2[260], d3[260], d4[260], pa[260], pb[260], pc[260];
  SstExpect ex[4];
  Tab* ta; Tab* tb; Tab* tc; Tab* tu = 0;
  const LangInfo* py = lang_find("python");
  Tab* r;
  u32 bm[2], uno;
  sst_dir(c, "s1", d1); sst_dir(c, "s2", d2); sst_dir(c, "s3", d3); sst_dir(c, "s4", d4);
  sm_set_dir(d1);
  st_fixture(c, "sess_a.txt", a_bytes, sizeof(a_bytes) - 1, pa, sizeof(pa));
  st_fixture(c, "sess_b.txt", b_bytes, sizeof(b_bytes) - 1, pb, sizeof(pb));
  st_fixture(c, "sess_c.c", c_bytes, sizeof(c_bytes) - 1, pc, sizeof(pc));
  ta = st_open(c, pa); tb = st_open(c, pb); tc = st_open(c, pc);
  docio_new(&tu);
  if (!ta || !tb || !tc || !tu) { st_checkf(c, 0, "roundtrip: fixtures open"); return; }
  ta->sel.anchor = 2; ta->sel.caret = 9;
  bm[0] = 0; bm[1] = 2; ta->bookmarks.set(bm, 2);
  ta->tv.wrap = 1; ta->tv.tab_width = 8;
  sst_type(tb, "four \xC3\xA4\n");
  tb->sel.anchor = tb->sel.caret = tb->doc->length();
  if (!py) py = lang_at(1);
  docio_set_lang(tc, py, 1);
  tc->view = VIEW_HEX;
  tc->hv.caret.pane = 1;
  sst_type(tu, u_text);
  tu->sel.anchor = 3; tu->sel.caret = 8;
  uno = tu->untitled_no;
  app_activate(app_tab_index(tb));
  st_pump(c, 20);
  sst_backup_all();
  st_checkf(c, sst_backup_count(d1) == 2, "roundtrip: 2 backups written (got %u)", sst_backup_count(d1));
  st_checkf(c, sm_write_session_sync(0) == E_OK, "roundtrip: session written");
  sst_record(ta, &ex[0]); sst_record(tb, &ex[1]); sst_record(tc, &ex[2]); sst_record(tu, &ex[3]);
  st_checkf(c, sst_copy_dir(d1, d2) == E_OK && sst_copy_dir(d1, d3) == E_OK && sst_copy_dir(d1, d4) == E_OK,
            "roundtrip: copies of the cfg dir");
  // closing a tab writes the session without it, then deletes its backups (13.4)
  sst_close_all(c);
  st_checkf(c, sst_backup_count(d1) == 0, "roundtrip: closing deleted the backups");
  // restart simulation: restore into fresh tabs
  st_checkf(c, sessmgr_restore_from(d2) == E_OK, "roundtrip: restore");
  st_pump(c, 50);
  st_checkf(c, app_tab_count() == 4, "roundtrip: 4 tabs restored (got %d)", app_tab_count());
  if (app_tab_count() == 4) {
    sst_compare(c, app_tab(0), &ex[0], "tab a");
    sst_compare(c, app_tab(1), &ex[1], "tab b");
    sst_compare(c, app_tab(2), &ex[2], "tab c");
    sst_compare(c, app_tab(3), &ex[3], "untitled");
    st_checkf(c, app_active_index() == 1, "roundtrip: active tab restored");
    st_checkf(c, app_tab(2)->hv.caret.pane == 1, "roundtrip: hex pane");
    r = app_tab(1);
    st_checkf(c, sst_doc_is(r, b_new, sizeof(b_new) - 1), "roundtrip: modified file restored byte-exact");
    st_checkf(c, r->notify.kind == NB_RECOVERED, "roundtrip: restored tab marked (bar %d)", r->notify.kind);
    r = app_tab(3);
    st_checkf(c, sst_doc_is(r, u_text, sizeof(u_text) - 1), "roundtrip: untitled restored byte-exact");
    st_checkf(c, r->notify.kind == NB_RECOVERED && r->untitled_no == uno, "roundtrip: untitled marked");
    st_checkf(c, app_tab(0)->notify.kind != NB_RECOVERED, "roundtrip: unmodified file not marked");
    st_checkf(c, sst_doc_is(app_tab(0), a_bytes, sizeof(a_bytes) - 1), "roundtrip: unmodified file from disk");
  }
  sst_close_all(c);
  // the file changed on disk after the backup: bar [Reload from disk] [Keep my version]
  st_fixture(c, "sess_b.txt", changed, sizeof(changed) - 1, 0, 0);
  sessmgr_restore_from(d3);
  st_pump(c, 50);
  r = sst_find_path("sess_b.txt");
  st_checkf(c, r && r->notify.kind == NB_CHANGED, "changed on disk: bar shown");
  if (r) {
    st_checkf(c, sst_doc_is(r, b_new, sizeof(b_new) - 1) && r->doc->modified(), "changed on disk: my version shown");
    sm_bar_fn(r, NB_CHANGED, SMB_RELOAD, 0);
    st_checkf(c, sst_doc_is(r, changed, sizeof(changed) - 1), "changed on disk: Reload from disk");
    st_checkf(c, r->doc->can_undo() && !r->doc->modified(), "changed on disk: reload is undoable, unmodified");
  }
  sst_close_all(c);
  // restore_session off: only the backups of unsaved tabs come back
  st_fixture(c, "sess_b.txt", b_bytes, sizeof(b_bytes) - 1, 0, 0);
  g_app.cfg.restore_session = 0;
  sessmgr_restore_from(d4);
  g_app.cfg.restore_session = 1;
  st_pump(c, 20);
  st_checkf(c, app_tab_count() == 2 && sst_find_path("sess_b.txt") && sst_find_untitled() && !sst_find_path("sess_a.txt"),
            "restore_session off: only the 2 backed-up tabs (got %d)", app_tab_count());
  sst_close_all(c);
  sm_set_dir(d1);
}

// 2. torn newest backup: the previous generation is restored
static void sst_torn(StCtx* c)
{
  char d1[260], d2[260], bdir[260];
  Tab* t = 0;
  int i;
  FaultFs ffs(fs_os());
  sst_dir(c, "t1", d1); sst_dir(c, "t2", d2);
  sm_set_dir(d1);
  sst_join(d1, "backup", bdir);
  docio_new(&t);
  if (!t) { st_checkf(c, 0, "torn: new tab"); return; }
  sst_type(t, "version one");
  sst_backup_all();
  st_checkf(c, sst_backup_count(d1) == 1, "torn: first generation written");
  sst_type(t, " and two");
  ffs.fail_from(FaultFs::F_WRITE, 2, E_IO);       // header written, data fails ...
  ffs.fail_from(FaultFs::F_REMOVE, 1, E_IO);      // ... and the partial file can't be removed (power loss)
  iothread_set_fs(&ffs);
  sst_backup_all();
  iothread_set_fs(0);
  i = sm_find(t->id);
  st_checkf(c, i >= 0 && g_sm[(u32)i].fail_count == 1, "torn: the backup job failed");
  st_checkf(c, sst_file_count(bdir, "T*.BAK") == 2, "torn: torn file left next to the old one (%u files)",
            sst_file_count(bdir, "T*.BAK"));
  sm_write_session_sync(0);
  sst_copy_dir(d1, d2);
  sst_close_all(c);
  sessmgr_restore_from(d2);
  st_pump(c, 20);
  t = sst_find_untitled();
  st_checkf(c, app_tab_count() == 1 && sst_doc_is(t, "version one", 11), "torn: previous generation restored");
  st_checkf(c, t && t->doc->modified(), "torn: restored tab is modified");
  sst_close_all(c);
}

static int sst_put_backup(const char* dir, u32 tabid, u32 seq, const char* path, u32 untitled_no, const char* text)
{
  BackupMeta m;
  MemSource src;
  char bdir[260];
  memset(&m, 0, sizeof(m));
  m.tabid = tabid; m.gen = 1; m.seq = seq;
  es_copy(m.path, sizeof(m.path), path ? path : "");
  m.untitled = (u8)(path ? 0 : 1);
  m.untitled_no = untitled_no;
  m.enc = enc_ansi();
  m.eol = EOL_CRLF;
  if (src.copy_from((const u8*)text, es_len(text)) != E_OK) return E_NOMEM;
  sst_join(dir, "backup", bdir);
  return backup_write(fs_os(), bdir, m, &src, 0, 0);
}

static int sst_put_session(const char* dir, u32 seq, const char* path, u32 id, int clean, u32 dsize, u32 dlo, u32 dhi)
{
  Session s;
  SessionTab* st;
  Ini ini;
  EStr txt;
  char p1[260], p2[260];
  int which = 0, rc;
  session_init(&s);
  s.seq = seq; s.next_tabid = id + 1; s.next_untitled = 1; s.clean_exit = (u8)clean;
  st = new SessionTab;
  memset(st->path, 0, sizeof(st->path));
  st->id = id; es_copy(st->path, sizeof(st->path), path);
  st->untitled = 0; st->view = 0; st->hex_pane = 0; st->hex_insert = 0; st->untitled_no = 0;
  st->enc = enc_ansi(); st->enc_forced = 0; st->eol = 0; st->lang_forced = 0; st->wrap = 0; st->lang[0] = 0;
  st->tab_width = 4; st->use_spaces = 0; st->read_only = 0; st->modified = 0; st->has_backup = 0;
  st->disk_missing = 0; st->pad[0] = st->pad[1] = 0;
  st->caret = st->anchor = 0; st->top_line = st->top_subrow = 0; st->hex_top = 0;
  st->disk_size = dsize; st->disk_mtime_lo = dlo; st->disk_mtime_hi = dhi;
  s.tabs.push(st);
  rc = session_to_ini(s, &ini);
  if (rc == E_OK) rc = ini.write(&txt);
  sst_join(dir, "session1.ini", p1); sst_join(dir, "session2.ini", p2);
  if (rc == E_OK) rc = ab_write(fs_os(), p1, p2, 0, (const u8*)txt.c_str(), txt.len(), &which);
  session_free(&s);
  return rc;
}

// 3. orphan backups: newer than the session -> extra tab; older -> superseded and deleted
static void sst_orphans(StCtx* c)
{
  char d1[260], pa[260], stale[260];
  Vec<BackupMeta> bl;
  char bdir[260];
  Tab* t;
  static const char a_bytes[] = "file a\r\n";
  sst_dir(c, "o1", d1);
  st_fixture(c, "sess_o.txt", a_bytes, sizeof(a_bytes) - 1, pa, sizeof(pa));
  ui_path_join(c->fixdir, "sess_stale.txt", stale, sizeof(stale));
  st_checkf(c, sst_put_session(d1, 50, pa, 3, 1, 0, 0, 0) == E_OK, "orphans: session written");
  st_checkf(c, sst_put_backup(d1, 0x20, 60, 0, 7, "orphan text") == E_OK, "orphans: new orphan written");
  st_checkf(c, sst_put_backup(d1, 0x21, 40, stale, 0, "stale text") == E_OK, "orphans: stale orphan written");
  st_checkf(c, sst_put_backup(d1, 3, 45, pa, 0, "superseded") == E_OK, "orphans: superseded backup written");
  sessmgr_restore_from(d1);
  st_pump(c, 20);
  st_checkf(c, app_tab_count() == 2, "orphans: 2 tabs (got %d)", app_tab_count());
  t = sst_find_path("sess_o.txt");
  st_checkf(c, t && sst_doc_is(t, a_bytes, sizeof(a_bytes) - 1) && !t->doc->modified(), "orphans: session file from disk");
  t = sst_find_untitled();
  st_checkf(c, t && sst_doc_is(t, "orphan text", 11) && t->doc->modified() && t->untitled_no == 7 && t->id == 0x20,
            "orphans: newer orphan restored as 'new 7'");
  st_checkf(c, t && t->notify.kind == NB_RECOVERED, "orphans: recovered tab marked");
  sst_join(d1, "backup", bdir);
  backup_list(fs_os(), bdir, &bl);
  st_checkf(c, bl.size() == 1 && bl[0].tabid == 0x20, "orphans: stale and superseded backups deleted (%u left)", bl.size());
  st_checkf(c, g_app.next_tabid > 0x21, "orphans: new tab ids above every backup id");
  sst_close_all(c);
}

// 4. interrupted safe save: target missing, ~PO (old) and ~PC (new) next to it
static void sst_pair(StCtx* c)
{
  char d1[260], dir[260], target[260], po[260], pc[260];
  static const char old_b[] = "old version\r\n";
  static const char new_b[] = "new version\r\nwith more\r\n";
  FileSys* fs = fs_os();
  FsStat st;
  Tab* t;
  sst_dir(c, "p1", d1);
  ui_path_join(c->fixdir, "pair", dir, sizeof(dir));
  CreateDirectoryA(dir, 0);
  ui_path_join(dir, "pair.txt", target, sizeof(target));
  ui_path_join(dir, "~PO0001.TMP", po, sizeof(po));
  ui_path_join(dir, "~PC0002.TMP", pc, sizeof(pc));
  fs->remove(target); fs->remove(po); fs->remove(pc);
  file_write_direct(fs, po, (const u8*)old_b, sizeof(old_b) - 1);
  file_write_direct(fs, pc, (const u8*)new_b, sizeof(new_b) - 1);
  memset(&st, 0, sizeof(st));
  fs->stat(po, &st);
  st_checkf(c, sst_put_session(d1, 10, target, 4, 0, st.size_lo, st.mtime_lo, st.mtime_hi) == E_OK, "pair: session");
  sessmgr_restore_from(d1);
  st_pump(c, 20);
  t = sst_find_path("pair.txt");
  st_checkf(c, t && sst_doc_is(t, new_b, sizeof(new_b) - 1) && t->disk_missing && t->doc->modified(),
            "pair: the new version is shown, file marked missing");
  st_checkf(c, t && t->notify.kind == NB_RECOVERED, "pair: recovery offered in a bar");
  st_checkf(c, !ui_file_exists(target), "pair: nothing restored silently");
  if (t) {
    sm_bar_fn(t, NB_RECOVERED, SMB_RESTORE_PC, 0);
    st_checkf(c, sst_file_is(c, target, new_b, sizeof(new_b) - 1), "pair: Restore puts the new version in place");
    st_checkf(c, sst_file_is(c, po, old_b, sizeof(old_b) - 1) && !ui_file_exists(pc), "pair: old version kept");
    st_checkf(c, !t->doc->modified() && !t->disk_missing, "pair: tab now matches the disk");
  }
  sst_close_all(c);
  // the old copy does not match the recorded disk state: no offer
  fs->remove(target);
  file_write_direct(fs, pc, (const u8*)new_b, sizeof(new_b) - 1);
  sst_dir(c, "p1", d1);
  sst_put_session(d1, 10, target, 4, 0, st.size_lo + 1, st.mtime_lo, st.mtime_hi);
  sessmgr_restore_from(d1);
  st_pump(c, 20);
  st_checkf(c, app_tab_count() == 0, "pair: unmatched old copy -> no tab (got %d)", app_tab_count());
  sst_close_all(c);
  fs->remove(po); fs->remove(pc); fs->remove(target);
}

// 5. exit prompt paths
static void sst_exit(StCtx* c)
{
  char d1[260], d2[260], pa[260], pb[260];
  static const char a0[] = "exit a\r\n";
  static const char b0[] = "exit b\r\n";
  Tab* ta; Tab* tb; Tab* tu = 0;
  Tab* t;
  int r;
  sst_dir(c, "e1", d1); sst_dir(c, "e2", d2);
  sm_set_dir(d1);
  st_fixture(c, "sess_ea.txt", a0, sizeof(a0) - 1, pa, sizeof(pa));
  st_fixture(c, "sess_eb.txt", b0, sizeof(b0) - 1, pb, sizeof(pb));
  ta = st_open(c, pa); tb = st_open(c, pb);
  docio_new(&tu);
  if (!ta || !tb || !tu) { st_checkf(c, 0, "exit: fixtures"); return; }
  sst_type(ta, "A1");
  sst_type(tb, "B1");
  sst_type(tu, "U1");
  // Cancel
  st_answer(c, Q_EXIT, IDCANCEL);
  r = sessmgr_exit();
  st_checkf(c, r == 0 && ta->doc->modified() && tb->doc->modified(), "exit: Cancel keeps everything");
  st_checkf(c, g_last_prompt_n == 2, "exit: one prompt listing the 2 named files (got %u)", g_last_prompt_n);
  // Keep for Next Session
  st_answer(c, Q_EXIT, IDRETRY);
  r = sessmgr_exit();
  st_checkf(c, r == 1 && sst_backup_count(d1) == 3, "exit: Keep writes backups of all 3 (got %u)", sst_backup_count(d1));
  {
    EBytes b;
    int which = 0;
    Session s;
    Ini ini;
    char p1[260], p2[260];
    session_init(&s);
    sst_join(d1, "session1.ini", p1); sst_join(d1, "session2.ini", p2);
    if (ab_read(fs_os(), p1, p2, session_validate, &b, &which) == E_OK) {
      ini.parse((const char*)b.data(), b.size());
      session_from_ini(ini, &s);
    }
    st_checkf(c, s.tabs.size() == 3 && s.clean_exit == 1, "exit: session with 3 tabs and clean_exit=1");
    session_free(&s);
  }
  sm_exit_reset();
  sst_copy_dir(d1, d2);
  sst_close_all(c);
  sessmgr_restore_from(d2);
  st_pump(c, 20);
  ta = sst_find_path("sess_ea.txt"); tb = sst_find_path("sess_eb.txt"); tu = sst_find_untitled();
  st_checkf(c, app_tab_count() == 3 && ta && tb && tu, "exit: Keep restores the 3 tabs");
  if (!ta || !tb || !tu) { sst_close_all(c); return; }
  st_checkf(c, sst_doc_is(ta, "exit a\r\nA1", 10) && sst_doc_is(tb, "exit b\r\nB1", 10) && sst_doc_is(tu, "U1", 2),
            "exit: kept contents byte-exact");
  st_checkf(c, ta->doc->modified() && ta->notify.kind != NB_RECOVERED, "exit: kept tabs modified, not marked as crash");
  // Save Selected (both ticked by default); the untitled tab is not asked about and stays kept
  st_answer(c, Q_EXIT, IDYES);
  r = sessmgr_exit();
  st_checkf(c, r == 1 && sst_file_is(c, pa, "exit a\r\nA1", 10) && sst_file_is(c, pb, "exit b\r\nB1", 10),
            "exit: Save Selected saved both files");
  st_checkf(c, sst_backup_count(d2) == 1, "exit: saved files lost their backups, untitled kept (%u)", sst_backup_count(d2));
  sm_exit_reset();
  // Discard: the named file is listed unmodified in the session, its backup deleted, disk unchanged
  sst_type(ta, "A2");
  sst_backup_all();
  st_answer(c, Q_EXIT, IDNO);
  r = sessmgr_exit();
  st_checkf(c, r == 1 && sst_file_is(c, pa, "exit a\r\nA1", 10), "exit: Discard leaves the file");
  st_checkf(c, sst_backup_count(d2) == 1, "exit: Discard deleted the backup");
  sm_exit_reset();
  // only an untitled tab is modified: no question at all (an unexpected one would fail the test)
  docio_close(ta, 0); docio_close(tb, 0);
  r = sessmgr_exit();
  st_checkf(c, r == 1 && g_last_prompt_n == 0, "exit: untitled only -> no prompt");
  sm_exit_reset();
  // backups off: the untitled tab is listed; Discard drops it from the session
  g_app.cfg.backup_enabled = 0;
  st_answer(c, Q_EXIT, IDNO);
  r = sessmgr_exit();
  st_checkf(c, r == 1 && g_last_prompt_n == 1, "exit: backups off lists the untitled tab");
  g_app.cfg.backup_enabled = 1;
  iothread_flush(5000);
  st_checkf(c, sst_backup_count(d2) == 0, "exit: backups off -> no backups kept");
  sm_exit_reset();
  t = sst_find_untitled();
  if (t) t->doc->set_saved();
  sst_close_all(c);
}

// 6. emergency: synchronous backups + session with clean_exit=0; -nosession never writes the session
static void sst_emergency(StCtx* c)
{
  char d1[260], d2[260], p1[260];
  Tab* t = 0;
  sst_dir(c, "m1", d1);
  sm_set_dir(d1);
  docio_new(&t);
  if (!t) return;
  sst_type(t, "emergency text");
  sessmgr_emergency();
  st_checkf(c, !iothread_running(), "emergency: worker stopped");
  iothread_start();
  st_checkf(c, sst_backup_count(d1) == 1, "emergency: synchronous backup written");
  sst_join(d1, "session1.ini", p1);
  st_checkf(c, ui_file_exists(p1), "emergency: session written (session1.ini)");
  sst_dir(c, "m2", d2);
  sst_copy_dir(d1, d2);
  t->doc->set_saved();
  sst_close_all(c);
  sessmgr_restore_from(d2);
  st_pump(c, 20);
  t = sst_find_untitled();
  st_checkf(c, t && sst_doc_is(t, "emergency text", 14) && t->notify.kind == NB_RECOVERED,
            "emergency: restored and marked");
  sst_close_all(c);
  sst_dir(c, "m3", d1);
  sm_set_dir(d1);
  g_app.nosession = 1;
  docio_new(&t);
  st_checkf(c, sm_write_session_sync(0) == E_OK, "nosession: write is a no-op");
  sst_join(d1, "session1.ini", p1);
  st_checkf(c, !ui_file_exists(p1), "nosession: no session file");
  g_app.nosession = 0;
  sst_close_all(c);
}

void st_session(StCtx* c)
{
  int on = g_on, manual = g_manual;
  char dir[260];
  u8 be = g_app.cfg.backup_enabled, rs = g_app.cfg.restore_session, au = g_app.cfg.ask_untitled_on_exit;
  es_copy(dir, sizeof(dir), g_dir);
  sst_close_all(c);
  g_on = 1;
  g_manual = 1;                                    // no timers: the test decides when backups and sessions happen
  g_app.cfg.backup_enabled = 1;
  g_app.cfg.restore_session = 1;
  g_app.cfg.ask_untitled_on_exit = 0;
  sst_roundtrip(c);
  sst_torn(c);
  sst_orphans(c);
  sst_pair(c);
  sst_exit(c);
  sst_emergency(c);
  sst_close_all(c);
  g_app.cfg.backup_enabled = be;
  g_app.cfg.restore_session = rs;
  g_app.cfg.ask_untitled_on_exit = au;
  g_manual = manual;
  g_on = on;
  if (dir[0]) sm_set_dir(dir);
  g_sm.release();
}
