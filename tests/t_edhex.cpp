// t_edhex.cpp - suite "edhex": hex view edits (DESIGN 6.18, 9.2, 7.3). Owner: T10.
#include "tf.h"
#include "edops.h"
#include "doc.h"
#include "codec.h"
#include "esource.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// helpers

static int hxt_hexval(char ch)
{
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

// "41 42 4A" -> bytes (blanks ignored)
static void hxt_parse(const char* s, EBytes* out)
{
  int hi = -1;
  out->clear();
  while (*s) {
    int v = hxt_hexval(*s++);
    if (v < 0) continue;
    if (hi < 0) hi = v;
    else { u8 b = (u8)(hi * 16 + v); out->push(b); hi = -1; }
  }
}

static void hxt_doc_bytes(Document* d, EBytes* out)
{
  out->resize(d->length());
  if (d->length()) d->read(0, out->data(), d->length());
}

static void hxt_fmt(const EBytes& b, char* out, u32 cap)
{
  u32 i, n = 0;
  static const char* k = "0123456789ABCDEF";
  out[0] = 0;
  for (i = 0; i < b.size() && n + 4 < cap; i++) {
    if (i) out[n++] = ' ';
    out[n++] = k[b[i] >> 4];
    out[n++] = k[b[i] & 15];
  }
  out[n] = 0;
}

static EncSpec hxt_enc(char e)
{
  switch (e) {
  case 'u': return enc_utf8(0);
  case 'U': return enc_utf8(1);
  case 'l': return enc_utf16le(1);
  case 'L': return enc_utf16le(0);
  case 'b': return enc_utf16be(1);
  case 'o': return enc_oem();
  default: return enc_ansi();
  }
}

static epos hxt_bom(char e, const EBytes& b)
{
  if (e == 'U' && b.size() >= 3) return 3;
  if ((e == 'l' || e == 'b') && b.size() >= 2) return 2;
  return 0;
}

static int hxt_open(Document* d, char e, const EBytes& init)
{
  MemSource* src = new MemSource;
  if (!src) return E_NOMEM;
  if (init.size()) src->copy_from(init.data(), init.size());
  return d->init_source(src, hxt_enc(e), hxt_bom(e, init), EOL_CRLF, 0);
}

static u32 hxt_parse_num(const char** p)
{
  u32 v = 0;
  while (**p >= '0' && **p <= '9') { v = v * 10 + (u32)(**p - '0'); (*p)++; }
  return v;
}

// Runs a script on d. Tokens (space separated):
//   pN caret N (nibble 0)   PN caret N nibble 1   sA,B selection anchor A caret B   i insert mode   o overwrite mode
//   nH type nibble H        cHHHH type code point (hex)   wHHHH.. write bytes   D Delete   B Backspace   R read-only
// Returns the rc of the last edit op; *now advances 100 ms per op.
static int hxt_run(Document* d, HexCaret* c, Selection* sel, const char* script, u32* now)
{
  const char* p = script;
  int rc = E_OK;
  while (*p) {
    char op;
    while (*p == ' ') p++;
    if (!*p) break;
    op = *p++;
    *now += 100;
    switch (op) {
    case 'p': case 'P':
      c->pos = hxt_parse_num(&p); c->nibble = (u8)(op == 'P'); c->unit_end = 0;
      sel->anchor = sel->caret = c->pos;
      break;
    case 's':
      sel->anchor = hxt_parse_num(&p);
      if (*p == ',') p++;
      sel->caret = hxt_parse_num(&p);
      c->pos = sel->caret; c->nibble = 0; c->unit_end = 0;
      break;
    case 'i': c->insert = 1; break;
    case 'o': c->insert = 0; break;
    case 'R': d->set_read_only(true); break;
    case 'n': rc = hx_type_nibble(d, c, sel, (u8)hxt_hexval(*p++), *now); break;
    case 'c': {
      ecp ch = 0;
      while (hxt_hexval(*p) >= 0) ch = ch * 16 + (ecp)hxt_hexval(*p++);
      rc = hx_type_char(d, c, sel, ch, *now);
      break;
    }
    case 'w': {
      EBytes b;
      char tmp[64];
      u32 n = 0;
      while (hxt_hexval(*p) >= 0 && n + 1 < sizeof(tmp)) tmp[n++] = *p++;
      tmp[n] = 0;
      hxt_parse(tmp, &b);
      rc = hx_write(d, c, sel, b.data(), b.size());
      break;
    }
    case 'D': rc = hx_delete(d, c, sel, 0); break;
    case 'B': rc = hx_delete(d, c, sel, 1); break;
    default: return E_SYNTAX;
    }
    while (*p && *p != ' ') p++;
  }
  return rc;
}

// ---------------------------------------------------------------------------------------------------------------
// golden table

struct HxCase {
  char enc;              // a ANSI, u UTF-8, U UTF-8 BOM, l UTF-16 LE BOM, L UTF-16 LE no BOM, b UTF-16 BE BOM
  const char* init;
  const char* script;
  const char* expect;    // bytes after the script
  epos caret; u8 nibble;
  int rc;                // rc of the last op
  int undos;             // undo steps back to the initial bytes (-1: don't check the count)
};

static const HxCase k_cases[] = {
  // ---- SBCS: overwrite nibbles
  { 'a', "41 42 43", "p0 n7 n0", "70 42 43", 1, 0, E_OK, 1 },
  { 'a', "41 42 43", "P1 n9", "41 49 43", 2, 0, E_OK, 1 },
  { 'a', "41 42 43", "p0 n1 n2 n3 n4 n5", "12 34 53", 2, 1, E_OK, 1 },
  { 'a', "41", "p1 nA nB", "41 AB", 2, 0, E_OK, 1 },
  { 'a', "", "p0 nF nF nE", "FF E0", 1, 1, E_OK, 1 },
  { 'a', "41 42", "p2 nA nB nC", "41 42 AB C0", 3, 1, E_OK, 1 },
  // ---- SBCS: insert nibbles
  { 'a', "41 42", "i p1 n1 n2 n3", "41 12 30 42", 2, 1, E_OK, 1 },
  { 'a', "41 42", "i p0 nD nE nA nD", "DE AD 41 42", 2, 0, E_OK, 1 },
  { 'a', "41 42", "i p2 n7", "41 42 70", 2, 1, E_OK, 1 },
  { 'a', "41 42", "i P0 n9", "49 42", 1, 0, E_OK, 1 },        // nibble 1 rewrites the low half in insert mode too
  // ---- selection replaced first (both modes)
  { 'a', "00 11 22 33", "s1,3 nF nE", "00 FE 33", 2, 0, E_OK, 1 },
  { 'a', "00 11 22 33", "i s3,1 nF nE nD", "00 FE D0 33", 2, 1, E_OK, 1 },
  { 'a', "00 11 22 33", "s0,4 n5", "50", 0, 1, E_OK, 1 },
  // ---- Delete
  { 'a', "00 11 22", "p1 D", "00 22", 1, 0, E_OK, 1 },
  { 'a', "00 11 22", "p1 D D", "00", 1, 0, E_OK, 1 },
  { 'a', "00 11 22", "p1 D D D", "00", 1, 0, E_RANGE, 1 },
  { 'a', "00 11 22", "i p0 D", "11 22", 0, 0, E_OK, 1 },
  { 'a', "00 11 22 33", "s3,1 D", "00 33", 1, 0, E_OK, 1 },
  { 'a', "00 11 22 33", "i s0,4 D", "", 0, 0, E_OK, 1 },
  { 'a', "00 11 22", "p3 D", "00 11 22", 3, 0, E_RANGE, 0 },
  // ---- Backspace: overwrite only moves, insert deletes the byte before the caret
  { 'a', "00 11 22", "p2 B", "00 11 22", 1, 0, E_OK, 0 },
  { 'a', "00 11 22", "P2 B", "00 11 22", 2, 0, E_OK, 0 },
  { 'a', "00 11 22", "p0 B", "00 11 22", 0, 0, E_RANGE, 0 },
  { 'a', "00 11 22", "i p2 B", "00 22", 1, 0, E_OK, 1 },
  { 'a', "00 11 22", "i p2 B B", "22", 0, 0, E_OK, 1 },
  { 'a', "00 11 22", "i p2 B B B", "22", 0, 0, E_RANGE, 1 },
  { 'a', "00 11 22", "i p3 B", "00 11", 2, 0, E_OK, 1 },
  { 'a', "00 11", "i p1 n5 B", "00 11", 1, 0, E_OK, 2 },       // removes the half-typed byte
  { 'a', "00 11 22 33", "s1,3 B", "00 33", 1, 0, E_OK, 1 },  // a selection is deleted in both modes
  // ---- characters
  { 'a', "41 42 43", "p1 c58", "41 58 43", 2, 0, E_OK, 1 },
  { 'a', "41 42 43", "p1 c58 c59 c5A", "41 58 59 5A", 4, 0, E_OK, 1 },
  { 'a', "41 42", "i p1 c58", "41 58 42", 2, 0, E_OK, 1 },
  { 'a', "41 42", "p1 c20AC", "41 80", 2, 0, E_OK, 1 },      // cp1252 euro
  { 'a', "41 42", "p1 c4E2D", "41 42", 1, 0, E_RANGE, 0 },   // unrepresentable
  { 'a', "41 42 43", "s0,2 c5A", "5A 43", 1, 0, E_OK, 1 },
  { 'o', "41 42", "p0 c2591", "B0 42", 1, 0, E_OK, 1 },       // OEM 437 light shade
  // ---- paste (hx_write)
  { 'a', "01 02 03", "p2 wAABBCC", "01 02 AA BB CC", 5, 0, E_OK, 1 },
  { 'a', "01 02 03", "i p1 wAABB", "01 AA BB 02 03", 3, 0, E_OK, 1 },
  { 'a', "01 02 03", "s0,3 w00", "00", 1, 0, E_OK, 1 },
  { 'a', "01 02 03", "p3 w0000", "01 02 03 00 00", 5, 0, E_OK, 1 },
  // ---- read-only
  { 'a', "41 42", "R p0 n1", "41 42", 0, 0, E_READONLY, 0 },
  { 'a', "41 42", "R p0 D", "41 42", 0, 0, E_READONLY, 0 },
  { 'a', "41 42", "R p1 B", "41 42", 0, 0, E_OK, 0 },          // moving is fine
  { 'a', "41 42", "R s0,1 w99", "41 42", 1, 0, E_READONLY, 0 },
  // ---- UTF-8 with BOM: [0, 3) is protected
  { 'U', "EF BB BF 41", "p1 n0", "EF BB BF 41", 1, 0, E_ACCESS, 0 },
  { 'U', "EF BB BF 41", "p0 D", "EF BB BF 41", 0, 0, E_ACCESS, 0 },
  { 'U', "EF BB BF 41", "i p3 B", "EF BB BF 41", 3, 0, E_ACCESS, 0 },
  { 'U', "EF BB BF 41", "i p2 c41", "EF BB BF 41", 2, 0, E_ACCESS, 0 },
  { 'U', "EF BB BF 41", "s0,4 D", "EF BB BF", 3, 0, E_OK, 1 },   // select all + Del keeps the BOM
  { 'U', "EF BB BF 41", "s0,2 D", "EF BB BF 41", 2, 0, E_ACCESS, 0 },
  { 'U', "EF BB BF 41", "s0,4 n1", "EF BB BF 41", 4, 0, E_ACCESS, 0 },
  { 'U', "EF BB BF 41", "i p3 n1 n2", "EF BB BF 12 41", 4, 0, E_OK, 1 },
  { 'U', "EF BB BF 41 42 43", "p3 cE9", "EF BB BF C3 A9 43", 5, 0, E_OK, 1 },   // overwrites 2 bytes
  { 'U', "EF BB BF 41 42", "p4 c20AC", "EF BB BF 41 E2 82 AC", 7, 0, E_OK, 1 }, // extends at the end
  { 'u', "41 42", "i p1 c4E2D", "41 E4 B8 AD 42", 4, 0, E_OK, 1 },
  // ---- UTF-16 LE with BOM: units
  { 'l', "FF FE 41 00 42 00", "p2 n7 n0", "FF FE 70 00 42 00", 3, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "p3 n1", "FF FE 41 10 42 00", 3, 1, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "p0 n1", "FF FE 41 00 42 00", 0, 0, E_ACCESS, 0 },
  { 'l', "FF FE 41 00 42 00", "i p4 n1 n2 n3 n4", "FF FE 41 00 12 34 42 00", 6, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "i p4 n1 n2 n3 n4 n5", "FF FE 41 00 12 34 50 00 42 00", 6, 1, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "i p3 n9", "FF FE 90 00 41 00 42 00", 2, 1, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "i p6 nA nB", "FF FE 41 00 42 00 AB 00", 7, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "p6 nA nB nC", "FF FE 41 00 42 00 AB C0", 7, 1, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "p3 D", "FF FE 42 00", 2, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "i p4 B", "FF FE 42 00", 2, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "i p2 B", "FF FE 41 00 42 00", 2, 0, E_ACCESS, 0 },
  { 'l', "FF FE 41 00 42 00", "i p3 B", "FF FE 42 00", 2, 0, E_OK, 1 },  // byte 2 is before the caret: its unit
  { 'l', "FF FE 41 00 42 00", "p2 c5A", "FF FE 5A 00 42 00", 4, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "p3 c5A", "FF FE 5A 00 42 00", 4, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "i p4 c1F600", "FF FE 41 00 3D D8 00 DE 42 00", 8, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "p4 c1F600", "FF FE 41 00 3D D8 00 DE", 8, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00 43 00", "s3,5 D", "FF FE 43 00", 2, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00 43 00", "s5,4 nF", "FF FE 41 00 F0 00 43 00", 4, 1, E_OK, 1 },
  { 'l', "FF FE 41 00 42 00", "s0,6 D", "FF FE", 2, 0, E_OK, 1 },
  { 'l', "FF FE 41 00", "p2 wAA", "FF FE 41 00", 2, 0, E_ALIGN, 0 },
  { 'l', "FF FE 41 00", "i p2 wAABB", "FF FE AA BB 41 00", 4, 0, E_OK, 1 },
  // dangling odd byte: nothing can follow it; it can be deleted
  { 'l', "FF FE 41 00 42", "p4 n1", "FF FE 41 00 42", 4, 0, E_ALIGN, 0 },
  { 'l', "FF FE 41 00 42", "p5 n1", "FF FE 41 00 42", 5, 0, E_ALIGN, 0 },
  { 'l', "FF FE 41 00 42", "i p5 n1", "FF FE 41 00 42", 5, 0, E_ALIGN, 0 },
  { 'l', "FF FE 41 00 42", "p5 c41", "FF FE 41 00 42", 5, 0, E_ALIGN, 0 },
  { 'l', "FF FE 41 00 42", "p4 D", "FF FE 41 00", 4, 0, E_OK, 1 },
  { 'l', "FF FE 41 00 42", "i p5 B", "FF FE 41 00", 4, 0, E_OK, 1 },
  { 'L', "41 00 42 00", "p0 n6", "61 00 42 00", 0, 1, E_OK, 1 },
  // ---- UTF-16 BE
  { 'b', "FE FF 00 41 00 42", "i p2 n4", "FE FF 40 00 00 41 00 42", 2, 1, E_OK, 1 },
  { 'b', "FE FF 00 41 00 42", "p4 c5A", "FE FF 00 41 00 5A", 6, 0, E_OK, 1 },
  { 'b', "FE FF 00 41 00 42", "p5 n3 n1", "FE FF 00 41 00 31", 6, 0, E_OK, 1 },
  { 'b', "FE FF 00 41", "i p4 cE9", "FE FF 00 41 00 E9", 6, 0, E_OK, 1 },
};

static void test_golden(TF* tf)
{
  u32 i;
  for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    const HxCase& k = k_cases[i];
    Document d;
    EBytes init, expect, got;
    HexCaret c;
    Selection sel;
    u32 now = 10000;
    int rc, undos = 0;
    char a[200], b[200];
    hxt_parse(k.init, &init);
    hxt_parse(k.expect, &expect);
    TF_EQ_I(hxt_open(&d, k.enc, init), E_OK);
    memset(&c, 0, sizeof(c));
    sel.anchor = sel.caret = 0;
    rc = hxt_run(&d, &c, &sel, k.script, &now);
    hxt_doc_bytes(&d, &got);
    if (rc != k.rc || got.size() != expect.size() || (got.size() && memcmp(got.data(), expect.data(), got.size()) != 0)
        || c.pos != k.caret || c.nibble != k.nibble || sel.caret != c.pos || (rc == E_OK && sel.anchor != c.pos)) {
      hxt_fmt(got, a, sizeof(a));
      hxt_fmt(expect, b, sizeof(b));
      tf_fail(tf, __FILE__, __LINE__, "case %u '%c' [%s] '%s': rc %s (want %s), bytes [%s] (want [%s]), caret %u.%u "
              "(want %u.%u), sel %u,%u", i, k.enc, k.init, k.script, e_errname(rc), e_errname(k.rc), a, b,
              (u32)c.pos, (u32)c.nibble, (u32)k.caret, (u32)k.nibble, (u32)sel.anchor, (u32)sel.caret);
      continue;
    }
    tf->checks++;
    // undo back to the initial bytes, redo forward to the result
    d.set_read_only(false);
    while (d.can_undo() && undos < 100) {
      Selection s2;
      if (d.undo(&s2) != E_OK) break;
      undos++;
    }
    hxt_doc_bytes(&d, &got);
    TF_CHECK(got.size() == init.size() && (init.size() == 0 || memcmp(got.data(), init.data(), init.size()) == 0));
    if (k.undos >= 0 && undos != k.undos)
      tf_fail(tf, __FILE__, __LINE__, "case %u '%s': %d undo steps, want %d", i, k.script, undos, k.undos);
    while (d.can_redo()) { Selection s2; if (d.redo(&s2) != E_OK) break; }
    hxt_doc_bytes(&d, &got);
    TF_CHECK(got.size() == expect.size() && (expect.size() == 0 || memcmp(got.data(), expect.data(), got.size()) == 0));
  }
}

