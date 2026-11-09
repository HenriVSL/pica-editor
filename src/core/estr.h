// estr.h - string helpers (DESIGN 6.3). Owner after Wave 0: overseer.
#ifndef PICA_ESTR_H
#define PICA_ESTR_H

#include "etypes.h"
#include "emem.h"
#include <stdarg.h>

u32   es_len(const char* s);
void  es_copy(char* dst, u32 cap, const char* src);        // truncates, always NUL-terminates
void  es_cat(char* dst, u32 cap, const char* src);
int   es_icmp(const char* a, const char* b);                // ASCII case-insensitive
int   es_nicmp(const char* a, const char* b, u32 n);
// %s %c %d %u %x %X %%, width, '0' and '-' flags; also: 'l' length modifier (long args), ".N" precision for %s.
// Returns the length written (always < cap; output truncated to fit, always NUL-terminated when cap > 0).
int   es_fmt(char* dst, u32 cap, const char* fmt, ...);
int   es_vfmt(char* dst, u32 cap, const char* fmt, va_list ap);
u32   es_u64_to_dec(u64 v, char* out);                      // out >= 21 bytes
char* es_trim(char* s);                                     // in place
int   es_kv(char* line, char** key, char** val);            // "key=value" (both trimmed); 0 if not a key line
int   es_parse_u32(const char* s, u32* out, int base);      // base 0: "0x1F", "1Fh" hex, else decimal; E_SYNTAX/E_RANGE
int   es_parse_i32(const char* s, i32* out);
int   es_wildcard(const char* mask, const char* name);      // DOS * and ?, case-insensitive; "*.*" matches all
u32   es_utf8_encode(ecp c, char* out);                     // 1..4 bytes
int   es_utf8_to_ecp(const char* s, u32 n, Vec<ecp>* out);  // lenient (bad bytes -> ECP_BADBYTE); appends
class EStr {
public:
  EStr(); ~EStr();
  const char* c_str() const;  u32 len() const;               // c_str() is never NULL ("" when empty)
  int  append(const char* s);  int appendn(const char* s, u32 n);  int appendf(const char* fmt, ...);  // appendf: <= 1023 chars per call
  void clear();  char* detach();    // caller frees with e_free
private:
  char* p_; u32 n_; u32 cap_;
  EStr(const EStr&); EStr& operator=(const EStr&);
};

#endif
