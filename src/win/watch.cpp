// watch.cpp - change detection (DESIGN 13.9). Owner after Wave 0: T14.
//
// Every named tab's file is stat'ed (GetFileAttributesExA, ~1 ms on M:) by the frame's poll timer (Config.poll_seconds,
// default 3 s), on activation (WM_ACTIVATEAPP), when a tab becomes active, and when the change notification of its
// folder fires (FindFirstChangeNotificationA for up to 16 folders; they fire for writes made inside the VM, never for
// Mac-side edits on M:, so polling is what catches those). A change (size or mtime differ from Tab::disk, or the
// file is missing / back) must look the same on a second check at least 1 s later before Pica acts on it (a writer
// may still be writing); a private 1 s timer runs while such candidates exist. Changes are acted on only while Pica
// is the active application and no modal window (message box, progress window) is open; otherwise they wait.
//
// Actions: unmodified + Config.reload_silent -> quiet reload (caret and scroll kept, status note); otherwise the
// tab's notification bar "X was changed by another program. [Reload] [Keep my version]"; on-demand documents get
// [Reload] only (their pieces point into the file); a missing file gets "X no longer exists on disk. [Save]
// [Close tab]" and the document becomes always-modified. Our own saves record the new state (watch_tab_saved).
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "watch.h"
#include "watch_int.h"
#include "largefile.h"
#include "docio.h"
#include "docio_int.h"
#include "views.h"
#include "notifybar.h"
#include "progress.h"
#include "iothread.h"
#include "selftest.h"
#include "fsys.h"
#include "config.h"
#include "estr.h"

enum { W_MAX_FOLDERS = 16, W_STABLE_MS = 1000, W_BTN_RELOAD = 1, W_BTN_KEEP = 2, W_BTN_SAVE = 1, W_BTN_CLOSE = 2 };

struct WTab {
  u32 tabid;
  FsStat cand; u32 cand_ms;       // a change seen once, waiting for the stability re-check
  FsStat bar_st;                  // state the NB_CHANGED / NB_MISSING bar was shown for
  u8 has_cand, bar_kind, we_always_mod, mod_before_missing;
  u32 seq_at_missing;
  u32 retry_ms;                   // a quiet reload failed (file locked): retry after this time
};
static Vec<WTab> g_wt;

struct WFolder { char dir[260]; HANDLE h; u8 failed, pad[3]; };
static WFolder g_folders[W_MAX_FOLDERS];
static u32 g_nfolders = 0;
static int g_folders_dirty = 1;
static int g_map[W_MAX_FOLDERS];   // watch_handles index -> folder index
static u32 g_nmap = 0;

static UINT g_timer = 0;
static int g_in_poll = 0, g_in_act = 0, g_test_inactive = 0;
static u32 g_last_active_id = 0;

static WTab* wt_find(u32 tabid)
{
  u32 i;
  for (i = 0; i < g_wt.size(); i++) if (g_wt[i].tabid == tabid) return &g_wt[i];
  return 0;
}

static WTab* wt_get(Tab* t)
{
  WTab* w = wt_find(t->id);
  WTab z;
  if (w) return w;
  memset(&z, 0, sizeof(z));
  z.tabid = t->id;
  if (g_wt.push(z) != E_OK) return 0;
  return &g_wt.back();
}

static int watch_on() { return g_app.cfg.check_disk ? 1 : 0; }

static int same_state(const FsStat& a, const FsStat& b)
{
  if (!a.exists || !b.exists) return !a.exists && !b.exists;
  return a.size_lo == b.size_lo && a.size_hi == b.size_hi && a.mtime_lo == b.mtime_lo && a.mtime_hi == b.mtime_hi;
}

static void recorded_state(Tab* t, FsStat* out)
{
  if (t->disk_missing) memset(out, 0, sizeof(*out));
  else *out = t->disk;
}

static int watchable(Tab* t)
{
  return t && t->doc && !t->untitled && t->path[0] && !t->saving && !t->indexing && !t->doc->indexing();
}

