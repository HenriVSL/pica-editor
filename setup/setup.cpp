// setup.cpp - Pica installer (T19): command line, install wizard (PropertySheet, PSH_WIZARD), silent install,
// uninstall (asks, then inst.cpp does the work). The same objects link into setup.exe (with the payload) and
// uninst.exe (without: it always uninstalls). Switches: docs/INSTALLER.md.
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string.h>
#include "setup.h"
#include "setup_ids.h"
#include "appinfo.h"

enum { M_FRESH, M_UPGRADE, M_REPAIR, M_DOWNGRADE };
enum { WM_APP_START = WM_APP + 1 };
enum { ID_SHEET_BACK = 0x3023, ID_SHEET_NEXT = 0x3024 };

static HINSTANCE g_inst;
static Opts g_o;
static InstallInfo g_ii;
static DWORD g_pms, g_pls;            // payload version
static char g_pver[32];
static char g_caption[80];
static int g_mode = M_FRESH;
static char g_oldver[32];             // version replaced by this run (upgrade / repair / downgrade)
static int g_rc = RC_FAIL;
static int g_ran;                     // inst_run was called
static int g_cancelled;
static char g_err[700];
static HFONT g_bold;
static int g_do_run, g_do_reboot;

// command-line values applied after the defaults of an existing install are known
static char g_arg_types[400];
static int g_arg_start = -1, g_arg_desk = -1, g_arg_ctx = -1;
static char g_args_raw[1200];
static char g_arg_dir[MAX_PATH];      // /D=         // everything after the program name (passed on to the uninstall copy)

// ---- small helpers ----

static int starts_i(const char* s, const char* prefix)
{
  int n = lstrlenA(prefix);
  return lstrlenA(s) >= n && CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, s, n, prefix, n) == 2;
}

static int msg(HWND owner, const char* text, UINT flags)
{
  return MessageBoxA(owner, text, g_caption, flags | MB_SETFOREGROUND);
}

