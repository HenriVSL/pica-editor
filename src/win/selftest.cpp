// selftest.cpp - pica.exe -selftest framework and helpers (DESIGN 17.3): framework, log, helpers (st_keys with a faked
// keyboard state through the real message loop, st_keys_modal for modal dialogs, st_wait_idle), the framework
// self-check (st_framework) and the cross-module scenarios with the accelerator and AltGr audits (st_scenarios).
// Owner after Wave 0: T15.
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "selftest.h"
#include "codec.h"
#include "fsys.h"
#include "estr.h"
#include "resource.h"
#include "ids_find.h"
#include "views.h"
#include "fifworker.h"
#include "iothread.h"
#include "clip.h"

static StCtx* g_ctx = 0;
static char g_log_path[260];
static char g_fixdir[260];

void selftest_set_paths(const char* log_path, const char* fixdir)
{
  es_copy(g_log_path, sizeof(g_log_path), log_path ? log_path : "");
  es_copy(g_fixdir, sizeof(g_fixdir), fixdir ? fixdir : "");
}

int selftest_active()
{
  return g_ctx != 0;
}

void selftest_log(const char* fmt, ...)
{
  char buf[1200];
  va_list ap;
  if (!g_ctx || !g_ctx->log) return;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(g_ctx->log, "  %s\n", buf);
  fflush(g_ctx->log);
}

static void log_line(StCtx* c, const char* fmt, ...)
{
  char buf[1200];
  va_list ap;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (c->log) { fprintf(c->log, "%s\n", buf); fflush(c->log); }
}

// ---- checks ----

void st_skip(StCtx* c, const char* why)
{
  c->skipped = 1;
  log_line(c, "SKIP st_%s (%s)", c->current ? c->current : "?", why ? why : "");
}

void st_check(StCtx* c, int ok, const char* file, int line, const char* expr, const char* msg)
{
  const char* f = file;
  const char* p;
  c->checks++;
  if (ok) return;
  c->failed++;
  for (p = file; p && *p; p++) if (*p == '\\' || *p == '/') f = p + 1;
  log_line(c, "  check failed: %s:%d: %s%s%s", f ? f : "?", line, expr ? expr : "", msg && msg[0] ? " - " : "",
           msg ? msg : "");
}

void st_checkf(StCtx* c, int ok, const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  c->checks++;
  if (ok) return;
  c->failed++;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_line(c, "  check failed: %s", buf);
}

// ---- helpers ----

void st_pump(StCtx* c, u32 ms)
{
  MSG m;
  u32 start = GetTickCount();
  (void)c;
  for (;;) {
    int any = 0;
    while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) {
      any = 1;
      if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); return; }
      app_dispatch(&m);
    }
    if (GetTickCount() - start >= ms) return;
    if (!any && !app_idle_work()) MsgWaitForMultipleObjects(0, 0, FALSE, 10, QS_ALLINPUT);
  }
}

// Idle = no pending idle work (highlighter, frame, backups), no I/O job queued or running, no find in files, no tab
// saving or indexing.
int st_wait_idle(StCtx* c, u32 timeout_ms)
{
  u32 start = GetTickCount();
  for (;;) {
    int busy, i;
    st_pump(c, 0);
    busy = app_idle_work();
    if (fif_running()) busy = 1;
    if (!iothread_flush(0)) busy = 1;
    for (i = 0; i < app_tab_count(); i++) {
      Tab* t = app_tab(i);
      if (t && (t->saving || t->indexing || t->io_pending)) busy = 1;
    }
    if (!busy) return 1;
    if (GetTickCount() - start >= timeout_ms) return 0;
    MsgWaitForMultipleObjects(0, 0, FALSE, 10, QS_ALLINPUT);
  }
}

void st_cmd(StCtx* c, int id)
{
  app_command(id);
  st_pump(c, 0);
}

void st_answer(StCtx* c, int qid, int button)
{
  (void)c;
  ui_answer_push(qid, button);
}

int st_fixture(StCtx* c, const char* name, const void* bytes, u32 n, char* path_out, u32 cap)
{
  FileSys* fs = fs_os();
  FsFile f;
  char path[300];
  int rc;
  ui_path_join(c->fixdir, name, path, sizeof(path));
  if (path_out) es_copy(path_out, cap, path);
  rc = fs->open(path, FS_WRITE, &f);
  if (rc != E_OK) { st_checkf(c, 0, "st_fixture: open %s: %s", path, e_errname(rc)); return rc; }
  rc = n ? fs->write(f, bytes, n) : E_OK;
  if (rc == E_OK) rc = fs->flush(f);
  fs->close(f);
  if (rc != E_OK) st_checkf(c, 0, "st_fixture: write %s: %s", path, e_errname(rc));
  return rc;
}

int st_file_bytes(StCtx* c, const char* path, EBytes* out)
{
  FileSys* fs = fs_os();
  FsFile f;
  u32 lo = 0, hi = 0, got = 0;
  int rc;
  (void)c;
  out->clear();
  rc = fs->open(path, FS_READ | FS_SHARE_ALL, &f);
  if (rc != E_OK) return rc;
  rc = fs->size(f, &lo, &hi);
  if (rc == E_OK && hi) rc = E_TOOBIG;
  if (rc == E_OK) rc = out->resize(lo);
  if (rc == E_OK && lo) rc = fs->read_at(f, 0, 0, out->data(), lo, &got);
  if (rc == E_OK && got != lo) rc = E_IO;
  fs->close(f);
  return rc;
}

Tab* st_open(StCtx* c, const char* path)
{
  Tab* t = 0;
  int rc = docio_open(path, 0, &t);
  if (rc < 0) { st_checkf(c, 0, "st_open %s: %s", path, e_errname(rc)); return 0; }
  st_pump(c, 50);
  return t;
}

// st_keys syntax: "^" Ctrl, "+" Shift, "%" Alt apply to the next key; "{NAME}" named keys ({PLUS} {CARET} {PERCENT}
// {LBRACE} {RBRACE} type those characters; {VKdd} = virtual key 0xdd); other chars are typed with the key VkKeyScan
// gives (shift/ctrl/alt from the layout); a char with no key on the layout is posted as WM_CHAR.
// Messages are posted to the focus window (the active view if the focus is not ours) and handled by the real loop
// (app_dispatch: accelerators, IsDialogMessage, TranslateMessage) while a faked keyboard state is set, so
// TranslateAccelerator, GetKeyState and WM_CHAR generation behave as for a real key. Like Windows, Alt without Ctrl
// gives WM_SYSKEYDOWN; Ctrl+Alt (AltGr) gives WM_KEYDOWN.
struct StKeyName { const char* name; int vk; int ch; };
static const StKeyName k_keys[] = {
  { "ENTER", VK_RETURN, 0 }, { "TAB", VK_TAB, 0 }, { "ESC", VK_ESCAPE, 0 }, { "BS", VK_BACK, 0 },
  { "DEL", VK_DELETE, 0 }, { "INS", VK_INSERT, 0 }, { "HOME", VK_HOME, 0 }, { "END", VK_END, 0 },
  { "PGUP", VK_PRIOR, 0 }, { "PGDN", VK_NEXT, 0 }, { "UP", VK_UP, 0 }, { "DOWN", VK_DOWN, 0 },
  { "LEFT", VK_LEFT, 0 }, { "RIGHT", VK_RIGHT, 0 }, { "SPACE", VK_SPACE, 0 },
  { "F1", VK_F1, 0 }, { "F2", VK_F2, 0 }, { "F3", VK_F3, 0 }, { "F4", VK_F4, 0 }, { "F5", VK_F5, 0 },
  { "F6", VK_F6, 0 }, { "F7", VK_F7, 0 }, { "F8", VK_F8, 0 }, { "F9", VK_F9, 0 }, { "F10", VK_F10, 0 },
  { "F11", VK_F11, 0 }, { "F12", VK_F12, 0 },
  { "PLUS", 0, '+' }, { "CARET", 0, '^' }, { "PERCENT", 0, '%' }, { "LBRACE", 0, '{' }, { "RBRACE", 0, '}' }
};

struct StKey { int vk; u8 ctrl, shift, alt, chr; };   // chr: post WM_CHAR with vk as the byte

static void st_key_char(int ch, StKey* k)
{
  SHORT s = VkKeyScanA((CHAR)ch);
  if (s == -1) { k->vk = ch & 0xFF; k->chr = 1; return; }
  k->vk = s & 0xFF;
  if (s & 0x100) k->shift = 1;
  if (s & 0x200) k->ctrl = 1;
  if (s & 0x400) k->alt = 1;
}

// Returns the number of keys, or -1 (logged) for a syntax error / overflow.
static int st_parse_keys(StCtx* c, const char* keys, StKey* out, u32 max)
{
  const char* p = keys;
  u32 n = 0;
  int ctrl = 0, shift = 0, alt = 0;
  while (p && *p) {
    StKey k;
    if (*p == '^') { ctrl = 1; p++; continue; }
    if (*p == '+') { shift = 1; p++; continue; }
    if (*p == '%') { alt = 1; p++; continue; }
    k.vk = 0; k.ctrl = (u8)ctrl; k.shift = (u8)shift; k.alt = (u8)alt; k.chr = 0;
    if (*p == '{') {
      char name[16];
      u32 m = 0, i;
      int found = 0;
      p++;
      while (*p && *p != '}' && m + 1 < sizeof(name)) name[m++] = *p++;
      name[m] = 0;
      if (*p == '}') p++;
      if (m > 2 && es_nicmp(name, "VK", 2) == 0) {
        u32 v = 0;
        if (es_parse_u32(name + 2, &v, 16) == E_OK && v > 0 && v < 256) { k.vk = (int)v; found = 1; }
      }
      for (i = 0; !found && i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if (es_icmp(name, k_keys[i].name) != 0) continue;
        if (k_keys[i].ch) st_key_char(k_keys[i].ch, &k);    // by name, never by key code (VK_LEFT == '%')
        else k.vk = k_keys[i].vk;
        found = 1;
      }
      if (!found) { st_checkf(c, 0, "st_keys: unknown key {%s} in \"%s\"", name, keys); return -1; }
    } else {
      st_key_char((u8)*p, &k);
      p++;
    }
    if (n >= max) { st_checkf(c, 0, "st_keys: too many keys in \"%s\"", keys); return -1; }
    out[n++] = k;
    ctrl = shift = alt = 0;
  }
  return (int)n;
}

