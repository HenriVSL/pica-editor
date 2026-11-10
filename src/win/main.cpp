// main.cpp - WinMain, command line, single instance, settings folder and file, language files, message loop, idle
// work, SEH guard (DESIGN 13.1, 15.5, 15.7, 15.11, 15.12). Owner: T07.
#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
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
#include "fifworker.h"
#include "winenc.h"
#include "frame_int.h"
#include "codec.h"
#include "config.h"
#include "ini.h"
#include "lexer.h"
#include "fileops.h"
#include "estr.h"
#include "resource.h"

App g_app;

static const DWORD k_copydata_magic = 0x41434950;   // 'PICA'
static const char k_block_tag[] = "PICA1";

static Vec<MainFileArg> g_cmd_files;
static char g_selftest_names[512];
static char g_log_path[260];
static char g_fixdir[260];
static int g_accept_copydata_st = 0;
static EStr g_lang_errors;

u32 app_now_ms()
{
  return GetTickCount();
}

const char* main_ini_path(char* out, u32 cap)
{
  ui_path_join(g_app.cfg_dir, APP_INI, out, cap);
  return out;
}

const char* main_lang_errors()
{
  return g_lang_errors.c_str();
}

void main_accept_copydata_in_selftest(int on)
{
  g_accept_copydata_st = on;
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

// ---- settings file (pica.ini through Ini + file_save: temp file + rename) ----

int main_load_config_from(const char* path, Config* out)
{
  EBytes bytes;
  Ini ini;
  int rc;
  config_defaults(out);
  rc = file_read_all(fs_os(), path, &bytes, 1u << 20);
  if (rc != E_OK) return rc;                                 // no file yet: defaults
  rc = ini.parse((const char*)bytes.data(), bytes.size());
  if (rc != E_OK) return rc;
  config_load(out, ini);
  return E_OK;
}

static void load_config()
{
  char path[300];
  main_ini_path(path, sizeof(path));
  main_load_config_from(path, &g_app.cfg);
}

int main_save_config_to(const char* path)
{
  EBytes bytes;
  Ini ini;
  EStr text;
  SaveOpts so;
  SaveResult sr;
  int rc;
  if (file_read_all(fs_os(), path, &bytes, 1u << 20) == E_OK)
    ini.parse((const char*)bytes.data(), bytes.size());     // keeps unknown keys and comments
  config_store(&g_app.cfg, &ini);
  rc = ini.write(&text);
  if (rc != E_OK) return rc;
  memset(&so, 0, sizeof(so));
  {
    ViewSource src((const u8*)text.c_str(), text.len());
    rc = file_save(fs_os(), path, &src, so, 0, 0, &sr);
  }
  return rc;
}

int app_save_config()
{
  char path[300];
  int rc;
  if (g_app.selftest || g_app.bench) return E_OK;          // selftests never touch the settings
  rc = main_save_config_to(main_ini_path(path, sizeof(path)));
  if (rc != E_OK) app_status_note("Could not save the settings to %s (%s)", path, e_errname(rc));
  return rc;
}

// ---- language files: built-in RCDATA + langs\*.lng overrides (DESIGN 2.3, T04 notes) ----

static const char* const k_lang_files[IDR_LANG_COUNT] = {
  "cpp.lng", "rc.lng", "make.lng", "bat.lng", "ini.lng", "asm.lng", "python.lng", "html.lng", "js.lng", "css.lng",
  "xml.lng", "md.lng", "diff.lng"
};

class RcLangSource : public LangSource {
public:
  u32 count() { return IDR_LANG_COUNT; }
  int get(u32 i, const char** name, const char** text, u32* len) {
    HRSRC r;
    HGLOBAL g;
    if (i >= IDR_LANG_COUNT) return E_RANGE;
    r = FindResourceA(g_app.hinst, MAKEINTRESOURCEA(IDR_LANG_FIRST + i), (LPCSTR)RT_RCDATA);
    g = r ? LoadResource(g_app.hinst, r) : 0;
    if (!g) return E_NOTFOUND;
    *name = k_lang_files[i];
    *text = (const char*)LockResource(g);
    *len = SizeofResource(g_app.hinst, r);
    return *text ? E_OK : E_NOTFOUND;
  }
};

struct LangFile { char name[64]; char* text; u32 len; };

class DirLangSource : public LangSource {
public:
  ~DirLangSource() {
    u32 i;
    for (i = 0; i < files_.size(); i++) e_free(files_[i].text);
  }
  u32 count() { return files_.size(); }
  int get(u32 i, const char** name, const char** text, u32* len) {
    if (i >= files_.size()) return E_RANGE;
    *name = files_[i].name;
    *text = files_[i].text;
    *len = files_[i].len;
    return E_OK;
  }
  void scan(const char* dir) {                 // adds *.lng files whose name isn't listed yet
    char mask[300], folder[300], path[300];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    ui_path_join(dir, "langs\\*.lng", mask, sizeof(mask));
    h = FindFirstFileA(mask, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
      LangFile f;
      EBytes b;
      u32 j;
      int dup = 0;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      for (j = 0; j < files_.size(); j++) if (es_icmp(files_[j].name, fd.cFileName) == 0) dup = 1;
      if (dup) continue;
      ui_path_join(dir, "langs", folder, sizeof(folder));
      ui_path_join(folder, fd.cFileName, path, sizeof(path));
      if (file_read_all(fs_os(), path, &b, 1u << 20) != E_OK) continue;
      es_copy(f.name, sizeof(f.name), fd.cFileName);
      f.len = b.size();
      f.text = (char*)e_alloc(f.len + 1);
      if (!f.text) continue;
      if (f.len) memcpy(f.text, b.data(), f.len);
      f.text[f.len] = 0;
      if (files_.push(f) != E_OK) e_free(f.text);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
private:
  Vec<LangFile> files_;
};

static RcLangSource g_lang_rc;
static DirLangSource g_lang_dir;           // kept for the life of the process (lexers may refer to the texts)

static void init_languages()
{
  int rc;
  g_lang_dir.scan(g_app.cfg_dir);            // settings folder first: it wins over the exe folder
  if (es_icmp(g_app.cfg_dir, g_app.exe_dir) != 0) g_lang_dir.scan(g_app.exe_dir);
  g_lang_errors.clear();
  rc = lang_init(&g_lang_rc, g_lang_dir.count() ? &g_lang_dir : 0, &g_lang_errors);
  if (rc != E_OK && g_lang_errors.len() == 0) g_lang_errors.appendf("Language files: %s\n", e_errname(rc));
}

static void show_lang_errors()
{
  char first[200];
  const char* e = g_lang_errors.c_str();
  u32 n = 0;
  if (!e[0]) return;
  while (e[n] && e[n] != '\n' && n + 1 < sizeof(first)) { first[n] = e[n]; n++; }
  first[n] = 0;
  selftest_log("language file errors: %s", e);
  statusbar_note_ms(15000, "Language file problem: %s", first);
}

// ---- message dispatch and idle work ----

// Ctrl+Alt+key that the current keyboard layout turns into a character (AltGr, e.g. AltGr+S = s-acute on Polish
// Programmers): such a key types, it is not an accelerator. Only the keys IDA_MAIN uses with Ctrl+Alt are checked,
// because ToAscii consumes dead keys.
static int altgr_char(const MSG* m)
{
  BYTE ks[256];
  WORD ch = 0;
  if (m->message != WM_KEYDOWN || GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) >= 0) return 0;
  if (m->wParam != 'S') return 0;
  if (!GetKeyboardState(ks)) return 0;
  if (ToAscii((UINT)m->wParam, (UINT)((m->lParam >> 16) & 0xFF), ks, &ch, 0) != 1) return 0;
  return (ch & 0xFF) >= 0x20;
}

// Ctrl+A in an edit field of a dialog selects its text (Win98 edit controls ignore it; users expect it).
static int dialog_select_all(const MSG* m)
{
  char cls[16];
  if (m->message != WM_KEYDOWN || m->wParam != 'A') return 0;
  if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0) return 0;
  if (!GetClassNameA(m->hwnd, cls, sizeof(cls)) || lstrcmpiA(cls, "Edit") != 0) return 0;
  SendMessageA(m->hwnd, EM_SETSEL, 0, -1);
  return 1;
}

void app_dispatch(MSG* m)
{
  if (g_app.find_dlg && (m->hwnd == g_app.find_dlg || IsChild(g_app.find_dlg, m->hwnd))) {
    if (dialog_select_all(m)) return;
    if (g_app.accel_find && TranslateAcceleratorA(g_app.frame, g_app.accel_find, m)) return;
    if (IsDialogMessageA(g_app.find_dlg, m)) return;
  } else {
    // The incremental search box and the results panel keep their editing keys (Ctrl+C/V/X/Z/A) away from the main
    // accelerators, which would act on the document.
    if (isearch_translate(m) || results_translate(m)) return;
    if (!altgr_char(m) && g_app.accel && g_app.frame && TranslateAcceleratorA(g_app.frame, g_app.accel, m)) return;
  }
  TranslateMessage(m);
  DispatchMessageA(m);
}

int app_idle_work()
{
  Tab* t;
  frame_idle();
  t = app_active();
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

static int guarded_idle(int* more)
{
  __try {
    *more = app_idle_work();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
  return 0;
}

static void emergency_exit()
{
  static int once = 0;
  if (once) ExitProcess(4);
  once = 1;
  sessmgr_emergency();
  MessageBoxA(0, APP_NAME " hit an internal error. Your unsaved work was backed up and will be restored at the next "
              "start.", APP_NAME, MB_OK | MB_ICONERROR | MB_TASKMODAL);
  ExitProcess(3);
}

static int run_loop()
{
  MSG m;
  HANDLE h[16];
  for (;;) {
    u32 n;
    DWORD r;
    int more = 0;
    while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) return (int)m.wParam;
      if (guarded_dispatch(&m)) emergency_exit();
    }
    if (guarded_idle(&more)) emergency_exit();
    if (more) continue;
    n = watch_handles(h, 16);
    r = MsgWaitForMultipleObjects(n, n ? h : 0, FALSE, INFINITE, QS_ALLINPUT);
    if (r != WAIT_FAILED && r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) watch_signalled(r - WAIT_OBJECT_0);
    else if (r == WAIT_FAILED) Sleep(10);
  }
}

// ---- command line (15.7) ----

static int path_is_absolute(const char* p)
{
  return (p[0] && p[1] == ':') || ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/'));
}

// Full path of a command-line file argument; base = folder relative names are resolved against (NULL = ours).
static void resolve_path(const char* arg, const char* base, char* out, u32 cap)
{
  char tmp[600];
  if (base && base[0] && !path_is_absolute(arg)) {
    if (arg[0] == '\\' || arg[0] == '/') {               // root-relative: the base folder's drive
      tmp[0] = base[0]; tmp[1] = base[1]; tmp[2] = 0;
      if (base[1] != ':') tmp[0] = 0;
      es_cat(tmp, sizeof(tmp), arg);
    } else {
      ui_path_join(base, arg, tmp, sizeof(tmp));
    }
  } else {
    es_copy(tmp, sizeof(tmp), arg);
  }
  if (ui_path_full(tmp, out, cap) != E_OK) es_copy(out, cap, tmp);
}

static int is_option(const char* a)
{
  return a[0] == '-' || (a[0] == '/' && a[1] && a[1] != '/' && a[1] != '\\' && !strchr(a + 1, '\\'));
}

// local = our own command line (all options); otherwise a second instance's arguments (file options only).
static int parse_args(int argc, char** argv, const char* base, int local, char* cfg_override, u32 cap,
                      Vec<MainFileArg>* out)
{
  MainFileArg next;
  int i;
  memset(&next, 0, sizeof(next));
  for (i = 0; i < argc; i++) {
    const char* a = argv[i];
    if (!a || !a[0]) continue;
    if (is_option(a)) {
      const char* o = a + 1;
      if (es_icmp(o, "selftest") == 0) {
        while (i + 1 < argc && argv[i + 1] && !is_option(argv[i + 1])) {
          i++;
          if (local) {
            if (g_selftest_names[0]) es_cat(g_selftest_names, sizeof(g_selftest_names), " ");
            es_cat(g_selftest_names, sizeof(g_selftest_names), argv[i]);
          }
        }
        if (local) g_app.selftest = 1;
      }
      else if (es_icmp(o, "cfg") == 0 && i + 1 < argc) { i++; if (local) es_copy(cfg_override, cap, argv[i]); }
      else if (es_icmp(o, "log") == 0 && i + 1 < argc) { i++; if (local) es_copy(g_log_path, sizeof(g_log_path), argv[i]); }
      else if (es_icmp(o, "fixdir") == 0 && i + 1 < argc) { i++; if (local) es_copy(g_fixdir, sizeof(g_fixdir), argv[i]); }
      else if (es_icmp(o, "bench") == 0) { if (local) g_app.bench = 1; }
      else if (es_icmp(o, "multi") == 0) { if (local) g_app.multi = 1; }
      else if (es_icmp(o, "nosession") == 0) { if (local) g_app.nosession = 1; }
      else if (es_icmp(o, "hex") == 0) next.hex = 1;
      else if (es_icmp(o, "ro") == 0) next.ro = 1;
      else if ((o[0] == 'n' || o[0] == 'N') && o[1] >= '0' && o[1] <= '9') {
        u32 line;
        if (es_parse_u32(o + 1, &line, 10) == E_OK) next.line = line;
      }
      // unknown options are ignored
    } else {
      resolve_path(a, base, next.path, sizeof(next.path));
      if (out->push(next) != E_OK) return E_NOMEM;
      memset(&next, 0, sizeof(next));
    }
  }
  return E_OK;
}

static void open_file_args(Vec<MainFileArg>* files)
{
  u32 i;
  for (i = 0; i < files->size(); i++) {
    MainFileArg* f = &(*files)[i];
    OpenOpts o;
    Tab* t = 0;
    int rc;
    docio_default_opts(&o);
    o.hex = f->hex;
    o.read_only = f->ro;
    o.line = f->line;
    rc = docio_open(f->path, &o, &t);
    if (rc < 0) app_status_note("Could not open %s (%s)", f->path, e_errname(rc));
  }
}

// ---- single instance (15.7) ----

int main_parse_block(const char* block, u32 size, Vec<MainFileArg>* out)
{
  Vec<char*> args;
  const char* p;
  const char* end;
  const char* cwd;
  char dummy[4];
  int rc;
  if (!block || size < sizeof(k_block_tag) + 1 || memcmp(block, k_block_tag, sizeof(k_block_tag)) != 0) return E_FORMAT;
  if (block[size - 1] != 0) return E_FORMAT;
  end = block + size;
  p = block + sizeof(k_block_tag);
  cwd = p;
  p += es_len(p) + 1;
  while (p < end && *p) {
    if (args.push((char*)p) != E_OK) return E_NOMEM;
    p += es_len(p) + 1;
  }
  rc = parse_args((int)args.size(), args.data(), cwd, 0, dummy, sizeof(dummy), out);
  return rc;
}

LRESULT main_on_copydata(HWND from, const COPYDATASTRUCT* cds)
{
  char* copy;
  (void)from;
  if (!cds || cds->dwData != k_copydata_magic || cds->cbData < sizeof(k_block_tag) + 1 || cds->cbData > 65536)
    return FALSE;
  if (g_app.bench || g_app.exiting || (g_app.selftest && !g_accept_copydata_st)) return FALSE;
  copy = (char*)e_alloc(cds->cbData);
  if (!copy) return FALSE;
  memcpy(copy, cds->lpData, cds->cbData);
  if (!PostMessageA(g_app.frame, WM_APP_COPYDATA_OPEN, (WPARAM)cds->cbData, (LPARAM)copy)) { e_free(copy); return FALSE; }
  return TRUE;
}

// Files handed over while a modal window (message box, progress, Options) is up wait until the frame is enabled
// again, so a tab never opens underneath a question about another tab.
struct PendingBlock { WPARAM size; char* block; };
static Vec<PendingBlock> g_pending_blocks;
static UINT g_pending_timer = 0;

static void CALLBACK copydata_retry(HWND w, UINT msg, UINT id, DWORD now)
{
  u32 i;
  (void)w; (void)msg; (void)now;
  if (!g_app.frame || !IsWindowEnabled(g_app.frame)) return;     // still modal: try again on the next tick
  KillTimer(0, id);
  g_pending_timer = 0;
  for (i = 0; i < g_pending_blocks.size(); i++)
    if (!PostMessageA(g_app.frame, WM_APP_COPYDATA_OPEN, g_pending_blocks[i].size, (LPARAM)g_pending_blocks[i].block))
      e_free(g_pending_blocks[i].block);
  g_pending_blocks.clear();
}

void main_on_copydata_open(WPARAM w, LPARAM l)
{
  Vec<MainFileArg> files;
  char* block = (char*)l;
  if (!block) return;
  if (g_app.frame && !IsWindowEnabled(g_app.frame) && !g_app.exiting) {
    PendingBlock pb;
    pb.size = w; pb.block = block;
    if (g_pending_blocks.push(pb) == E_OK) {
      if (!g_pending_timer) g_pending_timer = SetTimer(0, 0, 300, copydata_retry);
      if (g_pending_timer) return;
      g_pending_blocks.pop();
    }
  }
  if (main_parse_block(block, (u32)w, &files) == E_OK) open_file_args(&files);
  e_free(block);
  if (IsIconic(g_app.frame)) ShowWindow(g_app.frame, SW_RESTORE);
  SetForegroundWindow(g_app.frame);
}

// Hands our command line to a running Pica. Returns 1 when it took it (this process should exit).
// Explorer starts one "pica.exe" "%1" per selected file, all at about the same time: the first process to create the
// mutex is the main instance; every later one waits (up to 15 s, a slow first start on real hardware) for its frame
// window and hands its files over. WM_COPYDATA is only queued by the main instance (posted to itself), so files arrive
// as tabs in the order the handoffs complete, even while it is still restoring its session.
static int hand_off_to_running()
{
  HANDLE mutex;
  HWND other;
  Vec<char> block;
  char cwd[MAX_PATH];
  COPYDATASTRUCT cds;
  DWORD res = 0, err;
  int i, tries;
  mutex = CreateMutexA(0, FALSE, APP_NAME "SingleInstance");
  err = GetLastError();                                    // before any other call resets it
  other = FindWindowA(APP_CLASS_FRAME, 0);
  if (!other && mutex && err == ERROR_ALREADY_EXISTS) {
    for (tries = 0; tries < 300 && !other; tries++) {      // the first instance is still starting up
      Sleep(50);
      other = FindWindowA(APP_CLASS_FRAME, 0);
    }
  }
  if (!other) return 0;                                    // we are the first: keep the mutex for our lifetime
  if (!GetCurrentDirectoryA(sizeof(cwd), cwd)) cwd[0] = 0;
  if (block.insert(0, k_block_tag, sizeof(k_block_tag)) != E_OK) return 0;
  if (block.insert(block.size(), cwd, es_len(cwd) + 1) != E_OK) return 0;
  for (i = 1; i < __argc; i++)
    if (block.insert(block.size(), __argv[i], es_len(__argv[i]) + 1) != E_OK) return 0;
  if (block.push(0) != E_OK) return 0;
  if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
  SetForegroundWindow(other);                              // we are the foreground process: allowed on Win98
  cds.dwData = k_copydata_magic;
  cds.cbData = block.size();
  cds.lpData = block.data();
  // No SMTO_ABORTIFHUNG: a main instance busy with its startup (session restore, a big file) for a few seconds counts
  // as "hung" to Windows, and giving up would open a second window. Retry once if the window was replaced meanwhile.
  for (tries = 0; tries < 2; tries++) {
    res = 0;
    if (SendMessageTimeoutA(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_NORMAL, 30000, &res) && res == TRUE) return 1;
    Sleep(200);
    other = FindWindowA(APP_CLASS_FRAME, 0);
    if (!other) break;
  }
  return 0;
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

  cfg_override[0] = 0;
  parse_args(__argc - 1, __argv + 1, 0, 1, cfg_override, sizeof(cfg_override), &g_cmd_files);

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

  if (!g_app.selftest && !g_app.bench && !g_app.multi && g_app.cfg.single_instance && hand_off_to_running())
    return 0;

  init_languages();
  if (g_app.selftest) selftest_set_paths(g_log_path, g_fixdir);

  if (!frame_register(hinst) || !frame_create(show)) {
    MessageBoxA(0, "Could not create the main window.", APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
  }
  iothread_start();

  if (g_app.selftest) {
    show_lang_errors();
    rc = selftest_run(g_selftest_names);
  } else if (g_app.bench) {
    rc = bench_run();
    if (rc < 0) rc = 100;
  } else {
    sessmgr_startup();
    open_file_args(&g_cmd_files);
    g_cmd_files.release();
    if (app_tab_count() == 0 && !g_app.exiting) {          // like Notepad++: start with an empty "new 1"
      Tab* t = 0;
      docio_new(&t);
    }
    show_lang_errors();
    rc = run_loop();
  }
  if (IsWindow(g_app.frame)) DestroyWindow(g_app.frame);
  iothread_stop();
  return rc;
}
