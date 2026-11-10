// t_hexfmt.cpp - suite "hexfmt": offset/byte text, char glyphs, hex byte parsers, go-to parsers, data inspector.
// Owner: T06.
#include "tf.h"
#include "hexfmt.h"
#include "codec.h"
#include "ucd.h"
#include <string.h>

// Small CP437 subset (enough for the char column tests; independent of T02's tables).
class HexOemCodec : public Codec {
public:
  HexOemCodec() { kind = EK_SBCS; unit = 1; max_len = 1; ascii_compat = 1; self_sync = 1; pad = 0; cp = 437; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    (void)avail; (void)eof;
    switch (p[0]) {
    case 0x80: *out = 0xC7; break;
    case 0x81: *out = 0xFC; break;
    case 0xB0: *out = 0x2591; break;
    case 0xC9: *out = 0x2554; break;
    case 0xDB: *out = 0x2588; break;
    case 0xE1: *out = 0xDF; break;
    case 0xFE: *out = 0x25A0; break;
    case 0xF0: *out = 0x2261; break;   // not in the drawable ranges of ucd (U+2000-25FF covers it: drawable)
    case 0xF9: *out = 0x0300; break;   // fake: a combining mark -> '.'
    case 0xFA: *out = 0x4E00; break;   // fake: a CJK char -> '.' (not drawable)
    default: *out = p[0] < 0x80 ? p[0] : ECP_BADBYTE(p[0]); break;
    }
    return 1;
  }
  u32 encode(ecp c, u8* out) const { (void)c; (void)out; return 0; }
};

static void test_text(TF* tf)
{
  char o[9];
  char b[3];
  hex_offset_text(0, o);            TF_EQ_S(o, "00000000");
  hex_offset_text(0x1F40, o);       TF_EQ_S(o, "00001F40");
  hex_offset_text(0xFFFFFFFEu, o);  TF_EQ_S(o, "FFFFFFFE");
  b[2] = 0;
  hex_byte_text(0x4D, b);           TF_EQ_S(b, "4D");
  hex_byte_text(0x00, b);           TF_EQ_S(b, "00");
  hex_byte_text(0xAF, b);           TF_EQ_S(b, "AF");
}

static void test_glyph(TF* tf)
{
  HexOemCodec oem;
  EncSpec e8 = enc_utf8(0);
  const Codec* utf8 = codec_get(e8);
  const Codec* c1252 = codec_get(enc_cp(1252));
  const Codec* real437 = codec_get(enc_cp(437));
  u32 b;
  // NULL codec: ASCII only
  for (b = 0; b < 256; b++) {
    u16 g = hex_char_glyph((u8)b, 0);
    if (b >= 0x20 && b < 0x7F) TF_EQ_U(g, b); else TF_EQ_U(g, '.');
  }
  TF_EQ_U(hex_char_glyph('M', &oem), 'M');
  TF_EQ_U(hex_char_glyph(0x01, &oem), '.');      // C0 controls are '.' even in OEM
  TF_EQ_U(hex_char_glyph(0x7F, &oem), '.');
  TF_EQ_U(hex_char_glyph(0x80, &oem), 0xC7);
  TF_EQ_U(hex_char_glyph(0xC9, &oem), 0x2554);
  TF_EQ_U(hex_char_glyph(0xB0, &oem), 0x2591);
  TF_EQ_U(hex_char_glyph(0xFE, &oem), 0x25A0);
  if (uc_width(0x300) == 0) TF_EQ_U(hex_char_glyph(0xF9, &oem), '.');   // combining mark (needs T02's ucd)
  TF_EQ_U(hex_char_glyph(0xFA, &oem), '.');       // not drawable / wide
  TF_EQ_U(hex_char_glyph(0x9F, &oem), '.');       // undefined -> bad byte
  if (utf8) {                                     // non-SBCS: ASCII only
    TF_EQ_U(hex_char_glyph('A', utf8), 'A');
    TF_EQ_U(hex_char_glyph(0xC3, utf8), '.');
  }
  if (c1252) {
    TF_EQ_U(hex_char_glyph(0x80, c1252), 0x20AC);  // euro sign
    TF_EQ_U(hex_char_glyph(0x81, c1252), '.');     // undefined in 1252
    TF_EQ_U(hex_char_glyph(0xE9, c1252), 0xE9);
    TF_EQ_U(hex_char_glyph(0x0A, c1252), '.');
  }
  if (real437 && real437->kind == EK_SBCS) {      // T02's table, once it exists
    TF_EQ_U(hex_char_glyph(0xC9, real437), 0x2554);
    TF_EQ_U(hex_char_glyph(0xDB, real437), 0x2588);
    TF_EQ_U(hex_char_glyph(0x05, real437), '.');
  } else {
    tf_log(tf, "hexfmt: cp437 codec not available yet, real-table checks skipped");
  }
}

