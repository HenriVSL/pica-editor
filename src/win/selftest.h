// selftest.h - pica.exe -selftest framework (DESIGN 17.3). Framework in selftest.cpp (T00 basics, T15 completes).
// Each win module owns one st_<module> function in its own .cpp. Owner after Wave 0: overseer.
#ifndef PICA_SELFTEST_H
#define PICA_SELFTEST_H

#include <windows.h>
#include <stdio.h>
#include "etypes.h"
#include "emem.h"

struct Tab;
struct StCtx {
  FILE* log;
  char cfg_dir[260];        // -cfg (fixtures go to <cfg_dir>\fixtures unless -fixdir)
  char fixdir[260];         // fixture folder (no trailing backslash)
  const char* current;      // name of the running st_ function
  u32 checks, failed;       // in the running st_ function
  int skipped;              // the running function called st_skip
  u32 tests_passed, tests_failed, tests_skipped;
};
int  selftest_run(const char* names);           // names separated by spaces/commas; empty = all; returns failures
void selftest_set_paths(const char* log_path, const char* fixdir);   // -log / -fixdir (empty = defaults); before run
int  selftest_active();
void selftest_log(const char* fmt, ...);        // log line (no-op outside selftest mode)
// helpers (17.3)
void st_skip(StCtx* c, const char* why);        // logs "SKIP st_x (why)"
void st_check(StCtx* c, int ok, const char* file, int line, const char* expr, const char* msg);
void st_checkf(StCtx* c, int ok, const char* fmt, ...);   // printf-style message, no file/line
#define ST_CHECK(c, cond, msg) st_check((c), (cond) ? 1 : 0, __FILE__, __LINE__, #cond, (msg))
int  st_fixture(StCtx* c, const char* name, const void* bytes, u32 n, char* path_out, u32 cap);
Tab* st_open(StCtx* c, const char* path);       // docio_open + pump; NULL on failure (logged)
void st_cmd(StCtx* c, int id);                  // app_command
void st_keys(StCtx* c, const char* keys);       // "^s" Ctrl+S, "+{RIGHT}" Shift+Right, "%f" Alt+F, "{ENTER}", "abc"
void st_pump(StCtx* c, u32 ms);                 // run the message loop for ms milliseconds
int  st_wait_idle(StCtx* c, u32 timeout_ms);    // until the I/O worker, indexer, FiF and highlighter are idle
void st_answer(StCtx* c, int qid, int button);  // queue an answer for ui_ask
int  st_file_bytes(StCtx* c, const char* path, EBytes* out);
// module selftests
void st_frame(StCtx* c);      // frame.cpp (T07)
void st_textview(StCtx* c);   // textview.cpp (T08)
void st_docio(StCtx* c);      // docio.cpp (T09)
void st_hexview(StCtx* c);    // hexview.cpp (T10)
void st_search(StCtx* c);     // dlgfind.cpp (T11)
void st_fif(StCtx* c);        // fifpanel.cpp (T11)
void st_edit(StCtx* c);       // editcmds.cpp (T12)
void st_dialogs(StCtx* c);    // dlgmisc.cpp (T12)
void st_session(StCtx* c);    // sessmgr.cpp (T13)
void st_watch(StCtx* c);      // watch.cpp (T14)
void st_largefile(StCtx* c);  // largefile.cpp (T14)
void st_scenarios(StCtx* c);  // selftest.cpp (T15)

#endif
