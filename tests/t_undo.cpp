// t_undo.cpp - suite "undo": UndoLog groups, merge rule, state ops, save points, memory (DESIGN 6.6). Owner: T01.
#include "tf.h"
#include "undo.h"

static UndoSel us(epos a, epos c)
{
  UndoSel s;
  s.anchor = a; s.caret = c;
  return s;
}

static PieceRef pr(u16 src, u32 start, u32 len)
{
  PieceRef r;
  r.src = src; r.flags = 0; r.start = start; r.len = len; r.nbreaks = 0;
  return r;
}

// one group with one insert op of len bytes at pos (caret before = pos, after = pos + len)
static int typed(UndoLog& u, u32 kind, epos pos, epos len, u32 now)
{
  PieceVec v;
  int merged;
  v.push(pr(1, pos, len));
  merged = u.begin(kind, us(pos, pos), now);
  u.rec_insert(pos, len, v);
  u.end(us(pos + len, pos + len));
  return merged;
}

static void test_basic(TF* tf)
{
  UndoLog u;
  UndoStep st;
  PieceVec a, r;
  u32 s0 = u.seq(), s1;
  TF_CHECK(!u.can_undo());
  TF_CHECK(!u.can_redo());
  TF_CHECK(u.at_save_point());
  TF_EQ_I(u.pop_undo(&st), E_STATE);
  TF_EQ_I(u.pop_redo(&st), E_STATE);
  // a group with delete + insert
  r.push(pr(0, 10, 5)); r.push(pr(2, 0, 3));
  a.push(pr(3, 7, 4));
  TF_EQ_I(u.begin(UK_OTHER, us(1, 9), 100), 0);
  TF_EQ_I(u.rec_delete(10, 8, r), E_OK);
  TF_EQ_I(u.rec_insert(10, 4, a), E_OK);
  TF_CHECK(!u.can_undo());                           // open group
  u.end(us(14, 14));
  TF_CHECK(u.can_undo());
  TF_CHECK(!u.at_save_point());
  s1 = u.seq();
  TF_CHECK(s1 != s0);
  TF_EQ_I(u.pop_undo(&st), E_OK);
  TF_EQ_U(st.nops, 2);
  TF_EQ_U(st.ops[0].kind, 2);
  TF_EQ_U(st.ops[0].pos, 10);
  TF_EQ_U(st.ops[0].len, 8);
  TF_EQ_U(st.ops[0].ref_count, 2);
  TF_EQ_U(st.refs[st.ops[0].ref_first + 1].src, 2);
  TF_EQ_U(st.ops[1].kind, 1);
  TF_EQ_U(st.ops[1].ref_count, 1);
  TF_EQ_U(st.refs[st.ops[1].ref_first].start, 7);
  TF_EQ_U(st.before.anchor, 1);
  TF_EQ_U(st.before.caret, 9);
  TF_EQ_U(st.after.caret, 14);
  TF_EQ_U(u.seq(), s0);
  TF_CHECK(u.at_save_point());
  TF_CHECK(u.can_redo());
  TF_EQ_I(u.pop_redo(&st), E_OK);
  TF_EQ_U(st.nops, 2);
  TF_EQ_U(u.seq(), s1);
  // begin/end without recording: nothing happens, redo side survives
  TF_EQ_I(u.pop_undo(&st), E_OK);
  u.begin(UK_OTHER, us(0, 0), 200);
  u.end(us(0, 0));
  TF_CHECK(u.can_redo());
  TF_EQ_U(u.seq(), s0);
  // state op
  TF_EQ_I(u.pop_redo(&st), E_OK);
  u.begin(UK_OTHER, us(0, 0), 300);
  TF_EQ_I(u.rec_state(0x04E40000u, 0xFDE90002u, 0, 0x101), E_OK);
  u.end(us(0, 0));
  TF_EQ_I(u.pop_undo(&st), E_OK);
  TF_EQ_U(st.nops, 1);
  TF_EQ_U(st.ops[0].kind, 3);
  TF_EQ_U(st.ops[0].pos, 0x04E40000u);
  TF_EQ_U(st.ops[0].len, 0xFDE90002u);
  TF_EQ_U(st.ops[0].ref_first, 0);
  TF_EQ_U(st.ops[0].ref_count, 0x101);
  TF_CHECK(u.memory_bytes() > 0);
  u.clear();
  TF_CHECK(!u.can_undo());
  TF_CHECK(!u.can_redo());
}

