// inst.cpp - Pica installer operations (T19): payload, program files, WININIT.INI, registry, file associations,
// "Open with Pica", shortcuts, uninstall. No UI here (setup.cpp asks the questions). Every step is logged.
// Registry layout: docs/INSTALLER.md. Rule: never change anything we can't put back exactly.
#include <windows.h>
#include <shlobj.h>
#include <string.h>
#include <stdarg.h>
#include "setup.h"
#include "setup_ids.h"
#include "appinfo.h"

// ---- tables ----

const ExtDef g_exts[] = {
  { ".txt", G_TEXT, 1 }, { ".log", G_TEXT, 1 }, { ".ini", G_TEXT, 1 }, { ".nfo", G_TEXT, 0 },
  { ".htm", G_WEB, 0 }, { ".html", G_WEB, 0 }, { ".xml", G_WEB, 0 },
  { ".c", G_SRC, 1 }, { ".cpp", G_SRC, 1 }, { ".h", G_SRC, 1 }, { ".hpp", G_SRC, 0 }, { ".rc", G_SRC, 0 },
  { ".asm", G_SRC, 1 }, { ".py", G_SRC, 1 }, { ".css", G_SRC, 0 }, { ".md", G_SRC, 1 }, { ".mak", G_SRC, 1 },
  { ".diff", G_SRC, 0 },
  { ".bat", G_SCRIPT, 0 }, { ".cmd", G_SCRIPT, 0 }, { ".reg", G_SCRIPT, 0 }, { ".inf", G_SCRIPT, 0 },
  { ".vbs", G_SCRIPT, 0 }, { ".js", G_SCRIPT, 0 }
};
const int g_next = sizeof(g_exts) / sizeof(g_exts[0]);

struct PayFile {
  int id;
  const char* name;       // name in the install folder
  const char* tmp;        // 8.3 temp name (WININIT.INI needs short names)
  int required;
};
static const PayFile g_pay[] = {
  { IDR_PAY_PICA, APP_EXE, "PICA.NEW", 1 },
  { IDR_PAY_UNINST, "uninst.exe", "UNINST.NEW", 1 },
  { IDR_PAY_README, "README.TXT", "README.NEW", 0 },
  { IDR_PAY_LICENSE, "LICENSE.TXT", "LICENSE.NEW", 0 }
};
enum { NPAY = sizeof(g_pay) / sizeof(g_pay[0]) };

#define LEGACY_PROGID APP_NAME ".Document"   // shared ProgID of the first 1.0 setups (migrated)
#define KEY_APP       "Software\\" APP_NAME
#define KEY_PREV_OPEN KEY_APP "\\Previous\\Open"
#define KEY_PREV_EDIT KEY_APP "\\Previous\\Edit"
#define KEY_PREV_MADE KEY_APP "\\Previous\\Created"
#define KEY_UNINST    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" APP_NAME
#define KEY_CTX       "*\\shell\\" APP_NAME

// ---- strings and paths ----

void s_copy(char* dst, int cap, const char* src)
{
  if (cap <= 0) return;
  lstrcpynA(dst, src ? src : "", cap);
}

void s_cat(char* dst, int cap, const char* src)
{
  int n = lstrlenA(dst);
  if (n < cap - 1) lstrcpynA(dst + n, src, cap - n);
}

void p_join(const char* dir, const char* name, char* out, int cap)
{
  int n;
  s_copy(out, cap, dir);
  n = lstrlenA(out);
  if (n > 0 && out[n - 1] != '\\') s_cat(out, cap, "\\");
  s_cat(out, cap, name);
}

int p_exists(const char* path)
{
  return GetFileAttributesA(path) != 0xFFFFFFFF;
}

int p_is_dir(const char* path)
{
  DWORD a = GetFileAttributesA(path);
  return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void p_module_dir(char* out, int cap)
{
  char buf[MAX_PATH];
  char* p;
  GetModuleFileNameA(0, buf, sizeof(buf));
  p = strrchr(buf, '\\');
  if (p) *p = 0;
  s_copy(out, cap, buf);
}

int p_same(const char* a, const char* b)
{
  char sa[MAX_PATH], sb[MAX_PATH];
  if (!GetShortPathNameA(a, sa, sizeof(sa))) s_copy(sa, sizeof(sa), a);
  if (!GetShortPathNameA(b, sb, sizeof(sb))) s_copy(sb, sizeof(sb), b);
  return lstrcmpiA(sa, sb) == 0;
}

int p_normalize_dir(const char* in, char* out, int cap)
{
  char buf[MAX_PATH], full[MAX_PATH];
  char* fp;
  int n;
  s_copy(buf, sizeof(buf), in);
  // trim blanks and quotes
  n = lstrlenA(buf);
  while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '"')) buf[--n] = 0;
  fp = buf;
  while (*fp == ' ' || *fp == '"') fp++;
  if (!(fp[0] && fp[1] == ':' && fp[2] == '\\')) return 0;     // absolute, on a drive
  if (!GetFullPathNameA(fp, sizeof(full), full, &fp)) return 0;
  n = lstrlenA(full);
  while (n > 3 && full[n - 1] == '\\') full[--n] = 0;
  if (n <= 3) return 0;                                        // not a drive root
  if (n > MAX_PATH - 20) return 0;
  s_copy(out, cap, full);
  return 1;
}

static int make_dirs(const char* dir)
{
  char buf[MAX_PATH];
  int i, n;
  s_copy(buf, sizeof(buf), dir);
  n = lstrlenA(buf);
  for (i = 3; i <= n; i++) {
    if (buf[i] == '\\' || buf[i] == 0) {
      char c = buf[i];
      buf[i] = 0;
      if (!p_is_dir(buf) && !CreateDirectoryA(buf, 0)) {
        slog("error: cannot create folder %s (%lu)", buf, GetLastError());
        return 0;
      }
      buf[i] = c;
    }
  }
  return 1;
}

// ---- log ----

static HANDLE g_log = INVALID_HANDLE_VALUE;

void log_open(const char* path, int truncate)
{
  SYSTEMTIME st;
  g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, 0);
  if (g_log == INVALID_HANDLE_VALUE) return;
  SetFilePointer(g_log, 0, 0, FILE_END);
  GetLocalTime(&st);
  slog("---- %s setup %s, %04u-%02u-%02u %02u:%02u:%02u, pid %08lX", APP_NAME, APP_VERSION, st.wYear, st.wMonth, st.wDay,
       st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
}

void log_close()
{
  if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
  g_log = INVALID_HANDLE_VALUE;
}

void slog(const char* fmt, ...)
{
  char buf[1100];
  va_list ap;
  DWORD n;
  if (g_log == INVALID_HANDLE_VALUE) return;
  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);      // writes at most 1024 characters
  va_end(ap);
  s_cat(buf, sizeof(buf), "\r\n");
  WriteFile(g_log, buf, lstrlenA(buf), &n, 0);
}

// ---- registry helpers ----

static const char* root_name(HKEY root)
{
  return root == HKEY_CLASSES_ROOT ? "HKCR" : "HKLM";
}