static int bytes_are(EBytes& b, const char* hex)
{
  EBytes want;
  if (hex_parse_bytes(hex, es_len(hex), &want) != E_OK) return 0;
  return b.size() == want.size() && memcmp(b.data(), want.data(), b.size()) == 0;
}

static int pb(const char* s, EBytes* out) { return hex_parse_bytes(s, es_len(s), out); }

static int pp(const char* s, EBytes* bytes, EBytes* mask)
{
  ecp buf[128];
  u32 n = 0;
  while (s[n] && n < 128) { buf[n] = (u8)s[n]; n++; }
  return hex_parse_pattern(buf, n, bytes, mask);
}

static void test_parse_bytes(TF* tf)
{
  EBytes b, m;
  static const u8 k4d5a[2] = { 0x4D, 0x5A };
  TF_EQ_I(pb("4D 5A", &b), E_OK);         TF_EQ_U(b.size(), 2); TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("4d5a", &b), E_OK);          TF_EQ_U(b.size(), 2); TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("0x4D,0x5A", &b), E_OK);     TF_EQ_U(b.size(), 2); TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("0X4d, 0x5a", &b), E_OK);    TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("4D\r\n5A\n", &b), E_OK);    TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("\t4Dh;5Ah ", &b), E_OK);    TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("0x4D5A", &b), E_OK);        TF_EQ_U(b.size(), 2); TF_EQ_MEM(b.data(), k4d5a, 2);
  TF_EQ_I(pb("0x4", &b), E_OK);           TF_EQ_U(b.size(), 1); TF_EQ_U(b[0], 4);
  TF_EQ_I(pb("Fh", &b), E_OK);            TF_EQ_U(b.size(), 1); TF_EQ_U(b[0], 15);
  TF_EQ_I(pb("00 FF 7f 80", &b), E_OK);   TF_EQ_U(b.size(), 4); TF_EQ_U(b[1], 0xFF); TF_EQ_U(b[2], 0x7F);
  TF_EQ_I(pb("", &b), E_SYNTAX);
  TF_EQ_I(pb("  , ;\r\n", &b), E_SYNTAX);
  TF_EQ_I(pb("4", &b), E_SYNTAX);         // single digit needs 0x or h
  TF_EQ_I(pb("4D5", &b), E_SYNTAX);       // odd digit count
  TF_EQ_I(pb("hello", &b), E_SYNTAX);
  TF_EQ_I(pb("4G", &b), E_SYNTAX);
  TF_EQ_I(pb("0x", &b), E_SYNTAX);
  TF_EQ_I(pb("4D ??", &b), E_SYNTAX);     // wildcards only in patterns
  TF_EQ_I(pb("cafe", &b), E_OK);          TF_EQ_U(b.size(), 2); TF_EQ_U(b[0], 0xCA); TF_EQ_U(b[1], 0xFE);
  TF_CHECK(hex_parse_bytes(0, 0, &b) == E_SYNTAX);
  // a long run
  {
    EStr s;
    u32 i;
    for (i = 0; i < 1000; i++) s.appendf("%02X ", i & 0xFF);
    TF_EQ_I(pb(s.c_str(), &b), E_OK);
    TF_EQ_U(b.size(), 1000);
    TF_EQ_U(b[999], 999 & 0xFF);
  }
  // patterns
  TF_EQ_I(pp("DE AD ?? EF", &b, &m), E_OK);
  TF_EQ_U(b.size(), 4); TF_EQ_U(m.size(), 4);
  TF_EQ_U(b[0], 0xDE); TF_EQ_U(m[0], 0xFF); TF_EQ_U(b[2], 0); TF_EQ_U(m[2], 0); TF_EQ_U(b[3], 0xEF);
  TF_EQ_I(pp("DEAD??EF", &b, &m), E_OK);  TF_EQ_U(b.size(), 4); TF_EQ_U(m[2], 0);
  TF_EQ_I(pp("D? ?E", &b, &m), E_OK);
  TF_EQ_U(b[0], 0xD0); TF_EQ_U(m[0], 0xF0); TF_EQ_U(b[1], 0x0E); TF_EQ_U(m[1], 0x0F);
  TF_EQ_I(pp("4d 5a", &b, &m), E_OK);     TF_CHECK(bytes_are(b, "4D5A")); TF_EQ_U(m[0], 0xFF); TF_EQ_U(m[1], 0xFF);
  TF_EQ_I(pp("DE A", &b, &m), E_SYNTAX);
  TF_EQ_I(pp("", &b, &m), E_SYNTAX);
  TF_EQ_I(pp("XY", &b, &m), E_SYNTAX);
  {
    ecp nonascii[2] = { 0x4E00, 'A' };
    TF_EQ_I(hex_parse_pattern(nonascii, 2, &b, &m), E_SYNTAX);
  }
  TF_EQ_I(hex_parse_pattern(0, 0, &b, 0), E_SYNTAX);
}