static void test_merge(TF* tf)
{
  UndoLog u;
  UndoStep st;
  TF_EQ_I(typed(u, UK_TYPING, 0, 1, 1000), 0);
  TF_EQ_I(typed(u, UK_TYPING, 1, 1, 1500), 1);       // merges
  TF_EQ_I(typed(u, UK_TYPING, 2, 1, 3500), 1);       // exactly 2000 ms after the previous end: merges
  TF_EQ_I(typed(u, UK_TYPING, 3, 1, 5501), 0);       // 2001 ms: new group
  TF_EQ_I(typed(u, UK_TYPING, 5, 1, 5600), 0);       // caret moved (before != previous after)
  TF_EQ_I(typed(u, UK_DELETE, 6, 1, 5700), 0);       // kind changed
  TF_EQ_I(typed(u, UK_OTHER, 7, 1, 5800), 0);        // UK_OTHER never merges
  TF_EQ_I(typed(u, UK_OTHER, 8, 1, 5900), 0);
  TF_EQ_I(typed(u, UK_TYPING, 9, 1, 6000), 0);
  u.break_merge();
  TF_EQ_I(typed(u, UK_TYPING, 10, 1, 6100), 0);      // merging broken
  TF_EQ_I(typed(u, UK_TYPING, 11, 1, 6200), 1);      // ... only once
  // a selection before the edit (anchor != caret) does not merge
  {
    PieceVec v;
    v.push(pr(1, 0, 1));
    TF_EQ_I(u.begin(UK_TYPING, us(10, 12), 6300), 0);
    u.rec_insert(12, 1, v);
    u.end(us(13, 13));
  }
  // after an undo, typing starts a new group
  TF_EQ_I(u.pop_undo(&st), E_OK);
  TF_EQ_I(typed(u, UK_TYPING, 12, 1, 6400), 0);
  TF_CHECK(!u.can_redo());
  // groups: {0,1,2} {3} {5} {6 del} {7} {8} {9} {10,11} {12}
  {
    u32 n = 0;
    while (u.pop_undo(&st) == E_OK) n++;
    TF_EQ_U(n, 9);
  }
  // the first group holds three ops (merged typing)
  TF_EQ_I(u.pop_redo(&st), E_OK);
  TF_EQ_U(st.nops, 3);
  TF_EQ_U(st.after.caret, 3);
  TF_EQ_U(st.before.caret, 0);
  // clock wrap-around: now < previous time never merges
  {
    UndoLog w;
    typed(w, UK_TYPING, 0, 1, 0xFFFFFF00u);
    TF_EQ_I(typed(w, UK_TYPING, 1, 1, 0x00000100u), 1);   // wrapped by 0x200 ms: still within 2000 ms
    TF_EQ_I(typed(w, UK_TYPING, 2, 1, 0x00000050u), 0);   // went backwards
  }
}

static void test_save_point(TF* tf)
{
  UndoLog u;
  UndoStep st;
  u32 s2;
  typed(u, UK_OTHER, 0, 1, 0);
  typed(u, UK_OTHER, 1, 1, 0);
  u.set_save_point();
  TF_CHECK(u.at_save_point());
  typed(u, UK_OTHER, 2, 1, 0);
  TF_CHECK(!u.at_save_point());
  u.pop_undo(&st);
  TF_CHECK(u.at_save_point());                       // undo back to the save point clears "modified"
  u.pop_undo(&st);
  TF_CHECK(!u.at_save_point());
  u.pop_redo(&st);
  TF_CHECK(u.at_save_point());
  // discard a redo branch that contains the save point
  u.pop_undo(&st);
  typed(u, UK_OTHER, 5, 1, 0);                       // new branch: saved state is gone forever
  TF_CHECK(!u.at_save_point());
  u.pop_undo(&st);
  TF_CHECK(!u.at_save_point());
  // a save point at the branch point itself survives the discard
  u.set_save_point();
  typed(u, UK_OTHER, 6, 1, 0);
  u.pop_undo(&st);
  typed(u, UK_OTHER, 7, 1, 0);
  u.pop_undo(&st);
  TF_CHECK(u.at_save_point());
  // async save: capture seq, more history moves, then mark that state saved
  typed(u, UK_OTHER, 8, 1, 0);
  s2 = u.seq();
  typed(u, UK_OTHER, 9, 1, 0);
  u.set_save_point_seq(s2);
  TF_CHECK(!u.at_save_point());
  u.pop_undo(&st);
  TF_CHECK(u.at_save_point());
  // a merged (reopened) group gets a new state id
  {
    UndoLog m;
    typed(m, UK_TYPING, 0, 1, 10);
    m.set_save_point();
    TF_EQ_I(typed(m, UK_TYPING, 1, 1, 20), 1);
    TF_CHECK(!m.at_save_point());
  }
  // clear keeps the saved/modified status
  u.clear();
  TF_CHECK(u.at_save_point());
  typed(u, UK_OTHER, 0, 1, 0);
  u.clear();
  TF_CHECK(!u.at_save_point());
  u.invalidate_save_point();
  TF_CHECK(!u.at_save_point());
}

static void test_ids_unique(TF* tf)
{
  UndoLog u;
  UndoStep st;
  Vec<u32> seen;
  u32 i, j, dup = 0;
  seen.push(u.seq());
  for (i = 0; i < 300; i++) {
    u32 r = tf_rand_below(tf, 3);
    if (r == 0) u.pop_undo(&st);
    else if (r == 1) u.pop_redo(&st);
    else {
      typed(u, UK_OTHER, i, 1, 0);
      for (j = 0; j < seen.size(); j++) if (seen[j] == u.seq()) dup++;   // a brand-new state never reuses an id
      seen.push(u.seq());
    }
  }
  TF_EQ_U(dup, 0);
}

static void test_oom(TF* tf)
{
  UndoLog u;
  PieceVec v;
  u32 k, i;
  int failed = 0;
  for (i = 0; i < 50; i++) v.push(pr(1, i, 1));
  for (k = 1; k <= 6; k++) {
    int rc;
    u.begin(UK_OTHER, us(0, 0), 0);
    e_test_fail_after(k);
    rc = u.rec_insert(0, 50, v);
    e_test_fail_after(-1);
    u.end(us(0, 0));
    if (rc != E_OK) { TF_EQ_I(rc, E_NOMEM); failed++; }
  }
  TF_CHECK(failed > 0);
  // reserve() makes the next group infallible
  TF_EQ_I(u.reserve(2, 100), E_OK);
  u.begin(UK_OTHER, us(0, 0), 0);
  e_test_fail_after(1);
  TF_EQ_I(u.rec_insert(0, 50, v), E_OK);
  TF_EQ_I(u.rec_delete(0, 50, v), E_OK);
  e_test_fail_after(-1);
  u.end(us(0, 0));
}

void suite_undo(TF* tf)
{
  test_basic(tf);
  test_merge(tf);
  test_save_point(tf);
  test_ids_unique(tf);
  test_oom(tf);
}
