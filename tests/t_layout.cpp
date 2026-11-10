// t_layout.cpp - suite "layout": LineLayout tables (tabs, controls, bad bytes/units, wide, combining, non-drawable,
// show whitespace), hit testing both ways, wrap rows, LongLineIndex against full layouts. Owner: T06.
// Character widths come from a fixed fake property table (layout_int.h hook), so the tables do not depend on T02's
// ucd; a few checks at the end run against the real uc_width when it reports wide/combining characters.
#include "tf.h"
#include "layout.h"
#include "layout_int.h"
#include "ucd.h"
#include <string.h>

static int fake_width(ecp c)
{
  if ((c >= 0x0300 && c <= 0x036F) || c == 0x200B || c == 0xFEFF) return 0;
  if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xFF01 && c <= 0xFF60)) return 2;
  return 1;
}

static int fake_draw(ecp c)
{
  return (c >= 0x20 && c <= 0x52F) || (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x2000 && c <= 0x25FF) || c == 0xFFFD;
}

// UTF-16 LE test codec (independent of T02).
class LayU16Codec : public Codec {
public:
  LayU16Codec() { kind = EK_UTF16LE; unit = 2; max_len = 4; ascii_compat = 0; self_sync = 1; pad = 0; cp = 1200; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u32 u, u2;
    if (avail < 2) { if (!eof) return 0; *out = ECP_BADBYTE(p[0]); return 1; }
    u = (u32)p[0] | ((u32)p[1] << 8);
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (avail < 4) { if (!eof) return 0; *out = ECP_BADUNIT(u); return 2; }
      u2 = (u32)p[2] | ((u32)p[3] << 8);
      if (u2 >= 0xDC00 && u2 <= 0xDFFF) { *out = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00); return 4; }
      *out = ECP_BADUNIT(u); return 2;
    }
    if (u >= 0xDC00 && u <= 0xDFFF) { *out = ECP_BADUNIT(u); return 2; }
    *out = u;
    return 2;
  }
  u32 encode(ecp c, u8* out) const { (void)c; (void)out; return 0; }
};

static const Codec* utf8() { return codec_get(enc_utf8(0)); }

static LayoutOpts opts(u8 tw, u8 ws)
{
  LayoutOpts o;
  o.tab_width = tw; o.show_ws = ws; o.pad[0] = o.pad[1] = 0;
  return o;
}

static void enc8(const ecp* s, u32 n, EBytes* out)
{
  u32 i;
  out->clear();
  for (i = 0; i < n; i++) {
    char b[4];
    u32 k = es_utf8_encode(s[i], b);
    out->append(b, k);
  }
}

static int build8(LineLayout* L, const char* s, const LayoutOpts& o, u32 start)
{
  return L->build((const u8*)s, es_len(s), utf8(), o, start, 1);
}

static int glyphs_are(const LineLayout& L, u32 i, const char* s)
{
  const LayChar& c = L.chars()[i];
  u32 k, n = es_len(s);
  if (c.nglyph != n) return 0;
  for (k = 0; k < n; k++) if (L.glyphs()[c.glyph + k] != (u8)s[k]) return 0;
  return 1;
}

// structural invariants of any layout
static void check_invariants(TF* tf, const LineLayout& L, u32 start_cell)
{
  u32 i, g = 0, cell = start_cell, prev_off = 0, bad = 0;
  for (i = 0; i < L.count(); i++) {
    const LayChar& c = L.chars()[i];
    if (c.glyph != g) bad++;
    g += c.nglyph;
    if (i > 0 && c.off <= prev_off) bad++;
    if (i == 0 && c.off != 0) bad++;
    prev_off = c.off;
    if (c.kind == LK_ZERO) {
      if (i == 0 || c.width != 0) bad++;
    } else {
      if (c.cell != cell || c.width == 0) bad++;
      cell += c.width;
    }
    if (c.kind == LK_TAB && c.nglyph != c.width) bad++;
  }
  if (g != L.glyph_count()) bad++;
  if (cell != L.end_cell()) bad++;
  if (bad) tf_fail(tf, __FILE__, __LINE__, "layout invariants: %u problems", bad);
  else tf_check(tf, 1, __FILE__, __LINE__, "invariants");
}

static void test_names(TF* tf)
{
  u32 c;
  TF_EQ_S(lay_ctrl_name(0), "NUL"); TF_EQ_S(lay_ctrl_name(1), "SOH"); TF_EQ_S(lay_ctrl_name(9), "HT");
  TF_EQ_S(lay_ctrl_name(10), "LF"); TF_EQ_S(lay_ctrl_name(13), "CR"); TF_EQ_S(lay_ctrl_name(27), "ESC");
  TF_EQ_S(lay_ctrl_name(31), "US"); TF_EQ_S(lay_ctrl_name(127), "DEL"); TF_EQ_S(lay_ctrl_name(0x80), "PAD");
  TF_EQ_S(lay_ctrl_name(0x85), "NEL"); TF_EQ_S(lay_ctrl_name(0x9F), "APC");
  for (c = 0; c < 256; c++) {
    const char* n = lay_ctrl_name((u8)c);
    int ctrl = c < 0x20 || (c >= 0x7F && c <= 0x9F);
    if (ctrl) TF_CHECK(n && es_len(n) >= 2 && es_len(n) <= 3);
    else TF_CHECK(n == 0);
  }
}

