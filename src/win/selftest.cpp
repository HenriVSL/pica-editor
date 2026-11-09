// selftest.cpp - pica.exe -selftest framework and helpers (DESIGN 17.3). T00: framework, log, basic helpers and the
// framework self-check (st_framework). T15 completes st_keys/st_wait_idle and adds st_scenarios + the accelerator audit.
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

int st_wait_idle(StCtx* c, u32 timeout_ms)
{
  // T15 adds the I/O worker, indexer and FiF checks
  u32 start = GetTickCount();
  for (;;) {
    st_pump(c, 0);
    if (!app_idle_work()) return 1;
    if (GetTickCount() - start >= timeout_ms) return 0;
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

// st_keys syntax: "^" Ctrl, "+" Shift, "%" Alt apply to the next key; "{NAME}" named keys; other chars typed.
struct StKeyName { const char* name; int vk; };
static const StKeyName k_keys[] = {
  { "ENTER", VK_RETURN }, { "TAB", VK_TAB }, { "ESC", VK_ESCAPE }, { "BS", VK_BACK }, { "DEL", VK_DELETE },
  { "INS", VK_INSERT }, { "HOME", VK_HOME }, { "END", VK_END }, { "PGUP", VK_PRIOR }, { "PGDN", VK_NEXT },
  { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "SPACE", VK_SPACE },
  { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 },
  { "F7", VK_F7 }, { "F8", VK_F8 }, { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
  { "PLUS", '+' }, { "CARET", '^' }, { "PERCENT", '%' }, { "LBRACE", '{' }, { "RBRACE", '}' }
};

static void st_send_key(StCtx* c, int vk, int ctrl, int shift, int alt)
{
  BYTE ks[256], saved[256];
  HWND target = GetFocus();
  UINT scan = MapVirtualKeyA((UINT)vk, 0);
  LPARAM down = 1 | ((LPARAM)scan << 16) | (alt ? (1L << 29) : 0);
  LPARAM up = down | (1L << 30) | (1L << 31);
  if (!target) target = g_app.frame;
  GetKeyboardState(saved);
  memcpy(ks, saved, sizeof(ks));
  ks[VK_CONTROL] = (BYTE)(ctrl ? 0x80 : 0); ks[VK_LCONTROL] = ks[VK_CONTROL];
  ks[VK_SHIFT] = (BYTE)(shift ? 0x80 : 0);  ks[VK_LSHIFT] = ks[VK_SHIFT];
  ks[VK_MENU] = (BYTE)(alt ? 0x80 : 0);     ks[VK_LMENU] = ks[VK_MENU];
  ks[vk & 0xFF] = 0x80;
  SetKeyboardState(ks);
  PostMessageA(target, alt ? WM_SYSKEYDOWN : WM_KEYDOWN, (WPARAM)vk, down);
  st_pump(c, 0);
  ks[vk & 0xFF] = 0;
  SetKeyboardState(ks);
  PostMessageA(target, alt ? WM_SYSKEYUP : WM_KEYUP, (WPARAM)vk, up);
  st_pump(c, 0);
  SetKeyboardState(saved);
}

void st_keys(StCtx* c, const char* keys)
{
  const char* p = keys;
  int ctrl = 0, shift = 0, alt = 0;
  while (p && *p) {
    int vk = -1;
    if (*p == '^') { ctrl = 1; p++; continue; }
    if (*p == '+') { shift = 1; p++; continue; }
    if (*p == '%') { alt = 1; p++; continue; }
    if (*p == '{') {
      char name[16];
      u32 n = 0, i;
      p++;
      while (*p && *p != '}' && n + 1 < sizeof(name)) name[n++] = *p++;
      name[n] = 0;
      if (*p == '}') p++;
      for (i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) if (es_icmp(name, k_keys[i].name) == 0) vk = k_keys[i].vk;
      if (vk < 0) { st_checkf(c, 0, "st_keys: unknown key {%s}", name); return; }
      if (vk == '+' || vk == '^' || vk == '%' || vk == '{' || vk == '}') {
        SHORT s = VkKeyScanA((CHAR)vk);
        if (s != -1) { shift = shift || (s & 0x100); vk = s & 0xFF; }
      }
    } else {
      SHORT s = VkKeyScanA(*p);
      p++;
      if (s == -1) continue;
      vk = s & 0xFF;
      if (s & 0x100) shift = 1;
      if (s & 0x200) ctrl = 1;
      if (s & 0x400) alt = 1;
    }
    st_send_key(c, vk, ctrl, shift, alt);
    ctrl = shift = alt = 0;
  }
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
void st_scenarios(StCtx* c) { st_skip(c, "not implemented"); }
