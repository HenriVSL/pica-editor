// t_doc.cpp - suite "doc": Document (DESIGN 6.7, 7, 17.1). Owner: T01.
#include "tf.h"
#include "doc.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// Flat model with its own undo history

struct MOp { epos pos; u32 old_off, old_len, new_off, new_len; };
struct MGroup { u32 op_first, nops; Selection before, after; };

struct Model {
  EBytes cur, pool;
  Vec<MOp> ops;
  Vec<MGroup> groups;
  u32 ncur, saved;
  int saved_valid;
  Model() : ncur(0), saved(0), saved_valid(1) {}
  void begin(const Selection& before)
  {
    MGroup g;
    if (ncur < groups.size()) {
      if (saved_valid && saved > ncur) saved_valid = 0;
      ops.resize(groups[ncur].op_first);
      groups.resize(ncur);
    }
    g.op_first = ops.size(); g.nops = 0; g.before = before; g.after = before;
    groups.push(g);
    ncur++;
  }
  void rec(epos pos, epos del, const u8* ins, epos n)
  {
    MOp op;
    op.pos = pos;
    op.old_off = pool.size(); op.old_len = del; pool.append(cur.data() + pos, del);
    op.new_off = pool.size(); op.new_len = n; pool.append(ins, n);
    ops.push(op);
    groups[ncur - 1].nops++;
    cur.erase(pos, del);
    cur.insert(pos, ins, n);
  }
  void end(const Selection& after) { groups[ncur - 1].after = after; }
  int undo(Selection* s)
  {
    u32 i;
    MGroup g;
    if (ncur == 0) return 0;
    g = groups[--ncur];
    for (i = g.nops; i > 0; i--) {
      const MOp& op = ops[g.op_first + i - 1];
      cur.erase(op.pos, op.new_len);
      cur.insert(op.pos, pool.data() + op.old_off, op.old_len);
    }
    *s = g.before;
    return 1;
  }
  int redo(Selection* s)
  {
    u32 i;
    MGroup g;
    if (ncur >= groups.size()) return 0;
    g = groups[ncur++];
    for (i = 0; i < g.nops; i++) {
      const MOp& op = ops[g.op_first + i];
      cur.erase(op.pos, op.old_len);
      cur.insert(op.pos, pool.data() + op.new_off, op.new_len);
    }
    *s = g.after;
    return 1;
  }
  bool modified() const { return !(saved_valid && saved == ncur); }
  void set_saved() { saved = ncur; saved_valid = 1; }
  void clear_undo()
  {
    int was = !modified();
    groups.clear(); ops.clear(); pool.clear();
    ncur = 0; saved = 0; saved_valid = was;
  }
};

// unit value at offset i of a flat buffer
static u32 unit_at(const u8* p, u32 i, u32 unit, int be)
{
  if (unit == 1) return p[i];
  return be ? ((u32)p[i] << 8 | p[i + 1]) : ((u32)p[i + 1] << 8 | p[i]);
}

static void bf_eol(const u8* p, u32 n, u32 unit, int be, EolCounts* c)
{
  u32 i, prev = 0xFFFFFFFFu;
  c->crlf = c->lf = c->cr = 0;
  for (i = 0; i + unit <= n; i += unit) {
    u32 v = unit_at(p, i, unit, be);
    if (v == 0x0A) { if (prev == 0x0D) c->crlf++; else c->lf++; }
    else if (v == 0x0D) {
      u32 nx = i + 2 * unit <= n ? unit_at(p, i + unit, unit, be) : 0xFFFFFFFFu;
      if (nx != 0x0A) c->cr++;
    }
    prev = v;
  }
}

// brute-force line table: starts of every line
static void bf_lines(const u8* p, u32 n, u32 unit, int be, int crm, Vec<epos>* starts)
{
  u32 i, br = crm ? 0x0D : 0x0A;
  starts->clear();
  starts->push(0);
  for (i = 0; i + unit <= n; i += unit) if (unit_at(p, i, unit, be) == br) starts->push(i + unit);
}

static epos bf_line_end(const u8* p, u32 n, u32 unit, int be, int crm, const Vec<epos>& st, u32 line)
{
  epos bp;
  if (line + 1 >= st.size()) return n;
  bp = st[line + 1] - unit;
  if (!crm && bp >= unit && unit_at(p, bp - unit, unit, be) == 0x0D) return bp - unit;
  return bp;
}

static int doc_eq(TF* tf, Document& d, const EBytes& m, const char* where, u32 step)
{
  static u8 buf[4096];
  epos off = 0, n = m.size();
  if (d.length() != n) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: length %u, model %u", where, step, d.length(), n);
    return 0;
  }
  while (off < n) {
    epos k = n - off;
    if (k > sizeof(buf)) k = sizeof(buf);
    if (d.read(off, buf, k) != E_OK || memcmp(buf, m.data() + off, k) != 0) {
      tf_fail(tf, __FILE__, __LINE__, "%s step %u: content differs in [%u, %u)", where, step, off, off + k);
      return 0;
    }
    off += k;
  }
  return 1;
}

static int doc_check(TF* tf, Document& d, const char* where, u32 step)
{
  char why[200];
  if (d.tree()->check(why, sizeof(why)) != 0) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: check: %s", where, step, why);
    return 0;
  }
  return 1;
}

static int eol_eq(TF* tf, Document& d, const EBytes& m, const char* where, u32 step)
{
  EolCounts c;
  u32 unit = (d.enc().kind == EK_UTF16LE || d.enc().kind == EK_UTF16BE) ? 2 : 1;
  bf_eol(m.data(), m.size(), unit, d.enc().kind == EK_UTF16BE, &c);
  if (c.crlf != d.eol_counts().crlf || c.lf != d.eol_counts().lf || c.cr != d.eol_counts().cr) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: eol counts %u/%u/%u, recount %u/%u/%u", where, step,
            d.eol_counts().crlf, d.eol_counts().lf, d.eol_counts().cr, c.crlf, c.lf, c.cr);
    return 0;
  }
  return 1;
}

static int lines_eq(TF* tf, Document& d, const EBytes& m, int crm, const char* where, u32 step, u32 samples)
{
  Vec<epos> st;
  u32 unit = (d.enc().kind == EK_UTF16LE || d.enc().kind == EK_UTF16BE) ? 2 : 1, i;
  int be = d.enc().kind == EK_UTF16BE;
  bf_lines(m.data(), m.size(), unit, be, crm, &st);
  if (d.line_count() != st.size()) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: line_count %u, bf %u", where, step, d.line_count(), st.size());
    return 0;
  }
  for (i = 0; i < samples; i++) {
    u32 L = tf_rand_below(tf, st.size());
    epos ls = d.line_start(L), le = d.line_end(L), ln = d.line_next(L);
    epos bls = st[L], ble = bf_line_end(m.data(), m.size(), unit, be, crm, st, L);
    epos bln = L + 1 < st.size() ? st[L + 1] : m.size();
    epos p = tf_rand_below(tf, m.size() + 1);
    u32 lo = d.line_of(p), blo = 0;
    while (blo + 1 < st.size() && st[blo + 1] <= p) blo++;
    if (ls != bls || le != ble || ln != bln || lo != blo) {
      tf_fail(tf, __FILE__, __LINE__, "%s step %u: line %u start/end/next %u/%u/%u bf %u/%u/%u; line_of(%u) %u bf %u",
              where, step, L, ls, le, ln, bls, ble, bln, p, lo, blo);
      return 0;
    }
  }
  if (d.line_start(st.size()) != m.size()) { tf_fail(tf, __FILE__, __LINE__, "%s: line_start(count)", where); return 0; }
  return 1;
}