// Pica is the active app and nothing modal is open: changes may be acted on now.
static int can_act()
{
  HWND fg;
  DWORD pid = 0;
  if (g_test_inactive || g_in_act || g_app.exiting || !g_app.frame) return 0;
  if (!IsWindowEnabled(g_app.frame)) return 0;            // a message box or UiProgress disables the frame
  if (g_app.selftest) return 1;
  fg = GetForegroundWindow();
  if (!fg) return 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

// ---- folders and change notifications ----

static int folder_watchable(const char* dir)
{
  char root[8];
  UINT dt;
  if (!dir[0] || dir[1] != ':') return 0;              // UNC paths: polling only
  root[0] = dir[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
  if (root[0] == 'A' || root[0] == 'a' || root[0] == 'B' || root[0] == 'b') return 0;   // floppies: no noise
  dt = GetDriveTypeA(root);
  return dt == DRIVE_FIXED || dt == DRIVE_REMOTE || dt == DRIVE_RAMDISK || dt == DRIVE_REMOVABLE;
}

static void rebuild_folders()
{
  char want[W_MAX_FOLDERS][260];
  u32 nwant = 0, i, k;
  int n = app_tab_count(), ti;
  g_folders_dirty = 0;
  if (watch_on()) {
    for (ti = 0; ti < n && nwant < W_MAX_FOLDERS; ti++) {
      Tab* t = app_tab(ti);
      char dir[260];
      int dup = 0;
      if (!t || t->untitled || !t->path[0]) continue;
      ui_path_dir(t->path, dir, sizeof(dir));
      for (k = 0; k < nwant; k++) if (es_icmp(want[k], dir) == 0) { dup = 1; break; }
      if (dup || !folder_watchable(dir)) continue;
      es_copy(want[nwant], sizeof(want[0]), dir);
      nwant++;
    }
  }
  // close folders no longer wanted
  for (i = 0; i < g_nfolders;) {
    int keep = 0;
    for (k = 0; k < nwant; k++) if (es_icmp(want[k], g_folders[i].dir) == 0) { keep = 1; break; }
    if (keep) { i++; continue; }
    if (g_folders[i].h && g_folders[i].h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_folders[i].h);
    g_folders[i] = g_folders[g_nfolders - 1];
    g_nfolders--;
  }
  // open new ones
  for (k = 0; k < nwant; k++) {
    int have = 0;
    for (i = 0; i < g_nfolders; i++) if (es_icmp(want[k], g_folders[i].dir) == 0) { have = 1; break; }
    if (have || g_nfolders >= W_MAX_FOLDERS) continue;
    {
      WFolder* f = &g_folders[g_nfolders++];
      es_copy(f->dir, sizeof(f->dir), want[k]);
      f->h = FindFirstChangeNotificationA(f->dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                          FILE_NOTIFY_CHANGE_FILE_NAME);
      f->failed = (u8)(f->h == INVALID_HANDLE_VALUE || !f->h ? 1 : 0);
      if (f->failed) f->h = 0;
    }
  }
}

static void close_all_folders()
{
  u32 i;
  for (i = 0; i < g_nfolders; i++)
    if (g_folders[i].h && g_folders[i].h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(g_folders[i].h);
  g_nfolders = 0;
  g_nmap = 0;
}

// ---- acting on a change ----

static void timer_ensure();
static void check_tab(Tab* t, u32 now);

static void name_of(Tab* t, char* out, u32 cap) { tab_title(t, out, cap); }

static void changed_bar_fn(Tab* t, int kind, int button_id, void* ctx);
static void missing_bar_fn(Tab* t, int kind, int button_id, void* ctx);

static void show_changed_bar(Tab* t, WTab* w, const FsStat& st, const char* why)
{
  char name[280], text[300];
  NotifyButton b[2];
  u32 nb = 0;
  name_of(t, name, sizeof(name));
  memset(b, 0, sizeof(b));
  b[nb].id = W_BTN_RELOAD; es_copy(b[nb].text, sizeof(b[nb].text), "Reload"); nb++;
  if (t->ondemand) {
    es_fmt(text, sizeof(text), "%s was changed by another program. Pica reads this large file from disk, so reload "
           "it before working on it.", name);
  } else {
    b[nb].id = W_BTN_KEEP; es_copy(b[nb].text, sizeof(b[nb].text), "Keep my version"); nb++;
    if (why) es_fmt(text, sizeof(text), "%s was changed by another program (%s).", name, why);
    else es_fmt(text, sizeof(text), "%s was changed by another program.", name);
  }
  w->bar_st = st;
  w->bar_kind = NB_CHANGED;
  notifybar_show(t, NB_CHANGED, text, b, nb, changed_bar_fn, 0);
  app_update_tab(t);
}

static void act_missing(Tab* t, WTab* w)
{
  char name[280], text[300];
  NotifyButton b[2];
  name_of(t, name, sizeof(name));
  t->disk_missing = 1;
  w->mod_before_missing = (u8)(t->doc->modified() ? 1 : 0);
  w->seq_at_missing = t->doc->undo_seq();
  w->we_always_mod = (u8)(t->doc->modified() ? 0 : 1);
  t->doc->set_always_modified(true);                    // the exit dialog lists it; Save is enabled
  memset(b, 0, sizeof(b));
  b[0].id = W_BTN_SAVE; es_copy(b[0].text, sizeof(b[0].text), "Save");
  b[1].id = W_BTN_CLOSE; es_copy(b[1].text, sizeof(b[1].text), "Close tab");
  es_fmt(text, sizeof(text), "%s no longer exists on disk.", name);
  memset(&w->bar_st, 0, sizeof(w->bar_st));
  w->bar_kind = NB_MISSING;
  notifybar_show(t, NB_MISSING, text, b, 2, missing_bar_fn, 0);
  selftest_log("watch: %s is missing", t->path);
  app_on_doc_changed(t);
  app_update_title();
}

// The file is back or changed. Unmodified documents reload quietly (if allowed), others get the bar.
static void act_changed(Tab* t, WTab* w, const FsStat& st, u32 now)
{
  char name[280];
  int rc;
  name_of(t, name, sizeof(name));
  if (t->disk_missing) {                                  // it reappeared: treat it as changed
    t->disk_missing = 0;
    if (w->we_always_mod && t->doc->undo_seq() == w->seq_at_missing) t->doc->set_always_modified(false);
    w->we_always_mod = 0;
    if (t->notify.kind == NB_MISSING) notifybar_hide(t, NB_MISSING);
    w->bar_kind = 0;
    app_on_doc_changed(t);
  }
  if (!t->ondemand && !t->doc->modified() && g_app.cfg.reload_silent) {
    rc = docio_reload_ex(t, 0, 1, RL_QUIET);
    if (rc == E_OK) {
      w->retry_ms = 0;
      w->bar_kind = 0;
      if (t->notify.kind == NB_CHANGED) notifybar_hide(t, NB_CHANGED);
      app_status_note("Reloaded %s (changed on disk)", name);
      selftest_log("watch: reloaded %s quietly (%u bytes)", t->path, t->disk.size_lo);
      return;
    }
    if (rc == E_SHARING || rc == E_ACCESS || rc == E_NOTFOUND || rc == E_IO) {   // still being written: retry
      w->cand = st;
      w->cand_ms = now;
      w->has_cand = 1;
      w->retry_ms = now + 2000;
      selftest_log("watch: quiet reload of %s failed (%s), will retry", t->path, e_errname(rc));
      timer_ensure();
      return;
    }
    show_changed_bar(t, w, st, rc == E_CANCEL ? 0 : e_errname(rc));   // Cancel in the progress window: ask
    return;
  }
  selftest_log("watch: %s changed on disk (%s): bar", t->path, t->doc->modified() ? "modified" : "unmodified");
  show_changed_bar(t, w, st, 0);
}

static void act(Tab* t, WTab* w, const FsStat& st, u32 now)
{
  w->has_cand = 0;
  t->change_pending = 0;
  g_in_act = 1;
  if (!st.exists) act_missing(t, w);
  else act_changed(t, w, st, now);
  g_in_act = 0;
}

static void check_tab(Tab* t, u32 now)
{
  FsStat st, rec;
  WTab* w;
  if (!watchable(t)) return;
  w = wt_get(t);
  if (!w) return;
  memset(&st, 0, sizeof(st));
  if (fs_os()->stat(t->path, &st) != E_OK) return;     // unknown (network hiccup): try again later
  if (!st.exists) memset(&st, 0, sizeof(st));
  recorded_state(t, &rec);
  if (same_state(st, rec)) {                            // as we know it (also: changed back)
    w->has_cand = 0;
    t->change_pending = 0;
    if (w->bar_kind == NB_CHANGED) {                    // changed back to what we hold: the question is moot
      if (t->notify.kind == NB_CHANGED) notifybar_hide(t, NB_CHANGED);
      w->bar_kind = 0;
    }
    return;
  }
  if (w->bar_kind && t->notify.kind == w->bar_kind && same_state(st, w->bar_st)) {   // the bar already says so
    w->has_cand = 0;
    return;
  }
  if (!w->has_cand || !same_state(st, w->cand)) {       // first sight (or still moving): re-check in >= 1 s
    w->cand = st;
    w->cand_ms = now;
    w->has_cand = 1;
    timer_ensure();
    return;
  }
  if (now - w->cand_ms < W_STABLE_MS || (w->retry_ms && (i32)(now - w->retry_ms) < 0)) { timer_ensure(); return; }
  if (!can_act()) { t->change_pending = 1; timer_ensure(); return; }
  act(t, w, st, now);
}

// ---- notification bar callbacks ----

static void changed_bar_fn(Tab* t, int kind, int button_id, void* ctx)
{
  WTab* w;
  (void)kind; (void)ctx;
  if (!t || !t->doc || app_tab_index(t) < 0) return;
  w = wt_get(t);
  if (button_id == W_BTN_RELOAD) {
    int rc = docio_reload_ex(t, 0, 1, RL_ASK_LOSSY);
    if (w) w->bar_kind = 0;
    if (rc == E_CANCEL && w) {                         // kept the edits after all: same as Keep
      t->disk = w->bar_st;
      t->change_pending = 0;
    } else if (rc == E_OK) {
      char name[280];
      name_of(t, name, sizeof(name));
      app_status_note("Reloaded %s%s", name, t->doc->can_undo() ? " (Undo brings your version back)" : "");
    }
    return;
  }
  // Keep my version (or the bar was closed): remember the new disk state; ask again only on the next change. The
  // text now differs from the file, so it counts as modified (Save is enabled, exit lists it).
  if (w) {
    if (w->bar_st.exists) t->disk = w->bar_st;
    w->bar_kind = 0;
    w->has_cand = 0;
  }
  t->change_pending = 0;
  if (!t->ondemand && !t->doc->modified()) t->doc->set_always_modified(true);
  app_on_doc_changed(t);
  app_update_title();
}

static void missing_bar_fn(Tab* t, int kind, int button_id, void* ctx)
{
  WTab* w;
  (void)kind; (void)ctx;
  if (!t || !t->doc || app_tab_index(t) < 0) return;
  w = wt_get(t);
  if (w) w->bar_kind = 0;
  if (button_id == W_BTN_SAVE) {
    filecmd_save(t, 0);
  } else if (button_id == W_BTN_CLOSE) {
    int ask = !w || w->mod_before_missing || t->doc->undo_seq() != w->seq_at_missing;
    docio_close(t, ask);
  }
  // [x]: the bar goes away; the document stays always-modified while the file is missing
}

// ---- polling ----

static void CALLBACK watch_timer_proc(HWND hwnd, UINT msg, UINT id, DWORD now)
{
  u32 i, n = 0, ms = app_now_ms();
  (void)hwnd; (void)msg; (void)id; (void)now;
  if (g_in_poll) return;
  g_in_poll = 1;
  for (i = 0; i < g_wt.size(); i++) {
    Tab* t;
    if (!g_wt[i].has_cand) continue;
    t = docio_tab_by_id(g_wt[i].tabid);
    if (!t) continue;
    check_tab(t, ms);
  }
  for (i = 0; i < g_wt.size(); i++) if (g_wt[i].has_cand) n++;
  g_in_poll = 0;
  if (!n && g_timer) { KillTimer(0, g_timer); g_timer = 0; }
}

static void timer_ensure()
{
  if (!g_timer) g_timer = SetTimer(0, 0, W_STABLE_MS, watch_timer_proc);
}

void watch_poll(int all)
{
  int i, n;
  u32 now = app_now_ms();
  (void)all;                                            // every named tab is cheap to stat (1 ms each on M:)
  if (!watch_on() || g_in_poll) return;
  g_in_poll = 1;
  n = app_tab_count();
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    if (!t) continue;
    check_tab(t, now);
    if (app_tab_count() != n) break;                    // a callback closed a tab: next poll does the rest
  }
  g_in_poll = 0;
}

u32 watch_handles(HANDLE* out, u32 max)
{
  u32 i, n = 0;
  Tab* a;
  if (!watch_on()) {
    if (g_nfolders) close_all_folders();
    return 0;
  }
  if (g_folders_dirty) rebuild_folders();
  // a tab that just became active is checked at once (13.9)
  a = app_active();
  if (a && a->id != g_last_active_id) {
    g_last_active_id = a->id;
    if (!g_in_poll) { g_in_poll = 1; check_tab(a, app_now_ms()); g_in_poll = 0; }
  } else if (!a) {
    g_last_active_id = 0;
  }
  g_nmap = 0;
  for (i = 0; i < g_nfolders && n < max; i++) {
    if (g_folders[i].failed || !g_folders[i].h) continue;
    out[n] = g_folders[i].h;
    g_map[n] = (int)i;
    n++;
  }
  g_nmap = n;
  return n;
}

void watch_signalled(u32 i)
{
  WFolder* f;
  char dir[260];
  int k, n;
  u32 now = app_now_ms();
  if (i >= g_nmap) return;
  f = &g_folders[g_map[i]];
  if (!FindNextChangeNotification(f->h)) {              // can't re-arm: polling covers this folder
    FindCloseChangeNotification(f->h);
    f->h = 0;
    f->failed = 1;
  }
  if (g_in_poll) return;
  g_in_poll = 1;
  n = app_tab_count();
  for (k = 0; k < n; k++) {
    Tab* t = app_tab(k);
    if (!t || t->untitled || !t->path[0]) continue;
    ui_path_dir(t->path, dir, sizeof(dir));
    if (es_icmp(dir, f->dir) == 0) check_tab(t, now);
    if (app_tab_count() != n) break;
  }
  g_in_poll = 0;
}

void watch_tab_added(Tab* t)
{
  if (!t) return;
  g_folders_dirty = 1;
  if (!t->untitled && t->path[0]) wt_get(t);
  largefile_tab_added(t);
}

void watch_tab_removed(Tab* t)
{
  u32 i;
  if (!t) return;
  g_folders_dirty = 1;
  for (i = 0; i < g_wt.size(); i++) if (g_wt[i].tabid == t->id) { g_wt.erase(i, 1); break; }
  if (g_last_active_id == t->id) g_last_active_id = 0;
  largefile_tab_removed(t);
}

void watch_tab_saved(Tab* t)
{
  WTab* w;
  if (!t) return;
  w = wt_find(t->id);
  if (w) {
    if (w->bar_kind && t->notify.kind == w->bar_kind) notifybar_hide(t, w->bar_kind);
    w->has_cand = 0;
    w->bar_kind = 0;
    w->we_always_mod = 0;
    w->retry_ms = 0;
  }
  t->change_pending = 0;
}

// ---- selftest hooks ----

void watch_test_set_inactive(int on) { g_test_inactive = on ? 1 : 0; }

int watch_test_state(Tab* t, int* has_candidate, int* bar_kind)
{
  WTab* w = t ? wt_find(t->id) : 0;
  if (has_candidate) *has_candidate = w ? w->has_cand : 0;
  if (bar_kind) *bar_kind = w ? w->bar_kind : 0;
  return w ? 1 : 0;
}

u32 watch_test_folder_count()
{
  u32 i, n = 0;
  if (g_folders_dirty) rebuild_folders();
  for (i = 0; i < g_nfolders; i++) if (!g_folders[i].failed && g_folders[i].h) n++;
  return n;
}

// =================================================================================================================
// selftest (st_watch). The fixture files get an old mtime first, so every rewrite is a change even on FAT (2 s
// resolution). Scenarios: silent reload of an unmodified tab (size change and same-size change), the bar on a
// modified tab (Keep my version, Reload + Undo), delete -> missing bar + always-modified, recreate -> changed, own
// saves never trigger, queueing while inactive, change notifications on the fixture folder, a locked file (quiet
// reload retries, open asks Retry/Cancel), the read-only bar's Edit anyway. If <cfg_dir>\st_watch_ext holds a path,
// the test opens that file and waits up to 60 s for an outside edit (e.g. from the Mac side of M:) and logs the delay.

static void st_set_mtime(const char* path, int year)
{
  HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
  SYSTEMTIME s;
  FILETIME ft;
  if (h == INVALID_HANDLE_VALUE) return;
  memset(&s, 0, sizeof(s));
  s.wYear = (WORD)year; s.wMonth = 1; s.wDay = 1; s.wHour = 12;
  SystemTimeToFileTime(&s, &ft);
  SetFileTime(h, 0, 0, &ft);
  CloseHandle(h);
}

static void st_set_old_mtime(const char* path) { st_set_mtime(path, 2001); }

static int st_doc_is(Document* d, const char* s, u32 n)
{
  u8 buf[256];
  if (d->length() != n || n > sizeof(buf)) return 0;
  if (n && d->read(0, buf, n) != E_OK) return 0;
  return n == 0 || memcmp(buf, s, n) == 0;
}

// Polls like the timer would until the tab acted (no candidate left) or timeout; returns the elapsed ms.
static u32 st_poll_until_settled(StCtx* c, Tab* t, u32 timeout_ms)
{
  u32 start = GetTickCount();
  u32 id = t->id;
  watch_poll(0);
  for (;;) {
    int cand = 0;
    Tab* x = docio_tab_by_id(id);
    if (!x) return GetTickCount() - start;
    watch_test_state(x, &cand, 0);
    if (!cand && !x->change_pending && !x->indexing) return GetTickCount() - start;
    if (GetTickCount() - start >= timeout_ms) return 0xFFFFFFFFu;
    st_pump(c, 50);
  }
}

static void st_press(Tab* t, int button_id)
{
  NotifyFn fn = t->notify.fn;
  void* ctx = t->notify.ctx;
  int kind = t->notify.kind;
  notifybar_hide(t, NB_NONE);
  if (fn) fn(t, kind, button_id, ctx);
}

static void st_log_state(Tab* t, const char* what)
{
  FsStat st;
  int cand = 0;
  memset(&st, 0, sizeof(st));
  fs_os()->stat(t->path, &st);
  watch_test_state(t, &cand, 0);
  selftest_log("%s: disk %u/%08x%08x, tab %u/%08x%08x, cand %d pending %d", what, st.size_lo, st.mtime_hi, st.mtime_lo,
               t->disk.size_lo, t->disk.mtime_hi, t->disk.mtime_lo, cand, t->change_pending);
}

static void st_silent_reload(StCtx* c, const char* path)
{
  Tab* t;
  u32 ms;
  st_fixture(c, "w_quiet.txt", "one\r\ntwo\r\nthree\r\n", 16, 0, 0);
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) return;
  st_checkf(c, watch_test_state(t, 0, 0), "opened tab is watched");
  // caret on line 3, column 2
  t->sel.anchor = t->sel.caret = t->doc->line_start(2) + 2;
  // size change
  st_fixture(c, "w_quiet.txt", "one\r\ntwo\r\nthree\r\nfour\r\n", 22, 0, 0);
  ms = st_poll_until_settled(c, t, 5000);
  st_checkf(c, ms != 0xFFFFFFFFu && ms >= 900, "size change acted on after the stability check (%u ms)", ms);
  st_checkf(c, st_doc_is(t->doc, "one\r\ntwo\r\nthree\r\nfour\r\n", 22) && !t->doc->modified(),
            "unmodified tab reloaded quietly");
  st_checkf(c, t->notify.kind != NB_CHANGED, "no bar for a quiet reload");
  st_checkf(c, t->doc->line_of(t->sel.caret) == 2 && t->sel.caret == t->doc->line_start(2) + 2, "caret line/column kept");
  st_checkf(c, t->doc->can_undo(), "quiet reload of a small file is undoable");
  // same-size change. Win98 does not always give a rewritten file a new mtime when it was rewritten soon after a
  // SetFileTime (seen on C:), so the test stamps the new version itself, as a writer's clock would.
  st_fixture(c, "w_quiet.txt", "ONE\r\nTWO\r\nTHREE\r\nFOUR\r\n", 22, 0, 0);
  st_set_mtime(path, 2003);
  st_log_state(t, "same-size rewrite");
  ms = st_poll_until_settled(c, t, 5000);
  st_checkf(c, ms != 0xFFFFFFFFu && st_doc_is(t->doc, "ONE\r\nTWO\r\nTHREE\r\nFOUR\r\n", 22), "same-size change reloaded");
  // own save: no reload, no bar
  t->doc->replace(0, 0, (const u8*)"#", 1);
  st_checkf(c, docio_save_wait(t, 0, 0, 0) == E_OK, "own save");
  ms = st_poll_until_settled(c, t, 3000);
  {
    int cand = 1;
    watch_poll(0);
    watch_test_state(t, &cand, 0);
    st_checkf(c, !cand && t->notify.kind == NB_NONE && !t->doc->modified() && t->doc->length() == 23,
              "own save never triggers the watcher");
  }
  docio_close(t, 0);
}

static void st_modified_bar(StCtx* c, const char* path)
{
  Tab* t;
  u32 ms;
  Selection s;
  st_fixture(c, "w_mod.txt", "base\r\n", 6, 0, 0);
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) return;
  t->doc->replace(0, 0, (const u8*)"mine ", 5);
  st_fixture(c, "w_mod.txt", "theirs\r\n", 8, 0, 0);
  ms = st_poll_until_settled(c, t, 5000);
  st_checkf(c, ms != 0xFFFFFFFFu && t->notify.kind == NB_CHANGED && t->notify.nbuttons == 2,
            "modified tab: bar with Reload / Keep my version");
  st_checkf(c, st_doc_is(t->doc, "mine base\r\n", 11) && t->doc->modified(), "modified tab untouched by the change");
  // the bar is not shown again for the same state
  watch_poll(0);
  st_pump(c, 1200);
  watch_poll(0);
  st_checkf(c, t->notify.kind == NB_CHANGED, "bar stays (shown once)");
  // Keep my version: no more questions for this state; the tab stays modified
  st_press(t, W_BTN_KEEP);
  st_poll_until_settled(c, t, 3000);
  st_checkf(c, t->notify.kind == NB_NONE && t->doc->modified() && t->disk.size_lo == 8, "Keep my version records the disk state");
  // next change: bar again; Reload replaces the text, Undo brings the user's version back
  st_fixture(c, "w_mod.txt", "theirs 2\r\n", 10, 0, 0);
  st_poll_until_settled(c, t, 5000);
  st_checkf(c, t->notify.kind == NB_CHANGED, "next change: bar again");
  st_press(t, W_BTN_RELOAD);
  st_checkf(c, st_doc_is(t->doc, "theirs 2\r\n", 10) && !t->doc->modified(), "Reload loads the disk version");
  st_checkf(c, t->doc->undo(&s) == E_OK && st_doc_is(t->doc, "mine base\r\n", 11), "Undo brings my version back");
  // reload_silent off: an unmodified tab also gets the bar
  t->doc->redo(&s);
  g_app.cfg.reload_silent = 0;
  st_fixture(c, "w_mod.txt", "theirs 3\r\n", 10, 0, 0);
  st_set_old_mtime(path);
  st_poll_until_settled(c, t, 5000);
  st_checkf(c, t->notify.kind == NB_CHANGED && st_doc_is(t->doc, "theirs 2\r\n", 10), "reload_silent off: bar");
  st_press(t, 0);                                       // [x] = keep
  st_checkf(c, t->doc->modified(), "kept version counts as modified");
  g_app.cfg.reload_silent = 1;
  docio_close(t, 0);
}

