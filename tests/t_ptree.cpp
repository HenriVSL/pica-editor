// t_ptree.cpp - suite "ptree": AddBuffer, PieceTree, SnapshotSource, SynthSource (DESIGN 6.4, 6.5, 17.1). Owner: T01.
#include "tf.h"
#include "ptree.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// ---- helpers ----

static BreakSpec mk_bs(u32 unit, u8 b0, u8 b1)
{
  BreakSpec bs;
  bs.unit = (u8)unit; bs.b0 = b0; bs.b1 = b1; bs.pad = 0;
  return bs;
}

// brute-force break offsets of a flat buffer
static u32 bf_count(const u8* p, u32 n, const BreakSpec& bs)
{
  u32 i, c = 0;
  if (bs.unit == 1) { for (i = 0; i < n; i++) if (p[i] == bs.b0) c++; }
  else for (i = 0; i + 1 < n; i += 2) if (p[i] == bs.b0 && p[i + 1] == bs.b1) c++;
  return c;
}

static epos bf_break_pos(const u8* p, u32 n, const BreakSpec& bs, u32 k)
{
  u32 i;
  if (bs.unit == 1) { for (i = 0; i < n; i++) if (p[i] == bs.b0 && k-- == 0) return i; }
  else for (i = 0; i + 1 < n; i += 2) if (p[i] == bs.b0 && p[i + 1] == bs.b1 && k-- == 0) return i;
  return EPOS_NONE;
}

static u32 bf_breaks_before(const u8* p, u32 n, const BreakSpec& bs, epos pos)
{
  u32 i, c = 0;
  if (pos > n) pos = n;
  if (bs.unit == 1) { for (i = 0; i < pos; i++) if (p[i] == bs.b0) c++; }
  else for (i = 0; i + 2 <= pos; i += 2) if (p[i] == bs.b0 && p[i + 1] == bs.b1) c++;
  return c;
}

static int tree_check(TF* tf, PieceTree& t, const char* where, u32 step)
{
  char why[200];
  if (t.check(why, sizeof(why)) != 0) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: check: %s", where, step, why);
    return 0;
  }
  return 1;
}

static int tree_eq(TF* tf, PieceTree& t, const EBytes& m, const char* where, u32 step)
{
  static u8 buf[4096];
  epos off = 0, n = m.size();
  if (t.length() != n) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: length %u, model %u", where, step, t.length(), n);
    return 0;
  }
  while (off < n) {
    epos k = n - off;
    if (k > sizeof(buf)) k = sizeof(buf);
    if (t.read(off, buf, k) != E_OK || memcmp(buf, m.data() + off, k) != 0) {
      tf_fail(tf, __FILE__, __LINE__, "%s step %u: content differs in [%u, %u)", where, step, off, off + k);
      return 0;
    }
    off += k;
  }
  return 1;
}

static int lines_eq(TF* tf, PieceTree& t, const EBytes& m, const char* where, u32 step, u32 samples)
{
  const BreakSpec& bs = t.break_spec();
  u32 nb = bf_count(m.data(), m.size(), bs), i;
  if (t.break_count() != nb) {
    tf_fail(tf, __FILE__, __LINE__, "%s step %u: break_count %u, brute force %u", where, step, t.break_count(), nb);
    return 0;
  }
  for (i = 0; i < samples && nb; i++) {
    u32 k = tf_rand_below(tf, nb);
    epos bp = t.break_pos(k), bb = bf_break_pos(m.data(), m.size(), bs, k);
    if (bp != bb) { tf_fail(tf, __FILE__, __LINE__, "%s step %u: break_pos(%u) %u, bf %u", where, step, k, bp, bb); return 0; }
  }
  if (t.break_pos(nb) != EPOS_NONE) { tf_fail(tf, __FILE__, __LINE__, "%s: break_pos(count) not NONE", where); return 0; }
  for (i = 0; i < samples; i++) {
    epos p = tf_rand_below(tf, m.size() + 1);
    u32 a = t.breaks_before(p), b = bf_breaks_before(m.data(), m.size(), bs, p);
    if (a != b) { tf_fail(tf, __FILE__, __LINE__, "%s step %u: breaks_before(%u) %u, bf %u", where, step, p, a, b); return 0; }
  }
  return 1;
}

// A source that hands out short, odd-sized spans (like a block cache at unaligned piece starts): exercises every
// "unit straddles two get() calls" path.
class ChoppySource : public ByteSource {
public:
  ChoppySource(const u8* p, epos n) : p_(p), n_(n), k_(1) {}
  epos size() const { return n_; }
  const u8* get(epos off, epos* avail)
  {
    epos a;
    if (off >= n_) { *avail = 0; return 0; }
    k_ = k_ * 1103515245u + 12345u;
    a = 1 + (k_ >> 16) % 7;
    if (a > n_ - off) a = n_ - off;
    *avail = a;
    return p_ + off;
  }
private:
  const u8* p_; epos n_; u32 k_;
};

static void model_insert(EBytes& m, epos pos, const u8* p, epos n) { m.insert(pos, p, n); }
static void model_remove(EBytes& m, epos pos, epos n) { m.erase(pos, n); }

