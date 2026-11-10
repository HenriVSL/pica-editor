// t_adversarial.cpp - suite "adversarial": core QA (T16, TASKS T16). Owner: T16.
//
// Sections:
//   1. line-start cache inside PieceTree: differential tests against a flat model (every edit kind, undo/redo,
//      spec changes, swap, clear, OOM), memory bound, timing (cache on/off)
// Test helper names start with Adv/adv_ (coretest links every suite into one program: names must be unique).
#include "tf.h"
#include "ptree.h"
#include "ptree_int.h"
#include "doc_int.h"
#include "doc.h"
#include "docbind.h"
#include "edops.h"
#include "detect.h"
#include "layout.h"
#include "lexer.h"
#include "search.h"
#include "regex.h"
#include "ini.h"
#include "config.h"
#include "session.h"
#include "hexfmt.h"
#include "undo.h"
#include "fileops.h"
#include "fsys.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------------------------------------------
// Helpers

static u32 adv_rand(TF* tf, u32 n) { return n ? tf_rand_below(tf, n) : 0; }

// Break positions of a flat byte array under a spec (unit 2: pairs at even offsets).
static void adv_breaks(const u8* p, u32 n, const BreakSpec& bs, Vec<u32>* out)
{
  u32 i;
  out->clear();
  if (bs.unit == 1) {
    for (i = 0; i < n; i++) if (p[i] == bs.b0) out->push(i);
  } else {
    for (i = 0; i + 1 < n; i += 2) if (p[i] == bs.b0 && p[i + 1] == bs.b1) out->push(i);
  }
}

// Number of break units ending at or before pos.
static u32 adv_before(const Vec<u32>& br, u32 unit, u32 pos)
{
  u32 lo = 0, hi = br.size();
  while (lo < hi) {                          // first break with br + unit > pos
    u32 mid = lo + (hi - lo) / 2;
    if (br[mid] + unit <= pos) lo = mid + 1; else hi = mid;
  }
  return lo;
}

// Random text for line tests: regions of different break density (none, sparse, dense, every byte).
static void adv_fill(TF* tf, u8* p, u32 n, const BreakSpec& bs)
{
  u32 i = 0;
  while (i < n) {
    u32 run = 1 + adv_rand(tf, 9000), kind = adv_rand(tf, 5), j;
    if (run > n - i) run = n - i;
    for (j = 0; j < run; j++) {
      u8 b = (u8)('a' + adv_rand(tf, 26));
      u32 r = adv_rand(tf, 1000);
      if (kind == 1 && r < 30) b = bs.b0;
      else if (kind == 2 && r < 300) b = bs.b0;
      else if (kind == 3) b = (r < 500) ? bs.b0 : bs.b1;
      else if (kind == 4 && r < 20) b = (u8)(r & 1 ? 0x0D : 0x0A);
      p[i + j] = b;
    }
    i += run;
  }
}

static BreakSpec adv_spec(u32 unit, u8 b0, u8 b1)
{
  BreakSpec bs;
  bs.unit = (u8)unit; bs.b0 = b0; bs.b1 = b1; bs.pad = 0;
  return bs;
}

// Compare every line query path of the tree with the model: nq random break_pos / breaks_before calls plus the
// first and last breaks and the ends.
static int adv_tree_queries(TF* tf, PieceTree& t, const EBytes& m, u32 nq, const char* what)
{
  Vec<u32> br;
  const BreakSpec& bs = t.break_spec();
  u32 i, n = m.size(), bad = 0;
  adv_breaks(m.data(), n, bs, &br);
  if (t.break_count() != br.size()) { tf_fail(tf, __FILE__, __LINE__, "%s: break_count %u, model %u", what, t.break_count(), br.size()); return 1; }
  if (t.length() != n) { tf_fail(tf, __FILE__, __LINE__, "%s: length %u, model %u", what, t.length(), n); return 1; }
  for (i = 0; i < nq + 4 && !bad; i++) {
    u32 b, pos;
    if (br.size()) {
      b = i == 0 ? 0 : i == 1 ? br.size() - 1 : adv_rand(tf, br.size());
      if (t.break_pos(b) != br[b]) {
        tf_fail(tf, __FILE__, __LINE__, "%s: break_pos(%u) = %u, model %u", what, b, t.break_pos(b), br[b]);
        bad = 1;
      }
    }
    pos = i == 2 ? 0 : i == 3 ? n : adv_rand(tf, n + 1);
    if (t.breaks_before(pos) != adv_before(br, bs.unit, pos)) {
      tf_fail(tf, __FILE__, __LINE__, "%s: breaks_before(%u) = %u, model %u", what, pos, t.breaks_before(pos),
              adv_before(br, bs.unit, pos));
      bad = 1;
    }
  }
  if (t.break_pos(br.size()) != EPOS_NONE) { tf_fail(tf, __FILE__, __LINE__, "%s: break_pos past the end", what); bad = 1; }
  return bad;
}

// ---------------------------------------------------------------------------------------------------------------
// 1. Line-start cache: piece tree level

struct AdvRemoved { epos pos; PieceVec refs; };

static void adv_cache_tree_run(TF* tf, u32 unit, u8 b0, u8 b1, u32 size, u32 nops, const char* name)
{
  BreakSpec bs = adv_spec(unit, b0, b1);
  EBytes orig, m, ins;
  MemSource* src = new MemSource();
  PieceTree t;
  PieceVec removed;
  epos rpos = 0;
  int have_removed = 0, failed = 0;
  u32 op, typing_at = EPOS_NONE;
  char why[200];
  orig.resize(size);
  adv_fill(tf, orig.data(), size, bs);
  src->copy_from(orig.data(), size);
  m.append(orig.data(), size);
  TF_EQ_I(t.init(src, bs, 0), E_OK);
  failed |= adv_tree_queries(tf, t, m, 200, name);
  TF_CHECK(t.line_cache_bytes() > 0);
  for (op = 0; op < nops && !failed; op++) {
    u32 kind = adv_rand(tf, 100), len = t.length();
    int rc = E_OK;
    if (kind < 30) {                          // insert (sometimes large: add pieces >= 2 KB)
      u32 n = adv_rand(tf, 10) == 0 ? 2000 + adv_rand(tf, 70000) : 1 + adv_rand(tf, 3000);
      epos pos = adv_rand(tf, len + 1);
      if (unit == 2) { pos &= ~1u; n = (n + 1) & ~1u; if (len & 1) pos = pos > len - 1 ? len - 1 : pos; }
      ins.resize(n);
      adv_fill(tf, ins.data(), n, bs);
      rc = t.insert(pos, ins.data(), n, 0);
      if (rc == E_OK) m.insert(pos, ins.data(), n);
      typing_at = rc == E_OK ? pos + n : EPOS_NONE;
    } else if (kind < 45 && typing_at != EPOS_NONE && typing_at <= len) {   // typing: coalesces into one add piece
      u32 k, n;
      for (k = 0; k < 40 && rc == E_OK; k++) {
        u8 two[2];
        n = unit;
        two[0] = adv_rand(tf, 3) == 0 ? b0 : (u8)'x';
        two[1] = unit == 2 && two[0] == b0 ? b1 : (u8)'y';
        rc = t.insert(typing_at, two, n, 0);
        if (rc == E_OK) { m.insert(typing_at, two, n); typing_at += n; }
      }
    } else if (kind < 70 && len > 0) {        // remove (keep the refs for an undo-like re-insert)
      epos pos = adv_rand(tf, len);
      u32 n = 1 + adv_rand(tf, adv_rand(tf, 4) == 0 ? 140000 : 5000);
      if (n > len - pos) n = len - pos;
      if (unit == 2) {
        pos &= ~1u;
        n = (n + 1) & ~1u;
        if (n > len - pos) n = len - pos;     // odd only at the very end
      }
      removed.clear();
      rc = t.remove(pos, n, &removed);
      if (rc == E_OK) { m.erase(pos, n); rpos = pos; have_removed = 1; }
      typing_at = EPOS_NONE;
    } else if (kind < 85 && have_removed) {   // undo of the last remove: re-insert the same refs
      u32 i;
      EBytes back;
      for (i = 0; i < removed.size(); i++) {
        const PieceRef& r = removed[i];
        if (r.src == 0) back.append(orig.data() + r.start, r.len);
        else back.append(t.add_buffer()->chunk(r.src) + r.start, r.len);
      }
      if (rpos <= t.length()) {
        rc = t.insert_refs(rpos, removed.data(), removed.size());
        if (rc == E_OK) m.insert(rpos, back.data(), back.size());
      }
      have_removed = 0;
      typing_at = EPOS_NONE;
    } else if (kind < 90) {                   // break spec change and back (same unit: recount; cache must follow)
      BreakSpec other = bs;
      other.b0 = unit == 1 ? (u8)(b0 == 0x0A ? 0x0D : 0x0A) : b0;
      if (unit == 2) other.b0 = (u8)(b0 == 0x0A ? 0x0D : 0x0A), other.b1 = b1;
      rc = t.set_break_spec(other, 0);
      if (rc == E_OK) failed |= adv_tree_queries(tf, t, m, 40, name);
      rc = t.set_break_spec(bs, 0);
      have_removed = 0;                       // refs are tagged with the old spec: insert_refs would recount (fine)
    } else if (kind < 93) {                   // swap with a second tree and back
      PieceTree other;
      MemSource* s2 = new MemSource();
      EBytes o2;
      o2.resize(70000);
      adv_fill(tf, o2.data(), o2.size(), bs);
      s2->copy_from(o2.data(), o2.size());
      TF_EQ_I(other.init(s2, bs, 0), E_OK);
      failed |= adv_tree_queries(tf, other, o2, 30, name);
      t.swap(other);
      failed |= adv_tree_queries(tf, t, o2, 30, name);
      failed |= adv_tree_queries(tf, other, m, 30, name);
      t.swap(other);
      other.clear();
      delete s2;
    } else if (kind < 95 && unit == 2 && len > 4) {   // odd op: slow path (rebuild with realignment)
      epos pos = 1 + 2 * adv_rand(tf, (len - 2) / 2);
      rc = t.remove(pos, 1, 0);
      if (rc == E_OK) m.erase(pos, 1);
      if (rc == E_OK) {
        u8 z = 'z';
        rc = t.insert(pos, &z, 1, 0);
        if (rc == E_OK) m.insert(pos, &z, 1);
      }
      have_removed = 0;
      typing_at = EPOS_NONE;
    }
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: op %u kind %u: %s", name, op, kind, e_errname(rc)); failed = 1; }
    if (!failed && t.check(why, sizeof(why))) { tf_fail(tf, __FILE__, __LINE__, "%s: op %u: check: %s", name, op, why); failed = 1; }
    if (!failed) failed |= adv_tree_queries(tf, t, m, 60, name);
  }
  TF_CHECK(!failed);
  TF_CHECK(t.line_cache_bytes() <= 140000);
  // clear + init over another source of the same size: nothing stale may survive
  {
    MemSource* s3 = new MemSource();
    EBytes o3;
    o3.resize(size);
    adv_fill(tf, o3.data(), size, bs);
    s3->copy_from(o3.data(), size);
    t.clear();
    TF_EQ_U(t.line_cache_bytes(), 0);
    TF_EQ_I(t.init(s3, bs, 0), E_OK);
    TF_CHECK(!adv_tree_queries(tf, t, o3, 300, name));
    t.clear();
    delete s3;
  }
  delete src;
}

// The cache's block grid near 4 GB and the u32 wrap: a periodic source of EPOS_MAX bytes.
class AdvPeriodSource : public ByteSource {
public:
  AdvPeriodSource(epos size, u32 period, u8 brk) : size_(size), period_(period)
  {
    u32 i;
    n_ = period * (65536 / period + 2);
    buf_ = (u8*)e_alloc(n_);
    for (i = 0; buf_ && i < n_; i++) buf_[i] = (u8)(i % period == period - 1 ? brk : 'A' + i % 23);
  }
  ~AdvPeriodSource() { e_free(buf_); }
  epos size() const { return size_; }
  const u8* get(epos off, epos* avail)
  {
    u32 r;
    if (off >= size_ || !buf_) { *avail = 0; return 0; }
    r = off % period_;
    *avail = n_ - r;
    if (*avail > size_ - off) *avail = size_ - off;
    return buf_ + r;
  }
  u8 at(epos off) const { return buf_[off % period_]; }
  u32 breaks_in(epos a, epos b) const   // breaks at offsets in [a, b)
  {
    return (u32)(b / period_ - a / period_);  // breaks are at k * period + period - 1: floor(x / period) below x
  }
  epos size_; u32 period_; u32 n_; u8* buf_;
};

static void adv_cache_huge(TF* tf)
{
  AdvPeriodSource* src = new AdvPeriodSource(EPOS_MAX, 997, 0x0A);
  Document d;
  Vec<u32> blocks;
  EolCounts ec;
  u32 nb = (u32)(((u64)EPOS_MAX + 65535) / 65536), i, k, total;
  epos pos;
  int rc;
  for (i = 0; i < nb; i++) {
    epos a = i * 65536u, b = (i == nb - 1) ? EPOS_MAX : a + 65536u;
    blocks.push(src->breaks_in(a, b));
  }
  total = src->breaks_in(0, EPOS_MAX);
  ec.crlf = 0; ec.lf = total; ec.cr = 0;
  TF_EQ_I(d.init_source_indexing(src, enc_ansi(), 0, EOL_LF), E_OK);
  TF_EQ_I(d.finish_indexing(blocks.data(), nb, ec), E_OK);
  TF_EQ_U(d.line_count(), total + 1);
  // line queries all over the 4 GB, especially the last blocks (cache entries at blk 0xFFFF)
  for (k = 0; k < 400; k++) {
    u32 line = k < 100 ? total - k : adv_rand(tf, total + 1);
    epos expect = line == 0 ? 0 : (epos)((u64)line * 997);   // break l-1 is at l*997-1, the line starts after it
    if (d.line_start(line) != expect) { tf_fail(tf, __FILE__, __LINE__, "huge line_start(%u) = %u, expected %u", line, d.line_start(line), expect); break; }
    pos = k < 50 ? EPOS_MAX - k : adv_rand(tf, EPOS_MAX);
    if (d.line_of(pos) != (u32)(pos / 997)) { tf_fail(tf, __FILE__, __LINE__, "huge line_of(%u) = %u", pos, d.line_of(pos)); break; }
  }
  TF_CHECK(d.tree()->line_cache_bytes() > 0 && d.tree()->line_cache_bytes() <= 140000);
  // edits at the far end, at 2^31 and at the start, then undo: the cache must follow
  rc = d.replace(EPOS_MAX - 5, 5, (const u8*)"\n\n", 2);
  TF_EQ_I(rc, E_OK);
  TF_EQ_I(d.replace(0x80000000u, 3000, (const u8*)"x\ny", 3), E_OK);
  TF_EQ_I(d.replace(0, 0, (const u8*)"\n", 1), E_OK);
  {
    // model: each edit's effect on lines, checked through line_of on both sides of every edit
    u32 l0 = d.line_of(1);
    TF_EQ_U(l0, 1);
    TF_EQ_U(d.line_start(1), 1);
    TF_EQ_U(d.line_of(0x80000003u), d.line_of(0x80000002u) + 1);   // "x\ny" now at 2^31 + 1: its break ends at + 3
  }
  TF_EQ_I(d.undo(0), E_OK);
  TF_EQ_I(d.undo(0), E_OK);
  TF_EQ_I(d.undo(0), E_OK);
  TF_EQ_U(d.line_count(), total + 1);
  for (k = 0; k < 200; k++) {
    u32 line = k < 50 ? total - k : adv_rand(tf, total + 1);
    if (d.line_start(line) != (line ? (epos)((u64)line * 997) : 0)) { tf_fail(tf, __FILE__, __LINE__, "huge after undo line_start(%u)", line); break; }
  }
  TF_EQ_I(d.convert_eol(EOL_CRLF, 0), E_TOOBIG);   // on-demand documents refuse whole-document rewrites
}