struct HexOffCase { const char* expr; epos cur, size; int rc; epos want; const char* err; };

static void test_goto_offset(TF* tf)
{
  static const HexOffCase k[] = {
    { "1F40", 0, 0x10000, E_OK, 0x1F40, 0 },
    { "0x1F40", 0, 0x10000, E_OK, 0x1F40, 0 },
    { "0X1f40", 0, 0x10000, E_OK, 0x1F40, 0 },
    { "1F40h", 0, 0x10000, E_OK, 0x1F40, 0 },
    { "1f40H", 0, 0x10000, E_OK, 0x1F40, 0 },
    { "#8000", 0, 0x10000, E_OK, 8000, 0 },
    { "8000d", 0, 0x10000, E_OK, 8000, 0 },
    { "8000D", 0, 0x10000, E_OK, 8000, 0 },
    { "100", 0, 0x10000, E_OK, 0x100, 0 },          // hex is the default
    { "  1F 40  ", 0, 0x10000, E_OK, 0x1F40, 0 },   // blanks ignored
    { "+100", 0x50, 0x10000, E_OK, 0x150, 0 },
    { "-0x20", 0x50, 0x10000, E_OK, 0x30, 0 },
    { "- 20", 0x50, 0x10000, E_OK, 0x30, 0 },
    { "+#10", 0x50, 0x10000, E_OK, 0x5A, 0 },
    { "-10d", 0x50, 0x10000, E_OK, 0x46, 0 },
    { "end", 0, 0x1F40, E_OK, 0x1F40, 0 },
    { "END", 0, 0x1F40, E_OK, 0x1F40, 0 },
    { "end-16", 0, 0x1F40, E_OK, 0x1F40 - 0x16, 0 },
    { "end-#16", 0, 0x1F40, E_OK, 0x1F40 - 16, 0 },
    { "end - 0x10", 0, 0x1F40, E_OK, 0x1F30, 0 },
    { "end+0", 0, 0x1F40, E_OK, 0x1F40, 0 },
    { "50%", 0, 1000, E_OK, 500, 0 },
    { "0%", 0, 1000, E_OK, 0, 0 },
    { "100%", 0, 1000, E_OK, 1000, 0 },
    { "33%", 0, 0xFFFFFFFEu, E_OK, (epos)(((u64)0xFFFFFFFEu * 33) / 100), 0 },
    { "0", 5, 0, E_OK, 0, 0 },
    { "ed", 0, 0x1000, E_OK, 0xED, 0 },             // 'd' after a non-decimal digit: plain hex
    { "FFFFFFFE", 0, 0xFFFFFFFEu, E_OK, 0xFFFFFFFEu, 0 },
    { "", 0, 100, E_SYNTAX, 0, "Enter an offset" },
    { "   ", 0, 100, E_SYNTAX, 0, "Enter an offset" },
    { "1F41", 0, 0x1F40, E_RANGE, 0, "Offset beyond end of file (size 0x1F40)" },
    { "end+1", 0, 0x1F40, E_RANGE, 0, "Offset beyond end of file (size 0x1F40)" },
    { "-0x60", 0x50, 0x1000, E_RANGE, 0, "Offset before the start of the file" },
    { "end-0x1F41", 0, 0x1F40, E_RANGE, 0, "Offset before the start of the file" },
    { "101%", 0, 1000, E_RANGE, 0, "Percent must be 0-100" },
    { "1F%", 0, 1000, E_SYNTAX, 0, "Percent must be a decimal number 0-100" },
    { "100000000", 0, 0xFFFFFFFEu, E_RANGE, 0, "Number too large (max 0xFFFFFFFF)" },
    { "#4294967296", 0, 0xFFFFFFFEu, E_RANGE, 0, "Number too large (max 0xFFFFFFFF)" },
    { "xyz", 0, 100, E_SYNTAX, 0, 0 },
    { "0x", 0, 100, E_SYNTAX, 0, 0 },
    { "#", 0, 100, E_SYNTAX, 0, 0 },
    { "#1A", 0, 100, E_SYNTAX, 0, 0 },
    { "+", 0, 100, E_SYNTAX, 0, 0 },
    { "end-", 0, 100, E_SYNTAX, 0, 0 },
    { "end5", 0, 100, E_SYNTAX, 0, 0 },
    { "1-2", 0, 100, E_SYNTAX, 0, 0 },
    { "++1", 0, 100, E_SYNTAX, 0, 0 },
    { "12.5%", 0, 100, E_SYNTAX, 0, 0 },
  };
  u32 i;
  for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
    char err[96];
    epos out = 0xDEADBEEFu;
    int rc = goto_parse_offset(k[i].expr, k[i].cur, k[i].size, &out, err, sizeof(err));
    if (rc != k[i].rc) { tf_fail(tf, __FILE__, __LINE__, "offset '%s': rc %d, want %d (%s)", k[i].expr, rc, k[i].rc, err); continue; }
    tf_check(tf, 1, __FILE__, __LINE__, "rc");
    if (rc == E_OK) {
      if (out != k[i].want) tf_fail(tf, __FILE__, __LINE__, "offset '%s': got 0x%X want 0x%X", k[i].expr, out, k[i].want);
      TF_EQ_S(err, "");
    } else {
      TF_CHECK(err[0] != 0);
      TF_CHECK(out == 0xDEADBEEFu);
      if (k[i].err && strcmp(err, k[i].err) != 0)
        tf_fail(tf, __FILE__, __LINE__, "offset '%s': err '%s' want '%s'", k[i].expr, err, k[i].err);
    }
  }
  // NULL / tiny error buffers are fine
  {
    epos out;
    char tiny[4];
    TF_EQ_I(goto_parse_offset("zz", 0, 10, &out, 0, 0), E_SYNTAX);
    TF_EQ_I(goto_parse_offset("zz", 0, 10, &out, tiny, sizeof(tiny)), E_SYNTAX);
    TF_EQ_U(es_len(tiny), 3);
    TF_EQ_I(goto_parse_offset(0, 0, 10, &out, 0, 0), E_SYNTAX);
    {
      char longexpr[200];
      u32 j;
      for (j = 0; j < 150; j++) longexpr[j] = '0';
      longexpr[150] = 0;
      TF_EQ_I(goto_parse_offset(longexpr, 0, 10, &out, tiny, sizeof(tiny)), E_SYNTAX);
    }
  }
}