static void launch_pica(const char* dir)
{
  char exe[MAX_PATH], cmd[MAX_PATH + 4];
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  p_join(dir, APP_EXE, exe, sizeof(exe));
  s_copy(cmd, sizeof(cmd), "\"");
  s_cat(cmd, sizeof(cmd), exe);
  s_cat(cmd, sizeof(cmd), "\"");
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (CreateProcessA(exe, cmd, 0, 0, FALSE, 0, 0, dir, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    slog("started %s", exe);
  } else {
    slog("error: cannot start %s (%lu)", exe, GetLastError());
  }
}

static const char* usage_text()
{
  return "Pica setup switches:\n\n"
         "/S\tsilent install with the defaults (or the choices of the installed copy)\n"
         "/D=<folder>\tinstall folder (last switch, no quotes needed)\n"
         "/TYPES=<list>\tfile types: default, none, all, .txt,.c or +.bat,-.ini\n"
         "/STARTMENU=0|1  /DESKTOP=0|1  /CONTEXT=0|1\n"
         "/RUN\tstart Pica when done\n"
         "/FORCE\tallow a downgrade in silent mode\n"
         "/LOG=<file>\tlog file (default %TEMP%\\PICASET.LOG)\n"
         "/UNINSTALL [/S] [/PURGE]\tremove Pica (/PURGE also deletes settings, session and backups)\n\n"
         "Exit codes: 0 ok, 1 error or cancelled, 2 Pica is running, 3 restart needed, 4 newer version installed.";
}

// ---- command line ----

static int parse_args(char* bad, int cap)
{
  const char* cl = GetCommandLineA();
  char tok[MAX_PATH * 2];
  bad[0] = 0;
  if (*cl == '"') {
    cl++;
    while (*cl && *cl != '"') cl++;
    if (*cl) cl++;
  } else {
    while (*cl && *cl != ' ' && *cl != '\t') cl++;
  }
  while (*cl == ' ' || *cl == '\t') cl++;
  s_copy(g_args_raw, sizeof(g_args_raw), cl);
  for (;;) {
    int n = 0, q = 0;
    while (*cl == ' ' || *cl == '\t') cl++;
    if (!*cl) break;
    if (starts_i(cl, "/D=") || starts_i(cl, "\"/D=")) {        // NSIS style: the rest of the line is the folder
      const char* d = cl + (*cl == '"' ? 4 : 3);
      if (!p_normalize_dir(d, g_arg_dir, sizeof(g_arg_dir))) { s_copy(bad, cap, cl); return 0; }
      g_o.dir_given = 1;
      break;
    }
    while (*cl && (q || (*cl != ' ' && *cl != '\t'))) {
      if (*cl == '"') q = !q;
      else if (n < (int)sizeof(tok) - 1) tok[n++] = *cl;
      cl++;
    }
    tok[n] = 0;
    if (tok[0] == '-') tok[0] = '/';
    if (!lstrcmpiA(tok, "/S")) g_o.silent = 1;
    else if (!lstrcmpiA(tok, "/UNINSTALL")) g_o.uninstall = 1;
    else if (!lstrcmpiA(tok, "/PURGE")) g_o.purge = 1;
    else if (!lstrcmpiA(tok, "/FORCE")) g_o.force = 1;
    else if (!lstrcmpiA(tok, "/RUN")) g_o.run_after = 1;
    else if (!lstrcmpiA(tok, "/_LOCKTEST")) g_o.locktest = 1;
    else if (starts_i(tok, "/LOG=")) s_copy(g_o.log, sizeof(g_o.log), tok + 5);
    else if (starts_i(tok, "/TYPES=")) s_copy(g_arg_types, sizeof(g_arg_types), tok + 7);
    else if (starts_i(tok, "/STARTMENU=")) g_arg_start = tok[11] == '1';
    else if (starts_i(tok, "/DESKTOP=")) g_arg_desk = tok[9] == '1';
    else if (starts_i(tok, "/CONTEXT=")) g_arg_ctx = tok[9] == '1';
    else if (starts_i(tok, "/_CHILD=")) {
      const char* p = tok + 8;
      while (*p >= '0' && *p <= '9') g_o.child_pid = g_o.child_pid * 10 + (DWORD)(*p++ - '0');
    } else { s_copy(bad, cap, tok); return 0; }
  }
  return 1;
}

static int find_ext(const char* e)
{
  int i;
  char want[16];
  if (*e != '.') { s_copy(want, sizeof(want), "."); s_cat(want, sizeof(want), e); }
  else s_copy(want, sizeof(want), e);
  for (i = 0; i < g_next; i++) if (!lstrcmpiA(g_exts[i].ext, want)) return i;
  return -1;
}

// /TYPES=default|none|all|<list>; list items with + or - change the stored/default set, plain items replace it
static int apply_types(const char* spec, char* bad, int cap)
{
  char item[32];
  const char* s = spec;
  int i, relative = 1, first = 1;
  if (!spec[0]) return 1;
  if (!lstrcmpiA(spec, "default")) { opts_default_types(&g_o); return 1; }
  if (!lstrcmpiA(spec, "none") || !lstrcmpiA(spec, "all")) {
    for (i = 0; i < g_next; i++) g_o.types[i] = spec[0] == 'a' || spec[0] == 'A';
    return 1;
  }
  while (*s) {
    int n = 0, idx, sign = 0;
    while (*s == ',' || *s == ';') s++;
    if (!*s) break;
    while (*s && *s != ',' && *s != ';' && n < (int)sizeof(item) - 1) item[n++] = *s++;
    item[n] = 0;
    if (item[0] == '+') sign = 1;
    else if (item[0] == '-') sign = -1;
    if (first && !sign) { relative = 0; for (i = 0; i < g_next; i++) g_o.types[i] = 0; }
    first = 0;
    idx = find_ext(sign ? item + 1 : item);
    if (idx < 0) { s_copy(bad, cap, item); return 0; }
    g_o.types[idx] = sign >= 0 ? 1 : 0;
  }
  (void)relative;
  return 1;
}

static int compute_mode(const char* dir, char* oldver)
{
  DWORD ms, ls;
  char exe[MAX_PATH];
  int c;
  oldver[0] = 0;
  if (g_ii.installed && g_ii.have_ver && p_same(dir, g_ii.dir)) { ms = g_ii.ms; ls = g_ii.ls; }
  else {
    p_join(dir, APP_EXE, exe, sizeof(exe));
    if (!file_version(exe, &ms, &ls)) return g_ii.installed && p_same(dir, g_ii.dir) ? M_REPAIR : M_FRESH;
  }
  ver_str(ms, ls, oldver);
  c = ver_cmp(ms, ls, g_pms, g_pls);
  return c < 0 ? M_UPGRADE : c == 0 ? M_REPAIR : M_DOWNGRADE;
}

// ---- uninstall ----

static int uninstall_spawn_child()
{
  char tmpdir[MAX_PATH], tmp[MAX_PATH], self[MAX_PATH], cmd[MAX_PATH + 1400], name[64];
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  HANDLE map, ev, wait[2];
  DWORD* rcp;
  int rc = RC_FAIL;
  GetTempPathA(sizeof(tmpdir), tmpdir);
  p_join(tmpdir, "PICAUN.EXE", tmp, sizeof(tmp));
  GetModuleFileNameA(0, self, sizeof(self));
  SetFileAttributesA(tmp, FILE_ATTRIBUTE_NORMAL);
  if (!CopyFileA(self, tmp, FALSE)) { slog("cannot copy the uninstaller to %s", tmp); return -1; }
  wsprintfA(name, "PicaUninstRc%lu", GetCurrentProcessId());
  map = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, 4, name);
  rcp = map ? (DWORD*)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 4) : 0;
  wsprintfA(name, "PicaUninstDone%lu", GetCurrentProcessId());
  ev = CreateEventA(0, TRUE, FALSE, name);
  if (!rcp || !ev) { slog("cannot create the uninstall handshake"); return -1; }
  *rcp = RC_FAIL;
  wsprintfA(cmd, "\"%s\" /UNINSTALL /_CHILD=%lu ", tmp, GetCurrentProcessId());
  s_cat(cmd, sizeof(cmd), g_args_raw);
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  slog("running the uninstaller from %s", tmp);
  log_close();
  if (!CreateProcessA(tmp, cmd, 0, 0, FALSE, 0, 0, tmpdir, &si, &pi)) {
    log_open(g_o.log, 0);
    slog("cannot start %s (%lu)", tmp, GetLastError());
    return -1;
  }
  CloseHandle(pi.hThread);
  wait[0] = ev;
  wait[1] = pi.hProcess;
  WaitForMultipleObjects(2, wait, FALSE, INFINITE);
  rc = (int)*rcp;
  CloseHandle(pi.hProcess);
  UnmapViewOfFile(rcp);
  CloseHandle(map);
  CloseHandle(ev);
  return rc;
}

