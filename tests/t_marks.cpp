// t_marks.cpp - suite "marks": LineMarks (bookmarks) and RangeMarks (DESIGN 6.8). Owner: T01.
#include "tf.h"
#include "marks.h"
#include <string.h>

static Document* text_doc(const char* t)
{
  Document* d = new Document();
  MemSource* s = new MemSource();
  s->copy_from((const u8*)t, es_len(t));
  d->init_source(s, enc_ansi(), 0, EOL_CRLF, 0);
  return d;
}

static void test_line_basic(TF* tf)
{
  LineMarks m;
  u32 v[6];
  TF_EQ_U(m.count(), 0);
  TF_EQ_U(m.next(0), EPOS_NONE);
  TF_EQ_U(m.prev(0), EPOS_NONE);
  TF_EQ_I(m.toggle(5), 1);
  TF_EQ_I(m.toggle(2), 1);
  TF_EQ_I(m.toggle(9), 1);
  TF_CHECK(m.has(5));
  TF_CHECK(!m.has(4));
  TF_EQ_U(m.count(), 3);
  TF_EQ_U(m.at(0), 2);
  TF_EQ_U(m.at(2), 9);
  TF_EQ_U(m.at(3), EPOS_NONE);
  TF_EQ_U(m.next(2), 5);
  TF_EQ_U(m.next(5), 9);
  TF_EQ_U(m.next(9), 2);                             // wraps
  TF_EQ_U(m.next(100), 2);
  TF_EQ_U(m.next(0xFFFFFFFFu), 2);
  TF_EQ_U(m.prev(5), 2);
  TF_EQ_U(m.prev(2), 9);                             // wraps
  TF_EQ_U(m.prev(0), 9);
  TF_EQ_U(m.prev(6), 5);
  TF_EQ_I(m.toggle(5), 0);
  TF_CHECK(!m.has(5));
  v[0] = 7; v[1] = 3; v[2] = 7; v[3] = 1; v[4] = 3; v[5] = 0;
  TF_EQ_I(m.set(v, 6), E_OK);
  TF_EQ_U(m.count(), 4);
  TF_EQ_U(m.at(0), 0);
  TF_EQ_U(m.at(1), 1);
  TF_EQ_U(m.at(2), 3);
  TF_EQ_U(m.at(3), 7);
  m.clear();
  TF_EQ_U(m.count(), 0);
  TF_EQ_I(m.set(v, 0), E_OK);
}