static void st_missing(StCtx* c, const char* path)
{
  Tab* t;
  u32 ms;
  st_fixture(c, "w_del.txt", "keep me\r\n", 9, 0, 0);
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) return;
  fs_os()->remove(path);
  ms = st_poll_until_settled(c, t, 5000);
  st_checkf(c, ms != 0xFFFFFFFFu && t->disk_missing && t->notify.kind == NB_MISSING && t->notify.nbuttons == 2,
            "deleted file: missing bar with Save / Close tab");
  st_checkf(c, t->doc->modified() && st_doc_is(t->doc, "keep me\r\n", 9), "missing: document kept, always-modified");
  watch_poll(0);
  st_pump(c, 1200);
  watch_poll(0);
  st_checkf(c, t->notify.kind == NB_MISSING && t->disk_missing, "missing bar stays on later checks");
  // recreate: treated as changed (the document had no edits: quiet reload, modified flag cleared again)
  st_fixture(c, "w_del.txt", "back again\r\n", 12, 0, 0);
  ms = st_poll_until_settled(c, t, 5000);
  st_checkf(c, ms != 0xFFFFFFFFu && !t->disk_missing && t->notify.kind != NB_MISSING, "file back: missing state cleared");
  st_checkf(c, st_doc_is(t->doc, "back again\r\n", 12) && !t->doc->modified(), "file back: reloaded");
  // delete again and press Save: the file is written back
  fs_os()->remove(path);
  st_poll_until_settled(c, t, 5000);
  st_checkf(c, t->notify.kind == NB_MISSING, "deleted again: bar");
  st_press(t, W_BTN_SAVE);
  docio_wait_save(t);
  st_pump(c, 50);
  st_checkf(c, ui_file_exists(path) && !t->doc->modified() && !t->disk_missing && t->notify.kind == NB_NONE,
            "Save on the missing bar writes the file back");
  // delete + Close tab (no edits: no question)
  fs_os()->remove(path);
  st_poll_until_settled(c, t, 5000);
  {
    u32 id = t->id;
    st_press(t, W_BTN_CLOSE);
    st_pump(c, 30);
    st_checkf(c, docio_tab_by_id(id) == 0 && ui_unexpected_questions() == 0, "Close tab closes without a question");
  }
}

