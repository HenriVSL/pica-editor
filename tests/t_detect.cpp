// t_detect.cpp - suite "detect" (DESIGN 6.10, 12.1): a table of detection cases (BOMs, UTF-32, BOM-less UTF-16
// LE/BE, binary, pure ASCII, valid/invalid UTF-8, truncation at the sample end, CR-only, mixed EOL, options), plus
// detect_eol, detect_dominant_eol and detect_is_binary.
// Owner: T02.
#include "tf.h"
#include "detect.h"
#include <string.h>

enum { K_ANSI = 0, K_U8, K_U8B, K_LE, K_LEB, K_BE, K_BEB, K_1251 };

struct DCase {
  const char* name;
  const char* hex;          // input bytes, space separated
  u8 whole;                 // whole_file
  u8 opt;                   // bit 0: utf8_nobom off, bit 1: utf16_nobom off, bit 2: ansi = cp1251, bit 3: dflt LF
  u8 enc;                   // K_*
  u8 bom_len;
  u8 binary, pure_ascii, utf32, mixed;
  u8 crlf, lf, cr;
  u8 style;                 // EolStyle
};

static const DCase k_cases[] = {
  // name                       bytes                                             wh op enc    bl bi pa 32 mx crlf lf cr style
  { "empty",                    "",                                                1, 0, K_ANSI, 0, 0, 1, 0, 0, 0, 0, 0, EOL_CRLF },
  { "ascii",                    "41 42 43",                                        1, 0, K_ANSI, 0, 0, 1, 0, 0, 0, 0, 0, EOL_CRLF },
  { "ascii default LF",         "41 42 43",                                        1, 8, K_ANSI, 0, 0, 1, 0, 0, 0, 0, 0, EOL_LF },
  { "utf8 bom only",            "EF BB BF",                                        1, 0, K_U8B,  3, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf8 bom crlf",            "EF BB BF 41 0D 0A 42",                            1, 0, K_U8B,  3, 0, 0, 0, 0, 1, 0, 0, EOL_CRLF },
  { "utf8 bom invalid body",    "EF BB BF C3 28 0A",                               1, 0, K_U8B,  3, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf8 bom, utf8 opt off",   "EF BB BF C3 A4",                                  1, 1, K_U8B,  3, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf16le bom only",         "FF FE",                                           1, 0, K_LEB,  2, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf16le bom crlf",         "FF FE 41 00 0D 00 0A 00",                         1, 0, K_LEB,  2, 0, 0, 0, 0, 1, 0, 0, EOL_CRLF },
  { "utf16be bom lf",           "FE FF 00 41 00 0A",                               1, 0, K_BEB,  2, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf16le bom cr only",      "FF FE 61 00 0D 00 62 00",                         1, 0, K_LEB,  2, 0, 0, 0, 0, 0, 0, 1, EOL_CR },
  { "utf16le misaligned 0D0A",  "FF FE 0D 0A 0D 0A",                               1, 0, K_LEB,  2, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf16be bom odd length",   "FE FF 00 41 00 0A 42",                            1, 0, K_BEB,  2, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf16le bom, opt off",     "FF FE 41 00",                                     1, 2, K_LEB,  2, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf32le bom",              "FF FE 00 00 41 00 00 00",                         1, 0, K_ANSI, 0, 1, 0, 1, 0, 0, 0, 0, EOL_CRLF },
  { "utf32be bom",              "00 00 FE FF 00 00 00 41",                         1, 0, K_ANSI, 0, 1, 0, 1, 0, 0, 0, 0, EOL_CRLF },
  { "utf16le nobom",            "48 00 65 00 6C 00 6C 00 6F 00 0D 00 0A 00 57 00 6F 00 72 00",
                                                                                   1, 0, K_LE,   0, 0, 0, 0, 0, 1, 0, 0, EOL_CRLF },
  { "utf16be nobom",            "00 48 00 65 00 6C 00 6C 00 6F 00 0A 00 0A 00 57 00 6F 00 72",
                                                                                   1, 0, K_BE,   0, 0, 0, 0, 0, 0, 2, 0, EOL_LF },
  { "utf16le nobom opt off",    "48 00 65 00 6C 00 6C 00 6F 00 0D 00 0A 00 57 00 6F 00 72 00",
                                                                                   1, 2, K_ANSI, 0, 1, 0, 0, 0, 0, 1, 1, EOL_LF },
  { "utf16le nobom < 16 bytes", "48 00 69 00 0A 00",                               1, 0, K_ANSI, 0, 1, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf16le nobom cyrillic",   "1F 04 40 04 38 04 32 04 35 04 42 04 0D 00 0A 00",  1, 0, K_ANSI, 0, 1, 0, 0, 0, 0, 1, 1, EOL_LF },
  { "utf16le nobom partial CR", "48 00 65 00 6C 00 6C 00 6F 00 0A 00 57 00 0D 00",  0, 0, K_LE,   0, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf16le nobom whole CR",   "48 00 65 00 6C 00 6C 00 6F 00 0A 00 57 00 0D 00",  1, 0, K_LE,   0, 0, 0, 0, 0, 0, 1, 1, EOL_LF },
  { "nul in ascii",             "41 00 42",                                        1, 0, K_ANSI, 0, 1, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "binary with LF",           "00 01 0A 02",                                     1, 0, K_ANSI, 0, 1, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "all zero",                 "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00",
                                                                                   1, 0, K_ANSI, 0, 1, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "nul + utf8",               "C3 A4 00",                                        1, 0, K_ANSI, 0, 1, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf8 nobom",               "C3 A4 0A",                                        1, 0, K_U8,   0, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf8 nobom opt off",       "C3 A4 0A",                                        1, 1, K_ANSI, 0, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "utf8 4-byte",              "F0 9F 98 80",                                     1, 0, K_U8,   0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf8 3-byte + ascii",      "61 E2 82 AC 62",                                  1, 0, K_U8,   0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf8 invalid C3 28",       "C3 28",                                           1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf8 valid then invalid",  "C3 A4 41 E4 0A",                                  1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 1, 0, EOL_LF },
  { "latin1",                   "E4 0D 0A",                                        1, 0, K_ANSI, 0, 0, 0, 0, 0, 1, 0, 0, EOL_CRLF },
  { "latin1 ansi=1251",         "E4 0D 0A",                                        1, 4, K_1251, 0, 0, 0, 0, 0, 1, 0, 0, EOL_CRLF },
  { "utf8 truncated partial",   "C3 A4 E2 82",                                     0, 0, K_U8,   0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "utf8 truncated whole",     "C3 A4 E2 82",                                     1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "only truncated partial",   "41 E2 82",                                        0, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "truncated mid-sample",     "E2 82 41 C3 A4",                                  0, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "overlong",                 "C0 AF",                                           1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "surrogate",                "ED A0 80",                                        1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "above 10FFFF",             "F4 90 80 80",                                     1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "lone continuation",        "41 80 42",                                        1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "incomplete bom",           "EF BB",                                           1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "single FF",                "FF",                                              1, 0, K_ANSI, 0, 0, 0, 0, 0, 0, 0, 0, EOL_CRLF },
  { "cr only",                  "61 0D 62 0D",                                     1, 0, K_ANSI, 0, 0, 1, 0, 0, 0, 0, 2, EOL_CR },
  { "cr only partial",          "61 0D 62 0D",                                     0, 0, K_ANSI, 0, 0, 1, 0, 0, 0, 0, 1, EOL_CR },
  { "lf only",                  "61 0A 62 0A",                                     1, 0, K_ANSI, 0, 0, 1, 0, 0, 0, 2, 0, EOL_LF },
  { "crlf only",                "61 0D 0A 62 0D 0A",                               1, 0, K_ANSI, 0, 0, 1, 0, 0, 2, 0, 0, EOL_CRLF },
  { "mixed lf wins",            "61 0D 0A 62 0A 63 0A",                            1, 0, K_ANSI, 0, 0, 1, 0, 1, 1, 2, 0, EOL_LF },
  { "mixed tie -> crlf",        "0D 0A 0A",                                        1, 0, K_ANSI, 0, 0, 1, 0, 1, 1, 1, 0, EOL_CRLF },
  { "crlf + lone cr",           "61 0D 0A 62 0D 63",                               1, 0, K_ANSI, 0, 0, 1, 0, 0, 1, 0, 1, EOL_CRLF },
  { "lf + lone cr",             "61 0A 62 0D 63 0A",                               1, 0, K_ANSI, 0, 0, 1, 0, 0, 0, 2, 1, EOL_LF },
  { "cr at partial end",        "61 0D 0A 62 0D",                                  0, 0, K_ANSI, 0, 0, 1, 0, 0, 1, 0, 0, EOL_CRLF },
  { "cr at whole end",          "61 0D 0A 62 0D",                                  1, 0, K_ANSI, 0, 0, 1, 0, 0, 1, 0, 1, EOL_CRLF },
  { "cr cr lf",                 "0D 0D 0A",                                        1, 0, K_ANSI, 0, 0, 1, 0, 0, 1, 0, 1, EOL_CRLF },
  { "lf cr (mac-ish reversed)", "0A 0D 0A 0D",                                     1, 0, K_ANSI, 0, 0, 1, 0, 1, 1, 1, 1, EOL_CRLF },
};

static u32 hexb(const char* s, u8* out, u32 cap)
{
  u32 k = 0;
  while (*s && k < cap) {
    while (*s == ' ') s++;
    if (!*s) break;
    out[k++] = (u8)(((s[0] <= '9' ? s[0] - '0' : s[0] - 'A' + 10) << 4) | (s[1] <= '9' ? s[1] - '0' : s[1] - 'A' + 10));
    s += 2;
  }
  return k;
}

static EncSpec kind_spec(u8 k)
{
  switch (k) {
  case K_U8: return enc_utf8(0);
  case K_U8B: return enc_utf8(1);
  case K_LE: return enc_utf16le(0);
  case K_LEB: return enc_utf16le(1);
  case K_BE: return enc_utf16be(0);
  case K_BEB: return enc_utf16be(1);
  case K_1251: return enc_cp(1251);
  default: break;
  }
  return enc_cp(1252);
}

static DetectOpts make_opts(u8 opt)
{
  DetectOpts o;
  memset(&o, 0, sizeof(o));
  o.utf8_nobom = (u8)((opt & 1) ? 0 : 1);
  o.utf16_nobom = (u8)((opt & 2) ? 0 : 1);
  o.ansi = (opt & 4) ? enc_cp(1251) : enc_cp(1252);
  o.default_eol = (opt & 8) ? EOL_LF : EOL_CRLF;
  return o;
}

static void test_table(TF* tf)
{
  u32 i, ncases = sizeof(k_cases) / sizeof(k_cases[0]);
  u8 buf[128];
  TF_CHECK(ncases >= 40);
  for (i = 0; i < ncases; i++) {
    const DCase* c = &k_cases[i];
    DetectOpts o = make_opts(c->opt);
    DetectResult r;
    u32 n = hexb(c->hex, buf, sizeof(buf));
    EncSpec want = kind_spec(c->enc);
    int ok;
    TF_EQ_I(detect(buf, n, c->whole, o, &r), E_OK);
    ok = enc_equal(r.enc, want) && r.bom_len == c->bom_len && r.binary == c->binary &&
         r.pure_ascii == c->pure_ascii && r.utf32 == c->utf32 && r.eol_mixed == c->mixed && r.eol.crlf == c->crlf &&
         r.eol.lf == c->lf && r.eol.cr == c->cr && r.eol_style == (EolStyle)c->style;
    if (!ok) {
      tf_fail(tf, __FILE__, __LINE__,
              "case '%s': enc %u/%u/%u bom_len %u bin %u ascii %u u32 %u mixed %u eol %u/%u/%u style %u",
              c->name, (u32)r.enc.kind, (u32)r.enc.bom, (u32)r.enc.cp, r.bom_len, (u32)r.binary, (u32)r.pure_ascii,
              (u32)r.utf32, (u32)r.eol_mixed, r.eol.crlf, r.eol.lf, r.eol.cr, (u32)r.eol_style);
    } else TF_CHECK(ok);
  }
}

static void test_big_samples(TF* tf)
{
  enum { N = 65536 };
  u8* p = (u8*)e_alloc(N);
  DetectOpts o = make_opts(0);
  DetectResult r;
  u32 i;
  if (!p) { TF_CHECK(0); return; }
  // UTF-8 text, the 64 KB sample cuts a 3-byte character
  for (i = 0; i + 4 <= N; i += 4) { p[i] = 'a'; p[i + 1] = 0xE2; p[i + 2] = 0x82; p[i + 3] = 0xAC; }
  p[N - 4] = 'a'; p[N - 3] = 'b'; p[N - 2] = 0xE2; p[N - 1] = 0x82;
  detect(p, N, 0, o, &r);
  TF_CHECK(enc_equal(r.enc, enc_utf8(0)));
  detect(p, N, 1, o, &r);
  TF_CHECK(enc_equal(r.enc, enc_cp(1252)));
  // an invalid byte at the very end of a partial sample is still invalid (not a cut sequence)
  p[N - 2] = 'c'; p[N - 1] = 0xFF;
  detect(p, N, 0, o, &r);
  TF_CHECK(enc_equal(r.enc, enc_cp(1252)));
  // UTF-16 LE text with CRLF lines, ratio test over 32768 pairs
  for (i = 0; i + 2 <= N; i += 2) { p[i] = (u8)((i / 2) % 40 == 39 ? 0x0A : ((i / 2) % 40 == 38 ? 0x0D : 'x')); p[i + 1] = 0; }
  detect(p, N, 0, o, &r);
  TF_CHECK(enc_equal(r.enc, enc_utf16le(0)));
  TF_EQ_U(r.eol.crlf, N / 2 / 40);
  TF_EQ_U(r.eol_style, EOL_CRLF);
  // 6 % of the even bytes NUL as well: no longer UTF-16 (even ratio > 5 %) -> binary
  for (i = 0; i < N / 2; i += 16) p[2 * i] = 0;
  detect(p, N, 0, o, &r);
  TF_CHECK(r.binary == 1 && enc_equal(r.enc, enc_cp(1252)));
  TF_CHECK(detect_is_binary(p, N));
  e_free(p);
}

static void test_eol_helpers(TF* tf)
{
  EolCounts c;
  TF_EQ_I(detect_eol((const u8*)"a\r\nb\nc\rd", 8, enc_cp(1252), &c), E_OK);
  TF_CHECK(c.crlf == 1 && c.lf == 1 && c.cr == 1);
  TF_EQ_I(detect_eol((const u8*)"a\r", 2, enc_utf8(0), &c), E_OK);      // whole buffer: the final CR counts
  TF_CHECK(c.crlf == 0 && c.lf == 0 && c.cr == 1);
  TF_EQ_I(detect_eol((const u8*)"\0\r\0\n\0\n\0\r", 8, enc_utf16be(0), &c), E_OK);
  TF_CHECK(c.crlf == 1 && c.lf == 1 && c.cr == 1);
  TF_EQ_I(detect_eol((const u8*)"\r\0\n\0", 4, enc_utf16le(0), &c), E_OK);
  TF_CHECK(c.crlf == 1 && c.lf == 0 && c.cr == 0);
  TF_EQ_I(detect_eol((const u8*)"\0\r\0\n", 4, enc_utf16le(0), &c), E_OK);   // BE bytes read as LE: no breaks
  TF_CHECK(c.crlf == 0 && c.lf == 0 && c.cr == 0);
  // Shift-JIS bytes: trail bytes never look like CR/LF
  TF_EQ_I(detect_eol((const u8*)"\x93\xFA\r\n\x96\x7B\n", 7, enc_cp(932), &c), E_OK);
  TF_CHECK(c.crlf == 1 && c.lf == 1 && c.cr == 0);
  TF_EQ_I(detect_eol(0, 0, enc_cp(1252), &c), E_OK);
  TF_CHECK(c.crlf == 0 && c.lf == 0 && c.cr == 0);

  c.crlf = 0; c.lf = 0; c.cr = 3;
  TF_EQ_U(detect_dominant_eol(c, EOL_CRLF), EOL_CR);
  c.crlf = 1; c.lf = 0; c.cr = 3;
  TF_EQ_U(detect_dominant_eol(c, EOL_LF), EOL_CRLF);
  c.crlf = 2; c.lf = 3; c.cr = 0;
  TF_EQ_U(detect_dominant_eol(c, EOL_CRLF), EOL_LF);
  c.crlf = 3; c.lf = 3; c.cr = 0;
  TF_EQ_U(detect_dominant_eol(c, EOL_LF), EOL_CRLF);
  c.crlf = 0; c.lf = 0; c.cr = 0;
  TF_EQ_U(detect_dominant_eol(c, EOL_LF), EOL_LF);
  TF_EQ_U(detect_dominant_eol(c, EOL_CR), EOL_CR);
}

static void test_is_binary(TF* tf)
{
  u8 buf[64];
  u32 n;
  TF_EQ_I(detect_is_binary((const u8*)"hello\r\n", 7), 0);
  TF_EQ_I(detect_is_binary((const u8*)"he\0llo", 6), 1);
  TF_EQ_I(detect_is_binary(0, 0), 0);
  TF_EQ_I(detect_is_binary((const u8*)"", 0), 0);
  TF_EQ_I(detect_is_binary((const u8*)"\xFF\xFE" "a\0b\0", 6), 0);           // UTF-16 LE BOM
  TF_EQ_I(detect_is_binary((const u8*)"\xFE\xFF\0a\0b", 6), 0);              // UTF-16 BE BOM
  TF_EQ_I(detect_is_binary((const u8*)"\xFF\xFE\0\0a\0\0\0", 8), 1);         // UTF-32 LE BOM
  TF_EQ_I(detect_is_binary((const u8*)"\0\0\xFE\xFF\0\0\0a", 8), 1);         // UTF-32 BE BOM
  n = hexb("48 00 65 00 6C 00 6C 00 6F 00 0D 00 0A 00 57 00 6F 00 72 00", buf, sizeof(buf));
  TF_EQ_I(detect_is_binary(buf, n), 0);                                       // BOM-less UTF-16 LE pattern
  n = hexb("4D 5A 90 00 03 00 00 00 04 00 00 00 FF FF 00 00 B8 00", buf, sizeof(buf));
  TF_EQ_I(detect_is_binary(buf, n), 1);                                       // an EXE header
  TF_EQ_I(detect_is_binary((const u8*)"\xC3\xA4\xE4", 3), 0);                // high bytes are not binary
}

void suite_detect(TF* tf)
{
  enc_set_system(1252, 437);
  codec_init();
  test_table(tf);
  test_big_samples(tf);
  test_eol_helpers(tf);
  test_is_binary(tf);
}