static void uninstall_report(int rc)
{
  char name[64];
  HANDLE map, ev;
  DWORD* rcp;
  wsprintfA(name, "PicaUninstRc%lu", g_o.child_pid);
  map = OpenFileMappingA(FILE_MAP_WRITE, FALSE, name);
  rcp = map ? (DWORD*)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 4) : 0;
  if (rcp) { *rcp = (DWORD)rc; UnmapViewOfFile(rcp); }
  if (map) CloseHandle(map);
  wsprintfA(name, "PicaUninstDone%lu", g_o.child_pid);
  ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
  if (ev) { SetEvent(ev); CloseHandle(ev); }
}

static int uninstall_main()
{
  char dir[MAX_PATH], mdir[MAX_PATH], text[900], exe[MAX_PATH], self[MAX_PATH];
  int purge = g_o.purge, kept = 0, rc, child = g_o.child_pid != 0;
  wsprintfA(g_caption, "%s Uninstall", APP_NAME);
  detect_install(&g_ii);
  p_module_dir(mdir, sizeof(mdir));
  if (g_ii.installed) s_copy(dir, sizeof(dir), g_ii.dir);
  else {
    p_join(mdir, APP_EXE, exe, sizeof(exe));
    if (!child && p_exists(exe)) s_copy(dir, sizeof(dir), mdir);     // registry lost: clean the files only
    else {
      slog("Pica is not installed");
      if (!g_o.silent) msg(0, APP_NAME " is not installed on this computer.", MB_OK | MB_ICONINFORMATION);
      if (child) uninstall_report(RC_FAIL);
      return RC_FAIL;
    }
  }
  slog("uninstall: installed %d, folder %s, version %s, child of %08lX", g_ii.installed, dir, g_ii.ver, g_o.child_pid);

  // the installed uninst.exe can't delete itself: a temp copy does the work while this one waits
  if (!child && p_same(mdir, dir)) {
    rc = uninstall_spawn_child();
    if (rc >= 0) return rc;
    slog("continuing in place (uninst.exe is deleted when Windows restarts)");
  }

  if (!g_o.silent) {
    wsprintfA(text, "Remove %s %s from %s?", APP_NAME, g_ii.installed ? g_ii.ver : "", dir);
    if (msg(0, text, MB_YESNO | MB_ICONQUESTION) != IDYES) {
      slog("cancelled");
      if (child) uninstall_report(RC_FAIL);
      return RC_FAIL;
    }
  }
  while (pica_running()) {
    slog("Pica is running");
    if (g_o.silent ||
        msg(0, APP_NAME " is running. Please close it (your open files and unsaved changes are kept for the next "
               "session), then click Retry.\n\nCancel stops without removing anything.",
            MB_RETRYCANCEL | MB_ICONEXCLAMATION) != IDRETRY) {
      if (child) uninstall_report(g_o.silent ? RC_RUNNING : RC_FAIL);
      return g_o.silent ? RC_RUNNING : RC_FAIL;
    }
  }
  if (!g_o.silent && settings_exist(dir)) {
    purge = msg(0, "Also delete your " APP_NAME " settings, the saved session and the backups of unsaved changes?\n\n"
                   "Choose No to keep them: " APP_NAME " finds them again if you reinstall it into the same folder.",
                MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES;
  }
  rc = uninst_run(dir, purge, child, &kept);
  kept = !purge && settings_exist(dir);
  if (!g_o.silent) {
    wsprintfA(text, "%s was removed from your computer.", APP_NAME);
    if (kept) {
      s_cat(text, sizeof(text), "\n\nYour settings, session and backups were kept in ");
      s_cat(text, sizeof(text), dir);
      s_cat(text, sizeof(text), ".");
    }
    if (rc == RC_REBOOT) s_cat(text, sizeof(text), "\n\nSome files were in use: restart Windows to remove them.");
    msg(0, text, MB_OK | MB_ICONINFORMATION);
  }
  if (child) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, g_o.child_pid);
    uninstall_report(rc);
    if (parent) {
      WaitForSingleObject(parent, 15000);
      CloseHandle(parent);
    }
    if (uninst_finish(dir) == RC_REBOOT) slog("uninst.exe is deleted when Windows restarts");
    GetModuleFileNameA(0, self, sizeof(self));
    wininit_delete_later(self);              // this temp copy goes at the next restart
  }
  return rc;
}