static void st_inactive_queue(StCtx* c, const char* path)
{
  Tab* t;
  u32 ms;
  st_fixture(c, "w_q.txt", "a\r\n", 3, 0, 0);
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) return;
  watch_test_set_inactive(1);
  st_fixture(c, "w_q.txt", "bb\r\n", 4, 0, 0);
  watch_poll(0);
  st_pump(c, 1300);
  watch_poll(0);
  st_checkf(c, t->change_pending && st_doc_is(t->doc, "a\r\n", 3), "inactive: change queued, nothing done");
  watch_test_set_inactive(0);
  ms = st_poll_until_settled(c, t, 3000);
  st_checkf(c, ms != 0xFFFFFFFFu && ms < 1500 && st_doc_is(t->doc, "bb\r\n", 4) && !t->change_pending,
            "active again: queued change acted on (%u ms)", ms);
  docio_close(t, 0);
}

static void st_notifications(StCtx* c, const char* path)
{
  Tab* t;
  HANDLE h[16];
  u32 n, start;
  DWORD r;
  int fired = 0, cand = 0;
  st_fixture(c, "w_n.txt", "n\r\n", 3, 0, 0);
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) return;
  n = watch_handles(h, 16);
  st_checkf(c, n >= 1 && watch_test_folder_count() >= 1, "change notification for the fixture folder (%u)", n);
  if (n) {
    // drain old signals, then change the file and wait for the folder's signal
    start = GetTickCount();
    while (GetTickCount() - start < 500) {
      r = WaitForMultipleObjects(n, h, FALSE, 50);
      if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) watch_signalled(r - WAIT_OBJECT_0);
      n = watch_handles(h, 16);
    }
    watch_poll(0);
    st_pump(c, 1200);
    st_poll_until_settled(c, t, 2000);
    st_fixture(c, "w_n.txt", "nn\r\n", 4, 0, 0);
    start = GetTickCount();
    while (GetTickCount() - start < 3000 && !fired) {
      r = WaitForMultipleObjects(n, h, FALSE, 100);
      if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) { watch_signalled(r - WAIT_OBJECT_0); fired = 1; }
    }
    watch_test_state(t, &cand, 0);
    st_checkf(c, fired && cand, "notification fired and the tab was checked at once (candidate recorded)");
    st_poll_until_settled(c, t, 5000);
    st_checkf(c, st_doc_is(t->doc, "nn\r\n", 4), "notified change reloaded");
  }
  docio_close(t, 0);
  st_pump(c, 20);
  watch_handles(h, 16);
  st_checkf(c, watch_test_folder_count() == 0, "no folder watched without tabs");
}

