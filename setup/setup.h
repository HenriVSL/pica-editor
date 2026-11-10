// setup.h - Pica installer (T19): shared declarations of setup.cpp (UI, flow) and inst.cpp (operations).
// Include <windows.h> first. VC6 dialect (DESIGN 5.3), ANSI APIs only, everything Win98-safe.
#ifndef PICA_SETUP_H
#define PICA_SETUP_H

// exit codes (also in docs/INSTALLER.md)
enum {
  RC_OK = 0,          // done
  RC_FAIL = 1,        // error, bad switch or cancelled
  RC_RUNNING = 2,     // Pica is running (silent mode never waits and never kills it)
  RC_REBOOT = 3,      // done, but files were in use: WININIT.INI replaces them when Windows restarts
  RC_DOWNGRADE = 4    // a newer Pica is installed (silent mode without /FORCE)
};

// file-type groups of the wizard's "File types" page
enum { G_TEXT, G_WEB, G_SRC, G_SCRIPT, G_COUNT };

struct ExtDef {
  const char* ext;    // ".txt"
  int group;          // G_*
  int def;            // ticked by default
};
extern const ExtDef g_exts[];
extern const int g_next;
enum { MAX_EXT = 32 };

struct Opts {
  char dir[MAX_PATH];     // install folder (no trailing backslash)
  int startmenu;          // Programs\Pica.lnk
  int desktop;            // Desktop\Pica.lnk
  int context;            // HKCR\*\shell\Pica
  int types[MAX_EXT];     // per g_exts entry
  // switches
  int silent, force, purge, locktest, run_after, uninstall, dir_given;
  DWORD child_pid;        // /_CHILD=<pid>: the temp copy of uninst.exe started by the installed one
  char log[MAX_PATH];
};

struct InstallInfo {
  int installed;          // HKLM\Software\Pica\InstallDir present
  char dir[MAX_PATH];
  int have_ver;
  DWORD ms, ls;           // version of the installed pica.exe (resource, else the registry string)
  char ver[32];
};

// strings and paths
void s_copy(char* dst, int cap, const char* src);
void s_cat(char* dst, int cap, const char* src);
void p_join(const char* dir, const char* name, char* out, int cap);
int  p_exists(const char* path);
int  p_is_dir(const char* path);
void p_module_dir(char* out, int cap);
int  p_same(const char* a, const char* b);           // same file/folder (short-path compare)
int  p_normalize_dir(const char* in, char* out, int cap);   // absolute, no trailing '\', not a root

// log (always on: /LOG=<file>, default %TEMP%\PICASET.LOG)
void log_open(const char* path, int truncate);
void log_close();
void slog(const char* fmt, ...);

// versions
void ver_str(DWORD ms, DWORD ls, char* out);         // "1.0", "1.2.3"
int  ver_cmp(DWORD ams, DWORD als, DWORD bms, DWORD bls);
int  payload_version(DWORD* ms, DWORD* ls);          // from the embedded pica.exe's VS_FIXEDFILEINFO
int  file_version(const char* path, DWORD* ms, DWORD* ls);
int  has_payload();
void detect_install(InstallInfo* ii);
void opts_defaults(Opts* o, const InstallInfo* ii);  // the stored choices of an existing install, else defaults
void opts_default_types(Opts* o);
int  pica_running();
int  settings_exist(const char* dir);

// operations
typedef void (*ProgressFn)(int pct, const char* text, void* ctx);
int  inst_run(const Opts* o, ProgressFn fn, void* ctx, char* err, int errcap);
// defer_self: the caller deletes uninst.exe and removes the folder later (uninst_finish)
int  uninst_run(const char* dir, int purge, int defer_self, int* kept_folder);
int  uninst_finish(const char* dir);                 // delete uninst.exe, remove the folder if empty
int  wininit_delete_later(const char* path);         // NUL=<short path> in WININIT.INI [rename]
void notify_shell();

#endif
