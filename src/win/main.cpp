// main.cpp - WinMain, command line, settings folder, message loop, SEH guard (DESIGN 13.1, 15.7, 15.11, 15.12).
// T00: working shell. T07 adds single instance (WM_COPYDATA), lang_init from RCDATA + langs\ overrides, timers.
// Owner after Wave 0: T07.
#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "sessmgr.h"
#include "watch.h"
#include "iothread.h"
#include "selftest.h"
#include "bench.h"
#include "winenc.h"
#include "frame_int.h"
#include "codec.h"
#include "config.h"
#include "ini.h"
#include "fileops.h"
#include "estr.h"
#include "resource.h"

App g_app;

struct CmdFile { char path[260]; OpenOpts opts; };
static Vec<CmdFile> g_cmd_files;
static char g_selftest_names[512];
static char g_log_path[260];
static char g_fixdir[260];

u32 app_now_ms()
{
  return GetTickCount();
}

const char* main_ini_path(char* out, u32 cap)
{
  ui_path_join(g_app.cfg_dir, APP_INI, out, cap);
  return out;
}

// ---- settings folder (13.1) ----

static int dir_writable(const char* dir)
{
  char test[300];
  HANDLE h;
  ui_path_join(dir, "~pica.tst", test, sizeof(test));
  h = CreateFileA(test, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (h == INVALID_HANDLE_VALUE) return 0;
  CloseHandle(h);
  DeleteFileA(test);
  return 1;
}

typedef BOOL (WINAPI* SpecialFolderFn)(HWND, LPSTR, int, BOOL);

static void find_cfg_dir(const char* override_dir)
{
  char buf[MAX_PATH];
  if (override_dir && override_dir[0]) {
    if (ui_path_full(override_dir, g_app.cfg_dir, sizeof(g_app.cfg_dir)) != E_OK)
      es_copy(g_app.cfg_dir, sizeof(g_app.cfg_dir), override_dir);
    CreateDirectoryA(g_app.cfg_dir, 0);
    return;
  }
  if (dir_writable(g_app.exe_dir)) { es_copy(g_app.cfg_dir, sizeof(g_app.cfg_dir), g_app.exe_dir); return; }
  {
    HMODULE sh = LoadLibraryA("shell32.dll");
    SpecialFolderFn fn = sh ? (SpecialFolderFn)GetProcAddress(sh, "SHGetSpecialFolderPathA") : 0;
    if (fn && fn(0, buf, 0x001A /* CSIDL_APPDATA */, TRUE)) {
      ui_path_join(buf, APP_NAME, g_app.cfg_dir, sizeof(g_app.cfg_dir));
      CreateDirectoryA(g_app.cfg_dir, 0);
      if (dir_writable(g_app.cfg_dir)) return;
    }
  }
  GetWindowsDirectoryA(buf, sizeof(buf));
  ui_path_join(buf, APP_NAME, g_app.cfg_dir, sizeof(g_app.cfg_dir));
  CreateDirectoryA(g_app.cfg_dir, 0);
}

// ---- settings file ----

static void load_config()
{
  char path[300];
  EBytes bytes;
  Ini ini;
  config_defaults(&g_app.cfg);
  main_ini_path(path, sizeof(path));
  if (file_read_all(fs_os(), path, &bytes, 1u << 20) != E_OK) return;   // E_NOTIMPL until T05, or no file yet
  if (ini.parse((const char*)bytes.data(), bytes.size()) != E_OK) return;
  config_load(&g_app.cfg, ini);
}

int app_save_config()
{
  char path[300];
  EBytes bytes;
  Ini ini;
  EStr text;
  SaveOpts so;
  SaveResult sr;
  int rc;
  if (g_app.selftest || g_app.bench) return E_OK;          // selftests never touch the settings
  main_ini_path(path, sizeof(path));
  if (file_read_all(fs_os(), path, &bytes, 1u << 20) == E_OK)
    ini.parse((const char*)bytes.data(), bytes.size());     // keeps unknown keys and comments
  config_store(&g_app.cfg, &ini);
  rc = ini.write(&text);
  if (rc != E_OK) return rc;                                // E_NOTIMPL until T06
  {
    ViewSource src((const u8*)text.c_str(), text.len());
    so.keep_bak = 0; so.pad[0] = so.pad[1] = so.pad[2] = 0;
    rc = file_save(fs_os(), path, &src, so, 0, 0, &sr);    // E_NOTIMPL until T05
  }
  return rc;
}

// ---- message dispatch and idle work ----

void app_dispatch(MSG* m)
{
  if (g_app.find_dlg && (m->hwnd == g_app.find_dlg || IsChild(g_app.find_dlg, m->hwnd))) {
    if (g_app.accel_find && TranslateAcceleratorA(g_app.frame, g_app.accel_find, m)) return;
    if (IsDialogMessageA(g_app.find_dlg, m)) return;
  }
  if (g_app.accel && g_app.frame && TranslateAcceleratorA(g_app.frame, g_app.accel, m)) return;
  TranslateMessage(m);
  DispatchMessageA(m);
}

int app_idle_work()
{
  Tab* t = app_active();
  if (t && t->hl && t->hl->enabled() && t->hl->work(200)) return 1;
  sessmgr_idle();
  return 0;
}

// SEH guard (15.12): kept in a function without C++ objects so __try is allowed.
static int guarded_dispatch(MSG* m)
{
  __try {
    app_dispatch(m);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
  return 0;
}

static void emergency_exit()
{
  sessmgr_emergency();
  MessageBoxA(g_app.frame, APP_NAME " hit an internal error. Your unsaved work was backed up and will be restored at "
              "the next start.", APP_NAME, MB_OK | MB_ICONERROR);
  ExitProcess(3);
}

static int run_loop()
{
  MSG m;
  HANDLE h[16];
  for (;;) {
    u32 n;
    DWORD r;
    while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) return (int)m.wParam;
      if (guarded_dispatch(&m)) emergency_exit();
    }
    if (app_idle_work()) continue;
    n = watch_handles(h, 16);
    r = MsgWaitForMultipleObjects(n, n ? h : 0, FALSE, INFINITE, QS_ALLINPUT);
    if (r != WAIT_FAILED && r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) watch_signalled(r - WAIT_OBJECT_0);
    else if (r == WAIT_FAILED) Sleep(10);
  }
}