static HWND st_target()
{
  HWND f = GetFocus();
  Tab* t;
  if (f && GetWindowThreadProcessId(f, 0) == GetCurrentThreadId()) return f;
  t = app_active();
  if (t) {
    HWND v = t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
    if (v && IsWindowVisible(v)) { SetFocus(v); return v; }
  }
  return g_app.frame;
}

static int st_is_ext(int vk)
{
  return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END ||
         vk == VK_PRIOR || vk == VK_NEXT || vk == VK_INSERT || vk == VK_DELETE;
}

static void st_set_mods(BYTE* ks, int ctrl, int shift, int alt)
{
  ks[VK_CONTROL] = (BYTE)(ctrl ? 0x80 : 0); ks[VK_LCONTROL] = ks[VK_CONTROL]; ks[VK_RCONTROL] = 0;
  ks[VK_SHIFT] = (BYTE)(shift ? 0x80 : 0);  ks[VK_LSHIFT] = ks[VK_SHIFT];     ks[VK_RSHIFT] = 0;
  ks[VK_MENU] = (BYTE)(alt ? 0x80 : 0);     ks[VK_LMENU] = ks[VK_MENU];       ks[VK_RMENU] = 0;
}

static LPARAM st_key_lparam(const StKey& k, int up)
{
  UINT scan = MapVirtualKeyA((UINT)k.vk, 0);
  LPARAM lp = 1 | ((LPARAM)(scan & 0xFF) << 16);
  if (st_is_ext(k.vk)) lp |= 1L << 24;
  if (k.alt && !k.ctrl) lp |= 1L << 29;
  if (up) lp |= (1L << 30) | (1L << 31);
  return lp;
}

static void st_post_key(HWND target, const StKey& k, int up)
{
  int sys = k.alt && !k.ctrl;
  UINT msg = up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN);
  PostMessageA(target, msg, (WPARAM)k.vk, st_key_lparam(k, up));
}

static void st_send_key(StCtx* c, const StKey& k)
{
  BYTE ks[256], saved[256];
  HWND target = st_target();
  GetKeyboardState(saved);
  memcpy(ks, saved, sizeof(ks));
  st_set_mods(ks, k.ctrl, k.shift, k.alt);
  if (k.chr) {
    SetKeyboardState(ks);
    PostMessageA(target, WM_CHAR, (WPARAM)(k.vk & 0xFF), 1);
    st_pump(c, 0);
    SetKeyboardState(saved);
    return;
  }
  ks[k.vk & 0xFF] = 0x80;
  SetKeyboardState(ks);
  st_post_key(target, k, 0);
  st_pump(c, 0);                       // may run a modal dialog (st_keys_modal feeds it)
  ks[k.vk & 0xFF] = 0;
  SetKeyboardState(ks);
  if (!IsWindow(target)) target = st_target();
  st_post_key(target, k, 1);
  st_pump(c, 0);
  SetKeyboardState(saved);
}

void st_keys(StCtx* c, const char* keys)
{
  StKey k[128];
  int n = st_parse_keys(c, keys, k, sizeof(k) / sizeof(k[0])), i;
  for (i = 0; i < n; i++) st_send_key(c, k[i]);
}

// ---- keys for a modal dialog ----
// A modal dialog (Go To, About, Options...) runs its own message loop inside the key or command that opens it, so the
// keys meant for it are queued first and fed from a thread timer (WM_TIMER is dispatched by the dialog's loop): one
// message per 30 ms tick, only while one of our dialogs is up (never into the main window). If the dialog is still up
// about 1.2 s after the last key, it is cancelled (IDCANCEL) and the check fails, so a broken dialog can't hang the run.

static StKey g_mq[64];
static u32 g_mqn, g_mqi, g_mq_idle;
static int g_mq_up_pending, g_mq_seen, g_mq_forced;
static StKey g_mq_last;
static BYTE g_mq_state[256];
static char g_mq_title[128];

static BOOL CALLBACK mq_find_proc(HWND w, LPARAM lp)
{
  char cls[32];
  if (w == g_app.frame || w == g_app.find_dlg || !IsWindowVisible(w)) return TRUE;
  if (!GetClassNameA(w, cls, sizeof(cls)) || strcmp(cls, "#32770") != 0) return TRUE;
  if (GetWindow(w, GW_OWNER) != g_app.frame) return TRUE;
  *(HWND*)lp = w;
  return FALSE;
}

static HWND mq_dialog()
{
  HWND w = 0;
  EnumThreadWindows(GetCurrentThreadId(), mq_find_proc, (LPARAM)&w);
  return w;
}

static void mq_release(HWND target)
{
  BYTE ks[256];
  if (!g_mq_up_pending) return;
  memcpy(ks, g_mq_state, sizeof(ks));
  st_set_mods(ks, g_mq_last.ctrl, g_mq_last.shift, g_mq_last.alt);
  SetKeyboardState(ks);
  if (target) st_post_key(target, g_mq_last, 1);
  g_mq_up_pending = 0;
}

static VOID CALLBACK mq_tick(HWND hw, UINT msg, UINT id, DWORD now)
{
  HWND dlg = mq_dialog(), target;
  BYTE ks[256];
  (void)hw; (void)msg; (void)id; (void)now;
  if (!dlg) { mq_release(0); return; }
  if (!g_mq_seen) { g_mq_seen = 1; GetWindowTextA(dlg, g_mq_title, sizeof(g_mq_title)); }
  target = GetFocus();
  if (!target || (target != dlg && !IsChild(dlg, target))) target = dlg;
  if (g_mq_up_pending) { mq_release(target); return; }
  if (g_mqi < g_mqn) {
    const StKey& k = g_mq[g_mqi++];
    memcpy(ks, g_mq_state, sizeof(ks));
    st_set_mods(ks, k.ctrl, k.shift, k.alt);
    if (k.chr) { SetKeyboardState(ks); PostMessageA(target, WM_CHAR, (WPARAM)(k.vk & 0xFF), 1); return; }
    ks[k.vk & 0xFF] = 0x80;
    SetKeyboardState(ks);
    st_post_key(target, k, 0);
    g_mq_last = k;
    g_mq_up_pending = 1;
    g_mq_idle = 0;
    return;
  }
  if (++g_mq_idle > 40) {
    g_mq_idle = 0;
    g_mq_forced++;
    PostMessageA(dlg, WM_COMMAND, IDCANCEL, 0);
  }
}

// Opens a modal dialog with keys (open_keys) or a command (cmd, when open_keys is NULL) and types dlg_keys into it.
// Returns 1 if a dialog appeared; its caption goes to title (may be NULL).
static int st_keys_modal(StCtx* c, const char* open_keys, int cmd, const char* dlg_keys, char* title, u32 cap)
{
  int n = st_parse_keys(c, dlg_keys, g_mq, sizeof(g_mq) / sizeof(g_mq[0]));
  UINT timer;
  if (n < 0) return 0;
  g_mqn = (u32)n; g_mqi = 0; g_mq_idle = 0; g_mq_up_pending = 0; g_mq_seen = 0; g_mq_forced = 0;
  g_mq_title[0] = 0;
  GetKeyboardState(g_mq_state);
  st_set_mods(g_mq_state, 0, 0, 0);
  timer = SetTimer(0, 0, 30, (TIMERPROC)mq_tick);
  if (open_keys) st_keys(c, open_keys); else st_cmd(c, cmd);
  if (timer) KillTimer(0, timer);
  mq_release(st_target());
  SetKeyboardState(g_mq_state);
  st_pump(c, 30);
  st_checkf(c, timer != 0, "st_keys_modal: SetTimer failed");
  st_checkf(c, g_mq_seen, "st_keys_modal(%s): no dialog appeared", open_keys ? open_keys : "command");
  st_checkf(c, !g_mq_seen || g_mqi == g_mqn, "st_keys_modal(%s): dialog closed after %u of %u keys of \"%s\"",
            open_keys ? open_keys : "command", g_mqi, g_mqn, dlg_keys);
  st_checkf(c, !g_mq_forced, "st_keys_modal(%s): dialog \"%s\" still open after \"%s\" (cancelled)",
            open_keys ? open_keys : "command", g_mq_title, dlg_keys);
  if (title) es_copy(title, cap, g_mq_title);
  return g_mq_seen;
}

// ---- framework self-check ----

static Vec<int> g_trace_ids;
static void trace_fn(int id) { g_trace_ids.push(id); }

static void collect_menu_ids(HMENU m, Vec<int>* out)
{
  int i, n = GetMenuItemCount(m);
  for (i = 0; i < n; i++) {
    HMENU sub = GetSubMenu(m, i);
    if (sub) { collect_menu_ids(sub, out); continue; }
    {
      UINT id = GetMenuItemID(m, i);
      if (id != 0 && id != 0xFFFFFFFF) out->push((int)id);
    }
  }
}

