// shtest.cpp - T19 test helper for the installer (console, not shipped). Drives the Windows shell the way Explorer
// does and reports what happened, so association tests run through agent98 without clicking:
//   shtest exec <verb|-> <file> [wait]   ShellExecuteEx(verb, file); "wait" waits for the process to end
//   shtest waitpica <text> [secs]        waits for a Pica frame whose title contains <text>, prints the title
//   shtest titles                        prints the titles of all Pica frames
//   shtest close [secs]                  WM_CLOSE to every Pica frame, waits until none is left
//   shtest ver <file>                    prints the file version
//   shtest assoc <.ext>...               prints HKCR\.ext, its ProgID's open/edit commands, HKCR\*\shell\Pica
//   shtest key <HKCR|HKLM> <path>        exit 0 if the key exists (prints "exists"/"absent")
//   shtest waitfile <file> [secs]        waits for a file to exist
//   shtest wininit                       prints <windows>\WININIT.INI
//   shtest typename <file>               SHGetFileInfo type name (Explorer's Type column)
//   shtest verbs <.ext>                  every verb of the extension's ProgID with its command, and ShellNew
//   shtest reboot                        ExitWindowsEx(EWX_REBOOT | EWX_FORCE) (WININIT.INI test)
// Exit code 0 = as expected, 1 = not.
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "appinfo.h"

static char g_find[300];
static char g_found[400];

static BOOL CALLBACK enum_cb(HWND w, LPARAM lp)
{
  char cls[64], title[400];
  (void)lp;
  if (!GetClassNameA(w, cls, sizeof(cls)) || lstrcmpA(cls, APP_CLASS_FRAME) != 0) return TRUE;
  GetWindowTextA(w, title, sizeof(title));
  if (!g_find[0]) { printf("frame %08lX: %s\n", (DWORD)w, title); return TRUE; }
  if (strstr(title, g_find)) { lstrcpynA(g_found, title, sizeof(g_found)); return FALSE; }
  return TRUE;
}

static void reg_print(HKEY root, const char* key)
{
  char val[600];
  LONG n = sizeof(val);
  if (RegQueryValueA(root, key, val, &n) == ERROR_SUCCESS) printf("  %s = \"%s\"\n", key, val);
  else printf("  %s: (none)\n", key);
}