// ---- wizard ----

static void page_init(HWND d)
{
  if (!g_bold) {
    LOGFONTA lf;
    HFONT f = (HFONT)SendMessageA(d, WM_GETFONT, 0, 0);
    if (f && GetObjectA(f, sizeof(lf), &lf)) {
      lf.lfWeight = FW_BOLD;
      g_bold = CreateFontIndirectA(&lf);
    }
  }
  if (g_bold && GetDlgItem(d, IDC_TITLE)) SendDlgItemMessageA(d, IDC_TITLE, WM_SETFONT, (WPARAM)g_bold, 0);
}

static void set_result(HWND d, LONG v)
{
  SetWindowLongA(d, DWL_MSGRESULT, v);
}

static int query_cancel(HWND d)
{
  if (msg(GetParent(d), "Quit " APP_NAME " Setup? Nothing has been changed.", MB_YESNO | MB_ICONQUESTION) == IDYES) {
    g_cancelled = 1;
    slog("cancelled in the wizard");
    return 0;
  }
  return 1;
}

static int ask_downgrade(HWND owner, const char* oldver)
{
  char text[300];
  wsprintfA(text, "A newer %s (%s) is installed. Replace it with the older version %s?", APP_NAME, oldver, g_pver);
  return msg(owner, text, MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES;
}

static BOOL CALLBACK welcome_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
  char text[600];
  (void)wp;
  switch (m) {
  case WM_INITDIALOG:
    page_init(d);
    if (g_mode == M_UPGRADE) wsprintfA(text, "Upgrade %s %s -> %s", APP_NAME, g_oldver, g_pver);
    else if (g_mode == M_REPAIR) wsprintfA(text, "Reinstall %s %s", APP_NAME, g_pver);
    else if (g_mode == M_DOWNGRADE) wsprintfA(text, "Replace %s %s with %s", APP_NAME, g_oldver, g_pver);
    else wsprintfA(text, "Welcome to %s %s Setup", APP_NAME, g_pver);
    SetDlgItemTextA(d, IDC_TITLE, text);
    text[0] = 0;
    if (g_mode == M_UPGRADE)
      wsprintfA(text, "%s %s is installed in %s. Setup upgrades it to %s and keeps your settings, session and "
                      "backups.", APP_NAME, g_oldver, g_ii.dir, g_pver);
    else if (g_mode == M_REPAIR)
      wsprintfA(text, "%s %s is already installed in %s. Setup copies the program files again (repair) and keeps your "
                      "settings, session and backups.", APP_NAME, g_pver, g_ii.dir);
    else if (g_mode == M_DOWNGRADE)
      wsprintfA(text, "A newer %s (%s) is installed in %s. Setup can replace it with the older %s; your settings are "
                      "kept.", APP_NAME, g_oldver, g_ii.dir, g_pver);
    else
      wsprintfA(text, "This wizard installs %s on your computer.", APP_NAME);
    SetDlgItemTextA(d, IDC_W_STATUS, text);
    return TRUE;
  case WM_NOTIFY:
    switch (((NMHDR*)lp)->code) {
    case PSN_SETACTIVE: PropSheet_SetWizButtons(GetParent(d), PSWIZB_NEXT); set_result(d, 0); return TRUE;
    case PSN_WIZNEXT:
      set_result(d, (g_mode == M_DOWNGRADE && !ask_downgrade(GetParent(d), g_oldver)) ? -1 : 0);
      return TRUE;
    case PSN_QUERYCANCEL: set_result(d, query_cancel(d)); return TRUE;
    }
    break;
  }
  return FALSE;
}

static int CALLBACK browse_cb(HWND w, UINT m, LPARAM lp, LPARAM data)
{
  (void)lp;
  if (m == BFFM_INITIALIZED && data) SendMessageA(w, BFFM_SETSELECTION, TRUE, data);
  return 0;
}