static void st_framework(StCtx* c)
{
  Vec<int> ids;
  u32 i, j, routed = 0;
  int parts;
  char text[300];
  HMENU tabmenu;
  ACCEL acc[200];
  int nacc;
  EBytes back;
  static const u8 fix[] = { 'P', 'i', 'c', 'a', 0, 0x0D, 0x0A, 0xFF, 0x80, 0x0D };
  char path[300];

  ST_CHECK(c, IsWindow(g_app.frame), "frame window");
  ST_CHECK(c, IsWindowVisible(g_app.frame), "frame visible");
  ST_CHECK(c, g_app.statusbar != 0, "status bar");
  parts = g_app.statusbar ? (int)SendMessageA(g_app.statusbar, SB_GETPARTS, 0, 0) : 0;
  st_checkf(c, parts == SB_PARTS, "status bar parts: %d (want %d)", parts, (int)SB_PARTS);

  // command table: ids unique, ranges don't overlap, every entry has a handler
  for (i = 0; i < cmd_count(); i++) {
    const CmdEntry* a = cmd_at(i);
    st_checkf(c, a->fn || a->fni, "command %d (%s) has no handler", a->id, a->name);
    st_checkf(c, a->id >= 40000 && a->id + a->count - 1 <= 40999, "command %d out of range", a->id);
    for (j = i + 1; j < cmd_count(); j++) {
      const CmdEntry* b = cmd_at(j);
      st_checkf(c, a->id + a->count <= b->id || b->id + b->count <= a->id, "commands %s and %s overlap", a->name, b->name);
    }
  }

  // every menu item (main menu + tab context menu) maps to a command
  collect_menu_ids(GetMenu(g_app.frame), &ids);
  tabmenu = LoadMenuA(g_app.hinst, MAKEINTRESOURCEA(IDR_TABMENU));
  ST_CHECK(c, tabmenu != 0, "IDR_TABMENU loads");
  if (tabmenu) { collect_menu_ids(tabmenu, &ids); DestroyMenu(tabmenu); }
  st_checkf(c, ids.size() >= 80, "menu items: %u (want >= 80)", ids.size());
  for (i = 0; i < ids.size(); i++) st_checkf(c, cmd_find(ids[i], 0) != 0, "menu id %d has no command", ids[i]);

  // accelerators map to commands; AltGr-safe (15.5): no Ctrl+Alt+digit/E/+/<>, no plain Del/Ins
  nacc = g_app.accel ? CopyAcceleratorTableA(g_app.accel, acc, 200) : 0;
  st_checkf(c, nacc >= 40, "accelerators: %d (want >= 40)", nacc);
  for (i = 0; i < (u32)nacc; i++) {
    int k = acc[i].key, f = acc[i].fVirt;
    st_checkf(c, cmd_find(acc[i].cmd, 0) != 0, "accelerator for id %d has no command", (int)acc[i].cmd);
    if ((f & FCONTROL) && (f & FALT))
      st_checkf(c, !((k >= '0' && k <= '9') || k == 'E' || k == 0xBB || k == 0xE2 || k == 0xBF),
                "AltGr-unsafe accelerator Ctrl+Alt+%d", k);
    if (!(f & (FCONTROL | FALT | FSHIFT)))
      st_checkf(c, k != VK_DELETE && k != VK_INSERT, "plain Del/Ins accelerator (%d)", k);
  }

  // routing: every menu and accelerator id reaches app_command's dispatch (trace-only: handlers not called)
  g_trace_ids.clear();
  cmd_set_trace(trace_fn, 1);
  for (i = 0; i < ids.size(); i++) app_command(ids[i]);
  for (i = 0; i < (u32)nacc; i++) app_command(acc[i].cmd);
  cmd_set_trace(0, 0);
  for (i = 0; i < ids.size() + (u32)nacc && i < g_trace_ids.size(); i++) {
    int want = i < ids.size() ? ids[i] : acc[i - ids.size()].cmd;
    if (g_trace_ids[i] == want && cmd_find(want, 0)) routed++;
  }
  st_checkf(c, routed == ids.size() + (u32)nacc, "routed %u of %u ids", routed, ids.size() + (u32)nacc);
  g_trace_ids.release();

  // status notes reach the flags part
  ui_notimpl("selftest");
  text[0] = 0;
  if (g_app.statusbar) SendMessageA(g_app.statusbar, SB_GETTEXTA, SB_PART_FLAGS, (LPARAM)text);
  st_checkf(c, strstr(text, "Not implemented yet") != 0, "status note shown (got \"%s\")", text);

  // ui_ask answer queue
  ui_answer_push(Q_GENERIC, IDYES);
  st_checkf(c, ui_ask(Q_GENERIC, "framework check", MB_YESNO) == IDYES, "ui_ask answered from the queue");
  ST_CHECK(c, ui_answer_pending() == 0, "answer consumed");

  // fixtures round-trip through fs_os()
  if (st_fixture(c, "fw_check.bin", fix, sizeof(fix), path, sizeof(path)) == E_OK) {
    int rc = st_file_bytes(c, path, &back);
    st_checkf(c, rc == E_OK && back.size() == sizeof(fix) && memcmp(back.data(), fix, sizeof(fix)) == 0,
              "fixture round trip (%s, %u bytes)", e_errname(rc), back.size());
    fs_os()->remove(path);
  }

  // core is linked and initialised
  ST_CHECK(c, codec_get(enc_utf8(0)) != 0, "UTF-8 codec");
  es_fmt(text, sizeof(text), "%s-%u", APP_NAME, 42u);
  ST_CHECK(c, strcmp(text, "Pica-42") == 0, "es_fmt");
}

// ---- runner ----

typedef void (*StFn)(StCtx* c);
struct StEntry { const char* name; StFn fn; };
static const StEntry k_tests[] = {
  { "framework", st_framework }, { "frame", st_frame }, { "textview", st_textview }, { "docio", st_docio },
  { "hexview", st_hexview }, { "search", st_search }, { "fif", st_fif }, { "edit", st_edit },
  { "dialogs", st_dialogs }, { "session", st_session }, { "watch", st_watch }, { "largefile", st_largefile },
  { "scenarios", st_scenarios }
};

static int name_selected(const char* names, const char* name)
{
  char tok[64];
  const char* p = names;
  if (!names || !names[0]) return 1;
  while (*p) {
    u32 n = 0;
    while (*p == ' ' || *p == ',') p++;
    while (*p && *p != ' ' && *p != ',' && n + 1 < sizeof(tok)) tok[n++] = *p++;
    tok[n] = 0;
    if (n && (es_icmp(tok, name) == 0 || (es_nicmp(tok, "st_", 3) == 0 && es_icmp(tok + 3, name) == 0))) return 1;
  }
  return 0;
}

int selftest_run(const char* names)
{
  StCtx c;
  char path[300];
  u32 i;
  memset(&c, 0, sizeof(c));
  es_copy(c.cfg_dir, sizeof(c.cfg_dir), g_app.cfg_dir);
  if (g_fixdir[0]) es_copy(c.fixdir, sizeof(c.fixdir), g_fixdir);
  else ui_path_join(g_app.cfg_dir, "fixtures", c.fixdir, sizeof(c.fixdir));
  CreateDirectoryA(c.fixdir, 0);
  if (g_log_path[0]) es_copy(path, sizeof(path), g_log_path);
  else ui_path_join(g_app.cfg_dir, "selftest.log", path, sizeof(path));
  c.log = fopen(path, "w");
  g_ctx = &c;
  log_line(&c, "%s %s selftest (cfg %s, fixtures %s)", APP_NAME, APP_VERSION, c.cfg_dir, c.fixdir);
  st_pump(&c, 200);
  for (i = 0; i < sizeof(k_tests) / sizeof(k_tests[0]); i++) {
    int unexpected;
    if (!name_selected(names, k_tests[i].name)) continue;
    c.current = k_tests[i].name;
    c.checks = c.failed = 0;
    c.skipped = 0;
    ui_answer_clear();
    k_tests[i].fn(&c);
    st_pump(&c, 20);
    unexpected = ui_unexpected_questions();
    if (unexpected) { c.failed++; log_line(&c, "  %d unexpected question(s)", unexpected); }
    if (ui_answer_pending()) log_line(&c, "  note: %u queued answer(s) not used", ui_answer_pending());
    if (c.skipped && c.checks == 0 && c.failed == 0) { c.tests_skipped++; continue; }
    if (c.failed) { c.tests_failed++; log_line(&c, "FAIL st_%s: %u of %u checks failed", c.current, c.failed, c.checks); }
    else { c.tests_passed++; log_line(&c, "PASS st_%s (%u checks)", c.current, c.checks); }
  }
  log_line(&c, "selftest: %u passed, %u failed (%u skipped)", c.tests_passed, c.tests_failed, c.tests_skipped);
  if (c.log) fclose(c.log);
  g_ctx = 0;
  return (int)c.tests_failed;
}

// ---- cross-module scenarios (T15) ----
// Real user flows driven by keys through the message loop (accelerators, views, dialogs); commands without a key
// (Document menu) go through app_command like a menu click. Each scenario uses its own fixture and closes its tabs.

