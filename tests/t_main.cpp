// t_main.cpp - coretest runner (DESIGN 17.1, 17.2). Owner after Wave 0: overseer.
//   coretest [suite...] [-q] [-seed N] [-long] [-data DIR] [-tmp DIR]
// Prints one line per failed check and the summary "coretest: N checks, M failed"; exit code = failed suites.
// Leak check: e_alloc_count() must be equal before and after each suite.
// Per-suite builds compile this file with -DTF_SUITE=<name>: the table then holds only suite_<name>, so a suite binary
// links only that suite's sources.
#include "tf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

typedef void (*TfSuiteFn)(TF* tf);
struct TfSuite { const char* name; TfSuiteFn fn; };

#define TF_STR2(x) #x
#define TF_STR(x) TF_STR2(x)
#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)

#ifdef TF_SUITE
void TF_CAT(suite_, TF_SUITE)(TF* tf);
static const TfSuite g_suites[] = { { TF_STR(TF_SUITE), TF_CAT(suite_, TF_SUITE) } };
#else
void suite_estr(TF*); void suite_emem(TF*); void suite_ptree(TF*); void suite_undo(TF*); void suite_doc(TF*);
void suite_marks(TF*); void suite_codec(TF*); void suite_detect(TF*); void suite_ucd(TF*); void suite_layout(TF*);
void suite_lexer(TF*); void suite_regex(TF*); void suite_search(TF*); void suite_fif(TF*); void suite_hexfmt(TF*);
void suite_edtype(TF*); void suite_edlines(TF*); void suite_edhex(TF*); void suite_ini(TF*); void suite_config(TF*);
void suite_session(TF*); void suite_fileops(TF*); void suite_adversarial(TF*);
#if defined(_WIN32)
void suite_winenc(TF*);
#endif
static const TfSuite g_suites[] = {
  { "estr", suite_estr }, { "emem", suite_emem }, { "ptree", suite_ptree }, { "undo", suite_undo },
  { "doc", suite_doc }, { "marks", suite_marks }, { "codec", suite_codec }, { "detect", suite_detect },
  { "ucd", suite_ucd }, { "layout", suite_layout }, { "lexer", suite_lexer }, { "regex", suite_regex },
  { "search", suite_search }, { "fif", suite_fif }, { "hexfmt", suite_hexfmt }, { "edtype", suite_edtype },
  { "edlines", suite_edlines }, { "edhex", suite_edhex }, { "ini", suite_ini }, { "config", suite_config },
  { "session", suite_session }, { "fileops", suite_fileops },
#if defined(_WIN32)
  { "winenc", suite_winenc },
#endif
  { "adversarial", suite_adversarial }
};
#endif

static u32 g_total_checks = 0, g_total_failed = 0;

static void report_fail(TF* tf, const char* file, int line, const char* msg)
{
  tf->failed++;
  printf("FAIL %s: %s:%d: %s\n", tf->suite ? tf->suite : "?", file ? file : "?", line, msg);
  fflush(stdout);
}

void tf_check(TF* tf, int ok, const char* file, int line, const char* expr)
{
  tf->checks++;
  if (!ok) report_fail(tf, file, line, expr);
}

void tf_eq_u(TF* tf, u64 a, u64 b, const char* file, int line, const char* ea, const char* eb)
{
  char sa[24], sb[24], msg[400];
  tf->checks++;
  if (a == b) return;
  es_u64_to_dec(a, sa);
  es_u64_to_dec(b, sb);
  es_fmt(msg, sizeof(msg), "%s == %s (%s vs %s; as i32: %d vs %d)", ea, eb, sa, sb, (int)(i32)(u32)a, (int)(i32)(u32)b);
  report_fail(tf, file, line, msg);
}

void tf_eq_s(TF* tf, const char* a, const char* b, const char* file, int line, const char* ea, const char* eb)
{
  char msg[600];
  tf->checks++;
  if (a && b && strcmp(a, b) == 0) return;
  if (!a && !b) return;
  es_fmt(msg, sizeof(msg), "%s == %s (\"%.200s\" vs \"%.200s\")", ea, eb, a ? a : "(null)", b ? b : "(null)");
  report_fail(tf, file, line, msg);
}

void tf_eq_mem(TF* tf, const void* a, const void* b, u32 n, const char* file, int line, const char* ea, const char* eb)
{
  char msg[300];
  u32 i;
  const u8* pa = (const u8*)a;
  const u8* pb = (const u8*)b;
  tf->checks++;
  if (n == 0 || (a && b && memcmp(a, b, n) == 0)) return;
  if (!a || !b) { es_fmt(msg, sizeof(msg), "%s == %s (NULL pointer)", ea, eb); report_fail(tf, file, line, msg); return; }
  for (i = 0; i < n && pa[i] == pb[i]; i++) {}
  es_fmt(msg, sizeof(msg), "%s == %s (%u bytes; first difference at %u: %02X vs %02X)", ea, eb, n, i, (u32)pa[i], (u32)pb[i]);
  report_fail(tf, file, line, msg);
}