static void st_locked(StCtx* c, const char* path)
{
  Tab* t;
  HANDLE lk;
  u32 ms;
  st_fixture(c, "w_lock.txt", "lock\r\n", 6, 0, 0);
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) return;
  // a writer changes the file and keeps it open exclusively: the quiet reload fails silently and retries
  st_fixture(c, "w_lock.txt", "locked\r\n", 8, 0, 0);
  lk = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
  st_checkf(c, lk != INVALID_HANDLE_VALUE, "exclusive handle on the fixture");
  watch_poll(0);
  st_pump(c, 1300);
  watch_poll(0);
  st_pump(c, 300);
  st_checkf(c, st_doc_is(t->doc, "lock\r\n", 6) && ui_unexpected_questions() == 0 && t->notify.kind == NB_NONE,
            "locked: no popup, no bar, document unchanged");
  if (lk != INVALID_HANDLE_VALUE) CloseHandle(lk);
  ms = st_poll_until_settled(c, t, 6000);
  st_checkf(c, ms != 0xFFFFFFFFu && st_doc_is(t->doc, "locked\r\n", 8), "unlocked: reloaded on a later check (%u ms)", ms);
  docio_close(t, 0);
  // opening a locked file asks Retry / Cancel
  lk = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
  if (lk != INVALID_HANDLE_VALUE) {
    int rc;
    st_answer(c, Q_LOCKED_RETRY, IDRETRY);
    st_answer(c, Q_LOCKED_RETRY, IDCANCEL);
    rc = docio_open(path, 0, &t);
    st_checkf(c, rc == E_CANCEL && ui_answer_pending() == 0, "locked open: Retry asked again, Cancel gives up (%s)",
              e_errname(rc));
    CloseHandle(lk);
    st_answer(c, Q_LOCKED_RETRY, IDCANCEL);
    rc = docio_open(path, 0, &t);
    st_checkf(c, rc == E_OK && t && ui_answer_pending() == 1, "unlocked open: no question");
    ui_answer_clear();
    if (t) docio_close(t, 0);
  }
}