static void browse(HWND d)
{
  BROWSEINFOA bi;
  char cur[MAX_PATH], start[MAX_PATH], disp[MAX_PATH], path[MAX_PATH];
  LPITEMIDLIST pidl;
  LPMALLOC mal = 0;
  int n;
  char* slash;
  GetDlgItemTextA(d, IDC_F_DIR, cur, sizeof(cur));
  s_copy(start, sizeof(start), cur);
  while (start[0] && !p_is_dir(start) && (slash = strrchr(start, '\\')) != 0) *slash = 0;   // nearest existing parent
  memset(&bi, 0, sizeof(bi));
  bi.hwndOwner = GetParent(d);
  bi.pszDisplayName = disp;
  bi.lpszTitle = "Choose the folder for " APP_NAME ". Setup adds \\" APP_NAME " unless the folder already ends with it.";
  bi.ulFlags = BIF_RETURNONLYFSDIRS;
  bi.lpfn = browse_cb;
  bi.lParam = (LPARAM)start;
  pidl = SHBrowseForFolderA(&bi);
  if (!pidl) return;
  if (SHGetPathFromIDListA(pidl, path) && path[0]) {
    n = lstrlenA(path);
    if (n > 0 && path[n - 1] == '\\') path[--n] = 0;
    if (n < 5 || lstrcmpiA(path + n - 5, "\\" APP_NAME) != 0) s_cat(path, sizeof(path), "\\" APP_NAME);
    SetDlgItemTextA(d, IDC_F_DIR, path);
  }
  if (SHGetMalloc(&mal) == NOERROR && mal) { mal->Free(pidl); mal->Release(); }
}

static BOOL CALLBACK folder_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
  char text[600], dir[MAX_PATH];
  switch (m) {
  case WM_INITDIALOG:
    page_init(d);
    SetDlgItemTextA(d, IDC_F_DIR, g_o.dir);
    CheckDlgButton(d, IDC_F_START, g_o.startmenu ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_F_DESK, g_o.desktop ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_F_CTX, g_o.context ? BST_CHECKED : BST_UNCHECKED);
    if (g_ii.installed) {
      SendDlgItemMessageA(d, IDC_F_DIR, EM_SETREADONLY, TRUE, 0);
      EnableWindow(GetDlgItem(d, IDC_F_BROWSE), FALSE);
      wsprintfA(text, "%s is installed in this folder. To move it, uninstall it first. Only the program files are "
                      "replaced: settings, session and backups in this folder stay.", APP_NAME);
    } else {
      wsprintfA(text, "%s keeps its settings, session and backups of unsaved changes in this folder.", APP_NAME);
    }
    SetDlgItemTextA(d, IDC_F_NOTE, text);
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(wp) == IDC_F_BROWSE) browse(d);
    break;
  case WM_NOTIFY:
    switch (((NMHDR*)lp)->code) {
    case PSN_SETACTIVE: PropSheet_SetWizButtons(GetParent(d), PSWIZB_BACK | PSWIZB_NEXT); set_result(d, 0); return TRUE;
    case PSN_WIZNEXT:
      GetDlgItemTextA(d, IDC_F_DIR, text, sizeof(text));
      if (!p_normalize_dir(text, dir, sizeof(dir))) {
        msg(GetParent(d), "Please enter a full folder path, for example C:\\Program Files\\" APP_NAME ".",
            MB_OK | MB_ICONEXCLAMATION);
        set_result(d, -1);
        return TRUE;
      }
      if (!g_ii.installed) {
        int mode = compute_mode(dir, g_oldver);
        if (mode == M_DOWNGRADE && !ask_downgrade(GetParent(d), g_oldver)) { set_result(d, -1); return TRUE; }
        g_mode = mode;
      }
      s_copy(g_o.dir, sizeof(g_o.dir), dir);
      SetDlgItemTextA(d, IDC_F_DIR, dir);
      g_o.startmenu = IsDlgButtonChecked(d, IDC_F_START) == BST_CHECKED;
      g_o.desktop = IsDlgButtonChecked(d, IDC_F_DESK) == BST_CHECKED;
      g_o.context = IsDlgButtonChecked(d, IDC_F_CTX) == BST_CHECKED;
      set_result(d, 0);
      return TRUE;
    case PSN_QUERYCANCEL: set_result(d, query_cancel(d)); return TRUE;
    }
    break;
  }
  return FALSE;
}

static HWND make_ctl(HWND d, HFONT f, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
  RECT r;
  HWND c;
  r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
  MapDialogRect(d, &r);
  c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left, r.bottom - r.top, d,
                      (HMENU)id, g_inst, 0);
  if (c) SendMessageA(c, WM_SETFONT, (WPARAM)f, 0);
  return c;
}