static void rand_bytes(TF* tf, u8* p, u32 n, u32 unit)
{
  static const u8 alpha[] = "abcdefgh\n\n\r ";
  u32 i;
  if (unit == 1) { for (i = 0; i < n; i++) p[i] = alpha[tf_rand_below(tf, sizeof(alpha) - 1)]; return; }
  for (i = 0; i + 1 < n; i += 2) {
    u32 r = tf_rand_below(tf, 16);
    if (r < 2) { p[i] = 0x0A; p[i + 1] = 0; }
    else if (r == 2) { p[i] = 0x0D; p[i + 1] = 0; }
    else if (r == 3) { p[i] = 0; p[i + 1] = 0x0A; }        // misaligned-looking pair: never a break at even offsets
    else if (r == 4) { p[i] = 0x0A; p[i + 1] = 0x0A; }
    else { p[i] = (u8)('a' + r); p[i + 1] = 0; }
  }
  if (n & 1) p[n - 1] = 0x0A;
}

// ---- AddBuffer ----

static void test_addbuffer(TF* tf)
{
  AddBuffer ab;
  PieceVec refs;
  static u8 big[PT_ADD_CHUNK + 70000];
  u32 i, total;
  for (i = 0; i < sizeof(big); i++) big[i] = (u8)(i * 7);
  TF_EQ_I(ab.append((const u8*)"abc", 3, &refs), E_OK);
  TF_EQ_U(refs.size(), 1);
  TF_EQ_U(refs[0].src, 1);
  TF_EQ_U(refs[0].start, 0);
  TF_EQ_U(refs[0].len, 3);
  TF_EQ_MEM(ab.chunk(1), "abc", 3);
  refs.clear();
  // aligned append after an odd fill starts at an even offset
  TF_EQ_I(ab.append_aligned((const u8*)"xy", 2, 2, &refs), E_OK);
  TF_EQ_U(refs.size(), 1);
  TF_EQ_U(refs[0].start, 4);
  TF_EQ_U(ab.chunk_fill(1), 6);
  refs.clear();
  // big append: crosses into a new chunk, pieces <= PT_PIECE_MAX, none crossing a chunk end
  TF_EQ_I(ab.append(big, sizeof(big), &refs), E_OK);
  total = 0;
  for (i = 0; i < refs.size(); i++) {
    TF_CHECK(refs[i].len >= 1 && refs[i].len <= PT_PIECE_MAX);
    TF_CHECK(refs[i].start + refs[i].len <= PT_ADD_CHUNK);
    TF_CHECK(memcmp(ab.chunk(refs[i].src) + refs[i].start, big + total, refs[i].len) == 0);
    total += refs[i].len;
  }
  TF_EQ_U(total, sizeof(big));
  TF_EQ_U(ab.chunk_count(), 2);
  TF_EQ_U(ab.chunk_fill(1), PT_ADD_CHUNK);
  TF_EQ_U(ab.chunk_fill(2), sizeof(big) - (PT_ADD_CHUNK - 6));
  TF_CHECK(ab.chunk(3) == 0);
  TF_CHECK(ab.chunk(0) == 0);
  // OOM inside append leaves the buffer unchanged
  {
    u32 used = ab.bytes_used(), nch = ab.chunk_count(), k;
    for (k = 1; k <= 3; k++) {
      int rc;
      refs.clear();
      e_test_fail_after(k);
      rc = ab.append(big, sizeof(big), &refs);
      e_test_fail_after(-1);
      if (rc == E_OK) break;
      TF_EQ_I(rc, E_NOMEM);
      TF_EQ_U(ab.bytes_used(), used);
      TF_EQ_U(ab.chunk_count(), nch);
    }
  }
  ab.clear();
  TF_EQ_U(ab.bytes_used(), 0);
  TF_EQ_U(ab.chunk_count(), 0);
}

// ---- basic tree ----