// Document level: random edits with undo/redo, EOL conversion, reinterpret (unit changes) and convert.
static void adv_cache_doc(TF* tf, u32 nops)
{
  Document d;
  EBytes text, m;
  MemSource* src = new MemSource();
  BreakSpec bs = adv_spec(1, 0x0A, 0);
  u32 op, failed = 0;
  text.resize(400000);
  adv_fill(tf, text.data(), text.size(), bs);
  src->copy_from(text.data(), text.size());
  TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  for (op = 0; op < nops && !failed; op++) {
    u32 kind = adv_rand(tf, 100), len = d.length(), unit = d.tree()->break_spec().unit, k;
    int rc = E_OK;
    Selection sel;
    if (kind < 40) {
      epos pos = adv_rand(tf, len + 1), del = adv_rand(tf, 3) ? adv_rand(tf, 3000) : adv_rand(tf, 100000);
      u32 n = adv_rand(tf, 4) ? adv_rand(tf, 400) : adv_rand(tf, 20000);
      EBytes ins;
      if (del > len - pos) del = len - pos;
      if (unit == 2) { pos &= ~1u; del &= ~1u; n &= ~1u; if (pos + del > len) del = 0; }
      ins.resize(n);
      adv_fill(tf, ins.data(), n, bs);
      if (unit == 2) for (k = 1; k < n; k += 2) ins[k] = 0;   // UTF-16 LE: ASCII units
      rc = d.replace(pos, del, ins.data(), n);
      if (rc == E_ALIGN || rc == E_RANGE) rc = E_OK;
    } else if (kind < 60) {
      rc = d.undo(&sel);
      if (rc == E_STATE) rc = E_OK;
    } else if (kind < 75) {
      rc = d.redo(&sel);
      if (rc == E_STATE) rc = E_OK;
    } else if (kind < 82) {
      rc = d.convert_eol(adv_rand(tf, 3) == 0 ? EOL_CR : adv_rand(tf, 2) ? EOL_LF : EOL_CRLF, 0);
    } else if (kind < 88) {
      EncSpec e = adv_rand(tf, 2) ? enc_utf16le(0) : enc_ansi();
      rc = d.reinterpret(e, 0, 0);           // bytes unchanged, unit may change: undo history stays valid
    } else if (kind < 92) {
      ConvertReport rep;
      EncSpec e = adv_rand(tf, 2) ? enc_utf16le(0) : enc_utf8(0);
      rc = d.convert(e, &rep, 0);
    } else if (kind < 94) {
      d.set_saved();
    }
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "doc op %u kind %u: %s", op, kind, e_errname(rc)); failed = 1; break; }
    // compare line queries with the bytes
    {
      Vec<u32> br;
      BreakSpec cur = d.tree()->break_spec();
      u32 q, n = d.length();
      m.resize(n);
      d.read(0, m.data(), n);
      adv_breaks(m.data(), n, cur, &br);
      if (d.line_count() != br.size() + 1) { tf_fail(tf, __FILE__, __LINE__, "doc op %u: line_count %u, model %u", op, d.line_count(), br.size() + 1); failed = 1; }
      for (q = 0; q < 30 && !failed; q++) {
        u32 line = adv_rand(tf, br.size() + 1);
        epos pos = adv_rand(tf, n + 1);
        epos expect = line == 0 ? 0 : br[line - 1] + cur.unit;
        if (d.line_start(line) != expect) { tf_fail(tf, __FILE__, __LINE__, "doc op %u: line_start(%u) %u, model %u", op, line, d.line_start(line), expect); failed = 1; }
        if (d.line_of(pos) != adv_before(br, cur.unit, pos)) { tf_fail(tf, __FILE__, __LINE__, "doc op %u: line_of(%u)", op, pos); failed = 1; }
      }
    }
  }
  TF_CHECK(!failed);
}

// Allocation failure of the cache only disables it.
static void adv_cache_oom(TF* tf)
{
  BreakSpec bs = adv_spec(1, 0x0A, 0);
  EBytes o;
  MemSource* src = new MemSource();
  PieceTree t;
  o.resize(300000);
  adv_fill(tf, o.data(), o.size(), bs);
  src->copy_from(o.data(), o.size());
  TF_EQ_I(t.init(src, bs, 0), E_OK);
  {
    Vec<u32> br;
    epos got;
    u32 cnt;
    adv_breaks(o.data(), o.size(), bs, &br);
    e_test_fail_after(1);
    got = t.break_pos(br.size() - 7);         // first query: the table allocation fails
    e_test_fail_after(-1);
    TF_EQ_U(got, br[br.size() - 7]);
    TF_EQ_U(t.line_cache_bytes(), 0);
    e_test_fail_after(1);
    cnt = t.breaks_before(250000);
    e_test_fail_after(-1);
    TF_EQ_U(cnt, adv_before(br, 1, 250000));
  }
  TF_CHECK(!adv_tree_queries(tf, t, o, 100, "oom after"));
  TF_CHECK(t.line_cache_bytes() > 0);
  t.clear();
  delete src;
}

// Timing: random line_start / line_of on a fresh 93 KB, 3000-line document (T08's VM measurement) and on a 10 MB
// document, cache off and on. Loops at least 0.3 s (Win98 clock() ticks every 55 ms).
static double adv_time_queries(TF* tf, Document& d, int what, u32* sink)
{
  clock_t c0 = clock(), c1;
  u32 calls = 0, lines = d.line_count(), i;
  epos len = d.length();
  do {
    for (i = 0; i < 1000; i++) {
      if (what == 0) *sink += d.line_start(adv_rand(tf, lines));
      else *sink += d.line_of(adv_rand(tf, len + 1));
    }
    calls += 1000;
    c1 = clock();
  } while ((double)(c1 - c0) < 0.3 * CLOCKS_PER_SEC);
  return (double)(c1 - c0) * 1e6 / CLOCKS_PER_SEC / calls;
}

static void adv_cache_timing(TF* tf)
{
  u32 sizes[2] = { 93000, 10 * 1024 * 1024 }, s, saved_rng = tf->rng;   // timing loops draw a speed-dependent
  // number of random values: restore the generator afterwards so check counts are the same on every machine
  for (s = 0; s < 2; s++) {
    Document d;
    EBytes text;
    MemSource* src = new MemSource();
    u32 i = 0, sink_off = 0, sink_on = 0, seed;
    double off_ls, off_lo, on_ls, on_lo, warm_ls;
    text.resize(sizes[s]);
    while (i < text.size()) {                // lines of 0..60 chars, CRLF
      u32 n = adv_rand(tf, 61), j;
      for (j = 0; j < n && i < text.size(); j++) text[i++] = (u8)('a' + adv_rand(tf, 26));
      if (i < text.size()) text[i++] = 0x0D;
      if (i < text.size()) text[i++] = 0x0A;
    }
    src->copy_from(text.data(), text.size());
    TF_EQ_I(d.init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
    seed = tf->rng;
    pt_line_cache_enable(0);
    off_ls = adv_time_queries(tf, d, 0, &sink_off);
    off_lo = adv_time_queries(tf, d, 1, &sink_off);
    pt_line_cache_enable(1);
    tf->rng = seed;
    on_ls = adv_time_queries(tf, d, 0, &sink_on);     // includes filling the cache (fresh document)
    on_lo = adv_time_queries(tf, d, 1, &sink_on);
    warm_ls = adv_time_queries(tf, d, 0, &sink_on);   // the cache is warm now
    printf("  line cache timing (%u KB, %u lines): line_start %.2f -> %.2f us (warm %.2f), line_of %.2f -> %.2f us "
           "per call; cache %u bytes\n", sizes[s] / 1024, d.line_count(), off_ls, on_ls, warm_ls, off_lo, on_lo,
           d.tree()->line_cache_bytes());
    fflush(stdout);
    TF_CHECK(on_ls < 100.0 && on_lo < 100.0);   // VM target < 20 us (the line above is the measurement)
    TF_CHECK(d.tree()->line_cache_bytes() <= 140000);
  }
  tf->rng = saved_rng;
}

// ---------------------------------------------------------------------------------------------------------------
// 2. Pathological inputs through detection, Document, characters, codecs, layout, search, lexers and line ops

static const char* const g_adv_lng[13] = {
  "cpp", "rc", "make", "bat", "ini", "asm", "python", "html", "js", "css", "xml", "md", "diff"
};

static int adv_read_file(const char* path, EBytes* out)
{
  FILE* f = fopen(path, "rb");
  long n;
  out->clear();
  if (!f) return E_NOTFOUND;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0 || out->resize((u32)n) != E_OK || fread(out->data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return E_IO; }
  fclose(f);
  return E_OK;
}

class AdvLangs : public LangSource {
public:
  AdvLangs() : n_(0) {}
  u32 count() { return n_; }
  int get(u32 i, const char** name, const char** text, u32* len)
  {
    if (i >= n_) return E_RANGE;
    *name = nm_[i]; *text = (const char*)t_[i].data(); *len = t_[i].size();
    return E_OK;
  }
  int load(TF* tf)
  {
    u32 i;
    for (i = 0; i < 13; i++) {
      char path[400];
      es_fmt(nm_[i], sizeof(nm_[i]), "%s.lng", g_adv_lng[i]);
      es_fmt(path, sizeof(path), "%s/../../src/langs/%s", tf->data_dir, nm_[i]);
      if (adv_read_file(path, &t_[i]) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "cannot read %s", path); return 0; }
    }
    n_ = 13;
    return 1;
  }
  EBytes t_[13]; char nm_[13][24]; u32 n_;
};

static AdvLangs* g_adv_langs = 0;

static void adv_lang_init(TF* tf)
{
  EStr err;
  if (!g_adv_langs) g_adv_langs = new AdvLangs();
  if (!g_adv_langs->load(tf)) return;
  TF_EQ_I(lang_init(g_adv_langs, 0, &err), E_OK);
}

static void adv_lang_free()
{
  lang_shutdown();
  delete g_adv_langs;
  g_adv_langs = 0;
}

static void adv_to_ecp(const char* s, Vec<ecp>* out)
{
  out->clear();
  es_utf8_to_ecp(s, es_len(s), out);
}

static epos adv_bom_len(const u8* p, u32 n, const EncSpec& e)
{
  u8 bom[4];
  u32 k = enc_bom(e, bom);
  return (k && n >= k && memcmp(p, bom, k) == 0) ? k : 0;
}

static int adv_doc_init(Document& d, const u8* p, u32 n, EncSpec e, EolStyle eol)
{
  MemSource* src = new MemSource();
  epos bl;
  if (!src) return E_NOMEM;
  if (src->copy_from(p, n) != E_OK) { delete src; return E_NOMEM; }
  bl = adv_bom_len(p, n, e);
  e.bom = (u8)(bl ? 1 : 0);
  return d.init_source(src, e, bl, eol, 0);
}

static void adv_doc_bytes(Document& d, EBytes* out)
{
  out->resize(d.length());
  d.read(0, out->data(), d.length());
}

// Forward and backward character walks agree (same boundaries), snap_char lands on a boundary, and decoding then
// re-encoding every character gives back the exact bytes.
static int adv_char_checks(TF* tf, Document& d, const char* what)
{
  EBytes fw, re;
  epos pos, org = d.text_origin(), len = d.length(), steps = 0, k;
  u32 bad = 0;
  CharReader cr;
  ecp c;
  epos at;
  fw.resize(len + 1);
  memset(fw.data(), 0, len + 1);
  for (pos = org; pos < len && steps <= len; steps++) {
    epos nx = d.char_next(pos);
    if (nx <= pos) { tf_fail(tf, __FILE__, __LINE__, "%s: char_next(%u) = %u", what, pos, nx); return 1; }
    fw[pos] = 1;
    pos = nx;
  }
  fw[len] = 1;
  for (pos = len, k = 0; pos > org && k <= len; k++) {
    epos pv = d.char_prev(pos);
    if (pv >= pos || !fw[pv]) {
      if (!bad) tf_fail(tf, __FILE__, __LINE__, "%s: char_prev(%u) = %u is not a forward boundary", what, pos, pv);
      bad++;
      if (pv >= pos) return 1;
    }
    pos = pv;
  }
  if (!bad && k != steps) { tf_fail(tf, __FILE__, __LINE__, "%s: %u chars forward, %u backward", what, steps, k); bad++; }
  for (k = 0; k < 200 && len > org; k++) {
    epos q = org + adv_rand(tf, len - org), sn = d.snap_char(q);
    if (sn > q || !fw[sn]) { if (!bad) tf_fail(tf, __FILE__, __LINE__, "%s: snap_char(%u) = %u", what, q, sn); bad++; break; }
  }
  cr.init(d.bytes(), d.codec(), org, len);
  while (cr.next(&c, &at) > 0) {
    u8 out[8];
    u32 n = d.codec()->encode(c, out);
    if (n == 0) { if (!bad) tf_fail(tf, __FILE__, __LINE__, "%s: char %x at %u does not encode back", what, c, at); bad++; break; }
    re.append(out, n);
  }
  if (!bad) {
    EBytes all;
    adv_doc_bytes(d, &all);
    if (re.size() != len - org || memcmp(re.data(), all.data() + org, re.size()) != 0) {
      tf_fail(tf, __FILE__, __LINE__, "%s: decode/encode round trip differs (%u of %u bytes)", what, re.size(), len - org);
      bad++;
    }
  }
  return bad != 0;
}