// groups of the File types page: x, y, width, title, note (dialog units; page is 317 x 160)
static void types_create(HWND d)
{
  static const struct { int group, x, y, w; const char* title; const char* note; } gl[G_COUNT] = {
    { G_TEXT, 7, 30, 166, "Text files", 0 },
    { G_WEB, 178, 30, 132, "Web pages (now in your browser)", 0 },
    { G_SRC, 7, 59, 303, "Source code", 0 },
    { G_SCRIPT, 7, 100, 303, "Scripts and setup files",
      "Double-click still runs these; " APP_NAME " becomes their Edit command." }
  };
  HFONT f = (HFONT)SendMessageA(d, WM_GETFONT, 0, 0);
  int g, i;
  for (g = 0; g < G_COUNT; g++) {
    int cols = (gl[g].w - 10) / 38, k = 0, rows, top = gl[g].y + 11;
    for (i = 0; i < g_next; i++) if (g_exts[i].group == gl[g].group) k++;
    rows = (k + cols - 1) / cols;
    if (gl[g].note) {
      make_ctl(d, f, "STATIC", gl[g].note, SS_LEFT, gl[g].x + 7, top, gl[g].w - 14, 8, -1);
      top += 10;
    }
    make_ctl(d, f, "BUTTON", gl[g].title, BS_GROUPBOX, gl[g].x, gl[g].y, gl[g].w, top - gl[g].y + rows * 12 + 2,
             IDC_T_GROUP + g);
    k = 0;
    for (i = 0; i < g_next; i++) {
      if (g_exts[i].group != gl[g].group) continue;
      make_ctl(d, f, "BUTTON", g_exts[i].ext, BS_AUTOCHECKBOX | WS_TABSTOP, gl[g].x + 7 + (k % cols) * 38,
               top + (k / cols) * 12, 36, 10, IDC_T_FIRST + i);
      k++;
    }
  }
}

static void types_show(HWND d)
{
  int i;
  for (i = 0; i < g_next; i++) CheckDlgButton(d, IDC_T_FIRST + i, g_o.types[i] ? BST_CHECKED : BST_UNCHECKED);
}

static BOOL CALLBACK types_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
  int i;
  switch (m) {
  case WM_INITDIALOG:
    page_init(d);
    types_create(d);
    types_show(d);
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(wp) == IDC_T_DEFAULTS) {
      for (i = 0; i < g_next; i++) CheckDlgButton(d, IDC_T_FIRST + i, g_exts[i].def ? BST_CHECKED : BST_UNCHECKED);
    } else if (LOWORD(wp) == IDC_T_NONE) {
      for (i = 0; i < g_next; i++) CheckDlgButton(d, IDC_T_FIRST + i, BST_UNCHECKED);
    }
    break;
  case WM_NOTIFY:
    switch (((NMHDR*)lp)->code) {
    case PSN_SETACTIVE:
      PropSheet_SetWizButtons(GetParent(d), PSWIZB_BACK | PSWIZB_NEXT);
      SetDlgItemTextA(GetParent(d), ID_SHEET_NEXT, "&Install");
      set_result(d, 0);
      return TRUE;
    case PSN_KILLACTIVE:
      SetDlgItemTextA(GetParent(d), ID_SHEET_NEXT, "&Next >");
      set_result(d, 0);
      return TRUE;
    case PSN_WIZNEXT:
      for (i = 0; i < g_next; i++) g_o.types[i] = IsDlgButtonChecked(d, IDC_T_FIRST + i) == BST_CHECKED;
      set_result(d, 0);
      return TRUE;
    case PSN_QUERYCANCEL: set_result(d, query_cancel(d)); return TRUE;
    }
    break;
  }
  return FALSE;
}

static void progress_cb(int pct, const char* text, void* ctx)
{
  HWND d = (HWND)ctx;
  SendDlgItemMessageA(d, IDC_P_BAR, PBM_SETPOS, (WPARAM)pct, 0);
  SetDlgItemTextA(d, IDC_P_TEXT, text);
  UpdateWindow(d);
}

static BOOL CALLBACK progress_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
  (void)wp;
  switch (m) {
  case WM_INITDIALOG:
    page_init(d);
    SendDlgItemMessageA(d, IDC_P_BAR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    return TRUE;
  case WM_APP_START:
    while (pica_running()) {
      slog("Pica is running");
      if (msg(GetParent(d), APP_NAME " is running. Please close it (your open files and unsaved changes are kept for "
                            "the next session), then click Retry.\n\nCancel stops Setup without changing anything.",
              MB_RETRYCANCEL | MB_ICONEXCLAMATION) != IDRETRY) {
        g_cancelled = 1;
        g_rc = RC_FAIL;
        slog("cancelled: Pica is running");
        break;
      }
    }
    if (!g_cancelled) {
      g_ran = 1;
      g_rc = inst_run(&g_o, progress_cb, d, g_err, sizeof(g_err));
    }
    PropSheet_SetWizButtons(GetParent(d), PSWIZB_NEXT);
    PropSheet_PressButton(GetParent(d), PSBTN_NEXT);
    return TRUE;
  case WM_NOTIFY:
    switch (((NMHDR*)lp)->code) {
    case PSN_SETACTIVE:
      PropSheet_SetWizButtons(GetParent(d), 0);
      EnableWindow(GetDlgItem(GetParent(d), IDCANCEL), FALSE);
      PostMessageA(d, WM_APP_START, 0, 0);
      set_result(d, 0);
      return TRUE;
    case PSN_QUERYCANCEL: set_result(d, TRUE); return TRUE;    // no cancel while files are copied
    }
    break;
  }
  return FALSE;
}