static void test_basic(TF* tf)
{
  static const char text[] = "line one\nline two\r\nthree\n\nlast";
  MemSource src;
  PieceTree t;
  PieceVec removed, added;
  PSpan sp;
  u8 buf[64];
  EStr dump;
  BreakSpec lf = mk_bs(1, 0x0A, 0);
  TF_EQ_I(src.copy_from((const u8*)text, sizeof(text) - 1), E_OK);
  TF_EQ_I(t.init(&src, lf, 0), E_OK);
  TF_EQ_U(t.length(), sizeof(text) - 1);
  TF_EQ_U(t.break_count(), 4);
  TF_EQ_U(t.piece_count(), 1);
  TF_EQ_U(t.break_pos(0), 8);
  TF_EQ_U(t.break_pos(1), 18);
  TF_EQ_U(t.break_pos(3), 25);
  TF_EQ_U(t.break_pos(4), EPOS_NONE);
  TF_EQ_U(t.breaks_before(8), 0);
  TF_EQ_U(t.breaks_before(9), 1);
  TF_EQ_U(t.breaks_before(t.length()), 4);
  TF_EQ_I(t.span(3, &sp), E_OK);
  TF_EQ_U(sp.len, sizeof(text) - 1 - 3);
  TF_EQ_U(sp.src, 0);
  TF_EQ_I(t.span(t.length(), &sp), E_RANGE);
  TF_EQ_I(t.insert(5, (const u8*)"NEW\n", 4, &added), E_OK);
  TF_EQ_U(t.length(), sizeof(text) - 1 + 4);
  TF_EQ_U(t.break_count(), 5);
  TF_EQ_U(added.size(), 1);
  TF_EQ_U(added[0].nbreaks, 1);
  TF_EQ_I(t.read(0, buf, 14), E_OK);
  TF_EQ_MEM(buf, "line NEW\none\nl", 14);
  TF_EQ_I(t.span(5, &sp), E_OK);
  TF_CHECK(sp.src != 0);
  TF_EQ_U(sp.len, 4);
  TF_EQ_I(t.remove(2, 10, &removed), E_OK);
  TF_EQ_I(t.read(0, buf, 6), E_OK);
  TF_EQ_MEM(buf, "li\nlin", 6);
  TF_EQ_I(t.insert_refs(2, removed.data(), removed.size()), E_OK);
  TF_EQ_I(t.read(0, buf, 14), E_OK);
  TF_EQ_MEM(buf, "line NEW\none\nl", 14);
  TF_EQ_I(t.check(0, 0), 0);
  TF_EQ_I(t.remove(0, t.length() + 1, 0), E_RANGE);
  TF_EQ_I(t.insert(t.length() + 1, buf, 1, 0), E_RANGE);
  TF_EQ_I(t.read(t.length() - 1, buf, 2), E_RANGE);
  // coalescing of consecutive typing
  {
    u32 pc = t.piece_count(), i;
    for (i = 0; i < 20; i++) TF_EQ_I(t.insert(t.length(), (const u8*)"z", 1, 0), E_OK);
    TF_EQ_U(t.piece_count(), pc + 1);
    for (i = 0; i < 70000; i++) if (t.insert(10 + i, (const u8*)"\n", 1, 0) != E_OK) break;   // typing past the cap
    TF_EQ_U(i, 70000);
    TF_EQ_I(t.check(0, 0), 0);
    TF_CHECK(t.piece_count() >= pc + 3);
  }
  t.dump(&dump);
  TF_CHECK(dump.len() > 0);
  TF_CHECK(strstr(dump.c_str(), "PieceTree: length") != 0);
  // empty tree
  {
    PieceTree e;
    MemSource es;
    TF_EQ_I(e.init(&es, lf, 0), E_OK);
    TF_EQ_U(e.length(), 0);
    TF_EQ_U(e.break_count(), 0);
    TF_EQ_U(e.breaks_before(0), 0);
    TF_EQ_U(e.break_pos(0), EPOS_NONE);
    TF_EQ_I(e.check(0, 0), 0);
    TF_EQ_I(e.insert(0, (const u8*)"\n", 1, 0), E_OK);
    TF_EQ_U(e.break_count(), 1);
    TF_EQ_I(e.remove(0, 1, 0), E_OK);
    TF_EQ_U(e.length(), 0);
    TF_EQ_I(e.check(0, 0), 0);
  }
}

// ---- differential random test ----