// bad markers type back to their bytes; the UTF-8 codec can't store a lone surrogate unit marker
static void test_badmarkers(TF* tf)
{
  Document d;
  EBytes init, got;
  HexCaret c;
  Selection sel;
  hxt_parse("41 42", &init);
  TF_EQ_I(hxt_open(&d, 'u', init), E_OK);
  memset(&c, 0, sizeof(c));
  c.pos = 1; sel.anchor = sel.caret = 1;
  TF_EQ_I(hx_type_char(&d, &c, &sel, ECP_BADBYTE(0xFF), 1000), E_OK);
  hxt_doc_bytes(&d, &got);
  TF_EQ_U(got.size(), 2);
  TF_EQ_U(got[1], 0xFF);
  TF_EQ_U(c.pos, 2);
  TF_EQ_I(hx_type_char(&d, &c, &sel, ECP_BADUNIT(0xD800), 1100), E_RANGE);
  TF_EQ_U(d.length(), 2);
}

// typing merges into one undo group; a pause > 2 s or a caret move starts a new one
static void test_merge(TF* tf)
{
  Document d;
  EBytes init;
  HexCaret c;
  Selection sel, s2;
  int n = 0;
  hxt_parse("00 00 00 00 00 00", &init);
  TF_EQ_I(hxt_open(&d, 'a', init), E_OK);
  memset(&c, 0, sizeof(c));
  sel.anchor = sel.caret = 0;
  TF_EQ_I(hx_type_nibble(&d, &c, &sel, 1, 1000), E_OK);
  TF_EQ_I(hx_type_nibble(&d, &c, &sel, 2, 1100), E_OK);
  TF_EQ_I(hx_type_nibble(&d, &c, &sel, 3, 4000), E_OK);   // pause: new group
  TF_EQ_I(hx_type_nibble(&d, &c, &sel, 4, 4100), E_OK);
  c.pos = 4; c.nibble = 0; sel.anchor = sel.caret = 4;   // caret moved: new group
  TF_EQ_I(hx_type_nibble(&d, &c, &sel, 5, 4200), E_OK);
  TF_EQ_I(hx_type_char(&d, &c, &sel, 'A', 4300), E_OK);   // merges with the nibble before
  while (d.can_undo()) { if (d.undo(&s2) != E_OK) break; n++; }
  TF_EQ_I(n, 3);
  TF_CHECK(!d.modified());
  // the undo of the first group returns the selection before it
  TF_EQ_U(s2.caret, 0);
}