static BOOL CALLBACK finish_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
  char title[200], text[1200];
  (void)wp;
  switch (m) {
  case WM_INITDIALOG:
    page_init(d);
    return TRUE;
  case WM_NOTIFY:
    switch (((NMHDR*)lp)->code) {
    case PSN_SETACTIVE:
      PropSheet_SetWizButtons(GetParent(d), PSWIZB_FINISH);
      EnableWindow(GetDlgItem(GetParent(d), IDCANCEL), FALSE);
      ShowWindow(GetDlgItem(d, IDC_D_RUN), SW_HIDE);
      ShowWindow(GetDlgItem(d, IDC_D_REBOOT), SW_HIDE);
      if (g_cancelled) {
        s_copy(title, sizeof(title), "Setup was cancelled");
        s_copy(text, sizeof(text), "Nothing was changed. Run Setup again when " APP_NAME " is closed.");
      } else if (g_rc == RC_OK || g_rc == RC_REBOOT) {
        if (g_mode == M_UPGRADE) wsprintfA(title, "%s was upgraded to %s", APP_NAME, g_pver);
        else wsprintfA(title, "%s %s is installed", APP_NAME, g_pver);
        wsprintfA(text, "%s is in %s.", APP_NAME, g_o.dir);
        if (g_rc == RC_REBOOT) {
          s_cat(text, sizeof(text), "\n\nSome files were in use. Windows replaces them when it restarts; start "
                                    APP_NAME " after the restart.");
          ShowWindow(GetDlgItem(d, IDC_D_REBOOT), SW_SHOW);
          CheckDlgButton(d, IDC_D_REBOOT, BST_CHECKED);
        } else {
          if (g_o.context) s_cat(text, sizeof(text), "\n\nRight-click any file and choose \"Open with " APP_NAME "\".");
          ShowWindow(GetDlgItem(d, IDC_D_RUN), SW_SHOW);
          CheckDlgButton(d, IDC_D_RUN, BST_CHECKED);
        }
      } else {
        s_copy(title, sizeof(title), "Setup could not finish");
        s_copy(text, sizeof(text), g_err[0] ? g_err : "An error occurred.");
        s_cat(text, sizeof(text), "\n\nDetails are in the log: ");
        s_cat(text, sizeof(text), g_o.log);
      }
      SetDlgItemTextA(d, IDC_TITLE, title);
      SetDlgItemTextA(d, IDC_D_TEXT, text);
      set_result(d, 0);
      return TRUE;
    case PSN_WIZFINISH:
      g_do_run = IsWindowVisible(GetDlgItem(d, IDC_D_RUN)) && IsDlgButtonChecked(d, IDC_D_RUN) == BST_CHECKED;
      g_do_reboot = IsWindowVisible(GetDlgItem(d, IDC_D_REBOOT)) && IsDlgButtonChecked(d, IDC_D_REBOOT) == BST_CHECKED;
      set_result(d, 0);
      return TRUE;
    }
    break;
  }
  return FALSE;
}

static int run_wizard()
{
  static const struct { int id; DLGPROC proc; } pg[] = {
    { IDD_WELCOME, welcome_proc }, { IDD_FOLDER, folder_proc }, { IDD_TYPES, types_proc },
    { IDD_PROGRESS, progress_proc }, { IDD_FINISH, finish_proc }
  };
  enum { NPG = sizeof(pg) / sizeof(pg[0]) };
  PROPSHEETPAGEA psp[NPG];
  HPROPSHEETPAGE hp[NPG];
  PROPSHEETHEADERA psh;
  int i;
  for (i = 0; i < NPG; i++) {
    memset(&psp[i], 0, sizeof(psp[i]));
    psp[i].dwSize = sizeof(psp[i]);
    psp[i].dwFlags = PSP_USETITLE;
    psp[i].hInstance = g_inst;
    psp[i].pszTemplate = MAKEINTRESOURCE(pg[i].id);
    psp[i].pfnDlgProc = pg[i].proc;
    psp[i].pszTitle = g_caption;
    hp[i] = CreatePropertySheetPageA(&psp[i]);
    if (!hp[i]) { slog("error: CreatePropertySheetPage %d failed", i); return RC_FAIL; }
  }
  memset(&psh, 0, sizeof(psh));
  psh.dwSize = sizeof(psh);
  psh.dwFlags = PSH_WIZARD | PSH_USEHICON;
  psh.hInstance = g_inst;
  psh.hIcon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCE(IDI_SETUP), IMAGE_ICON, 16, 16, 0);
  psh.pszCaption = g_caption;
  psh.nPages = NPG;
  psh.phpage = hp;
  if (PropertySheetA(&psh) < 0) slog("error: PropertySheet failed (%lu)", GetLastError());
  if (g_bold) DeleteObject(g_bold);
  if (!g_ran) return RC_FAIL;
  if (g_do_reboot) {
    slog("restarting Windows");
    log_close();
    ExitWindowsEx(EWX_REBOOT, 0);
  } else if (g_do_run) {
    launch_pica(g_o.dir);
  }
  return g_rc;
}

