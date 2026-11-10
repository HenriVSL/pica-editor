// rxbt.cpp - budgeted backtracker for backreferences and lookahead (DESIGN 11.1). Owner: T03.
// Runs the same program as the Pike VM over a decoded window: up to BT_STARTS start positions plus BT_EXTRA more
// characters of room for the match (and context). A match longer than the window is not found (documented limit).
// Choice points live on an explicit stack; captures and loop marks are undone through a trail. Lookahead runs as a
// nested, atomic sub-match (ECMAScript/PCRE semantics). The step budget covers the whole search call.
#include "regex_int.h"
#include <string.h>

enum { BT_STARTS = 32768, BT_EXTRA = 65536, BT_MAX_FRAMES = 1000000, BT_MAX_TRAIL = 1000000 };

struct BtCtx {
  const RxProg* p;
  const RxInst* code;
  const ecp* wc;
  u32 wn;              // window characters
  u32 cons;            // characters [0, cons) may be consumed by the match (they end <= hi)
  ecp prev0;           // character before the window
  int has_prev0;
  u32 ncap;
  u32 steps, budget;
  RxMState* st;
};

static void undo_to(BtCtx* b, u32 len)
{
  Vec<RxTrail>& tr = b->st->trail;
  u32* sl = b->st->sl.data();
  while (tr.size() > len) {
    sl[tr.back().slot] = tr.back().old;
    tr.pop();
  }
}

static int set_slot(BtCtx* b, u32 slot, u32 val)
{
  RxTrail t;
  u32* sl = b->st->sl.data();
  if (b->st->trail.size() >= BT_MAX_TRAIL) return E_BUDGET;
  t.slot = slot;
  t.old = sl[slot];
  if (b->st->trail.push(t) != E_OK) return E_NOMEM;
  sl[slot] = val;
  return E_OK;
}

// Runs from pc at character index i. lim = consumption limit. 1 matched (MATCH or LOOKEND reached, choice points
// above the entry level discarded), 0 no match, < 0 error/budget.
static int bt_run(BtCtx* b, u32 pc, u32 i, u32 lim)
{
  RxMState* st = b->st;
  const RxInst* code = b->code;
  const ecp* wc = b->wc;
  u32 base = st->bstk.size();
  for (;;) {
    const RxInst* in;
    int r;
    if (++b->steps > b->budget) return E_BUDGET;
    in = &code[pc];
    switch (in->op) {
    case RXO_CHAR:
      if (i < lim && wc[i] == in->x) { pc++; i++; continue; }
      goto fail;
    case RXO_CHARI:
      if (i < lim && rx_fold(wc[i]) == in->x) { pc++; i++; continue; }
      goto fail;
    case RXO_ANY:
      if (i < lim && wc[i] != 10 && wc[i] != 13) { pc++; i++; continue; }
      goto fail;
    case RXO_ANYNL:
      if (i < lim) { pc++; i++; continue; }
      goto fail;
    case RXO_CLASS:
      if (i < lim && rx_class_has(b->p, in->x, wc[i])) { pc++; i++; continue; }
      goto fail;
    case RXO_MATCH:
    case RXO_LOOKEND:
      st->bstk.resize(base);
      return 1;
    case RXO_JMP:
      pc = in->x;
      continue;
    case RXO_SPLIT: {
      RxBtFrame f;
      if (st->bstk.size() >= BT_MAX_FRAMES) return E_BUDGET;
      f.pc = in->y; f.i = i; f.trail = st->trail.size();
      if (st->bstk.push(f) != E_OK) return E_NOMEM;
      pc = in->x;
      continue;
    }
    case RXO_SAVE:
      if ((r = set_slot(b, in->x, i)) != E_OK) return r;
      pc++;
      continue;
    case RXO_MARK:
      if ((r = set_slot(b, b->ncap + in->x, i)) != E_OK) return r;
      pc++;
      continue;
    case RXO_CHKPROG:
      if (st->sl[b->ncap + in->x] == i) goto fail;
      pc++;
      continue;
    case RXO_BACKREF: {
      u32 s = st->sl[2 * in->x], e = st->sl[2 * in->x + 1], len, k;
      if (s == RX_NONE || e == RX_NONE || e < s) goto fail;
      len = e - s;
      if (len > lim - i) goto fail;
      if (in->arg) {
        for (k = 0; k < len; k++) if (rx_fold(wc[s + k]) != rx_fold(wc[i + k])) goto fail;
      } else {
        for (k = 0; k < len; k++) if (wc[s + k] != wc[i + k]) goto fail;
      }
      i += len;
      pc++;
      continue;
    }
    case RXO_LOOK: {
      u32 tl = st->trail.size();
      r = bt_run(b, pc + 1, i, b->wn);
      if (r < 0) return r;
      if (r == 0) undo_to(b, tl);
      if (in->arg == 0) {            // (?= ): keep the captures it set
        if (!r) goto fail;
      } else {                        // (?! )
        if (r) { undo_to(b, tl); goto fail; }
      }
      pc = in->x;
      continue;
    }
    default: {
      RxAt at;
      at.prev = i > 0 ? wc[i - 1] : b->prev0;
      at.has_prev = (u8)(i > 0 ? 1 : b->has_prev0);
      at.next = i < b->wn ? wc[i] : 0;
      at.has_next = (u8)(i < b->wn ? 1 : 0);
      if (rx_assert(in->op, &at)) { pc++; continue; }
      goto fail;
    }
    }
  fail:
    if (st->bstk.size() <= base) return 0;
    {
      RxBtFrame f = st->bstk.back();
      st->bstk.pop();
      undo_to(b, f.trail);
      pc = f.pc;
      i = f.i;
    }
  }
}