struct HexLineCase { const char* expr; u32 cur, count; int rc; u32 line, col; const char* err; };

static void test_goto_line(TF* tf)
{
  static const HexLineCase k[] = {
    { "120", 0, 500, E_OK, 119, 0, 0 },
    { "1", 10, 500, E_OK, 0, 0, 0 },
    { "500", 0, 500, E_OK, 499, 0, 0 },
    { "120:5", 0, 500, E_OK, 119, 4, 0 },
    { " 120 : 5 ", 0, 500, E_OK, 119, 4, 0 },
    { "120:1", 0, 500, E_OK, 119, 0, 0 },
    { "+10", 9, 500, E_OK, 19, 0, 0 },     // cur_line 9 = line 10 (1-based)
    { "-5", 9, 500, E_OK, 4, 0, 0 },
    { "-9", 9, 500, E_OK, 0, 0, 0 },
    { "+0", 9, 500, E_OK, 9, 0, 0 },
    { "end", 0, 500, E_OK, 499, 0, 0 },
    { "END", 0, 500, E_OK, 499, 0, 0 },
    { "end-1", 0, 500, E_OK, 498, 0, 0 },
    { "end:3", 0, 500, E_OK, 499, 2, 0 },
    { ":7", 41, 500, E_OK, 41, 6, 0 },
    { "+2:3", 0, 500, E_OK, 2, 2, 0 },
    { "1", 0, 0, E_OK, 0, 0, 0 },          // line_count 0 is treated as 1
    { "4294967295", 0, 0xFFFFFFFFu, E_OK, 0xFFFFFFFEu, 0, 0 },
    { "", 0, 500, E_SYNTAX, 0, 0, "Enter a line number" },
    { "0", 0, 500, E_RANGE, 0, 0, "Lines start at 1" },
    { "501", 0, 500, E_RANGE, 0, 0, "Line beyond end of file (500 lines)" },
    { "+491", 9, 500, E_RANGE, 0, 0, "Line beyond end of file (500 lines)" },
    { "-10", 9, 500, E_RANGE, 0, 0, "Line before the start of the file" },
    { "end-500", 0, 500, E_RANGE, 0, 0, "Line before the start of the file" },
    { "end+1", 0, 500, E_RANGE, 0, 0, "Line beyond end of file (500 lines)" },
    { "12:0", 0, 500, E_RANGE, 0, 0, "Columns start at 1" },
    { "12:", 0, 500, E_SYNTAX, 0, 0, "Enter a column after ':'" },
    { "4294967296", 0, 500, E_RANGE, 0, 0, "Line number too large" },
    { "1:4294967296", 0, 500, E_RANGE, 0, 0, "Column number too large" },
    { "abc", 0, 500, E_SYNTAX, 0, 0, 0 },
    { "0x10", 0, 500, E_SYNTAX, 0, 0, 0 },
    { "12:x", 0, 500, E_SYNTAX, 0, 0, 0 },
    { "1:2:3", 0, 500, E_SYNTAX, 0, 0, 0 },
    { "+", 0, 500, E_SYNTAX, 0, 0, 0 },
    { "end5", 0, 500, E_SYNTAX, 0, 0, 0 },
    { "-", 0, 500, E_SYNTAX, 0, 0, 0 },
  };
  u32 i;
  for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
    char err[96];
    u32 line = 0xDEADBEEFu, col = 0xDEADBEEFu;
    int rc = goto_parse_line(k[i].expr, k[i].cur, k[i].count, &line, &col, err, sizeof(err));
    if (rc != k[i].rc) { tf_fail(tf, __FILE__, __LINE__, "line '%s': rc %d, want %d (%s)", k[i].expr, rc, k[i].rc, err); continue; }
    tf_check(tf, 1, __FILE__, __LINE__, "rc");
    if (rc == E_OK) {
      if (line != k[i].line || col != k[i].col)
        tf_fail(tf, __FILE__, __LINE__, "line '%s': got %u:%u want %u:%u", k[i].expr, line, col, k[i].line, k[i].col);
      TF_EQ_S(err, "");
    } else {
      TF_CHECK(err[0] != 0);
      TF_CHECK(line == 0xDEADBEEFu && col == 0xDEADBEEFu);
      if (k[i].err && strcmp(err, k[i].err) != 0)
        tf_fail(tf, __FILE__, __LINE__, "line '%s': err '%s' want '%s'", k[i].expr, err, k[i].err);
    }
  }
  TF_EQ_I(goto_parse_line(0, 0, 10, 0, 0, 0, 0), E_SYNTAX);
  TF_EQ_I(goto_parse_line("3", 0, 10, 0, 0, 0, 0), E_OK);   // NULL outputs allowed
}