static void random_run(TF* tf, u32 unit, u32 ops, const char* name, int choppy)
{
  EBytes m, orig;
  MemSource* msrc = new MemSource();
  ByteSource* src = msrc;
  PieceTree t;
  BreakSpec bs = unit == 1 ? mk_bs(1, 0x0A, 0) : mk_bs(2, 0x0A, 0);
  Vec<PieceRef> stash;          // recently removed pieces, re-inserted later (undo-style)
  epos stash_pos = 0, stash_len = 0;
  u8 buf[3000];
  u32 step;
  int ok = 1;
  {
    u32 n0 = unit == 2 ? 1501 : 1500;                 // unit 2 starts with an odd trailing byte
    rand_bytes(tf, buf, n0, unit);
    msrc->copy_from(buf, n0);
    m.append(buf, n0);
    if (choppy) { orig.append(buf, n0); src = new ChoppySource(orig.data(), n0); }
  }
  TF_EQ_I(t.init(src, bs, 0), E_OK);
  for (step = 0; step < ops && ok; step++) {
    u32 r = tf_rand_below(tf, 100);
    epos len = m.size();
    if (r < 45 || len < 64) {                          // insert
      epos pos = tf_rand_below(tf, len + 1), n;
      u32 sz = tf_rand_below(tf, 8) == 0 ? 400 : 12;
      n = 1 + tf_rand_below(tf, sz);
      if (unit == 2) { pos &= ~1u; n = (n + 1) & ~1u; }   // len may be odd: pos <= len - 1 then
      rand_bytes(tf, buf, n, unit);
      if (t.insert(pos, buf, n, 0) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "insert failed"); break; }
      model_insert(m, pos, buf, n);
    } else if (r < 85) {                              // remove (sometimes stash the pieces)
      epos pos = tf_rand_below(tf, len), n = 1 + tf_rand_below(tf, tf_rand_below(tf, 10) == 0 ? 600 : 16);
      PieceVec rem;
      if (unit == 2) { pos &= ~1u; n = (n + 1) & ~1u; }
      if (n > len - pos) n = len - pos;
      if (unit == 2 && (n & 1) && pos + n != len) n--;
      if (n == 0) continue;
      if (t.remove(pos, n, &rem) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "remove failed"); break; }
      {
        epos sum = 0;
        u32 i;
        for (i = 0; i < rem.size(); i++) sum += rem[i].len;
        if (sum != n) { tf_fail(tf, __FILE__, __LINE__, "removed refs cover %u of %u bytes", sum, n); ok = 0; }
      }
      if (tf_rand_below(tf, 3) == 0 && !(unit == 2 && (n & 1))) {
        stash.clear();
        stash.insert(0, rem.data(), rem.size());
        stash_pos = pos; stash_len = n;
        // the model keeps the bytes in a side buffer so they can be re-inserted anywhere
        {
          static u8 side[700];
          memcpy(side, m.data() + pos, n);
          model_remove(m, pos, n);
          if (tf_rand_below(tf, 2) == 0) {          // put them straight back somewhere (undo / move)
            epos p2 = tf_rand_below(tf, m.size() + 1);
            if (unit == 2) p2 &= ~1u;
            if (unit == 2 && (m.size() & 1) && p2 == m.size()) p2 = 0;
            if (t.insert_refs(p2, stash.data(), stash.size()) != E_OK) {
              tf_fail(tf, __FILE__, __LINE__, "insert_refs failed"); break;
            }
            model_insert(m, p2, side, n);
            stash.clear();
          }
        }
      } else model_remove(m, pos, n);
    } else if (r < 92) {                              // read a random range
      epos pos = len ? tf_rand_below(tf, len) : 0, n = tf_rand_below(tf, 200);
      if (n > len - pos) n = len - pos;
      if (t.read(pos, buf, n) != E_OK || memcmp(buf, m.data() + pos, n) != 0) {
        tf_fail(tf, __FILE__, __LINE__, "%s step %u: read mismatch", name, step); break;
      }
    } else {                                          // line queries
      if (!lines_eq(tf, t, m, name, step, 4)) break;
    }
    if (!tree_check(tf, t, name, step)) break;
    if ((step & 15) == 0 && !tree_eq(tf, t, m, name, step)) break;
    if (m.size() > 2600) {                            // keep the document small: trim a random chunk
      epos pos = tf_rand_below(tf, 800) & ~1u, n = 1000;
      if (t.remove(pos, n, 0) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "trim failed"); break; }
      model_remove(m, pos, n);
    }
  }
  (void)stash_pos; (void)stash_len;
  TF_CHECK(step == ops);
  TF_CHECK(tree_eq(tf, t, m, name, step));
  TF_CHECK(lines_eq(tf, t, m, name, step, 50));
  t.clear();
  if (src != msrc) delete src;
  delete msrc;
}

// larger pieces: PT_PIECE_MAX splits, chunk crossings, multi-piece removes
static void big_run(TF* tf)
{
  EBytes m;
  MemSource src;
  PieceTree t;
  BreakSpec bs = mk_bs(1, 0x0A, 0);
  static u8 buf[300000];
  u32 step;
  rand_bytes(tf, buf, 200000, 1);
  src.copy_from(buf, 200000);
  m.append(buf, 200000);
  TF_EQ_I(t.init(&src, bs, 0), E_OK);
  TF_EQ_U(t.piece_count(), 4);
  for (step = 0; step < 300; step++) {
    epos len = m.size();
    if (tf_rand_below(tf, 2) == 0 || len < 1000) {
      epos pos = tf_rand_below(tf, len + 1), n = 1 + tf_rand_below(tf, sizeof(buf));
      rand_bytes(tf, buf, n, 1);
      TF_EQ_I(t.insert(pos, buf, n, 0), E_OK);
      model_insert(m, pos, buf, n);
    } else {
      epos pos = tf_rand_below(tf, len), n = 1 + tf_rand_below(tf, 250000);
      if (n > len - pos) n = len - pos;
      TF_EQ_I(t.remove(pos, n, 0), E_OK);
      model_remove(m, pos, n);
    }
    if (!tree_check(tf, t, "big", step)) break;
    if (m.size() > 3000000) { TF_EQ_I(t.remove(0, 2000000, 0), E_OK); model_remove(m, 0, 2000000); }
  }
  TF_CHECK(tree_eq(tf, t, m, "big", step));
  TF_CHECK(lines_eq(tf, t, m, "big", step, 200));
}

// ---- break specs ----