static void st_readonly_bar(StCtx* c, const char* path)
{
  Tab* t;
  FsStat st;
  st_fixture(c, "w_ro.txt", "ro\r\n", 4, 0, 0);
  fs_os()->stat(path, &st);
  fs_os()->set_attrs(path, st.attrs | FSA_READONLY);
  t = st_open(c, path);
  if (t) {
    st_checkf(c, t->doc->read_only() && t->notify.kind == NB_READONLY, "read-only file: bar");
    st_press(t, 1);
    st_checkf(c, !t->doc->read_only() && t->doc->replace(0, 0, (const u8*)"x", 1) == E_OK, "Edit anyway: editable");
    docio_close(t, 0);
  }
  fs_os()->stat(path, &st);
  fs_os()->set_attrs(path, st.attrs & ~(u32)FSA_READONLY);
}

static void st_external(StCtx* c)
{
  char marker[300], path[300];
  EBytes b;
  Tab* t;
  u32 i, start, n0;
  ui_path_join(c->cfg_dir, "st_watch_ext", marker, sizeof(marker));
  if (st_file_bytes(c, marker, &b) != E_OK || b.size() == 0) return;
  for (i = 0; i < b.size() && i < sizeof(path) - 1 && b[i] != '\r' && b[i] != '\n'; i++) path[i] = (char)b[i];
  path[i] = 0;
  t = st_open(c, path);
  if (!t) return;
  n0 = t->doc->length();
  selftest_log("watch: waiting up to 60 s for an outside change of %s (%u bytes)", path, n0);
  es_cat(path, sizeof(path), ".wait");                 // closed at once, so an outside script sees it: edit now
  {
    FsFile f;
    if (fs_os()->open(path, FS_WRITE, &f) == E_OK) { fs_os()->write(f, "w", 1); fs_os()->close(f); }
  }
  start = GetTickCount();
  while (GetTickCount() - start < 60000 && t->doc->length() == n0 && t->notify.kind == NB_NONE) st_pump(c, 50);
  st_checkf(c, t->doc->length() != n0 || t->notify.kind != NB_NONE, "outside change seen");
  selftest_log("watch: outside change acted on %u ms after %s was written (%u bytes now)", GetTickCount() - start,
               path, t->doc->length());
  fs_os()->remove(path);
  docio_close(t, 0);
}

