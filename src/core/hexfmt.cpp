// hexfmt.cpp - hex formatting, hex byte parsers, go-to parsers, data inspector (DESIGN 6.17). Owner: T06.
#include "hexfmt.h"
#include "estr.h"
#include "ucd.h"
#include <stdio.h>
#include <string.h>

static const char k_hex[] = "0123456789ABCDEF";

void hex_offset_text(epos off, char out[9])
{
  int i;
  for (i = 7; i >= 0; i--) { out[i] = k_hex[off & 15]; off >>= 4; }
  out[8] = 0;
}

void hex_byte_text(u8 b, char out[2]) { out[0] = k_hex[b >> 4]; out[1] = k_hex[b & 15]; }

u16 hex_char_glyph(u8 b, const Codec* sbcs)
{
  ecp cp = 0;
  if (!sbcs || sbcs->kind != EK_SBCS) return (u16)((b >= 0x20 && b < 0x7F) ? b : '.');
  if (b < 0x20 || b == 0x7F) return '.';
  if (sbcs->decode(&b, 1, 1, &cp) != 1) return '.';
  if (ECP_IS_BAD(cp) || cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp > 0xFFFF) return '.';
  if (!uc_is_drawable(cp) || uc_width(cp) != 1) return '.';
  return (u16)cp;
}

// ---- hex byte parsers ----

static inline u32 hx_ch(const char* t, u32 i) { return (u8)t[i]; }
static inline u32 hx_ch(const ecp* t, u32 i) { return t[i]; }

