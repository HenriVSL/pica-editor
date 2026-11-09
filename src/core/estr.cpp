// estr.cpp - string helpers (DESIGN 6.3). Owner after Wave 0: overseer.
#include "estr.h"
#include <string.h>

u32 es_len(const char* s)
{
  return s ? (u32)strlen(s) : 0;
}

void es_copy(char* dst, u32 cap, const char* src)
{
  u32 i;
  if (!dst || cap == 0) return;
  if (!src) src = "";
  for (i = 0; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

void es_cat(char* dst, u32 cap, const char* src)
{
  u32 n;
  if (!dst || cap == 0) return;
  n = 0;
  while (n < cap && dst[n]) n++;
  if (n >= cap) { dst[cap - 1] = 0; return; }
  es_copy(dst + n, cap - n, src);
}

static int lower_ascii(int c)
{
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int es_icmp(const char* a, const char* b)
{
  int ca, cb;
  if (!a) a = "";
  if (!b) b = "";
  for (;;) {
    ca = lower_ascii((u8)*a++);
    cb = lower_ascii((u8)*b++);
    if (ca != cb) return ca - cb;
    if (!ca) return 0;
  }
}

int es_nicmp(const char* a, const char* b, u32 n)
{
  int ca, cb;
  if (!a) a = "";
  if (!b) b = "";
  while (n--) {
    ca = lower_ascii((u8)*a++);
    cb = lower_ascii((u8)*b++);
    if (ca != cb) return ca - cb;
    if (!ca) return 0;
  }
  return 0;
}

// ---- formatting ----

struct FmtOut { char* d; u32 cap; u32 n; };

static void fo_put(FmtOut* o, char c)
{
  if (o->cap && o->n + 1 < o->cap) o->d[o->n++] = c;
}

static void fo_field(FmtOut* o, const char* s, u32 len, int width, int left, char pad, int sign_first)
{
  u32 i;
  int fill = width > (int)len ? width - (int)len : 0;
  if (left) {
    for (i = 0; i < len; i++) fo_put(o, s[i]);
    while (fill-- > 0) fo_put(o, ' ');
    return;
  }
  if (pad == '0' && sign_first && len > 0) {   // "-0042": sign before the zeros
    fo_put(o, s[0]);
    s++; len--;
  }
  while (fill-- > 0) fo_put(o, pad);
  for (i = 0; i < len; i++) fo_put(o, s[i]);
}

int es_vfmt(char* dst, u32 cap, const char* fmt, va_list ap)
{
  FmtOut o;
  char num[40];
  o.d = dst; o.cap = cap; o.n = 0;
  if (!fmt) fmt = "";
  while (*fmt) {
    int left = 0, width = 0, prec = -1, is_long = 0;
    char pad = ' ';
    char conv;
    if (*fmt != '%') { fo_put(&o, *fmt++); continue; }
    fmt++;
    for (;;) {
      if (*fmt == '-') { left = 1; fmt++; }
      else if (*fmt == '0') { pad = '0'; fmt++; }
      else break;
    }
    if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
    else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
      else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
    }
    if (*fmt == 'l') { is_long = 1; fmt++; }
    conv = *fmt;
    if (!conv) break;
    fmt++;
    switch (conv) {
    case 's': {
      const char* s = va_arg(ap, const char*);
      u32 len;
      if (!s) s = "(null)";
      len = 0;
      while (s[len] && (prec < 0 || (int)len < prec)) len++;
      fo_field(&o, s, len, width, left, ' ', 0);
      break;
    }
    case 'c': {
      char c = (char)va_arg(ap, int);
      fo_field(&o, &c, 1, width, left, ' ', 0);
      break;
    }
    case 'd': case 'i': {
      i64 v = is_long ? (i64)va_arg(ap, long) : (i64)va_arg(ap, int);
      u64 u;
      u32 len = 0;
      int neg = v < 0;
      char tmp[24];
      u = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
      es_u64_to_dec(u, tmp);
      if (neg) num[len++] = '-';
      es_copy(num + len, sizeof(num) - len, tmp);
      len = es_len(num);
      fo_field(&o, num, len, width, left, left ? ' ' : pad, neg);
      break;
    }
    case 'u': {
      u64 v = is_long ? (u64)va_arg(ap, unsigned long) : (u64)va_arg(ap, unsigned int);
      u32 len = es_u64_to_dec(v, num);
      fo_field(&o, num, len, width, left, left ? ' ' : pad, 0);
      break;
    }
    case 'x': case 'X': {
      u64 v = is_long ? (u64)va_arg(ap, unsigned long) : (u64)va_arg(ap, unsigned int);
      const char* digits = conv == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
      char tmp[24];
      u32 len = 0, i;
      do { tmp[len++] = digits[(u32)(v & 15)]; v >>= 4; } while (v);
      for (i = 0; i < len; i++) num[i] = tmp[len - 1 - i];
      fo_field(&o, num, len, width, left, left ? ' ' : pad, 0);
      break;
    }
    case '%':
      fo_put(&o, '%');
      break;
    default:                      // unknown conversion: copy it literally
      fo_put(&o, '%');
      fo_put(&o, conv);
      break;
    }
  }
  if (o.cap) o.d[o.n] = 0;
  return (int)o.n;
}

int es_fmt(char* dst, u32 cap, const char* fmt, ...)
{
  int n;
  va_list ap;
  va_start(ap, fmt);
  n = es_vfmt(dst, cap, fmt, ap);
  va_end(ap);
  return n;
}

u32 es_u64_to_dec(u64 v, char* out)
{
  char tmp[24];
  u32 n = 0, i;
  do { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
  for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = 0;
  return n;
}

static int is_ws(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

char* es_trim(char* s)
{
  u32 a, n;
  if (!s) return s;
  a = 0;
  while (s[a] && is_ws(s[a])) a++;
  n = es_len(s + a);
  while (n > 0 && is_ws(s[a + n - 1])) n--;
  if (a) memmove(s, s + a, n);
  s[n] = 0;
  return s;
}

int es_kv(char* line, char** key, char** val)
{
  char* eq;
  char* k;
  if (!line) return 0;
  k = line;
  while (*k && is_ws(*k)) k++;
  if (!*k || *k == ';' || *k == '#' || *k == '[') return 0;
  eq = strchr(k, '=');
  if (!eq) return 0;
  *eq = 0;
  es_trim(k);
  if (!*k) { *eq = '='; return 0; }
  if (key) *key = k;
  if (val) *val = es_trim(eq + 1);
  return 1;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int es_parse_u32(const char* s, u32* out, int base)
{
  const char* b;
  const char* e;
  u64 v = 0;
  int hex = 0;
  if (!s) return E_SYNTAX;
  b = s;
  while (*b && is_ws(*b)) b++;
  e = b + es_len(b);
  while (e > b && is_ws(e[-1])) e--;
  if (b == e) return E_SYNTAX;
  if (base == 16 || base == 0) {
    if (e - b > 2 && b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) { hex = 1; b += 2; }
    else if (e - b > 1 && (e[-1] == 'h' || e[-1] == 'H')) {
      const char* q;
      int all = 1;
      for (q = b; q < e - 1; q++) if (hexval(*q) < 0) { all = 0; break; }
      if (all) { hex = 1; e--; }
    }
    if (base == 16) hex = 1;
  } else if (base != 10) {
    return E_SYNTAX;
  }
  if (b == e) return E_SYNTAX;
  for (; b < e; b++) {
    int d = hex ? hexval(*b) : ((*b >= '0' && *b <= '9') ? *b - '0' : -1);
    if (d < 0) return E_SYNTAX;
    v = v * (hex ? 16 : 10) + (u32)d;
    if (v > 0xFFFFFFFFu) return E_RANGE;
  }
  if (out) *out = (u32)v;
  return E_OK;
}

int es_parse_i32(const char* s, i32* out)
{
  const char* b;
  const char* e;
  i64 v = 0;
  int neg = 0;
  if (!s) return E_SYNTAX;
  b = s;
  while (*b && is_ws(*b)) b++;
  e = b + es_len(b);
  while (e > b && is_ws(e[-1])) e--;
  if (b < e && (*b == '-' || *b == '+')) { neg = *b == '-'; b++; }
  if (b == e) return E_SYNTAX;
  for (; b < e; b++) {
    if (*b < '0' || *b > '9') return E_SYNTAX;
    v = v * 10 + (*b - '0');
    if (v > (i64)0x80000000u) return E_RANGE;
  }
  if (neg) v = -v;
  if (v > 2147483647 || v < -2147483647 - 1) return E_RANGE;
  if (out) *out = (i32)v;
  return E_OK;
}

int es_wildcard(const char* mask, const char* name)
{
  const char* m;
  const char* n;
  const char* star_m = 0;
  const char* star_n = 0;
  if (!mask || !name) return 0;
  if (es_icmp(mask, "*.*") == 0 || es_icmp(mask, "*") == 0) return 1;
  m = mask; n = name;
  while (*n) {
    if (*m == '*') { star_m = ++m; star_n = n; continue; }
    if (*m && (*m == '?' || lower_ascii((u8)*m) == lower_ascii((u8)*n))) { m++; n++; continue; }
    if (star_m) { m = star_m; n = ++star_n; continue; }
    return 0;
  }
  while (*m == '*') m++;
  if (!*m) return 1;
  // DOS: "name.*" also matches "name" (no extension)
  if (m[0] == '.' && m[1] == '*' && !strchr(name, '.')) {
    m += 2;
    while (*m == '*') m++;
    return *m == 0;
  }
  return 0;
}

u32 es_utf8_encode(ecp c, char* out)
{
  if (c >= 0x110000u && c < 0x110100u) { out[0] = (char)(u8)(c - 0x110000u); return 1; }   // bad byte: raw
  if (c >= 0x110000u || (c >= 0xD800u && c <= 0xDFFFu)) c = ECP_REPLACEMENT;
  if (c < 0x80) { out[0] = (char)c; return 1; }
  if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
  if (c < 0x10000) {
    out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}

// Strict UTF-8 decode of one sequence at p (n bytes available, nothing follows). Returns the length of a valid
// sequence (1..4) and sets *c, or 0 if the bytes at p do not start a valid complete sequence.
static u32 utf8_strict(const u8* p, u32 n, ecp* c)
{
  u8 b0 = p[0];
  u32 len, i;
  ecp v;
  u8 lo = 0x80, hi = 0xBF;
  if (b0 < 0x80) { *c = b0; return 1; }
  if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; v = b0 & 0x1F; }
  else if (b0 >= 0xE0 && b0 <= 0xEF) {
    len = 3; v = b0 & 0x0F;
    if (b0 == 0xE0) lo = 0xA0;
    if (b0 == 0xED) hi = 0x9F;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    len = 4; v = b0 & 0x07;
    if (b0 == 0xF0) lo = 0x90;
    if (b0 == 0xF4) hi = 0x8F;
  } else return 0;
  if (n < len) return 0;
  for (i = 1; i < len; i++) {
    u8 b = p[i];
    if (i == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) return 0;
    v = (v << 6) | (b & 0x3F);
  }
  *c = v;
  return len;
}

int es_utf8_to_ecp(const char* s, u32 n, Vec<ecp>* out)
{
  const u8* p = (const u8*)s;
  u32 i = 0;
  if (!out) return E_RANGE;
  if (!s) return E_OK;
  if (out->reserve(out->size() + n) != E_OK) return E_NOMEM;
  while (i < n) {
    ecp c;
    u32 k = utf8_strict(p + i, n - i, &c);
    if (k == 0) { c = ECP_BADBYTE(p[i]); k = 1; }
    if (out->push(c) != E_OK) return E_NOMEM;
    i += k;
  }
  return E_OK;
}

// ---- EStr ----

EStr::EStr() : p_(0), n_(0), cap_(0) {}

EStr::~EStr()
{
  if (p_) e_free(p_);
}

const char* EStr::c_str() const
{
  return p_ ? p_ : "";
}

u32 EStr::len() const
{
  return n_;
}

int EStr::appendn(const char* s, u32 n)
{
  if (!s || n == 0) return E_OK;
  if (n > 0xFFFFFFF0u - n_) return E_NOMEM;
  if (n_ + n + 1 > cap_) {
    u32 nc = cap_ ? cap_ : 32;
    char* np;
    while (nc < n_ + n + 1) nc = nc > 0x7FFFFFFFu ? n_ + n + 1 : nc * 2;
    np = (char*)e_realloc(p_, nc);
    if (!np) return E_NOMEM;
    p_ = np; cap_ = nc;
  }
  memcpy(p_ + n_, s, n);
  n_ += n;
  p_[n_] = 0;
  return E_OK;
}

int EStr::append(const char* s)
{
  return appendn(s, es_len(s));
}

int EStr::appendf(const char* fmt, ...)
{
  char buf[1024];
  int n;
  va_list ap;
  va_start(ap, fmt);
  n = es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return appendn(buf, (u32)n);
}

void EStr::clear()
{
  n_ = 0;
  if (p_) p_[0] = 0;
}

char* EStr::detach()
{
  char* r = p_;
  if (!r) {
    r = (char*)e_alloc(1);
    if (r) r[0] = 0;
  }
  p_ = 0; n_ = 0; cap_ = 0;
  return r;
}