// T13: a reload while the tab's backup is being written must wait for the I/O job (reset_source frees the bytes the
// backup snapshot reads). A 3 MB modified document gets a real IOJ_BACKUP job over a SnapshotSource (sessmgr itself
// is off outside st_session), then the file changes and is reloaded at once.
static void st_reload_during_backup(StCtx* c, const char* path)
{
  Tab* t;
  u8* big;
  u32 i, n = 3u * 1024u * 1024u, id = 0;
  int rc, pending_before = 0;
  char bkdir[300];
  big = (u8*)e_valloc(n);
  if (!big) { ST_CHECK(c, 0, "3 MB buffer"); return; }
  for (i = 0; i < n; i++) big[i] = (u8)((i % 71) == 70 ? '\n' : 'a' + (i % 23));
  rc = st_fixture(c, "w_bk.txt", big, n, 0, 0);
  if (rc != E_OK) { e_vfree(big, n); return; }
  st_set_old_mtime(path);
  t = st_open(c, path);
  if (!t) { e_vfree(big, n); return; }
  t->doc->replace(10, 0, (const u8*)"EDIT", 4);
  ui_path_join(c->fixdir, "bk_t14", bkdir, sizeof(bkdir));
  {
    SnapshotSource* snap = new SnapshotSource();
    IoJob* job = new IoJob;
    if (snap && job && t->doc->snapshot(snap, 0) == E_OK) {
      memset(job, 0, sizeof(*job));
      job->kind = IOJ_BACKUP;
      job->tabid = t->id;
      es_copy(job->path, sizeof(job->path), bkdir);
      job->data = snap;
      job->meta.tabid = t->id;
      job->meta.gen = 1;
      job->meta.seq = 1;
      es_copy(job->meta.path, sizeof(job->meta.path), path);
      job->meta.enc = t->doc->enc();
      job->seq = t->doc->change_seq();
      id = iothread_submit(job);                     // owns job and snap
      pending_before = id && iothread_job_pending(id);
    } else {
      delete snap;
      delete job;
    }
  }
  st_checkf(c, id != 0, "backup job submitted");
  big[0] = 'Z';
  st_fixture(c, "w_bk.txt", big, n, 0, 0);
  rc = docio_reload_ex(t, 0, 0, RL_QUIET);           // not undoable: reset_source frees the snapshot's bytes
  st_checkf(c, rc == E_OK && !t->doc->modified() && t->doc->length() == n, "reload while the backup %s (%s)",
            pending_before ? "was running" : "had already finished", e_errname(rc));
  st_checkf(c, !id || !iothread_job_pending(id), "the backup job finished before the reload");
  {
    u8 b0 = 0;
    t->doc->read(0, &b0, 1);
    st_checkf(c, b0 == 'Z', "reloaded bytes are the new file");
  }
  selftest_log("reload during backup: job %s when the reload started", pending_before ? "pending" : "done");
  st_pump(c, 100);
  docio_close(t, 0);
  {
    FsFind f;
    char fp[300];
    while (fs_os()->find_first(bkdir, &f) == E_OK) {
      ui_path_join(bkdir, f.name, fp, sizeof(fp));
      fs_os()->find_close(&f);
      if (fs_os()->remove(fp) != E_OK) break;
    }
    RemoveDirectoryA(bkdir);
  }
  e_vfree(big, n);
}

void st_watch(StCtx* c)
{
  char p[300];
  int i;
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  ui_answer_clear();
  ui_path_join(c->fixdir, "w_quiet.txt", p, sizeof(p)); st_silent_reload(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_mod.txt", p, sizeof(p)); st_modified_bar(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_del.txt", p, sizeof(p)); st_missing(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_q.txt", p, sizeof(p)); st_inactive_queue(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_n.txt", p, sizeof(p)); st_notifications(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_lock.txt", p, sizeof(p)); st_locked(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_ro.txt", p, sizeof(p)); st_readonly_bar(c, p); fs_os()->remove(p);
  ui_path_join(c->fixdir, "w_bk.txt", p, sizeof(p)); st_reload_during_backup(c, p); fs_os()->remove(p);
  st_external(c);
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  watch_test_set_inactive(0);
  st_pump(c, 20);
}
