// t_codec.cpp - suite "codec" (DESIGN 6.9, 7.1, 17.1): enc_* helpers, labels and tokens; SBCS tables vs Python
// vectors (tests/data/codec/sbcs.txt) and full 256-byte round trips; strict UTF-8 and UTF-16 vs Python vectors and an
// invalid corpus; DBCS logic through a test provider built from Python cp932 vectors (cp932.bin); random round trips
// (decode -> encode byte-exact) in every codec, CharReader over a source with tiny chunks, codec_prev against forward
// decoding; codec_encode_str substitution; out-of-memory.
// Owner: T02.
#include "tf.h"
#include "codec.h"
#include "codec_tables.h"
#include "ptree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// data files and parsing

static u8* load_data(TF* tf, const char* name, u32* n)
{
  char path[512];
  FILE* f;
  long sz;
  u8* buf;
  *n = 0;
#if defined(_WIN32)
  es_fmt(path, sizeof(path), "%s\\codec\\%s", tf->data_dir ? tf->data_dir : "tests\\data", name);
#else
  es_fmt(path, sizeof(path), "%s/codec/%s", tf->data_dir ? tf->data_dir : "tests/data", name);
#endif
  f = fopen(path, "rb");
  if (!f) { tf_fail(tf, __FILE__, __LINE__, "missing data file %s (make -C tests data)", path); return 0; }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (u8*)e_alloc((u32)sz + 1);
  if (!buf) { fclose(f); tf_fail(tf, __FILE__, __LINE__, "out of memory loading %s", path); return 0; }
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f); e_free(buf); tf_fail(tf, __FILE__, __LINE__, "read error %s", path); return 0;
  }
  fclose(f);
  buf[sz] = 0;
  *n = (u32)sz;
  return buf;
}

struct LineIt { const char* p; const char* end; };

// Next non-empty line that is not a '#' comment; returns 0 at the end.
static int next_line(LineIt* it, const char** s, const char** e)
{
  while (it->p < it->end) {
    const char* a = it->p;
    const char* b = a;
    while (b < it->end && *b != '\n') b++;
    it->p = b < it->end ? b + 1 : b;
    while (b > a && (b[-1] == '\r' || b[-1] == ' ')) b--;
    if (b == a || *a == '#') continue;
    *s = a; *e = b;
    return 1;
  }
  return 0;
}

