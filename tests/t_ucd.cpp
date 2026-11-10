// t_ucd.cpp - suite "ucd" (DESIGN 6.11): spot checks of width, case mappings, word/space/drawable classes, fold
// idempotence over all code points, and per-block checksums against Python (tests/data/codec/ucd_blocks.txt).
// Owner: T02.
#include "tf.h"
#include "ucd.h"
#include <stdio.h>
#include <string.h>

static u8* load_blocks(TF* tf, u32* n)
{
  char path[512];
  FILE* f;
  long sz;
  u8* buf;
  *n = 0;
#if defined(_WIN32)
  es_fmt(path, sizeof(path), "%s\\codec\\ucd_blocks.txt", tf->data_dir ? tf->data_dir : "tests\\data");
#else
  es_fmt(path, sizeof(path), "%s/codec/ucd_blocks.txt", tf->data_dir ? tf->data_dir : "tests/data");
#endif
  f = fopen(path, "rb");
  if (!f) { tf_fail(tf, __FILE__, __LINE__, "missing data file %s (make -C tests data)", path); return 0; }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (u8*)e_alloc((u32)sz + 1);
  if (!buf) { fclose(f); TF_CHECK(0); return 0; }
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); e_free(buf); TF_CHECK(0); return 0; }
  fclose(f);
  buf[sz] = 0;
  *n = (u32)sz;
  return buf;
}

static u32 hex_at(const u8* p, u32 n)
{
  u32 v = 0, i;
  for (i = 0; i < n; i++) {
    u8 c = p[i];
    v = v * 16 + (u32)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
  }
  return v;
}

static void test_spots(TF* tf)
{
  // case folding (TASKS T02 acceptance: 'A', A-umlaut, Zhe, capital sharp s)
  TF_EQ_U(uc_fold('A'), 'a');
  TF_EQ_U(uc_fold('a'), 'a');
  TF_EQ_U(uc_fold(0xC4), 0xE4);
  TF_EQ_U(uc_fold(0x416), 0x436);
  TF_EQ_U(uc_fold(0x1E9E), 0xDF);
  TF_EQ_U(uc_fold(0xDF), 0xDF);
  TF_EQ_U(uc_fold(0x3A3), 0x3C3);
  TF_EQ_U(uc_fold(0x3C2), 0x3C3);                // final sigma
  TF_EQ_U(uc_fold(0x212A), 'k');                 // Kelvin sign
  TF_EQ_U(uc_fold(0x17F), 's');                  // long s
  TF_EQ_U(uc_fold(0x130), 0x130);                // dotted capital I has no simple folding
  TF_EQ_U(uc_fold(0x1F88), 0x1F80);
  TF_EQ_U(uc_fold(0x10400), 0x10428);            // Deseret (supplementary)
  TF_EQ_U(uc_fold(0xFF21), 0xFF41);              // fullwidth A
  TF_EQ_U(uc_fold(0x13F8), 0x13F0);              // Cherokee small ye -> capital
  TF_EQ_U(uc_fold(0xAB70), 0x13A0);
  TF_EQ_U(uc_fold(0x4E2D), 0x4E2D);
  TF_EQ_U(uc_fold(ECP_BADBYTE(0x41)), ECP_BADBYTE(0x41));
  // upper / lower
  TF_EQ_U(uc_upper('a'), 'A');
  TF_EQ_U(uc_upper(0xE4), 0xC4);
  TF_EQ_U(uc_upper(0xFF), 0x178);
  TF_EQ_U(uc_upper(0xDF), 0xDF);
  TF_EQ_U(uc_upper(0xB5), 0x39C);
  TF_EQ_U(uc_upper(0x436), 0x416);
  TF_EQ_U(uc_upper(0x1F80), 0x1F88);
  TF_EQ_U(uc_upper(0x1FB3), 0x1FBC);
  TF_EQ_U(uc_upper(0x149), 0x149);
  TF_EQ_U(uc_upper(0x10428), 0x10400);
  TF_EQ_U(uc_upper(0x1C5), 0x1C4);
  TF_EQ_U(uc_lower(0x1C5), 0x1C6);
  TF_EQ_U(uc_lower('Z'), 'z');
  TF_EQ_U(uc_lower(0x178), 0xFF);
  TF_EQ_U(uc_lower(0x130), 'i');
  TF_EQ_U(uc_lower(0x1E9E), 0xDF);
  TF_EQ_U(uc_lower(0x3A3), 0x3C3);
  TF_EQ_U(uc_lower(0x416), 0x436);
  TF_EQ_U(uc_lower(0x10400), 0x10428);
  TF_EQ_U(uc_lower('1'), '1');
  TF_EQ_U(uc_upper(ECP_BADUNIT(0xD800)), ECP_BADUNIT(0xD800));
  // width
  TF_EQ_I(uc_width('A'), 1);
  TF_EQ_I(uc_width(0xE4), 1);
  TF_EQ_I(uc_width(0xAD), 1);                    // soft hyphen is visible
  TF_EQ_I(uc_width(0x301), 0);
  TF_EQ_I(uc_width(0x200B), 0);
  TF_EQ_I(uc_width(0x200D), 0);
  TF_EQ_I(uc_width(0xFEFF), 0);
  TF_EQ_I(uc_width(0x1160), 0);
  TF_EQ_I(uc_width(0x416), 1);
  TF_EQ_I(uc_width(0x1100), 2);
  TF_EQ_I(uc_width(0x4E2D), 2);
  TF_EQ_I(uc_width(0x3000), 2);
  TF_EQ_I(uc_width(0xFF21), 2);
  TF_EQ_I(uc_width(0xFF71), 1);                  // half-width katakana
  TF_EQ_I(uc_width(0xAC00), 2);
  TF_EQ_I(uc_width(0x1F600), 2);
  TF_EQ_I(uc_width(0x20000), 2);
  TF_EQ_I(uc_width(0x2FFF0), 2);                 // unassigned in the CJK plane: still wide
  TF_EQ_I(uc_width(0x378), 1);                   // unassigned elsewhere: 1
  TF_EQ_I(uc_width(0xE0001), 0);                 // language tag (Cf)
  TF_EQ_I(uc_width(0x0600), 1);                  // visible prepended mark
  TF_EQ_I(uc_width(ECP_BADBYTE(0xE9)), 1);
  // word
  TF_CHECK(uc_is_word('a') && uc_is_word('Z') && uc_is_word('0') && uc_is_word('_'));
  TF_CHECK(uc_is_word(0xE4) && uc_is_word(0xAA) && uc_is_word(0xB5) && uc_is_word(0x416) && uc_is_word(0x4E2D));
  TF_CHECK(uc_is_word(0x301) && uc_is_word(0x660) && uc_is_word(0x203F) && uc_is_word(0x10400));
  TF_CHECK(!uc_is_word(' ') && !uc_is_word('-') && !uc_is_word('.') && !uc_is_word(0xD7) && !uc_is_word(0xF7));
  TF_CHECK(!uc_is_word(0x2014) && !uc_is_word(0x3000) && !uc_is_word(0xA0) && !uc_is_word(0xB2));
  TF_CHECK(!uc_is_word(ECP_BADBYTE(0x41)) && !uc_is_word(ECP_BADUNIT(0xD800)) && !uc_is_word(0xD800));
  // space
  TF_CHECK(uc_is_space(' ') && uc_is_space('\t') && uc_is_space('\n') && uc_is_space('\r') && uc_is_space(0x0B));
  TF_CHECK(uc_is_space(0x85) && uc_is_space(0xA0) && uc_is_space(0x2003) && uc_is_space(0x2028) && uc_is_space(0x3000));
  TF_CHECK(!uc_is_space('a') && !uc_is_space(0x200B) && !uc_is_space(0x1C) && !uc_is_space(0) && !uc_is_space(0xFEFF));
  TF_CHECK(!uc_is_space(ECP_BADBYTE(0x20)));
  // drawable (WGL4 ranges)
  TF_CHECK(uc_is_drawable('A') && uc_is_drawable(0x416) && uc_is_drawable(0x1E9E) && uc_is_drawable(0x20AC));
  TF_CHECK(uc_is_drawable(0x2591) && uc_is_drawable(0xFB01) && uc_is_drawable(0xFFFD));
  TF_CHECK(!uc_is_drawable(0x1F) && !uc_is_drawable(0x4E2D) && !uc_is_drawable(0x1F600) && !uc_is_drawable(0x530));
  TF_CHECK(!uc_is_drawable(ECP_BADBYTE(0x41)));
}