static void test_break_specs(TF* tf)
{
  EBytes m;
  MemSource src;
  PieceTree t;
  u8 buf[512];
  u32 i;
  // mixed content, several odd-length pieces (unit 1 edits)
  rand_bytes(tf, buf, 301, 1);
  src.copy_from(buf, 301);
  m.append(buf, 301);
  TF_EQ_I(t.init(&src, mk_bs(1, 0x0A, 0), 0), E_OK);
  for (i = 0; i < 40; i++) {
    epos pos = tf_rand_below(tf, m.size() + 1), n = 1 + tf_rand_below(tf, 7);
    rand_bytes(tf, buf, n, 1);
    TF_EQ_I(t.insert(pos, buf, n, 0), E_OK);
    model_insert(m, pos, buf, n);
  }
  TF_CHECK(tree_check(tf, t, "spec lf", 0));
  TF_CHECK(lines_eq(tf, t, m, "spec lf", 0, 40));
  TF_EQ_I(t.set_break_spec(mk_bs(1, 0x0D, 0), 0), E_OK);            // CR mode
  TF_CHECK(tree_check(tf, t, "spec cr", 0));
  TF_CHECK(lines_eq(tf, t, m, "spec cr", 0, 40));
  TF_EQ_I(t.set_break_spec(mk_bs(2, 0x0A, 0), 0), E_OK);            // reinterpret as UTF-16 LE: realigns
  TF_CHECK(tree_check(tf, t, "spec u16le", 0));
  TF_CHECK(tree_eq(tf, t, m, "spec u16le", 0));
  TF_CHECK(lines_eq(tf, t, m, "spec u16le", 0, 40));
  // misaligned edits in unit-2 mode (undo across a reinterpret) take the slow path and stay exact
  for (i = 0; i < 30; i++) {
    epos pos = tf_rand_below(tf, m.size() + 1), n = 1 + tf_rand_below(tf, 5);
    if (i & 1) {
      rand_bytes(tf, buf, n, 1);
      TF_EQ_I(t.insert(pos, buf, n, 0), E_OK);
      model_insert(m, pos, buf, n);
    } else if (m.size() > 10) {
      if (pos >= m.size()) pos = m.size() - 1;
      if (n > m.size() - pos) n = m.size() - pos;
      TF_EQ_I(t.remove(pos, n, 0), E_OK);
      model_remove(m, pos, n);
    }
    if (!tree_check(tf, t, "slow u16", i)) break;
  }
  TF_CHECK(tree_eq(tf, t, m, "slow u16", 0));
  TF_CHECK(lines_eq(tf, t, m, "slow u16", 0, 40));
  TF_EQ_I(t.set_break_spec(mk_bs(2, 0, 0x0A), 0), E_OK);            // UTF-16 BE
  TF_CHECK(tree_check(tf, t, "spec u16be", 0));
  TF_CHECK(lines_eq(tf, t, m, "spec u16be", 0, 40));
  TF_EQ_I(t.set_break_spec(mk_bs(1, 0x0A, 0), 0), E_OK);
  TF_CHECK(tree_check(tf, t, "spec back", 0));
  TF_CHECK(lines_eq(tf, t, m, "spec back", 0, 40));
  // refs counted for another spec are recounted on insert_refs
  {
    PieceVec rem;
    epos n = m.size() / 2;
    static u8 side[4096];
    memcpy(side, m.data(), n);
    TF_EQ_I(t.remove(0, n, &rem), E_OK);
    model_remove(m, 0, n);
    TF_EQ_I(t.set_break_spec(mk_bs(1, 0x0D, 0), 0), E_OK);
    TF_EQ_I(t.insert_refs(0, rem.data(), rem.size()), E_OK);
    model_insert(m, 0, side, n);
    TF_CHECK(tree_check(tf, t, "stale refs", 0));
    TF_CHECK(lines_eq(tf, t, m, "stale refs", 0, 40));
  }
}

// ---- indexing: init_uncounted + set_counts ----

static void test_uncounted(TF* tf)
{
  SynthSource syn(65536 * 5 + 1234, 7, SYN_TEXT_LF);
  PieceTree t;
  u32 counts[6], i;
  epos off;
  TF_EQ_I(t.init_uncounted(&syn, mk_bs(1, 0x0A, 0)), E_OK);
  TF_EQ_U(t.piece_count(), 6);
  TF_EQ_U(t.break_count(), 0);
  for (i = 0, off = 0; i < 6; i++, off += 65536) {
    epos n = syn.size() - off, j;
    u32 c = 0;
    if (n > 65536) n = 65536;
    for (j = 0; j < n; j++) if (SynthSource::byte_at(off + j, 7, SYN_TEXT_LF) == 0x0A) c++;
    counts[i] = c;
    if (i < 5) TF_EQ_U(c, 2048);                     // formula: two lines per 64-byte record
  }
  TF_EQ_I(t.set_counts(counts, 5), E_STATE);
  TF_EQ_I(t.set_counts(counts, 6), E_OK);
  TF_EQ_I(t.check(0, 0), 0);
  TF_EQ_U(t.break_count(), 5 * 2048 + counts[5]);
  TF_EQ_U(t.breaks_before(65536), 2048);
}

// ---- SnapshotSource ----