static int hx_val(u32 c)
{
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

static int hx_sep(u32 c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';';
}

// Tokens separated by whitespace, ',' or ';'. A token is an optional "0x" prefix, hex digits (and '?' nibble
// wildcards when mask != NULL), and an optional 'h' suffix (without prefix). Digits are taken in pairs, one byte
// each, in written order ("4d5a" = 4D 5A, "0x4D5A" = 4D 5A); a single digit is a byte only with a prefix or suffix
// ("0x4" = 04, "4h" = 04).
template <class C> static int hx_parse(const C* t, u32 n, EBytes* bytes, EBytes* mask)
{
  u32 i = 0;
  if (!bytes || (!t && n)) return E_SYNTAX;
  bytes->clear();
  if (mask) mask->clear();
  while (i < n) {
    u32 s, e, nd, k;
    int prefix = 0, suffix = 0;
    if (hx_sep(hx_ch(t, i))) { i++; continue; }
    s = i;
    while (i < n && !hx_sep(hx_ch(t, i))) i++;
    e = i;                                          // token [s, e)
    if (e - s >= 2 && hx_ch(t, s) == '0' && (hx_ch(t, s + 1) == 'x' || hx_ch(t, s + 1) == 'X')) { prefix = 1; s += 2; }
    else if (e - s >= 2 && (hx_ch(t, e - 1) == 'h' || hx_ch(t, e - 1) == 'H')) { suffix = 1; e--; }
    nd = e - s;
    if (nd == 0) return E_SYNTAX;
    for (k = s; k < e; k++) {
      u32 c = hx_ch(t, k);
      if (hx_val(c) < 0 && !(mask && c == '?')) return E_SYNTAX;
    }
    if (nd % 2 && !(nd == 1 && (prefix || suffix))) return E_SYNTAX;
    k = s;
    while (k < e) {
      u32 hi, lo;
      u8 v = 0, m = 0;
      if (nd == 1) { hi = '0'; lo = hx_ch(t, k); k += 1; }
      else { hi = hx_ch(t, k); lo = hx_ch(t, k + 1); k += 2; }
      if (hi != '?') { v = (u8)(hx_val(hi) << 4); m |= 0xF0; }
      if (lo != '?') { v = (u8)(v | hx_val(lo)); m |= 0x0F; }
      if (bytes->push(v) != E_OK) return E_NOMEM;
      if (mask && mask->push(m) != E_OK) return E_NOMEM;
    }
  }
  if (bytes->size() == 0) return E_SYNTAX;
  return E_OK;
}

int hex_parse_bytes(const char* text, u32 n, EBytes* out)
{
  return hx_parse(text, n, out, (EBytes*)0);
}

int hex_parse_pattern(const ecp* text, u32 n, EBytes* bytes, EBytes* mask)
{
  if (!mask) return E_SYNTAX;
  return hx_parse(text, n, bytes, mask);
}

// ---- go-to parsers ----

static void set_err(char* err, u32 cap, const char* msg)
{
  if (err && cap) es_copy(err, cap, msg);
}

// Copies expr without whitespace, lowercased. Returns length or -1 if it does not fit.
static int squeeze(const char* expr, char* out, u32 cap)
{
  u32 n = 0;
  if (!expr) { out[0] = 0; return 0; }
  for (; *expr; expr++) {
    char ch = *expr;
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    if (n + 1 >= cap) return -1;
    out[n++] = ch;
  }
  out[n] = 0;
  return (int)n;
}

static int all_dec(const char* s, u32 n)
{
  u32 i;
  if (n == 0) return 0;
  for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
  return 1;
}

static int all_hex(const char* s, u32 n)
{
  u32 i;
  if (n == 0) return 0;
  for (i = 0; i < n; i++) if (hx_val((u8)s[i]) < 0) return 0;
  return 1;
}

static int digits_value(const char* s, u32 n, u32 base, u64* v)
{
  u64 x = 0;
  u32 i;
  for (i = 0; i < n; i++) {
    x = x * base + (u32)hx_val((u8)s[i]);
    if (x > 0xFFFFFFFFu) return E_RANGE;
  }
  *v = x;
  return E_OK;
}

// Offset number: "#dec", "decd", "0xhex", "hexh", "hex" (hex default). s is squeezed (lowercase, no spaces).
static int offset_number(const char* s, u64* v)
{
  u32 n = es_len(s);
  if (n >= 2 && s[0] == '#') return all_dec(s + 1, n - 1) ? digits_value(s + 1, n - 1, 10, v) : E_SYNTAX;
  if (n >= 3 && s[0] == '0' && s[1] == 'x') return all_hex(s + 2, n - 2) ? digits_value(s + 2, n - 2, 16, v) : E_SYNTAX;
  if (n >= 2 && s[n - 1] == 'd' && all_dec(s, n - 1)) return digits_value(s, n - 1, 10, v);
  if (n >= 2 && s[n - 1] == 'h' && all_hex(s, n - 1)) return digits_value(s, n - 1, 16, v);
  if (all_hex(s, n)) return digits_value(s, n, 16, v);
  return E_SYNTAX;
}

static const char k_off_syntax[] = "Not an offset: use 1F40, 0x1F40, #8000, +100, -20, end-10 or 50%";

int goto_parse_offset(const char* expr, epos cur, epos size, epos* out, char* err, u32 errcap)
{
  char s[72];
  char msg[96];
  int n, rc;
  i64 base = 0, r = 0;
  int sign = 0;
  u64 v = 0;
  const char* num;
  if (err && errcap) err[0] = 0;
  n = squeeze(expr, s, sizeof(s));
  if (n < 0) { set_err(err, errcap, "Expression too long"); return E_SYNTAX; }
  if (n == 0) { set_err(err, errcap, "Enter an offset"); return E_SYNTAX; }
  if (s[n - 1] == '%') {
    u64 pct;
    s[n - 1] = 0;
    if (!all_dec(s, (u32)n - 1)) { set_err(err, errcap, "Percent must be a decimal number 0-100"); return E_SYNTAX; }
    if (digits_value(s, (u32)n - 1, 10, &pct) != E_OK || pct > 100) {
      set_err(err, errcap, "Percent must be 0-100"); return E_RANGE;
    }
    if (out) *out = (epos)(((u64)size * pct) / 100);
    return E_OK;
  }
  num = s;
  if (s[0] == 'e' && s[1] == 'n' && s[2] == 'd') {
    base = (i64)size;
    num = s + 3;
    if (*num == 0) { if (out) *out = size; return E_OK; }
    if (*num != '+' && *num != '-') { set_err(err, errcap, k_off_syntax); return E_SYNTAX; }
  } else if (s[0] == '+' || s[0] == '-') {
    base = (i64)cur;
  }
  if (*num == '+' || *num == '-') { sign = (*num == '-') ? -1 : 1; num++; }
  rc = offset_number(num, &v);
  if (rc == E_RANGE) { set_err(err, errcap, "Number too large (max 0xFFFFFFFF)"); return E_RANGE; }
  if (rc != E_OK) { set_err(err, errcap, k_off_syntax); return E_SYNTAX; }
  if (sign == 0) r = (i64)v;
  else if (sign > 0) r = base + (i64)v;
  else r = base - (i64)v;
  if (r < 0) { set_err(err, errcap, "Offset before the start of the file"); return E_RANGE; }
  if (r > (i64)size) {
    es_fmt(msg, sizeof(msg), "Offset beyond end of file (size 0x%X)", (u32)size);
    set_err(err, errcap, msg);
    return E_RANGE;
  }
  if (out) *out = (epos)r;
  return E_OK;
}

static int line_number(const char* s, u32 n, u64* v)
{
  if (!all_dec(s, n)) return E_SYNTAX;
  return digits_value(s, n, 10, v);
}

static const char k_line_syntax[] = "Not a line: use 120, 120:5, +10, -5 or end";

int goto_parse_line(const char* expr, u32 cur_line, u32 line_count, u32* line, u32* col, char* err, u32 errcap)
{
  char s[72];
  char msg[96];
  int n, rc, sign = 0;
  u32 llen, clen = 0;
  char* colon;
  const char* lp;
  i64 base = 0, r = 0;
  u64 v = 0, cv = 0;
  if (err && errcap) err[0] = 0;
  if (line_count == 0) line_count = 1;
  n = squeeze(expr, s, sizeof(s));
  if (n < 0) { set_err(err, errcap, "Expression too long"); return E_SYNTAX; }
  if (n == 0) { set_err(err, errcap, "Enter a line number"); return E_SYNTAX; }
  colon = strchr(s, ':');
  if (colon) {
    *colon = 0;
    clen = es_len(colon + 1);
    if (clen == 0) { set_err(err, errcap, "Enter a column after ':'"); return E_SYNTAX; }
    rc = line_number(colon + 1, clen, &cv);
    if (rc == E_RANGE) { set_err(err, errcap, "Column number too large"); return E_RANGE; }
    if (rc != E_OK) { set_err(err, errcap, k_line_syntax); return E_SYNTAX; }
    if (cv == 0) { set_err(err, errcap, "Columns start at 1"); return E_RANGE; }
  }
  lp = s;
  llen = es_len(s);
  if (llen == 0) {                                  // ":5" = column on the current line
    r = (i64)cur_line + 1;
  } else {
    if (llen >= 3 && s[0] == 'e' && s[1] == 'n' && s[2] == 'd') {
      base = (i64)line_count;
      lp = s + 3;
      if (*lp == 0) sign = 2;
      else if (*lp != '+' && *lp != '-') { set_err(err, errcap, k_line_syntax); return E_SYNTAX; }
    } else if (s[0] == '+' || s[0] == '-') {
      base = (i64)cur_line + 1;
    }
    if (sign == 2) r = base;
    else {
      if (*lp == '+' || *lp == '-') { sign = (*lp == '-') ? -1 : 1; lp++; }
      rc = line_number(lp, es_len(lp), &v);
      if (rc == E_RANGE) { set_err(err, errcap, "Line number too large"); return E_RANGE; }
      if (rc != E_OK) { set_err(err, errcap, k_line_syntax); return E_SYNTAX; }
      if (sign == 0) {
        if (v == 0) { set_err(err, errcap, "Lines start at 1"); return E_RANGE; }
        r = (i64)v;
      } else if (sign > 0) r = base + (i64)v;
      else r = base - (i64)v;
    }
  }
  if (r < 1) { set_err(err, errcap, "Line before the start of the file"); return E_RANGE; }
  if (r > (i64)line_count) {
    es_fmt(msg, sizeof(msg), "Line beyond end of file (%u lines)", line_count);
    set_err(err, errcap, msg);
    return E_RANGE;
  }
  if (line) *line = (u32)(r - 1);
  if (col) *col = colon ? (u32)(cv - 1) : 0;
  return E_OK;
}

// ---- data inspector ----

static void put_u64(char* out, u32 cap, u64 v)
{
  char buf[24];
  es_u64_to_dec(v, buf);
  es_copy(out, cap, buf);
}

static void put_i64(char* out, u32 cap, u64 bits)
{
  char buf[24];
  if (bits >> 63) {                                 // negative: magnitude = two's complement (exact for INT64_MIN)
    buf[0] = '-';
    es_u64_to_dec((~bits) + 1, buf + 1);
  } else {
    es_u64_to_dec(bits, buf);
  }
  es_copy(out, cap, buf);
}

// Formats with sprintf (DESIGN 5.3 item 4: floating point only, buffer >= 64) and normalises the exponent to at
// least two digits without extra leading zeros (VC6 prints "1e+020", clang "1e+20").
static void put_real(char* out, u32 cap, double d, int dbl)
{
  char buf[64];
  char* e;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  sprintf(buf, dbl ? "%.17g" : "%.9g", d);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  e = strchr(buf, 'e');
  if (e) {
    char* d0 = e + 1;
    u32 nd;
    if (*d0 == '+' || *d0 == '-') d0++;
    nd = es_len(d0);
    while (nd > 2 && d0[0] == '0') { memmove(d0, d0 + 1, nd); nd--; }
  }
  es_copy(out, cap, buf);
}

static void put_special(char* out, u32 cap, int neg, int nan)
{
  if (nan) es_copy(out, cap, "NaN");
  else es_copy(out, cap, neg ? "-Inf" : "+Inf");
}

int inspect_bytes(const u8* p, u32 avail, int big_endian, InspectOut* out)
{
  u8 b[8];
  u32 i, n;
  u64 v;
  if (!out) return E_RANGE;
  if (!p) avail = 0;
  n = avail > 8 ? 8 : avail;
  for (i = 0; i < 8; i++) b[i] = (u8)(i < n ? p[i] : 0);
  out->avail = (u8)n;
  es_copy(out->i8, sizeof(out->i8), "-");     es_copy(out->u8_, sizeof(out->u8_), "-");
  es_copy(out->i16, sizeof(out->i16), "-");   es_copy(out->u16_, sizeof(out->u16_), "-");
  es_copy(out->i32_, sizeof(out->i32_), "-"); es_copy(out->u32_, sizeof(out->u32_), "-");
  es_copy(out->i64, sizeof(out->i64), "-");   es_copy(out->u64_, sizeof(out->u64_), "-");
  es_copy(out->f32, sizeof(out->f32), "-");   es_copy(out->f64, sizeof(out->f64), "-");
  es_copy(out->bin, sizeof(out->bin), "-");
  if (n >= 1) {
    char bits[9];
    es_fmt(out->i8, sizeof(out->i8), "%d", (int)(signed char)b[0]);
    es_fmt(out->u8_, sizeof(out->u8_), "%u", (u32)b[0]);
    for (i = 0; i < 8; i++) bits[i] = (char)((b[0] & (0x80 >> i)) ? '1' : '0');
    bits[8] = 0;
    es_copy(out->bin, sizeof(out->bin), bits);
  }
  if (n >= 2) {
    u32 x = big_endian ? ((u32)b[0] << 8) | b[1] : ((u32)b[1] << 8) | b[0];
    es_fmt(out->i16, sizeof(out->i16), "%d", (int)(x >= 0x8000 ? (i32)x - 0x10000 : (i32)x));
    es_fmt(out->u16_, sizeof(out->u16_), "%u", x);
  }
  if (n >= 4) {
    u32 x = big_endian ? ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3]
                       : ((u32)b[3] << 24) | ((u32)b[2] << 16) | ((u32)b[1] << 8) | b[0];
    float f;
    put_i64(out->i32_, sizeof(out->i32_), (x & 0x80000000u) ? (u64)x | ((u64)0xFFFFFFFFu << 32) : (u64)x);
    es_fmt(out->u32_, sizeof(out->u32_), "%u", x);
    if (((x >> 23) & 0xFF) == 0xFF) put_special(out->f32, sizeof(out->f32), (int)(x >> 31), (x & 0x7FFFFF) != 0);
    else if (x == 0x80000000u) es_copy(out->f32, sizeof(out->f32), "-0");
    else {
      memcpy(&f, &x, 4);
      put_real(out->f32, sizeof(out->f32), (double)f, 0);
    }
  }
  if (n >= 8) {
    double d;
    v = 0;
    for (i = 0; i < 8; i++) v = (v << 8) | b[big_endian ? i : 7 - i];
    put_i64(out->i64, sizeof(out->i64), v);
    put_u64(out->u64_, sizeof(out->u64_), v);
    if ((u32)((v >> 52) & 0x7FF) == 0x7FF)
      put_special(out->f64, sizeof(out->f64), (int)(v >> 63), (v & (((u64)1 << 52) - 1)) != 0);
    else if (v == ((u64)1 << 63)) es_copy(out->f64, sizeof(out->f64), "-0");
    else {
      memcpy(&d, &v, 8);
      put_real(out->f64, sizeof(out->f64), d, 1);
    }
  }
  return E_OK;
}