// random operations: invariants hold, undo restores the initial bytes exactly
static void test_random(TF* tf)
{
  static const char encs[] = { 'a', 'U', 'l', 'b', 'L', 'u' };
  u32 e, k, rounds = tf->long_run ? 6000 : 1500;
  for (e = 0; e < sizeof(encs); e++) {
    Document d;
    EBytes init, got;
    HexCaret c;
    Selection sel;
    u32 now = 5000, i, bad = 0;
    char enc = encs[e];
    init.clear();
    if (enc == 'U') { init.push(0xEF); init.push(0xBB); init.push(0xBF); }
    if (enc == 'l') { init.push(0xFF); init.push(0xFE); }
    if (enc == 'b') { init.push(0xFE); init.push(0xFF); }
    for (i = 0; i < 40; i++) init.push((u8)tf_rand_below(tf, 256));
    TF_EQ_I(hxt_open(&d, enc, init), E_OK);
    memset(&c, 0, sizeof(c));
    sel.anchor = sel.caret = 0;
    for (k = 0; k < rounds; k++) {
      u32 op = tf_rand_below(tf, 12);
      epos len = d.length();
      int rc = E_OK;
      now += tf_rand_below(tf, 3) ? 50 : 2500;
      switch (op) {
      case 0: c.pos = tf_rand_below(tf, len + 1); c.nibble = (u8)tf_rand_below(tf, 2); c.unit_end = 0;
              if (c.pos == len) c.nibble = 0;
              sel.anchor = sel.caret = c.pos; break;
      case 1: sel.anchor = tf_rand_below(tf, len + 1); sel.caret = tf_rand_below(tf, len + 1);
              c.pos = sel.caret; c.nibble = 0; c.unit_end = 0; break;
      case 2: c.insert = (u8)!c.insert; break;
      case 3: case 4: case 5: rc = hx_type_nibble(&d, &c, &sel, (u8)tf_rand_below(tf, 16), now); break;
      case 6: rc = hx_type_char(&d, &c, &sel, 0x20 + tf_rand_below(tf, 0x5F), now); break;
      case 7: rc = hx_type_char(&d, &c, &sel, tf_rand_below(tf, 2) ? 0xE9 : 0x20AC, now); break;
      case 8: rc = hx_delete(&d, &c, &sel, 0); break;
      case 9: rc = hx_delete(&d, &c, &sel, 1); break;
      case 10: {
        u8 b[5]; u32 n = 1 + tf_rand_below(tf, 4), j;
        for (j = 0; j < n; j++) b[j] = (u8)tf_rand_below(tf, 256);
        rc = hx_write(&d, &c, &sel, b, n);
        break;
      }
      default: {
        Selection s2;
        if (d.can_undo() && d.undo(&s2) == E_OK) {
          sel = s2; c.pos = s2.caret; c.nibble = 0; c.unit_end = 0;
        }
        break;
      }
      }
      // invariants
      len = d.length();
      if (rc != E_OK && rc != E_RANGE && rc != E_ACCESS && rc != E_ALIGN) bad++;
      if (c.pos > len || (c.pos == len && c.nibble)) bad++;
      if (op >= 3 && op <= 10 && rc == E_OK && sel.caret != c.pos) bad++;
      if (sel_hi(sel) > len) bad++;
      if (d.text_origin() > 0) {
        u8 head[3];
        d.read(0, head, d.text_origin());
        if (memcmp(head, init.data(), d.text_origin()) != 0) bad++;
      }
      if ((enc == 'l' || enc == 'b' || enc == 'L') && (len & 1)) bad++;
    }
    if (bad) tf_fail(tf, __FILE__, __LINE__, "random '%c': %u invariant violations", enc, bad);
    else tf->checks++;
    while (d.can_undo()) { Selection s2; if (d.undo(&s2) != E_OK) break; }
    hxt_doc_bytes(&d, &got);
    TF_CHECK(got.size() == init.size() && memcmp(got.data(), init.data(), init.size()) == 0);
    {
      char why[200];
      why[0] = 0;
      TF_EQ_I(d.tree()->check(why, sizeof(why)), 0);
    }
  }
}