static void insp(const u8* p, u32 n, int be, InspectOut* o)
{
  memset(o, 0x55, sizeof(*o));
  inspect_bytes(p, n, be, o);
}

static void test_inspect(TF* tf)
{
  InspectOut o;
  static const u8 ff[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  static const u8 seq[8] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
  static const u8 fffe[2] = { 0xFF, 0xFE };
  static const u8 min64le[8] = { 0, 0, 0, 0, 0, 0, 0, 0x80 };
  static const u8 min64be[8] = { 0x80, 0, 0, 0, 0, 0, 0, 0 };
  static const u8 one_f[4] = { 0x00, 0x00, 0x80, 0x3F };
  static const u8 tenth_f[4] = { 0xCD, 0xCC, 0xCC, 0x3D };
  static const u8 max_f[4] = { 0xFF, 0xFF, 0x7F, 0x7F };
  static const u8 den_f[4] = { 0x01, 0x00, 0x00, 0x00 };
  static const u8 inf_f[4] = { 0x00, 0x00, 0x80, 0x7F };
  static const u8 ninf_f[4] = { 0x00, 0x00, 0x80, 0xFF };
  static const u8 nan_f[4] = { 0x01, 0x00, 0xC0, 0x7F };
  static const u8 nzero_f[4] = { 0x00, 0x00, 0x00, 0x80 };
  static const u8 tenth_d[8] = { 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F };
  static const u8 e300_d[8] = { 0x9C, 0x75, 0x00, 0x88, 0x3C, 0xE4, 0x37, 0x7E };
  static const u8 den_d[8] = { 0x01, 0, 0, 0, 0, 0, 0, 0 };
  static const u8 inf_d[8] = { 0, 0, 0, 0, 0, 0, 0xF0, 0x7F };
  static const u8 ninf_d[8] = { 0, 0, 0, 0, 0, 0, 0xF0, 0xFF };
  static const u8 nan_d[8] = { 1, 0, 0, 0, 0, 0, 0xF8, 0x7F };
  static const u8 neg25_d[8] = { 0, 0, 0, 0, 0, 0, 0x04, 0xC0 };
  static const u8 big_d[8] = { 0x00, 0x00, 0x00, 0x54, 0x34, 0x6F, 0x9D, 0x41 };
  static const u8 zero8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

  insp(ff, 8, 0, &o);
  TF_EQ_S(o.i8, "-1"); TF_EQ_S(o.u8_, "255"); TF_EQ_S(o.i16, "-1"); TF_EQ_S(o.u16_, "65535");
  TF_EQ_S(o.i32_, "-1"); TF_EQ_S(o.u32_, "4294967295"); TF_EQ_S(o.i64, "-1");
  TF_EQ_S(o.u64_, "18446744073709551615"); TF_EQ_S(o.f32, "NaN"); TF_EQ_S(o.f64, "NaN"); TF_EQ_S(o.bin, "11111111");
  TF_EQ_U(o.avail, 8);

  insp(fffe, 2, 0, &o);                            // the DESIGN 9.1 example
  TF_EQ_S(o.i8, "-1"); TF_EQ_S(o.u8_, "255"); TF_EQ_S(o.i16, "-257"); TF_EQ_S(o.u16_, "65279");
  TF_EQ_S(o.i32_, "-"); TF_EQ_S(o.u32_, "-"); TF_EQ_S(o.i64, "-"); TF_EQ_S(o.u64_, "-");
  TF_EQ_S(o.f32, "-"); TF_EQ_S(o.f64, "-"); TF_EQ_U(o.avail, 2);
  insp(fffe, 2, 1, &o);
  TF_EQ_S(o.i16, "-2"); TF_EQ_S(o.u16_, "65534");

  insp(seq, 8, 0, &o);
  TF_EQ_S(o.i8, "18"); TF_EQ_S(o.bin, "00010010");
  TF_EQ_S(o.i16, "13330"); TF_EQ_S(o.u16_, "13330"); TF_EQ_S(o.i32_, "2018915346"); TF_EQ_S(o.u32_, "2018915346");
  TF_EQ_S(o.i64, "-1090226688147180526"); TF_EQ_S(o.u64_, "17356517385562371090");
  TF_EQ_S(o.f32, "1.73782444e+34"); TF_EQ_S(o.f64, "-4.8864596550437747e+235");
  insp(seq, 8, 1, &o);
  TF_EQ_S(o.i16, "4660"); TF_EQ_S(o.u16_, "4660"); TF_EQ_S(o.i32_, "305419896"); TF_EQ_S(o.u32_, "305419896");
  TF_EQ_S(o.i64, "1311768467463790320"); TF_EQ_S(o.u64_, "1311768467463790320");
  TF_EQ_S(o.f32, "5.69045661e-28"); TF_EQ_S(o.f64, "5.626349274901198e-221");

  insp(min64le, 8, 0, &o);
  TF_EQ_S(o.i64, "-9223372036854775808"); TF_EQ_S(o.u64_, "9223372036854775808"); TF_EQ_S(o.f64, "-0");
  TF_EQ_S(o.i32_, "0"); TF_EQ_S(o.f32, "0");
  insp(min64be, 8, 1, &o);
  TF_EQ_S(o.i64, "-9223372036854775808"); TF_EQ_S(o.f64, "-0");
  TF_EQ_S(o.i32_, "-2147483648"); TF_EQ_S(o.u32_, "2147483648"); TF_EQ_S(o.f32, "-0");
  TF_EQ_S(o.i8, "-128"); TF_EQ_S(o.i16, "-32768"); TF_EQ_S(o.u16_, "32768"); TF_EQ_S(o.bin, "10000000");

  insp(zero8, 8, 0, &o);
  TF_EQ_S(o.i64, "0"); TF_EQ_S(o.u64_, "0"); TF_EQ_S(o.f32, "0"); TF_EQ_S(o.f64, "0"); TF_EQ_S(o.bin, "00000000");

  insp(one_f, 4, 0, &o);    TF_EQ_S(o.f32, "1"); TF_EQ_S(o.f64, "-"); TF_EQ_U(o.avail, 4);
  insp(tenth_f, 4, 0, &o);  TF_EQ_S(o.f32, "0.100000001");
  insp(max_f, 4, 0, &o);    TF_EQ_S(o.f32, "3.40282347e+38");
  insp(den_f, 4, 0, &o);    TF_EQ_S(o.f32, "1.40129846e-45");
  insp(inf_f, 4, 0, &o);    TF_EQ_S(o.f32, "+Inf");
  insp(ninf_f, 4, 0, &o);   TF_EQ_S(o.f32, "-Inf");
  insp(nan_f, 4, 0, &o);    TF_EQ_S(o.f32, "NaN");
  insp(nzero_f, 4, 0, &o);  TF_EQ_S(o.f32, "-0"); TF_EQ_S(o.i32_, "-2147483648");
  {
    u8 r[4];                                       // the same float big-endian
    r[0] = one_f[3]; r[1] = one_f[2]; r[2] = one_f[1]; r[3] = one_f[0];
    insp(r, 4, 1, &o); TF_EQ_S(o.f32, "1");
  }
  insp(tenth_d, 8, 0, &o);  TF_EQ_S(o.f64, "0.10000000000000001");
  insp(e300_d, 8, 0, &o);   TF_EQ_S(o.f64, "1.0000000000000001e+300");
  insp(den_d, 8, 0, &o);    TF_EQ_S(o.f64, "4.9406564584124654e-324");
  insp(inf_d, 8, 0, &o);    TF_EQ_S(o.f64, "+Inf");
  insp(ninf_d, 8, 0, &o);   TF_EQ_S(o.f64, "-Inf");
  insp(nan_d, 8, 0, &o);    TF_EQ_S(o.f64, "NaN");
  insp(neg25_d, 8, 0, &o);  TF_EQ_S(o.f64, "-2.5");
  insp(big_d, 8, 0, &o);    TF_EQ_S(o.f64, "123456789");

  insp(seq, 3, 0, &o);
  TF_EQ_S(o.i16, "13330"); TF_EQ_S(o.i32_, "-"); TF_EQ_U(o.avail, 3);
  insp(seq, 7, 0, &o);
  TF_EQ_S(o.i32_, "2018915346"); TF_EQ_S(o.i64, "-"); TF_EQ_S(o.f64, "-"); TF_EQ_U(o.avail, 7);
  insp(seq, 100, 0, &o);
  TF_EQ_U(o.avail, 8); TF_EQ_S(o.u64_, "17356517385562371090");
  insp(seq, 0, 0, &o);
  TF_EQ_S(o.i8, "-"); TF_EQ_S(o.u8_, "-"); TF_EQ_S(o.bin, "-"); TF_EQ_U(o.avail, 0);
  insp(0, 5, 0, &o);
  TF_EQ_S(o.i8, "-"); TF_EQ_U(o.avail, 0);
  TF_CHECK(inspect_bytes(seq, 8, 0, 0) != E_OK);
  // every byte value: i8/u8 consistent
  {
    u32 b;
    for (b = 0; b < 256; b++) {
      u8 x = (u8)b;
      char want[8];
      insp(&x, 1, 0, &o);
      es_fmt(want, sizeof(want), "%u", b);
      TF_EQ_S(o.u8_, want);
      es_fmt(want, sizeof(want), "%d", b < 128 ? (int)b : (int)b - 256);
      TF_EQ_S(o.i8, want);
    }
  }
}

void suite_hexfmt(TF* tf)
{
  test_text(tf);
  test_glyph(tf);
  test_parse_bytes(tf);
  test_goto_offset(tf);
  test_goto_line(tf);
  test_inspect(tf);
}