// Next whitespace-separated token in [*s, e); returns 0 if none.
static int next_tok(const char** s, const char* e, const char** ts, u32* tl)
{
  const char* p = *s;
  const char* q;
  while (p < e && (*p == ' ' || *p == '\t')) p++;
  if (p >= e) { *s = p; return 0; }
  q = p;
  while (q < e && *q != ' ' && *q != '\t') q++;
  *ts = p; *tl = (u32)(q - p);
  *s = q;
  return 1;
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static u32 parse_hex_u32(const char* s, u32 n)
{
  u32 v = 0, i;
  for (i = 0; i < n; i++) { int h = hexval(s[i]); if (h < 0) break; v = v * 16 + (u32)h; }
  return v;
}

// "41C3A9" -> bytes; "." -> empty. Returns count.
static u32 parse_hex_bytes(const char* s, u32 n, u8* out, u32 cap)
{
  u32 i, k = 0;
  if (n == 1 && s[0] == '.') return 0;
  for (i = 0; i + 1 < n && k < cap; i += 2) out[k++] = (u8)(hexval(s[i]) * 16 + hexval(s[i + 1]));
  return k;
}

// "5E,573" -> ecps; "." -> empty.
static u32 parse_ecp_list(const char* s, u32 n, ecp* out, u32 cap)
{
  u32 i = 0, k = 0;
  if (n == 1 && s[0] == '.') return 0;
  while (i < n && k < cap) {
    u32 j = i;
    while (j < n && s[j] != ',') j++;
    out[k++] = parse_hex_u32(s + i, j - i);
    i = j + 1;
  }
  return k;
}

// space-separated hex bytes: "E0 80 AF"
static u32 hexb(const char* s, u8* out)
{
  u32 k = 0;
  while (*s) {
    int h, l;
    while (*s == ' ') s++;
    if (!*s) break;
    h = hexval(s[0]); l = hexval(s[1]);
    out[k++] = (u8)(h * 16 + l);
    s += 2;
  }
  return k;
}

// ---------------------------------------------------------------------------------------------------------------
// a source that hands out tiny chunks (1-3 bytes) so readers must stitch characters across chunk ends

class ChunkSource : public ByteSource {
public:
  ChunkSource(const u8* p, epos n, u32 seed) : p_(p), n_(n), seed_(seed) {}
  epos size() const { return n_; }
  const u8* get(epos off, epos* avail)
  {
    epos end;
    if (off >= n_) { *avail = 0; return 0; }
    end = off + 1;
    while (end < n_ && (((end * 2654435761u) ^ seed_) >> 7) % 3 != 0) end++;
    *avail = end - off;
    return p_ + off;
  }
private:
  const u8* p_; epos n_; u32 seed_;
};

// ---------------------------------------------------------------------------------------------------------------
// DBCS test provider from cp932.bin

static u16 g_t_single[256];          // 0xFFFF lead, 0xFFFE undefined, else code point
static u16 g_t_rowidx[256];          // 1 + row
static u16 g_t_rows[64][256];
static u32 g_t_enc[256 + 64 * 256];
static u32 g_t_nenc = 0;

static int cmp_u32(const void* a, const void* b)
{
  u32 x = *(const u32*)a, y = *(const u32*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

class TestDbcs : public DbcsProvider {
public:
  int is_lead(u8 b) const { return g_t_single[b] == 0xFFFF; }
  ecp decode_pair(u8 l, u8 t) const
  {
    u16 v;
    if (!g_t_rowidx[l]) return ECP_BADBYTE(l);
    v = g_t_rows[g_t_rowidx[l] - 1][t];
    return v ? (ecp)v : ECP_BADBYTE(l);
  }
  ecp decode_single(u8 b) const { return g_t_single[b] >= 0xFFFE ? ECP_BADBYTE(b) : (ecp)g_t_single[b]; }
  u32 encode(ecp c, u8* out) const
  {
    u32 lo = 0, hi = g_t_nenc;
    while (lo < hi) {
      u32 mid = (lo + hi) / 2, key = g_t_enc[mid] >> 16;
      if (key == c) {
        u32 v = g_t_enc[mid] & 0xFFFF;
        if (v > 0xFF) { out[0] = (u8)(v >> 8); out[1] = (u8)v; return 2; }
        out[0] = (u8)v; return 1;
      }
      if (key < c) lo = mid + 1; else hi = mid;
    }
    return 0;
  }
};

static TestDbcs g_test932;

static int load_cp932(TF* tf)
{
  u32 n, i, b, t, nrows = 0, k = 0, j;
  u8* d = load_data(tf, "cp932.bin", &n);
  if (!d) return 0;
  if (n < 516 || memcmp(d, "P932", 4) != 0) { tf_fail(tf, __FILE__, __LINE__, "cp932.bin: bad header"); e_free(d); return 0; }
  for (b = 0; b < 256; b++) {
    g_t_single[b] = (u16)(d[4 + 2 * b] | (d[5 + 2 * b] << 8));
    g_t_rowidx[b] = 0;
    if (g_t_single[b] == 0xFFFF) g_t_rowidx[b] = (u16)(++nrows);
  }
  if (nrows > 64 || n != 516 + nrows * 512) { tf_fail(tf, __FILE__, __LINE__, "cp932.bin: bad size"); e_free(d); return 0; }
  for (i = 0; i < nrows; i++)
    for (t = 0; t < 256; t++) g_t_rows[i][t] = (u16)(d[516 + i * 512 + 2 * t] | (d[517 + i * 512 + 2 * t] << 8));
  e_free(d);
  for (b = 0; b < 256; b++) if (g_t_single[b] < 0xFFFE) g_t_enc[k++] = ((u32)g_t_single[b] << 16) | b;
  for (b = 0; b < 256; b++) {
    if (!g_t_rowidx[b]) continue;
    for (t = 0; t < 256; t++) {
      u16 v = g_t_rows[g_t_rowidx[b] - 1][t];
      if (v) g_t_enc[k++] = ((u32)v << 16) | (b << 8) | t;
    }
  }
  qsort(g_t_enc, k, sizeof(u32), cmp_u32);
  j = 0;
  for (i = 0; i < k; i++) if (j == 0 || (g_t_enc[j - 1] >> 16) != (g_t_enc[i] >> 16)) g_t_enc[j++] = g_t_enc[i];
  g_t_nenc = j;
  TF_EQ_U(nrows, 55);
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// round trip machinery

enum { RT_MAX = 256 };

struct RtState {
  u32 fails;
  u32 strings;
};

static void rt_fail(TF* tf, RtState* st, const Codec* c, const u8* p, u32 n, const char* what)
{
  char hex[200];
  u32 i, k = 0;
  st->fails++;
  if (st->fails > 5) return;
  for (i = 0; i < n && k + 3 < sizeof(hex); i++) k += (u32)es_fmt(hex + k, sizeof(hex) - k, "%02X", (u32)p[i]);
  hex[k] = 0;
  tf_fail(tf, __FILE__, __LINE__, "cp %u: %s on bytes %s", (u32)c->cp, what, hex);
}

// decode -> encode byte-exact (dup_ok: a different encoding is accepted if it decodes to the same characters);
// CharReader over tiny chunks gives the same characters and positions; codec_prev agrees with the boundaries.
static void roundtrip(TF* tf, RtState* st, const Codec* c, const u8* p, u32 n, int dup_ok)
{
  ecp cps[RT_MAX], cps2[RT_MAX];
  epos bounds[RT_MAX + 1];
  u32 k, used = 0, i, at = 0, unrep = 0;
  EBytes out;
  CharReader rd;
  st->strings++;
  k = codec_decode_buf(c, p, n, 1, cps, RT_MAX, &used);
  if (used != n) { rt_fail(tf, st, c, p, n, "decode_buf did not consume everything"); return; }
  for (i = 0; i < k; i++) {
    bounds[i] = at;
    at += c->decode(p + at, n - at, 1, &cps2[i]);
    if (cps2[i] != cps[i]) { rt_fail(tf, st, c, p, n, "decode_buf != decode"); return; }
  }
  bounds[k] = n;
  if (codec_encode_str(c, cps, k, &out, &unrep, 0) != E_OK || unrep != 0) {
    rt_fail(tf, st, c, p, n, "encode failed"); return;
  }
  if (out.size() != n || (n && memcmp(out.data(), p, n) != 0)) {
    // DBCS duplicates (several pairs decode to one character): each character must encode to its own bytes, or
    // to other bytes that decode back to the same character on their own
    if (!dup_ok) { rt_fail(tf, st, c, p, n, "re-encoded bytes differ"); return; }
    for (i = 0; i < k; i++) {
      u8 eb[8];
      u32 el = c->encode(cps[i], eb);
      ecp back;
      if (el == bounds[i + 1] - bounds[i] && memcmp(eb, p + bounds[i], el) == 0) continue;
      if (el == 0 || c->decode(eb, el, 1, &back) != el || back != cps[i]) {
        rt_fail(tf, st, c, p, n, "dup re-encode differs"); return;
      }
    }
  }
  {
    ChunkSource src(p, n, st->strings);
    ecp v;
    epos pos;
    rd.init(&src, c, 0, n);
    for (i = 0; i < k; i++) {
      if (rd.next(&v, &pos) != 1 || v != cps[i] || pos != bounds[i]) { rt_fail(tf, st, c, p, n, "CharReader"); return; }
    }
    if (rd.next(&v, &pos) != 0 || rd.pos() != n) { rt_fail(tf, st, c, p, n, "CharReader end"); return; }
    for (i = 1; i <= k; i++) {
      if (codec_prev(&src, c, bounds[i], 0) != bounds[i - 1]) { rt_fail(tf, st, c, p, n, "codec_prev"); return; }
      if (i >= 2 && codec_prev(&src, c, bounds[i], bounds[i - 2]) != bounds[i - 1]) {
        rt_fail(tf, st, c, p, n, "codec_prev with floor"); return;
      }
    }
    if (codec_prev(&src, c, 0, 0) != 0) { rt_fail(tf, st, c, p, n, "codec_prev at floor"); return; }
  }
}

static u32 gen_utf8ish(TF* tf, u8* buf, u32 cap)
{
  static const u8 k_odd[14] = { 0x80, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xFE, 0xFF };
  const Codec* u8c = codec_get(enc_utf8(0));
  u32 n = 0, pieces = tf_rand_below(tf, 10), i;
  for (i = 0; i < pieces && n + 4 <= cap; i++) {
    u32 k = tf_rand_below(tf, 8), len;
    u8 tmp[4];
    ecp c;
    if (k <= 2) { buf[n++] = (u8)(0x20 + tf_rand_below(tf, 95)); continue; }
    if (k == 6) { buf[n++] = k_odd[tf_rand_below(tf, 14)]; continue; }
    if (k == 7) { buf[n++] = (u8)tf_rand_below(tf, 256); continue; }
    if (k == 3) c = 0x80 + tf_rand_below(tf, 0x780);
    else if (k == 4) c = 0x800 + tf_rand_below(tf, 0xF800);
    else c = 0x10000 + tf_rand_below(tf, 0x100000);
    len = u8c->encode(c, tmp);
    if (len == 0) continue;                       // surrogate
    if (tf_rand_below(tf, 6) == 0) len = 1 + tf_rand_below(tf, len);   // truncate sometimes
    memcpy(buf + n, tmp, len);
    n += len;
  }
  return n;
}

static u32 gen_random(TF* tf, u8* buf, u32 cap)
{
  u32 n = tf_rand_below(tf, cap + 1), i;
  for (i = 0; i < n; i++) {
    u32 r = tf_rand(tf);
    // bias toward interesting values: NUL, CR, LF, ASCII, high bytes
    buf[i] = (u8)((r & 3) == 0 ? (r >> 8) % 128 : (r >> 8));
  }
  return n;
}

static u32 gen_utf16ish(TF* tf, u8* buf, u32 cap, int be)
{
  u32 n = 0, units = tf_rand_below(tf, 10), i;
  for (i = 0; i < units && n + 4 <= cap; i++) {
    u32 k = tf_rand_below(tf, 6);
    u16 u;
    if (k == 0) u = (u16)(0x20 + tf_rand_below(tf, 95));
    else if (k == 1) u = (u16)(0xD800 + tf_rand_below(tf, 0x400));
    else if (k == 2) u = (u16)(0xDC00 + tf_rand_below(tf, 0x400));
    else u = (u16)tf_rand_below(tf, 0x10000);
    if (be) { buf[n++] = (u8)(u >> 8); buf[n++] = (u8)u; }
    else { buf[n++] = (u8)u; buf[n++] = (u8)(u >> 8); }
  }
  if (tf_rand_below(tf, 5) == 0 && n < cap) buf[n++] = (u8)tf_rand(tf);
  return n;
}

// ---------------------------------------------------------------------------------------------------------------
// tests

static void test_enc_helpers(TF* tf)
{
  static const char* k_tokens[] = { "ansi", "oem", "utf8", "utf8bom", "utf16le", "utf16be", "utf16le-nobom",
                                    "utf16be-nobom", "cp1251", "cp866", "cp20866", "cp932", "cp28605" };
  char buf[64];
  EncSpec e, e2;
  u8 bom[4];
  u32 i;
  TF_CHECK(enc_equal(enc_ansi(), enc_cp(1252)));
  TF_CHECK(enc_equal(enc_oem(), enc_cp(437)));
  TF_CHECK(!enc_equal(enc_utf8(0), enc_utf8(1)));
  TF_EQ_U(enc_cp(65001).kind, EK_UTF8);
  TF_EQ_U(enc_cp(1200).kind, EK_UTF16LE);
  TF_EQ_U(enc_cp(1201).kind, EK_UTF16BE);
  TF_EQ_U(enc_cp(1251).kind, EK_SBCS);
  TF_EQ_U(enc_cp(932).kind, EK_DBCS);
  TF_EQ_U(enc_cp(949).kind, EK_DBCS);
  for (i = 0; i < sizeof(k_tokens) / sizeof(k_tokens[0]); i++) {
    TF_EQ_I(enc_parse(k_tokens[i], &e), E_OK);
    enc_token(e, buf, sizeof(buf));
    TF_EQ_S(buf, k_tokens[i]);
    TF_EQ_I(enc_parse(buf, &e2), E_OK);
    TF_CHECK(enc_equal(e, e2));
  }
  TF_EQ_I(enc_parse("ANSI", &e), E_OK);
  TF_CHECK(enc_equal(e, enc_ansi()));
  TF_EQ_I(enc_parse("cp", &e), E_SYNTAX);
  TF_EQ_I(enc_parse("cpx", &e), E_SYNTAX);
  TF_EQ_I(enc_parse("cp70000", &e), E_SYNTAX);
  TF_EQ_I(enc_parse("latin1", &e), E_SYNTAX);
  TF_EQ_I(enc_parse("", &e), E_SYNTAX);
  TF_EQ_I(enc_parse(0, &e), E_SYNTAX);
  TF_EQ_I(enc_parse("cp1252", &e), E_OK);
  enc_token(e, buf, sizeof(buf));
  TF_EQ_S(buf, "ansi");
  TF_EQ_I(enc_parse("cp65001", &e), E_OK);
  TF_CHECK(enc_equal(e, enc_utf8(0)));

  TF_EQ_S(enc_label(enc_ansi(), buf, sizeof(buf)), "ANSI");
  TF_EQ_S(enc_label(enc_oem(), buf, sizeof(buf)), "OEM");
  TF_EQ_S(enc_label(enc_utf8(0), buf, sizeof(buf)), "UTF-8");
  TF_EQ_S(enc_label(enc_utf8(1), buf, sizeof(buf)), "UTF-8 BOM");
  TF_EQ_S(enc_label(enc_utf16le(1), buf, sizeof(buf)), "UTF-16 LE");
  TF_EQ_S(enc_label(enc_utf16be(1), buf, sizeof(buf)), "UTF-16 BE");
  TF_EQ_S(enc_label(enc_utf16le(0), buf, sizeof(buf)), "UTF-16 LE no BOM");
  TF_EQ_S(enc_label(enc_cp(1251), buf, sizeof(buf)), "Windows-1251");
  TF_EQ_S(enc_label(enc_cp(874), buf, sizeof(buf)), "Windows-874");
  TF_EQ_S(enc_label(enc_cp(866), buf, sizeof(buf)), "OEM 866");
  TF_EQ_S(enc_label(enc_cp(20866), buf, sizeof(buf)), "KOI8-R");
  TF_EQ_S(enc_label(enc_cp(21866), buf, sizeof(buf)), "KOI8-U");
  TF_EQ_S(enc_label(enc_cp(28595), buf, sizeof(buf)), "ISO-8859-5");
  TF_EQ_S(enc_label(enc_cp(28605), buf, sizeof(buf)), "ISO-8859-15");
  TF_EQ_S(enc_label(enc_cp(932), buf, sizeof(buf)), "Shift-JIS");
  TF_EQ_S(enc_label(enc_cp(1361), buf, sizeof(buf)), "Code page 1361");
  TF_EQ_S(enc_label(enc_cp(1251), buf, 4), "Win");
  // every built-in table has a real name
  for (i = 0; i < g_sbcs_table_count; i++) {
    enc_label(enc_cp(g_sbcs_tables[i].cp), buf, sizeof(buf));
    if (g_sbcs_tables[i].cp != 1252 && g_sbcs_tables[i].cp != 437) TF_CHECK(es_nicmp(buf, "Code page", 9) != 0);
  }
  // system code pages switch the ANSI/OEM aliases
  enc_set_system(1251, 866);
  TF_EQ_S(enc_label(enc_cp(1251), buf, sizeof(buf)), "ANSI");
  TF_EQ_S(enc_label(enc_cp(1252), buf, sizeof(buf)), "Windows-1252");
  TF_EQ_S(enc_label(enc_cp(866), buf, sizeof(buf)), "OEM");
  TF_EQ_S(enc_label(enc_cp(437), buf, sizeof(buf)), "OEM 437");
  TF_EQ_U(enc_ansi().cp, 1251);
  enc_set_system(1252, 437);

  TF_EQ_U(enc_bom(enc_utf8(1), bom), 3);
  TF_EQ_MEM(bom, "\xEF\xBB\xBF", 3);
  TF_EQ_U(enc_bom(enc_utf16le(1), bom), 2);
  TF_EQ_MEM(bom, "\xFF\xFE", 2);
  TF_EQ_U(enc_bom(enc_utf16be(1), bom), 2);
  TF_EQ_MEM(bom, "\xFE\xFF", 2);
  TF_EQ_U(enc_bom(enc_utf8(0), bom), 0);
  TF_EQ_U(enc_bom(enc_utf16le(0), bom), 0);
  TF_EQ_U(enc_bom(enc_ansi(), bom), 0);
}

static void test_registry(TF* tf)
{
  static const u16 k_menu[10] = { 1250, 1251, 1253, 1254, 1257, 437, 850, 852, 866, 20866 };
  static const u16 k_design[] = { 1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 874, 437, 850, 852, 855, 857,
                                  860, 861, 862, 863, 865, 866, 869, 20866, 21866, 28591, 28592, 28593, 28594, 28595,
                                  28596, 28597, 28598, 28599, 28605 };
  const Codec* c;
  BreakSpec bs;
  u32 i;
  TF_EQ_I(codec_init(), E_OK);
  TF_EQ_I(codec_init(), E_OK);                       // idempotent
  for (i = 0; i < 10; i++) TF_CHECK(codec_get(enc_cp(k_menu[i])) != 0);
  for (i = 0; i < sizeof(k_design) / sizeof(k_design[0]); i++) {
    TF_CHECK(codec_sbcs_supported(k_design[i]));
    c = codec_get(enc_cp(k_design[i]));
    TF_CHECK(c != 0);
    if (c) {
      TF_EQ_U(c->cp, k_design[i]);
      TF_EQ_U(c->kind, EK_SBCS); TF_EQ_U(c->unit, 1); TF_EQ_U(c->max_len, 1);
      TF_EQ_U(c->ascii_compat, 1); TF_EQ_U(c->self_sync, 1);
    }
  }
  TF_CHECK(!codec_sbcs_supported(932));
  TF_CHECK(!codec_sbcs_supported(65001));
  TF_CHECK(codec_get(enc_cp(932)) == 0);              // no provider registered
  TF_CHECK(codec_get(enc_cp(1361)) == 0);
  TF_CHECK(codec_get(enc_cp(12345)) == 0);
  c = codec_get(enc_utf8(1));
  TF_CHECK(c && c->kind == EK_UTF8 && c->unit == 1 && c->max_len == 4 && c->ascii_compat && c->self_sync);
  TF_CHECK(c == codec_get(enc_utf8(0)));
  c = codec_get(enc_utf16le(1));
  TF_CHECK(c && c->kind == EK_UTF16LE && c->unit == 2 && c->max_len == 4 && !c->ascii_compat && c->self_sync);
  c = codec_get(enc_utf16be(0));
  TF_CHECK(c && c->kind == EK_UTF16BE && c->unit == 2 && c->cp == 1201);
  codec_break_spec(enc_utf8(0), 0, &bs);
  TF_CHECK(bs.unit == 1 && bs.b0 == 0x0A);
  codec_break_spec(enc_cp(1252), 1, &bs);
  TF_CHECK(bs.unit == 1 && bs.b0 == 0x0D);
  codec_break_spec(enc_utf16le(1), 0, &bs);
  TF_CHECK(bs.unit == 2 && bs.b0 == 0x0A && bs.b1 == 0);
  codec_break_spec(enc_utf16be(1), 1, &bs);
  TF_CHECK(bs.unit == 2 && bs.b0 == 0 && bs.b1 == 0x0D);
}

static void test_sbcs(TF* tf)
{
  u32 n, t, b, ncp = 0, fails = 0;
  u8* d = load_data(tf, "sbcs.txt", &n);
  LineIt it;
  const char* s;
  const char* e;
  // vectors from Python
  if (d) {
    it.p = (const char*)d; it.end = (const char*)d + n;
    while (next_line(&it, &s, &e)) {
      const Codec* c;
      u32 cp, row;
      if (e - s < 4 || memcmp(s, "cp ", 3) != 0) { tf_fail(tf, __FILE__, __LINE__, "sbcs.txt: expected 'cp N'"); break; }
      cp = 0;
      for (s += 3; s < e; s++) cp = cp * 10 + (u32)(*s - '0');
      c = codec_get(enc_cp((u16)cp));
      TF_CHECK(c != 0);
      ncp++;
      for (row = 0; row < 16; row++) {
        const char* ts;
        u32 tl, col;
        if (!next_line(&it, &s, &e)) break;
        for (col = 0; col < 16 && next_tok(&s, e, &ts, &tl); col++) {
          u8 byte = (u8)(row * 16 + col);
          ecp want = (tl == 1 && ts[0] == '-') ? ECP_BADBYTE(byte) : parse_hex_u32(ts, tl);
          ecp got = 0;
          if (c) c->decode(&byte, 1, 1, &got);
          if (got != want) {
            fails++;
            if (fails <= 5) tf_fail(tf, __FILE__, __LINE__, "cp%u byte %02X: got %X want %X", cp, (u32)byte, got, want);
          }
        }
      }
    }
    e_free(d);
    TF_EQ_U(ncp, g_sbcs_table_count);
    TF_EQ_U(fails, 0);
  }
  // all 256 bytes round-trip; reverse tables consistent over the whole BMP
  fails = 0;
  for (t = 0; t < g_sbcs_table_count; t++) {
    const Codec* c = codec_get(enc_cp(g_sbcs_tables[t].cp));
    u32 enc_count = 0, cpv;
    if (!c) { fails++; continue; }
    for (b = 0; b < 256; b++) {
      u8 byte = (u8)b, out[4];
      ecp v;
      if (c->decode(&byte, 1, 0, &v) != 1) fails++;
      if (b < 0x20 && v != b) fails++;                 // C0 controls are themselves in every table
      if (c->encode(v, out) != 1 || out[0] != byte) fails++;
    }
    for (cpv = 0; cpv < 0x10000; cpv++) {
      u8 out[4];
      ecp v;
      if (c->encode(cpv, out) == 1) {
        enc_count++;
        c->decode(out, 1, 1, &v);
        if (v != cpv) fails++;
      }
    }
    if (enc_count != 128u + g_sbcs_tables[t].nrev) fails++;
    {
      u8 out[4];
      if (c->encode(ECP_BADBYTE(0x81), out) != 1 || out[0] != 0x81) fails++;
      if (c->encode(ECP_BADBYTE(0x41), out) != 1 || out[0] != 0x41) fails++;
      if (c->encode(ECP_BADUNIT(0xD800), out) != 0) fails++;
      if (c->encode(0x1F600, out) != 0) fails++;
      if (c->encode(0x110000u + 0x100u, out) != 0) fails++;
    }
  }
  TF_EQ_U(fails, 0);
  // a few well-known values
  {
    const Codec* c1251 = codec_get(enc_cp(1251));
    const Codec* c866 = codec_get(enc_cp(866));
    const Codec* koi = codec_get(enc_cp(20866));
    const Codec* c1252 = codec_get(enc_cp(1252));
    u8 out[4], byte;
    ecp v;
    if (c1251 && c866 && koi && c1252) {
      TF_EQ_U(c1251->encode(0x416, out), 1); TF_EQ_U(out[0], 0xC6);     // Zhe
      TF_EQ_U(c866->encode(0x416, out), 1); TF_EQ_U(out[0], 0x86);
      TF_EQ_U(koi->encode(0x416, out), 1); TF_EQ_U(out[0], 0xF6);
      TF_EQ_U(c1252->encode(0x20AC, out), 1); TF_EQ_U(out[0], 0x80);
      TF_EQ_U(c1252->encode(0x416, out), 0);
      byte = 0x81; c1252->decode(&byte, 1, 1, &v); TF_EQ_U(v, ECP_BADBYTE(0x81));
      byte = 0xE4; c1252->decode(&byte, 1, 1, &v); TF_EQ_U(v, 0xE4);
      byte = 0xB0; c866->decode(&byte, 1, 1, &v); TF_EQ_U(v, 0x2591);    // light shade
      byte = 0x01; c866->decode(&byte, 1, 1, &v); TF_EQ_U(v, 0x01);      // control, not a smiley
    } else TF_CHECK(0);
  }
}

struct U8Case { const char* in; int eof; u32 used; ecp first; };

static void test_utf8(TF* tf)
{
  // invalid corpus: every bad byte is one ECP_BADBYTE (DESIGN 7.1)
  static const char* k_bad[] = { "C0 AF", "C1 BF", "E0 80 AF", "E0 9F BF", "ED A0 80", "ED BF BF", "F0 80 80 80",
                                 "F0 8F BF BF", "F4 90 80 80", "F5 80 80 80", "F7 BF BF BF", "F8 88 80 80 80",
                                 "FC 84 80 80 80 80", "FE", "FF", "80", "BF BF", "E2 82", "F0 9F 98", "C2" };
  static const U8Case k_one[] = {
    { "41", 1, 1, 0x41 }, { "C2 A9", 1, 2, 0xA9 }, { "DF BF", 1, 2, 0x7FF }, { "E0 A0 80", 1, 3, 0x800 },
    { "E2 82 AC", 1, 3, 0x20AC }, { "ED 9F BF", 1, 3, 0xD7FF }, { "EE 80 80", 1, 3, 0xE000 },
    { "EF BB BF", 1, 3, 0xFEFF }, { "EF BF BF", 1, 3, 0xFFFF }, { "F0 90 80 80", 1, 4, 0x10000 },
    { "F0 9F 98 80", 1, 4, 0x1F600 }, { "F4 8F BF BF", 1, 4, 0x10FFFF },
    { "E2 82", 0, 0, 0 }, { "F0 9F 98", 0, 0, 0 }, { "C2", 0, 0, 0 }, { "F4", 0, 0, 0 },
    { "C0", 0, 1, ECP_BADBYTE(0xC0) }, { "E0 80", 0, 1, ECP_BADBYTE(0xE0) }, { "F4 90", 0, 1, ECP_BADBYTE(0xF4) },
    { "E2 82 41", 1, 1, ECP_BADBYTE(0xE2) }, { "E2 41", 0, 1, ECP_BADBYTE(0xE2) }, { "80 80", 0, 1, ECP_BADBYTE(0x80) }
  };
  const Codec* c = codec_get(enc_utf8(0));
  u8 in[32], out[8];
  ecp cps[32], v;
  u32 i, j, n, k, used;
  for (i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
    n = hexb(k_bad[i], in);
    k = codec_decode_buf(c, in, n, 1, cps, 32, &used);
    TF_EQ_U(k, n);
    TF_EQ_U(used, n);
    for (j = 0; j < k; j++) TF_EQ_U(cps[j], ECP_BADBYTE(in[j]));
  }
  for (i = 0; i < sizeof(k_one) / sizeof(k_one[0]); i++) {
    n = hexb(k_one[i].in, in);
    v = 0;
    TF_EQ_U(c->decode(in, n, k_one[i].eof, &v), k_one[i].used);
    if (k_one[i].used) TF_EQ_U(v, k_one[i].first);
  }
  // "E2 82 41": bad, bad, 'A'
  n = hexb("E2 82 41", in);
  k = codec_decode_buf(c, in, n, 1, cps, 32, &used);
  TF_EQ_U(k, 3);
  TF_CHECK(cps[0] == ECP_BADBYTE(0xE2) && cps[1] == ECP_BADBYTE(0x82) && cps[2] == 0x41);
  // partial input (eof = 0) stops before an incomplete sequence
  n = hexb("41 42 E2 82", in);
  k = codec_decode_buf(c, in, n, 0, cps, 32, &used);
  TF_EQ_U(k, 2); TF_EQ_U(used, 2);
  k = codec_decode_buf(c, in, n, 1, cps, 32, &used);
  TF_EQ_U(k, 4); TF_EQ_U(used, 4);
  k = codec_decode_buf(c, in, n, 1, cps, 3, &used);   // cap
  TF_EQ_U(k, 3); TF_EQ_U(used, 3);
  // encoder
  TF_EQ_U(c->encode(0x7F, out), 1);
  TF_EQ_U(c->encode(0x80, out), 2); TF_EQ_MEM(out, "\xC2\x80", 2);
  TF_EQ_U(c->encode(0x7FF, out), 2);
  TF_EQ_U(c->encode(0x800, out), 3); TF_EQ_MEM(out, "\xE0\xA0\x80", 3);
  TF_EQ_U(c->encode(0xFFFF, out), 3);
  TF_EQ_U(c->encode(0x10000, out), 4); TF_EQ_MEM(out, "\xF0\x90\x80\x80", 4);
  TF_EQ_U(c->encode(0x10FFFF, out), 4); TF_EQ_MEM(out, "\xF4\x8F\xBF\xBF", 4);
  TF_EQ_U(c->encode(0xD800, out), 0);
  TF_EQ_U(c->encode(0xDFFF, out), 0);
  TF_EQ_U(c->encode(ECP_BADBYTE(0xC0), out), 1); TF_EQ_U(out[0], 0xC0);
  TF_EQ_U(c->encode(ECP_BADBYTE(0x41), out), 1); TF_EQ_U(out[0], 0x41);
  TF_EQ_U(c->encode(ECP_BADUNIT(0xD800), out), 0);
  TF_EQ_U(c->encode(0x130000, out), 0);
  TF_EQ_U(c->encode(0xFFFFFFFFu, out), 0);
  // Python vectors
  {
    u32 dn, cases = 0, fails = 0;
    u8* d = load_data(tf, "utf8.txt", &dn);
    LineIt it;
    const char* s;
    const char* e;
    if (d) {
      it.p = (const char*)d; it.end = (const char*)d + dn;
      while (next_line(&it, &s, &e)) {
        const char* ts;
        u32 tl, bn, en;
        u8 bytes[128];
        ecp want[128], got[128];
        EBytes enc;
        u32 unrep = 0;
        if (!next_tok(&s, e, &ts, &tl)) continue;
        bn = parse_hex_bytes(ts, tl, bytes, 128);
        if (!next_tok(&s, e, &ts, &tl)) continue;
        en = parse_ecp_list(ts, tl, want, 128);
        cases++;
        k = codec_decode_buf(c, bytes, bn, 1, got, 128, &used);
        if (k != en || used != bn || memcmp(got, want, k * sizeof(ecp)) != 0) {
          fails++;
          if (fails <= 5) tf_fail(tf, __FILE__, __LINE__, "utf8.txt case %u: decode mismatch", cases);
          continue;
        }
        if (codec_encode_str(c, got, k, &enc, &unrep, 0) != E_OK || enc.size() != bn ||
            (bn && memcmp(enc.data(), bytes, bn) != 0)) {
          fails++;
          if (fails <= 5) tf_fail(tf, __FILE__, __LINE__, "utf8.txt case %u: encode mismatch", cases);
        }
      }
      e_free(d);
      TF_EQ_U(cases, 1500);
      TF_EQ_U(fails, 0);
    }
  }
}

static void test_utf16(TF* tf)
{
  const Codec* le = codec_get(enc_utf16le(1));
  const Codec* be = codec_get(enc_utf16be(1));
  u8 in[16], out[8];
  ecp v;
  // hand cases
  TF_EQ_U(le->decode((const u8*)"A\0", 2, 1, &v), 2); TF_EQ_U(v, 0x41);
  TF_EQ_U(be->decode((const u8*)"\0A", 2, 1, &v), 2); TF_EQ_U(v, 0x41);
  TF_EQ_U(le->decode((const u8*)"A", 1, 0, &v), 0);
  TF_EQ_U(le->decode((const u8*)"A", 1, 1, &v), 1); TF_EQ_U(v, ECP_BADBYTE(0x41));
  hexb("3D D8 00 DE", in);                                      // U+1F600 LE
  TF_EQ_U(le->decode(in, 4, 1, &v), 4); TF_EQ_U(v, 0x1F600);
  TF_EQ_U(le->decode(in, 2, 0, &v), 0);                         // high surrogate, more may follow
  TF_EQ_U(le->decode(in, 3, 0, &v), 0);
  TF_EQ_U(le->decode(in, 2, 1, &v), 2); TF_EQ_U(v, ECP_BADUNIT(0xD83D));
  TF_EQ_U(le->decode(in, 3, 1, &v), 2); TF_EQ_U(v, ECP_BADUNIT(0xD83D));
  TF_EQ_U(le->decode(in + 2, 2, 1, &v), 2); TF_EQ_U(v, ECP_BADUNIT(0xDE00));
  hexb("3D D8 41 00", in);
  TF_EQ_U(le->decode(in, 4, 1, &v), 2); TF_EQ_U(v, ECP_BADUNIT(0xD83D));
  hexb("D8 3D DE 00", in);
  TF_EQ_U(be->decode(in, 4, 1, &v), 4); TF_EQ_U(v, 0x1F600);
  TF_EQ_U(le->encode(0x1F600, out), 4); TF_EQ_MEM(out, "\x3D\xD8\x00\xDE", 4);
  TF_EQ_U(be->encode(0x1F600, out), 4); TF_EQ_MEM(out, "\xD8\x3D\xDE\x00", 4);
  TF_EQ_U(le->encode(0x20AC, out), 2); TF_EQ_MEM(out, "\xAC\x20", 2);
  TF_EQ_U(be->encode(0x20AC, out), 2); TF_EQ_MEM(out, "\x20\xAC", 2);
  TF_EQ_U(le->encode(0xD800, out), 0);
  TF_EQ_U(le->encode(ECP_BADUNIT(0xDC01), out), 2); TF_EQ_MEM(out, "\x01\xDC", 2);
  TF_EQ_U(be->encode(ECP_BADUNIT(0xDC01), out), 2); TF_EQ_MEM(out, "\xDC\x01", 2);
  TF_EQ_U(le->encode(ECP_BADBYTE(0x7F), out), 1); TF_EQ_U(out[0], 0x7F);
  TF_EQ_U(le->encode(0x110000u + 0x200u, out), 0);
  TF_EQ_U(le->encode(0x10FFFF, out), 4); TF_EQ_MEM(out, "\xFF\xDB\xFF\xDF", 4);
  // Python vectors
  {
    u32 dn, cases = 0, fails = 0;
    u8* d = load_data(tf, "utf16.txt", &dn);
    LineIt it;
    const char* s;
    const char* e;
    if (d) {
      it.p = (const char*)d; it.end = (const char*)d + dn;
      while (next_line(&it, &s, &e)) {
        const char* ts;
        u32 tl, bn, en, k, used, unrep = 0;
        u8 bytes[128];
        ecp want[128], cps[128];
        const Codec* c;
        EBytes enc;
        if (!next_tok(&s, e, &ts, &tl)) continue;
        c = (tl == 2 && ts[0] == 'b') ? be : le;
        if (!next_tok(&s, e, &ts, &tl)) continue;
        bn = parse_hex_bytes(ts, tl, bytes, 128);
        if (!next_tok(&s, e, &ts, &tl)) continue;
        en = parse_ecp_list(ts, tl, want, 128);
        cases++;
        k = codec_decode_buf(c, bytes, bn, 1, cps, 128, &used);
        if (k != en || used != bn || memcmp(cps, want, k * sizeof(ecp)) != 0) {
          fails++;
          if (fails <= 5) tf_fail(tf, __FILE__, __LINE__, "utf16.txt case %u: decode mismatch", cases);
          continue;
        }
        if (codec_encode_str(c, cps, k, &enc, &unrep, 0) != E_OK || enc.size() != bn ||
            (bn && memcmp(enc.data(), bytes, bn) != 0)) {
          fails++;
          if (fails <= 5) tf_fail(tf, __FILE__, __LINE__, "utf16.txt case %u: encode mismatch", cases);
        }
      }
      e_free(d);
      TF_EQ_U(cases, 3000);
      TF_EQ_U(fails, 0);
    }
  }
}

static void test_dbcs(TF* tf)
{
  const Codec* c;
  u8 in[32], out[4];
  ecp cps[32], v;
  u32 n, k, used, l, t, fails = 0;
  if (!load_cp932(tf)) return;
  TF_EQ_I(codec_register_dbcs(932, &g_test932), E_OK);
  c = codec_get(enc_cp(932));
  TF_CHECK(c != 0);
  if (!c) { codec_register_dbcs(932, 0); return; }
  TF_EQ_U(c->kind, EK_DBCS); TF_EQ_U(c->unit, 1); TF_EQ_U(c->max_len, 2);
  TF_EQ_U(c->self_sync, 0); TF_EQ_U(c->ascii_compat, 0); TF_EQ_U(c->cp, 932);
  TF_CHECK(codec_get(enc_cp(936)) == 0);
  // known text: "nihon" (93FA 967B), 815C = U+2015, 5C = backslash, half-width katakana B1, NEC 8740, ED40 / FA5C
  n = hexb("93 FA 96 7B 81 5C 5C B1 87 40 ED 40 FA 5C", in);
  k = codec_decode_buf(c, in, n, 1, cps, 32, &used);
  TF_EQ_U(k, 8); TF_EQ_U(used, n);
  TF_CHECK(cps[0] == 0x65E5 && cps[1] == 0x672C && cps[2] == 0x2015 && cps[3] == 0x5C && cps[4] == 0xFF71 &&
           cps[5] == 0x2460 && cps[6] == 0x7E8A && cps[7] == 0x7E8A);
  // bad pair: the lead byte alone is bad, the trail decodes on its own
  n = hexb("81 20 41", in);
  k = codec_decode_buf(c, in, n, 1, cps, 32, &used);
  TF_EQ_U(k, 3);
  TF_CHECK(cps[0] == ECP_BADBYTE(0x81) && cps[1] == 0x20 && cps[2] == 0x41);
  TF_EQ_U(c->decode(in, 1, 0, &v), 0);                          // lead at the end, more may follow
  TF_EQ_U(c->decode(in, 1, 1, &v), 1); TF_EQ_U(v, ECP_BADBYTE(0x81));
  in[0] = 0xF0;                                                  // not a lead in the Python table: undefined
  TF_EQ_U(c->decode(in, 1, 1, &v), 1); TF_EQ_U(v, ECP_BADBYTE(0xF0));
  // encoder
  TF_EQ_U(c->encode(0x65E5, out), 2); TF_EQ_MEM(out, "\x93\xFA", 2);
  TF_EQ_U(c->encode(0xFF71, out), 1); TF_EQ_U(out[0], 0xB1);
  TF_EQ_U(c->encode(0x41, out), 1); TF_EQ_U(out[0], 0x41);
  TF_EQ_U(c->encode(0xE9, out), 0);
  TF_EQ_U(c->encode(0x1F600, out), 0);
  TF_EQ_U(c->encode(ECP_BADBYTE(0x81), out), 1); TF_EQ_U(out[0], 0x81);
  TF_EQ_U(c->encode(ECP_BADUNIT(0xD800), out), 0);
  // every pair against the table
  for (l = 0; l < 256; l++) {
    for (t = 0; t < 256; t++) {
      ecp want;
      u32 wl;
      in[0] = (u8)l; in[1] = (u8)t;
      if (g_t_single[l] == 0xFFFF) {
        u16 tv = g_t_rows[g_t_rowidx[l] - 1][t];
        want = tv ? (ecp)tv : ECP_BADBYTE(l); wl = tv ? 2 : 1;
      } else {
        want = g_t_single[l] == 0xFFFE ? ECP_BADBYTE(l) : (ecp)g_t_single[l]; wl = 1;
      }
      if (c->decode(in, 2, 1, &v) != wl || v != want) fails++;
    }
  }
  TF_EQ_U(fails, 0);
  // codec_prev decodes forward from the floor (trail bytes overlap ASCII: 815C ends with a backslash byte)
  {
    ViewSource src((const u8*)"\x81\x5C\x5C\x81\x5C", 5);
    TF_EQ_U(codec_prev(&src, c, 5, 0), 3);
    TF_EQ_U(codec_prev(&src, c, 3, 0), 2);
    TF_EQ_U(codec_prev(&src, c, 2, 0), 0);
    TF_EQ_U(codec_prev(&src, c, 5, 2), 3);
  }
}

// a provider without lead bytes acts as a single-byte code page
class AsciiProv : public DbcsProvider {
public:
  int is_lead(u8 b) const { (void)b; return 0; }
  ecp decode_pair(u8 l, u8 t) const { (void)t; return ECP_BADBYTE(l); }
  ecp decode_single(u8 b) const { return b < 0x80 ? (ecp)b : ECP_BADBYTE(b); }
  u32 encode(ecp c, u8* out) const { if (c < 0x80) { out[0] = (u8)c; return 1; } return 0; }
};

static void test_prov_sbcs(TF* tf)
{
  AsciiProv prov;
  const Codec* c;
  u8 b = 0xC4, out[4];
  ecp v;
  TF_CHECK(codec_get(enc_cp(20127)) == 0);
  TF_EQ_I(codec_register_dbcs(20127, &prov), E_OK);
  TF_EQ_U(enc_cp(20127).kind, EK_SBCS);
  c = codec_get(enc_cp(20127));
  TF_CHECK(c != 0);
  if (c) {
    TF_EQ_U(c->kind, EK_SBCS); TF_EQ_U(c->max_len, 1); TF_EQ_U(c->self_sync, 1); TF_EQ_U(c->ascii_compat, 1);
    TF_EQ_U(c->decode(&b, 1, 1, &v), 1); TF_EQ_U(v, ECP_BADBYTE(0xC4));
    TF_EQ_U(c->encode(v, out), 1); TF_EQ_U(out[0], 0xC4);
    TF_EQ_U(c->encode(0xC4, out), 0);
  }
  TF_EQ_I(codec_register_dbcs(20127, 0), E_OK);
  TF_CHECK(codec_get(enc_cp(20127)) == 0);
  TF_EQ_I(codec_register_dbcs(65001, &prov), E_RANGE);
  TF_EQ_I(codec_register_dbcs(0, &prov), E_RANGE);
}

// 100,000 random strings per run over all codecs (UTF-8 30 %, UTF-16 LE/BE 20 % each, SBCS 15 %, DBCS 15 %);
// -long: 400,000 per codec family.
static void test_random_roundtrips(TF* tf)
{
  RtState st;
  u8 buf[64];
  u32 i, n, t, per8, per16, persb;
  const Codec* c;
  per8 = tf->long_run ? 400000 : 30000;
  per16 = tf->long_run ? 400000 : 20000;
  persb = tf->long_run ? 400000 : 15000;
  // UTF-8: half UTF-8-like, half raw random bytes
  st.fails = 0; st.strings = 0;
  c = codec_get(enc_utf8(0));
  for (i = 0; i < per8; i++) {
    n = (i & 1) ? gen_utf8ish(tf, buf, 48) : gen_random(tf, buf, 24);
    roundtrip(tf, &st, c, buf, n, 0);
  }
  TF_EQ_U(st.fails, 0);
  // UTF-16 LE / BE
  for (t = 0; t < 2; t++) {
    st.fails = 0; st.strings = 0;
    c = codec_get(t ? enc_utf16be(1) : enc_utf16le(1));
    for (i = 0; i < per16; i++) {
      n = (i & 1) ? gen_utf16ish(tf, buf, 48, (int)t) : gen_random(tf, buf, 24);
      roundtrip(tf, &st, c, buf, n, 0);
    }
    TF_EQ_U(st.fails, 0);
  }
  // SBCS: every table (the exhaustive 256-byte check above is the real proof; this exercises the helpers)
  st.fails = 0; st.strings = 0;
  for (i = 0; i < persb; i++) {
    c = codec_get(enc_cp(g_sbcs_tables[i % g_sbcs_table_count].cp));
    n = gen_random(tf, buf, 24);
    roundtrip(tf, &st, c, buf, n, 0);
  }
  TF_EQ_U(st.fails, 0);
  // DBCS (test provider): duplicates may re-encode to the canonical pair
  if (load_cp932(tf) && codec_register_dbcs(932, &g_test932) == E_OK) {
    c = codec_get(enc_cp(932));
    st.fails = 0; st.strings = 0;
    for (i = 0; i < persb; i++) {
      n = gen_random(tf, buf, 24);
      roundtrip(tf, &st, c, buf, n, 1);
    }
    TF_EQ_U(st.fails, 0);
  }
  codec_register_dbcs(932, 0);
  TF_CHECK(codec_get(enc_cp(932)) == 0);
}

static void test_prev_inside(TF* tf)
{
  const Codec* u8c = codec_get(enc_utf8(0));
  const Codec* le = codec_get(enc_utf16le(1));
  const Codec* sb = codec_get(enc_cp(1252));
  {
    // "A" U+20AC "B": prev from inside the euro sign lands in it (pos - 1 rule), from after it on its start
    ViewSource src((const u8*)"A\xE2\x82\xAC" "B", 5);
    TF_EQ_U(codec_prev(&src, u8c, 4, 0), 1);
    TF_EQ_U(codec_prev(&src, u8c, 5, 0), 4);
    TF_EQ_U(codec_prev(&src, u8c, 1, 0), 0);
    TF_EQ_U(codec_prev(&src, u8c, 3, 0), 2);
    TF_EQ_U(codec_prev(&src, u8c, 4, 3), 3);   // floor caps the step
    TF_EQ_U(codec_prev(&src, u8c, 9, 0), 4);   // pos beyond the end is clamped
  }
  {
    // UTF-16 LE: 'A', U+1F600 pair, odd trailing byte
    ViewSource src((const u8*)"A\0\x3D\xD8\x00\xDE\x7F", 7);
    TF_EQ_U(codec_prev(&src, le, 7, 0), 6);    // odd trailing byte
    TF_EQ_U(codec_prev(&src, le, 6, 0), 2);    // surrogate pair
    TF_EQ_U(codec_prev(&src, le, 2, 0), 0);
    TF_EQ_U(codec_prev(&src, le, 4, 0), 2);    // inside the pair
    TF_EQ_U(codec_prev(&src, le, 6, 4), 4);
  }
  {
    ViewSource src((const u8*)"abc", 3);
    TF_EQ_U(codec_prev(&src, sb, 3, 0), 2);
    TF_EQ_U(codec_prev(&src, sb, 0, 0), 0);
  }
}

static void test_charreader(TF* tf)
{
  const Codec* u8c = codec_get(enc_utf8(0));
  const Codec* le = codec_get(enc_utf16le(1));
  CharReader rd;
  ecp v;
  epos at;
  u32 i, n;
  u8 big[3000];
  {
    ViewSource src((const u8*)"a\xC3\xA4" "b\xE2\x82\xAC", 7);
    rd.init(&src, u8c, 1, 7);
    TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_U(v, 0xE4); TF_EQ_U(at, 1);
    TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_U(v, 'b'); TF_EQ_U(at, 3);
    TF_EQ_I(rd.next(&v, 0), 1); TF_EQ_U(v, 0x20AC);
    TF_EQ_U(rd.pos(), 7);
    TF_EQ_I(rd.next(&v, &at), 0);
    // end inside a character: the cut sequence reads as bad bytes
    rd.init(&src, u8c, 0, 6);
    TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_I(rd.next(&v, &at), 1);
    TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_U(v, ECP_BADBYTE(0xE2)); TF_EQ_U(at, 4);
    TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_U(v, ECP_BADBYTE(0x82));
    TF_EQ_I(rd.next(&v, &at), 0);
    rd.init(&src, u8c, 2, 100);                 // end clamped to the size
    TF_EQ_I(rd.next(&v, &at), 1); TF_EQ_U(v, ECP_BADBYTE(0xA4));
    rd.init(&src, u8c, 50, 100);
    TF_EQ_I(rd.next(&v, &at), 0);
  }
  // long input across buffer refills, UTF-16 pairs straddling the internal buffer end
  n = 0;
  for (i = 0; n + 4 <= sizeof(big); i++) {
    if (i % 3 == 0) { big[n++] = 0x3D; big[n++] = 0xD8; big[n++] = 0x00; big[n++] = 0xDE; }
    else { big[n++] = (u8)('a' + i % 26); big[n++] = 0; }
  }
  {
    ChunkSource src(big, n, 7);
    u32 count = 0, bad = 0;
    epos expect = 0;
    int rc;
    rd.init(&src, le, 0, n);
    while ((rc = rd.next(&v, &at)) == 1) {
      if (at != expect) bad++;
      if (count % 3 == 0) { if (v != 0x1F600) bad++; expect += 4; }
      else { if (v != (ecp)('a' + count % 26)) bad++; expect += 2; }
      count++;
    }
    TF_EQ_I(rc, 0);
    TF_EQ_U(count, i);
    TF_EQ_U(bad, 0);
  }
}

static void test_encode_str(TF* tf)
{
  const Codec* c1252 = codec_get(enc_cp(1252));
  const Codec* u8c = codec_get(enc_utf8(0));
  static const ecp k_s[5] = { 'A', 0x416, 'B', 0x20AC, 0x1F600 };
  EBytes out;
  u32 unrep = 99;
  TF_EQ_I(codec_encode_str(c1252, k_s, 5, &out, &unrep, '?'), E_OK);
  TF_EQ_U(unrep, 2);
  TF_EQ_U(out.size(), 5);
  TF_EQ_MEM(out.data(), "A?B\x80?", 5);
  out.clear();
  TF_EQ_I(codec_encode_str(c1252, k_s, 5, &out, &unrep, 0), E_RANGE);
  TF_EQ_U(unrep, 1);
  TF_EQ_U(out.size(), 1);
  out.clear();
  TF_EQ_I(codec_encode_str(c1252, k_s, 5, &out, &unrep, 0x416), E_OK);   // substitute itself unrepresentable
  TF_EQ_MEM(out.data(), "A?B\x80?", 5);
  out.clear();
  TF_EQ_I(codec_encode_str(u8c, k_s, 5, &out, 0, 0), E_OK);
  TF_EQ_U(out.size(), 1 + 2 + 1 + 3 + 4);
  out.clear();
  TF_EQ_I(codec_encode_str(u8c, k_s, 0, &out, &unrep, 0), E_OK);
  TF_EQ_U(unrep, 0);
  TF_EQ_U(out.size(), 0);
  out.release();
  // out of memory: fails cleanly
  e_test_fail_after(1);
  TF_EQ_I(codec_encode_str(u8c, k_s, 5, &out, &unrep, 0), E_NOMEM);
  e_test_fail_after(-1);
  out.release();
}

void suite_codec(TF* tf)
{
  enc_set_system(1252, 437);
  codec_init();
  test_enc_helpers(tf);
  test_registry(tf);
  test_sbcs(tf);
  test_utf8(tf);
  test_utf16(tf);
  test_dbcs(tf);
  codec_register_dbcs(932, 0);
  test_prov_sbcs(tf);
  test_prev_inside(tf);
  test_charreader(tf);
  test_encode_str(tf);
  test_random_roundtrips(tf);
}