// busy and indexing documents refuse edits with E_BUSY
static void test_busy(TF* tf)
{
  Document d;
  EBytes init;
  HexCaret c;
  Selection sel;
  hxt_parse("41 42", &init);
  TF_EQ_I(hxt_open(&d, 'a', init), E_OK);
  memset(&c, 0, sizeof(c));
  sel.anchor = sel.caret = 0;
  d.set_busy(true);
  TF_EQ_I(hx_type_nibble(&d, &c, &sel, 1, 1), E_BUSY);
  TF_EQ_I(hx_type_char(&d, &c, &sel, 'x', 1), E_BUSY);
  TF_EQ_I(hx_write(&d, &c, &sel, init.data(), 1), E_BUSY);
  TF_EQ_I(hx_delete(&d, &c, &sel, 0), E_BUSY);
  d.set_busy(false);
  TF_EQ_I(hx_delete(&d, &c, &sel, 0), E_OK);
  TF_EQ_U(d.length(), 1);
  TF_EQ_I(hx_type_nibble(0, &c, &sel, 1, 1), E_RANGE);
}

// out of memory: the edit fails and nothing changes
static void test_oom(TF* tf)
{
  int k, fails = 0;
  for (k = 1; k < 40; k++) {
    Document d;
    EBytes init, got;
    HexCaret c;
    Selection sel;
    int rc;
    hxt_parse("FF FE 41 00 42 00", &init);
    TF_EQ_I(hxt_open(&d, 'l', init), E_OK);
    memset(&c, 0, sizeof(c));
    c.insert = 1; c.pos = 4; sel.anchor = sel.caret = 4;
    e_test_fail_after(k);
    rc = hx_type_nibble(&d, &c, &sel, 7, 100);
    e_test_fail_after(-1);
    hxt_doc_bytes(&d, &got);
    if (rc == E_NOMEM) {
      fails++;
      TF_CHECK(got.size() == init.size() && memcmp(got.data(), init.data(), got.size()) == 0);
      TF_EQ_U(c.pos, 4);
    } else {
      TF_EQ_I(rc, E_OK);
      TF_EQ_U(got.size(), 8);
    }
  }
  TF_CHECK(fails >= 0);
}

void suite_edhex(TF* tf)
{
  test_golden(tf);
  test_badmarkers(tf);
  test_merge(tf);
  test_random(tf);
  test_busy(tf);
  test_oom(tf);
}