static int install_main()
{
  char bad[300], text[600];
  int rc;
  detect_install(&g_ii);
  opts_defaults(&g_o, &g_ii);
  if (!payload_version(&g_pms, &g_pls)) {
    slog("error: payload has no version");
    if (!g_o.silent) msg(0, "This setup.exe is damaged.", MB_OK | MB_ICONSTOP);
    return RC_FAIL;
  }
  ver_str(g_pms, g_pls, g_pver);
  wsprintfA(g_caption, "%s %s Setup", APP_NAME, g_pver);
  slog("payload %s; installed %d (%s, version %s)", g_pver, g_ii.installed, g_ii.dir, g_ii.ver);
  if (g_arg_start >= 0) g_o.startmenu = g_arg_start;
  if (g_arg_desk >= 0) g_o.desktop = g_arg_desk;
  if (g_arg_ctx >= 0) g_o.context = g_arg_ctx;
  if (!apply_types(g_arg_types, bad, sizeof(bad))) {
    slog("error: unknown file type %s", bad);
    if (!g_o.silent) { wsprintfA(text, "Unknown file type in /TYPES: %s", bad); msg(0, text, MB_OK | MB_ICONSTOP); }
    return RC_FAIL;
  }
  if (g_o.dir_given) s_copy(g_o.dir, sizeof(g_o.dir), g_arg_dir);
  if (g_o.dir_given && g_ii.installed && !p_same(g_o.dir, g_ii.dir)) {
    slog("error: installed in %s, /D asks for another folder", g_ii.dir);
    wsprintfA(text, "%s is already installed in %s. Uninstall it first to install it into another folder.", APP_NAME,
              g_ii.dir);
    if (!g_o.silent) msg(0, text, MB_OK | MB_ICONSTOP);
    return RC_FAIL;
  }
  if (g_ii.installed) s_copy(g_o.dir, sizeof(g_o.dir), g_ii.dir);
  g_mode = compute_mode(g_o.dir, g_oldver);
  slog("mode %s%s%s", g_mode == M_UPGRADE ? "upgrade from " : g_mode == M_REPAIR ? "repair" :
       g_mode == M_DOWNGRADE ? "downgrade from " : "fresh install", g_mode == M_UPGRADE || g_mode == M_DOWNGRADE ?
       g_oldver : "", "");
  if (!g_o.silent) return run_wizard();

  if (g_mode == M_DOWNGRADE && !g_o.force) {
    slog("refused: %s is newer than %s (use /FORCE)", g_oldver, g_pver);
    return RC_DOWNGRADE;
  }
  if (pica_running()) {
    slog("refused: Pica is running (close it and run setup again)");
    return RC_RUNNING;
  }
  rc = inst_run(&g_o, 0, 0, g_err, sizeof(g_err));
  if (rc == RC_FAIL) slog("error: %s", g_err);
  if (rc == RC_OK && g_o.run_after) launch_pica(g_o.dir);
  return rc;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
  char bad[300], text[700];
  HANDLE mutex;
  int ok, rc;
  (void)prev; (void)cmdline; (void)show;
  g_inst = inst;
  InitCommonControls();
  ok = parse_args(bad, sizeof(bad));
  if (!g_o.log[0]) {
    GetTempPathA(sizeof(g_o.log), g_o.log);
    p_join(g_o.log, "PICASET.LOG", g_o.log, sizeof(g_o.log));
  }
  log_open(g_o.log, g_o.child_pid == 0);
  slog("command line: %s", g_args_raw);
  wsprintfA(g_caption, "%s Setup", APP_NAME);
  if (!ok) {
    slog("error: unknown switch %s", bad);
    if (!g_o.silent) {
      wsprintfA(text, "Unknown switch: %s\n\n", bad);
      s_cat(text, sizeof(text), usage_text());
      msg(0, text, MB_OK | MB_ICONINFORMATION);
    }
    log_close();
    return RC_FAIL;
  }
  mutex = CreateMutexA(0, FALSE, "PicaSetupRunning");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS && !g_o.child_pid) {
    slog("error: another setup is running");
    if (!g_o.silent) msg(0, APP_NAME " Setup is already running.", MB_OK | MB_ICONINFORMATION);
    log_close();
    return RC_FAIL;
  }
  CoInitialize(0);
  if (g_o.uninstall || !has_payload()) rc = uninstall_main();
  else rc = install_main();
  CoUninitialize();
  slog("exit code %d", rc);
  log_close();
  if (mutex) CloseHandle(mutex);
  return rc;
}