static void test_tabs(TF* tf)
{
  static const u8 tws[6] = { 1, 2, 3, 4, 8, 16 };
  u32 t, k, start;
  LineLayout L;
  for (t = 0; t < 6; t++) {
    for (start = 0; start < 3; start++) {
      for (k = 0; k < 20; k++) {
        char s[40];
        u32 i, tw = tws[t], abs, w;
        LayoutOpts o = opts((u8)tw, (u8)(k & 1));
        for (i = 0; i < k; i++) s[i] = 'a';
        s[k] = '\t'; s[k + 1] = 'b'; s[k + 2] = 0;
        TF_EQ_I(build8(&L, s, o, start * 5), E_OK);
        TF_EQ_U(L.count(), k + 2);
        abs = start * 5 + k;
        w = tw - abs % tw;
        TF_EQ_U(L.chars()[k].kind, LK_TAB);
        TF_EQ_U(L.chars()[k].cell, abs);
        TF_EQ_U(L.chars()[k].width, w);
        TF_EQ_U(L.chars()[k].nglyph, w);
        TF_EQ_U(L.glyphs()[L.chars()[k].glyph], (k & 1) ? 0xBB : ' ');
        if (w > 1) TF_EQ_U(L.glyphs()[L.chars()[k].glyph + 1], ' ');
        TF_EQ_U(L.chars()[k + 1].cell, abs + w);
        TF_EQ_U((abs + w) % tw, 0);
        TF_EQ_U(L.end_cell(), abs + w + 1);
        check_invariants(tf, L, start * 5);
      }
    }
  }
  // tab_width 0 behaves as 1; consecutive tabs
  TF_EQ_I(build8(&L, "\t\t", opts(0, 0), 0), E_OK);
  TF_EQ_U(L.end_cell(), 2);
  TF_EQ_I(build8(&L, "\t\tx", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.chars()[1].cell, 4); TF_EQ_U(L.chars()[2].cell, 8);
  TF_EQ_I(build8(&L, "\t", opts(255, 1), 0), E_OK);
  TF_EQ_U(L.chars()[0].width, 255);
}

static void test_controls(TF* tf)
{
  LineLayout L;
  u32 c;
  for (c = 0; c < 0x80; c++) {
    u8 b[3];
    const char* nm;
    if (c == 9 || (c >= 0x20 && c < 0x7F)) continue;
    nm = lay_ctrl_name((u8)c);
    b[0] = 'a'; b[1] = (u8)c; b[2] = 'z';
    TF_EQ_I(L.build(b, 3, utf8(), opts(4, 0), 0, 1), E_OK);
    TF_EQ_U(L.count(), 3);
    TF_EQ_U(L.chars()[1].kind, LK_CTRL);
    TF_EQ_U(L.chars()[1].width, es_len(nm));
    TF_CHECK(glyphs_are(L, 1, nm));
    TF_EQ_U(L.chars()[2].cell, 1 + es_len(nm));
    check_invariants(tf, L, 0);
  }
  // lone CR inside a line, C1 control from UTF-8 (U+0085)
  TF_EQ_I(build8(&L, "a\rb", opts(4, 0), 0), E_OK);
  TF_CHECK(glyphs_are(L, 1, "CR")); TF_EQ_U(L.chars()[1].width, 2);
  TF_EQ_I(build8(&L, "\xC2\x85", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.count(), 1); TF_EQ_U(L.chars()[0].kind, LK_CTRL); TF_CHECK(glyphs_are(L, 0, "NEL"));
  // spaces with and without show_ws
  TF_EQ_I(build8(&L, "a b", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.chars()[1].kind, LK_SPACE); TF_EQ_U(L.glyphs()[1], ' ');
  TF_EQ_I(build8(&L, "a b", opts(4, 1), 0), E_OK);
  TF_EQ_U(L.glyphs()[1], 0xB7);
  // NBSP is an ordinary character
  TF_EQ_I(build8(&L, "\xC2\xA0", opts(4, 1), 0), E_OK);
  TF_EQ_U(L.chars()[0].kind, LK_CHAR); TF_EQ_U(L.glyphs()[0], 0xA0);
}

static void test_bad(TF* tf)
{
  LineLayout L;
  LayU16Codec u16;
  // UTF-8 bad bytes
  TF_EQ_I(build8(&L, "a\xFF" "b", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.count(), 3); TF_EQ_U(L.chars()[1].kind, LK_BAD); TF_EQ_U(L.chars()[1].width, 3);
  TF_CHECK(glyphs_are(L, 1, "xFF")); TF_EQ_U(L.chars()[2].cell, 4);
  TF_EQ_I(build8(&L, "\xC0\x80", opts(4, 0), 0), E_OK);          // overlong: two bad bytes
  TF_EQ_U(L.count(), 2); TF_CHECK(glyphs_are(L, 0, "xC0")); TF_CHECK(glyphs_are(L, 1, "x80"));
  TF_EQ_U(L.chars()[1].off, 1); TF_EQ_U(L.end_cell(), 6);
  TF_EQ_I(build8(&L, "\xE2\x82", opts(4, 0), 0), E_OK);          // truncated at the end of the file
  TF_EQ_U(L.count(), 2); TF_CHECK(glyphs_are(L, 0, "xE2")); TF_CHECK(glyphs_are(L, 1, "x82"));
  TF_EQ_U(L.bytes(), 2);
  TF_EQ_I(L.build((const u8*)"ab\xE2\x82", 4, utf8(), opts(4, 0), 0, 0), E_OK);   // segment: stops before it
  TF_EQ_U(L.count(), 2); TF_EQ_U(L.bytes(), 2); TF_EQ_U(L.end_cell(), 2);
  TF_EQ_U(L.char_at_offset(2), 2); TF_EQ_U(L.cell_of_offset(3), 2);
  TF_EQ_I(build8(&L, "\xE2\x82\xAC", opts(4, 0), 0), E_OK);      // a complete euro sign
  TF_EQ_U(L.count(), 1); TF_EQ_U(L.chars()[0].kind, LK_CHAR); TF_EQ_U(L.glyphs()[0], 0x20AC);
  TF_EQ_U(L.char_at_offset(1), 0); TF_EQ_U(L.char_at_offset(2), 0); TF_EQ_U(L.char_at_offset(3), 1);
  // UTF-16 bad units, odd trailing byte, surrogate pair
  {
    static const u8 s1[] = { 'A', 0, 0x00, 0xD8, 'B', 0, 0x3D, 0xD8, 0x00, 0xDE, 0x00, 0xDC, 'C' };
    TF_EQ_I(L.build(s1, sizeof(s1), &u16, opts(4, 0), 0, 1), E_OK);
    TF_EQ_U(L.count(), 6);
    TF_EQ_U(L.chars()[1].kind, LK_BAD); TF_CHECK(glyphs_are(L, 1, "uD800")); TF_EQ_U(L.chars()[1].width, 5);
    TF_EQ_U(L.chars()[2].cell, 6);
    TF_EQ_U(L.chars()[3].kind, LK_NODRAW); TF_EQ_U(L.chars()[3].width, 1); TF_EQ_U(L.chars()[3].off, 6);
    TF_EQ_U(L.glyphs()[L.chars()[3].glyph], 0xFFFD);
    TF_CHECK(glyphs_are(L, 4, "uDC00")); TF_EQ_U(L.chars()[4].off, 10);
    TF_EQ_U(L.chars()[5].kind, LK_BAD); TF_CHECK(glyphs_are(L, 5, "x43")); TF_EQ_U(L.chars()[5].off, 12);
    TF_EQ_U(L.bytes(), sizeof(s1));
    check_invariants(tf, L, 0);
    TF_EQ_I(L.build(s1, 7, &u16, opts(4, 0), 0, 0), E_OK);      // half a pair at a segment end: stops
    TF_EQ_U(L.bytes(), 6);
    TF_EQ_I(L.build(s1, 7, &u16, opts(4, 0), 0, 1), E_OK);      // at eof: lone unit + bad byte
    TF_EQ_U(L.count(), 4); TF_CHECK(glyphs_are(L, 3, "x3D"));
  }
  // no codec: one byte per character
  TF_EQ_I(L.build((const u8*)"a\tb", 3, 0, opts(4, 0), 0, 1), E_OK);
  TF_EQ_U(L.count(), 3); TF_EQ_U(L.end_cell(), 5);
  // NULL data
  TF_EQ_I(L.build(0, 10, utf8(), opts(4, 0), 7, 1), E_OK);
  TF_EQ_U(L.count(), 0); TF_EQ_U(L.end_cell(), 7); TF_EQ_U(L.offset_of_cell(9, 1), 0);
}

static void test_wide_combining(TF* tf)
{
  LineLayout L;
  // U+4E00 U+4E8C wide, 'e' + U+0301, U+0E01 (not drawable), U+1F600 (> FFFF)
  TF_EQ_I(build8(&L, "\xE4\xB8\x80\xE4\xBA\x8C" "e\xCC\x81x\xE0\xB8\x81\xF0\x9F\x98\x80" "z", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.count(), 8);
  TF_EQ_U(L.chars()[0].kind, LK_WIDE); TF_EQ_U(L.chars()[0].width, 2); TF_EQ_U(L.chars()[0].cell, 0);
  TF_EQ_U(L.chars()[1].kind, LK_WIDE); TF_EQ_U(L.chars()[1].cell, 2); TF_EQ_U(L.chars()[1].off, 3);
  TF_EQ_U(L.chars()[2].cell, 4);                                   // e
  TF_EQ_U(L.chars()[3].kind, LK_ZERO); TF_EQ_U(L.chars()[3].cell, 4); TF_EQ_U(L.chars()[3].width, 0);
  TF_EQ_U(L.glyphs()[L.chars()[3].glyph], 0x301);
  TF_EQ_U(L.chars()[4].cell, 5);                                   // x
  TF_EQ_U(L.chars()[5].kind, LK_NODRAW); TF_EQ_U(L.chars()[5].width, 1); TF_EQ_U(L.glyphs()[L.chars()[5].glyph], 0xE01);
  TF_EQ_U(L.chars()[6].kind, LK_NODRAW); TF_EQ_U(L.chars()[6].width, 1); TF_EQ_U(L.glyphs()[L.chars()[6].glyph], 0xFFFD);
  TF_EQ_U(L.chars()[7].cell, 8); TF_EQ_U(L.end_cell(), 9);
  check_invariants(tf, L, 0);
  // caret positions: before the combining mark = after its base
  TF_EQ_U(L.cell_of_offset(L.chars()[3].off), 5);
  TF_EQ_U(L.cell_of_offset(1), 0);                                 // inside the first wide char
  TF_EQ_U(L.cell_of_offset(3), 2);
  // hit tests on the wide chars
  TF_EQ_U(L.offset_of_cell(0, 1), 0);
  TF_EQ_U(L.offset_of_cell(1, 0), 0);                              // floor: inside -> start
  TF_EQ_U(L.offset_of_cell(1, 1), 3);                              // nearest: middle of a 2-cell char -> next
  TF_EQ_U(L.offset_of_cell(5, 1), L.chars()[4].off);              // after e+mark -> x, never between e and mark
  TF_EQ_U(L.offset_of_cell(5, 0), L.chars()[4].off);
  TF_EQ_U(L.offset_of_cell(4, 1), L.chars()[2].off);
  TF_EQ_U(L.offset_of_cell(9, 1), L.bytes());
  TF_EQ_U(L.offset_of_cell(1000, 0), L.bytes());
  // several marks on one base, and a mark at the start of a layout
  TF_EQ_I(build8(&L, "a\xCC\x81\xCC\x82\xCC\x83" "b", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.count(), 5); TF_EQ_U(L.chars()[3].kind, LK_ZERO); TF_EQ_U(L.chars()[3].cell, 0);
  TF_EQ_U(L.chars()[4].cell, 1); TF_EQ_U(L.cell_of_offset(L.chars()[3].off), 1);
  TF_EQ_I(build8(&L, "\xCC\x81" "b", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.chars()[0].kind, LK_CHAR); TF_EQ_U(L.chars()[0].width, 1); TF_EQ_U(L.chars()[1].cell, 1);
  // mark after a wide char and after a tab
  TF_EQ_I(build8(&L, "\xE4\xB8\x80\xCC\x81\t\xCC\x81x", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.chars()[1].cell, 0); TF_EQ_U(L.cell_of_offset(L.chars()[1].off), 2);
  TF_EQ_U(L.chars()[2].cell, 2); TF_EQ_U(L.chars()[2].width, 2);
  TF_EQ_U(L.chars()[3].cell, 2); TF_EQ_U(L.chars()[4].cell, 4);
}

// random line of mixed content (UTF-8)
static void random_line(TF* tf, EBytes* out, u32 nchars, int with_bad)
{
  static const ecp pool[] = { 'a', 'b', 'Z', '0', ' ', ' ', '\t', '.', ',', '-', '(', 0x01, 0x7F, 0x0D,
                              0xE9, 0x20AC, 0x4E00, 0xAC00, 0xFF21, 0x0301, 0x0302, 0x200B, 0x0E01, 0x1F600,
                              0x85, 0x2014 };
  Vec<ecp> s;
  u32 i;
  for (i = 0; i < nchars; i++) {
    ecp c;
    if (with_bad && tf_rand_below(tf, 20) == 0) c = ECP_BADBYTE(0x80 + tf_rand_below(tf, 0x80));
    else c = pool[tf_rand_below(tf, sizeof(pool) / sizeof(pool[0]))];
    s.push(c);
  }
  enc8(s.data(), s.size(), out);
}

static int is_stop(const LineLayout& L, u32 off)
{
  u32 i;
  if (off == L.bytes()) return 1;
  for (i = 0; i < L.count(); i++) if (L.chars()[i].off == off) return L.chars()[i].kind != LK_ZERO;
  return 0;
}

static void test_hit_inverse(TF* tf)
{
  u32 round;
  LineLayout L;
  for (round = 0; round < 300; round++) {
    EBytes line;
    u32 i, c, bad = 0, start = tf_rand_below(tf, 3) * 7;
    LayoutOpts o = opts((u8)(1 + tf_rand_below(tf, 8)), (u8)tf_rand_below(tf, 2));
    random_line(tf, &line, tf_rand_below(tf, 80), 1);
    TF_EQ_I(L.build(line.data(), line.size(), utf8(), o, start, 1), E_OK);
    TF_EQ_U(L.bytes(), line.size());
    check_invariants(tf, L, start);
    for (i = 0; i < L.count(); i++) {
      const LayChar& ch = L.chars()[i];
      u32 b;
      if (ch.kind == LK_ZERO) continue;
      if (L.offset_of_cell(ch.cell, 0) != ch.off) bad++;
      if (L.offset_of_cell(ch.cell, 1) != ch.off) bad++;
      if (L.cell_of_offset(ch.off) != ch.cell) bad++;
      for (b = ch.off; b < (i + 1 < L.count() ? L.chars()[i + 1].off : L.bytes()); b++)
        if (L.char_at_offset(b) != i) bad++;
    }
    if (L.cell_of_offset(L.bytes()) != L.end_cell()) bad++;
    if (L.char_at_offset(L.bytes()) != L.count()) bad++;
    for (c = 0; c <= L.end_cell() + 2; c++) {
      u32 f = L.offset_of_cell(c, 0), n = L.offset_of_cell(c, 1), fc = L.cell_of_offset(f), nc = L.cell_of_offset(n);
      if (!is_stop(L, f) || !is_stop(L, n)) bad++;
      if (c >= start && c <= L.end_cell() && fc > c) bad++;          // floor never right of the cell
      if (nc < fc) bad++;
      if (nc < c && nc != fc) bad++;                                 // nearest left of the cell = the floor
    }
    if (bad) { tf_fail(tf, __FILE__, __LINE__, "hit test round %u: %u problems", round, bad); break; }
    tf_check(tf, 1, __FILE__, __LINE__, "hit");
  }
}

static u32 row_end_cell(const LineLayout& L, u32 a, u32 b)   // end cell of chars [a, b)
{
  u32 i, e = L.chars()[a].cell;
  for (i = a; i < b; i++) if (L.chars()[i].kind != LK_ZERO) e = L.chars()[i].cell + L.chars()[i].width;
  return e;
}

static void check_rows(TF* tf, const LineLayout& L, const Vec<u32>& rows, u32 w, int word)
{
  u32 r, bad = 0;
  if (rows.size() == 0 || rows[0] != 0) bad++;
  for (r = 0; r < rows.size(); r++) {
    u32 a = rows[r], b = r + 1 < rows.size() ? rows[r + 1] : L.count(), i, fit_end;
    if (r > 0 && a <= rows[r - 1]) bad++;
    if (a < L.count() && L.chars()[a].kind == LK_ZERO) bad++;     // never split a combining sequence
    if (a >= b && L.count() > 0) bad++;
    if (b <= a) continue;
    // the row fits, or it is one character (+ marks), or (word mode) only trailing blanks overflow
    if (row_end_cell(L, a, b) - L.chars()[a].cell <= w) continue;
    fit_end = a + 1;
    while (fit_end < b && L.chars()[fit_end].kind == LK_ZERO) fit_end++;
    if (fit_end == b) continue;
    if (word) {
      i = b;
      while (i > a && (L.chars()[i - 1].kind == LK_SPACE || L.chars()[i - 1].kind == LK_TAB ||
                       L.chars()[i - 1].kind == LK_ZERO)) i--;
      if (i > a && row_end_cell(L, a, i) - L.chars()[a].cell <= w) continue;
      if (i <= fit_end) continue;                                   // one character (+ marks) + hanging blanks
    }
    bad++;
    {
      u32 j;
      EStr d;
      for (j = a; j < b; j++) d.appendf(" %u:k%u c%u w%u", j, L.chars()[j].kind, L.chars()[j].cell, L.chars()[j].width);
      tf_log(tf, "row %u [%u,%u):%s", r, a, b, d.c_str());
    }
  }
  if (bad) tf_fail(tf, __FILE__, __LINE__, "wrap rows: %u problems (w %u word %d)", bad, w, word);
  else tf_check(tf, 1, __FILE__, __LINE__, "rows");
}

static void test_wrap(TF* tf)
{
  LineLayout L;
  Vec<u32> rows;
  u32 round;
  TF_EQ_I(build8(&L, "hello world foo", opts(4, 0), 0), E_OK);
  TF_EQ_I(L.wrap_rows(8, 1, &rows), E_OK);
  TF_EQ_U(rows.size(), 3); TF_EQ_U(rows[0], 0); TF_EQ_U(rows[1], 6); TF_EQ_U(rows[2], 12);
  TF_EQ_I(L.wrap_rows(4, 0, &rows), E_OK);
  TF_EQ_U(rows.size(), 4); TF_EQ_U(rows[1], 4); TF_EQ_U(rows[2], 8); TF_EQ_U(rows[3], 12);
  TF_EQ_I(L.wrap_rows(5, 1, &rows), E_OK);             // the space after "hello" hangs past the edge
  TF_EQ_U(rows.size(), 3); TF_EQ_U(rows[1], 6); TF_EQ_U(rows[2], 12);
  TF_EQ_I(L.wrap_rows(100, 1, &rows), E_OK);
  TF_EQ_U(rows.size(), 1);
  TF_EQ_I(L.wrap_rows(0, 1, &rows), E_OK);               // width 0 = 1
  TF_EQ_U(rows.size(), 13);                              // spaces hang: "h" "e" "l" "l" "o " ...
  TF_EQ_I(build8(&L, "abcdefghij", opts(4, 0), 0), E_OK);   // no break opportunity: hard breaks
  TF_EQ_I(L.wrap_rows(4, 1, &rows), E_OK);
  TF_EQ_U(rows.size(), 3); TF_EQ_U(rows[1], 4); TF_EQ_U(rows[2], 8);
  TF_EQ_I(build8(&L, "path/to/some-file,x", opts(4, 0), 0), E_OK);   // punctuation breaks
  TF_EQ_I(L.wrap_rows(10, 1, &rows), E_OK);
  TF_EQ_U(rows[1], 8);                                   // "path/to/" | "some-file," | "x"
  TF_EQ_U(rows[2], 18);
  TF_EQ_I(build8(&L, "f(xxxxxxxx", opts(4, 0), 0), E_OK);   // no break after '('
  TF_EQ_I(L.wrap_rows(4, 1, &rows), E_OK);
  TF_EQ_U(rows[1], 4);
  // wide characters are never split
  TF_EQ_I(build8(&L, "\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80", opts(4, 0), 0), E_OK);
  TF_EQ_I(L.wrap_rows(3, 0, &rows), E_OK);
  TF_EQ_U(rows.size(), 3); TF_EQ_U(rows[1], 1); TF_EQ_U(rows[2], 2);
  TF_EQ_I(L.wrap_rows(1, 1, &rows), E_OK);               // wider than the row: one per row
  TF_EQ_U(rows.size(), 3);
  TF_EQ_I(build8(&L, "a\xE4\xB8\x80" "b", opts(4, 0), 0), E_OK);
  TF_EQ_I(L.wrap_rows(2, 0, &rows), E_OK);
  TF_EQ_U(rows.size(), 3); TF_EQ_U(rows[1], 1); TF_EQ_U(rows[2], 2);
  // combining marks stay with their base
  TF_EQ_I(build8(&L, "ab\xCC\x81" "cd", opts(4, 0), 0), E_OK);
  TF_EQ_I(L.wrap_rows(2, 0, &rows), E_OK);
  TF_EQ_U(rows.size(), 2); TF_EQ_U(rows[1], 3);
  TF_EQ_I(L.wrap_rows(1, 0, &rows), E_OK);
  TF_EQ_U(rows.size(), 4); TF_EQ_U(rows[1], 1); TF_EQ_U(rows[2], 3); TF_EQ_U(rows[3], 4);
  // empty layout
  TF_EQ_I(build8(&L, "", opts(4, 0), 0), E_OK);
  TF_EQ_I(L.wrap_rows(10, 1, &rows), E_OK);
  TF_EQ_U(rows.size(), 1); TF_EQ_U(rows[0], 0);
  TF_EQ_I(L.wrap_rows(10, 1, 0), E_RANGE);
  // random lines
  for (round = 0; round < 300; round++) {
    EBytes line;
    u32 w = 1 + tf_rand_below(tf, 30), start = tf_rand_below(tf, 2) * 9;
    int word = (int)tf_rand_below(tf, 2);
    random_line(tf, &line, tf_rand_below(tf, 200), 1);
    TF_EQ_I(L.build(line.data(), line.size(), utf8(), opts((u8)(1 + tf_rand_below(tf, 8)), 0), start, 1), E_OK);
    TF_EQ_I(L.wrap_rows(w, word, &rows), E_OK);
    check_rows(tf, L, rows, w, word);
  }
}

static void test_glyph_cap(TF* tf)
{
  LineLayout L;
  EBytes b;
  u32 i;
  for (i = 0; i < 30000; i++) { u8 t = '\t'; b.append(&t, 1); }
  TF_EQ_I(L.build(b.data(), b.size(), utf8(), opts(8, 0), 0, 1), E_OK);
  TF_EQ_U(L.count(), 65535 / 8);
  TF_EQ_U(L.bytes(), 65535 / 8);
  TF_CHECK(L.glyph_count() <= 65535);
  TF_EQ_U(L.char_at_offset(29000), L.count());
  b.clear();
  for (i = 0; i < 70000; i++) { u8 t = 'x'; b.append(&t, 1); }
  TF_EQ_I(L.build(b.data(), b.size(), utf8(), opts(8, 0), 0, 1), E_OK);
  TF_EQ_U(L.bytes(), 65535);
  check_invariants(tf, L, 0);
}

static void test_oom(TF* tf)
{
  LineLayout L;
  EBytes line;
  i32 n;
  int ok = 0;
  random_line(tf, &line, 300, 1);
  for (n = 1; n < 50 && !ok; n++) {
    LineLayout fresh;
    int rc;
    e_test_fail_after(n);
    rc = fresh.build(line.data(), line.size(), utf8(), opts(4, 0), 0, 1);
    e_test_fail_after(-1);
    if (rc == E_OK) ok = 1;
    else { TF_EQ_I(rc, E_NOMEM); TF_EQ_U(fresh.count(), 0); TF_EQ_U(fresh.bytes(), 0); }
  }
  TF_CHECK(ok);
}

// ---- LongLineIndex ----

class LayChunkSource : public ByteSource {   // serves odd-sized chunks so characters straddle chunk ends
public:
  LayChunkSource(const u8* p, epos n) : p_(p), n_(n), fail_at_(EPOS_NONE) {}
  epos size() const { return n_; }
  const u8* get(epos off, epos* avail)
  {
    epos k;
    if (off >= n_ || off >= fail_at_) { *avail = 0; return 0; }
    k = 1 + ((off * 2654435761u) >> 27) % 13;
    if (k > n_ - off) k = n_ - off;
    *avail = k;
    return p_ + off;
  }
  int err() const { return E_OK; }
  const u8* p_; epos n_; epos fail_at_;
};

class LayCountCb : public ProgressCb {
public:
  LayCountCb() : calls(0), cancel_at(0), last(0) {}
  int progress(u32 done, u32 total) { (void)total; calls++; last = done; return cancel_at && calls >= cancel_at; }
  u32 calls, cancel_at, last;
};

// Checks an index against layouts built from its checkpoints and (for small lines) a full layout.
static void check_index(TF* tf, const LongLineIndex& ix, const u8* p, epos start, epos end, const Codec* c,
                        const LayoutOpts& o, int full)
{
  LineLayout L, F;
  epos pos = start;
  u32 cell = 0, bad = 0, k = 0;
  if (full) {
    TF_EQ_I(F.build(p + start, end - start, c, o, 0, 1), E_OK);
    TF_EQ_U(F.bytes(), end - start);
    TF_EQ_U(F.end_cell(), ix.total_cells());
  }
  for (;;) {
    epos cp, next;
    u32 cc, ncell;
    if (ix.at_offset(pos, &cp, &cc) != E_OK) { bad++; break; }
    if (cp != pos || cc != cell) { bad++; break; }
    // next checkpoint: the one at_offset reports for end, walking forward
    next = end;
    ncell = ix.total_cells();
    {
      epos probe = pos + LAY_CHECKPOINT, q;
      u32 qc;
      while (probe < end) {
        if (ix.at_offset(probe, &q, &qc) == E_OK && q > pos) { next = q; ncell = qc; break; }
        probe += 64;
      }
    }
    if (next != end && next - pos < LAY_CHECKPOINT) bad++;
    if (L.build(p + pos, next - pos, c, o, cell, 1) != E_OK) { bad++; break; }
    if (L.bytes() != next - pos || L.end_cell() != ncell) bad++;
    if (L.count() && L.chars()[0].kind == LK_ZERO) bad++;
    if (full) {
      u32 fi = F.char_at_offset(pos - start);
      if (pos < end && (fi >= F.count() || F.chars()[fi].off != pos - start || F.chars()[fi].kind == LK_ZERO)) bad++;
      if (F.cell_of_offset(pos - start) != cell) bad++;
    }
    k++;
    if (next >= end) break;
    pos = next;
    cell = ncell;
  }
  if (bad) tf_fail(tf, __FILE__, __LINE__, "index check: %u problems over %u checkpoints", bad, k);
  else tf_check(tf, 1, __FILE__, __LINE__, "index");
}

static void test_index(TF* tf)
{
  EBytes line;
  LongLineIndex ix, ix2;
  LayoutOpts o = opts(4, 0);
  LayU16Codec u16;
  u32 i;
  // empty index
  TF_EQ_I(ix.at_offset(0, 0, 0), E_STATE);
  TF_EQ_I(ix.at_cell(0, 0, 0), E_STATE);
  TF_EQ_U(ix.total_cells(), 0);
  // small line: compare with a full layout (ASCII-heavy content stays under the glyph limit)
  random_line(tf, &line, 20000, 1);
  {
    MemSource ms;
    ms.copy_from(line.data(), line.size());
    TF_EQ_I(ix.build(&ms, utf8(), o, 0, line.size(), 0), E_OK);
    TF_EQ_U(ix.start(), 0); TF_EQ_U(ix.end(), line.size());
    if (line.size() < 60000) check_index(tf, ix, line.data(), 0, line.size(), utf8(), o, 1);
  }
  // 1 MB line, with a prefix (start > 0), through odd chunks and through memory: same answers
  {
    EBytes big;
    u8 pre[5] = { 'x', 'y', '\t', 0xCC, 0x81 };
    MemSource ms;
    epos start = 5;
    big.append(pre, 5);
    while (big.size() < 1048576 + 5) {
      random_line(tf, &line, 4000, 1);
      big.append(line.data(), line.size());
    }
    ms.copy_from(big.data(), big.size());
    {
      LayChunkSource cs(big.data(), big.size());
      LayCountCb cb;
      TF_EQ_I(ix.build(&ms, utf8(), o, start, big.size(), 0), E_OK);
      TF_EQ_I(ix2.build(&cs, utf8(), o, start, big.size(), &cb), E_OK);
      TF_EQ_U(ix.total_cells(), ix2.total_cells());
      TF_CHECK(cb.calls >= 4);
      TF_EQ_U(cb.last, big.size() - start);
    }
    check_index(tf, ix, big.data(), start, big.size(), utf8(), o, 0);
    // random queries agree between the two indexes and bracket the position
    for (i = 0; i < 2000; i++) {
      epos pos = start + tf_rand_below(tf, big.size() - start + 1), a, b;
      u32 ca, cbv, cell = tf_rand_below(tf, ix.total_cells() + 10);
      TF_EQ_I(ix.at_offset(pos, &a, &ca), E_OK);
      TF_EQ_I(ix2.at_offset(pos, &b, &cbv), E_OK);
      TF_CHECK(a == b && ca == cbv && a <= pos && pos - a < 2 * LAY_CHECKPOINT + 64);
      TF_EQ_I(ix.at_cell(cell, &a, &ca), E_OK);
      TF_CHECK(ca <= cell);
      TF_EQ_I(ix2.at_cell(cell, &b, &cbv), E_OK);
      TF_CHECK(a == b && ca == cbv);
    }
    TF_EQ_I(ix.at_offset(start - 1, 0, 0), E_RANGE);
    TF_EQ_I(ix.at_offset(big.size() + 1, 0, 0), E_RANGE);
    TF_EQ_I(ix.at_offset(big.size(), 0, 0), E_OK);
    // cancel and I/O errors clear the index
    {
      LayCountCb cb;
      LayChunkSource cs(big.data(), big.size());
      cb.cancel_at = 2;
      TF_EQ_I(ix2.build(&ms, utf8(), o, start, big.size(), &cb), E_CANCEL);
      TF_EQ_U(ix2.total_cells(), 0); TF_EQ_I(ix2.at_offset(start, 0, 0), E_STATE);
      cs.fail_at_ = 500000;
      TF_EQ_I(ix2.build(&cs, utf8(), o, start, big.size(), 0), E_IO);
      TF_EQ_I(ix2.at_offset(start, 0, 0), E_STATE);
    }
    TF_EQ_I(ix2.build(&ms, utf8(), o, 10, 5, 0), E_RANGE);
    TF_EQ_I(ix2.build(&ms, utf8(), o, 0, big.size() + 1, 0), E_RANGE);
    TF_EQ_I(ix2.build(0, utf8(), o, 0, 0, 0), E_RANGE);
    TF_EQ_I(ix2.build(&ms, utf8(), o, 7, 7, 0), E_OK);       // empty range
    TF_EQ_U(ix2.total_cells(), 0); TF_EQ_I(ix2.at_offset(7, 0, 0), E_OK);
  }
  // UTF-16 through odd chunks (units and surrogate pairs straddle)
  {
    EBytes w;
    MemSource ms;
    u32 k;
    for (k = 0; k < 70000; k++) {
      u32 r = tf_rand_below(tf, 10);
      u8 u[4];
      if (r < 6) { u[0] = (u8)('a' + r); u[1] = 0; w.append(u, 2); }
      else if (r == 6) { u[0] = 0x00; u[1] = 0x4E; w.append(u, 2); }
      else if (r == 7) { u[0] = 0x3D; u[1] = 0xD8; u[2] = 0x00; u[3] = 0xDE; w.append(u, 4); }
      else if (r == 8) { u[0] = 0x01; u[1] = 0x03; w.append(u, 2); }
      else { u[0] = 0x00; u[1] = 0xDC; w.append(u, 2); }
    }
    ms.copy_from(w.data(), w.size());
    {
      LayChunkSource cs(w.data(), w.size());
      TF_EQ_I(ix.build(&ms, &u16, o, 0, w.size(), 0), E_OK);
      TF_EQ_I(ix2.build(&cs, &u16, o, 0, w.size(), 0), E_OK);
      TF_EQ_U(ix.total_cells(), ix2.total_cells());
    }
    check_index(tf, ix, w.data(), 0, w.size(), &u16, o, 0);
  }
  ix.clear();
  TF_EQ_U(ix.total_cells(), 0); TF_EQ_I(ix.at_cell(0, 0, 0), E_STATE);
}

static void test_real_ucd(TF* tf)
{
  LineLayout L;
  if (uc_width(0x4E00) != 2 || uc_width(0x0301) != 0) {
    tf_log(tf, "layout: real ucd not available yet (T02), real-table checks skipped");
    return;
  }
  TF_EQ_I(build8(&L, "\xE4\xB8\x80" "e\xCC\x81x", opts(4, 0), 0), E_OK);
  TF_EQ_U(L.chars()[0].kind, LK_WIDE); TF_EQ_U(L.chars()[2].kind, LK_ZERO); TF_EQ_U(L.end_cell(), 4);
}

void suite_layout(TF* tf)
{
  lay_set_props(fake_width, fake_draw);
  test_names(tf);
  test_tabs(tf);
  test_controls(tf);
  test_bad(tf);
  test_wide_combining(tf);
  test_hit_inverse(tf);
  test_wrap(tf);
  test_glyph_cap(tf);
  test_oom(tf);
  test_index(tf);
  lay_set_props(0, 0);
  test_real_ucd(tf);
}