// Lines: line_start <= line_end <= line_next, consecutive, the last line ends at length(); layout every line.
static int adv_line_checks(TF* tf, Document& d, const char* what)
{
  u32 lines = d.line_count(), l, bad = 0, lim = lines < 800 ? lines : 800;
  epos prev_next = 0;
  LineLayout lay;
  LayoutOpts lo;
  EBytes buf;
  lo.tab_width = 4; lo.show_ws = 1; lo.pad[0] = lo.pad[1] = 0;
  for (l = 0; l < lim && !bad; l++) {
    epos a = d.line_start(l), e = d.line_end(l), nx = d.line_next(l);
    if (a != (l ? prev_next : 0) || a > e || e > nx || (l + 1 == lines && nx != d.length())) {
      tf_fail(tf, __FILE__, __LINE__, "%s: line %u: start %u end %u next %u (prev next %u)", what, l, a, e, nx, prev_next);
      bad = 1;
      break;
    }
    if (l + 1 == lines && e != d.length()) { tf_fail(tf, __FILE__, __LINE__, "%s: last line end", what); bad = 1; }
    prev_next = nx;
    {
      u32 n = e - a > 65536 ? 65536 : e - a, i;
      buf.resize(n);
      d.read(a, buf.data(), n);
      if (lay.build(buf.data(), n, d.codec(), lo, 0, n == e - a) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: layout line %u", what, l); bad = 1; break; }
      for (i = 1; i < lay.count(); i++) {
        if (lay.chars()[i].off <= lay.chars()[i - 1].off || lay.chars()[i].cell < lay.chars()[i - 1].cell) {
          tf_fail(tf, __FILE__, __LINE__, "%s: layout line %u char %u not monotonic", what, l, i);
          bad = 1;
          break;
        }
      }
      if (lay.bytes() > n) { tf_fail(tf, __FILE__, __LINE__, "%s: layout bytes", what); bad = 1; }
      if (!bad) {
        Vec<u32> rows;
        if (lay.wrap_rows(1 + adv_rand(tf, 80), (int)adv_rand(tf, 2), &rows) != E_OK || rows.size() == 0 || rows[0] != 0) {
          tf_fail(tf, __FILE__, __LINE__, "%s: wrap_rows line %u", what, l);
          bad = 1;
        }
      }
    }
  }
  return bad;
}

static int adv_search_checks(TF* tf, Document& d, const char* what)
{
  static const char* const pats[] = { "a", ".", "\\w+", "^", "$", "[^a-z]+", "(?i)A", "\\x{FFFD}|\\r" };
  static const u8 modes[] = { SM_NORMAL, SM_REGEX, SM_REGEX, SM_REGEX, SM_REGEX, SM_REGEX, SM_REGEX, SM_REGEX };
  u32 i, bad = 0;
  for (i = 0; i < 8 && !bad; i++) {
    Searcher s;
    SearchOpts o;
    SearchError err;
    Vec<ecp> pat;
    Vec<MarkRange> all;
    RxMatch m;
    u32 cnt = 0, j;
    int rc;
    o.mode = modes[i]; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
    adv_to_ecp(pats[i], &pat);
    if (s.compile(pat.data(), pat.size(), o, d.enc(), &err) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: compile %s", what, pats[i]); bad = 1; break; }
    rc = s.count(d.bytes(), d.codec(), d.text_origin(), d.length(), &cnt, 0);
    if (rc < 0) { tf_fail(tf, __FILE__, __LINE__, "%s: count %s: %s", what, pats[i], e_errname(rc)); bad = 1; break; }
    rc = s.find_all(d.bytes(), d.codec(), d.text_origin(), d.length(), &all, 2000000, 0);
    if (rc < 0 || all.size() != cnt) { tf_fail(tf, __FILE__, __LINE__, "%s: find_all %s: %d, %u vs %u", what, pats[i], rc, all.size(), cnt); bad = 1; break; }
    for (j = 0; j < all.size(); j++) {
      if (all[j].pos < d.text_origin() || all[j].pos > d.length() || all[j].len > d.length() - all[j].pos ||
          (j && all[j].pos < all[j - 1].pos + all[j - 1].len)) {
        tf_fail(tf, __FILE__, __LINE__, "%s: match %u of %s out of order", what, j, pats[i]);
        bad = 1;
        break;
      }
    }
    if (!bad) {                              // backward from the end: the last forward match starting before it
      u32 last = all.size();
      while (last > 0 && all[last - 1].pos >= d.length()) last--;
      rc = s.find(d.bytes(), d.codec(), d.text_origin(), d.length(), d.length(), 1, &m, 0);
      if (rc != (last ? 1 : 0)) { tf_fail(tf, __FILE__, __LINE__, "%s: backward %s: %d", what, pats[i], rc); bad = 1; }
      else if (rc == 1 && (m.pos + m.len > d.length() || m.pos < all[last - 1].pos)) {
        // equal except for overlapping matches (T03); never before the last forward match
        tf_fail(tf, __FILE__, __LINE__, "%s: backward %s at %u, last forward %u", what, pats[i], m.pos, all[last - 1].pos);
        bad = 1;
      }
    }
  }
  return bad;
}

static int adv_undo_all(Document& d)
{
  Selection s;
  int n = 0;
  while (d.undo(&s) == E_OK) n++;
  return n;
}

// Line commands, case change, comment toggle, indentation on everything; undoing all of it restores the bytes.
static int adv_edit_checks(TF* tf, Document& d, const char* what)
{
  EBytes before, after;
  EdCtx e;
  u32 i, bad = 0;
  int rc;
  adv_doc_bytes(d, &before);
  memset(&e, 0, sizeof(e));
  e.doc = &d;
  ed_lang_from(lang_find("cpp"), 4, 0, &e.lang);
  for (i = 0; i < 14; i++) {
    epos len = d.length(), a = d.text_origin() + adv_rand(tf, len - d.text_origin() + 1),
         b = d.text_origin() + adv_rand(tf, len - d.text_origin() + 1);
    e.sel.anchor = d.snap_char(a); e.sel.caret = d.snap_char(b);
    if (i == 0) { e.sel.anchor = d.text_origin(); e.sel.caret = len; }
    e.now_ms = i * 10000;
    switch (i % 7) {
      case 0: rc = ed_change_case(&e, (int)(i & 1)); break;
      case 1: rc = ed_toggle_comment(&e); break;
      case 2: rc = ed_indent(&e, (int)adv_rand(tf, 2)); break;
      case 3: rc = ed_duplicate(&e); break;
      case 4: rc = ed_move_lines(&e, (int)adv_rand(tf, 2)); break;
      case 5: rc = ed_delete_lines(&e); break;
      default: rc = ed_delete_word(&e, (int)adv_rand(tf, 2)); break;
    }
    if (rc != E_OK && rc != E_RANGE && rc != E_ALIGN && rc != E_STATE && rc != E_NOTFOUND) {
      tf_fail(tf, __FILE__, __LINE__, "%s: edit op %u: %s", what, i, e_errname(rc));
      bad = 1;
      break;
    }
    if (e.sel.anchor > d.length() || e.sel.caret > d.length()) { tf_fail(tf, __FILE__, __LINE__, "%s: edit op %u: selection out of range", what, i); bad = 1; break; }
  }
  adv_undo_all(d);
  adv_doc_bytes(d, &after);
  if (after.size() != before.size() || memcmp(after.data(), before.data(), before.size()) != 0) {
    tf_fail(tf, __FILE__, __LINE__, "%s: undoing the edit ops did not restore the bytes", what);
    bad = 1;
  }
  return bad;
}

// Convert to other encodings and the other EOL styles, then undo: exact bytes back; reinterpret back and forth.
static int adv_convert_checks(TF* tf, Document& d, const char* what)
{
  EncSpec targets[5];
  EBytes before, after;
  u32 i, bad = 0;
  EncSpec orig = d.enc();
  targets[0] = enc_utf8(0); targets[1] = enc_utf16le(1); targets[2] = enc_ansi(); targets[3] = enc_cp(866);
  targets[4] = enc_utf16be(0);
  adv_doc_bytes(d, &before);
  for (i = 0; i < 5 && !bad; i++) {
    ConvertReport r1, r2;
    int same = enc_equal(d.enc(), targets[i]) ? 1 : 0;
    int rc = d.check_convert(targets[i], &r1, 0);
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: check_convert %u: %s", what, i, e_errname(rc)); bad = 1; break; }
    rc = d.convert(targets[i], &r2, 0);
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: convert %u: %s", what, i, e_errname(rc)); bad = 1; break; }
    if (r1.unrepresentable != r2.unrepresentable || r1.invalid_src != r2.invalid_src) {
      tf_fail(tf, __FILE__, __LINE__, "%s: convert %u: check said %u/%u, convert %u/%u", what, i, r1.unrepresentable,
              r1.invalid_src, r2.unrepresentable, r2.invalid_src);
      bad = 1;
    }
    if (!bad) bad = (u32)adv_line_checks(tf, d, what);
    if (!bad) bad = (u32)adv_char_checks(tf, d, what);
    if (!bad && !same) {                      // converting to the same encoding records nothing
      Selection s;
      if (d.undo(&s) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: undo convert %u", what, i); bad = 1; }
    }
  }
  for (i = 0; i < 3 && !bad; i++) {
    if (d.convert_eol((EolStyle)i, 0) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: convert_eol %u", what, i); bad = 1; break; }
    bad = (u32)adv_line_checks(tf, d, what);
    adv_undo_all(d);
  }
  adv_doc_bytes(d, &after);
  if (!bad && (!enc_equal(d.enc(), orig) || after.size() != before.size() ||
               memcmp(after.data(), before.data(), before.size()) != 0)) {
    tf_fail(tf, __FILE__, __LINE__, "%s: undoing conversions did not restore bytes and encoding", what);
    bad = 1;
  }
  if (!bad) {
    if (d.reinterpret(enc_utf16le(0), 0, 0) != E_OK || d.reinterpret(orig, d.text_origin(), 0) != E_OK) {
      tf_fail(tf, __FILE__, __LINE__, "%s: reinterpret", what);
      bad = 1;
    } else {
      adv_doc_bytes(d, &after);
      if (after.size() != before.size() || memcmp(after.data(), before.data(), before.size()) != 0) {
        tf_fail(tf, __FILE__, __LINE__, "%s: reinterpret changed bytes", what);
        bad = 1;
      }
    }
  }
  return bad;
}

static void adv_highlight_all(TF* tf, Document& d, const char* lang, const char* what)
{
  Highlighter h;
  DocLines dl;
  const LangInfo* li = lang_find(lang);
  u32 l, guard = 0;
  Vec<ecp> buf;
  Vec<u8> st;
  if (!li) return;
  dl.bind(&d, &h);
  if (h.attach(&dl, li) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: attach", what); dl.unbind(); return; }
  h.set_target(d.line_count() - 1);
  while (h.work(1000) && guard < 100000) guard++;
  buf.resize(LEX_LINE_MAX);
  st.resize(LEX_LINE_MAX);
  for (l = 0; l < d.line_count() && l < 3000; l++) {
    u32 n = 0;
    if (dl.lp_get(l, buf.data(), LEX_LINE_MAX, &n) != E_OK || n > LEX_LINE_MAX) { tf_fail(tf, __FILE__, __LINE__, "%s: lp_get %u", what, l); break; }
    if (h.style_line(l, buf.data(), n, st.data()) != 1) { tf_fail(tf, __FILE__, __LINE__, "%s: style_line %u", what, l); break; }
  }
  TF_CHECK(h.valid_upto() == d.line_count());
  h.detach();
  dl.unbind();
}

struct AdvInput { const char* name; EBytes b; };

static void adv_make_inputs(TF* tf, Vec<AdvInput*>* out)
{
  static const u8 inv[] = {                  // invalid / overlong / surrogate / truncated UTF-8
    0xC0, 0x80, 0xC1, 0xBF, 0xE0, 0x80, 0x80, 0xE0, 0x9F, 0xBF, 0xED, 0xA0, 0x80, 0xED, 0xBF, 0xBF, 0xF0, 0x80, 0x80,
    0x80, 0xF4, 0x90, 0x80, 0x80, 0xF5, 0x80, 0x80, 0x80, 0xF8, 0x88, 0x80, 0x80, 0x80, 0xFC, 0x84, 0x80, 0x80, 0x80,
    0x80, 0xFE, 0xFF, 0x80, 0xBF, 0xC3, 0xE2, 0x82, 0xF0, 0x9F, 0x98, 0x0A, 0xE2, 0x0D, 0x0A, 0xC3, 0xA9, 0xA9 };
  const char* names[] = { "empty", "bom8 only", "bom16le only", "bom16be only", "all NUL", "all CR", "all LF",
                          "broken eol", "random bytes", "invalid utf8", "utf16 odd tail", "long line", "tabs+ctrl" };
  u32 i, j;
  for (i = 0; i < 13; i++) {
    AdvInput* in = new AdvInput;
    EBytes& b = in->b;
    in->name = names[i];
    switch (i) {
      case 0: break;
      case 1: b.append("\xEF\xBB\xBF", 3); break;
      case 2: b.append("\xFF\xFE", 2); break;
      case 3: b.append("\xFE\xFF", 2); break;
      case 4: b.resize(20000); break;
      case 5: b.resize(5001); memset(b.data(), 0x0D, b.size()); break;
      case 6: b.resize(5000); memset(b.data(), 0x0A, b.size()); break;
      case 7:
        for (j = 0; j < 8000; j++) {
          u32 r = adv_rand(tf, 10);
          u8 c = (u8)(r < 2 ? 0x0D : r < 4 ? 0x0A : 'a' + adv_rand(tf, 26));
          b.push(c);
          if (r == 5) { b.push(0x0D); b.push(0x0A); }
        }
        break;
      case 8: b.resize(20001); for (j = 0; j < b.size(); j++) b[j] = (u8)adv_rand(tf, 256); break;
      case 9:
        for (j = 0; j < 1500; j++) {
          u32 k = adv_rand(tf, sizeof(inv) - 4), m = 1 + adv_rand(tf, 4);
          b.append(inv + k, m);
          if (adv_rand(tf, 4) == 0) b.append("ab\xC3\xA9", 4);
        }
        break;
      case 10:
        b.append("\xFF\xFE", 2);
        for (j = 0; j < 8000; j++) { u8 u[2]; u[0] = (u8)adv_rand(tf, 256); u[1] = (u8)(adv_rand(tf, 3) ? 0 : 0xD8 + adv_rand(tf, 8)); b.append(u, 2); }
        b.push(0x41);
        break;
      case 11: b.resize(70000); for (j = 0; j < b.size(); j++) b[j] = (u8)(j % 251 == 0 ? 0x09 : 'a' + j % 26); break;
      default: for (j = 0; j < 10000; j++) b.push((u8)(adv_rand(tf, 3) ? adv_rand(tf, 33) : 0x7F + adv_rand(tf, 0x22))); break;
    }
    out->push(in);
  }
}

static void adv_pathological(TF* tf)
{
  Vec<AdvInput*> ins;
  EncSpec encs[7];
  u32 i, j;
  encs[0] = enc_utf8(1); encs[1] = enc_ansi(); encs[2] = enc_utf16le(1); encs[3] = enc_utf16be(1); encs[4] = enc_cp(437);
  encs[5] = enc_cp(1251); encs[6] = enc_utf8(0);
  adv_make_inputs(tf, &ins);
  for (i = 0; i < ins.size(); i++) {
    const EBytes& b = ins[i]->b;
    DetectOpts o;
    DetectResult r;
    o.utf8_nobom = 1; o.utf16_nobom = 1; o.pad[0] = o.pad[1] = 0; o.ansi = enc_ansi(); o.default_eol = EOL_CRLF;
    clock_t t0 = clock();
    TF_EQ_I(detect(b.data(), b.size(), 1, o, &r), E_OK);
    TF_CHECK(r.bom_len <= b.size());
    for (j = 0; j < 8; j++) {
      Document d;
      char what[80];
      EncSpec e = j == 7 ? r.enc : encs[j];
      EolStyle eol = j == 7 ? r.eol_style : (j == 4 ? EOL_CR : EOL_CRLF);
      int rc;
      es_fmt(what, sizeof(what), "%s as %u", ins[i]->name, j);
      rc = adv_doc_init(d, b.data(), b.size(), e, eol);
      if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: init %s", what, e_errname(rc)); continue; }
      if (adv_line_checks(tf, d, what)) continue;
      if (adv_char_checks(tf, d, what)) continue;
      if (j == 0 || j == 7 || (j == 2 && tf->long_run)) {
        if (adv_search_checks(tf, d, what)) continue;
        adv_highlight_all(tf, d, (i & 1) ? "cpp" : "html", what);
        if (adv_edit_checks(tf, d, what)) continue;
      }
      if ((j == 0 || j == 2 || j == 7 || tf->long_run) && j != 4 && j != 5 && adv_convert_checks(tf, d, what)) continue;
      TF_CHECK(1);
    }
    tf_log(tf, "    %s: %u ms", ins[i]->name, (u32)((clock() - t0) * 1000 / CLOCKS_PER_SEC));
  }
  for (i = 0; i < ins.size(); i++) delete ins[i];
}

// A 5 MB line: line queries, layout index, literal and regex search at its end, highlighting (capped prefix).
static void adv_long_line(TF* tf)
{
  EBytes b;
  Document d;
  u32 i, n = 5 * 1024 * 1024;
  b.resize(n);
  for (i = 0; i < n; i++) b[i] = (u8)("abc def\t\xC3\xA9xyz"[i % 13]);
  memcpy(b.data() + n - 7, "needle!", 7);
  TF_EQ_I(adv_doc_init(d, b.data(), n, enc_utf8(0), EOL_CRLF), E_OK);
  TF_EQ_U(d.line_count(), 1);
  TF_EQ_U(d.line_end(0), n);
  TF_EQ_U(d.line_of(n), 0);
  {
    LongLineIndex idx;
    LayoutOpts lo;
    epos cp; u32 cell;
    lo.tab_width = 8; lo.show_ws = 0; lo.pad[0] = lo.pad[1] = 0;
    TF_EQ_I(idx.build(d.bytes(), d.codec(), lo, 0, n, 0), E_OK);
    TF_CHECK(idx.total_cells() > n / 2);
    TF_EQ_I(idx.at_offset(n - 1, &cp, &cell), E_OK);
    TF_CHECK(cp <= n - 1 && n - 1 - cp <= 2 * LAY_CHECKPOINT);
  }
  {
    static const char* const pats[] = { "needle", "needle!$", "e.d.e", "[n]eedle" };
    u32 k;
    for (k = 0; k < 4; k++) {
      Searcher s;
      SearchOpts o;
      SearchError err;
      Vec<ecp> pat;
      RxMatch m;
      epos lo = k == 2 ? n - 1024 * 1024 : 0;          // the Pike VM case gets 1 MB (0.75 MB/s on the PII)
      o.mode = k == 0 ? SM_NORMAL : SM_REGEX; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
      adv_to_ecp(pats[k], &pat);
      TF_EQ_I(s.compile(pat.data(), pat.size(), o, d.enc(), &err), E_OK);
      TF_EQ_I(s.find(d.bytes(), d.codec(), lo, n, lo, 0, &m, 0), 1);
      TF_EQ_U(m.pos, k == 2 ? n - 6 : n - 7);                 // "e.d.e" = "eedle"
      TF_EQ_I(s.find(d.bytes(), d.codec(), lo, n, n, 1, &m, 0), 1);
      TF_EQ_U(m.pos, k == 2 ? n - 6 : n - 7);
    }
  }
  adv_highlight_all(tf, d, "cpp", "long line");
  {
    EdCtx e;
    memset(&e, 0, sizeof(e));
    e.doc = &d;
    ed_lang_from(0, 4, 0, &e.lang);
    e.sel.anchor = n - 3; e.sel.caret = n - 3;
    TF_EQ_I(ed_duplicate(&e), E_OK);
    TF_EQ_U(d.length(), 2 * n + 2);
    TF_EQ_U(d.line_count(), 2);
    TF_EQ_I(ed_move_lines(&e, 0), E_OK);
    TF_EQ_U(d.line_count(), 2);
    adv_undo_all(d);
    TF_EQ_U(d.length(), n);
  }
}

// A 4 GB - 2 document (on-demand path with a virtual source): search at both ends, edits at the ends.
static void adv_huge_search(TF* tf)
{
  AdvPeriodSource* src = new AdvPeriodSource(EPOS_MAX, 1009, 0x0A);
  Document d;
  Vec<u32> blocks;
  EolCounts ec;
  u32 nb = (u32)(((u64)EPOS_MAX + 65535) / 65536), i;
  Searcher s;
  SearchOpts o;
  SearchError err;
  Vec<ecp> pat;
  RxMatch m;
  for (i = 0; i < nb; i++) {
    epos a = i * 65536u, e = (i == nb - 1) ? EPOS_MAX : a + 65536u;
    blocks.push(src->breaks_in(a, e));
  }
  ec.crlf = 0; ec.lf = src->breaks_in(0, EPOS_MAX); ec.cr = 0;
  TF_EQ_I(d.init_source_indexing(src, enc_ansi(), 0, EOL_LF), E_OK);
  TF_EQ_I(d.finish_indexing(blocks.data(), nb, ec), E_OK);
  o.mode = SM_EXTENDED; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
  adv_to_ecp("\\n", &pat);
  TF_EQ_I(s.compile(pat.data(), pat.size(), o, d.enc(), &err), E_OK);
  TF_EQ_I(s.find(d.bytes(), d.codec(), 0, EPOS_MAX, EPOS_MAX, 1, &m, 0), 1);
  TF_EQ_U(m.pos, (epos)(((u64)EPOS_MAX + 1) / 1009 * 1009 - 1));
  TF_EQ_I(s.find(d.bytes(), d.codec(), 0, EPOS_MAX, EPOS_MAX - 5000, 0, &m, 0), 1);
  TF_CHECK(src->at(m.pos) == 0x0A && m.pos >= EPOS_MAX - 5000);
  TF_EQ_I(d.replace(EPOS_MAX - 1, 1, (const u8*)"zz", 2), E_TOOBIG);   // EPOS_MAX limit
  TF_EQ_I(d.replace(EPOS_MAX - 1, 1, (const u8*)"z", 1), E_OK);
  TF_EQ_I(d.replace(d.length(), 0, (const u8*)"z", 1), E_TOOBIG);
  TF_EQ_I(d.replace(0, 1, 0, 0), E_OK);
  TF_EQ_I(d.replace(d.length(), 0, (const u8*)"zq", 2), E_TOOBIG);
  TF_EQ_I(d.replace(d.length(), 0, (const u8*)"q", 1), E_OK);
  TF_EQ_U(d.length(), EPOS_MAX);
  {
    u8 tail[2];
    TF_EQ_I(d.read(EPOS_MAX - 2, tail, 2), E_OK);
    TF_CHECK(tail[0] == 'z' && tail[1] == 'q');
  }
  adv_to_ecp("zq", &pat);
  o.mode = SM_NORMAL;
  TF_EQ_I(s.compile(pat.data(), pat.size(), o, d.enc(), &err), E_OK);
  TF_EQ_I(s.find(d.bytes(), d.codec(), 0, EPOS_MAX, EPOS_MAX, 1, &m, 0), 1);
  TF_EQ_U(m.pos, EPOS_MAX - 2);
  adv_undo_all(d);
  TF_EQ_U(d.length(), EPOS_MAX);
}

// ---------------------------------------------------------------------------------------------------------------
// 3. Regex bombs, compile limits, garbage patterns, Replace All edge cases

static void adv_regex_bombs(TF* tf)
{
  static const char* const pats[] = {
    "(a*)*b", "(a|a)*b", "(a|aa)*c", "(a+)+b", "((a+)+)+b", "(.*)*x", "(a?){30}a{30}", "(?:a+){10}b",
    "(\\w+\\s?)*$", "(a*)*\\1b", "(?=(a+))a*b\\1", "(x+x+)+y", "(a|b|ab)*c", "[a-z]*[a-z]*[a-z]*[a-z]*z",
    "(?i)(A|a)*B", ".*.*.*.*.*=", "(a{1,5}){1,5}[bc]", "(a|a?)+b", "(?:(?:a*)*)*b", "^(a+)+$", "(a*)+\\1x", 0 };
  EBytes subj;
  MemSource src;
  const Codec* c = codec_get(enc_utf8(0));
  u32 i, n = 20000;
  double worst = 0;
  subj.resize(n);
  for (i = 0; i < n; i++) subj[i] = (u8)(i % 5000 == 4999 ? '\n' : 'a');
  subj[n - 1] = '!';
  src.copy_from(subj.data(), n);
  for (i = 0; pats[i]; i++) {
    Searcher s;
    SearchOpts o;
    SearchError err;
    Vec<ecp> pat;
    u32 cnt = 0;
    clock_t c0;
    double secs;
    int rc;
    o.mode = SM_REGEX; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
    adv_to_ecp(pats[i], &pat);
    rc = s.compile(pat.data(), pat.size(), o, enc_utf8(0), &err);
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "bomb %s: compile %s", pats[i], e_errname(rc)); continue; }
    c0 = clock();
    rc = s.count(&src, c, 0, n, &cnt, 0);
    secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    if (secs > worst) worst = secs;
    if (rc < 0 && rc != E_BUDGET) tf_fail(tf, __FILE__, __LINE__, "bomb %s: %s", pats[i], e_errname(rc));
    if (secs > 20.0) tf_fail(tf, __FILE__, __LINE__, "bomb %s took %.1f s", pats[i], secs);
    TF_CHECK(rc >= 0 || rc == E_BUDGET);
  }
  tf_log(tf, "  regex bombs: slowest %u ms on 20 KB", (u32)(worst * 1000));
}

static void adv_regex_limits(TF* tf)
{
  static const char* const bad[] = {
    "[", "(", ")", "a{2,1}", "\\", "[z-a]", "(?", "(?<=a)b", "a**", "a{1001}", "(?:a{1000}){1000}",
    "((a{100}){100}){100}", "\\x{110000}", "(?<n>a)", "\\k<n>", "(?|a)", "[[:nope:]]", "\\p{L}", "(?x)a", "a++",
    "(?>a)", "\\8", "x{,", "[\\", "(?i", "(?#", "\\Qabc", 0 };
  u32 i;
  for (i = 0; bad[i]; i++) {
    Vec<ecp> pat;
    RxProg* prog = 0;
    RxError err;
    int rc;
    adv_to_ecp(bad[i], &pat);
    memset(&err, 0x55, sizeof(err));
    rc = rx_compile(pat.data(), pat.size(), 0, &prog, &err);
    if (rc == E_OK) {                        // a few are legal in our dialect (literal '{', \Q to the end ...)
      rx_free(prog);
      continue;
    }
    TF_CHECK(prog == 0 || prog == (RxProg*)0);
    TF_CHECK(err.pos <= pat.size() && memchr(err.msg, 0, sizeof(err.msg)) != 0 && err.msg[0] != 0);
  }
  {
    Vec<ecp> pat;
    RxProg* prog = 0;
    RxError err;
    for (i = 0; i < 1001; i++) pat.push('a');
    TF_EQ_I(rx_compile(pat.data(), pat.size(), 0, &prog, &err), E_TOOBIG);
    pat.clear();
    for (i = 0; i < 101; i++) pat.push('(');
    pat.push('a');
    for (i = 0; i < 101; i++) pat.push(')');
    TF_EQ_I(rx_compile(pat.data(), pat.size(), 0, &prog, &err), E_TOOBIG);
    TF_EQ_I(rx_compile(pat.data(), 0, 0, &prog, &err), E_OK);   // empty pattern matches empty
    rx_free(prog);
  }
}

// Random token soup (including NUL, bad markers, lone surrogates, U+10FFFF) against invalid UTF-8.
static void adv_regex_garbage(TF* tf, u32 rounds)
{
  static const char* const tok[] = {
    "a", "b", ".", "*", "+", "?", "|", "(", ")", "(?:", "(?=", "(?!", "[", "]", "^", "$", "\\b", "\\w", "\\s", "\\d",
    "{2}", "{1,3}", "{,", "}", "\\1", "\\2", "-", "[^", "\\x{FFFD}", "\\n", "\\r", "(?i)", "*?", "+?", "\\<", "\\>",
    "\\A", "\\z", "[:alpha:]", "\\Q", "\\E", "\\", "\xC3\xA9", "\xE2\x82\xAC" };
  EBytes subj;
  MemSource src;
  const Codec* c = codec_get(enc_utf8(0));
  u32 r, i, ok = 0;
  for (i = 0; i < 400; i++) {
    u32 k = adv_rand(tf, 8);
    if (k == 0) subj.push((u8)(0x80 + adv_rand(tf, 128)));
    else if (k == 1) subj.push('\n');
    else if (k == 2) subj.append("\xC3\xA9", 2);
    else subj.push((u8)("ab x"[adv_rand(tf, 4)]));
  }
  src.copy_from(subj.data(), subj.size());
  for (r = 0; r < rounds; r++) {
    Vec<ecp> pat, t;
    Searcher s;
    SearchOpts o;
    SearchError err;
    u32 n = 1 + adv_rand(tf, 8), cnt = 0, j;
    Vec<MarkRange> all;
    RxMatch m;
    int rc;
    for (i = 0; i < n; i++) {
      u32 k = adv_rand(tf, 50);
      if (k < 44) { adv_to_ecp(tok[k], &t); pat.insert(pat.size(), t.data(), t.size()); }
      else if (k == 44) pat.push(0);
      else if (k == 45) pat.push(ECP_BADBYTE(0x80 + adv_rand(tf, 128)));
      else if (k == 46) pat.push(0xD800 + adv_rand(tf, 0x800));
      else if (k == 47) pat.push(0x10FFFF);
      else if (k == 48) pat.push(ECP_BADUNIT(0xDC00));
      else pat.push(0xFFFF);
    }
    o.mode = (u8)(adv_rand(tf, 4) ? SM_REGEX : adv_rand(tf, 2) ? SM_NORMAL : SM_EXTENDED);
    o.match_case = (u8)adv_rand(tf, 2); o.whole_word = (u8)(adv_rand(tf, 5) == 0); o.dot_all = (u8)adv_rand(tf, 2);
    rc = s.compile(pat.data(), pat.size(), o, enc_utf8(0), &err);
    if (rc != E_OK) { if (rc >= 0 || memchr(err.msg, 0, sizeof(err.msg)) == 0) tf_fail(tf, __FILE__, __LINE__, "garbage compile rc %d", rc); continue; }
    ok++;
    rc = s.count(&src, c, 0, subj.size(), &cnt, 0);
    if (rc < 0 && rc != E_BUDGET) { tf_fail(tf, __FILE__, __LINE__, "garbage count %s", e_errname(rc)); continue; }
    if (rc == E_BUDGET) continue;
    rc = s.find_all(&src, c, 0, subj.size(), &all, 100000, 0);
    if (rc < 0 || all.size() != cnt) { tf_fail(tf, __FILE__, __LINE__, "garbage find_all %d %u/%u", rc, all.size(), cnt); continue; }
    for (j = 0; j < all.size(); j++) if (all[j].pos + all[j].len > subj.size()) { tf_fail(tf, __FILE__, __LINE__, "garbage match out of range"); break; }
    rc = s.find(&src, c, 0, subj.size(), subj.size(), 1, &m, 0);
    if (rc < 0 && rc != E_BUDGET) tf_fail(tf, __FILE__, __LINE__, "garbage backward %s", e_errname(rc));
    if (rc == 1 && m.pos + m.len > subj.size()) tf_fail(tf, __FILE__, __LINE__, "garbage backward out of range");
    {
      EBytes out;
      Vec<ecp> rep;
      u32 unrep = 0;
      adv_to_ecp("[$0$1\\U$2\\E${9}$$\\x{1F600}]", &rep);
      if (cnt && all.size()) {
        rc = s.find(&src, c, 0, subj.size(), all[0].pos, 0, &m, 0);
        if (rc == 1) {
          rc = s.expand(rep.data(), rep.size(), &src, c, m, &out, &unrep);
          if (rc != E_OK && rc != E_SYNTAX) tf_fail(tf, __FILE__, __LINE__, "garbage expand %s", e_errname(rc));
        }
      }
    }
  }
  TF_CHECK(ok > rounds / 10);
}

class AdvDocSink : public EditSink {
public:
  Document* d;
  AdvDocSink(Document* dd) : d(dd) {}
  int apply(epos pos, epos del, const u8* ins, epos n) { return d->replace(pos, del, ins, n); }
};

struct AdvRepCase { const char* text; u8 enc; u8 mode; const char* pat; const char* rep; const char* expect; u32 unrep; };

static void adv_replace_edges(TF* tf)
{
  static const AdvRepCase cases[] = {
    { "a\nb\n", 0, SM_REGEX, "^", "> ", "> a\n> b\n> ", 0 },
    { "a\r\nb", 0, SM_REGEX, "$", "!", "a!\r\nb!", 0 },
    { "baaac", 0, SM_REGEX, "a*", "-", "-b--c-", 0 },
    { "aab", 0, SM_REGEX, "(a)(b)?", "[$2$1${2}]", "[a][bab]", 0 },
    { "abc", 0, SM_REGEX, "(\\w+)", "\\U$1\\E-\\u$1", "ABC-Abc", 0 },
    { "x", 0, SM_NORMAL, "x", "$0\\", "$0\\", 0 },
    { "x y x", 1, SM_REGEX, "x", "\\x{1F600}", "? y ?", 2 },
    { "a\r\nb\r\n", 0, SM_REGEX, "\\r\\n", "\\n", "a\nb\n", 0 },
    { "aa", 0, SM_REGEX, "(?=a)", "^", "^a^a", 0 },
    { "\xFF\xFE" "ab", 0, SM_REGEX, ".", "[$0]", "[\xFF][\xFE][a][b]", 0 },
    { "\xC3\xA9\xFF", 0, SM_REGEX, ".", "\\U$0", "\xC3\x89\xFF", 0 },
    { "aaa", 0, SM_REGEX, "a", "$$", "$$$", 0 },
    { "ab", 0, SM_REGEX, "(a)|(b)", "<$1|$2>", "<a|><|b>", 0 },
    { "a.b", 0, SM_EXTENDED, ".", "\\t", "a\tb", 0 },
    { "", 0, SM_REGEX, "^$", "E", "E", 0 },
    { "abc", 0, SM_REGEX, "(?:)", "-", "-a-b-c-", 0 },
    { "x\r\ny", 0, SM_REGEX, "\\r?$", "#", "x#\r\ny#", 0 },
    { "AbC", 0, SM_HEX, "62", "7A 7A", "AzzC", 0 },
    { 0, 0, 0, 0, 0, 0, 0 } };
  u32 i;
  for (i = 0; cases[i].text; i++) {
    const AdvRepCase& k = cases[i];
    Document d;
    EncSpec e = k.enc ? enc_ansi() : enc_utf8(0);
    Searcher s;
    SearchOpts o;
    SearchError err;
    Vec<ecp> pat, rep;
    AdvDocSink sink(&d);
    EBytes out;
    u32 nrep = 0, unrep = 0;
    Selection sel;
    int rc;
    TF_EQ_I(adv_doc_init(d, (const u8*)k.text, es_len(k.text), e, EOL_LF), E_OK);
    o.mode = k.mode; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
    adv_to_ecp(k.pat, &pat);
    adv_to_ecp(k.rep, &rep);
    rc = s.compile(pat.data(), pat.size(), o, e, &err);
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "replace case %u: compile %s", i, e_errname(rc)); continue; }
    sel.anchor = sel.caret = 0;
    d.begin_edit(UK_OTHER, sel, 0);
    rc = search_replace_all(s, d.bytes(), d.codec(), 0, d.length(), rep.data(), rep.size(), &sink, 0, &nrep, &unrep);
    d.end_edit(sel);
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "replace case %u: %s", i, e_errname(rc)); continue; }
    adv_doc_bytes(d, &out);
    if (out.size() != es_len(k.expect) || memcmp(out.data(), k.expect, out.size()) != 0) {
      EStr got;
      got.appendn((const char*)out.data(), out.size());
      tf_fail(tf, __FILE__, __LINE__, "replace case %u: got '%s', expected '%s'", i, got.c_str(), k.expect);
    }
    TF_EQ_U(unrep, k.unrep);
    adv_undo_all(d);
    TF_EQ_U(d.length(), es_len(k.text));
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 4. Lexer garbage: random code points (bad markers, NUL, surrogates, U+10FFFF) and states; convergence on garbage

static void adv_lexer_garbage(TF* tf, u32 per_lang)
{
  u32 li, r, i;
  Vec<ecp> s;
  Vec<u8> st;
  s.resize(3000);
  st.resize(3000);
  for (li = 0; li < lang_count(); li++) {
    const LangInfo* L = lang_at(li);
    u32 bad = 0;
    if (!L || !L->lexer) continue;
    for (r = 0; r < per_lang && !bad; r++) {
      u32 n = adv_rand(tf, r & 1 ? 3000 : 120);
      u16 state = (u16)(r == 0 ? 0 : adv_rand(tf, 0x7FFF)), out;
      for (i = 0; i < n; i++) {
        u32 k = adv_rand(tf, 20);
        ecp c;
        if (k < 8) c = (ecp)"/*\"'<>{}()[]#;-=`\\&!?:\t \x7F"[adv_rand(tf, 26)];
        else if (k < 12) c = 'a' + adv_rand(tf, 26);
        else if (k == 12) c = ECP_BADBYTE(adv_rand(tf, 256));
        else if (k == 13) c = ECP_BADUNIT(0xD800 + adv_rand(tf, 0x800));
        else if (k == 14) c = 0;
        else if (k == 15) c = 0x10FFFF;
        else if (k == 16) c = 0xD800 + adv_rand(tf, 0x800);
        else if (k == 17) c = adv_rand(tf, 0x110000);
        else c = '0' + adv_rand(tf, 10);
        s[i] = c;
      }
      memset(st.data(), 0xEE, n);
      out = L->lexer->lex_line(s.data(), n, state, st.data());
      if (out >= 0x7FFF) { tf_fail(tf, __FILE__, __LINE__, "lexer %s: state %u", L->id, (u32)out); bad = 1; }
      for (i = 0; i < n && !bad; i++) if (st[i] >= ST_COUNT) { tf_fail(tf, __FILE__, __LINE__, "lexer %s: style %u", L->id, (u32)st[i]); bad = 1; }
      if (!bad && L->lexer->lex_line(s.data(), n, state, 0) != out) { tf_fail(tf, __FILE__, __LINE__, "lexer %s: NULL styles differ", L->id); bad = 1; }
    }
    TF_CHECK(!bad);
  }
}

// Highlighter over a garbage document with random edits and undo: states always equal a fresh full lex.
static void adv_lexer_converge(TF* tf, u32 edits)
{
  u32 li;
  for (li = 0; li < lang_count(); li++) {
    const LangInfo* L = lang_at(li);
    Document d;
    EBytes b;
    Highlighter h;
    DocLines dl;
    u32 i, e, bad = 0;
    for (i = 0; i < 6000; i++) {
      u32 k = adv_rand(tf, 30);
      if (k == 0) b.push('\n');
      else if (k == 1) b.push((u8)(0x80 + adv_rand(tf, 128)));
      else if (k < 6) b.push((u8)"/*\"'<>{}#\\"[adv_rand(tf, 10)]);
      else b.push((u8)("ab =-"[adv_rand(tf, 5)]));
    }
    TF_EQ_I(adv_doc_init(d, b.data(), b.size(), enc_utf8(0), EOL_LF), E_OK);
    dl.bind(&d, &h);
    TF_EQ_I(h.attach(&dl, L), E_OK);
    for (e = 0; e < edits && !bad; e++) {
      u32 kind = adv_rand(tf, 10), guard = 0, l;
      if (kind < 7) {
        static const char* const ins[] = { "/*", "*/", "\"", "'", "<!--", "-->", "\n", "<script>", "```", "\\\n", "\xFF", "#", "{" };
        const char* t = ins[adv_rand(tf, 13)];
        epos pos = adv_rand(tf, d.length() + 1), del = adv_rand(tf, 4);
        if (del > d.length() - pos) del = d.length() - pos;
        d.replace(pos, del, (const u8*)t, es_len(t));
      } else {
        Selection s;
        d.undo(&s);
      }
      h.set_target(d.line_count() - 1);
      while (h.work(500) && guard < 10000) guard++;
      if (e % 10 == 9 || e + 1 == edits) {
        Highlighter fresh;
        DocLines dl2;
        dl2.bind(&d, &fresh);
        fresh.attach(&dl2, L);
        fresh.set_target(d.line_count() - 1);
        guard = 0;
        while (fresh.work(5000) && guard < 10000) guard++;
        for (l = 0; l < d.line_count(); l++) {
          u16 a = 0, c = 0;
          int ka = h.debug_state(l, &a), kc = fresh.debug_state(l, &c);
          if (ka != kc || a != c) { tf_fail(tf, __FILE__, __LINE__, "converge %s: edit %u line %u: %u/%u vs %u/%u", L->id, e, l, ka, (u32)a, kc, (u32)c); bad = 1; break; }
        }
        fresh.detach();
        dl2.unbind();
      }
    }
    TF_CHECK(!bad);
    h.detach();
    dl.unbind();
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 5. Settings, session, .lng, hex patterns and go-to expressions: truncation at every byte and mutations

static int adv_cstr_ok(const char* s, u32 cap) { return memchr(s, 0, cap) != 0; }

static int adv_config_sane(const Config& c)
{
  u32 i;
  if (c.font_pt < 6 || c.font_pt > 72 || c.font_px < 0 || c.font_px > 64) return 0;
  if (c.win_w < 200 || c.win_w > 16000 || c.win_h < 150 || c.win_h > 16000) return 0;
  if (c.win_x < -32000 || c.win_x > 32000 || c.win_y < -32000 || c.win_y > 32000) return 0;
  if (c.find_mode > 3 || c.new_eol > 2) return 0;
  if (!adv_cstr_ok(c.font_face, sizeof(c.font_face))) return 0;
  for (i = 0; i < CFG_MRU_MAX; i++) if (!adv_cstr_ok(c.mru[i], sizeof(c.mru[i]))) return 0;
  for (i = 0; i < CFG_HIST_MAX; i++) {
    if (!adv_cstr_ok(c.find_hist[i], 256) || !adv_cstr_ok(c.repl_hist[i], 256) || !adv_cstr_ok(c.fif_dirs[i], 256) ||
        !adv_cstr_ok(c.fif_masks[i], 256)) return 0;
  }
  return 1;
}

static void adv_mutate(TF* tf, const EStr& in, EBytes* out)
{
  static const char junk[] = "[]=\r\n\0;#\"' \t\xFF\xC3=0x-9";
  u32 n = 1 + adv_rand(tf, 4), k;
  out->clear();
  out->append(in.c_str(), in.len());
  for (k = 0; k < n; k++) {
    u32 at = adv_rand(tf, out->size() + 1), what = adv_rand(tf, 4);
    u8 c = (u8)(adv_rand(tf, 2) ? junk[adv_rand(tf, sizeof(junk) - 1)] : adv_rand(tf, 256));
    if (what == 0 && at < out->size()) (*out)[at] = c;
    else if (what == 1) out->insert(at, &c, 1);
    else if (what == 2 && at < out->size()) out->erase(at, 1 + adv_rand(tf, 20));
    else if (out->size()) { u32 b = adv_rand(tf, out->size()); u32 l = adv_rand(tf, 60); if (l > out->size() - b) l = out->size() - b; { EBytes cp; cp.append(out->data() + b, l); out->insert(at, cp.data(), cp.size()); } }
  }
}

static void adv_config_corrupt(TF* tf, u32 mutations)
{
  Config c, c2;
  Ini ini;
  EStr text;
  u32 n, i, bad = 0;
  config_defaults(&c);
  config_mru_add(&c, "C:\\a b\\file one.txt");
  config_mru_add(&c, "M:\\dev\\x;y=z.cpp");
  config_hist_add(c.find_hist, CFG_HIST_MAX, " lead and trail ");
  config_hist_add(c.find_hist, CFG_HIST_MAX, "\"quoted\"");
  es_copy(c.font_face, sizeof(c.font_face), "Lucida Console");
  config_store(&c, &ini);
  TF_EQ_I(ini.write(&text), E_OK);
  n = text.len();
  for (i = 0; i <= n && !bad; i++) {           // truncation at every byte
    Ini t;
    if (t.parse(text.c_str(), i) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "config parse trunc %u", i); bad = 1; break; }
    config_load(&c2, t);
    if (!adv_config_sane(c2)) { tf_fail(tf, __FILE__, __LINE__, "config trunc %u not sane", i); bad = 1; }
  }
  {                                            // the untruncated text loads back exactly
    Ini t;
    EStr t2;
    t.parse(text.c_str(), n);
    config_load(&c2, t);
    config_store(&c2, &t);
    t.write(&t2);
    TF_EQ_S(t2.c_str(), text.c_str());
  }
  for (i = 0; i < mutations && !bad; i++) {
    EBytes m;
    Ini t, t3;
    EStr w1, w2;
    adv_mutate(tf, text, &m);
    if (t.parse((const char*)m.data(), m.size()) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "config parse mutation"); bad = 1; break; }
    config_load(&c2, t);
    if (!adv_config_sane(c2)) { tf_fail(tf, __FILE__, __LINE__, "config mutation %u not sane", i); bad = 1; }
    t.write(&w1);                              // write -> parse -> write is stable
    t3.parse(w1.c_str(), w1.len());
    t3.write(&w2);
    if (strcmp(w1.c_str(), w2.c_str()) != 0) { tf_fail(tf, __FILE__, __LINE__, "ini write not stable after mutation %u", i); bad = 1; }
  }
  TF_CHECK(!bad);
}

static void adv_session_text(EStr* out)
{
  Session s;
  Ini ini;
  u32 i;
  session_init(&s);
  s.seq = 77; s.active = 2; s.next_tabid = 9; s.next_untitled = 3; s.clean_exit = 0;
  for (i = 0; i < 5; i++) {
    SessionTab* t = new SessionTab;
    memset((void*)t, 0, (size_t)((char*)&t->bookmarks - (char*)t));   // every field before the Vec
    t->id = i + 3;
    t->untitled = (u8)(i == 1);
    t->untitled_no = i == 1 ? 2 : 0;
    if (!t->untitled) es_fmt(t->path, sizeof(t->path), "C:\\dir %u\\f=%u;x.txt", i, i);
    t->enc = i == 3 ? enc_utf16le(1) : enc_utf8(0);
    t->eol = (u8)(i % 3); t->tab_width = (u8)(1 + i * 3); t->caret = i * 1000; t->anchor = i * 999;
    t->hex_top = 0x10 * i; t->disk_size = 4096 + i; t->disk_mtime_hi = 0x01D0 + i; t->disk_mtime_lo = 0xDEADBEEF;
    es_copy(t->lang, sizeof(t->lang), i == 2 ? "python" : "cpp");
    t->bookmarks.push(i); t->bookmarks.push(i * 10 + 5); t->bookmarks.push(4000000000u);
    s.tabs.push(t);
  }
  session_to_ini(s, &ini);
  ini.write(out);
  session_free(&s);
}

static int adv_session_sane(const Session& s)
{
  u32 i, j;
  if (s.tabs.size() > 4096 || (s.tabs.size() && s.active >= s.tabs.size())) return 0;
  for (i = 0; i < s.tabs.size(); i++) {
    const SessionTab* t = s.tabs[i];
    if (t->id == 0 || t->tab_width < 1 || t->tab_width > 16 || t->eol > 2 || t->caret > EPOS_MAX) return 0;
    if (!adv_cstr_ok(t->path, sizeof(t->path)) || !adv_cstr_ok(t->lang, sizeof(t->lang))) return 0;
    if (!t->untitled && !t->path[0]) return 0;
    if (t->id >= s.next_tabid && t->id != 0xFFFFFFFFu) return 0;
    for (j = 1; j < t->bookmarks.size(); j++) if (t->bookmarks[j] <= t->bookmarks[j - 1]) return 0;
    for (j = 0; j < i; j++) if (s.tabs[j]->id == t->id) return 0;
  }
  return 1;
}

static void adv_session_corrupt(TF* tf, u32 mutations)
{
  EStr text, full;
  u32 n, i, bad = 0, okcount = 0;
  adv_session_text(&text);
  n = text.len();
  {
    Ini t;
    Session s;
    session_init(&s);
    t.parse(text.c_str(), n);
    TF_EQ_I(session_from_ini(t, &s), E_OK);
    TF_EQ_U(s.tabs.size(), 5);
    session_to_ini(s, &t);
    t.write(&full);
    session_free(&s);
  }
  for (i = 0; i <= n + mutations && !bad; i++) {
    Ini t;
    Session s;
    EBytes m;
    int rc;
    u32 vseq = 0;
    if (i <= n) { m.append(text.c_str(), i); }
    else adv_mutate(tf, text, &m);
    session_init(&s);
    t.parse((const char*)m.data(), m.size());
    rc = session_from_ini(t, &s);
    if (rc != E_OK && rc != E_FORMAT) { tf_fail(tf, __FILE__, __LINE__, "session %u: %s", i, e_errname(rc)); bad = 1; }
    if (rc == E_FORMAT && s.tabs.size()) { tf_fail(tf, __FILE__, __LINE__, "session %u: E_FORMAT with tabs", i); bad = 1; }
    if (rc == E_OK) {
      okcount++;
      if (!adv_session_sane(s)) { tf_fail(tf, __FILE__, __LINE__, "session %u: accepted but not sane", i); bad = 1; }
      if (i <= n) {                          // a truncated text that still parses is the whole session
        Ini t2;
        EStr w;
        session_to_ini(s, &t2);
        t2.write(&w);
        if (strcmp(w.c_str(), full.c_str()) != 0) { tf_fail(tf, __FILE__, __LINE__, "session truncated at %u parses differently", i); bad = 1; }
      }
    }
    if ((session_validate(m.data(), m.size(), &vseq) == E_OK) != (rc == E_OK)) { tf_fail(tf, __FILE__, __LINE__, "session_validate disagrees at %u", i); bad = 1; }
    session_free(&s);
  }
  TF_CHECK(!bad);
  TF_CHECK(okcount >= 1);
}

static void adv_lng_corrupt(TF* tf, u32 mutations)
{
  u32 f, i;
  if (!g_adv_langs || g_adv_langs->count() != 13) return;
  for (f = 0; f < 13; f++) {
    const EBytes& src = g_adv_langs->t_[f];
    u32 n = src.size(), step = (f == 4 || f == 12) ? 1 : 97, bad = 0;   // ini.lng and diff.lng at every byte
    EStr base;
    base.appendn((const char*)src.data(), n);
    for (i = 0; i <= n + mutations && !bad; i += (i < n ? step : 1)) {
      LangInfo li;
      EStr err;
      EBytes m;
      int rc;
      if (i <= n) m.append(src.data(), i);
      else adv_mutate(tf, base, &m);
      rc = lang_parse((const char*)m.data(), m.size(), &li, &err);
      if (rc != E_OK && rc != E_SYNTAX) {
        char dp[400];
        FILE* df;
        tf_path(tf, "lng_fail.bin", dp, sizeof(dp));
        df = fopen(dp, "wb");
        if (df) { fwrite(m.data(), 1, m.size(), df); fclose(df); }
        tf_fail(tf, __FILE__, __LINE__, "lng %s at %u: %s (%s; input in %s)", g_adv_lng[f], i, e_errname(rc), err.c_str(), dp);
        bad = 1;
      }
      if (rc == E_SYNTAX && err.len() == 0) { tf_fail(tf, __FILE__, __LINE__, "lng %s at %u: no message", g_adv_lng[f], i); bad = 1; }
      if (rc == E_OK) {
        Vec<ecp> s;
        Vec<u8> st;
        adv_to_ecp("int x = 1; /* a */ \"s\" <a b='c'> # -- ;", &s);
        st.resize(s.size());
        if (!adv_cstr_ok(li.id, sizeof(li.id)) || !adv_cstr_ok(li.name, sizeof(li.name)) || !adv_cstr_ok(li.exts, sizeof(li.exts))) { tf_fail(tf, __FILE__, __LINE__, "lng strings"); bad = 1; }
        else if (li.lexer && li.lexer->lex_line(s.data(), s.size(), 0, st.data()) >= 0x7FFF) { tf_fail(tf, __FILE__, __LINE__, "lng lexer state"); bad = 1; }
      }
      if (i == n && step > 1) i = n;         // the full text is also parsed with a large step
    }
    TF_CHECK(!bad);
  }
}

// Regression (T16 bug 1): a 48-character keyword (KW_MAX_LEN + 1) was reported as "out of memory" (E_NOMEM).
static void adv_lng_long_keyword(TF* tf)
{
  u32 n;
  for (n = 46; n <= 50; n++) {
    EStr text, err;
    LangInfo li;
    u32 i;
    int rc;
    text.append("name=Test\r\nkeywords1=short ");
    for (i = 0; i < n; i++) text.append("k");
    text.append(" other\r\n");
    rc = lang_parse(text.c_str(), text.len(), &li, &err);
    if (n <= 47) TF_EQ_I(rc, E_OK);
    else {
      TF_EQ_I(rc, E_SYNTAX);
      TF_CHECK(strstr(err.c_str(), "out of memory") == 0 && err.len() > 0);
    }
  }
}

static void adv_hexgoto_garbage(TF* tf, u32 rounds)
{
  static const char alpha[] = "0123456789abcdefABCDEFxXhH? ,;\r\n\t#d+-%:endEND";
  u32 r, i, bad = 0;
  for (r = 0; r < rounds && !bad; r++) {
    char t[64];
    u32 n = adv_rand(tf, 40);
    EBytes b, mask;
    Vec<ecp> e;
    epos off = 0, size = adv_rand(tf, 3) ? adv_rand(tf, 100000) : EPOS_MAX;
    u32 line = 0, col = 0, lines = 1 + adv_rand(tf, 100000);
    char err[128];
    int rc;
    for (i = 0; i < n; i++) t[i] = adv_rand(tf, 8) ? alpha[adv_rand(tf, sizeof(alpha) - 1)] : (char)(1 + adv_rand(tf, 255));
    t[n] = 0;
    rc = hex_parse_bytes(t, n, &b);
    if (rc == E_OK && b.size() == 0) { tf_fail(tf, __FILE__, __LINE__, "hex_parse_bytes '%s' empty ok", t); bad = 1; }
    for (i = 0; i < n; i++) e.push((u8)t[i]);
    rc = hex_parse_pattern(e.data(), e.size(), &b, &mask);
    if (rc == E_OK && (b.size() == 0 || b.size() != mask.size())) { tf_fail(tf, __FILE__, __LINE__, "hex_parse_pattern '%s'", t); bad = 1; }
    err[0] = 0;
    rc = goto_parse_offset(t, adv_rand(tf, size + 1), size, &off, err, sizeof(err));
    if (rc == E_OK && off > size) { tf_fail(tf, __FILE__, __LINE__, "goto offset '%s' -> %u > %u", t, off, size); bad = 1; }
    if (rc != E_OK && (!adv_cstr_ok(err, sizeof(err)) || !err[0])) { tf_fail(tf, __FILE__, __LINE__, "goto offset '%s' no message", t); bad = 1; }
    rc = goto_parse_line(t, adv_rand(tf, lines), lines, &line, &col, err, sizeof(err));
    if (rc == E_OK && line >= lines) { tf_fail(tf, __FILE__, __LINE__, "goto line '%s' -> %u of %u", t, line, lines); bad = 1; }
  }
  TF_CHECK(!bad);
}

// ---------------------------------------------------------------------------------------------------------------
// 6. Backup files: every byte flipped, junk appended, duplicated trailer

static void adv_backup_corrupt(TF* tf)
{
  FileSys* fs = fs_os();
  char dir[300], full[400], name[13];
  BackupMeta m, *got;
  Vec<BackupMeta> list;
  EBytes good, data;
  ByteSource* src = 0;
  MemSource ms;
  u32 i, hlen = 0, n, accepted = 0, bad = 0;
  epos dlen = 0;
  static const u8 body[] = "hello\r\nworld\xFF\x00\xC3\xA9 last line";
  tf_path(tf, "advbak", dir, sizeof(dir));
  fs->mkdir(dir);
  backup_delete_tab(fs, dir, 0x123);
  memset(&m, 0, sizeof(m));
  m.tabid = 0x123; m.gen = 7; m.seq = 99; es_copy(m.path, sizeof(m.path), "C:\\x y\\a=b;c.txt");
  m.enc = enc_utf8(0); m.eol = EOL_CRLF; es_copy(m.lang, sizeof(m.lang), "cpp"); m.caret = 5; m.anchor = 3;
  m.disk_size = 10; m.disk_mtime_hi = 1; m.disk_mtime_lo = 2; m.written_hi = 3; m.written_lo = 4;
  ms.copy_from(body, sizeof(body) - 1);
  TF_EQ_I(backup_write(fs, dir, m, &ms, 0, 0), E_OK);
  backup_file_name(m.tabid, m.gen, name);
  es_fmt(full, sizeof(full), "%s/%s", dir, name);
  TF_EQ_I(file_read_all(fs, full, &good, 1 << 20), E_OK);
  TF_EQ_I(backup_header_parse(good.data(), good.size(), &m, &hlen, &dlen), E_OK);
  n = good.size();
  for (i = 0; i <= n + 2 && !bad; i++) {
    EBytes f;
    int rc;
    f.append(good.data(), n);
    if (i < n) f[i] ^= (u8)(i & 1 ? 0x01 : 0x20);
    else if (i == n) f.push('x');                                  // junk appended
    else if (i == n + 1) f.append(BACKUP_TRAILER, 17);             // trailer twice
    else { f.erase(hlen, 1); }                                     // one data byte missing
    TF_EQ_I(file_write_direct(fs, full, f.data(), f.size()), E_OK);
    rc = backup_list(fs, dir, &list);
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "backup_list after flip %u: %s", i, e_errname(rc)); bad = 1; break; }
    got = list.size() ? &list[0] : 0;
    if (got && (got->tabid != m.tabid || got->gen != m.gen)) got = 0;
    if (i >= n || i < 10 || i >= n - 17 || (i >= hlen - 12 && i < hlen)) {
      // structure (magic, @DATA line, trailer, size) damaged: must be rejected
      if (got && !(i >= hlen - 12 && i < hlen - 2 && f[i] >= '0' && f[i] <= '9' && 0)) {
        if (i < hlen - 12 || i >= hlen) { tf_fail(tf, __FILE__, __LINE__, "backup with byte %u damaged was accepted", i); bad = 1; }
      }
    }
    if (got) {
      accepted++;
      rc = backup_open(fs, dir, *got, &src);
      if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "listed backup %u does not open: %s", i, e_errname(rc)); bad = 1; }
      else {
        if (src->size() + 17 > f.size()) { tf_fail(tf, __FILE__, __LINE__, "backup %u size", i); bad = 1; }
        if (i < hlen || i >= hlen + dlen) {   // header damage that still parses: data exact
          data.resize(src->size());
          src->read(0, data.data(), src->size());
          if (src->size() != sizeof(body) - 1 || memcmp(data.data(), body, sizeof(body) - 1) != 0) { tf_fail(tf, __FILE__, __LINE__, "backup %u data", i); bad = 1; }
        }
        delete src;
        src = 0;
      }
    }
  }
  TF_CHECK(!bad);
  TF_CHECK(accepted > 0);
  backup_delete_tab(fs, dir, 0x123);

}