static void rand_text(TF* tf, u8* p, u32 n, u32 unit, int be)
{
  static const u8 alpha[] = "abcdefgh\n\n\r\r ";
  u32 i;
  if (unit == 1) { for (i = 0; i < n; i++) p[i] = alpha[tf_rand_below(tf, sizeof(alpha) - 1)]; return; }
  for (i = 0; i + 1 < n; i += 2) {
    u32 r = tf_rand_below(tf, 16), v;
    if (r < 3) v = 0x0A; else if (r < 5) v = 0x0D; else if (r == 5) v = 0x0A00; else v = 'a' + r;
    if (be) { p[i] = (u8)(v >> 8); p[i + 1] = (u8)v; } else { p[i] = (u8)v; p[i + 1] = (u8)(v >> 8); }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Differential random test

static int sel_eq(const Selection& a, const Selection& b) { return a.anchor == b.anchor && a.caret == b.caret; }

static void random_run(TF* tf, const EncSpec& enc, EolStyle eol, u32 ops, const char* name)
{
  Document d;
  Model m;
  MemSource* src = new MemSource();
  u32 unit = (enc.kind == EK_UTF16LE || enc.kind == EK_UTF16BE) ? 2 : 1, step;
  int be = enc.kind == EK_UTF16BE, crm = eol == EOL_CR;
  static u8 buf[1200];
  u32 n0 = 600;
  rand_text(tf, buf, n0, unit, be);
  src->copy_from(buf, n0);
  m.cur.append(buf, n0);
  TF_EQ_I(d.init_source(src, enc, 0, eol, 0), E_OK);
  if (!eol_eq(tf, d, m.cur, name, 0)) return;
  for (step = 0; step < ops; step++) {
    u32 r = tf_rand_below(tf, 100);
    epos len = m.cur.size();
    Selection s, ms;
    int rc;
    if (r < 45 || (r < 55 && len < 64)) {              // one replace, implicit group
      epos pos = tf_rand_below(tf, len + 1), del = tf_rand_below(tf, 5) ? tf_rand_below(tf, 6) : tf_rand_below(tf, 300);
      epos ins = tf_rand_below(tf, 4) ? tf_rand_below(tf, 8) : tf_rand_below(tf, 200);
      if (unit == 2) { pos &= ~1u; del &= ~1u; ins &= ~1u; }
      if (del > len - pos) del = len - pos;
      if (unit == 2 && (del & 1) && pos + del != len) del--;
      rand_text(tf, buf, ins, unit, be);
      rc = d.replace(pos, del, buf, ins);
      if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s step %u: replace %d", name, step, rc); break; }
      if (del || ins) {
        s.anchor = s.caret = pos;
        m.begin(s);
        m.rec(pos, del, buf, ins);
        s.anchor = s.caret = pos + ins;
        m.end(s);
      }
    } else if (r < 55) {                               // multi-op group with explicit selections
      u32 k = 2 + tf_rand_below(tf, 3), j, any = 0;
      Selection b, a;
      b.anchor = tf_rand_below(tf, len + 1); b.caret = tf_rand_below(tf, len + 1);
      d.begin_edit(UK_OTHER, b, step);
      for (j = 0; j < k; j++) {
        epos cl = m.cur.size(), pos = tf_rand_below(tf, cl + 1), del = tf_rand_below(tf, 10), ins = tf_rand_below(tf, 10);
        if (unit == 2) { pos &= ~1u; del &= ~1u; ins &= ~1u; }
        if (del > cl - pos) del = cl - pos;
        if (unit == 2 && (del & 1) && pos + del != cl) del--;
        rand_text(tf, buf, ins, unit, be);
        rc = d.replace(pos, del, buf, ins);
        if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s step %u: group replace %d", name, step, rc); break; }
        if (del || ins) {
          if (!any) m.begin(b);
          any = 1;
          m.rec(pos, del, buf, ins);
        }
      }
      a.anchor = tf_rand_below(tf, m.cur.size() + 1); a.caret = a.anchor;
      d.end_edit(a);
      if (any) m.end(a);
    } else if (r < 72) {
      int mu = m.undo(&ms);
      rc = d.undo(&s);
      if (mu ? (rc != E_OK || !sel_eq(s, ms)) : rc != E_STATE) {
        tf_fail(tf, __FILE__, __LINE__, "%s step %u: undo rc %d (model %d) sel %u/%u vs %u/%u", name, step, rc, mu,
                s.anchor, s.caret, ms.anchor, ms.caret);
        break;
      }
    } else if (r < 87) {
      int mr = m.redo(&ms);
      rc = d.redo(&s);
      if (mr ? (rc != E_OK || !sel_eq(s, ms)) : rc != E_STATE) {
        tf_fail(tf, __FILE__, __LINE__, "%s step %u: redo rc %d (model %d)", name, step, rc, mr);
        break;
      }
    } else if (r < 92) {
      d.set_saved();
      m.set_saved();
    } else if (r < 93) {
      d.clear_undo();
      m.clear_undo();
    } else if (r < 94) {                               // async save of a past state
      u32 sq = d.undo_seq();
      u32 at = m.ncur;
      if (m.undo(&ms) && d.undo(&s) == E_OK) {
        d.set_saved_seq(sq);
        m.saved = at; m.saved_valid = 1;
      }
    } else {
      if (!lines_eq(tf, d, m.cur, crm, name, step, 6)) break;
    }
    if (!doc_check(tf, d, name, step)) break;
    if ((step & 3) == 0 || r >= 55) {                  // full compares every 4th edit and after every undo/redo
      if (!doc_eq(tf, d, m.cur, name, step)) break;
      if (!eol_eq(tf, d, m.cur, name, step)) break;
    }
    if (d.modified() != m.modified() || d.can_undo() != (m.ncur > 0) || d.can_redo() != (m.ncur < m.groups.size())) {
      tf_fail(tf, __FILE__, __LINE__, "%s step %u: modified %d/%d can_undo %d/%d can_redo %d/%d", name, step,
              d.modified(), m.modified(), d.can_undo(), m.ncur > 0, d.can_redo(), m.ncur < m.groups.size());
      break;
    }
    if (m.cur.size() > 1600) {                          // trim (recorded like any edit)
      epos pos = tf_rand_below(tf, 400) & ~1u;
      TF_EQ_I(d.replace(pos, 800, 0, 0), E_OK);
      s.anchor = s.caret = pos;
      m.begin(s);
      m.rec(pos, 800, 0, 0);
      m.end(s);
    }
    if (m.pool.size() > 4000000) { d.clear_undo(); m.clear_undo(); }
  }
  TF_CHECK(step == ops);
  TF_CHECK(lines_eq(tf, d, m.cur, crm, name, step, 100));
}

// ---------------------------------------------------------------------------------------------------------------
// Basics

static void test_init(TF* tf)
{
  Document d;
  u32 n;
  const u8* e;
  u8 buf[8];
  TF_EQ_U(d.length(), 0);
  TF_EQ_U(d.line_count(), 1);
  TF_CHECK(!d.modified());
  TF_EQ_I(d.init_empty(enc_utf8(1), EOL_LF), E_OK);
  TF_EQ_U(d.length(), 3);
  TF_EQ_U(d.text_origin(), 3);
  TF_EQ_I(d.read(0, buf, 3), E_OK);
  TF_EQ_MEM(buf, "\xEF\xBB\xBF", 3);
  TF_CHECK(!d.modified());
  TF_CHECK(!d.can_undo());
  e = d.eol_bytes(&n);
  TF_EQ_U(n, 1);
  TF_EQ_U(e[0], 0x0A);
  TF_EQ_U(d.char_next(0), 3);
  TF_EQ_U(d.char_prev(3), 3);
  TF_EQ_U(d.snap_char(1), 3);
  TF_EQ_I(d.init_empty(enc_utf16be(1), EOL_CRLF), E_OK);
  TF_EQ_U(d.length(), 2);
  TF_EQ_U(d.text_origin(), 2);
  e = d.eol_bytes(&n);
  TF_EQ_U(n, 4);
  TF_EQ_MEM(e, "\x00\x0D\x00\x0A", 4);
  TF_EQ_I(d.init_empty(enc_utf16le(0), EOL_CR), E_OK);
  e = d.eol_bytes(&n);
  TF_EQ_U(n, 2);
  TF_EQ_MEM(e, "\x0D\x00", 2);
  TF_EQ_I(d.init_empty(enc_ansi(), EOL_CRLF), E_OK);
  TF_EQ_U(d.length(), 0);
  e = d.eol_bytes(&n);
  TF_EQ_MEM(e, "\r\n", 2);
  d.set_eol_style(EOL_LF);
  TF_EQ_I((int)d.eol_style(), (int)EOL_LF);
  e = d.eol_bytes(&n);
  TF_EQ_U(n, 1);
  TF_CHECK(d.codec() != 0);
}

static void test_lines(TF* tf)
{
  Document d;
  MemSource* s = new MemSource();
  static const char text[] = "one\r\ntwo\nthree\rstill three\r\n\nlast";
  s->copy_from((const u8*)text, sizeof(text) - 1);
  TF_EQ_I(d.init_source(s, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  TF_EQ_U(d.line_count(), 5);
  TF_EQ_U(d.line_start(0), 0);
  TF_EQ_U(d.line_end(0), 3);                         // CR of CRLF belongs to the EOL
  TF_EQ_U(d.line_next(0), 5);
  TF_EQ_U(d.line_end(1), 8);
  TF_EQ_U(d.line_start(2), 9);
  TF_EQ_U(d.line_end(2), 26);                        // lone CR is text in LF mode
  TF_EQ_U(d.line_start(3), 28);
  TF_EQ_U(d.line_end(3), 28);
  TF_EQ_U(d.line_start(4), 29);
  TF_EQ_U(d.line_end(4), 33);
  TF_EQ_U(d.line_next(4), 33);
  TF_EQ_U(d.line_of(0), 0);
  TF_EQ_U(d.line_of(4), 0);
  TF_EQ_U(d.line_of(5), 1);
  TF_EQ_U(d.line_of(33), 4);
  TF_EQ_U(d.line_of(1000), 4);
  TF_EQ_U(d.eol_counts().crlf, 2);
  TF_EQ_U(d.eol_counts().lf, 2);
  TF_EQ_U(d.eol_counts().cr, 1);
  TF_CHECK(d.eol_mixed());
  // CR mode
  {
    Document c;
    MemSource* cs = new MemSource();
    cs->copy_from((const u8*)"a\rb\rc", 5);
    TF_EQ_I(c.init_source(cs, enc_ansi(), 0, EOL_CR, 0), E_OK);
    TF_EQ_U(c.line_count(), 3);
    TF_EQ_U(c.line_end(0), 1);
    TF_EQ_U(c.line_start(1), 2);
    TF_EQ_U(c.eol_counts().cr, 2);
    TF_CHECK(!c.eol_mixed());
    TF_EQ_I(c.replace(5, 0, (const u8*)"\n", 1), E_OK);
    TF_CHECK(c.eol_mixed());
  }
  // UTF-16 LE with BOM: break units at even offsets only
  {
    Document u;
    MemSource* us = new MemSource();
    static const u8 b[] = { 0xFF, 0xFE, 'a', 0, 0x0D, 0, 0x0A, 0, 0x0A, 0x0A, 'b', 0, 0x0A, 0 };
    us->copy_from(b, sizeof(b));
    TF_EQ_I(u.init_source(us, enc_utf16le(1), 2, EOL_CRLF, 0), E_OK);
    TF_EQ_U(u.line_count(), 3);
    TF_EQ_U(u.line_end(0), 4);
    TF_EQ_U(u.line_start(1), 8);
    TF_EQ_U(u.line_end(1), 12);
    TF_EQ_U(u.eol_counts().crlf, 1);
    TF_EQ_U(u.eol_counts().lf, 1);
    TF_EQ_U(u.char_next(2), 4);
    TF_EQ_U(u.char_next(0), 2);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Characters

class FakeDbcs : public DbcsProvider {        // Shift-JIS-like lead/trail ranges (test only)
public:
  int is_lead(u8 b) const { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }
  ecp decode_pair(u8 lead, u8 trail) const
  {
    if (trail < 0x40 || trail == 0x7F || trail > 0xFC) return ECP_BADBYTE(lead);
    return 0x4E00 + (((u32)lead << 8 | trail) & 0xFFF);
  }
  ecp decode_single(u8 b) const { return b < 0x80 ? (ecp)b : ECP_BADBYTE(b); }
  u32 encode(ecp c, u8* out) const { if (c < 0x80) { out[0] = (u8)c; return 1; } return 0; }
};

static void split_at(Document& d, epos pos)      // leaves a piece boundary at pos, bytes unchanged
{
  epos n = (d.enc().kind == EK_UTF16LE || d.enc().kind == EK_UTF16BE) ? 2 : 1;
  d.replace(pos, 0, (const u8*)"\x01\x01", n);
  d.replace(pos, n, 0, 0);
}

static void test_chars(TF* tf)
{
  {   // UTF-8 with multibyte characters split across pieces, and an invalid byte
    Document d;
    MemSource* s = new MemSource();
    static const u8 t[] = { 'a', 0xC3, 0xA9, 'b', 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0xFF, 'z' };
    static const epos starts[] = { 0, 1, 3, 4, 7, 11, 12, 13 };
    u32 i;
    ecp c;
    s->copy_from(t, sizeof(t));
    TF_EQ_I(d.init_source(s, enc_utf8(0), 0, EOL_LF, 0), E_OK);
    split_at(d, 2); split_at(d, 5); split_at(d, 6); split_at(d, 9);
    TF_CHECK(d.tree()->piece_count() >= 5);
    for (i = 0; i + 1 < 8; i++) TF_EQ_U(d.char_next(starts[i]), starts[i + 1]);
    for (i = 7; i > 0; i--) TF_EQ_U(d.char_prev(starts[i]), starts[i - 1]);
    TF_EQ_U(d.char_prev(0), 0);
    TF_EQ_U(d.char_next(13), 13);
    TF_EQ_U(d.snap_char(2), 1);
    TF_EQ_U(d.snap_char(5), 4);
    TF_EQ_U(d.snap_char(6), 4);
    TF_EQ_U(d.snap_char(9), 7);
    TF_EQ_U(d.snap_char(11), 11);
    TF_EQ_U(d.snap_char(100), 13);
    TF_EQ_U(d.decode(1, &c), 2); TF_EQ_U(c, 0xE9);
    TF_EQ_U(d.decode(4, &c), 3); TF_EQ_U(c, 0x20AC);
    TF_EQ_U(d.decode(7, &c), 4); TF_EQ_U(c, 0x1F600);
    TF_EQ_U(d.decode(11, &c), 1); TF_EQ_U(c, ECP_BADBYTE(0xFF));
    TF_EQ_U(d.decode(13, &c), 0);
  }
  {   // UTF-16 LE with BOM, surrogate pair and a lone surrogate, odd trailing byte
    Document d;
    MemSource* s = new MemSource();
    static const u8 t[] = { 0xFF, 0xFE, 'A', 0, 0xE9, 0, 0x3D, 0xD8, 0x00, 0xDE, 0x00, 0xDC, 'z', 0, 0x41 };
    ecp c;
    s->copy_from(t, sizeof(t));
    TF_EQ_I(d.init_source(s, enc_utf16le(1), 2, EOL_LF, 0), E_OK);
    split_at(d, 8);
    TF_EQ_U(d.char_next(0), 2);
    TF_EQ_U(d.char_next(2), 4);
    TF_EQ_U(d.char_next(4), 6);
    TF_EQ_U(d.char_next(6), 10);                     // pair across a piece boundary
    TF_EQ_U(d.char_next(10), 12);
    TF_EQ_U(d.char_next(12), 14);
    TF_EQ_U(d.char_next(14), 15);                    // odd trailing byte: one bad byte
    TF_EQ_U(d.char_prev(10), 6);
    TF_EQ_U(d.char_prev(6), 4);
    TF_EQ_U(d.char_prev(2), 2);
    TF_EQ_U(d.char_prev(15), 14);
    TF_EQ_U(d.snap_char(8), 6);
    TF_EQ_U(d.snap_char(7), 6);
    TF_EQ_U(d.snap_char(3), 2);
    TF_EQ_U(d.decode(6, &c), 4); TF_EQ_U(c, 0x1F600);
    TF_EQ_U(d.decode(10, &c), 2); TF_EQ_U(c, ECP_BADUNIT(0xDC00));
    TF_EQ_U(d.decode(14, &c), 1); TF_EQ_U(c, ECP_BADBYTE(0x41));
    // alignment rule
    TF_EQ_I(d.replace(3, 0, (const u8*)"xx", 2), E_ALIGN);
    TF_EQ_I(d.replace(2, 1, 0, 0), E_ALIGN);
    TF_EQ_I(d.replace(2, 0, (const u8*)"x", 1), E_ALIGN);
    TF_EQ_I(d.replace(15, 0, (const u8*)"xx", 2), E_ALIGN);   // nothing after the odd trailing byte
    TF_EQ_I(d.replace(12, 0, (const u8*)"x\0", 2), E_OK);     // before it is fine
    TF_EQ_I(d.replace(16, 1, 0, 0), E_OK);                    // the dangling byte itself can go
    TF_EQ_U(d.length(), 16);
    TF_EQ_I(d.tree()->check(0, 0), 0);
  }
  {   // DBCS: forward decoding from the line start
    FakeDbcs prov;
    EncSpec e;
    Document d;
    MemSource* s = new MemSource();
    static const u8 t[] = { 'a', 0x82, 0xA0, 0x82, 0x82, '\n', 0x82, 0x41, 0x82, 'x', 0x82 };
    e.kind = EK_DBCS; e.bom = 0; e.cp = 932;
    TF_EQ_I(codec_register_dbcs(932, &prov), E_OK);
    s->copy_from(t, sizeof(t));
    TF_EQ_I(d.init_source(s, e, 0, EOL_LF, 0), E_OK);
    TF_EQ_U(d.char_next(1), 3);
    TF_EQ_U(d.char_next(3), 5);
    TF_EQ_U(d.char_prev(5), 3);                      // 82 82 is a pair: a self-sync rule would get this wrong
    TF_EQ_U(d.char_prev(3), 1);
    TF_EQ_U(d.char_prev(6), 5);
    TF_EQ_U(d.char_prev(8), 6);
    TF_EQ_U(d.snap_char(4), 3);
    TF_EQ_U(d.snap_char(2), 1);
    TF_EQ_U(d.char_next(10), 11);                    // lead byte at the end: one bad byte
    TF_EQ_I(codec_register_dbcs(932, 0), E_OK);
  }
}

static void test_replace_text(TF* tf)
{
  Document d;
  ecp s1[3], s2[2];
  u32 bad = 99;
  u8 buf[8];
  s1[0] = 'h'; s1[1] = 0xE9; s1[2] = 0x20AC;
  s2[0] = 'x'; s2[1] = 0x416;
  TF_EQ_I(d.init_empty(enc_cp(1252), EOL_CRLF), E_OK);
  TF_EQ_I(d.replace_text(0, 0, s1, 3, &bad), E_OK);
  TF_EQ_U(bad, 0);
  TF_EQ_U(d.length(), 3);
  TF_EQ_I(d.read(0, buf, 3), E_OK);
  TF_EQ_MEM(buf, "h\xE9\x80", 3);
  TF_EQ_I(d.replace_text(1, 1, s2, 2, &bad), E_RANGE);
  TF_EQ_U(bad, 1);
  TF_EQ_U(d.length(), 3);
  TF_EQ_I(d.init_empty(enc_utf8(0), EOL_CRLF), E_OK);
  TF_EQ_I(d.replace_text(0, 0, s2, 2, &bad), E_OK);
  TF_EQ_U(d.length(), 3);
  TF_EQ_I(d.read(0, buf, 3), E_OK);
  TF_EQ_MEM(buf, "x\xD0\x96", 3);
}

static void test_states(TF* tf)
{
  Document d;
  Selection s;
  TF_EQ_I(d.init_empty(enc_ansi(), EOL_CRLF), E_OK);
  TF_CHECK(d.editable());
  d.set_read_only(true);
  TF_CHECK(d.read_only());
  TF_CHECK(!d.editable());
  TF_EQ_I(d.replace(0, 0, (const u8*)"a", 1), E_READONLY);
  d.set_read_only(false);
  d.set_busy(true);
  TF_EQ_I(d.replace(0, 0, (const u8*)"a", 1), E_BUSY);
  TF_EQ_I(d.undo(&s), E_BUSY);
  d.set_busy(false);
  TF_EQ_I(d.replace(1, 0, (const u8*)"a", 1), E_RANGE);
  TF_EQ_I(d.replace(0, 1, 0, 0), E_RANGE);
  TF_EQ_I(d.replace(0, 0, (const u8*)"a", 1), E_OK);
  TF_CHECK(d.modified());
  d.set_saved();
  TF_CHECK(!d.modified());
  d.set_always_modified(true);
  TF_CHECK(d.modified());
  d.set_saved();
  TF_CHECK(!d.modified());
  {
    u32 c0 = d.change_seq();
    d.replace(0, 1, (const u8*)"b", 1);
    TF_CHECK(d.change_seq() != c0);
  }
  // undo while a group is open is refused
  s.anchor = s.caret = 0;
  d.begin_edit(UK_OTHER, s, 0);
  TF_EQ_I(d.undo(&s), E_STATE);
  d.end_edit(s);
  TF_EQ_I(d.undo(&s), E_OK);
}

// ---------------------------------------------------------------------------------------------------------------
// Undo details: selections, merging, save points

static void test_undo_details(TF* tf)
{
  Document d;
  Selection s, a;
  u32 i, sq;
  TF_EQ_I(d.init_empty(enc_ansi(), EOL_CRLF), E_OK);
  // typing merges into one group (caret-adjacent, same kind, within 2 s)
  for (i = 0; i < 5; i++) {
    s.anchor = s.caret = i;
    d.begin_edit(UK_TYPING, s, 1000 + i * 100);
    d.replace(i, 0, (const u8*)"abcde" + i, 1);
    a.anchor = a.caret = i + 1;
    d.end_edit(a);
  }
  s.anchor = s.caret = 5;
  d.begin_edit(UK_TYPING, s, 9000);                  // too late: new group
  d.replace(5, 0, (const u8*)"f", 1);
  a.anchor = a.caret = 6;
  d.end_edit(a);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_U(d.length(), 5);
  TF_EQ_U(s.caret, 5);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_U(d.length(), 0);
  TF_EQ_U(s.caret, 0);
  TF_EQ_I(d.undo(&s), E_STATE);
  TF_EQ_I(d.redo(&s), E_OK);
  TF_EQ_U(s.caret, 5);
  TF_EQ_U(d.length(), 5);
  // save point, undo to it, modified clears; a new edit after undo discards the saved branch
  d.set_saved();
  TF_CHECK(!d.modified());
  TF_EQ_I(d.redo(&s), E_OK);
  TF_CHECK(d.modified());
  TF_EQ_I(d.undo(&s), E_OK);
  TF_CHECK(!d.modified());
  TF_EQ_I(d.undo(&s), E_OK);
  TF_CHECK(d.modified());
  d.replace(0, 0, (const u8*)"Q", 1);
  TF_CHECK(d.modified());
  TF_EQ_I(d.undo(&s), E_OK);
  TF_CHECK(d.modified());                            // saved state was on the discarded redo side
  // async save: snapshot seq, document busy while saving, then set_saved_seq
  d.replace(0, 0, (const u8*)"xyz", 3);
  sq = d.undo_seq();
  d.set_busy(true);
  TF_EQ_I(d.replace(0, 0, (const u8*)"!", 1), E_BUSY);
  d.set_busy(false);
  d.set_saved_seq(sq);
  TF_CHECK(!d.modified());
  // the save breaks merging
  s.anchor = s.caret = 3;
  d.begin_edit(UK_TYPING, s, 100);
  d.replace(3, 0, (const u8*)"1", 1);
  a.anchor = a.caret = 4;
  d.end_edit(a);
  d.set_saved();
  s.anchor = s.caret = 4;
  d.begin_edit(UK_TYPING, s, 150);
  d.replace(4, 0, (const u8*)"2", 1);
  a.anchor = a.caret = 5;
  d.end_edit(a);
  TF_CHECK(d.modified());
  TF_EQ_I(d.undo(&s), E_OK);
  TF_CHECK(!d.modified());
  // clear_undo keeps the status
  d.clear_undo();
  TF_CHECK(!d.can_undo());
  TF_CHECK(!d.modified());
}

// ---------------------------------------------------------------------------------------------------------------
// Listeners

struct EvLog : public DocListener {
  Vec<EditEvent> ev;
  Document* remove_from;
  EvLog() : remove_from(0) {}
  void doc_changed(Document* d, const EditEvent& e)
  {
    ev.push(e);
    if (remove_from) { remove_from = 0; d->remove_listener(this); }
  }
};

static void test_listeners(TF* tf)
{
  Document d;
  EvLog a, b;
  Selection s;
  MemSource* src = new MemSource();
  src->copy_from((const u8*)"l0\nl1\nl2\n", 9);
  TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_LF, 0), E_OK);
  TF_EQ_I(d.add_listener(&a), E_OK);
  TF_EQ_I(d.add_listener(&a), E_OK);                 // no duplicates
  TF_EQ_I(d.add_listener(&b), E_OK);
  TF_EQ_I(d.replace(4, 3, (const u8*)"X\nY\nZ", 5), E_OK);   // "l0\nl" + "X\nY\nZ" + "2\n"
  TF_EQ_U(a.ev.size(), 1);
  TF_EQ_U(a.ev[0].pos, 4);
  TF_EQ_U(a.ev[0].removed, 3);
  TF_EQ_U(a.ev[0].inserted, 5);
  TF_EQ_U(a.ev[0].line, 1);
  TF_EQ_U(a.ev[0].lines_removed, 1);
  TF_EQ_U(a.ev[0].lines_added, 2);
  TF_EQ_U(a.ev[0].flags, 0);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_CHECK(a.ev.size() >= 2);
  TF_CHECK((a.ev.back().flags & EV_UNDO) != 0);
  TF_EQ_I(d.redo(&s), E_OK);
  TF_CHECK((a.ev.back().flags & EV_REDO) != 0);
  // a listener removing itself during dispatch; the other still gets the event
  {
    u32 nb = b.ev.size(), na = a.ev.size();
    a.remove_from = &d;
    d.replace(0, 0, (const u8*)"q", 1);
    TF_EQ_U(a.ev.size(), na + 1);
    TF_EQ_U(b.ev.size(), nb + 1);
    d.replace(0, 0, (const u8*)"q", 1);
    TF_EQ_U(a.ev.size(), na + 1);
    TF_EQ_U(b.ev.size(), nb + 2);
  }
  // reset fires EV_RESET, clears undo, not modified
  {
    MemSource* s2 = new MemSource();
    s2->copy_from((const u8*)"new\n", 4);
    TF_EQ_I(d.reset_source(s2, enc_ansi(), 0, EOL_LF, 0), E_OK);
    TF_CHECK((b.ev.back().flags & EV_RESET) != 0);
    TF_EQ_U(b.ev.back().inserted, 4);
    TF_CHECK(!d.can_undo());
    TF_CHECK(!d.modified());
    TF_EQ_U(d.line_count(), 2);
  }
  d.remove_listener(&b);
}

// ---------------------------------------------------------------------------------------------------------------
// Encoding changes

static void doc_bytes(Document& d, EBytes* out)
{
  out->clear();
  out->resize(d.length());
  d.read(0, out->data(), d.length());
}

static void test_convert(TF* tf)
{
  Document d;
  MemSource* src = new MemSource();
  ConvertReport rep;
  EBytes b;
  Selection s;
  static const u8 t[] = "h\xC3\xA9 \xE2\x82\xAC \xD0\x96 \xFF\r\nend";
  src->copy_from(t, sizeof(t) - 1);
  TF_EQ_I(d.init_source(src, enc_utf8(0), 0, EOL_CRLF, 0), E_OK);
  TF_EQ_I(d.check_convert(enc_cp(1252), &rep, 0), E_OK);
  TF_EQ_U(rep.unrepresentable, 1);                   // U+0416
  TF_EQ_U(rep.invalid_src, 1);                       // the FF byte
  TF_EQ_U(rep.first_bad, 8);                         // the U+0416 comes first
  TF_EQ_I(d.convert(enc_cp(1252), &rep, 0), E_OK);
  doc_bytes(d, &b);
  TF_EQ_U(b.size(), 13);
  TF_EQ_MEM(b.data(), "h\xE9 \x80 ? ?\r\nend", 13);
  TF_CHECK(d.enc().kind == EK_SBCS && d.enc().cp == 1252);
  TF_CHECK(d.modified());
  TF_EQ_U(d.eol_counts().crlf, 1);
  TF_EQ_I(d.undo(&s), E_OK);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), t, sizeof(t) - 1);
  TF_CHECK(d.enc().kind == EK_UTF8);
  TF_CHECK(!d.modified());
  TF_EQ_I(d.redo(&s), E_OK);
  TF_CHECK(d.enc().cp == 1252);
  TF_EQ_I(d.undo(&s), E_OK);
  // UTF-8 -> UTF-8 with BOM (lossless, keeps the invalid byte)
  TF_EQ_I(d.check_convert(enc_utf8(1), &rep, 0), E_OK);
  TF_EQ_U(rep.invalid_src + rep.unrepresentable, 0);
  TF_EQ_I(d.convert(enc_utf8(1), &rep, 0), E_OK);
  TF_EQ_U(d.text_origin(), 3);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), "\xEF\xBB\xBF", 3);
  TF_EQ_MEM(b.data() + 3, t, sizeof(t) - 1);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_U(d.text_origin(), 0);
  TF_EQ_U(d.length(), sizeof(t) - 1);
  // UTF-8 -> UTF-16 LE (BOM): lines and EOL counts follow, undo restores the byte mode
  TF_EQ_I(d.convert(enc_utf16le(1), &rep, 0), E_OK);
  TF_EQ_U(d.text_origin(), 2);
  TF_EQ_U(d.line_count(), 2);
  TF_EQ_U(d.eol_counts().crlf, 1);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), "\xFF\xFE" "h\0\xE9\0", 6);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  TF_EQ_U(d.tree()->break_spec().unit, 2);
  TF_EQ_I(d.replace(2, 0, (const u8*)"x\0", 2), E_OK);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_U(d.tree()->break_spec().unit, 1);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), t, sizeof(t) - 1);
  TF_EQ_I(d.redo(&s), E_OK);
  TF_EQ_U(d.tree()->break_spec().unit, 2);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  // UTF-16 LE -> BE
  TF_EQ_I(d.convert(enc_utf16be(1), &rep, 0), E_OK);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), "\xFE\xFF" "\0h\0\xE9", 6);
  TF_EQ_U(d.line_count(), 2);
  TF_EQ_U(d.line_end(0), d.length() - 6 - 4);       // "end" + CRLF
  // read-only / unsupported
  {
    EncSpec bad;
    bad.kind = EK_DBCS; bad.bom = 0; bad.cp = 949;
    TF_EQ_I(d.convert(bad, &rep, 0), E_UNSUPPORTED);
    d.set_read_only(true);
    TF_EQ_I(d.convert(enc_utf8(0), &rep, 0), E_READONLY);
    d.set_read_only(false);
  }
}