int main(int argc, char** argv)
{
  int secs, i;
  if (argc < 2) { printf("usage: see shtest.cpp\n"); return 1; }
  if (!lstrcmpiA(argv[1], "exec") && argc >= 4) {
    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = lstrcmpA(argv[2], "-") ? argv[2] : 0;
    sei.lpFile = argv[3];
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei)) { printf("exec failed: error %lu, hInstApp %ld\n", GetLastError(), (long)sei.hInstApp); return 1; }
    printf("exec ok (%s %s)\n", argv[2], argv[3]);
    if (sei.hProcess) {
      if (argc >= 5 && !lstrcmpiA(argv[4], "wait")) {
        DWORD r = WaitForSingleObject(sei.hProcess, 30000);
        printf("process %s\n", r == WAIT_OBJECT_0 ? "ended" : "still running after 30 s");
      }
      CloseHandle(sei.hProcess);
    }
    return 0;
  }
  if (!lstrcmpiA(argv[1], "waitpica") && argc >= 3) {
    secs = argc >= 4 ? atoi(argv[3]) : 20;
    lstrcpynA(g_find, argv[2], sizeof(g_find));
    for (i = 0; i < secs * 10; i++) {
      g_found[0] = 0;
      EnumWindows(enum_cb, 0);
      if (g_found[0]) { printf("pica title: %s\n", g_found); return 0; }
      Sleep(100);
    }
    printf("no Pica window with \"%s\" after %d s\n", argv[2], secs);
    g_find[0] = 0;
    EnumWindows(enum_cb, 0);
    return 1;
  }
  if (!lstrcmpiA(argv[1], "titles")) {
    g_find[0] = 0;
    EnumWindows(enum_cb, 0);
    return 0;
  }
  if (!lstrcmpiA(argv[1], "close")) {
    HWND w;
    secs = argc >= 3 ? atoi(argv[2]) : 20;
    for (i = 0; i < secs * 10; i++) {
      w = FindWindowA(APP_CLASS_FRAME, 0);
      if (!w) { printf("no Pica running\n"); return 0; }
      if (i % 20 == 0) PostMessageA(w, WM_CLOSE, 0, 0);
      Sleep(100);
    }
    printf("Pica still running after %d s\n", secs);
    return 1;
  }
  if (!lstrcmpiA(argv[1], "ver") && argc >= 3) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeA(argv[2], &dummy);
    char* buf;
    VS_FIXEDFILEINFO* f = 0;
    UINT n = 0;
    if (!size) { printf("no version: %s\n", argv[2]); return 1; }
    buf = (char*)malloc(size);
    if (!buf || !GetFileVersionInfoA(argv[2], 0, size, buf) || !VerQueryValueA(buf, (char*)"\\", (void**)&f, &n) || !f) {
      printf("no version: %s\n", argv[2]);
      return 1;
    }
    printf("version %u.%u.%u.%u: %s\n", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
           HIWORD(f->dwFileVersionLS), LOWORD(f->dwFileVersionLS), argv[2]);
    free(buf);
    return 0;
  }
  if (!lstrcmpiA(argv[1], "assoc")) {
    char prog[300], key[400];
    LONG n;
    reg_print(HKEY_CLASSES_ROOT, "*\\shell\\" APP_NAME "\\command");
    for (i = 2; i < argc; i++) {
      n = sizeof(prog);
      if (RegQueryValueA(HKEY_CLASSES_ROOT, argv[i], prog, &n) != ERROR_SUCCESS || !prog[0]) {
        printf("%s: no ProgID\n", argv[i]);
        continue;
      }
      printf("%s -> %s\n", argv[i], prog);
      wsprintfA(key, "%s\\shell\\open\\command", prog);
      reg_print(HKEY_CLASSES_ROOT, key);
      wsprintfA(key, "%s\\shell\\edit\\command", prog);
      reg_print(HKEY_CLASSES_ROOT, key);
    }
    return 0;
  }
  if (!lstrcmpiA(argv[1], "key") && argc >= 4) {
    HKEY h, root = !lstrcmpiA(argv[2], "HKLM") ? HKEY_LOCAL_MACHINE : HKEY_CLASSES_ROOT;
    if (RegOpenKeyExA(root, argv[3], 0, KEY_READ, &h) == ERROR_SUCCESS) {
      RegCloseKey(h);
      printf("exists: %s\\%s\n", argv[2], argv[3]);
      return 0;
    }
    printf("absent: %s\\%s\n", argv[2], argv[3]);
    return 1;
  }
  if (!lstrcmpiA(argv[1], "waitfile") && argc >= 3) {
    secs = argc >= 4 ? atoi(argv[3]) : 20;
    for (i = 0; i < secs * 10; i++) {
      if (GetFileAttributesA(argv[2]) != 0xFFFFFFFF) { printf("found %s\n", argv[2]); return 0; }
      Sleep(100);
    }
    printf("missing %s\n", argv[2]);
    return 1;
  }
  if (!lstrcmpiA(argv[1], "wininit")) {
    char path[MAX_PATH], line[600];
    FILE* f;
    GetWindowsDirectoryA(path, sizeof(path));
    lstrcatA(path, "\\WININIT.INI");
    f = fopen(path, "r");
    if (!f) { printf("no %s\n", path); return 1; }
    while (fgets(line, sizeof(line), f)) printf("%s", line);
    fclose(f);
    return 0;
  }
  if (!lstrcmpiA(argv[1], "typename") && argc >= 3) {
    SHFILEINFOA fi;
    memset(&fi, 0, sizeof(fi));
    if (!SHGetFileInfoA(argv[2], 0, &fi, sizeof(fi), SHGFI_TYPENAME)) { printf("SHGetFileInfo failed\n"); return 1; }
    printf("type name of %s: %s\n", argv[2], fi.szTypeName);
    return 0;
  }
  if (!lstrcmpiA(argv[1], "verbs") && argc >= 3) {
    char prog[300], key[600], verb[260];
    LONG n = sizeof(prog);
    HKEY h;
    DWORD k, vl;
    if (RegQueryValueA(HKEY_CLASSES_ROOT, argv[2], prog, &n) != ERROR_SUCCESS || !prog[0]) { printf("%s: no ProgID\n", argv[2]); return 1; }
    n = sizeof(key);
    if (RegQueryValueA(HKEY_CLASSES_ROOT, prog, key, &n) != ERROR_SUCCESS) key[0] = 0;
    printf("%s -> %s \"%s\"\n", argv[2], prog, key);
    wsprintfA(key, "%s\\shell", prog);
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
      for (k = 0;; k++) {
        char ck[700];
        vl = sizeof(verb);
        if (RegEnumKeyExA(h, k, verb, &vl, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        wsprintfA(ck, "%s\\%s\\command", key, verb);
        reg_print(HKEY_CLASSES_ROOT, ck);
      }
      RegCloseKey(h);
    }
    wsprintfA(key, "%s\\DefaultIcon", prog);
    reg_print(HKEY_CLASSES_ROOT, key);
    wsprintfA(key, "%s\\ShellNew", argv[2]);
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &h) == ERROR_SUCCESS) { printf("  %s exists\n", key); RegCloseKey(h); }
    return 0;
  }
  if (!lstrcmpiA(argv[1], "reboot")) {
    printf("rebooting\n");
    fflush(stdout);
    return ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0) ? 0 : 1;   // FORCE: our own console window would veto
  }
  printf("unknown command %s\n", argv[1]);
  return 1;
}