// ---------------------------------------------------------------------------------------------------------------
// 7. Undo stress: random edits with undo/redo, groups and save points against a flat model with its own history;
//    random allocation failures (the edit fails with E_NOMEM and nothing changes); memory bounded

struct AdvMOp { epos pos; u32 old_off, old_len, new_off, new_len; };
struct AdvMGroup { u32 op_first, nops; };

struct AdvModel {
  EBytes cur, pool;
  Vec<AdvMOp> ops;
  Vec<AdvMGroup> groups;
  u32 ncur, saved;
  int saved_valid;
  AdvModel() : ncur(0), saved(0), saved_valid(1) {}
  void begin()
  {
    AdvMGroup g;
    if (ncur < groups.size()) {
      if (saved_valid && saved > ncur) saved_valid = 0;
      ops.resize(groups[ncur].op_first);
      groups.resize(ncur);
    }
    g.op_first = ops.size(); g.nops = 0;
    groups.push(g);
    ncur++;
  }
  void rec(epos pos, epos del, const u8* ins, epos n)
  {
    AdvMOp op;
    op.pos = pos;
    op.old_off = pool.size(); op.old_len = del; pool.append(cur.data() + pos, del);
    op.new_off = pool.size(); op.new_len = n; pool.append(ins, n);
    ops.push(op);
    groups[ncur - 1].nops++;
    cur.erase(pos, del);
    cur.insert(pos, ins, n);
  }
  void drop_empty() { if (ncur && groups[ncur - 1].nops == 0) { groups.pop(); ncur--; } }
  int undo()
  {
    u32 i;
    AdvMGroup g;
    if (ncur == 0) return 0;
    g = groups[--ncur];
    for (i = g.nops; i > 0; i--) {
      const AdvMOp& op = ops[g.op_first + i - 1];
      cur.erase(op.pos, op.new_len);
      cur.insert(op.pos, pool.data() + op.old_off, op.old_len);
    }
    return 1;
  }
  int redo()
  {
    u32 i;
    AdvMGroup g;
    if (ncur >= groups.size()) return 0;
    g = groups[ncur++];
    for (i = 0; i < g.nops; i++) {
      const AdvMOp& op = ops[g.op_first + i];
      cur.erase(op.pos, op.old_len);
      cur.insert(op.pos, pool.data() + op.new_off, op.new_len);
    }
    return 1;
  }
  bool modified() const { return !saved_valid || saved != ncur; }
};