static void test_convert_eol(TF* tf)
{
  Document d;
  MemSource* src = new MemSource();
  EBytes b;
  Selection s;
  static const char t[] = "a\r\nb\nc\rd\r\n";
  src->copy_from((const u8*)t, sizeof(t) - 1);
  TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  TF_EQ_U(d.line_count(), 4);
  TF_EQ_I(d.convert_eol(EOL_LF, 0), E_OK);
  doc_bytes(d, &b);
  TF_EQ_U(b.size(), 8);
  TF_EQ_MEM(b.data(), "a\nb\nc\nd\n", 8);
  TF_EQ_U(d.eol_counts().lf, 4);
  TF_EQ_U(d.eol_counts().crlf + d.eol_counts().cr, 0);
  TF_EQ_I((int)d.eol_style(), (int)EOL_LF);
  TF_EQ_U(d.line_count(), 5);
  TF_EQ_I(d.convert_eol(EOL_CR, 0), E_OK);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), "a\rb\rc\rd\r", 8);
  TF_EQ_U(d.line_count(), 5);                        // CR mode
  TF_EQ_U(d.line_end(1), 3);
  TF_EQ_U(d.eol_counts().cr, 4);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  TF_EQ_I(d.replace(1, 0, (const u8*)"\r", 1), E_OK);   // typing a CR breaks a line in CR mode
  TF_EQ_U(d.line_count(), 6);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_I(d.convert_eol(EOL_CRLF, 0), E_OK);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), "a\r\nb\r\nc\r\nd\r\n", 12);
  TF_EQ_U(d.line_count(), 5);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_I(d.undo(&s), E_OK);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), t, sizeof(t) - 1);
  TF_EQ_U(d.line_count(), 4);
  TF_EQ_I((int)d.eol_style(), (int)EOL_CRLF);
  TF_CHECK(!d.modified());
  TF_EQ_I(d.redo(&s), E_OK);
  TF_EQ_I(d.redo(&s), E_OK);
  TF_EQ_U(d.line_count(), 5);
  TF_EQ_I((int)d.eol_style(), (int)EOL_CR);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  // UTF-16 BE
  {
    Document u;
    MemSource* us = new MemSource();
    static const u8 ut[] = { 0xFE, 0xFF, 0, 'a', 0, 0x0D, 0, 0x0A, 0, 'b', 0, 0x0A, 0x0A, 0 };
    us->copy_from(ut, sizeof(ut));
    TF_EQ_I(u.init_source(us, enc_utf16be(1), 2, EOL_CRLF, 0), E_OK);
    TF_EQ_I(u.convert_eol(EOL_CRLF, 0), E_OK);
    doc_bytes(u, &b);
    TF_EQ_U(b.size(), 16);
    TF_EQ_MEM(b.data() + 8, "\0b\0\x0D\0\x0A\x0A\0", 8);
    TF_EQ_U(u.eol_counts().crlf, 2);
    TF_EQ_U(u.line_count(), 3);
  }
}