static void sc_esc(const u8* p, u32 n, char* out, u32 cap)
{
  u32 i, o = 0;
  if (!cap) return;
  for (i = 0; i < n && o + 5 < cap; i++) {
    u8 b = p[i];
    if (b == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
    else if (b == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
    else if (b == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
    else if (b >= 0x20 && b < 0x7F) out[o++] = (char)b;
    else { es_fmt(out + o, cap - o, "\\x%02X", (u32)b); o += 4; }
  }
  if (i < n && o + 4 < cap) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
  out[o] = 0;
}

static int sc_doc_bytes(Tab* t, EBytes* out)
{
  epos n = t->doc->length();
  out->clear();
  if (out->resize(n) != E_OK) return E_NOMEM;
  return n ? t->doc->read(0, out->data(), n) : E_OK;
}

// The document holds exactly these bytes.
static int sc_text(StCtx* c, Tab* t, const char* want, u32 n, const char* what)
{
  EBytes b;
  char got[200], exp[200];
  int ok;
  if (!t || !t->doc) { st_checkf(c, 0, "%s: no document", what); return 0; }
  ok = sc_doc_bytes(t, &b) == E_OK && b.size() == n && (n == 0 || memcmp(b.data(), want, n) == 0);
  if (!ok) {
    sc_esc(b.data(), b.size(), got, sizeof(got));
    sc_esc((const u8*)want, n, exp, sizeof(exp));
  } else got[0] = exp[0] = 0;
  st_checkf(c, ok, "%s: document \"%s\", want \"%s\"", what, got, exp);
  return ok;
}
#define SC_TEXT(c, t, s, what) sc_text((c), (t), (s), (u32)(sizeof(s) - 1), (what))

// The file on disk holds exactly these bytes.
static int sc_file(StCtx* c, const char* path, const char* want, u32 n, const char* what)
{
  EBytes b;
  char got[200], exp[200];
  int rc = st_file_bytes(c, path, &b);
  int ok = rc == E_OK && b.size() == n && (n == 0 || memcmp(b.data(), want, n) == 0);
  if (!ok) {
    sc_esc(b.data(), b.size(), got, sizeof(got));
    sc_esc((const u8*)want, n, exp, sizeof(exp));
  } else got[0] = exp[0] = 0;
  st_checkf(c, ok, "%s: file (%s) \"%s\", want \"%s\"", what, e_errname(rc), got, exp);
  return ok;
}
#define SC_FILE(c, p, s, what) sc_file((c), (p), (s), (u32)(sizeof(s) - 1), (what))

static HWND sc_focus(StCtx* c)
{
  Tab* t = app_active();
  HWND v;
  if (!t) return 0;
  v = t->view == VIEW_HEX ? g_app.hexview : g_app.textview;
  if (v) SetFocus(v);
  st_pump(c, 0);
  st_checkf(c, v && GetFocus() == v, "focus in the %s view", t->view == VIEW_HEX ? "hex" : "text");
  return v;
}

static Tab* sc_open(StCtx* c, const char* name, const char* bytes, u32 n, char* path, u32 cap)
{
  Tab* t;
  if (st_fixture(c, name, bytes, n, path, cap) != E_OK) return 0;
  t = st_open(c, path);
  if (!t) return 0;
  st_checkf(c, t == app_active(), "%s: opened tab is active", name);
  st_wait_idle(c, 5000);
  sc_focus(c);
  return t;
}

static void sc_close(StCtx* c, Tab* t)
{
  if (t && app_tab_index(t) >= 0) docio_close(t, 0);
  st_pump(c, 20);
}

static void sc_status(int part, char* out, u32 cap)
{
  char buf[512];
  buf[0] = 0;
  if (g_app.statusbar && LOWORD(SendMessageA(g_app.statusbar, SB_GETTEXTLENGTHA, part, 0)) < sizeof(buf))
    SendMessageA(g_app.statusbar, SB_GETTEXTA, part, (LPARAM)buf);
  es_copy(out, cap, buf);
}

static int sc_title_has(const char* s)
{
  char title[400];
  title[0] = 0;
  GetWindowTextA(g_app.frame, title, sizeof(title));
  return strstr(title, s) != 0;
}

static int sc_undo_all(StCtx* c, Tab* t, int redo)
{
  int i;
  for (i = 0; i < 40 && (redo ? t->doc->can_redo() : t->doc->can_undo()); i++) st_keys(c, redo ? "^y" : "^z");
  return i;
}

// 1. Open, type, undo/redo, save, close, reopen: bytes on disk (ANSI CRLF and UTF-8 BOM LF).
static void sc_save_cycle(StCtx* c)
{
  static const char orig[] = "first line\r\nsecond\r\n";
  static const char three[] = "first line\r\nsecond\r\nthird\r\n";
  static const char want[] = "first line!\r\nsecond\r\nthird\r\n";
  static const char orig8[] = "\xEF\xBB\xBFhello\n";
  static const char want8[] = "\xEF\xBB\xBFhello w\xC3\xA9rld\n";
  char path[300];
  Tab* t = sc_open(c, "sc_save.txt", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  st_keys(c, "^{END}third{ENTER}");
  SC_TEXT(c, t, three, "save: typed a line at the end");
  st_keys(c, "^{HOME}{END}!");
  SC_TEXT(c, t, want, "save: '!' at the end of line 1");
  st_wait_idle(c, 2000);
  ST_CHECK(c, t->doc->modified() && sc_title_has("sc_save.txt *"), "save: modified, '*' in the title");
  st_keys(c, "^z");
  SC_TEXT(c, t, three, "save: Ctrl+Z undoes the '!'");
  st_keys(c, "^y");
  SC_TEXT(c, t, want, "save: Ctrl+Y redoes it");
  st_keys(c, "{BS}");
  SC_TEXT(c, t, three, "save: Backspace");
  st_keys(c, "{LEFT}{DEL}");
  SC_TEXT(c, t, "first lin\r\nsecond\r\nthird\r\n", "save: Left, Del");
  st_keys(c, "e!");
  SC_TEXT(c, t, want, "save: typed again");
  st_keys(c, "^s");
  ST_CHECK(c, st_wait_idle(c, 15000), "save: Ctrl+S finished");
  ST_CHECK(c, !t->doc->modified(), "save: not modified after Ctrl+S");
  SC_FILE(c, path, want, "save: Ctrl+S wrote the bytes");
  st_wait_idle(c, 2000);
  ST_CHECK(c, !sc_title_has("*"), "save: no '*' in the title");
  st_keys(c, "^w");
  ST_CHECK(c, app_tab_index(t) < 0, "save: Ctrl+W closed the saved tab without asking");
  t = st_open(c, path);
  if (t) {
    SC_TEXT(c, t, want, "save: reopened");
    ST_CHECK(c, enc_equal(t->doc->enc(), enc_ansi()) && t->doc->eol_style() == EOL_CRLF && !t->doc->eol_mixed(),
             "save: reopened as ANSI CRLF");
    ST_CHECK(c, !t->doc->modified() && !t->doc->can_undo(), "save: reopened clean, no undo history");
    sc_close(c, t);
  }

  t = sc_open(c, "sc_save8.txt", orig8, sizeof(orig8) - 1, path, sizeof(path));
  if (!t) return;
  ST_CHECK(c, enc_equal(t->doc->enc(), enc_utf8(1)) && t->doc->eol_style() == EOL_LF, "save8: UTF-8 BOM, LF");
  st_keys(c, "^{END}{UP}{END} w\xE9rld");               // e-acute has no US key: posted as WM_CHAR 0xE9 (cp1252)
  SC_TEXT(c, t, want8, "save8: typed e-acute in UTF-8");
  st_keys(c, "^s");
  ST_CHECK(c, st_wait_idle(c, 15000), "save8: Ctrl+S finished");
  SC_FILE(c, path, want8, "save8: Ctrl+S wrote UTF-8 with the BOM");
  st_keys(c, "^a{DEL}");
  SC_TEXT(c, t, "\xEF\xBB\xBF", "save8: Select All + Del keeps the BOM");
  st_keys(c, "^z");
  ST_CHECK(c, !t->doc->modified(), "save8: Ctrl+Z back to the save point");
  SC_TEXT(c, t, want8, "save8: undo restores the saved text");
  sc_close(c, t);
}

// 2. Text/hex switching never changes a byte; caret mapping; hex typing; undo across views.
static void sc_hex_switch(StCtx* c)
{
  static const char orig[] = "A\xC3\xA9" "B\r\nline2\nend";   // UTF-8 without BOM, mixed EOL
  static const char edited[] = "Bz\xA9" "B\r\nline2\nend";
  char path[300];
  int i;
  Tab* t = sc_open(c, "sc_hex.txt", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  ST_CHECK(c, enc_equal(t->doc->enc(), enc_utf8(0)), "hex: detected UTF-8 without BOM");
  ST_CHECK(c, t->view == VIEW_TEXT, "hex: opens in the text view");
  st_keys(c, "{RIGHT}");
  st_checkf(c, t->sel.caret == 1, "hex: caret after 'A' (%u)", t->sel.caret);
  st_keys(c, "^+h");
  ST_CHECK(c, t->view == VIEW_HEX && g_app.hexview && IsWindowVisible(g_app.hexview), "hex: Ctrl+Shift+H shows hex");
  st_checkf(c, t->hv.caret.pos == 1 && t->sel.caret == 1, "hex: same caret byte (%u)", t->hv.caret.pos);
  if (t->hv.caret.pane) st_keys(c, "{TAB}");
  st_keys(c, "{RIGHT}{RIGHT}{RIGHT}");                    // nibble steps: 1.lo, 2.hi, 2.lo
  st_checkf(c, t->hv.caret.pos == 2, "hex: Right x3 = middle of e-acute (%u)", t->hv.caret.pos);
  st_keys(c, "^+h");
  ST_CHECK(c, t->view == VIEW_TEXT, "hex: back to text");
  st_checkf(c, t->sel.caret == 1, "hex: caret snapped to the character start (%u)", t->sel.caret);
  for (i = 0; i < 4; i++) st_keys(c, "^+h");
  ST_CHECK(c, t->view == VIEW_TEXT && t->sel.caret == 1, "hex: four more switches, same place");
  SC_TEXT(c, t, orig, "hex: switching changed no byte");
  ST_CHECK(c, !t->doc->modified() && !t->doc->can_undo(), "hex: not modified, nothing to undo");

  st_keys(c, "^+h");
  sc_focus(c);
  if (t->hv.caret.insert) st_keys(c, "{INS}");
  if (t->hv.caret.pane) st_keys(c, "{TAB}");
  st_keys(c, "^{HOME}42");
  SC_TEXT(c, t, "B\xC3\xA9" "B\r\nline2\nend", "hex: typed 4 2 over 'A' (overwrite)");
  st_checkf(c, t->hv.caret.pos == 1, "hex: caret moved to the next byte (%u)", t->hv.caret.pos);
  st_keys(c, "{TAB}z{TAB}");
  SC_TEXT(c, t, edited, "hex: typed 'z' in the character pane");
  st_keys(c, "^+h");
  ST_CHECK(c, t->view == VIEW_TEXT && t->doc->modified(), "hex: text view, modified");
  sc_undo_all(c, t, 0);
  SC_TEXT(c, t, orig, "hex: Ctrl+Z in the text view undoes the hex edits");
  ST_CHECK(c, !t->doc->modified(), "hex: back at the saved state");
  sc_undo_all(c, t, 1);
  SC_TEXT(c, t, edited, "hex: Ctrl+Y redoes them");
  sc_undo_all(c, t, 0);
  sc_close(c, t);
}

// 3. Find / Replace dialog by keys, Replace All + undo, F3 / Shift+F3, regex mode with a live error.
static void sc_find_replace(StCtx* c)
{
  static const char orig[] = "cat dog cat bird cat\r\n";
  static const char foxed[] = "fox dog fox bird fox\r\n";
  char path[300], status[300];
  HWND f, repl;
  Tab* t = sc_open(c, "sc_find.txt", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  st_keys(c, "^h");
  ST_CHECK(c, g_app.find_dlg && IsWindowVisible(g_app.find_dlg), "find: Ctrl+H opens the dialog");
  if (!g_app.find_dlg || !IsWindowVisible(g_app.find_dlg)) { sc_close(c, t); return; }
  f = GetFocus();
  ST_CHECK(c, f && IsChild(g_app.find_dlg, f), "find: focus in the dialog");
  st_keys(c, "cat%l");
  repl = GetDlgItem(g_app.find_dlg, IDC_FIND_REPL);
  f = GetFocus();
  ST_CHECK(c, f && repl && (f == repl || IsChild(repl, f)), "find: Alt+L moves to Replace with");
  st_keys(c, "fox%a");
  SC_TEXT(c, t, foxed, "find: Alt+A = Replace All");
  GetDlgItemTextA(g_app.find_dlg, IDC_FIND_STATUS, status, sizeof(status));
  st_checkf(c, strstr(status, "3") != 0, "find: status counts 3 (\"%s\")", status);
  st_keys(c, "{ESC}");
  ST_CHECK(c, !IsWindowVisible(g_app.find_dlg), "find: Esc closes the dialog");
  sc_focus(c);
  st_keys(c, "^z");
  SC_TEXT(c, t, orig, "find: one Ctrl+Z undoes Replace All");
  st_keys(c, "^y");
  SC_TEXT(c, t, foxed, "find: Ctrl+Y redoes it");
  st_keys(c, "^z^{HOME}{F3}");
  st_checkf(c, sel_lo(t->sel) == 0 && sel_hi(t->sel) == 3, "find: F3 finds 'cat' at 0 (%u-%u)", sel_lo(t->sel),
            sel_hi(t->sel));
  st_keys(c, "{F3}");
  st_checkf(c, sel_lo(t->sel) == 8 && sel_hi(t->sel) == 11, "find: F3 again at 8 (%u-%u)", sel_lo(t->sel),
            sel_hi(t->sel));
  st_keys(c, "+{F3}");
  st_checkf(c, sel_lo(t->sel) == 0, "find: Shift+F3 back to 0 (%u)", sel_lo(t->sel));
  // regex: Alt+G picks Regex, Alt+N goes back to the field (text selected), a bad pattern shows a live error
  st_keys(c, "^{HOME}^f%g%n(ab");
  st_pump(c, 400);                                          // live validation (200 ms debounce)
  GetDlgItemTextA(g_app.find_dlg, IDC_FIND_STATUS, status, sizeof(status));
  st_checkf(c, strstr(status, "error") != 0, "find: live regex error (\"%s\")", status);
  st_keys(c, "%nb\\w{PLUS}d{ENTER}");
  st_checkf(c, sel_lo(t->sel) == 12 && sel_hi(t->sel) == 16, "find: regex b\\w+d finds 'bird' (%u-%u)",
            sel_lo(t->sel), sel_hi(t->sel));
  st_keys(c, "%m{ESC}");                                    // back to Normal mode for later runs
  ST_CHECK(c, !IsWindowVisible(g_app.find_dlg), "find: closed again");
  SC_TEXT(c, t, orig, "find: searching changed nothing");
  sc_close(c, t);
}

// 4. Convert encodings and line endings (Document menu), save each, undo back to the original bytes; lossy convert.
static void sc_encoding(StCtx* c)
{
  static const char orig[] = "caf\xE9\r\nna\xEFve\r\n";
  static const char utf8_crlf[] = "caf\xC3\xA9\r\nna\xC3\xAFve\r\n";
  static const char utf8_lf[] = "caf\xC3\xA9\nna\xC3\xAFve\n";
  static const char utf16[] = "\xFF\xFE" "c\0a\0f\0\xE9\0\r\0\n\0n\0a\0\xEF\0v\0e\0\r\0\n\0";
  static const char loss[] = "\xD0\x96x\r\n";
  char path[300], sb[200];
  Tab* t = sc_open(c, "sc_enc.txt", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  ST_CHECK(c, enc_equal(t->doc->enc(), enc_ansi()) && t->doc->eol_style() == EOL_CRLF, "enc: opened as ANSI CRLF");
  st_cmd(c, IDM_ENC_CONV_UTF8);
  SC_TEXT(c, t, utf8_crlf, "enc: converted to UTF-8");
  ST_CHECK(c, enc_equal(t->doc->enc(), enc_utf8(0)) && t->doc->modified(), "enc: UTF-8, modified");
  st_wait_idle(c, 2000);
  sc_status(SB_PART_ENC, sb, sizeof(sb));
  st_checkf(c, strstr(sb, "UTF-8") != 0, "enc: status bar says UTF-8 (\"%s\")", sb);
  sc_focus(c);
  st_keys(c, "^s");
  ST_CHECK(c, st_wait_idle(c, 15000), "enc: saved");
  SC_FILE(c, path, utf8_crlf, "enc: UTF-8 on disk");
  st_cmd(c, IDM_EOL_LF);
  SC_TEXT(c, t, utf8_lf, "enc: line endings LF");
  ST_CHECK(c, t->doc->eol_style() == EOL_LF, "enc: style LF");
  st_wait_idle(c, 2000);
  sc_status(SB_PART_EOL, sb, sizeof(sb));
  st_checkf(c, strstr(sb, "LF") != 0 && strstr(sb, "CRLF") == 0, "enc: status bar says LF (\"%s\")", sb);
  sc_focus(c);
  st_keys(c, "^s");
  ST_CHECK(c, st_wait_idle(c, 15000), "enc: saved LF");
  SC_FILE(c, path, utf8_lf, "enc: UTF-8 LF on disk");
  st_keys(c, "^z");
  SC_TEXT(c, t, utf8_crlf, "enc: Ctrl+Z undoes the EOL conversion");
  st_keys(c, "^z");
  SC_TEXT(c, t, orig, "enc: Ctrl+Z undoes the encoding conversion");
  ST_CHECK(c, enc_equal(t->doc->enc(), enc_ansi()) && t->doc->eol_style() == EOL_CRLF, "enc: ANSI CRLF again");
  st_keys(c, "^s");
  ST_CHECK(c, st_wait_idle(c, 15000), "enc: saved the original");
  SC_FILE(c, path, orig, "enc: original bytes on disk");
  st_cmd(c, IDM_ENC_CONV_UTF16LE);
  SC_TEXT(c, t, utf16, "enc: UTF-16 LE with BOM");
  ST_CHECK(c, t->doc->text_origin() == 2, "enc: BOM is not text");
  sc_focus(c);
  st_keys(c, "^z");
  SC_TEXT(c, t, orig, "enc: undo UTF-16");
  ST_CHECK(c, !t->doc->modified(), "enc: at the save point");
  sc_close(c, t);

  t = sc_open(c, "sc_loss.txt", loss, sizeof(loss) - 1, path, sizeof(path));
  if (!t) return;
  ST_CHECK(c, enc_equal(t->doc->enc(), enc_utf8(0)), "loss: opened as UTF-8");
  st_answer(c, Q_CONVERT_LOSS, IDCANCEL);
  st_cmd(c, IDM_ENC_CONV_ANSI);
  SC_TEXT(c, t, loss, "loss: Cancel converts nothing");
  ST_CHECK(c, !t->doc->modified() && enc_equal(t->doc->enc(), enc_utf8(0)), "loss: still UTF-8, clean");
  st_answer(c, Q_CONVERT_LOSS, IDOK);
  st_cmd(c, IDM_ENC_CONV_ANSI);
  SC_TEXT(c, t, "?x\r\n", "loss: Convert turns the Cyrillic letter into '?'");
  sc_focus(c);
  st_keys(c, "^z");
  SC_TEXT(c, t, loss, "loss: undo");
  sc_close(c, t);
}

// 5. Line commands: duplicate, move, cut line (clipboard), delete line, paste, case, comment.
static void sc_lines(StCtx* c)
{
  static const char orig[] = "alpha\r\nbeta\r\ngamma\r\n";
  char path[300];
  ClipData cd;
  Tab* t = sc_open(c, "sc_lines.c", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  st_keys(c, "^d");
  SC_TEXT(c, t, "alpha\r\nalpha\r\nbeta\r\ngamma\r\n", "lines: Ctrl+D duplicates the line");
  st_checkf(c, t->sel.caret == 0 && sel_empty(t->sel), "lines: caret stays (%u)", t->sel.caret);
  st_keys(c, "{DOWN}^+{DOWN}");
  SC_TEXT(c, t, "alpha\r\nbeta\r\nalpha\r\ngamma\r\n", "lines: Ctrl+Shift+Down moves it down");
  st_checkf(c, sel_lo(t->sel) == 13 && sel_hi(t->sel) == 20, "lines: moved line selected (%u-%u)", sel_lo(t->sel),
            sel_hi(t->sel));
  st_keys(c, "^l");
  SC_TEXT(c, t, "alpha\r\nbeta\r\ngamma\r\n", "lines: Ctrl+L cuts the line");
  st_checkf(c, t->sel.caret == 13 && sel_empty(t->sel), "lines: caret at the next line (%u)", t->sel.caret);
  if (clip_get(&cd) == E_OK) {
    static const ecp want[] = { 'a', 'l', 'p', 'h', 'a', '\r', '\n' };
    st_checkf(c, cd.text.size() == 7 && memcmp(cd.text.data(), want, sizeof(want)) == 0,
              "lines: clipboard has \"alpha\\r\\n\" (%u chars)", cd.text.size());
    clip_free(&cd);
  } else ST_CHECK(c, 0, "lines: clipboard readable after Ctrl+L");
  st_keys(c, "^+l");
  SC_TEXT(c, t, "alpha\r\nbeta\r\n", "lines: Ctrl+Shift+L deletes the line");
  st_keys(c, "^{HOME}^v");
  SC_TEXT(c, t, "alpha\r\nalpha\r\nbeta\r\n", "lines: Ctrl+V pastes the cut line (clipboard kept by Ctrl+Shift+L)");
  st_keys(c, "^{HOME}+{END}^+u");
  SC_TEXT(c, t, "ALPHA\r\nalpha\r\nbeta\r\n", "lines: Ctrl+Shift+U");
  st_checkf(c, sel_lo(t->sel) == 0 && sel_hi(t->sel) == 5, "lines: selection kept (%u-%u)", sel_lo(t->sel),
            sel_hi(t->sel));
  st_keys(c, "^u");
  SC_TEXT(c, t, "alpha\r\nalpha\r\nbeta\r\n", "lines: Ctrl+U");
  st_keys(c, "^q");
  SC_TEXT(c, t, "// alpha\r\nalpha\r\nbeta\r\n", "lines: Ctrl+Q comments the line (C)");
  st_keys(c, "^q");
  SC_TEXT(c, t, "alpha\r\nalpha\r\nbeta\r\n", "lines: Ctrl+Q again removes it");
  st_keys(c, "^{HOME}{DOWN}{DOWN}^+{UP}");
  SC_TEXT(c, t, "alpha\r\nbeta\r\nalpha\r\n", "lines: Ctrl+Shift+Up");
  sc_undo_all(c, t, 0);
  SC_TEXT(c, t, orig, "lines: everything undone");
  ST_CHECK(c, !t->doc->modified(), "lines: at the save point");
  sc_close(c, t);
}

// 6. Bookmarks: toggle, next/previous with wrap, they follow edits, clear.
static void sc_bookmarks(StCtx* c)
{
  static const char orig[] = "L0\r\nL1\r\nL2\r\nL3\r\nL4\r\nL5\r\nL6\r\nL7\r\nL8\r\nL9\r\n";
  char path[300], note[300];
  Tab* t = sc_open(c, "sc_bm.txt", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  st_keys(c, "{DOWN}{DOWN}^{F2}{DOWN}{DOWN}{DOWN}^{F2}");
  ST_CHECK(c, t->bookmarks.count() == 2 && t->bookmarks.has(2) && t->bookmarks.has(5), "bm: Ctrl+F2 on lines 3, 6");
  st_keys(c, "^{HOME}{F2}");
  st_checkf(c, t->doc->line_of(t->sel.caret) == 2, "bm: F2 -> line 3 (%u)", t->doc->line_of(t->sel.caret) + 1);
  st_keys(c, "{F2}");
  st_checkf(c, t->doc->line_of(t->sel.caret) == 5, "bm: F2 -> line 6 (%u)", t->doc->line_of(t->sel.caret) + 1);
  st_keys(c, "{F2}");
  st_checkf(c, t->doc->line_of(t->sel.caret) == 2, "bm: F2 wraps to line 3 (%u)", t->doc->line_of(t->sel.caret) + 1);
  st_keys(c, "+{F2}");
  st_checkf(c, t->doc->line_of(t->sel.caret) == 5, "bm: Shift+F2 wraps to line 6 (%u)",
            t->doc->line_of(t->sel.caret) + 1);
  st_keys(c, "^{HOME}x{ENTER}");
  ST_CHECK(c, t->bookmarks.count() == 2 && t->bookmarks.has(3) && t->bookmarks.has(6), "bm: follow an inserted line");
  sc_undo_all(c, t, 0);
  SC_TEXT(c, t, orig, "bm: undo");
  ST_CHECK(c, t->bookmarks.count() == 2 && t->bookmarks.has(2) && t->bookmarks.has(5), "bm: back after undo");
  st_keys(c, "^{HOME}{F2}{F2}^{F2}");
  ST_CHECK(c, t->bookmarks.count() == 1 && t->bookmarks.has(2), "bm: Ctrl+F2 removes one");
  st_cmd(c, IDM_BM_CLEAR);
  ST_CHECK(c, t->bookmarks.count() == 0, "bm: Clear All");
  sc_focus(c);
  st_keys(c, "^{HOME}{F2}");
  sc_status(SB_PART_FLAGS, note, sizeof(note));
  st_checkf(c, t->sel.caret == 0 && strstr(note, "No bookmarks") != 0, "bm: F2 without bookmarks (\"%s\")", note);
  sc_close(c, t);
}

// 7. Go To (modal dialog fed by keys): line, end, an invalid entry, Esc; offsets in the hex view.
static void sc_goto(StCtx* c)
{
  static const char orig[] = "line 1\r\nline 2\r\nline 3\r\nline 4\r\nline 5\r\nline 6\r\nline 7\r\nline 8\r\n"
                             "line 9\r\nline 10\r\n";
  char path[300], title[128];
  Tab* t = sc_open(c, "sc_goto.txt", orig, sizeof(orig) - 1, path, sizeof(path));
  if (!t) return;
  st_keys_modal(c, "^g", 0, "7{ENTER}", title, sizeof(title));
  st_checkf(c, strstr(title, "Line") != 0, "goto: Ctrl+G opens Go To Line (\"%s\")", title);
  st_checkf(c, t->sel.caret == t->doc->line_start(6) && sel_empty(t->sel), "goto: line 7 (caret %u)", t->sel.caret);
  st_keys_modal(c, "^g", 0, "end{ENTER}", 0, 0);
  st_checkf(c, t->doc->line_of(t->sel.caret) == t->doc->line_count() - 1, "goto: end (line %u)",
            t->doc->line_of(t->sel.caret) + 1);
  st_keys_modal(c, "^g", 0, "3:4{ENTER}", 0, 0);
  st_checkf(c, t->sel.caret == t->doc->line_start(2) + 3, "goto: 3:4 = line 3 column 4 (caret %u)", t->sel.caret);
  st_keys_modal(c, "^g", 0, "zz{ENTER}{ESC}", 0, 0);
  st_checkf(c, t->sel.caret == t->doc->line_start(2) + 3, "goto: invalid entry + Esc moves nothing (%u)",
            t->sel.caret);
  st_keys(c, "^+h");
  ST_CHECK(c, t->view == VIEW_HEX, "goto: hex view");
  st_keys_modal(c, "^g", 0, "1A{ENTER}", title, sizeof(title));
  st_checkf(c, strstr(title, "Offset") != 0, "goto: Ctrl+G in hex = Go To Offset (\"%s\")", title);
  st_checkf(c, t->sel.caret == 0x1A && t->hv.caret.pos == 0x1A, "goto: offset 1A (%u)", t->hv.caret.pos);
  st_keys_modal(c, "^g", 0, "#5{ENTER}", 0, 0);
  st_checkf(c, t->hv.caret.pos == 5, "goto: #5 decimal (%u)", t->hv.caret.pos);
  st_keys_modal(c, "^g", 0, "{ESC}", 0, 0);
  st_checkf(c, t->hv.caret.pos == 5, "goto: Esc keeps the caret (%u)", t->hv.caret.pos);
  st_keys(c, "^+h");
  SC_TEXT(c, t, orig, "goto: nothing changed");
  sc_close(c, t);
}

// 8. Help: F1 opens the read-only help tab at the shortcuts, Help > Regular Expressions jumps in the same tab;
// About is a modal dialog. <cfg>\st_help.hold (seconds) keeps the help tab up for a screenshot.
static int sc_line_is(Tab* t, epos pos, const char* s)
{
  u32 line = t->doc->line_of(pos), n = es_len(s);
  epos a = t->doc->line_start(line), b = t->doc->line_end(line);
  char buf[80];
  if (b - a != n || n >= sizeof(buf) || t->doc->read(a, (u8*)buf, n) != E_OK) return 0;
  return memcmp(buf, s, n) == 0;
}

static void sc_help(StCtx* c)
{
  int n0 = app_tab_count();
  epos len;
  char title[128], path[300];
  u32 hold = 0;
  Tab* t;
  st_keys(c, "{F1}");
  st_wait_idle(c, 3000);
  t = app_active();
  ST_CHECK(c, app_tab_count() == n0 + 1 && t && es_icmp(ui_path_name(t->path), "Pica Help.md") == 0,
           "help: F1 opens the help tab");
  if (!t || es_icmp(ui_path_name(t->path), "Pica Help.md") != 0) return;
  ST_CHECK(c, t->doc->read_only(), "help: read-only");
  ST_CHECK(c, t->lang && es_icmp(t->lang->id, "md") == 0, "help: Markdown colouring");
  ST_CHECK(c, t->view == VIEW_TEXT && sc_line_is(t, t->sel.caret, "## Keyboard shortcuts"), "help: at the shortcuts");
  st_checkf(c, t->tv.top_line == t->doc->line_of(t->sel.caret), "help: heading at the top (top %u, caret line %u)",
            t->tv.top_line, t->doc->line_of(t->sel.caret));
  st_cmd(c, IDM_HELP_REGEX);
  ST_CHECK(c, app_tab_count() == n0 + 1 && app_active() == t, "help: Regular Expressions reuses the tab");
  ST_CHECK(c, sc_line_is(t, t->sel.caret, "## Regular expressions"), "help: at the regex section");
  sc_focus(c);
  len = t->doc->length();
  st_keys(c, "x{DEL}");
  ST_CHECK(c, t->doc->length() == len && !t->doc->modified(), "help: typing changes nothing");
  ui_path_join(c->cfg_dir, "st_help.hold", path, sizeof(path));
  if (ui_file_exists(path)) {
    EBytes b;
    hold = 20;
    if (st_file_bytes(c, path, &b) == E_OK && b.size() && b.push(0) == E_OK) es_parse_u32((char*)b.data(), &hold, 10);
    st_cmd(c, IDM_HELP_KEYS);
    st_pump(c, hold * 1000);
  }
  st_keys(c, "^w");
  ST_CHECK(c, app_tab_count() == n0, "help: Ctrl+W closes it");
  ui_path_join(g_app.cfg_dir, "Pica Help.md", path, sizeof(path));
  ST_CHECK(c, ui_file_exists(path), "help: file in the settings folder");
  st_keys_modal(c, 0, IDM_HELP_ABOUT, "{ENTER}", title, sizeof(title));
  st_checkf(c, strstr(title, "About") != 0, "help: About dialog (\"%s\")", title);
}

// 9. Accelerator audit (15.5): every key reaches its command through the real loop (trace-only, nothing runs);
// a key whose menu item is grey is swallowed (checked statically instead). Plus: the table has no extra keys, every
// shortcut shown in a menu works, and view keys (Del, Ins, Tab) reach the view.
struct ScAccel { const char* keys; int id; };
static const ScAccel k_accels[] = {
  { "^n", IDM_FILE_NEW }, { "^o", IDM_FILE_OPEN }, { "^s", IDM_FILE_SAVE }, { "^%s", IDM_FILE_SAVEAS },
  { "^+s", IDM_FILE_SAVEALL }, { "^w", IDM_FILE_CLOSE }, { "^{F4}", IDM_FILE_CLOSE },
  { "^z", IDM_EDIT_UNDO }, { "%{BS}", IDM_EDIT_UNDO }, { "^y", IDM_EDIT_REDO }, { "^+z", IDM_EDIT_REDO },
  { "^x", IDM_EDIT_CUT }, { "+{DEL}", IDM_EDIT_CUT }, { "^c", IDM_EDIT_COPY }, { "^{INS}", IDM_EDIT_COPY },
  { "^v", IDM_EDIT_PASTE }, { "+{INS}", IDM_EDIT_PASTE }, { "^a", IDM_EDIT_SELECTALL },
  { "^d", IDM_LINE_DUP }, { "^l", IDM_LINE_CUT }, { "^+l", IDM_LINE_DEL }, { "^+{UP}", IDM_LINE_UP },
  { "^+{DOWN}", IDM_LINE_DOWN }, { "^q", IDM_EDIT_COMMENT }, { "^+u", IDM_EDIT_UPPER }, { "^u", IDM_EDIT_LOWER },
  { "^f", IDM_SEARCH_FIND }, { "^h", IDM_SEARCH_REPLACE }, { "^+f", IDM_SEARCH_FIF },
  { "{F3}", IDM_SEARCH_NEXT }, { "+{F3}", IDM_SEARCH_PREV }, { "^{F3}", IDM_SEARCH_SELNEXT },
  { "^+{F3}", IDM_SEARCH_SELPREV }, { "^i", IDM_SEARCH_ISEARCH }, { "{F4}", IDM_SEARCH_RESNEXT },
  { "+{F4}", IDM_SEARCH_RESPREV }, { "^g", IDM_SEARCH_GOTO }, { "^b", IDM_SEARCH_BRACE },
  { "^{VKDD}", IDM_SEARCH_BRACE }, { "^{F2}", IDM_BM_TOGGLE }, { "{F2}", IDM_BM_NEXT }, { "+{F2}", IDM_BM_PREV },
  { "^+h", IDM_VIEW_HEX }, { "^{TAB}", IDM_WIN_NEXT }, { "^{PGDN}", IDM_WIN_NEXT }, { "^+{TAB}", IDM_WIN_PREV },
  { "^{PGUP}", IDM_WIN_PREV }, { "{F1}", IDM_HELP_KEYS }
};

static int sc_acc_mods(const StKey& k)
{
  return (k.ctrl ? FCONTROL : 0) | (k.shift ? FSHIFT : 0) | (k.alt ? FALT : 0);
}

static int sc_acc_find(const ACCEL* acc, int n, int vk, int mods, int id)
{
  int i;
  for (i = 0; i < n; i++)
    if ((int)acc[i].key == vk && (acc[i].fVirt & (FCONTROL | FSHIFT | FALT)) == mods && (id < 0 || acc[i].cmd == id))
      return i;
  return -1;
}

// "Ctrl+Shift+F3" -> vk + FCONTROL/FSHIFT/FALT; 0 if not understood.
static int sc_parse_shortcut(const char* s, int* vk, int* mods)
{
  static const StKeyName names[] = {
    { "Up", VK_UP, 0 }, { "Down", VK_DOWN, 0 }, { "Left", VK_LEFT, 0 }, { "Right", VK_RIGHT, 0 }, { "Tab", VK_TAB, 0 },
    { "Del", VK_DELETE, 0 }, { "Ins", VK_INSERT, 0 }, { "PgUp", VK_PRIOR, 0 }, { "PgDn", VK_NEXT, 0 },
    { "Home", VK_HOME, 0 }, { "End", VK_END, 0 }, { "Backspace", VK_BACK, 0 }, { "Esc", VK_ESCAPE, 0 }
  };
  char tok[16];
  const char* p = s;
  u32 i;
  *vk = 0; *mods = 0;
  for (;;) {
    u32 n = 0;
    while (*p && *p != '+' && n + 1 < sizeof(tok)) tok[n++] = *p++;
    tok[n] = 0;
    if (*p == '+' && p[1]) {
      p++;
      if (es_icmp(tok, "Ctrl") == 0) *mods |= FCONTROL;
      else if (es_icmp(tok, "Shift") == 0) *mods |= FSHIFT;
      else if (es_icmp(tok, "Alt") == 0) *mods |= FALT;
      else return 0;
      continue;
    }
    if (*p) return 0;
    if (n == 1 && ((tok[0] >= 'A' && tok[0] <= 'Z') || (tok[0] >= '0' && tok[0] <= '9'))) { *vk = tok[0]; return 1; }
    if (n >= 2 && n <= 3 && tok[0] == 'F') {
      u32 f = 0;
      if (es_parse_u32(tok + 1, &f, 10) == E_OK && f >= 1 && f <= 12) { *vk = VK_F1 + (int)f - 1; return 1; }
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      if (es_icmp(tok, names[i].name) == 0) { *vk = names[i].vk; return 1; }
    return 0;
  }
}

static void sc_menu_shortcuts(StCtx* c, HMENU m, const ACCEL* acc, int nacc, u32* seen)
{
  int i, n = GetMenuItemCount(m);
  for (i = 0; i < n; i++) {
    char text[200];
    const char* tab;
    HMENU sub = GetSubMenu(m, i);
    UINT id;
    int vk, mods;
    if (sub) { sc_menu_shortcuts(c, sub, acc, nacc, seen); continue; }
    id = GetMenuItemID(m, i);
    if (id == 0 || id == 0xFFFFFFFF) continue;
    if (!GetMenuStringA(m, (UINT)i, text, sizeof(text), MF_BYPOSITION)) continue;
    tab = strchr(text, '\t');
    if (!tab) continue;
    tab++;
    // view keys and the system Alt+F4 are not accelerators (15.5)
    if (!strcmp(tab, "Del") || !strcmp(tab, "Ins") || !strcmp(tab, "Tab") || !strcmp(tab, "Shift+Tab") ||
        !strcmp(tab, "Alt+F4"))
      continue;
    (*seen)++;
    if (!sc_parse_shortcut(tab, &vk, &mods)) { st_checkf(c, 0, "menu \"%s\": shortcut not understood", text); continue; }
    st_checkf(c, sc_acc_find(acc, nacc, vk, mods, (int)id) >= 0, "menu \"%s\" shows %s but no accelerator sends %d",
              text, tab, (int)id);
  }
}

static void sc_accel_audit(StCtx* c)
{
  static const char text[] = "int f(void) { return (1); }\r\nline2\r\n";
  ACCEL acc[200];
  int nacc, i, grey = 0, live = 0;
  u32 seen = 0;
  char path[300];
  Tab* t2;
  Tab* t;
  // a state where (nearly) every command is enabled: two tabs, modified named file, undo + redo, a selection,
  // clipboard data
  t = sc_open(c, "sc_audit.c", text, sizeof(text) - 1, path, sizeof(path));
  if (!t) return;
  if (docio_new(&t2) != E_OK) t2 = 0;                       // after the file: an empty "new N" would be reused
  app_activate(app_tab_index(t));
  sc_focus(c);
  st_keys(c, "^{END}a");
  t->doc->break_undo_merge();                              // two undo steps: undo + redo both enabled
  st_keys(c, "b^z^{HOME}+{RIGHT}+{RIGHT}^c");
  ST_CHECK(c, t->doc->can_undo() && t->doc->can_redo() && t->doc->modified() && !sel_empty(t->sel),
           "audit: undo, redo, modified, selection");
  nacc = g_app.accel ? CopyAcceleratorTableA(g_app.accel, acc, 200) : 0;
  g_trace_ids.clear();
  cmd_set_trace(trace_fn, 1);
  for (i = 0; i < (int)(sizeof(k_accels) / sizeof(k_accels[0])); i++) {
    const ScAccel& a = k_accels[i];
    StKey k[2];
    UINT st;
    const CmdEntry* e = cmd_find(a.id, 0);
    if (st_parse_keys(c, a.keys, k, 2) != 1) { st_checkf(c, 0, "audit: bad key spec %s", a.keys); continue; }
    st_checkf(c, sc_acc_find(acc, nacc, k[0].vk, sc_acc_mods(k[0]), a.id) >= 0, "audit: %s -> %s not in IDA_MAIN",
              a.keys, e ? e->name : "?");
    st = cmd_state(a.id);
    sc_focus(c);
    g_trace_ids.clear();
    st_send_key(c, k[0]);
    if (st & MF_GRAYED) {
      grey++;
      st_checkf(c, g_trace_ids.size() == 0, "audit: %s (grey %s) sent %u commands", a.keys, e ? e->name : "?",
                g_trace_ids.size());
      selftest_log("audit: %s -> %s is grey now (key swallowed; table entry checked)", a.keys, e ? e->name : "?");
    } else {
      live++;
      st_checkf(c, g_trace_ids.size() == 1 && g_trace_ids[0] == a.id, "audit: %s -> %s: got %u commands (first %d)",
                a.keys, e ? e->name : "?", g_trace_ids.size(), g_trace_ids.size() ? g_trace_ids[0] : 0);
    }
  }
  cmd_set_trace(0, 0);
  g_trace_ids.release();
  st_checkf(c, live >= 44, "audit: %d keys checked live, %d grey", live, grey);
  // no accelerator outside the 15.5 list
  for (i = 0; i < nacc; i++) {
    u32 j;
    int found = 0;
    for (j = 0; !found && j < sizeof(k_accels) / sizeof(k_accels[0]); j++) {
      StKey k[2];
      if (st_parse_keys(c, k_accels[j].keys, k, 2) == 1 && (int)acc[i].key == k[0].vk &&
          (acc[i].fVirt & (FCONTROL | FSHIFT | FALT)) == sc_acc_mods(k[0]) && acc[i].cmd == k_accels[j].id)
        found = 1;
    }
    st_checkf(c, found, "audit: accelerator key %d mods %d -> %d is not in the 15.5 list", (int)acc[i].key,
              (int)acc[i].fVirt, (int)acc[i].cmd);
  }
  st_checkf(c, nacc == (int)(sizeof(k_accels) / sizeof(k_accels[0])), "audit: %d accelerators, want %u", nacc,
            (u32)(sizeof(k_accels) / sizeof(k_accels[0])));
  // every shortcut shown in the menus works
  sc_menu_shortcuts(c, GetMenu(g_app.frame), acc, nacc, &seen);
  st_checkf(c, seen >= 39, "audit: %u menu shortcuts checked", seen);
  // view keys are not accelerators: they reach the view
  sc_undo_all(c, t, 0);
  st_keys(c, "^{HOME}{DEL}");
  SC_TEXT(c, t, "nt f(void) { return (1); }\r\nline2\r\n", "audit: Del reaches the view");
  st_keys(c, "{INS}");
  ST_CHECK(c, t->tv.overwrite == 1, "audit: Ins toggles overwrite");
  st_keys(c, "{INS}{TAB}");
  ST_CHECK(c, t->tv.overwrite == 0, "audit: Ins again");
  SC_TEXT(c, t, "\tnt f(void) { return (1); }\r\nline2\r\n", "audit: Tab reaches the view");
  sc_close(c, t);
  sc_close(c, t2);
}

// 10. AltGr: characters typed with Ctrl+Alt (Finnish layout when it can be loaded, else simulated Finnish/German
// AltGr keys on the current layout) are never swallowed by an accelerator and arrive in the document.
struct ScAltGr { int ch; int vk; const char* where; };
static const ScAltGr k_altgr[] = {
  { '@', '2', "FI 2" }, { '{', '7', "FI 7" }, { '[', '8', "FI 8" }, { ']', '9', "FI 9" }, { '}', '0', "FI 0" },
  { '\\', 0xBB, "FI +" }, { '|', 0xE2, "FI <" }, { '~', 0xBA, "FI dead" }, { 0x80, 'E', "FI E (euro)" },
  { '$', '4', "FI 4" }, { 0xA3, '3', "FI 3 (pound)" }, { 0xB5, 'M', "FI M (micro)" },
  { '@', 'Q', "DE Q" }, { '\\', 0xDB, "DE sharp s" }, { '~', 0xBB, "DE +" }
};

static void sc_altgr(StCtx* c)
{
  HKL old = GetKeyboardLayout(0), fi = 0, list[32];
  int nl, i, loaded = 0, real;
  u32 typed = 0;
  Tab* t;
  EBytes want;
  BYTE ks[256], saved[256];
  if (docio_new(&t) != E_OK || !t) { ST_CHECK(c, 0, "altgr: new tab"); return; }
  sc_focus(c);
  nl = GetKeyboardLayoutList(32, list);
  for (i = 0; i < nl; i++) if (LOWORD((DWORD)list[i]) == 0x040B) fi = list[i];
  if (!fi) { fi = LoadKeyboardLayoutA("0000040B", KLF_NOTELLSHELL); loaded = fi != 0; }
  if (fi && (VkKeyScanExA('@', fi) & 0x600) != 0x600) {          // not a real Finnish layout (Win98 fallback)
    if (loaded && fi != old) UnloadKeyboardLayout(fi);
    fi = 0; loaded = 0;
  }
  real = fi != 0;
  if (real) ActivateKeyboardLayout(fi, 0);
  selftest_log("altgr: %s", real ? "Finnish layout loaded" : "no Finnish layout: simulated AltGr keys");
  g_trace_ids.clear();
  cmd_set_trace(trace_fn, 1);
  GetKeyboardState(saved);
  for (i = 0; i < (int)(sizeof(k_altgr) / sizeof(k_altgr[0])); i++) {
    const ScAltGr& a = k_altgr[i];
    StKey k;
    HWND target = st_target();
    epos len0 = t->doc->length();
    u8 last = 0;
    k.vk = a.vk; k.ctrl = 1; k.alt = 1; k.shift = 0; k.chr = 0;
    if (real) {
      SHORT s = VkKeyScanExA((CHAR)a.ch, fi);
      if (a.where[0] != 'F') continue;                            // German positions only when simulating
      if (s == -1 || (s & 0x600) != 0x600 || (MapVirtualKeyExA((UINT)(s & 0xFF), 2, fi) & 0x80008000u)) {
        selftest_log("altgr: '%c' skipped (dead key or not AltGr on this layout)", a.ch);
        continue;
      }
      k.vk = s & 0xFF; k.shift = (u8)((s & 0x100) ? 1 : 0);
    }
    g_trace_ids.clear();
    memcpy(ks, saved, sizeof(ks));
    st_set_mods(ks, 1, k.shift, 1);
    ks[VK_RMENU] = 0x80;
    ks[k.vk] = 0x80;
    SetKeyboardState(ks);
    st_post_key(target, k, 0);                                    // Ctrl+Alt: WM_KEYDOWN, like a real AltGr key
    st_pump(c, 0);
    if (t->doc->length() == len0) {                               // the layout made no character: as Windows would
      PostMessageA(target, WM_CHAR, (WPARAM)a.ch, st_key_lparam(k, 0));
      st_pump(c, 0);
    }
    ks[k.vk] = 0;
    SetKeyboardState(ks);
    st_post_key(target, k, 1);
    st_pump(c, 0);
    SetKeyboardState(saved);
    if (t->doc->length() > len0) t->doc->read(t->doc->length() - 1, &last, 1);
    st_checkf(c, g_trace_ids.size() == 0, "altgr: '%c' (%s) triggered command %d", a.ch, a.where,
              g_trace_ids.size() ? g_trace_ids[0] : 0);
    st_checkf(c, t->doc->length() == len0 + 1 && last == (u8)a.ch, "altgr: '%c' (%s) typed (len %u -> %u, last 0x%02X)",
              a.ch, a.where, len0, t->doc->length(), (u32)last);
    if (t->doc->length() == len0 + 1) { want.push((u8)a.ch); typed++; }
  }
  // known limit: Ctrl+Alt+S (Save As, Notepad++'s key) takes AltGr+S on layouts that put a letter there
  {
    StKey k;
    HWND target = st_target();
    k.vk = 'S'; k.ctrl = 1; k.alt = 1; k.shift = 0; k.chr = 0;
    g_trace_ids.clear();
    memcpy(ks, saved, sizeof(ks));
    st_set_mods(ks, 1, 0, 1);
    ks['S'] = 0x80;
    SetKeyboardState(ks);
    st_post_key(target, k, 0);
    st_pump(c, 0);
    ks['S'] = 0;
    SetKeyboardState(ks);
    st_post_key(target, k, 1);
    st_pump(c, 0);
    SetKeyboardState(saved);
    selftest_log("altgr: Ctrl+Alt+S -> %s (Polish programmers' AltGr+S is s-acute: see HANDOFF T15)",
                 g_trace_ids.size() && g_trace_ids[0] == IDM_FILE_SAVEAS ? "Save As" : "nothing");
  }
  cmd_set_trace(0, 0);
  g_trace_ids.release();
  if (real) { ActivateKeyboardLayout(old, 0); if (loaded) UnloadKeyboardLayout(fi); }
  st_checkf(c, typed >= 10, "altgr: %u characters typed", typed);
  sc_text(c, t, (const char*)want.data(), want.size(), "altgr: document holds exactly the typed characters");
  sc_close(c, t);
}

struct ScEntry { const char* name; void (*fn)(StCtx*); };
static const ScEntry k_sc[] = {
    { "save cycle", sc_save_cycle }, { "hex switch", sc_hex_switch }, { "find/replace", sc_find_replace },
    { "encoding", sc_encoding }, { "line commands", sc_lines }, { "bookmarks", sc_bookmarks }, { "go to", sc_goto },
    { "help", sc_help }, { "accelerator audit", sc_accel_audit }, { "altgr", sc_altgr }
};

void st_scenarios(StCtx* c)
{
  u32 i;
  int tabs0 = app_tab_count();
  ShowWindow(g_app.frame, SW_SHOWNORMAL);
  SetForegroundWindow(g_app.frame);
  if (g_app.find_dlg && IsWindowVisible(g_app.find_dlg)) SendMessageA(g_app.find_dlg, WM_COMMAND, IDCANCEL, 0);
  st_pump(c, 50);
  for (i = 0; i < sizeof(k_sc) / sizeof(k_sc[0]); i++) {
    u32 f0 = c->failed, n0 = c->checks;
    k_sc[i].fn(c);
    st_wait_idle(c, 5000);
    log_line(c, "  scenario %s: %u checks, %u failed", k_sc[i].name, c->checks - n0, c->failed - f0);
  }
  st_checkf(c, app_tab_count() <= tabs0, "scenarios left %d tabs (had %d)", app_tab_count(), tabs0);
}