void tf_log(TF* tf, const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  if (tf->quiet) return;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  printf("  [%s] %s\n", tf->suite ? tf->suite : "?", buf);
  fflush(stdout);
}

void tf_fail(TF* tf, const char* file, int line, const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  tf->checks++;
  report_fail(tf, file, line, buf);
}

u32 tf_rand(TF* tf)
{
  u32 x = tf->rng ? tf->rng : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  tf->rng = x;
  return x;
}

u32 tf_rand_below(TF* tf, u32 n)
{
  return n ? tf_rand(tf) % n : 0;
}

void tf_path(TF* tf, const char* name, char* out, u32 cap)
{
#if defined(_WIN32)
  const char* sep = "\\";
#else
  const char* sep = "/";
#endif
  es_copy(out, cap, tf->tmp_dir);
  es_cat(out, cap, sep);
  es_cat(out, cap, name);
}

static void mkdir_one(const char* p)
{
#if defined(_WIN32)
  _mkdir(p);
#else
  mkdir(p, 0777);
#endif
}

static void mkdir_p(const char* path)
{
  char buf[512];
  u32 i, n;
  es_copy(buf, sizeof(buf), path);
  n = es_len(buf);
  for (i = 1; i < n; i++) {
    if ((buf[i] == '/' || buf[i] == '\\') && buf[i - 1] != ':') {
      char c = buf[i];
      buf[i] = 0;
      mkdir_one(buf);
      buf[i] = c;
    }
  }
  mkdir_one(buf);
}

static u32 name_hash(const char* s)
{
  u32 h = 2166136261u;
  while (*s) { h ^= (u8)*s++; h *= 16777619u; }
  return h;
}

int main(int argc, char** argv)
{
  TF tf;
  const char* names[64];
  u32 nnames = 0, i, nsuites = (u32)(sizeof(g_suites) / sizeof(g_suites[0]));
  int failed_suites = 0, a;
  memset(&tf, 0, sizeof(tf));
  tf.seed = 1;
  tf.data_dir = "tests/data";
  tf.tmp_dir = "tests/tmp";
  for (a = 1; a < argc; a++) {
    const char* s = argv[a];
    if (strcmp(s, "-q") == 0) tf.quiet = 1;
    else if (strcmp(s, "-long") == 0) tf.long_run = 1;
    else if (strcmp(s, "-seed") == 0 && a + 1 < argc) { u32 v; if (es_parse_u32(argv[++a], &v, 0) == E_OK) tf.seed = v; }
    else if (strcmp(s, "-data") == 0 && a + 1 < argc) tf.data_dir = argv[++a];
    else if (strcmp(s, "-tmp") == 0 && a + 1 < argc) tf.tmp_dir = argv[++a];
    else if (s[0] == '-') { printf("coretest: unknown option %s\n", s); return 100; }
    else if (nnames < 64) names[nnames++] = s;
  }
  mkdir_p(tf.tmp_dir);
  for (a = 0; a < (int)nnames; a++) {
    int found = 0;
    for (i = 0; i < nsuites; i++) if (strcmp(names[a], g_suites[i].name) == 0) found = 1;
    if (!found) { printf("coretest: unknown suite %s\n", names[a]); failed_suites++; }
  }
  for (i = 0; i < nsuites; i++) {
    u32 before, after;
    int selected = nnames == 0;
    for (a = 0; a < (int)nnames; a++) if (strcmp(names[a], g_suites[i].name) == 0) selected = 1;
    if (!selected) continue;
    tf.suite = g_suites[i].name;
    tf.checks = 0; tf.failed = 0;
    tf.rng = (tf.seed * 2654435761u) ^ name_hash(tf.suite);
    if (!tf.rng) tf.rng = 1;
    e_test_fail_after(-1);
    before = e_alloc_count();
    g_suites[i].fn(&tf);
    e_test_fail_after(-1);
    after = e_alloc_count();
    if (after != before) tf_fail(&tf, __FILE__, __LINE__, "leak check: %d blocks", (int)(after - before));
    if (!tf.quiet || tf.failed) printf("suite %s: %u checks, %u failed\n", tf.suite, tf.checks, tf.failed);
    g_total_checks += tf.checks;
    g_total_failed += tf.failed;
    if (tf.failed) failed_suites++;
  }
  printf("coretest: %u checks, %u failed\n", g_total_checks, g_total_failed);
  return failed_suites;
}