static void test_snapshot(TF* tf)
{
  EBytes m, frozen;
  MemSource src;
  PieceTree t;
  PieceVec list;
  SnapshotSource snap;
  u8 buf[1000];
  u32 i;
  rand_bytes(tf, buf, 1000, 1);
  src.copy_from(buf, 1000);
  m.append(buf, 1000);
  TF_EQ_I(t.init(&src, mk_bs(1, 0x0A, 0), 0), E_OK);
  for (i = 0; i < 50; i++) {
    epos pos = tf_rand_below(tf, m.size() + 1);
    rand_bytes(tf, buf, 9, 1);
    t.insert(pos, buf, 9, 0);
    model_insert(m, pos, buf, 9);
  }
  TF_EQ_I(t.snapshot(&list), E_OK);
  TF_EQ_U(list.size(), t.piece_count());
  TF_EQ_I(snap.init(list, t.add_buffer(), &src), E_OK);
  frozen.append(m.data(), m.size());
  // keep editing; the snapshot still reads the frozen bytes
  for (i = 0; i < 300; i++) {
    epos pos = tf_rand_below(tf, m.size() + 1), n;
    if (i & 1) {
      rand_bytes(tf, buf, 30, 1);
      t.insert(pos, buf, 30, 0);
      model_insert(m, pos, buf, 30);
    } else if (m.size() > 100) {
      n = 1 + tf_rand_below(tf, 40);
      if (pos + n > m.size()) pos = m.size() - n;
      t.remove(pos, n, 0);
      model_remove(m, pos, n);
    }
  }
  TF_CHECK(tree_eq(tf, t, m, "snapshot live", 0));
  TF_EQ_U(snap.size(), frozen.size());
  {
    static u8 out[200000];
    TF_EQ_I(snap.read(0, out, snap.size()), E_OK);
    TF_EQ_MEM(out, frozen.data(), frozen.size());
    // random-access reads (binary search path)
    for (i = 0; i < 200; i++) {
      epos p = tf_rand_below(tf, frozen.size()), n = tf_rand_below(tf, 64);
      if (n > frozen.size() - p) n = frozen.size() - p;
      TF_EQ_I(snap.read(p, out, n), E_OK);
      if (memcmp(out, frozen.data() + p, n) != 0) { TF_CHECK(0); break; }
    }
  }
  TF_EQ_I(snap.err(), E_OK);
  {
    epos av = 1;
    TF_CHECK(snap.get(snap.size(), &av) == 0);
    TF_EQ_U(av, 0);
  }
}

// ---- OOM inside insert/remove ----

static void test_oom(TF* tf)
{
  EBytes m;
  MemSource src;
  PieceTree t;
  static u8 buf[200000];
  u32 k, ok_ins = 0, ok_rem = 0;
  rand_bytes(tf, buf, 5000, 1);
  src.copy_from(buf, 5000);
  m.append(buf, 5000);
  TF_EQ_I(t.init(&src, mk_bs(1, 0x0A, 0), 0), E_OK);
  for (k = 0; k < 30; k++) {                          // fragment a bit
    epos pos = tf_rand_below(tf, m.size());
    t.insert(pos, buf, 3, 0);
    model_insert(m, pos, buf, 3);
  }
  for (k = 1; k <= 40; k++) {
    PieceVec added, removed;
    int rc;
    epos pos = tf_rand_below(tf, m.size());
    rand_bytes(tf, buf, sizeof(buf), 1);
    e_test_fail_after(k);
    rc = t.insert(pos, buf, sizeof(buf), &added);
    e_test_fail_after(-1);
    if (rc == E_OK) { model_insert(m, pos, buf, sizeof(buf)); ok_ins++; }
    else TF_EQ_I(rc, E_NOMEM);
    if (!tree_check(tf, t, "oom insert", k) || !tree_eq(tf, t, m, "oom insert", k)) break;
    pos = tf_rand_below(tf, m.size() / 2);
    e_test_fail_after(k);
    rc = t.remove(pos, m.size() / 3, &removed);
    e_test_fail_after(-1);
    if (rc == E_OK) { model_remove(m, pos, m.size() / 3); ok_rem++; }
    else TF_EQ_I(rc, E_NOMEM);
    if (!tree_check(tf, t, "oom remove", k) || !tree_eq(tf, t, m, "oom remove", k)) break;
    if (m.size() > 1000000) { t.remove(0, 500000, 0); model_remove(m, 0, 500000); }
  }
  TF_CHECK(ok_ins > 0);
  TF_CHECK(ok_rem > 0);
  // OOM in set_break_spec (rebuild) leaves the tree as it was
  for (k = 1; k <= 12; k++) {
    int rc;
    e_test_fail_after(k);
    rc = t.set_break_spec(mk_bs(2, 0x0A, 0), 0);
    e_test_fail_after(-1);
    if (rc == E_OK) break;
    TF_EQ_I(rc, E_NOMEM);
    TF_EQ_U(t.break_spec().unit, 1);
    if (!tree_check(tf, t, "oom spec", k)) break;
  }
  TF_CHECK(tree_check(tf, t, "oom spec done", 0));
  TF_CHECK(tree_eq(tf, t, m, "oom spec done", 0));
}

// ---- 3.9 GB synthetic document: offsets near 2^31 and 2^32 - 2 ----