// 1 = value present (out filled), 0 = key present without that value, -1 = key absent
static int reg_get(HKEY root, const char* key, const char* name, char* out, int cap)
{
  HKEY h;
  DWORD type = 0, size = (DWORD)cap;
  LONG r;
  out[0] = 0;
  if (RegOpenKeyExA(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return -1;
  r = RegQueryValueExA(h, (LPSTR)name, 0, &type, (BYTE*)out, &size);
  RegCloseKey(h);
  if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) { out[0] = 0; return 0; }
  out[cap - 1] = 0;
  return 1;
}

static DWORD reg_get_dw(HKEY root, const char* key, const char* name, DWORD def)
{
  HKEY h;
  DWORD v = def, type = 0, size = sizeof(v);
  if (RegOpenKeyExA(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return def;
  if (RegQueryValueExA(h, (LPSTR)name, 0, &type, (BYTE*)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = def;
  RegCloseKey(h);
  return v;
}

static int reg_key_exists(HKEY root, const char* key)
{
  HKEY h;
  if (RegOpenKeyExA(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return 0;
  RegCloseKey(h);
  return 1;
}

static int reg_key_empty(HKEY root, const char* key)
{
  HKEY h;
  DWORD nsub = 0, nval = 0;
  LONG r;
  if (RegOpenKeyExA(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return 0;
  r = RegQueryInfoKeyA(h, 0, 0, 0, &nsub, 0, 0, &nval, 0, 0, 0, 0);
  RegCloseKey(h);
  return r == ERROR_SUCCESS && nsub == 0 && nval == 0;
}

// records a key we created under HKCR, so uninstall can delete it again once it is empty
static void record_created(const char* key)
{
  HKEY h;
  DWORD one = 1;
  char name[300];
  s_copy(name, sizeof(name), "HKCR\\");
  s_cat(name, sizeof(name), key);
  if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, KEY_PREV_MADE, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) return;
  RegSetValueExA(h, name, 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
  RegCloseKey(h);
}

// creates every missing part of key; under HKCR with track=1 each new part is recorded (record_created)
static int reg_ensure(HKEY root, const char* key, int track)
{
  char buf[300];
  int i, n;
  HKEY h;
  s_copy(buf, sizeof(buf), key);
  n = lstrlenA(buf);
  for (i = 1; i <= n; i++) {
    if (buf[i] == '\\' || buf[i] == 0) {
      char c = buf[i];
      buf[i] = 0;
      if (!reg_key_exists(root, buf)) {
        if (RegCreateKeyExA(root, buf, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) {
          slog("error: cannot create %s\\%s", root_name(root), buf);
          return 0;
        }
        RegCloseKey(h);
        if (track && root == HKEY_CLASSES_ROOT) record_created(buf);
      }
      buf[i] = c;
    }
  }
  return 1;
}

static int reg_set(HKEY root, const char* key, const char* name, const char* val)
{
  HKEY h;
  LONG r;
  if (RegCreateKeyExA(root, key, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) {
    slog("error: cannot open %s\\%s", root_name(root), key);
    return 0;
  }
  r = RegSetValueExA(h, name, 0, REG_SZ, (const BYTE*)val, lstrlenA(val) + 1);
  RegCloseKey(h);
  if (r != ERROR_SUCCESS) slog("error: cannot write %s\\%s [%s]", root_name(root), key, name ? name : "");
  return r == ERROR_SUCCESS;
}

static int reg_set_dw(HKEY root, const char* key, const char* name, DWORD v)
{
  HKEY h;
  LONG r;
  if (RegCreateKeyExA(root, key, 0, 0, 0, KEY_WRITE, 0, &h, 0) != ERROR_SUCCESS) return 0;
  r = RegSetValueExA(h, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
  RegCloseKey(h);
  return r == ERROR_SUCCESS;
}

static void reg_del_value(HKEY root, const char* key, const char* name)
{
  HKEY h;
  if (RegOpenKeyExA(root, key, 0, KEY_WRITE, &h) != ERROR_SUCCESS) return;
  RegDeleteValueA(h, name);
  RegCloseKey(h);
}

// deletes key and everything below it (own recursion: NT's RegDeleteKey refuses keys with subkeys)
static void reg_del_tree(HKEY root, const char* key)
{
  HKEY h;
  char sub[300], path[600];
  DWORD n;
  if (RegOpenKeyExA(root, key, 0, KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS) return;
  for (;;) {
    n = sizeof(sub);
    if (RegEnumKeyExA(h, 0, sub, &n, 0, 0, 0, 0) != ERROR_SUCCESS) break;
    s_copy(path, sizeof(path), key);
    s_cat(path, sizeof(path), "\\");
    s_cat(path, sizeof(path), sub);
    if (lstrlenA(path) >= 590) break;
    reg_del_tree(root, path);
    if (reg_key_exists(root, path)) break;      // could not delete: avoid looping forever
  }
  RegCloseKey(h);
  if (RegDeleteKeyA(root, key) == ERROR_SUCCESS) slog("deleted %s\\%s", root_name(root), key);
}

// deletes key only if it has no values and no subkeys (Win9x RegDeleteKey would delete a whole tree)
static int reg_del_if_empty(HKEY root, const char* key)
{
  if (!reg_key_empty(root, key)) return 0;
  if (RegDeleteKeyA(root, key) != ERROR_SUCCESS) return 0;
  slog("deleted empty %s\\%s", root_name(root), key);
  return 1;
}

// value names of key (at most max, each < 260 chars); returns the count
static int reg_value_names(HKEY root, const char* key, char names[][260], int max)
{
  HKEY h;
  int n = 0;
  DWORD i, len;
  if (RegOpenKeyExA(root, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return 0;
  for (i = 0; n < max; i++) {
    len = 260;
    if (RegEnumValueA(h, i, names[n], &len, 0, 0, 0, 0) != ERROR_SUCCESS) break;
    n++;
  }
  RegCloseKey(h);
  return n;
}

// ---- versions ----

void ver_str(DWORD ms, DWORD ls, char* out)
{
  if (LOWORD(ls)) wsprintfA(out, "%u.%u.%u.%u", HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
  else if (HIWORD(ls)) wsprintfA(out, "%u.%u.%u", HIWORD(ms), LOWORD(ms), HIWORD(ls));
  else wsprintfA(out, "%u.%u", HIWORD(ms), LOWORD(ms));
}

int ver_cmp(DWORD ams, DWORD als, DWORD bms, DWORD bls)
{
  if (ams != bms) return ams < bms ? -1 : 1;
  if (als != bls) return als < bls ? -1 : 1;
  return 0;
}

static int parse_ver(const char* s, DWORD* ms, DWORD* ls)
{
  DWORD part[4];
  int i = 0;
  part[0] = part[1] = part[2] = part[3] = 0;
  if (!s || *s < '0' || *s > '9') return 0;
  while (*s && i < 4) {
    if (*s >= '0' && *s <= '9') part[i] = part[i] * 10 + (DWORD)(*s - '0');
    else if (*s == '.') i++;
    else break;
    s++;
  }
  *ms = MAKELONG(part[1], part[0]);
  *ls = MAKELONG(part[3], part[2]);
  return 1;
}

static const BYTE* res_data(int id, DWORD* size)
{
  HRSRC r = FindResourceA(0, MAKEINTRESOURCE(id), RT_RCDATA);
  HGLOBAL g;
  if (!r) return 0;
  g = LoadResource(0, r);
  if (!g) return 0;
  *size = SizeofResource(0, r);
  return (const BYTE*)LockResource(g);
}

int has_payload()
{
  DWORD n;
  return res_data(IDR_PAY_PICA, &n) != 0;
}

int payload_version(DWORD* ms, DWORD* ls)
{
  DWORD n, i;
  const BYTE* p = res_data(IDR_PAY_PICA, &n);
  if (!p || n < 64) return 0;
  for (i = 0; i + sizeof(VS_FIXEDFILEINFO) <= n; i++) {
    if (p[i] == 0xBD && p[i + 1] == 0x04 && p[i + 2] == 0xEF && p[i + 3] == 0xFE) {
      VS_FIXEDFILEINFO f;
      memcpy(&f, p + i, sizeof(f));
      *ms = f.dwFileVersionMS;
      *ls = f.dwFileVersionLS;
      return 1;
    }
  }
  return 0;
}

int file_version(const char* path, DWORD* ms, DWORD* ls)
{
  DWORD dummy = 0, size;
  char* buf;
  VS_FIXEDFILEINFO* f = 0;
  UINT flen = 0;
  int ok = 0;
  size = GetFileVersionInfoSizeA((char*)path, &dummy);
  if (!size) return 0;
  buf = (char*)HeapAlloc(GetProcessHeap(), 0, size);
  if (!buf) return 0;
  if (GetFileVersionInfoA((char*)path, 0, size, buf) && VerQueryValueA(buf, (char*)"\\", (void**)&f, &flen) && f &&
      flen >= sizeof(VS_FIXEDFILEINFO)) {
    *ms = f->dwFileVersionMS;
    *ls = f->dwFileVersionLS;
    ok = 1;
  }
  HeapFree(GetProcessHeap(), 0, buf);
  return ok;
}

void detect_install(InstallInfo* ii)
{
  char exe[MAX_PATH], v[64];
  memset(ii, 0, sizeof(*ii));
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, "InstallDir", ii->dir, sizeof(ii->dir)) != 1 || !ii->dir[0]) return;
  ii->installed = 1;
  p_join(ii->dir, APP_EXE, exe, sizeof(exe));
  if (file_version(exe, &ii->ms, &ii->ls)) ii->have_ver = 1;
  else if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, "Version", v, sizeof(v)) == 1 && parse_ver(v, &ii->ms, &ii->ls))
    ii->have_ver = 1;
  if (ii->have_ver) ver_str(ii->ms, ii->ls, ii->ver);
  else s_copy(ii->ver, sizeof(ii->ver), "?");
}

void opts_default_types(Opts* o)
{
  int i;
  for (i = 0; i < g_next; i++) o->types[i] = g_exts[i].def;
}

void opts_defaults(Opts* o, const InstallInfo* ii)
{
  char types[600];
  int i;
  opts_default_types(o);
  o->startmenu = 1;
  o->desktop = 0;
  o->context = 1;
  if (ii && ii->installed) {
    s_copy(o->dir, sizeof(o->dir), ii->dir);
    o->startmenu = (int)reg_get_dw(HKEY_LOCAL_MACHINE, KEY_APP, "StartMenu", 1);
    o->desktop = (int)reg_get_dw(HKEY_LOCAL_MACHINE, KEY_APP, "Desktop", 0);
    o->context = (int)reg_get_dw(HKEY_LOCAL_MACHINE, KEY_APP, "ContextMenu", 1);
    if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, "Types", types + 1, sizeof(types) - 2) == 1) {
      types[0] = ';';
      s_cat(types, sizeof(types), ";");
      CharLowerA(types);
      for (i = 0; i < g_next; i++) {
        char pat[16];
        s_copy(pat, sizeof(pat), ";");
        s_cat(pat, sizeof(pat), g_exts[i].ext);
        s_cat(pat, sizeof(pat), ";");
        o->types[i] = strstr(types, pat) != 0;
      }
    }
  } else {
    char pf[MAX_PATH];
    if (reg_get(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion", "ProgramFilesDir", pf,
                sizeof(pf)) != 1 || !pf[0])
      s_copy(pf, sizeof(pf), "C:\\Program Files");
    p_join(pf, APP_NAME, o->dir, sizeof(o->dir));
  }
}

int pica_running()
{
  return FindWindowA(APP_CLASS_FRAME, 0) != 0;
}

// ---- special folders and shortcuts ----

static int special_dir(int csidl, char* out)
{
  LPITEMIDLIST pidl = 0;
  LPMALLOC m = 0;
  int ok;
  out[0] = 0;
  if (SHGetSpecialFolderLocation(0, csidl, &pidl) != NOERROR || !pidl) return 0;
  ok = SHGetPathFromIDListA(pidl, out) ? 1 : 0;
  if (SHGetMalloc(&m) == NOERROR && m) {
    m->Free(pidl);
    m->Release();
  }
  return ok && out[0];
}

// own GUID constants (no uuid.lib dependency)
static const GUID G_CLSID_ShellLink = { 0x00021401, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID G_IID_IShellLinkA = { 0x000214EE, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID G_IID_IPersistFile = { 0x0000010B, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int make_link(const char* lnk, const char* target, const char* workdir, const char* desc)
{
  IShellLinkA* sl = 0;
  IPersistFile* pf = 0;
  WCHAR wpath[MAX_PATH];
  HRESULT hr;
  hr = CoCreateInstance(G_CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, G_IID_IShellLinkA, (void**)&sl);
  if (FAILED(hr) || !sl) { slog("error: IShellLink unavailable (0x%08lX)", (DWORD)hr); return 0; }
  sl->SetPath(target);
  sl->SetWorkingDirectory(workdir);
  sl->SetDescription(desc);
  sl->SetIconLocation(target, 0);
  hr = sl->QueryInterface(G_IID_IPersistFile, (void**)&pf);
  if (SUCCEEDED(hr) && pf) {
    MultiByteToWideChar(CP_ACP, 0, lnk, -1, wpath, MAX_PATH);
    hr = pf->Save(wpath, TRUE);
    pf->Release();
  }
  sl->Release();
  if (FAILED(hr)) { slog("error: cannot save shortcut %s (0x%08lX)", lnk, (DWORD)hr); return 0; }
  SHChangeNotify(SHCNE_CREATE, SHCNF_PATH, lnk, 0);
  slog("shortcut %s", lnk);
  return 1;
}

static void delete_link_value(const char* value)
{
  char lnk[MAX_PATH];
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, value, lnk, sizeof(lnk)) == 1 && lnk[0]) {
    if (DeleteFileA(lnk)) {
      SHChangeNotify(SHCNE_DELETE, SHCNF_PATH, lnk, 0);
      slog("deleted shortcut %s", lnk);
    }
  }
  reg_del_value(HKEY_LOCAL_MACHINE, KEY_APP, value);
}

static int set_link(int want, int csidl, const char* value, const char* dir)
{
  char folder[MAX_PATH], lnk[MAX_PATH], exe[MAX_PATH], old[MAX_PATH];
  if (!want) { delete_link_value(value); return 1; }
  if (!special_dir(csidl, folder)) { slog("error: special folder %d not found", csidl); return 0; }
  p_join(folder, APP_NAME ".lnk", lnk, sizeof(lnk));
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, value, old, sizeof(old)) == 1 && old[0] && lstrcmpiA(old, lnk) != 0)
    delete_link_value(value);
  p_join(dir, APP_EXE, exe, sizeof(exe));
  if (!make_link(lnk, exe, dir, APP_NAME " text and hex editor")) return 0;
  reg_set(HKEY_LOCAL_MACHINE, KEY_APP, value, lnk);
  return 1;
}

// ---- WININIT.INI (Win9x replaces/deletes files there at the next start; MoveFileEx delay is NT only) ----

static char g_wi[2048];      // pending [rename] lines

static int wininit_line(const char* dst, const char* src)
{
  char line[MAX_PATH * 2 + 8];
  s_copy(line, sizeof(line), dst);
  s_cat(line, sizeof(line), "=");
  s_cat(line, sizeof(line), src);
  s_cat(line, sizeof(line), "\r\n");
  if (lstrlenA(g_wi) + lstrlenA(line) >= (int)sizeof(g_wi)) return 0;
  s_cat(g_wi, sizeof(g_wi), line);
  slog("WININIT.INI [rename] %s=%s", dst, src);
  return 1;
}

static int wininit_flush()
{
  char path[MAX_PATH];
  HANDLE h;
  char* old = 0;
  char* out;
  DWORD size = 0, n = 0, pos = 0, outlen = 0;
  int ok;
  if (!g_wi[0]) return 1;
  GetWindowsDirectoryA(path, sizeof(path));
  p_join(path, "WININIT.INI", path, sizeof(path));
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (h != INVALID_HANDLE_VALUE) {
    size = GetFileSize(h, 0);
    if (size > 0 && size < 65536) {
      old = (char*)HeapAlloc(GetProcessHeap(), 0, size + 1);
      if (old && ReadFile(h, old, size, &n, 0)) old[n] = 0;
      else n = 0;
    }
    CloseHandle(h);
  }
  if (old && n) {          // drop lines the file already has (a second uninstall before the restart)
    char keep[sizeof(g_wi)];
    char* l = g_wi;
    keep[0] = 0;
    while (*l) {
      char* e = strchr(l, '\n');
      char line[MAX_PATH * 2 + 8];
      int len = e ? (int)(e - l) + 1 : lstrlenA(l);
      lstrcpynA(line, l, len + 1 < (int)sizeof(line) ? len + 1 : (int)sizeof(line));
      if (!strstr(old, line)) s_cat(keep, sizeof(keep), line);
      l += len;
    }
    s_copy(g_wi, sizeof(g_wi), keep);
    if (!g_wi[0]) { HeapFree(GetProcessHeap(), 0, old); return 1; }
  }
  out = (char*)HeapAlloc(GetProcessHeap(), 0, n + sizeof(g_wi) + 32);
  if (!out) return 0;
  // insert our lines right after an existing [rename] header, else append the section
  if (old && n) {
    DWORD i = 0;
    while (i < n) {
      DWORD e = i;
      while (e < n && old[e] != '\n') e++;
      if (e - i >= 8 && CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, old + i, 8, "[rename]", 8) == 2) {
        pos = e < n ? e + 1 : n;
        break;
      }
      i = e + 1;
    }
  }
  if (pos) {
    memcpy(out, old, pos);
    outlen = pos;
    if (old[pos - 1] != '\n') { out[outlen++] = '\r'; out[outlen++] = '\n'; }
    memcpy(out + outlen, g_wi, lstrlenA(g_wi));
    outlen += lstrlenA(g_wi);
    memcpy(out + outlen, old + pos, n - pos);
    outlen += n - pos;
  } else {
    if (old && n) {
      memcpy(out, old, n);
      outlen = n;
      if (old[n - 1] != '\n') { out[outlen++] = '\r'; out[outlen++] = '\n'; }
    }
    memcpy(out + outlen, "[rename]\r\n", 10);
    outlen += 10;
    memcpy(out + outlen, g_wi, lstrlenA(g_wi));
    outlen += lstrlenA(g_wi);
  }
  h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  ok = h != INVALID_HANDLE_VALUE && WriteFile(h, out, outlen, &n, 0) && n == outlen;
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  if (old) HeapFree(GetProcessHeap(), 0, old);
  HeapFree(GetProcessHeap(), 0, out);
  if (!ok) slog("error: cannot write %s", path);
  g_wi[0] = 0;
  return ok;
}

int wininit_delete_later(const char* path)
{
  char sp[MAX_PATH];
  if (!GetShortPathNameA(path, sp, sizeof(sp))) return 0;
  return wininit_line("NUL", sp) && wininit_flush();
}

// ---- file associations ----
// Previous\Open\<.ext> and Previous\Edit\<ProgID> hold what was there before us:
//   "=<value>" the old default value, "!" key present without a default value, "-" key absent (we created it).
// Restores happen only while the entry still points at Pica.

static void our_command(const char* dir, char* out, int cap)
{
  char exe[MAX_PATH];
  p_join(dir, APP_EXE, exe, sizeof(exe));
  s_copy(out, cap, "\"");
  s_cat(out, cap, exe);
  s_cat(out, cap, "\" \"%1\"");
}

static void encode_prev(int st, const char* cur, char* rec, int cap)
{
  if (st < 0) s_copy(rec, cap, "-");
  else if (st == 0) s_copy(rec, cap, "!");
  else { s_copy(rec, cap, "="); s_cat(rec, cap, cur); }
}

static void restore_default(HKEY root, const char* key, const char* rec)
{
  if (rec[0] == '=') reg_set(root, key, 0, rec + 1);
  else reg_del_value(root, key, "");
}

// Each ticked type gets its own ProgID Pica.<ext> (Pica.txt, Pica.c): its name ("Text Document") and its other verbs
// (print, printto, ...) are copied from the previous ProgID, so Explorer's Type column, New menu and Print stay as
// they were; only open (and the icon) become Pica's. Pica 1.0 setups before this used one shared "Pica.Document":
// it counts as ours and is migrated.
static void ext_progid(const char* ext, char* out, int cap)
{
  s_copy(out, cap, APP_NAME ".");
  s_cat(out, cap, ext[0] == '.' ? ext + 1 : ext);
}

static int is_ours(const char* ext, const char* cur)
{
  char pid[64];
  ext_progid(ext, pid, sizeof(pid));
  return lstrcmpiA(cur, pid) == 0 || lstrcmpiA(cur, LEGACY_PROGID) == 0;
}

static int starts_pica(const char* progid)
{
  int n = lstrlenA(APP_NAME ".");
  return lstrlenA(progid) >= n && CompareStringA(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, progid, n, APP_NAME ".", n) == 2;
}

// copies key src (values and subkeys) to dst under HKCR; dst is ours, so nothing is tracked
static void reg_copy_tree(const char* src, const char* dst, int depth)
{
  HKEY hs, hd;
  DWORD i, nlen, dlen, type;
  char name[260], sub[600], dsub[600];
  BYTE data[2048];
  if (depth > 8 || RegOpenKeyExA(HKEY_CLASSES_ROOT, src, 0, KEY_READ, &hs) != ERROR_SUCCESS) return;
  if (RegCreateKeyExA(HKEY_CLASSES_ROOT, dst, 0, 0, 0, KEY_WRITE, 0, &hd, 0) != ERROR_SUCCESS) { RegCloseKey(hs); return; }
  for (i = 0;; i++) {
    nlen = sizeof(name);
    dlen = sizeof(data);
    if (RegEnumValueA(hs, i, name, &nlen, 0, &type, data, &dlen) != ERROR_SUCCESS) break;
    RegSetValueExA(hd, name, 0, type, data, dlen);
  }
  RegCloseKey(hd);
  for (i = 0;; i++) {
    nlen = sizeof(name);
    if (RegEnumKeyExA(hs, i, name, &nlen, 0, 0, 0, 0) != ERROR_SUCCESS) break;
    if (lstrlenA(src) + nlen + 2 >= sizeof(sub) || lstrlenA(dst) + nlen + 2 >= sizeof(dsub)) continue;
    s_copy(sub, sizeof(sub), src); s_cat(sub, sizeof(sub), "\\"); s_cat(sub, sizeof(sub), name);
    s_copy(dsub, sizeof(dsub), dst); s_cat(dsub, sizeof(dsub), "\\"); s_cat(dsub, sizeof(dsub), name);
    reg_copy_tree(sub, dsub, depth + 1);
  }
  RegCloseKey(hs);
}

// (re)builds HKCR\Pica.<ext> from the saved previous value rec
static int build_progid(const char* ext, const char* rec, const char* dir, const char* cmd)
{
  char pid[64], key[400], src[260], desc[300], icon[MAX_PATH + 4], verbs[300], name[260];
  int have_src = 0;
  HKEY h;
  DWORD i, nlen;
  ext_progid(ext, pid, sizeof(pid));
  reg_del_tree(HKEY_CLASSES_ROOT, pid);
  if (rec[0] == '=' && rec[1] && !starts_pica(rec + 1) && reg_key_exists(HKEY_CLASSES_ROOT, rec + 1)) {
    s_copy(src, sizeof(src), rec + 1);
    have_src = 1;
  }
  if (!have_src || reg_get(HKEY_CLASSES_ROOT, src, 0, desc, sizeof(desc)) != 1 || !desc[0]) {
    s_copy(desc, sizeof(desc), ext[0] == '.' ? ext + 1 : ext);
    CharUpperA(desc);
    s_cat(desc, sizeof(desc), " File");
  }
  p_join(dir, APP_EXE, icon, sizeof(icon));
  s_cat(icon, sizeof(icon), ",0");
  s_copy(key, sizeof(key), pid);
  s_cat(key, sizeof(key), "\\shell\\open\\command");
  if (!reg_ensure(HKEY_CLASSES_ROOT, key, 0) || !reg_set(HKEY_CLASSES_ROOT, key, 0, cmd)) return 0;
  s_copy(key, sizeof(key), pid);
  s_cat(key, sizeof(key), "\\DefaultIcon");
  if (!reg_set(HKEY_CLASSES_ROOT, pid, 0, desc) || !reg_set(HKEY_CLASSES_ROOT, key, 0, icon)) return 0;
  verbs[0] = 0;
  if (have_src) {
    s_copy(key, sizeof(key), src);
    s_cat(key, sizeof(key), "\\shell");
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
      for (i = 0;; i++) {
        char from[600], to[600];
        nlen = sizeof(name);
        if (RegEnumKeyExA(h, i, name, &nlen, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        if (lstrcmpiA(name, "open") == 0) continue;
        s_copy(from, sizeof(from), key); s_cat(from, sizeof(from), "\\"); s_cat(from, sizeof(from), name);
        s_copy(to, sizeof(to), pid); s_cat(to, sizeof(to), "\\shell\\"); s_cat(to, sizeof(to), name);
        reg_copy_tree(from, to, 0);
        if (verbs[0]) s_cat(verbs, sizeof(verbs), ",");
        s_cat(verbs, sizeof(verbs), name);
      }
      RegCloseKey(h);
    }
  }
  s_copy(key, sizeof(key), pid);
  s_cat(key, sizeof(key), "\\shell");
  reg_set(HKEY_CLASSES_ROOT, key, 0, "open");
  slog("progid: HKCR\\%s \"%s\", verbs open%s%s (from %s)", pid, desc, verbs[0] ? "," : "", verbs,
       have_src ? src : "nothing");
  return 1;
}

static int open_assoc(const char* ext, const char* dir, const char* cmd)
{
  char cur[300], rec[310], pid[64];
  int st = reg_get(HKEY_CLASSES_ROOT, ext, 0, cur, sizeof(cur));
  ext_progid(ext, pid, sizeof(pid));
  if (st == 1 && is_ours(ext, cur)) {           // already ours: keep the first saved previous value
    if (reg_get(HKEY_LOCAL_MACHINE, KEY_PREV_OPEN, ext, rec, sizeof(rec)) != 1) s_copy(rec, sizeof(rec), "!");
  } else {
    encode_prev(st, cur, rec, sizeof(rec));
    if (!reg_set(HKEY_LOCAL_MACHINE, KEY_PREV_OPEN, ext, rec)) return 0;
  }
  if (!build_progid(ext, rec, dir, cmd)) return 0;
  if (!(st == 1 && lstrcmpiA(cur, pid) == 0)) {
    if (!reg_ensure(HKEY_CLASSES_ROOT, ext, 1) || !reg_set(HKEY_CLASSES_ROOT, ext, 0, pid)) return 0;
    slog("open: HKCR\\%s %s -> %s", ext, st == 1 && is_ours(ext, cur) ? cur : rec, pid);
  }
  return 1;
}

static void open_unassoc(const char* ext)
{
  char cur[300], rec[310], pid[64];
  int st;
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_PREV_OPEN, ext, rec, sizeof(rec)) != 1) return;
  st = reg_get(HKEY_CLASSES_ROOT, ext, 0, cur, sizeof(cur));
  if (st == 1 && is_ours(ext, cur)) {
    restore_default(HKEY_CLASSES_ROOT, ext, rec);
    slog("open: HKCR\\%s restored %s", ext, rec);
  } else {
    slog("open: HKCR\\%s now \"%s\", not ours: left alone", ext, cur);
  }
  ext_progid(ext, pid, sizeof(pid));
  reg_del_tree(HKEY_CLASSES_ROOT, pid);
  reg_del_value(HKEY_LOCAL_MACHINE, KEY_PREV_OPEN, ext);
}

// the ProgID that runs a script type, or 0 when nothing handles it (then Pica may open it)
static int script_handler(const char* ext, char* progid, int cap)
{
  if (reg_get(HKEY_CLASSES_ROOT, ext, 0, progid, cap) != 1 || !progid[0]) return 0;
  if (is_ours(ext, progid)) return 0;
  return reg_key_exists(HKEY_CLASSES_ROOT, progid);
}

static int edit_assoc(const char* progid, const char* cmd)
{
  char verb[300], ckey[300], cur[600], rec[610];
  int st, verb_exists;
  s_copy(verb, sizeof(verb), progid);
  s_cat(verb, sizeof(verb), "\\shell\\edit");
  s_copy(ckey, sizeof(ckey), verb);
  s_cat(ckey, sizeof(ckey), "\\command");
  st = reg_get(HKEY_CLASSES_ROOT, ckey, 0, cur, sizeof(cur));
  if (st == 1 && lstrcmpiA(cur, cmd) == 0) return 1;
  verb_exists = reg_key_exists(HKEY_CLASSES_ROOT, verb);
  if (!verb_exists) s_copy(rec, sizeof(rec), "-");
  else encode_prev(st < 0 ? 0 : st, cur, rec, sizeof(rec));
  if (!reg_set(HKEY_LOCAL_MACHINE, KEY_PREV_EDIT, progid, rec)) return 0;
  if (!verb_exists) {
    char shell[300];
    s_copy(shell, sizeof(shell), progid);
    s_cat(shell, sizeof(shell), "\\shell");
    if (!reg_ensure(HKEY_CLASSES_ROOT, shell, 1) || !reg_ensure(HKEY_CLASSES_ROOT, ckey, 0)) return 0;
    reg_set(HKEY_CLASSES_ROOT, verb, 0, "&Edit with " APP_NAME);
  } else if (!reg_ensure(HKEY_CLASSES_ROOT, ckey, 1)) {
    return 0;
  }
  if (!reg_set(HKEY_CLASSES_ROOT, ckey, 0, cmd)) return 0;
  slog("edit: HKCR\\%s %s -> %s", ckey, rec, cmd);
  return 1;
}

static void edit_restore(const char* progid, const char* cmd)
{
  char verb[300], ckey[300], cur[600], rec[610];
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_PREV_EDIT, progid, rec, sizeof(rec)) != 1) return;
  s_copy(verb, sizeof(verb), progid);
  s_cat(verb, sizeof(verb), "\\shell\\edit");
  s_copy(ckey, sizeof(ckey), verb);
  s_cat(ckey, sizeof(ckey), "\\command");
  if (reg_get(HKEY_CLASSES_ROOT, ckey, 0, cur, sizeof(cur)) == 1 && lstrcmpiA(cur, cmd) == 0) {
    if (rec[0] == '-') reg_del_tree(HKEY_CLASSES_ROOT, verb);
    else restore_default(HKEY_CLASSES_ROOT, ckey, rec);
    slog("edit: HKCR\\%s restored %s", ckey, rec);
  } else {
    slog("edit: HKCR\\%s now \"%s\", not ours: left alone", ckey, cur);
  }
  reg_del_value(HKEY_LOCAL_MACHINE, KEY_PREV_EDIT, progid);
}

static int in_list(char list[][260], int n, const char* s)
{
  int i;
  for (i = 0; i < n; i++) if (lstrcmpiA(list[i], s) == 0) return 1;
  return 0;
}

// makes the associations match types[] (all zero = remove everything); idempotent
static int assoc_apply(const char* dir, const int* types)
{
  static char want_open[MAX_EXT][260], want_edit[MAX_EXT][260], recs[64][260];
  int nopen = 0, nedit = 0, nrec, i, ok = 1;
  char cmd[600], progid[260];
  our_command(dir, cmd, sizeof(cmd));
  for (i = 0; i < g_next && i < MAX_EXT; i++) {
    if (!types[i]) continue;
    if (g_exts[i].group == G_SCRIPT && script_handler(g_exts[i].ext, progid, sizeof(progid))) {
      if (!in_list(want_edit, nedit, progid)) s_copy(want_edit[nedit++], 260, progid);
    } else {
      if (g_exts[i].group == G_SCRIPT) slog("%s has no program that runs it: Pica opens it", g_exts[i].ext);
      s_copy(want_open[nopen++], 260, g_exts[i].ext);
    }
  }
  nrec = reg_value_names(HKEY_LOCAL_MACHINE, KEY_PREV_OPEN, recs, 64);
  for (i = 0; i < nrec; i++) if (!in_list(want_open, nopen, recs[i])) open_unassoc(recs[i]);
  for (i = 0; i < nopen; i++) if (!open_assoc(want_open[i], dir, cmd)) ok = 0;
  nrec = reg_value_names(HKEY_LOCAL_MACHINE, KEY_PREV_EDIT, recs, 64);
  for (i = 0; i < nrec; i++) if (!in_list(want_edit, nedit, recs[i])) edit_restore(recs[i], cmd);
  for (i = 0; i < nedit; i++) if (!edit_assoc(want_edit[i], cmd)) ok = 0;
  return ok;
}

// deletes the HKCR keys we created once they are empty again (deepest first)
static void created_cleanup()
{
  static char recs[64][260];
  int n = reg_value_names(HKEY_LOCAL_MACHINE, KEY_PREV_MADE, recs, 64), k, j;
  for (k = 0; k < n; k++) {
    int best = -1, bl = -1;
    const char* key;
    for (j = 0; j < n; j++) if (recs[j][0] && lstrlenA(recs[j]) > bl) { best = j; bl = lstrlenA(recs[j]); }
    if (best < 0) break;
    key = recs[best] + 5;     // after "HKCR\"
    if (!reg_key_exists(HKEY_CLASSES_ROOT, key) || reg_del_if_empty(HKEY_CLASSES_ROOT, key))
      reg_del_value(HKEY_LOCAL_MACHINE, KEY_PREV_MADE, recs[best]);
    recs[best][0] = 0;
  }
}

static int context_set(int want, const char* cmd)
{
  if (!want) {
    reg_del_tree(HKEY_CLASSES_ROOT, KEY_CTX);
    return 1;
  }
  if (!reg_ensure(HKEY_CLASSES_ROOT, "*\\shell", 1) || !reg_ensure(HKEY_CLASSES_ROOT, KEY_CTX "\\command", 0)) return 0;
  slog("context menu: HKCR\\" KEY_CTX);
  return reg_set(HKEY_CLASSES_ROOT, KEY_CTX, 0, "Open with &" APP_NAME) &&
         reg_set(HKEY_CLASSES_ROOT, KEY_CTX "\\command", 0, cmd);
}

void notify_shell()
{
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
}

// ---- settings (DESIGN 13.1: next to the exe if writable, else Application Data\Pica, else <windows>\Pica) ----

static const char* const g_cfg_files[] = { APP_INI, "session1.ini", "session2.ini", "~pica.tst" };
enum { NCFG = sizeof(g_cfg_files) / sizeof(g_cfg_files[0]) };

static int cfg_dirs(const char* dir, char out[3][MAX_PATH])
{
  char buf[MAX_PATH];
  int n = 0;
  s_copy(out[n++], MAX_PATH, dir);
  if (special_dir(0x001A /* CSIDL_APPDATA */, buf)) p_join(buf, APP_NAME, out[n++], MAX_PATH);
  GetWindowsDirectoryA(buf, sizeof(buf));
  p_join(buf, APP_NAME, out[n++], MAX_PATH);
  return n;
}

int settings_exist(const char* dir)
{
  char d[3][MAX_PATH], p[MAX_PATH];
  int n = cfg_dirs(dir, d), i, j;
  for (i = 0; i < n; i++) {
    for (j = 0; j < NCFG - 1; j++) {
      p_join(d[i], g_cfg_files[j], p, sizeof(p));
      if (p_exists(p)) return 1;
    }
    p_join(d[i], "backup", p, sizeof(p));
    if (p_is_dir(p)) return 1;
  }
  return 0;
}

static void delete_file(const char* path, int* reboot)
{
  if (!p_exists(path)) return;
  SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
  if (DeleteFileA(path)) { slog("deleted %s", path); return; }
  slog("in use: %s (deleted when Windows restarts)", path);
  if (wininit_delete_later(path)) *reboot = 1;
}

static void purge_settings(const char* dir, int* reboot)
{
  char d[3][MAX_PATH], p[MAX_PATH], b[MAX_PATH];
  int n = cfg_dirs(dir, d), i, j;
  for (i = 0; i < n; i++) {
    for (j = 0; j < NCFG; j++) {
      p_join(d[i], g_cfg_files[j], p, sizeof(p));
      delete_file(p, reboot);
    }
    p_join(d[i], "backup", b, sizeof(b));
    if (p_is_dir(b)) {
      WIN32_FIND_DATAA fd;
      HANDLE f;
      p_join(b, "*.*", p, sizeof(p));
      f = FindFirstFileA(p, &fd);
      if (f != INVALID_HANDLE_VALUE) {
        do {
          if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
          p_join(b, fd.cFileName, p, sizeof(p));
          delete_file(p, reboot);
        } while (FindNextFileA(f, &fd));
        FindClose(f);
      }
      if (RemoveDirectoryA(b)) slog("deleted folder %s", b);
    }
    if (i > 0 && p_is_dir(d[i]) && RemoveDirectoryA(d[i])) slog("deleted folder %s", d[i]);
  }
}

// ---- install ----

static void step(ProgressFn fn, void* ctx, int pct, const char* text)
{
  slog("[%d%%] %s", pct, text);
  if (fn) fn(pct, text, ctx);
}

static int write_file(const char* path, const BYTE* data, DWORD size)
{
  HANDLE h;
  DWORD n = 0;
  int ok;
  SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
  h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (h == INVALID_HANDLE_VALUE) return 0;
  ok = WriteFile(h, data, size, &n, 0) && n == size;
  if (!CloseHandle(h)) ok = 0;
  if (!ok) DeleteFileA(path);
  return ok;
}

int inst_run(const Opts* o, ProgressFn fn, void* ctx, char* err, int errcap)
{
  char path[MAX_PATH], tmp[MAX_PATH], cmd[600], files[400], oldfiles[400], types[600], ver[32], buf[MAX_PATH + 4];
  const BYTE* data[NPAY];
  DWORD size[NPAY], ms = 0, ls = 0;
  HANDLE lock[NPAY];
  int i, reboot = 0, ok = 1;
  err[0] = 0;
  g_wi[0] = 0;
  for (i = 0; i < NPAY; i++) lock[i] = INVALID_HANDLE_VALUE;
  payload_version(&ms, &ls);
  ver_str(ms, ls, ver);
  slog("install %s %s into %s (start menu %d, desktop %d, context menu %d)", APP_NAME, ver, o->dir, o->startmenu,
       o->desktop, o->context);

  step(fn, ctx, 5, "Creating the folder...");
  if (!make_dirs(o->dir)) {
    wsprintfA(err, "Setup could not create the folder %s.", o->dir);
    return RC_FAIL;
  }

  // phase 1: every new file next to its target under a temp name (a full disk changes nothing)
  files[0] = 0;
  for (i = 0; i < NPAY; i++) {
    data[i] = res_data(g_pay[i].id, &size[i]);
    if (!data[i]) {
      if (g_pay[i].required) { s_copy(err, errcap, "This setup.exe is damaged (payload missing)."); return RC_FAIL; }
      continue;
    }
    step(fn, ctx, 10 + i * 10, "Copying files...");
    p_join(o->dir, g_pay[i].tmp, tmp, sizeof(tmp));
    if (!write_file(tmp, data[i], size[i])) {
      int j;
      slog("error: cannot write %s (%lu)", tmp, GetLastError());
      for (j = 0; j <= i; j++) {
        p_join(o->dir, g_pay[j].tmp, tmp, sizeof(tmp));
        DeleteFileA(tmp);
      }
      wsprintfA(err, "Setup could not write to %s. Is the disk full or write-protected?", o->dir);
      return RC_FAIL;
    }
    if (files[0]) s_cat(files, sizeof(files), ";");
    s_cat(files, sizeof(files), g_pay[i].name);
  }

  // phase 2: swap them in; a file in use is replaced by WININIT.INI at the next start
  step(fn, ctx, 55, "Replacing program files...");
  for (i = 0; i < NPAY; i++) {
    if (!data[i]) continue;
    p_join(o->dir, g_pay[i].name, path, sizeof(path));
    p_join(o->dir, g_pay[i].tmp, tmp, sizeof(tmp));
    if (o->locktest && p_exists(path))       // test switch: hold the old file open like a running program would
      lock[i] = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (p_exists(path)) {
      SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
      if (!DeleteFileA(path)) {
        char sp[MAX_PATH], st[MAX_PATH];
        if (!GetShortPathNameA(path, sp, sizeof(sp)) || !GetShortPathNameA(tmp, st, sizeof(st)) ||
            !wininit_line("NUL", sp) || !wininit_line(sp, st)) {
          wsprintfA(err, "%s is in use and could not be replaced.", path);
          ok = 0;
          break;
        }
        slog("in use: %s (replaced when Windows restarts)", path);
        reboot = 1;
        continue;
      }
    }
    if (!MoveFileA(tmp, path)) {
      wsprintfA(err, "Setup could not create %s.", path);
      ok = 0;
      break;
    }
    slog("installed %s (%lu bytes)", path, size[i]);
  }
  for (i = 0; i < NPAY; i++) if (lock[i] != INVALID_HANDLE_VALUE) CloseHandle(lock[i]);
  if (reboot && !wininit_flush()) { s_copy(err, errcap, "Setup could not write WININIT.INI."); ok = 0; }
  if (!ok) return RC_FAIL;

  // files of an older version that this one doesn't ship
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, "Files", oldfiles, sizeof(oldfiles)) == 1) {
    char* s = oldfiles;
    while (*s) {
      char* e = s;
      char name[MAX_PATH];
      while (*e && *e != ';') e++;
      lstrcpynA(name, s, (int)(e - s) + 1 < MAX_PATH ? (int)(e - s) + 1 : MAX_PATH);
      if (name[0]) {
        int keep = 0;
        for (i = 0; i < NPAY; i++) if (data[i] && lstrcmpiA(name, g_pay[i].name) == 0) keep = 1;
        if (!keep && !strchr(name, '\\')) {
          p_join(o->dir, name, path, sizeof(path));
          delete_file(path, &reboot);
        }
      }
      s = *e ? e + 1 : e;
    }
  }

  step(fn, ctx, 70, "Writing the registry...");
  our_command(o->dir, cmd, sizeof(cmd));
  types[0] = 0;
  for (i = 0; i < g_next; i++) {
    if (!o->types[i]) continue;
    if (types[0]) s_cat(types, sizeof(types), ";");
    s_cat(types, sizeof(types), g_exts[i].ext);
  }
  ok = reg_set(HKEY_LOCAL_MACHINE, KEY_APP, "InstallDir", o->dir) && reg_set(HKEY_LOCAL_MACHINE, KEY_APP, "Version", ver) &&
       reg_set(HKEY_LOCAL_MACHINE, KEY_APP, "Files", files) && reg_set(HKEY_LOCAL_MACHINE, KEY_APP, "Types", types) &&
       reg_set_dw(HKEY_LOCAL_MACHINE, KEY_APP, "StartMenu", (DWORD)o->startmenu) &&
       reg_set_dw(HKEY_LOCAL_MACHINE, KEY_APP, "Desktop", (DWORD)o->desktop) &&
       reg_set_dw(HKEY_LOCAL_MACHINE, KEY_APP, "ContextMenu", (DWORD)o->context);

  step(fn, ctx, 78, "Setting up file types...");
  if (ok) ok = context_set(o->context, cmd);
  if (ok) ok = assoc_apply(o->dir, o->types);
  reg_del_tree(HKEY_CLASSES_ROOT, LEGACY_PROGID);
  created_cleanup();

  step(fn, ctx, 88, "Creating shortcuts...");
  if (ok) ok = set_link(o->startmenu, CSIDL_PROGRAMS, "StartMenuLnk", o->dir);
  if (ok) ok = set_link(o->desktop, CSIDL_DESKTOPDIRECTORY, "DesktopLnk", o->dir);

  step(fn, ctx, 95, "Registering the uninstaller...");
  if (ok) {
    char dn[64];
    p_join(o->dir, "uninst.exe", path, sizeof(path));
    s_copy(buf, sizeof(buf), "\"");
    s_cat(buf, sizeof(buf), path);
    s_cat(buf, sizeof(buf), "\"");
    s_copy(dn, sizeof(dn), APP_NAME " ");
    s_cat(dn, sizeof(dn), ver);
    ok = reg_set(HKEY_LOCAL_MACHINE, KEY_UNINST, "DisplayName", dn) &&
         reg_set(HKEY_LOCAL_MACHINE, KEY_UNINST, "DisplayVersion", ver) &&
         reg_set(HKEY_LOCAL_MACHINE, KEY_UNINST, "UninstallString", buf) &&
         reg_set(HKEY_LOCAL_MACHINE, KEY_UNINST, "InstallLocation", o->dir);
    p_join(o->dir, APP_EXE, buf, sizeof(buf));
    s_cat(buf, sizeof(buf), ",0");
    if (ok) ok = reg_set(HKEY_LOCAL_MACHINE, KEY_UNINST, "DisplayIcon", buf);
    reg_set_dw(HKEY_LOCAL_MACHINE, KEY_UNINST, "NoModify", 1);
    reg_set_dw(HKEY_LOCAL_MACHINE, KEY_UNINST, "NoRepair", 1);
  }
  notify_shell();
  if (!ok) {
    if (!err[0]) s_copy(err, errcap, "Setup could not write the registry or create the shortcuts (see the log).");
    return RC_FAIL;
  }
  step(fn, ctx, 100, reboot ? "Done; restart Windows to finish." : "Done.");
  slog("install finished: %s", reboot ? "restart needed" : "ok");
  return reboot ? RC_REBOOT : RC_OK;
}

// ---- uninstall ----

int uninst_run(const char* dir, int purge, int defer_self, int* kept_folder)
{
  char path[MAX_PATH], files[400], cmd[600];
  static const int none[MAX_EXT] = { 0 };
  int i, reboot = 0;
  g_wi[0] = 0;
  slog("uninstall from %s (delete settings %d)", dir, purge);
  our_command(dir, cmd, sizeof(cmd));

  // registry first: associations back to what they were, then our own keys
  assoc_apply(dir, none);
  context_set(0, cmd);
  reg_del_tree(HKEY_CLASSES_ROOT, LEGACY_PROGID);
  created_cleanup();
  delete_link_value("StartMenuLnk");
  delete_link_value("DesktopLnk");
  if (reg_get(HKEY_LOCAL_MACHINE, KEY_APP, "Files", files, sizeof(files)) != 1) files[0] = 0;
  reg_del_tree(HKEY_LOCAL_MACHINE, KEY_UNINST);
  reg_del_tree(HKEY_LOCAL_MACHINE, KEY_APP);
  notify_shell();

  // program files (the recorded list + what this version knows), leftovers of an interrupted install
  {
    char* s = files;
    while (*s) {
      char* e = s;
      char name[MAX_PATH];
      while (*e && *e != ';') e++;
      lstrcpynA(name, s, (int)(e - s) + 1 < MAX_PATH ? (int)(e - s) + 1 : MAX_PATH);
      if (name[0] && !strchr(name, '\\') && !(defer_self && lstrcmpiA(name, "uninst.exe") == 0)) {
        p_join(dir, name, path, sizeof(path));
        delete_file(path, &reboot);
      }
      s = *e ? e + 1 : e;
    }
  }
  for (i = 0; i < NPAY; i++) {
    if (!(defer_self && lstrcmpiA(g_pay[i].name, "uninst.exe") == 0)) {
      p_join(dir, g_pay[i].name, path, sizeof(path));
      delete_file(path, &reboot);
    }
    p_join(dir, g_pay[i].tmp, path, sizeof(path));
    delete_file(path, &reboot);
  }
  if (purge) purge_settings(dir, &reboot);
  wininit_flush();
  if (!defer_self) uninst_finish(dir);
  if (kept_folder) *kept_folder = p_is_dir(dir);
  slog("uninstall finished: %s", reboot ? "restart needed" : "ok");
  return reboot ? RC_REBOOT : RC_OK;
}

int uninst_finish(const char* dir)
{
  char path[MAX_PATH];
  int reboot = 0, tries;
  p_join(dir, "uninst.exe", path, sizeof(path));
  for (tries = 0; tries < 50 && p_exists(path); tries++) {
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileA(path)) { slog("deleted %s", path); break; }
    Sleep(100);
  }
  if (p_exists(path)) delete_file(path, &reboot);
  if (RemoveDirectoryA(dir)) slog("deleted folder %s", dir);
  else if (p_is_dir(dir)) slog("folder kept (not empty): %s", dir);
  return reboot ? RC_REBOOT : RC_OK;
}