int rxbt_search(RxMState* st, RxReader* rd, epos lo, epos from, epos hi, epos ctx, u32 budget, RxMatch* m,
                RxProgress* g)
{
  const RxProg* p = st->prog;
  BtCtx b;
  epos ws = from;
  ecp prev0 = 0;
  int r, has_prev0;
  u32 nsl = p->ncap + p->nloops, k;
  const ecp* pre = p->prefix.data();
  u32 plen = p->prefix.size();
  RxPre* req = (!plen && st->req.ok && p->line_local && !p->anchored) ? &st->req : 0;
  epos req_q = 0;
  int req_valid = 0;

  if ((st->pre.ok && st->pre.never) || (req && req->never)) return 0;
  if (st->sl.resize(nsl + 1) != E_OK) return E_NOMEM;
  r = rxr_prev(rd, lo, ws, &prev0, 0);
  if (r < 0) return r;
  has_prev0 = r;
  b.p = p;
  b.code = p->code.data();
  b.ncap = p->ncap;
  b.steps = 0;
  b.budget = budget ? budget : 1;
  b.st = st;
  for (;;) {
    epos pos = ws, jump = 0;
    int complete = 0;
    u32 wn, cons, nstart, i;
    u32 cap_chars = BT_STARTS + BT_EXTRA;
    st->wc.clear();
    st->wp.clear();
    {
      epos room = ctx - ws;
      if (room < cap_chars) cap_chars = room;   // never more characters than bytes
    }
    if (st->wc.reserve(cap_chars + 1) != E_OK || st->wp.reserve(cap_chars + 2) != E_OK) return E_NOMEM;
    for (;;) {
      ecp ch;
      u32 len;
      if (pos >= ctx) { complete = 1; break; }
      if (st->wc.size() >= BT_STARTS + BT_EXTRA) break;
      len = rxr_decode(rd, pos, &ch);
      if (len == 0) {
        if (rd->err) return rd->err;
        complete = 1;
        break;
      }
      if (st->wc.push(ch) != E_OK || st->wp.push(pos) != E_OK) return E_NOMEM;
      pos += len;
    }
    if (st->wp.push(pos) != E_OK) return E_NOMEM;
    wn = st->wc.size();
    cons = wn;
    while (cons > 0 && st->wp[cons] > hi) cons--;
    nstart = complete ? wn : (wn < BT_STARTS ? wn : BT_STARTS);
    b.wc = st->wc.data();
    b.wn = wn;
    b.cons = cons;
    b.prev0 = prev0;
    b.has_prev0 = has_prev0;
    for (i = 0; i <= nstart; i++) {
      if (i == nstart && !complete) break;     // continues in the next window
      if ((i & 1023) == 0 && i < wn && (r = rxp_tick(g, st->wp[i])) != E_OK) return r;
      if (st->wp[i] > hi) break;
      if (p->anchored && st->wp[i] != lo) break;
      if (req && (!req_valid || st->wp[i] > req_q)) {   // every match contains the literal, on the same line
        epos q = 0, ls;
        r = rxpre_find(req, rd->s, st->wp[i], hi, hi, from, &q, g);
        rxr_reset(rd);
        if (r < 0) return r;
        if (r == 0) return 0;
        req_q = q;
        req_valid = 1;
        ls = rxr_line_start(rd, st->wp[i], q);
        while (i < nstart && st->wp[i] < ls) i++;
        if (st->wp[i] < ls) { jump = ls; break; }   // beyond this window: the next one starts there
      }
      if (p->bol) {
        RxAt at;
        at.prev = i > 0 ? b.wc[i - 1] : prev0;
        at.has_prev = (u8)(i > 0 ? 1 : has_prev0);
        at.next = i < wn ? b.wc[i] : 0;
        at.has_next = (u8)(i < wn ? 1 : 0);
        if (!rx_assert(RXO_BOL, &at)) continue;
      }
      if (plen) {
        if (i > cons || plen > cons - i) continue;
        if (p->prefix_icase) {
          for (k = 0; k < plen; k++) if (rx_fold(b.wc[i + k]) != pre[k]) break;
        } else {
          for (k = 0; k < plen; k++) if (b.wc[i + k] != pre[k]) break;
        }
        if (k < plen) continue;
      } else if (p->first_ok) {
        if (i >= cons || !rx_first_has(p, b.wc[i])) continue;
      }
      for (k = 0; k < nsl; k++) st->sl[k] = RX_NONE;
      st->trail.clear();
      st->bstk.clear();
      r = bt_run(&b, 0, i, cons);
      if (r < 0) return r;
      if (r == 1) {
        for (k = 0; k < p->ncap; k++) st->best[k] = st->sl[k] == RX_NONE ? EPOS_NONE : st->wp[st->sl[k]];
        rx_fill_match(p, st->best, m);
        return 1;
      }
    }
    if (jump) {
      ws = jump;
      r = rxr_prev(rd, lo, ws, &prev0, 0);
      if (r < 0) return r;
      has_prev0 = r;
      continue;
    }
    if (complete || nstart >= wn || st->wp[nstart] > hi) return 0;
    prev0 = st->wc[nstart - 1];
    has_prev0 = 1;
    ws = st->wp[nstart];
  }
}