static void test_invariants(TF* tf)
{
  u32 c, bad_fold = 0, bad_range = 0, bad_case = 0;
  for (c = 0; c < 0x110000u; c++) {
    ecp f = uc_fold(c), u = uc_upper(c), l = uc_lower(c);
    if (uc_fold(f) != f) bad_fold++;
    if (f >= 0x110000u || u >= 0x110000u || l >= 0x110000u) bad_range++;
    if (c >= 0xD800 && c <= 0xDFFF && (f != c || u != c || l != c)) bad_case++;
    if (uc_width(c) < 0 || uc_width(c) > 2) bad_range++;
  }
  TF_EQ_U(bad_fold, 0);
  TF_EQ_U(bad_range, 0);
  TF_EQ_U(bad_case, 0);
}

static void test_blocks(TF* tf)
{
  u32 n, i = 0, blocks = 0, fails = 0;
  u8* d = load_blocks(tf, &n);
  if (!d) return;
  while (i < n) {
    u32 j = i, blk, want, h = 0, c;
    while (j < n && d[j] != '\n') j++;
    if (d[i] != '#' && j - i >= 13) {
      blk = hex_at(d + i, 4);
      want = hex_at(d + i + 5, 8);
      for (c = blk * 256; c < blk * 256 + 256; c++) {
        h = h * 31 + (u32)uc_width(c);
        h = h * 31 + uc_fold(c);
        h = h * 31 + uc_upper(c);
        h = h * 31 + uc_lower(c);
        h = h * 31 + (u32)uc_is_word(c);
      }
      if (h != want) {
        fails++;
        if (fails <= 5) tf_fail(tf, __FILE__, __LINE__, "block %04X: checksum %08X, Python %08X", blk, h, want);
      }
      blocks++;
    }
    i = j + 1;
  }
  e_free(d);
  TF_EQ_U(blocks, 0x110000u / 256);
  TF_EQ_U(fails, 0);
}

void suite_ucd(TF* tf)
{
  test_spots(tf);
  test_invariants(tf);
  test_blocks(tf);
}
