// tf.h - tiny test framework for coretest (DESIGN 17.1). Owner after Wave 0: overseer.
// Suites are functions `void suite_<name>(TF* tf)` in tests/t_<name>.cpp. The check macros use the parameter `tf`.
#ifndef PICA_TF_H
#define PICA_TF_H

#include "etypes.h"
#include "emem.h"
#include "estr.h"

struct TF {
  const char* suite;      // current suite name
  u32 checks, failed;     // counts for the current suite
  int quiet;              // -q: print only failures and the summary
  int long_run;           // -long: bigger random runs
  u32 seed;               // -seed N (default 1); suites seed their own generators from it
  const char* data_dir;   // -data DIR (default: tests/data relative to the working directory)
  const char* tmp_dir;    // -tmp DIR  (default: tests/tmp); created by the runner
  u32 rng;                // state of tf_rand
};

void tf_check(TF* tf, int ok, const char* file, int line, const char* expr);
void tf_eq_u(TF* tf, u64 a, u64 b, const char* file, int line, const char* ea, const char* eb);
void tf_eq_s(TF* tf, const char* a, const char* b, const char* file, int line, const char* ea, const char* eb);
void tf_eq_mem(TF* tf, const void* a, const void* b, u32 n, const char* file, int line, const char* ea, const char* eb);
void tf_log(TF* tf, const char* fmt, ...);        // printed unless -q
void tf_fail(TF* tf, const char* file, int line, const char* fmt, ...);   // counts a failed check with a message
u32  tf_rand(TF* tf);                             // deterministic xorshift32, seeded from -seed per suite
u32  tf_rand_below(TF* tf, u32 n);                // 0..n-1 (n > 0)
void tf_path(TF* tf, const char* name, char* out, u32 cap);   // tmp_dir + separator + name

#define TF_CHECK(cond)     tf_check(tf, (cond) ? 1 : 0, __FILE__, __LINE__, #cond)
#define TF_EQ_U(a, b)      tf_eq_u(tf, (u64)(a), (u64)(b), __FILE__, __LINE__, #a, #b)
#define TF_EQ_S(a, b)      tf_eq_s(tf, (a), (b), __FILE__, __LINE__, #a, #b)
#define TF_EQ_MEM(a, b, n) tf_eq_mem(tf, (a), (b), (u32)(n), __FILE__, __LINE__, #a, #b)
#define TF_EQ_I(a, b)      tf_eq_u(tf, (u64)(i64)(a), (u64)(i64)(b), __FILE__, __LINE__, #a, #b)

#endif