static void test_line_edits(TF* tf)
{
  Document* d = text_doc("l0\r\nl1\r\nl2\r\nl3\r\nl4\r\nl5\r\n");
  LineMarks m;
  Selection s;
  m.attach(d);
  m.toggle(1); m.toggle(3); m.toggle(5);
  // insert two lines inside line 0: later marks shift by 2
  TF_EQ_I(d->replace(1, 0, (const u8*)"a\r\nb\r\n", 6), E_OK);
  TF_EQ_U(m.at(0), 3);
  TF_EQ_U(m.at(1), 5);
  TF_EQ_U(m.at(2), 7);
  TF_EQ_I(d->undo(&s), E_OK);
  TF_EQ_U(m.at(0), 1);
  TF_EQ_U(m.at(2), 5);
  // pure insertion of a line break at the very start of a marked line moves the mark with its text
  TF_EQ_I(d->replace(d->line_start(3), 0, (const u8*)"\r\n", 2), E_OK);
  TF_EQ_U(m.at(1), 4);
  TF_EQ_U(m.at(2), 6);
  TF_EQ_I(d->undo(&s), E_OK);
  TF_EQ_U(m.at(1), 3);
  // typing at the end of a marked line (not at its start) leaves it in place
  TF_EQ_I(d->replace(d->line_end(1), 0, (const u8*)"\r\n", 2), E_OK);
  TF_EQ_U(m.at(0), 1);
  TF_EQ_U(m.at(1), 4);
  TF_EQ_I(d->undo(&s), E_OK);
  // delete from inside line 0 to inside line 4 (4 breaks): marks 1 and 3 collapse onto line 0, 5 -> 1
  TF_EQ_I(d->replace(1, d->line_start(4) + 1 - 1, 0, 0), E_OK);
  TF_EQ_U(m.count(), 2);
  TF_EQ_U(m.at(0), 0);
  TF_EQ_U(m.at(1), 1);
  // a non-line edit changes nothing
  TF_EQ_I(d->replace(0, 1, (const u8*)"Z", 1), E_OK);
  TF_EQ_U(m.count(), 2);
  // reset: marks past the new end go away
  {
    MemSource* s2 = new MemSource();
    s2->copy_from((const u8*)"only", 4);
    TF_EQ_I(d->reset_source(s2, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
    TF_EQ_U(m.count(), 1);
    TF_EQ_U(m.at(0), 0);
  }
  m.detach();
  delete d;
}

// random edits: marks stay sorted, unique and inside the document; marks on untouched lines keep their text
static void test_line_random(TF* tf)
{
  Document* d = text_doc("");
  LineMarks m;
  u32 step, bad = 0;
  u8 buf[64];
  m.attach(d);
  for (step = 0; step < 3000; step++) {
    epos len = d->length(), pos = tf_rand_below(tf, len + 1), del = tf_rand_below(tf, 30), n = tf_rand_below(tf, 30), i;
    if (del > len - pos) del = len - pos;
    for (i = 0; i < n; i++) buf[i] = (u8)(tf_rand_below(tf, 4) == 0 ? '\n' : 'a' + tf_rand_below(tf, 26));
    if (tf_rand_below(tf, 5) == 0) m.toggle(tf_rand_below(tf, d->line_count()));
    // remember the text of a mark's line well below the edit
    {
      u32 L = d->line_of(pos), k, j, mk = EPOS_NONE;
      u8 before[16], after[16];
      epos ls, ll;
      for (k = 0; k < m.count(); k++) if (m.at(k) + 2 <= L || m.at(k) > d->line_of(pos + del) + 1) { mk = m.at(k); break; }
      ls = mk != EPOS_NONE ? d->line_start(mk) : 0;
      ll = mk != EPOS_NONE ? emin<epos>(16, d->line_end(mk) - ls) : 0;
      if (mk != EPOS_NONE) d->read(ls, before, ll);
      d->replace(pos, del, buf, n);
      if (mk != EPOS_NONE) {
        for (k = 0; k < m.count(); k++) {
          epos s2 = d->line_start(m.at(k));
          epos l2 = emin<epos>(16, d->line_end(m.at(k)) - s2);
          d->read(s2, after, l2);
          if (l2 == ll && memcmp(before, after, ll) == 0) break;
        }
        if (k == m.count()) bad++;
      }
      for (j = 0; j + 1 < m.count(); j++) if (m.at(j) >= m.at(j + 1)) bad++;
      if (m.count() && m.at(m.count() - 1) >= d->line_count()) bad++;
    }
  }
  TF_EQ_U(bad, 0);
  m.detach();
  delete d;
}

static void test_range_basic(TF* tf)
{
  RangeMarks r;
  MarkRange in[6];
  in[0].pos = 50; in[0].len = 5;
  in[1].pos = 10; in[1].len = 5;
  in[2].pos = 12; in[2].len = 3;   // overlaps [10,15): dropped
  in[3].pos = 30; in[3].len = 0;   // empty: dropped
  in[4].pos = 15; in[4].len = 2;   // touches: kept
  in[5].pos = 100; in[5].len = 1;
  TF_EQ_I(r.set(in, 6), E_OK);
  TF_EQ_U(r.count(), 4);
  TF_EQ_U(r.at(0).pos, 10);
  TF_EQ_U(r.at(1).pos, 15);
  TF_EQ_U(r.at(2).pos, 50);
  TF_EQ_U(r.at(3).pos, 100);
  TF_EQ_U(r.at(9).len, 0);
  TF_EQ_U(r.first_ending_after(0), 0);
  TF_EQ_U(r.first_ending_after(14), 0);
  TF_EQ_U(r.first_ending_after(15), 1);
  TF_EQ_U(r.first_ending_after(54), 2);
  TF_EQ_U(r.first_ending_after(55), 3);
  TF_EQ_U(r.first_ending_after(101), 4);
  r.clear();
  TF_EQ_U(r.count(), 0);
  TF_EQ_U(r.first_ending_after(5), 0);
  // cap at 10,000
  {
    Vec<MarkRange> many;
    u32 i;
    for (i = 0; i < 12000; i++) { MarkRange x; x.pos = i * 3; x.len = 2; many.push(x); }
    TF_EQ_I(r.set(many.data(), many.size()), E_OK);
    TF_EQ_U(r.count(), 10000);
  }
}

static void test_range_edits(TF* tf)
{
  Document* d = text_doc("0123456789abcdefghijklmnopqrstuvwxyz");
  RangeMarks r;
  Vec<MarkRange> model;
  MarkRange in[3];
  Selection s;
  u32 step, i, bad = 0;
  u8 buf[16];
  in[0].pos = 2; in[0].len = 3;
  in[1].pos = 10; in[1].len = 4;
  in[2].pos = 20; in[2].len = 2;
  r.attach(d);
  r.set(in, 3);
  TF_EQ_I(d->replace(0, 0, (const u8*)"XX", 2), E_OK);       // before: all shift
  TF_EQ_U(r.at(0).pos, 4);
  TF_EQ_U(r.at(2).pos, 22);
  TF_EQ_I(d->replace(7, 0, (const u8*)"Y", 1), E_OK);        // exactly at the end of [4,7): kept, later shift
  TF_EQ_U(r.count(), 3);
  TF_EQ_U(r.at(0).pos, 4);
  TF_EQ_U(r.at(1).pos, 13);
  TF_EQ_I(d->replace(13, 0, (const u8*)"Z", 1), E_OK);       // at the start of [13,17): shifts
  TF_EQ_U(r.at(1).pos, 14);
  TF_EQ_I(d->replace(15, 1, 0, 0), E_OK);                    // inside: dropped
  TF_EQ_U(r.count(), 2);
  TF_EQ_U(r.at(1).pos, 23);
  TF_EQ_I(d->undo(&s), E_OK);                                // undo is an edit too (range stays dropped)
  TF_EQ_U(r.count(), 2);
  TF_EQ_U(r.at(1).pos, 24);
  // reinterpret (bytes unchanged) keeps them, a reset with new bytes clears them
  TF_EQ_I(d->reinterpret(enc_utf8(0), 0, 0), E_OK);
  TF_EQ_U(r.count(), 2);
  {
    MemSource* s2 = new MemSource();
    s2->copy_from((const u8*)"new", 3);
    TF_EQ_I(d->reset_source(s2, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
    TF_EQ_U(r.count(), 0);
  }
  // random edits against a model of the same rule
  {
    MemSource* s3 = new MemSource();
    static u8 big[4000];
    for (i = 0; i < sizeof(big); i++) big[i] = (u8)('a' + i % 26);
    s3->copy_from(big, sizeof(big));
    d->reset_source(s3, enc_ansi(), 0, EOL_CRLF, 0);
  }
  for (i = 0; i < 300; i++) {
    MarkRange x;
    x.pos = i * 13; x.len = 1 + i % 9;
    model.push(x);
  }
  r.set(model.data(), model.size());
  TF_EQ_U(r.count(), 300);
  for (step = 0; step < 2000 && !bad; step++) {
    epos len = d->length(), pos = tf_rand_below(tf, len + 1), del = tf_rand_below(tf, 12), n = tf_rand_below(tf, 12), end;
    u32 k;
    if (del > len - pos) del = len - pos;
    memset(buf, 'q', sizeof(buf));
    d->replace(pos, del, buf, n);
    if (!del && !n) continue;
    end = pos + del;
    for (i = 0, k = 0; i < model.size(); i++) {
      MarkRange x = model[i];
      if (x.pos + x.len <= pos) model[k++] = x;
      else if (x.pos >= end) { x.pos = x.pos - del + n; model[k++] = x; }
    }
    model.resize(k);
    if (r.count() != model.size()) bad++;
    for (i = 0; i < model.size() && !bad; i++) if (r.at(i).pos != model[i].pos || r.at(i).len != model[i].len) bad++;
  }
  TF_EQ_U(bad, 0);
  TF_CHECK(model.size() > 0);
  r.detach();
  delete d;
}

static void test_detach(TF* tf)
{
  Document* d = text_doc("a\nb\nc\n");
  LineMarks* m = new LineMarks();
  RangeMarks* r = new RangeMarks();
  m->attach(d);
  r->attach(d);
  m->toggle(2);
  delete m;                                          // destructor detaches
  delete r;
  TF_EQ_I(d->replace(0, 0, (const u8*)"\n", 1), E_OK);    // no dangling listener
  TF_EQ_U(d->line_count(), 5);
  delete d;
}

// DocLines (docbind.cpp) needs the lexer sources, which the marks suite does not link yet. Build with
// -DT01_DOCBIND and add "docbind lexer lexgen lexhtml lexmd" to the suite's source list to enable this test.
#ifdef T01_DOCBIND
#include "docbind.h"
static void test_docbind(TF* tf)
{
  Document d;
  DocLines dl;
  Highlighter h;
  MemSource* s = new MemSource();
  ecp buf[64];
  u32 n = 99;
  s->copy_from((const u8*)"\xEF\xBB\xBF" "ab\xC3\xA9\r\n\r\nxyz", 14);
  TF_EQ_I(d.init_source(s, enc_utf8(1), 3, EOL_CRLF, 0), E_OK);
  TF_EQ_U(dl.lp_count(), 0);
  TF_EQ_I(dl.lp_get(0, buf, 64, &n), E_STATE);
  dl.bind(&d, 0);
  TF_EQ_U(dl.lp_count(), 3);
  TF_EQ_I(dl.lp_get(0, buf, 64, &n), E_OK);
  TF_EQ_U(n, 3);                                     // BOM skipped, EOL excluded
  TF_EQ_U(buf[0], 'a');
  TF_EQ_U(buf[2], 0xE9);
  TF_EQ_I(dl.lp_get(1, buf, 64, &n), E_OK);
  TF_EQ_U(n, 0);
  TF_EQ_I(dl.lp_get(2, buf, 2, &n), E_OK);
  TF_EQ_U(n, 2);
  TF_EQ_U(buf[1], 'y');
  TF_EQ_I(dl.lp_get(3, buf, 64, &n), E_RANGE);
  dl.unbind();
  h.attach(&dl, 0);
  dl.bind(&d, &h);
  TF_EQ_I(d.replace(4, 0, (const u8*)"\r\n", 2), E_OK);  // lines_changed path
  TF_EQ_I(d.reinterpret(enc_ansi(), 0, 0), E_OK);       // lines_reset path
  TF_EQ_I(dl.lp_get(0, buf, 64, &n), E_OK);
  TF_EQ_U(n, 4);                                     // the BOM bytes are text in ANSI
  dl.unbind();
  h.detach();
}
#endif

void suite_marks(TF* tf)
{
  enc_set_system(1252, 437);
  codec_init();
  test_line_basic(tf);
  test_line_edits(tf);
  test_line_random(tf);
  test_range_basic(tf);
  test_range_edits(tf);
  test_detach(tf);
#ifdef T01_DOCBIND
  test_docbind(tf);
#endif
}