static void test_reinterpret(TF* tf)
{
  Document d;
  MemSource* src = new MemSource();
  EBytes b, m;
  Selection s;
  u32 i;
  static const u8 t[] = { 'a', 0, 0x0A, 0, 'b', 0, 0x0A, 0x0A, 'c', 0 };
  src->copy_from(t, sizeof(t));
  m.append(t, sizeof(t));
  TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  TF_EQ_U(d.line_count(), 4);
  // odd edits in byte mode, then reinterpret as UTF-16 and undo them across the switch
  TF_EQ_I(d.replace(1, 0, (const u8*)"\n", 1), E_OK);
  TF_EQ_I(d.replace(5, 2, (const u8*)"z", 1), E_OK);
  TF_EQ_I(d.reinterpret(enc_utf16le(0), 0, 0), E_OK);
  TF_CHECK(d.enc().kind == EK_UTF16LE);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  for (i = 0; i < 2; i++) {
    TF_EQ_I(d.undo(&s), E_OK);
    TF_EQ_I(d.tree()->check(0, 0), 0);
  }
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), t, sizeof(t));
  TF_EQ_U(d.line_count(), 2);                        // only 0A 00 at offset 2; "0A 0A" at 6 is U+0A0A
  {
    EolCounts c;
    bf_eol(m.data(), m.size(), 2, 0, &c);
    TF_EQ_U(d.eol_counts().lf, c.lf);
  }
  TF_EQ_I(d.redo(&s), E_OK);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  TF_EQ_I(d.reinterpret(enc_ansi(), 0, 0), E_OK);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  // reload as UTF-8 with a BOM present
  {
    Document u;
    MemSource* us = new MemSource();
    us->copy_from((const u8*)"\xEF\xBB\xBFx\n", 5);
    TF_EQ_I(u.init_source(us, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
    TF_EQ_I(u.reinterpret(enc_utf8(1), 3, 0), E_OK);
    TF_EQ_U(u.text_origin(), 3);
    TF_CHECK(u.enc().bom == 1);
    TF_EQ_U(u.char_next(0), 3);
  }
}

static void test_reload(TF* tf)
{
  Document d;
  MemSource* src = new MemSource();
  MemSource nw, bom;
  EBytes b;
  Selection s;
  src->copy_from((const u8*)"abc", 3);
  TF_EQ_I(d.init_source(src, enc_utf8(0), 0, EOL_LF, 0), E_OK);
  nw.copy_from((const u8*)"xyz\nq", 5);
  TF_EQ_I(d.reload_as_edit(&nw), E_OK);
  doc_bytes(d, &b);
  TF_EQ_MEM(b.data(), "xyz\nq", 5);
  TF_EQ_U(d.line_count(), 2);
  TF_CHECK(d.modified());
  TF_EQ_I(d.undo(&s), E_OK);
  doc_bytes(d, &b);
  TF_EQ_U(b.size(), 3);
  TF_EQ_MEM(b.data(), "abc", 3);
  TF_CHECK(!d.modified());
  // the new file has a BOM: origin follows, undoably
  bom.copy_from((const u8*)"\xEF\xBB\xBFhi", 5);
  TF_EQ_I(d.reload_as_edit(&bom), E_OK);
  TF_EQ_U(d.text_origin(), 3);
  TF_CHECK(d.enc().bom == 1);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_U(d.text_origin(), 0);
  TF_CHECK(d.enc().bom == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// On-demand indexing, snapshots, bytes()

static void test_indexing(TF* tf)
{
  Document d;
  SynthSource* syn = new SynthSource(65536 * 3 + 100, 4, SYN_TEXT_CRLF);
  EvLog log;
  u32 counts[4], i;
  EolCounts ec;
  ConvertReport rep;
  epos off;
  TF_EQ_I(d.init_source_indexing(syn, enc_ansi(), 0, EOL_CRLF), E_OK);
  TF_CHECK(d.indexing());
  TF_CHECK(!d.editable());
  TF_EQ_I(d.replace(0, 0, (const u8*)"x", 1), E_BUSY);
  d.add_listener(&log);
  ec.crlf = ec.lf = ec.cr = 0;
  for (i = 0, off = 0; i < 4; i++, off += 65536) {
    epos n = d.length() - off, j;
    u32 c = 0;
    if (n > 65536) n = 65536;
    for (j = 0; j < n; j++) if (SynthSource::byte_at(off + j, 4, SYN_TEXT_CRLF) == 0x0A) c++;
    counts[i] = c;
    ec.crlf += c;
  }
  TF_EQ_I(d.finish_indexing(counts, 4, ec), E_OK);
  TF_EQ_I(d.finish_indexing(counts, 4, ec), E_STATE);
  TF_CHECK(!d.indexing());
  TF_CHECK(d.editable());
  TF_EQ_U(log.ev.size(), 1);
  TF_CHECK((log.ev[0].flags & EV_INDEXED) != 0);
  TF_EQ_U(d.line_count(), ec.crlf + 1);
  TF_EQ_U(d.line_count(), 3 * 2048 + counts[3] + 1);
  TF_EQ_I(d.replace(0, 0, (const u8*)"\r\n", 2), E_OK);   // records start with text
  TF_EQ_U(d.eol_counts().crlf, ec.crlf + 1);
  TF_EQ_I(d.convert(enc_utf8(0), &rep, 0), E_TOOBIG);
  TF_EQ_I(d.convert_eol(EOL_LF, 0), E_TOOBIG);
  d.remove_listener(&log);
}

static void test_snapshot(TF* tf)
{
  Document d;
  MemSource* src = new MemSource();
  SnapshotSource snap;
  EBytes frozen, b;
  u8 buf[100];
  u32 i;
  rand_text(tf, buf, 100, 1, 0);
  src->copy_from(buf, 100);
  TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  for (i = 0; i < 20; i++) d.replace(tf_rand_below(tf, d.length() + 1), 0, buf, 7);
  doc_bytes(d, &frozen);
  TF_EQ_I(d.snapshot(&snap, 0), E_OK);
  for (i = 0; i < 100; i++) {
    epos p = tf_rand_below(tf, d.length());
    if (i & 1) d.replace(p, 0, buf, 13);
    else d.replace(p, emin<epos>(5, d.length() - p), 0, 0);
  }
  b.resize(snap.size());
  TF_EQ_U(snap.size(), frozen.size());
  TF_EQ_I(snap.read(0, b.data(), snap.size()), E_OK);
  TF_EQ_MEM(b.data(), frozen.data(), frozen.size());
  // bytes(): live view equals read()
  {
    ByteSource* v = d.bytes();
    EBytes x;
    doc_bytes(d, &frozen);
    TF_EQ_U(v->size(), d.length());
    x.resize(v->size());
    TF_EQ_I(v->read(0, x.data(), v->size()), E_OK);
    TF_EQ_MEM(x.data(), frozen.data(), frozen.size());
    d.replace(0, 1, (const u8*)"#", 1);
    TF_EQ_I(v->read(0, x.data(), 1), E_OK);
    TF_EQ_U(x[0], '#');
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Out of memory inside replace / undo / redo

static void test_oom(TF* tf)
{
  Document d;
  MemSource* src = new MemSource();
  EBytes before, after;
  static u8 big[300000];
  u32 k, ok = 0;
  Selection s;
  rand_text(tf, big, sizeof(big), 1, 0);
  src->copy_from(big, 4000);
  TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  for (k = 0; k < 40; k++) d.replace(tf_rand_below(tf, d.length()), 0, big + k, 3);   // fragment
  for (k = 1; k <= 60; k++) {
    u32 sq = d.undo_seq(), cs;
    bool cu = d.can_undo();
    epos pos = tf_rand_below(tf, d.length()), del = emin<epos>(1000, d.length() - pos);
    EolCounts ec = d.eol_counts();
    int rc;
    doc_bytes(d, &before);
    cs = d.change_seq();
    e_test_fail_after(k);
    rc = d.replace(pos, del, big, k * 5000);
    e_test_fail_after(-1);
    if (rc != E_OK) {
      TF_EQ_I(rc, E_NOMEM);
      doc_bytes(d, &after);
      TF_CHECK(after.size() == before.size() && memcmp(after.data(), before.data(), before.size()) == 0);
      TF_EQ_U(d.undo_seq(), sq);
      TF_CHECK(d.can_undo() == cu);
      TF_EQ_U(d.change_seq(), cs);
      TF_CHECK(d.eol_counts().crlf == ec.crlf && d.eol_counts().lf == ec.lf && d.eol_counts().cr == ec.cr);
      if (!doc_check(tf, d, "oom replace", k)) break;
    } else {
      ok++;
      // OOM during undo of what just succeeded: unchanged or undone, never half
      doc_bytes(d, &after);
      e_test_fail_after(1 + k % 3);
      rc = d.undo(&s);
      e_test_fail_after(-1);
      if (rc == E_OK) TF_CHECK(d.length() == before.size());
      else { TF_EQ_I(rc, E_NOMEM); TF_EQ_U(d.length(), after.size()); TF_CHECK(d.can_undo()); }
      if (!doc_check(tf, d, "oom undo", k)) break;
      if (d.length() > 200000) d.replace(0, 100000, 0, 0);
    }
  }
  TF_CHECK(ok > 0);
}

// ---------------------------------------------------------------------------------------------------------------
// 3.9 GB synthetic document

static void test_huge(TF* tf)
{
  const epos size = 0xF9FFFF00u;
  Document d;
  SynthSource* syn = new SynthSource(size, 3, SYN_TEXT_CRLF);
  Vec<u32> counts;
  u32 nblocks = size / 65536 + 1, i, tail = 0, total, L;
  epos j, last = (nblocks - 1) * (epos)65536, p;
  EolCounts ec;
  u8 buf[8];
  Selection s;
  TF_EQ_I(d.init_source_indexing(syn, enc_ansi(), 0, EOL_CRLF), E_OK);
  counts.resize(nblocks);
  for (i = 0; i + 1 < nblocks; i++) counts[i] = 2048;
  for (j = last; j < size; j++) if (SynthSource::byte_at(j, 3, SYN_TEXT_CRLF) == 0x0A) tail++;
  counts[nblocks - 1] = tail;
  total = (nblocks - 1) * 2048 + tail;
  ec.crlf = total; ec.lf = 0; ec.cr = 0;
  TF_EQ_I(d.finish_indexing(counts.data(), nblocks, ec), E_OK);
  TF_EQ_U(d.line_count(), total + 1);
  // around 2^31: line of a position, start/end with CRLF, an edit that adds a line
  p = 0x80000000u + 5;
  L = d.line_of(p);
  TF_CHECK(d.line_start(L) <= p && p < d.line_next(L));
  TF_EQ_U(d.line_next(L) - d.line_end(L), 2);        // CRLF
  TF_EQ_I(d.replace(p, 0, (const u8*)"\r\n", 2), E_OK);
  TF_EQ_U(d.line_count(), total + 2);
  TF_EQ_U(d.line_end(L), p);
  TF_EQ_U(d.line_start(L + 1), p + 2);
  TF_EQ_U(d.eol_counts().crlf, total + 1);
  TF_EQ_I(d.undo(&s), E_OK);
  TF_EQ_U(d.line_count(), total + 1);
  // delete the very end and type at the end
  TF_EQ_I(d.replace(size - 10, 10, (const u8*)"\nEND", 4), E_OK);
  TF_EQ_U(d.length(), size - 6);
  TF_EQ_I(d.read(size - 10, buf, 4), E_OK);
  TF_EQ_MEM(buf, "\nEND", 4);
  TF_EQ_U(d.line_start(d.line_count() - 1), size - 9);
  TF_EQ_U(d.line_of(size - 7), d.line_count() - 1);
  TF_EQ_I(d.tree()->check(0, 0), 0);
  // the last 10 bytes held exactly one CRLF (record end); the new tail adds one bare LF
  TF_EQ_U(SynthSource::byte_at(size - 2, 3, SYN_TEXT_CRLF), 0x0D);
  TF_EQ_U(SynthSource::byte_at(size - 1, 3, SYN_TEXT_CRLF), 0x0A);
  TF_CHECK(SynthSource::byte_at(size - 11, 3, SYN_TEXT_CRLF) >= ' ');
  TF_EQ_U(d.eol_counts().crlf, total - 1);
  TF_EQ_U(d.eol_counts().lf, 1);
  TF_EQ_U(d.eol_counts().cr, 0);
}

void suite_doc(TF* tf)
{
  u32 ops = tf->long_run ? 2000000 : 200000;
  enc_set_system(1252, 437);
  codec_init();
  test_init(tf);
  test_lines(tf);
  test_chars(tf);
  test_replace_text(tf);
  test_states(tf);
  test_undo_details(tf);
  test_listeners(tf);
  test_convert(tf);
  test_convert_eol(tf);
  test_reinterpret(tf);
  test_reload(tf);
  test_indexing(tf);
  test_snapshot(tf);
  test_oom(tf);
  test_huge(tf);
  random_run(tf, enc_ansi(), EOL_CRLF, ops, "random ansi");
  random_run(tf, enc_utf16le(0), EOL_CRLF, ops, "random utf16le");
  random_run(tf, enc_utf8(0), EOL_CR, ops / 4, "random cr mode");
  random_run(tf, enc_utf16be(0), EOL_LF, ops / 4, "random utf16be");
}