static void test_huge(TF* tf)
{
  const epos size = 0xF9FFFF00u;                    // ~3.9 GB, not a multiple of 64 KB
  const u32 seed = 3;
  SynthSource syn(size, seed, SYN_TEXT_CRLF);
  PieceTree t;
  Vec<u32> counts;
  u32 nblocks = size / 65536 + 1, i, tail = 0;
  epos last = (nblocks - 1) * (epos)65536, j;
  u8 buf[64], ref[64];
  u32 total;
  TF_EQ_I(t.init_uncounted(&syn, mk_bs(1, 0x0A, 0)), E_OK);
  TF_EQ_U(t.piece_count(), nblocks);
  TF_EQ_I(counts.resize(nblocks), E_OK);
  for (i = 0; i + 1 < nblocks; i++) counts[i] = 2048;
  for (j = last; j < size; j++) if (SynthSource::byte_at(j, seed, SYN_TEXT_CRLF) == 0x0A) tail++;
  counts[nblocks - 1] = tail;
  TF_EQ_I(t.set_counts(counts.data(), nblocks), E_OK);
  total = (nblocks - 1) * 2048 + tail;
  TF_EQ_U(t.break_count(), total);
  TF_EQ_U(t.length(), size);
  TF_EQ_I(t.check(0, 0), 0);
  // break positions follow the record formula: 2 breaks per 64 bytes
  {
    u32 b = 0x2000000u * 2 + 1;                       // second break of record 0x2000000 (offset 2^31)
    epos bp = t.break_pos(b), rec = 0x80000000u;
    TF_CHECK(bp > rec && bp < rec + 64);
    TF_EQ_U(SynthSource::byte_at(bp, seed, SYN_TEXT_CRLF), 0x0A);
    TF_EQ_U(t.breaks_before(rec), 0x4000000u);
    TF_EQ_U(t.breaks_before(size), total);
    TF_EQ_U(t.breaks_before(size - 1), total - (SynthSource::byte_at(size - 1, seed, SYN_TEXT_CRLF) == 0x0A ? 1 : 0));
  }
  // edits near 2^31
  {
    epos p = 0x80000000u - 3;
    TF_EQ_I(t.insert(p, (const u8*)"\nXY\n", 4, 0), E_OK);
    TF_EQ_U(t.length(), size + 4);
    TF_EQ_U(t.break_count(), total + 2);
    TF_EQ_I(t.read(p - 2, buf, 10), E_OK);
    for (j = 0; j < 2; j++) ref[j] = SynthSource::byte_at(p - 2 + j, seed, SYN_TEXT_CRLF);
    memcpy(ref + 2, "\nXY\n", 4);
    for (j = 0; j < 4; j++) ref[6 + j] = SynthSource::byte_at(p + j, seed, SYN_TEXT_CRLF);
    TF_EQ_MEM(buf, ref, 10);
    TF_EQ_I(t.remove(p, 4, 0), E_OK);
    TF_EQ_U(t.length(), size);
    TF_EQ_U(t.break_count(), total);
    TF_EQ_I(t.check(0, 0), 0);
  }
}

// a source of EPOS_MAX - 100 bytes, grown to exactly EPOS_MAX (2^32 - 2) and edited at the very end
static void test_huge_end(TF* tf)
{
  const epos size = EPOS_MAX - 100;
  const u32 seed = 9;
  SynthSource syn(size, seed, SYN_TEXT_LF);
  PieceTree t;
  Vec<u32> counts;
  u32 nblocks = size / 65536 + 1, i, tail = 0, total;
  epos last = (nblocks - 1) * (epos)65536, j;
  u8 buf[8], pad[100];
  TF_EQ_I(t.init_uncounted(&syn, mk_bs(1, 0x0A, 0)), E_OK);
  TF_EQ_I(counts.resize(nblocks), E_OK);
  for (i = 0; i + 1 < nblocks; i++) counts[i] = 2048;
  for (j = last; j < size; j++) if (SynthSource::byte_at(j, seed, SYN_TEXT_LF) == 0x0A) tail++;
  counts[nblocks - 1] = tail;
  TF_EQ_I(t.set_counts(counts.data(), nblocks), E_OK);
  total = (nblocks - 1) * 2048 + tail;
  memset(pad, 'p', sizeof(pad));
  TF_EQ_I(t.insert(t.length(), pad, 99, 0), E_OK);
  TF_EQ_I(t.insert(EPOS_MAX - 101, pad, 1, 0), E_OK);
  TF_EQ_U(t.length(), EPOS_MAX);
  TF_EQ_I(t.insert(t.length(), pad, 1, 0), E_TOOBIG);
  TF_EQ_I(t.insert(0, pad, 1, 0), E_TOOBIG);
  TF_EQ_I(t.remove(EPOS_MAX - 2, 2, 0), E_OK);
  TF_EQ_I(t.insert(EPOS_MAX - 2, (const u8*)"\n\n", 2, 0), E_OK);
  TF_EQ_U(t.length(), EPOS_MAX);
  TF_EQ_U(t.break_count(), total + 2);
  TF_EQ_U(t.break_pos(total + 1), EPOS_MAX - 1);
  TF_EQ_U(t.break_pos(total), EPOS_MAX - 2);
  TF_EQ_U(t.breaks_before(EPOS_MAX), total + 2);
  TF_EQ_U(t.breaks_before(EPOS_MAX - 1), total + 1);
  TF_EQ_U(t.breaks_before(EPOS_MAX - 2), total);
  TF_EQ_I(t.read(EPOS_MAX - 4, buf, 4), E_OK);
  TF_EQ_MEM(buf, "pp\n\n", 4);
  TF_EQ_I(t.read(EPOS_MAX - 1, buf, 2), E_RANGE);
  TF_EQ_I(t.read(EPOS_MAX - 1, buf, 1), E_OK);
  TF_EQ_I(t.remove(EPOS_MAX - 1, 2, 0), E_RANGE);
  TF_EQ_I(t.check(0, 0), 0);
  TF_EQ_I(t.remove(size, EPOS_MAX - size, 0), E_OK);
  TF_EQ_U(t.length(), size);
  TF_EQ_I(t.check(0, 0), 0);
}