static int adv_same(Document& d, const EBytes& m)
{
  EBytes b;
  adv_doc_bytes(d, &b);
  return b.size() == m.size() && memcmp(b.data(), m.data(), m.size()) == 0;
}

static void adv_undo_stress(TF* tf, u32 nops)
{
  Document d;
  AdvModel m;
  EBytes ins;
  u32 op, bad = 0, nomem = 0, now = 0;
  u32 base_allocs;
  epos inserted = 0;
  static const u8 init[] = "first line\r\nsecond\r\n\r\nthird line here\r\n";
  TF_EQ_I(adv_doc_init(d, init, sizeof(init) - 1, enc_ansi(), EOL_CRLF), E_OK);
  m.cur.append(init, sizeof(init) - 1);
  base_allocs = e_alloc_count();
  for (op = 0; op < nops && !bad; op++) {
    u32 kind = adv_rand(tf, 100), len = d.length();
    int fail = adv_rand(tf, 50) == 0, rc;
    Selection sel;
    now += adv_rand(tf, 3) ? 100 : 5000;
    if (kind < 55) {                          // one edit (typing kinds merge, others don't)
      epos pos = adv_rand(tf, len + 1), del = adv_rand(tf, 4) ? 0 : adv_rand(tf, 30);
      u32 n = adv_rand(tf, 3) ? 1 : adv_rand(tf, 40), i, uk = adv_rand(tf, 3) ? UK_TYPING : UK_OTHER;
      if (len > 16384) { del = adv_rand(tf, 400); n = 0; }   // keep the model small (memmove cost on the VM)
      if (del > len - pos) del = len - pos;
      ins.resize(n);
      for (i = 0; i < n; i++) ins[i] = (u8)(adv_rand(tf, 8) ? 'a' + adv_rand(tf, 26) : 0x0A);
      sel.anchor = sel.caret = pos;
      d.begin_edit(uk, sel, now);
      if (fail) e_test_fail_after(1 + adv_rand(tf, 6));
      rc = d.replace(pos, del, ins.data(), n);
      e_test_fail_after(-1);
      sel.anchor = sel.caret = pos + n;
      d.end_edit(sel);
      if (rc == E_OK) {
        if (del || n) {
          // merging is the Document's business; the model treats every edit as its own group unless it merged
          m.begin();
          m.rec(pos, del, ins.data(), n);
        }
        inserted += n;
      } else if (rc == E_NOMEM) {
        nomem++;
        if (!adv_same(d, m.cur)) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: E_NOMEM changed the bytes", op); bad = 1; }
      } else { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: %s", op, e_errname(rc)); bad = 1; }
      d.break_undo_merge();                   // keep group boundaries identical to the model
    } else if (kind < 75) {
      if (fail) e_test_fail_after(1 + adv_rand(tf, 6));
      rc = d.undo(&sel);
      e_test_fail_after(-1);
      if (rc == E_OK) m.undo();
      else if (rc == E_NOMEM) { nomem++; if (!adv_same(d, m.cur)) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: undo E_NOMEM changed bytes", op); bad = 1; } }
      else if (rc != E_STATE || m.ncur != 0) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: undo %s", op, e_errname(rc)); bad = 1; }
    } else if (kind < 92) {
      if (fail) e_test_fail_after(1 + adv_rand(tf, 6));
      rc = d.redo(&sel);
      e_test_fail_after(-1);
      if (rc == E_OK) m.redo();
      else if (rc == E_NOMEM) { nomem++; if (!adv_same(d, m.cur)) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: redo E_NOMEM changed bytes", op); bad = 1; } }
      else if (rc != E_STATE || m.ncur != m.groups.size()) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: redo %s", op, e_errname(rc)); bad = 1; }
    } else if (kind < 97) {                   // multi-op group
      u32 k, nk = 2 + adv_rand(tf, 4), did = 0;
      sel.anchor = sel.caret = 0;
      d.begin_edit(UK_OTHER, sel, now);
      for (k = 0; k < nk; k++) {
        epos L = d.length(), pos = adv_rand(tf, L + 1), del = adv_rand(tf, 5);
        if (del > L - pos) del = L - pos;
        if (d.replace(pos, del, (const u8*)"XY\n", 3) == E_OK) {
          if (!did) m.begin();
          did = 1;
          m.rec(pos, del, (const u8*)"XY\n", 3);
        }
      }
      d.end_edit(sel);
      d.break_undo_merge();
    } else {
      d.set_saved();
      m.saved = m.ncur; m.saved_valid = 1;
    }
    if (!bad && (op % 97 == 0 || op + 1 == nops)) {
      if (!adv_same(d, m.cur)) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: bytes differ from the model", op); bad = 1; }
      else if (d.modified() != m.modified()) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: modified %d, model %d", op, (int)d.modified(), (int)m.modified()); bad = 1; }
      else if (d.can_undo() != (m.ncur > 0) || d.can_redo() != (m.ncur < m.groups.size())) { tf_fail(tf, __FILE__, __LINE__, "undo stress op %u: can_undo/redo", op); bad = 1; }
      else if (d.tree()->piece_count() > 3 * op + 10) { tf_fail(tf, __FILE__, __LINE__, "undo stress: %u pieces after %u ops", d.tree()->piece_count(), op); bad = 1; }
      else if (d.tree()->add_buffer()->bytes_used() > inserted + 3 * op + 16) { tf_fail(tf, __FILE__, __LINE__, "undo stress: add buffer %u for %u inserted", d.tree()->add_buffer()->bytes_used(), inserted); bad = 1; }
    }
  }
  TF_CHECK(!bad);
  TF_CHECK(nomem > 0);
  // undo everything, then redo everything
  while (m.undo()) TF_EQ_I(d.undo(0), E_OK);
  TF_EQ_I(d.undo(0), E_STATE);
  TF_CHECK(adv_same(d, m.cur));
  while (m.redo()) { if (d.redo(0) != E_OK) { TF_CHECK(0); break; } }
  TF_CHECK(adv_same(d, m.cur));
  TF_CHECK(e_alloc_count() >= base_allocs);
  // the UndoLog alone: memory per recorded op stays small
  {
    UndoLog log;
    PieceVec refs;
    PieceRef r;
    UndoSel us;
    u32 i, N = nops;
    r.src = 1; r.flags = 0; r.start = 0; r.len = 1; r.nbreaks = 0;
    refs.push(r);
    us.anchor = us.caret = 0;
    for (i = 0; i < N; i++) {
      log.begin(UK_OTHER, us, i * 10000);
      log.rec_insert(i, 1, refs);
      log.end(us);
      log.break_merge();
    }
    tf_log(tf, "  undo log: %u bytes for %u groups", log.memory_bytes(), N);
    TF_CHECK(log.memory_bytes() <= N * 120 + 4096);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 8. Convert / reinterpret round trips across every codec

static void adv_codec_roundtrips(TF* tf)
{
  Vec<EncSpec> targets;
  u32 cp, t, nsbcs = 0, bad = 0;
  const Codec* u8c = codec_get(enc_utf8(0));
  for (cp = 1; cp < 65536; cp++) if (codec_sbcs_supported((u16)cp)) { targets.push(enc_cp((u16)cp)); nsbcs++; }
  targets.push(enc_utf8(0)); targets.push(enc_utf8(1)); targets.push(enc_utf16le(0)); targets.push(enc_utf16le(1));
  targets.push(enc_utf16be(0)); targets.push(enc_utf16be(1));
  tf_log(tf, "  codec round trips: %u SBCS code pages + 6 Unicode encodings", nsbcs);
  TF_CHECK(nsbcs >= 30);
  for (t = 0; t < targets.size() && !bad; t++) {
    const Codec* c = codec_get(targets[t]);
    Vec<ecp> chars;
    EBytes src8, expect, now;
    Document d;
    ConvertReport rep;
    u32 b, unrep = 0;
    u8 bom[4];
    u32 bl;
    char lab[40];
    enc_label(targets[t], lab, sizeof(lab));
    if (!c) { tf_fail(tf, __FILE__, __LINE__, "no codec for %s", lab); continue; }
    if (c->kind == EK_SBCS) {
      for (b = 1; b < 256; b++) {
        u8 by = (u8)b;
        ecp x;
        if (b == 0x0D || b == 0x0A) continue;
        if (c->decode(&by, 1, 1, &x) == 1 && !ECP_IS_BAD(x)) chars.push(x);
        if ((b & 15) == 15) { chars.push(0x0D); chars.push(0x0A); }
      }
    } else {
      static const ecp uni[] = { 'A', 0xE9, 0x20AC, 0x4E2D, 0x1F600, 0xFEFF, 0xFFFD, 0x10FFFF, 0x0D, 0x0A, 0x7F, 0x80,
                                 0x0, 0x0D, 'z', 0x0A, 0xD7FF, 0xE000, 0x0A };
      for (b = 0; b < 19; b++) chars.push(uni[b]);
      chars.insert(0, &uni[5], 1);           // U+FEFF as the first character (a "BOM" inside the text)
    }
    if (codec_encode_str(u8c, chars.data(), chars.size(), &src8, &unrep, 0) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: utf8 encode", lab); continue; }
    TF_EQ_I(adv_doc_init(d, src8.data(), src8.size(), enc_utf8(0), EOL_CRLF), E_OK);
    TF_EQ_I(d.check_convert(targets[t], &rep, 0), E_OK);
    if (rep.unrepresentable || rep.invalid_src) { tf_fail(tf, __FILE__, __LINE__, "%s: check_convert %u/%u", lab, rep.unrepresentable, rep.invalid_src); bad = 1; break; }
    TF_EQ_I(d.convert(targets[t], &rep, 0), E_OK);
    bl = enc_bom(targets[t], bom);
    expect.append(bom, bl);
    {
      EBytes body;
      codec_encode_str(c, chars.data(), chars.size(), &body, &unrep, 0);
      expect.append(body.data(), body.size());
    }
    if (!adv_same(d, expect)) { tf_fail(tf, __FILE__, __LINE__, "%s: converted bytes differ", lab); bad = 1; break; }
    TF_EQ_U(d.text_origin(), bl);
    if (adv_line_checks(tf, d, lab) || adv_char_checks(tf, d, lab)) { bad = 1; break; }
    TF_EQ_I(d.convert(enc_utf8(0), &rep, 0), E_OK);
    if (!adv_same(d, src8)) { tf_fail(tf, __FILE__, __LINE__, "%s: converting back differs", lab); bad = 1; break; }
    if (!enc_equal(targets[t], enc_utf8(0))) {   // converting to the same encoding records no undo step
      TF_EQ_I(d.undo(0), E_OK);
      TF_CHECK(adv_same(d, expect) && enc_equal(d.enc(), targets[t]));
      TF_EQ_I(d.undo(0), E_OK);
      TF_CHECK(adv_same(d, src8) && enc_equal(d.enc(), enc_utf8(0)));
      TF_EQ_I(d.redo(0), E_OK);
      TF_CHECK(adv_same(d, expect));
    } else TF_EQ_I(d.undo(0), E_STATE);
    // reinterpret: as UTF-8 and back, bytes untouched, BOM re-detected
    TF_EQ_I(d.reinterpret(enc_utf8(0), 0, 0), E_OK);
    TF_EQ_I(d.reinterpret(targets[t], bl, 0), E_OK);
    TF_CHECK(adv_same(d, expect) && d.text_origin() == bl);
    // lossy: characters the target can't store, counted exactly; first_bad = their first byte offset
    if (c->kind == EK_SBCS) {
      Vec<ecp> lossy;
      EBytes l8;
      Document d2;
      u32 k, expect_bad = 0;
      epos first = EPOS_NONE, off = 0;
      static const ecp extra[] = { 0x4E2D, 'a', 0x1F600, 0x3B1, 0x416, 0x5D0, 0x20AC, 0xE9, 0x2603 };
      for (k = 0; k < 9; k++) {
        u8 tmp[4], u[4];
        u32 ul = u8c->encode(extra[k], u);
        if (c->encode(extra[k], tmp) == 0) { expect_bad++; if (first == EPOS_NONE) first = off; }
        lossy.push(extra[k]);
        off += ul;
      }
      codec_encode_str(u8c, lossy.data(), lossy.size(), &l8, &unrep, 0);
      TF_EQ_I(adv_doc_init(d2, l8.data(), l8.size(), enc_utf8(0), EOL_CRLF), E_OK);
      TF_EQ_I(d2.check_convert(targets[t], &rep, 0), E_OK);
      TF_EQ_U(rep.unrepresentable, expect_bad);
      if (expect_bad) TF_EQ_U(rep.first_bad, first);
    }
  }
  // invalid source bytes: one U+FFFD each in Unicode targets, '?' in ANSI
  {
    static const u8 inv[] = "a\xFF" "b\xC3" "\xE2\x82" "c\xED\xA0\x80";
    Document d;
    ConvertReport rep;
    TF_EQ_I(adv_doc_init(d, inv, sizeof(inv) - 1, enc_utf8(0), EOL_CRLF), E_OK);
    TF_EQ_I(d.check_convert(enc_utf16le(0), &rep, 0), E_OK);
    TF_EQ_U(rep.invalid_src, 7);
    TF_EQ_U(rep.first_bad, 1);
    TF_EQ_I(d.convert(enc_utf16le(0), &rep, 0), E_OK);
    TF_EQ_U(d.length(), 2 * 10);
    TF_EQ_I(d.convert(enc_ansi(), &rep, 0), E_OK);
    TF_EQ_U(rep.unrepresentable, 7);         // the U+FFFDs can't be stored in ANSI
    TF_EQ_U(d.length(), 10);
    adv_undo_all(d);
    TF_CHECK(d.length() == sizeof(inv) - 1);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 9. Out of memory at every allocation in the hot paths
//
// For n = 1, 2, ...: make the n-th allocation fail, run the operation, check that it either succeeded completely or
// failed cleanly (state unchanged), with no leaks. The sweep ends when the n-th allocation was never reached
// (probed after the run: the pending failure still fires).

class AdvOomOp {
public:
  virtual ~AdvOomOp() {}
  virtual int setup() = 0;                   // fresh state (no injection)
  virtual int run() = 0;                     // the operation under test
  virtual int unchanged() = 0;               // after a failure: 1 if the state is as before
  virtual int done() = 0;                    // after success: 1 if the result is right
  virtual void teardown() = 0;
  virtual int keeps_on_success() { return 0; }   // 1: a successful run legitimately keeps memory (registries)
};

static int adv_fail_pending(u32 n)
{
  u32 i;
  for (i = 0; i <= n; i++) {
    void* p = e_alloc(1);
    if (!p) return 1;
    e_free(p);
  }
  return 0;
}

static void adv_oom_sweep(TF* tf, AdvOomOp* op, const char* what, u32 max_n)
{
  u32 n, fails = 0, bad = 0;
  if (op->setup() != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: setup", what); return; }   // warm-up: the
  if (op->run() != E_OK || !op->done()) tf_fail(tf, __FILE__, __LINE__, "%s: fails without injection", what);
  op->unchanged();
  op->teardown();                            // op's own buffers reach their final size before counting starts
  for (n = 1; n <= max_n && !bad; n++) {
    u32 before = e_alloc_count(), after;
    int rc, pending;
    if (op->setup() != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: setup", what); return; }
    e_test_fail_after((i32)n);
    rc = op->run();
    pending = adv_fail_pending(n);
    e_test_fail_after(-1);
    if (rc == E_OK) {
      if (!op->done()) { tf_fail(tf, __FILE__, __LINE__, "%s: allocation %u failed but the result is wrong", what, n); bad = 1; }
    } else {
      fails++;
      if (rc != E_NOMEM) { tf_fail(tf, __FILE__, __LINE__, "%s: allocation %u: %s instead of E_NOMEM", what, n, e_errname(rc)); bad = 1; }
      else if (!op->unchanged()) { tf_fail(tf, __FILE__, __LINE__, "%s: allocation %u failed and the state changed", what, n); bad = 1; }
    }
    op->teardown();
    after = e_alloc_count();
    if (after != before && !(rc == E_OK && op->keeps_on_success())) { tf_fail(tf, __FILE__, __LINE__, "%s: allocation %u: %d blocks leaked", what, n, (int)(after - before)); bad = 1; }
    if (pending) break;                      // the operation never reached allocation n: all covered
  }
  if (n > max_n) tf_fail(tf, __FILE__, __LINE__, "%s: more than %u allocations", what, max_n);
  tf_log(tf, "  oom %s: %u allocations, %u clean failures", what, n, fails);
  TF_CHECK(!bad);
}

// Document operations: the state is the bytes, encoding, line count, undo/redo availability and modified flag.
class AdvOomDoc : public AdvOomOp {
public:
  Document* d; EBytes before, after, big; Vec<ecp> txt; EncSpec enc_before; u32 lines_before; bool cu, cr, mod; int kind;
  AdvOomDoc(int k) : d(0), kind(k)
  {
    big.resize(300000); memset(big.data(), 'q', big.size()); big[1000] = '\n';
    adv_to_ecp("Tx\xE2\x82\xAC\n\n", &txt);
  }
  int setup()
  {
    EBytes t;
    u32 i;
    Selection s;
    d = new Document();
    for (i = 0; i < 3000; i++) t.push((u8)(i % 37 == 36 ? '\n' : i % 41 == 40 ? '\r' : 'a' + i % 26));
    t.append("\xC3\xA9\xFF", 3);
    if (adv_doc_init(*d, t.data(), t.size(), enc_utf8(0), EOL_LF) != E_OK) return E_NOMEM;
    d->replace(5, 2, (const u8*)"xyz\n", 4);
    d->replace(100, 0, (const u8*)"\r\n\r\n", 4);
    if (kind == 2) d->undo(&s);
    snap(&before);
    enc_before = d->enc(); lines_before = d->line_count(); cu = d->can_undo(); cr = d->can_redo(); mod = d->modified();
    return E_OK;
  }
  void snap(EBytes* b) { adv_doc_bytes(*d, b); }
  int run()
  {
    Selection s;
    ConvertReport rep;
    s.anchor = s.caret = 0;
    switch (kind) {
      case 0: return d->replace(10, 50, big.data(), big.size());
      case 1: return d->undo(&s);
      case 2: return d->redo(&s);
      case 3: return d->convert(enc_utf16le(1), &rep, 0);
      case 4: return d->convert_eol(EOL_CRLF, 0);
      case 5: return d->reinterpret(enc_utf16be(0), 0, 0);
      case 6: return d->convert_eol(EOL_CR, 0);
      default: return d->replace_text(3, 1, txt.data(), txt.size(), 0);
    }
  }
  int unchanged()
  {
    snap(&after);
    return after.size() == before.size() && memcmp(after.data(), before.data(), before.size()) == 0 &&
           enc_equal(d->enc(), enc_before) && d->line_count() == lines_before && d->can_undo() == cu &&
           d->can_redo() == cr && d->modified() == mod && consistent();
  }
  int consistent()
  {
    Vec<u32> br;
    EBytes b;
    char why[200];
    snap(&b);
    adv_breaks(b.data(), b.size(), d->tree()->break_spec(), &br);
    return d->tree()->check(why, sizeof(why)) == 0 && d->line_count() == br.size() + 1;
  }
  int done()
  {
    Selection s;
    int ok = consistent();
    if (kind != 1 && kind != 5 && ok) {      // undo of the operation restores the state exactly
      ok = d->undo(&s) == E_OK;
      snap(&after);
      ok = ok && after.size() == before.size() && memcmp(after.data(), before.data(), before.size()) == 0 &&
           enc_equal(d->enc(), enc_before);
    }
    return ok;
  }
  void teardown() { delete d; d = 0; }
};

// Search: compile + find_all + replace all into a document (one group; on failure the group is undone).
class AdvOomSearch : public AdvOomOp {
public:
  Document* d; EBytes before; int kind; u32 hits; Vec<ecp> pat, rep;
  AdvOomSearch(int k) : d(0), kind(k), hits(0)
  {
    adv_to_ecp(kind == 0 ? "foo" : kind == 1 ? "(f)(o+)\\s*(?=b)" : "(\\w)\\1", &pat);
    adv_to_ecp("<$2\\U$1\\E>", &rep);
  }
  int setup()
  {
    EBytes t;
    u32 i;
    d = new Document();
    for (i = 0; i < 400; i++) t.push((u8)(i % 29 == 28 ? '\n' : "abcfoo bar"[i % 10]));
    if (adv_doc_init(*d, t.data(), t.size(), enc_utf8(0), EOL_LF) != E_OK) return E_NOMEM;
    adv_doc_bytes(*d, &before);
    return E_OK;
  }
  int run()
  {
    Searcher s;
    SearchOpts o;
    SearchError err;
    Vec<MarkRange> all;
    AdvDocSink sink(d);
    Selection sel;
    u32 n = 0, unrep = 0;
    int rc;
    o.mode = kind == 0 ? SM_NORMAL : SM_REGEX; o.match_case = 0; o.whole_word = 0; o.dot_all = 0;
    rc = s.compile(pat.data(), pat.size(), o, d->enc(), &err);
    if (rc != E_OK) return rc;
    rc = s.find_all(d->bytes(), d->codec(), 0, d->length(), &all, 100000, 0);
    if (rc < 0) return rc;
    hits = all.size();
    sel.anchor = sel.caret = 0;
    d->begin_edit(UK_OTHER, sel, 0);
    rc = search_replace_all(s, d->bytes(), d->codec(), 0, d->length(), rep.data(), rep.size(), &sink, 0, &n, &unrep);
    d->end_edit(sel);
    if (rc != E_OK && n) d->undo(0);         // the caller undoes a partly applied group
    return rc;
  }
  int unchanged() { return adv_same(*d, before); }
  int done() { return hits > 0 && !adv_same(*d, before); }
  void teardown() { delete d; d = 0; }
};

// INI, config, session: parse and write.
class AdvOomText : public AdvOomOp {
public:
  int kind; EStr text, out; Session s;
  AdvOomText(int k) : kind(k) { session_init(&s); }
  ~AdvOomText() { session_free(&s); }
  int setup()
  {
    out.clear();
    if (text.len() == 0) {
      if (kind == 2) adv_session_text(&text);
      else { Config c; Ini ini; config_defaults(&c); config_mru_add(&c, "C:\\a.txt"); config_store(&c, &ini); ini.write(&text); }
    }
    return E_OK;
  }
  int run()
  {
    Ini ini;
    int rc = ini.parse(text.c_str(), text.len());
    if (rc != E_OK) return rc;
    if (kind == 1) { Config c; config_load(&c, ini); config_store(&c, &ini); }
    if (kind == 2) {
      Ini i2;
      rc = session_from_ini(ini, &s);
      if (rc != E_OK) return rc;
      rc = session_to_ini(s, &i2);
      if (rc != E_OK) return rc;
      return i2.write(&out);
    }
    return ini.write(&out);
  }
  int unchanged() { return s.tabs.size() == 0 || s.tabs.size() == 5; }   // parse failed, or a later step did
  int done() { return kind == 1 ? out.len() > 0 : strcmp(out.c_str(), text.c_str()) == 0; }
  void teardown() { session_free(&s); session_init(&s); }
};

// Layout, long-line index, highlighter, .lng parsing, line commands.
class AdvOomMisc : public AdvOomOp {
public:
  int kind; Document* d; EBytes before, b60; u32 result; Vec<ecp> txt, buf; Vec<u8> st;
  AdvOomMisc(int k) : kind(k), d(0), result(0)
  {
    adv_to_ecp("x\r\ny\rz\n\xE2\x82\xAC", &txt);
    buf.resize(2000); st.resize(2000);
  }
  int setup()
  {
    EBytes t;
    u32 i;
    d = new Document();
    for (i = 0; i < (kind == 0 ? 70000u : 9000u); i++) t.push((u8)(i % 997 == 996 ? '\n' : "int x;\t/* \xC3\xA9 */ \"s\" "[i % 21]));
    if (adv_doc_init(*d, t.data(), t.size(), enc_utf8(0), EOL_LF) != E_OK) return E_NOMEM;
    adv_doc_bytes(*d, &before);
    b60.resize(8000);
    d->read(0, b60.data(), 8000);
    result = 0;
    return E_OK;
  }
  int run()
  {
    LayoutOpts lo;
    lo.tab_width = 4; lo.show_ws = 1; lo.pad[0] = lo.pad[1] = 0;
    switch (kind) {
      case 0: { LongLineIndex idx; int rc = idx.build(d->bytes(), d->codec(), lo, 0, d->length(), 0); result = idx.total_cells(); return rc; }
      case 1: { LineLayout l; Vec<u32> rows; int rc; rc = l.build(b60.data(), 8000, d->codec(), lo, 0, 1); if (rc == E_OK) rc = l.wrap_rows(40, 1, &rows); result = rows.size(); return rc; }
      case 2: {
        Highlighter h;
        DocLines dl;
        u32 guard = 0, l;
        int rc;
        dl.bind(d, &h);
        rc = h.attach(&dl, lang_find("cpp"));
        if (rc == E_OK) {
          h.set_target(d->line_count() - 1);
          while (h.work(50) && guard < 100000) guard++;
          for (l = 0; rc == E_OK && l < 20 && l < d->line_count(); l++) {
            u32 n = 0;
            rc = dl.lp_get(l, buf.data(), 2000, &n);
            if (rc == E_OK) h.style_line(l, buf.data(), n, st.data());
          }
          result = h.valid_upto();
        }
        h.detach();
        dl.unbind();
        return rc;
      }
      case 3: {
        LangInfo li;
        EStr err;
        if (!g_adv_langs) return E_STATE;
        return lang_parse((const char*)g_adv_langs->t_[0].data(), g_adv_langs->t_[0].size(), &li, &err);
      }
      default: {
        EdCtx e;
        int rc;
        memset(&e, 0, sizeof(e));
        e.doc = d;
        ed_lang_from(lang_find("cpp"), 4, 0, &e.lang);
        e.sel.anchor = 500; e.sel.caret = 6000;
        switch (kind) {
          case 4: rc = ed_move_lines(&e, 1); break;
          case 5: rc = ed_duplicate(&e); break;
          case 6: rc = ed_change_case(&e, 1); break;
          case 7: rc = ed_toggle_comment(&e); break;
          case 8: rc = ed_indent(&e, 0); break;
          default: rc = ed_insert_text(&e, txt.data(), txt.size(), 0); break;
        }
        return rc;
      }
    }
  }
  int unchanged() { return adv_same(*d, before); }
  int done()
  {
    if (kind >= 4) { adv_undo_all(*d); return adv_same(*d, before); }
    if (kind == 2) return result == d->line_count();
    return kind == 3 || result > 0;
  }
  void teardown() { delete d; d = 0; }
  int keeps_on_success() { return kind == 3; }   // lang_parse: the lexer belongs to the registry until lang_shutdown
};

static void adv_oom_all(TF* tf)
{
  static const char* const dn[] = { "replace 300 KB", "undo", "redo", "convert utf16", "convert_eol crlf",
                                    "reinterpret utf16be", "convert_eol cr", "replace_text" };
  static const char* const mn[] = { "long line index", "layout + wrap", "highlighter", "lng parse", "move lines",
                                    "duplicate", "change case", "toggle comment", "indent", "insert text" };
  u32 i;
  for (i = 0; i < 8; i++) { AdvOomDoc op((int)i); adv_oom_sweep(tf, &op, dn[i], 2000); }
  for (i = 0; i < 3; i++) { AdvOomSearch op((int)i); adv_oom_sweep(tf, &op, i == 0 ? "replace all literal" : i == 1 ? "replace all regex" : "replace all backref", 3000); }
  for (i = 0; i < 3; i++) { AdvOomText op((int)i); adv_oom_sweep(tf, &op, i == 0 ? "ini" : i == 1 ? "config" : "session", 5000); }
  for (i = 0; i < 10; i++) { AdvOomMisc op((int)i); adv_oom_sweep(tf, &op, mn[i], 3000); }
}

// ---------------------------------------------------------------------------------------------------------------
// 10. EOL counting (memchr fast path, T16): exact vs detect_eol over choppy sources and every edit; open timing

class AdvChoppySource : public ByteSource {   // returns 1-7 byte spans from a private copy
public:
  AdvChoppySource(const u8* p, u32 n, u32 seed) : seed_(seed | 1) { b_.append(p, n); }
  epos size() const { return b_.size(); }
  const u8* get(epos off, epos* avail)
  {
    if (off >= b_.size()) { *avail = 0; return 0; }
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    *avail = 1 + seed_ % 7;
    if (*avail > b_.size() - off) *avail = b_.size() - off;
    return b_.data() + off;
  }
  EBytes b_; u32 seed_;
};

static int adv_eol_same(Document& d, const char* what, TF* tf)
{
  EBytes b;
  EolCounts c;
  adv_doc_bytes(d, &b);
  detect_eol(b.data(), b.size(), d.enc(), &c);
  if (c.crlf != d.eol_counts().crlf || c.lf != d.eol_counts().lf || c.cr != d.eol_counts().cr) {
    tf_fail(tf, __FILE__, __LINE__, "%s: eol counts %u/%u/%u, detect_eol %u/%u/%u", what, d.eol_counts().crlf,
            d.eol_counts().lf, d.eol_counts().cr, c.crlf, c.lf, c.cr);
    return 1;
  }
  return 0;
}

static void adv_eol_counts(TF* tf, u32 rounds)
{
  u32 r, bad = 0;
  for (r = 0; r < rounds && !bad; r++) {
    EBytes b;
    Document d;
    u32 kind = r % 3, unit = kind ? 2 : 1;   // ANSI, UTF-16 LE, UTF-16 BE
    EncSpec enc = kind == 0 ? enc_ansi() : kind == 1 ? enc_utf16le(0) : enc_utf16be(0);
    u32 n = adv_rand(tf, r < 30 ? 6 : 3000), i, e;
    int cr_mode = (int)adv_rand(tf, 3) == 0;
    for (i = 0; i < n; i++) {
      u32 k = adv_rand(tf, 7);
      u8 c = (u8)(k == 0 || k == 3 ? 0x0D : k == 1 ? 0x0A : k == 2 ? 'x' : k == 4 ? 0x0A : 'y');
      if (unit == 1) b.push(c);
      else {
        u8 hi = (u8)(adv_rand(tf, 12) == 0 ? 0x0A : adv_rand(tf, 12) == 0 ? 0x0D : 0);   // look-alike bytes
        if (kind == 1) { b.push(c); b.push(hi); } else { b.push(hi); b.push(c); }
      }
    }
    if (unit == 2 && adv_rand(tf, 4) == 0) b.push(0x0A);   // odd tail byte
    {
      AdvChoppySource* src = new AdvChoppySource(b.data(), b.size(), r + 1);
      if (d.init_source(src, enc, 0, cr_mode ? EOL_CR : EOL_CRLF, 0) != E_OK) { TF_CHECK(0); break; }
    }
    if (adv_eol_same(d, "eol init", tf)) { bad = 1; break; }
    for (e = 0; e < 30 && !bad; e++) {
      static const char* const ins[] = { "\r", "\n", "\r\n", "\n\r", "x", "", "\r\r\n\n" };
      epos len = d.length(), pos = adv_rand(tf, len + 1), del = adv_rand(tf, 4) * unit;
      const char* t = ins[adv_rand(tf, 7)];
      EBytes tb;
      u32 j;
      for (j = 0; t[j]; j++) {
        if (unit == 1) tb.push((u8)t[j]);
        else if (kind == 1) { tb.push((u8)t[j]); tb.push(0); }
        else { tb.push(0); tb.push((u8)t[j]); }
      }
      if (unit == 2) pos &= ~1u;
      if (del > len - pos) del = len - pos;
      if (unit == 2 && (del & 1) && pos + del != len) del--;
      if (adv_rand(tf, 4) == 0) { d.undo(0); }
      else if (adv_rand(tf, 9) == 0) { d.convert_eol((EolStyle)adv_rand(tf, 3), 0); }
      else {
        int rc = d.replace(pos, del, tb.data(), tb.size());
        if (rc != E_OK && rc != E_ALIGN) { tf_fail(tf, __FILE__, __LINE__, "eol edit: %s", e_errname(rc)); bad = 1; }
      }
      if (!bad && adv_eol_same(d, "eol edit", tf)) bad = 1;
    }
    if (!bad && d.reinterpret(unit == 1 ? enc_cp(437) : (kind == 1 ? enc_utf16be(0) : enc_utf16le(0)), 0, 0) == E_OK &&
        adv_eol_same(d, "eol reinterpret", tf)) bad = 1;
    if (!bad && d.reinterpret(unit == 1 ? enc_utf16le(0) : enc_ansi(), 0, 0) == E_OK && adv_eol_same(d, "eol reinterpret unit", tf)) bad = 1;
  }
  TF_CHECK(!bad);
}

// The memchr counters against the reference loop on random windows (x/y boundary corrections, known counts).
static void adv_eol_windows(TF* tf, u32 rounds)
{
  u32 r, bad = 0;
  for (r = 0; r < rounds && !bad; r++) {
    EBytes b;
    u32 unit = 1 + (r & 1), be = (r >> 1) & 1, n = adv_rand(tf, r < 50 ? 12 : 9000), i, w;
    for (i = 0; i < n; i++) {
      u32 k = adv_rand(tf, 5);
      b.push((u8)(k == 0 ? 0x0D : k == 1 ? 0x0A : (unit == 2 && k == 2) ? 0 : 'a'));
    }
    for (w = 0; w < 20 && !bad; w++) {
      AdvChoppySource src(b.data(), b.size(), r * 31 + w);
      epos a = adv_rand(tf, n + 1), e = a + adv_rand(tf, n - a + 1), q;
      EolCounts ref, fast;
      u32 mode, known_lf = 0, known_cr = 0;
      if (unit == 2) a &= ~1u;
      if (e < a) e = a;
      for (q = a; q + unit <= e; q += unit) {      // units of the two kinds inside [a, e)
        u32 v = unit == 1 ? b[q] : be ? ((u32)b[q] << 8 | b[q + 1]) : ((u32)b[q + 1] << 8 | b[q]);
        if (v == 0x0A) known_lf++;
        if (v == 0x0D) known_cr++;
      }
      // the reference counts a CR/LF unit cut by e (e odd in UTF-16) only if the whole unit is inside: same here
      doc_count_eol_test(&src, a, e, unit, (int)be, 1, 0, 0, &ref);
      for (mode = 0; mode < 3 && !bad; mode++) {
        u8 kb = (u8)(mode == 0 ? 0 : mode == 1 ? 0x0A : 0x0D);
        u32 kn = mode == 1 ? known_lf : known_cr;
        if (unit == 2 && ((e - a) & 1) && mode) continue;   // known counts are whole units (tree semantics)
        doc_count_eol_test(&src, a, e, unit, (int)be, 0, kb, kn, &fast);
        if (fast.crlf != ref.crlf || fast.lf != ref.lf || fast.cr != ref.cr) {
          tf_fail(tf, __FILE__, __LINE__, "eol window unit %u be %u [%u, %u) of %u mode %u: %u/%u/%u, reference %u/%u/%u",
                  unit, be, a, e, n, mode, fast.crlf, fast.lf, fast.cr, ref.crlf, ref.lf, ref.cr);
          bad = 1;
        }
      }
    }
  }
  TF_CHECK(!bad);
}

static void adv_open_timing(TF* tf)
{
  EBytes b;
  u32 n = 10 * 1024 * 1024, i = 0, reps = 0;
  clock_t c0, c1;
  double tree_ms, total_ms;
  int ok = 1;                                // no checks inside the timed loops: check counts must not depend on speed
  BreakSpec bs = adv_spec(1, 0x0A, 0);
  b.resize(n);
  while (i < n) {
    u32 len = adv_rand(tf, 70), j;
    for (j = 0; j < len && i < n; j++) b[i++] = (u8)('a' + adv_rand(tf, 26));
    if (i < n) b[i++] = 0x0D;
    if (i < n) b[i++] = 0x0A;
  }
  c0 = clock();
  do {
    MemSource* src = new MemSource();
    PieceTree t;
    src->copy_from(b.data(), n);
    if (t.init(src, bs, 0) != E_OK) ok = 0;
    t.clear();
    delete src;
    reps++;
    c1 = clock();
  } while ((double)(c1 - c0) < 0.3 * CLOCKS_PER_SEC);
  tree_ms = (double)(c1 - c0) * 1000.0 / CLOCKS_PER_SEC / reps;
  reps = 0;
  c0 = clock();
  do {
    Document d;
    if (adv_doc_init(d, b.data(), n, enc_ansi(), EOL_CRLF) != E_OK) ok = 0;
    reps++;
    c1 = clock();
  } while ((double)(c1 - c0) < 0.3 * CLOCKS_PER_SEC);
  total_ms = (double)(c1 - c0) * 1000.0 / CLOCKS_PER_SEC / reps;
  TF_CHECK(ok);
  printf("  open timing (10 MB CRLF, in memory, incl. a 10 MB copy): PieceTree::init %.0f ms, Document::init_source "
         "%.0f ms", tree_ms, total_ms);
  {                                          // the same text as UTF-16 LE (20 MB)
    EBytes w;
    w.resize(2 * n);
    for (i = 0; i < n; i++) { w[2 * i] = b[i]; w[2 * i + 1] = 0; }
    reps = 0;
    c0 = clock();
    do {
      Document d;
      if (adv_doc_init(d, w.data(), w.size(), enc_utf16le(0), EOL_CRLF) != E_OK) ok = 0;
      reps++;
      c1 = clock();
    } while ((double)(c1 - c0) < 0.3 * CLOCKS_PER_SEC);
    printf(", UTF-16 LE 20 MB init_source %.0f ms\n", (double)(c1 - c0) * 1000.0 / CLOCKS_PER_SEC / reps);
    fflush(stdout);
    {                                        // verification outside the timed loops
      Document d1, d2;
      if (adv_doc_init(d1, b.data(), n, enc_ansi(), EOL_CRLF) != E_OK || adv_eol_same(d1, "open 10 MB", tf)) ok = 0;
      if (adv_doc_init(d2, w.data(), w.size(), enc_utf16le(0), EOL_CRLF) != E_OK || adv_eol_same(d2, "open 20 MB utf16", tf)) ok = 0;
    }
    TF_CHECK(ok);
  }
  fflush(stdout);
}

// ---------------------------------------------------------------------------------------------------------------
// 11. Document::reset_source_indexing (T16 addition for T14's reload into on-demand mode)

class AdvEvLog : public DocListener {
public:
  Vec<EditEvent> ev;
  void doc_changed(Document* d, const EditEvent& e) { (void)d; ev.push(e); }
};

static void adv_reset_indexing(TF* tf)
{
  Document d;
  AdvEvLog log;
  EBytes nb;
  Vec<u32> blocks;
  EolCounts ec;
  u32 i, nblocks;
  static const u8 old_text[] = "old\r\ntext\r\n";
  TF_EQ_I(adv_doc_init(d, old_text, sizeof(old_text) - 1, enc_ansi(), EOL_CRLF), E_OK);
  TF_EQ_I(d.replace(0, 0, (const u8*)"x", 1), E_OK);
  TF_CHECK(d.can_undo());
  d.add_listener(&log);
  for (i = 0; i < 200000; i++) nb.push((u8)(i % 50 == 49 ? '\n' : i % 50 == 48 ? '\r' : 'a' + i % 26));
  {
    MemSource* src = new MemSource();
    src->copy_from(nb.data(), nb.size());
    TF_EQ_I(d.reset_source_indexing(src, enc_ansi(), 0, EOL_CRLF), E_OK);
  }
  TF_EQ_U(log.ev.size(), 1);
  if (log.ev.size() == 1) {
    TF_CHECK(log.ev[0].flags == EV_RESET && log.ev[0].removed == sizeof(old_text) && log.ev[0].inserted == nb.size());
    TF_EQ_U(log.ev[0].lines_added, 0);
  }
  TF_CHECK(d.indexing() && !d.editable() && !d.can_undo() && !d.modified());
  TF_EQ_I(d.replace(0, 0, (const u8*)"y", 1), E_BUSY);
  TF_EQ_U(d.line_count(), 1);
  TF_EQ_U(d.length(), nb.size());
  nblocks = (nb.size() + 65535) / 65536;
  for (i = 0; i < nblocks; i++) {
    u32 a = i * 65536, e = a + 65536 < nb.size() ? a + 65536 : nb.size(), c = 0, j;
    for (j = a; j < e; j++) if (nb[j] == '\n') c++;
    blocks.push(c);
  }
  detect_eol(nb.data(), nb.size(), enc_ansi(), &ec);
  TF_EQ_I(d.finish_indexing(blocks.data(), nblocks, ec), E_OK);
  TF_CHECK(log.ev.size() == 2 && log.ev[1].flags == EV_INDEXED);
  TF_EQ_U(d.line_count(), nb.size() / 50 + 1);
  TF_EQ_U(d.line_start(7), 7 * 50);
  TF_CHECK(d.editable());
  {
    ConvertReport rep;
    TF_EQ_I(d.convert(enc_utf8(0), &rep, 0), E_TOOBIG);   // on-demand mode
  }
  TF_EQ_I(d.replace(0, 0, (const u8*)"y", 1), E_OK);
  // refusals take the source too (the leak check of the runner sees it)
  d.set_busy(true);
  {
    MemSource* src = new MemSource();
    src->copy_from(nb.data(), 10);
    TF_EQ_I(d.reset_source_indexing(src, enc_ansi(), 0, EOL_CRLF), E_BUSY);
  }
  d.set_busy(false);
  {
    Selection sel;
    MemSource* src = new MemSource();
    src->copy_from(nb.data(), 10);
    sel.anchor = sel.caret = 0;
    d.begin_edit(UK_OTHER, sel, 0);
    TF_EQ_I(d.reset_source_indexing(src, enc_ansi(), 0, EOL_CRLF), E_STATE);
    d.end_edit(sel);
  }
  TF_EQ_U(d.length(), nb.size() + 1);
  d.remove_listener(&log);
}

// ---------------------------------------------------------------------------------------------------------------

// Section timing in the verbose log (tf_log): finds the slow parts on the VM.
#define ADV_T(call, name) do { clock_t t0_ = clock(); call; tf_log(tf, "  %s: %u ms", name, (u32)((clock() - t0_) * 1000 / CLOCKS_PER_SEC)); } while (0)

void suite_adversarial(TF* tf)
{
  u32 k = tf->long_run ? 4 : 1;
  enc_set_system(1252, 437);
  codec_init();
  ADV_T(adv_cache_tree_run(tf, 1, 0x0A, 0, 300000, 100 * k, "cache lf"), "adv_cache_tree_run");
  ADV_T(adv_cache_tree_run(tf, 1, 0x0D, 0, 300000, 60 * k, "cache cr"), "adv_cache_tree_run");
  ADV_T(adv_cache_tree_run(tf, 2, 0x0A, 0x00, 300000, 100 * k, "cache utf16le"), "adv_cache_tree_run");
  ADV_T(adv_cache_tree_run(tf, 2, 0x00, 0x0A, 300000, 60 * k, "cache utf16be"), "adv_cache_tree_run");
  ADV_T(adv_cache_huge(tf), "adv_cache_huge");
  ADV_T(adv_cache_doc(tf, 150 * k), "adv_cache_doc");
  ADV_T(adv_cache_oom(tf), "adv_cache_oom");
  ADV_T(adv_lang_init(tf), "adv_lang_init");
  ADV_T(adv_pathological(tf), "adv_pathological");
  ADV_T(adv_long_line(tf), "adv_long_line");
  ADV_T(adv_huge_search(tf), "adv_huge_search");
  ADV_T(adv_regex_bombs(tf), "adv_regex_bombs");
  ADV_T(adv_regex_limits(tf), "adv_regex_limits");
  ADV_T(adv_regex_garbage(tf, 3000 * k), "adv_regex_garbage");
  ADV_T(adv_replace_edges(tf), "adv_replace_edges");
  ADV_T(adv_lexer_garbage(tf, 200 * k), "adv_lexer_garbage");
  ADV_T(adv_lexer_converge(tf, 60 * k), "adv_lexer_converge");
  ADV_T(adv_config_corrupt(tf, 500 * k), "adv_config_corrupt");
  ADV_T(adv_session_corrupt(tf, 500 * k), "adv_session_corrupt");
  ADV_T(adv_lng_corrupt(tf, 150 * k), "adv_lng_corrupt");
  ADV_T(adv_lng_long_keyword(tf), "adv_lng_long_keyword");
  ADV_T(adv_hexgoto_garbage(tf, 20000 * k), "adv_hexgoto_garbage");
  ADV_T(adv_backup_corrupt(tf), "adv_backup_corrupt");
  ADV_T(adv_undo_stress(tf, tf->long_run ? 1000000 : 100000), "adv_undo_stress");
  ADV_T(adv_codec_roundtrips(tf), "adv_codec_roundtrips");
  ADV_T(adv_oom_all(tf), "adv_oom_all");
  ADV_T(adv_eol_counts(tf, 150 * k), "adv_eol_counts");
  ADV_T(adv_eol_windows(tf, 200 * k), "adv_eol_windows");
  ADV_T(adv_reset_indexing(tf), "adv_reset_indexing");
  ADV_T(adv_cache_timing(tf), "adv_cache_timing");
  ADV_T(adv_open_timing(tf), "adv_open_timing");
  adv_lang_free();
}