// ---- command line (15.7) ----

static void parse_command_line(char* cfg_override, u32 cap)
{
  OpenOpts next;
  int i;
  docio_default_opts(&next);
  cfg_override[0] = 0;
  for (i = 1; i < __argc; i++) {
    const char* a = __argv[i];
    if (a[0] == '-' || a[0] == '/') {
      const char* o = a + 1;
      if (es_icmp(o, "selftest") == 0) {
        g_app.selftest = 1;
        while (i + 1 < __argc && __argv[i + 1][0] != '-' && __argv[i + 1][0] != '/') {
          if (g_selftest_names[0]) es_cat(g_selftest_names, sizeof(g_selftest_names), " ");
          es_cat(g_selftest_names, sizeof(g_selftest_names), __argv[++i]);
        }
      }
      else if (es_icmp(o, "cfg") == 0 && i + 1 < __argc) es_copy(cfg_override, cap, __argv[++i]);
      else if (es_icmp(o, "log") == 0 && i + 1 < __argc) es_copy(g_log_path, sizeof(g_log_path), __argv[++i]);
      else if (es_icmp(o, "fixdir") == 0 && i + 1 < __argc) es_copy(g_fixdir, sizeof(g_fixdir), __argv[++i]);
      else if (es_icmp(o, "bench") == 0) g_app.bench = 1;
      else if (es_icmp(o, "multi") == 0) g_app.multi = 1;
      else if (es_icmp(o, "nosession") == 0) g_app.nosession = 1;
      else if (es_icmp(o, "hex") == 0) next.hex = 1;
      else if (es_icmp(o, "ro") == 0) next.read_only = 1;
      else if ((o[0] == 'n' || o[0] == 'N') && o[1] >= '0' && o[1] <= '9') {
        u32 line;
        if (es_parse_u32(o + 1, &line, 10) == E_OK) next.line = line;
      }
      // unknown options are ignored
    } else {
      CmdFile f;
      es_copy(f.path, sizeof(f.path), a);
      f.opts = next;
      g_cmd_files.push(f);
      docio_default_opts(&next);
    }
  }
}

static void open_command_line_files()
{
  u32 i;
  for (i = 0; i < g_cmd_files.size(); i++) {
    Tab* t = 0;
    int rc = docio_open(g_cmd_files[i].path, &g_cmd_files[i].opts, &t);
    if (rc < 0) app_status_note("Could not open %s (%s)", g_cmd_files[i].path, e_errname(rc));
  }
  g_cmd_files.release();
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show)
{
  INITCOMMONCONTROLSEX icc;
  char cfg_override[260];
  char* slash;
  int rc = 0;
  (void)prev; (void)cmdline;

  g_app.hinst = hinst;
  g_app.active = -1;
  g_app.next_tabid = 1;
  g_app.next_untitled = 1;

  parse_command_line(cfg_override, sizeof(cfg_override));

  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES;
  InitCommonControlsEx(&icc);

  codec_init();
  enc_set_system((u16)GetACP(), (u16)GetOEMCP());
  winenc_init();

  GetModuleFileNameA(0, g_app.exe_dir, sizeof(g_app.exe_dir));
  slash = 0;
  {
    char* p;
    for (p = g_app.exe_dir; *p; p++) if (*p == '\\' || *p == '/') slash = p;
  }
  if (slash) *slash = 0;
  find_cfg_dir(cfg_override);
  load_config();
  if (g_app.selftest) selftest_set_paths(g_log_path, g_fixdir);

  if (!frame_register(hinst) || !frame_create(show)) {
    MessageBoxA(0, "Could not create the main window.", APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
  }
  iothread_start();

  if (g_app.selftest) {
    rc = selftest_run(g_selftest_names);
  } else if (g_app.bench) {
    rc = bench_run();
    if (rc < 0) rc = 100;
  } else {
    sessmgr_startup();
    open_command_line_files();
    rc = run_loop();
  }
  if (IsWindow(g_app.frame)) DestroyWindow(g_app.frame);
  iothread_stop();
  return rc;
}