// ---- SynthSource ----

static void test_synth(TF* tf)
{
  int kind;
  for (kind = SYN_TEXT_CRLF; kind <= SYN_UTF8; kind++) {
    SynthSource s(200000, 11, kind);
    u8 buf[256];
    epos off;
    u32 i, bad = 0;
    for (off = 0; off < 200000; off += 65000) {
      epos n = 200000 - off;
      if (n > sizeof(buf)) n = sizeof(buf);
      TF_EQ_I(s.read(off, buf, n), E_OK);
      for (i = 0; i < n; i++) if (buf[i] != SynthSource::byte_at(off + i, 11, kind)) bad++;
    }
    TF_EQ_U(bad, 0);
    if (kind == SYN_TEXT_CRLF) {                      // records end with CRLF; two lines each
      TF_EQ_U(SynthSource::byte_at(62, 11, kind), 0x0D);
      TF_EQ_U(SynthSource::byte_at(63, 11, kind), 0x0A);
    }
    if (kind == SYN_UTF8) TF_EQ_U(SynthSource::byte_at(63, 11, kind), 0x0A);
  }
  TF_CHECK(SynthSource::byte_at(5, 1, SYN_BINARY) != SynthSource::byte_at(5, 2, SYN_BINARY) ||
           SynthSource::byte_at(6, 1, SYN_BINARY) != SynthSource::byte_at(6, 2, SYN_BINARY));
  {
    SynthSource s(10, 1, SYN_TEXT_LF);
    epos av = 5;
    TF_CHECK(s.get(10, &av) == 0);
    TF_EQ_U(av, 0);
    TF_CHECK(s.get(9, &av) != 0);
    TF_EQ_U(av, 1);
  }
}

// ---- performance smoke ----

static u32 ms_since(clock_t c0)
{
  return (u32)((double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC);
}

static void test_perf(TF* tf)
{
  const epos size = 10 * 1024 * 1024;
  SynthSource syn(size, 5, SYN_TEXT_CRLF);
  MemSource mem;
  PieceTree t;
  u8* data = (u8*)e_valloc(size);
  u32 n = tf->long_run ? 1000000 : 100000, i, ms;
  clock_t c0;
  if (!data) { TF_CHECK(0); return; }
  TF_EQ_I(syn.read(0, data, size), E_OK);
  mem.adopt(data, size, 1);
  c0 = clock();
  TF_EQ_I(t.init(&mem, mk_bs(1, 0x0A, 0), 0), E_OK);
  tf_log(tf, "ptree perf: init 10 MB (count breaks): %u ms", ms_since(c0));
  c0 = clock();
  for (i = 0; i < n; i++) {
    u8 b = (u8)(i % 9 == 0 ? '\n' : 'a' + i % 26);
    if (t.insert(tf_rand_below(tf, t.length() + 1), &b, 1, 0) != E_OK) { TF_CHECK(0); break; }
  }
  ms = ms_since(c0);
  tf_log(tf, "ptree perf: %u single-byte inserts at random positions in 10 MB: %u ms (%u ns/op), %u pieces", n, ms,
         (u32)((double)ms * 1e6 / n), t.piece_count());
  c0 = clock();
  for (i = 0; i < 100000; i++) {
    u32 b = tf_rand_below(tf, t.break_count());
    if (t.break_pos(b) == EPOS_NONE) { TF_CHECK(0); break; }
  }
  tf_log(tf, "ptree perf: 100000 break_pos lookups: %u ms", ms_since(c0));
  c0 = clock();
  for (i = 0; i < 100000; i++) t.breaks_before(tf_rand_below(tf, t.length()));
  tf_log(tf, "ptree perf: 100000 breaks_before lookups: %u ms", ms_since(c0));
  TF_EQ_U(t.length(), size + n);
  TF_EQ_I(t.check(0, 0), 0);
}

void suite_ptree(TF* tf)
{
  u32 ops = tf->long_run ? 2000000 : 200000;
  test_addbuffer(tf);
  test_basic(tf);
  random_run(tf, 1, ops, "random unit1", 0);
  random_run(tf, 2, ops, "random unit2", 0);
  random_run(tf, 1, ops / 10, "random unit1 choppy", 1);
  random_run(tf, 2, ops / 10, "random unit2 choppy", 1);
  big_run(tf);
  test_break_specs(tf);
  test_uncounted(tf);
  test_snapshot(tf);
  test_oom(tf);
  test_synth(tf);
  test_huge(tf);
  test_huge_end(tf);
  test_perf(tf);
}
