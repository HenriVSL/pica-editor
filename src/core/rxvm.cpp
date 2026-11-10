// rxvm.cpp - RxMatcher: Pike VM over ByteSource + Codec (DESIGN 6.14, 11.3), and the character reader shared with
// the backtracker. Owner: T03.
// The VM keeps one thread per program counter (sparse sets, captures per thread) in priority order, which gives
// leftmost-first (Perl-like) results in O(text x program) time. Unanchored searches add a new lowest-priority
// thread at each position until a match is found; while no thread is alive the prefilter (rxpre.cpp) or the
// first-character set skips ahead.
#include "regex_int.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// Reader

void rxr_init(RxReader* r, ByteSource* s, const Codec* c)
{
  r->s = s;
  r->c = c;
  r->size = s->size();
  r->chunk = 0;
  r->cbeg = r->cend = 0;
  r->err = E_OK;
  r->fast = (u8)((c->ascii_compat && c->unit == 1) ? 1 : 0);
  r->unit = c->unit ? c->unit : 1;
  r->pad[0] = r->pad[1] = 0;
}

u32 rxr_decode(RxReader* r, epos pos, ecp* cp)
{
  const u8* p;
  epos avail;
  u32 len;
  if (pos >= r->size) return 0;
  if (!r->chunk || pos < r->cbeg || pos >= r->cend) {
    avail = 0;
    p = r->s->get(pos, &avail);
    if (!p || avail == 0) {
      r->chunk = 0;
      r->err = r->s->err() != E_OK ? r->s->err() : E_IO;
      return 0;
    }
    if (avail > r->size - pos) avail = r->size - pos;
    r->chunk = p;
    r->cbeg = pos;
    r->cend = pos + avail;
  }
  p = r->chunk + (pos - r->cbeg);
  if (r->fast && *p < 0x80) { *cp = *p; return 1; }
  avail = r->cend - pos;
  len = r->c->decode(p, avail, r->cend >= r->size, cp);
  if (len == 0) {                    // character split across chunks
    u8 buf[8];
    epos n = r->size - pos;
    if (n > 8) n = 8;
    if (n > r->c->max_len && r->c->max_len) n = r->c->max_len;
    rxr_reset(r);
    if (r->s->read(pos, buf, n) != E_OK) { r->err = r->s->err() != E_OK ? r->s->err() : E_IO; return 0; }
    len = r->c->decode(buf, n, pos + n >= r->size, cp);
    if (len == 0) {
      if (r->unit == 2 && n >= 2) { *cp = ECP_BADUNIT((u32)buf[0] | ((u32)buf[1] << 8)); len = 2; }
      else { *cp = ECP_BADBYTE(buf[0]); len = 1; }
    }
  }
  return len;
}

epos rxr_snap(RxReader* r, epos lo, epos b)
{
  epos at = b, stop = b > lo + 65536 ? b - 65536 : lo;
  u8 buf[256];
  rxr_reset(r);
  while (at > stop) {
    epos n = at - stop, k;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (r->s->read(at - n, buf, n) != E_OK) return lo;
    for (k = n; k > 0; k--) {
      if (buf[k - 1] == 10 || buf[k - 1] == 13) { rxr_reset(r); return at - n + k; }
    }
    at -= n;
  }
  rxr_reset(r);
  return stop;
}

int rxr_prev(RxReader* r, epos lo, epos pos, ecp* cp, epos* start)
{
  const Codec* c = r->c;
  epos st;
  u32 len;
  if (pos <= lo) return 0;
  if (c->max_len == 1) st = pos - 1;
  else if (c->self_sync) st = codec_prev(r->s, c, pos, lo);
  else st = codec_prev(r->s, c, pos, rxr_snap(r, lo, pos - 1));
  rxr_reset(r);
  if (st >= pos) st = pos - (r->unit <= pos - lo ? r->unit : 1);
  len = rxr_decode(r, st, cp);
  if (len == 0) return r->err != E_OK ? r->err : E_IO;
  if (start) *start = st;
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Matcher state

RxMatcher::RxMatcher() : prog_(0), budget_(10000000), ctx_end_(EPOS_NONE), st_(0) {}

RxMatcher::~RxMatcher()
{
  free_state_();
}

void RxMatcher::free_state_()
{
  if (st_) {
    u32 k;
    for (k = 0; k < 2; k++) {
      e_free(st_->l[k].sparse);
      e_free(st_->l[k].pcs);
      e_free(st_->l[k].caps);
    }
    e_free(st_->tmp);
    e_free(st_->best);
    e_free(st_->none);
    e_free(st_->stk);
    delete st_;
    st_ = 0;
  }
}

int RxMatcher::init(const RxProg* p)
{
  RxMState* st;
  u32 k, ninst, ncap;
  free_state_();
  prog_ = 0;
  if (!p) return E_STATE;
  st = new RxMState;
  if (!st) return E_NOMEM;
  st_ = st;
  ninst = p->code.size();
  ncap = p->ncap;
  st->prog = p;
  st->ninst = ninst;
  st->ncap = ncap;
  st->pre_codec = 0;
  st->pre_key = 0;
  st->pre_built = 0;
  st->tmp = st->best = st->none = 0;
  st->stk = 0;
  for (k = 0; k < 2; k++) {
    st->l[k].n = 0;
    st->l[k].sparse = (u32*)e_alloc(ninst * sizeof(u32));
    st->l[k].pcs = (u32*)e_alloc(ninst * sizeof(u32));
    st->l[k].caps = (epos*)e_alloc(ninst * ncap * sizeof(epos));
  }
  st->tmp = (epos*)e_alloc(ncap * sizeof(epos));
  st->best = (epos*)e_alloc(ncap * sizeof(epos));
  st->none = (epos*)e_alloc(ncap * sizeof(epos));
  st->stk = (RxStk*)e_alloc((ninst + 2) * sizeof(RxStk));
  if (!st->l[0].sparse || !st->l[0].pcs || !st->l[0].caps || !st->l[1].sparse || !st->l[1].pcs || !st->l[1].caps ||
      !st->tmp || !st->best || !st->none || !st->stk) {
    free_state_();
    return E_NOMEM;
  }
  memset(st->l[0].sparse, 0, ninst * sizeof(u32));
  memset(st->l[1].sparse, 0, ninst * sizeof(u32));
  for (k = 0; k < ncap; k++) st->none[k] = EPOS_NONE;
  prog_ = p;
  return E_OK;
}

void RxMatcher::set_budget(u32 steps)
{
  budget_ = steps;
}

void RxMatcher::set_context_end(epos ctx_end)
{
  ctx_end_ = ctx_end;
}

// ---------------------------------------------------------------------------------------------------------------
// Pike VM

static void addthread(RxMState* st, RxList* l, u32 pc0, const epos* caps_in, epos pos, const RxAt* at)
{
  const RxInst* code = st->prog->code.data();
  epos* t = st->tmp;
  u32 ncap = st->ncap, sp;
  RxStk* stk = st->stk;
  memcpy(t, caps_in, ncap * sizeof(epos));
  stk[0].kind = 0;
  stk[0].a = pc0;
  sp = 1;
  while (sp) {
    u32 pc;
    sp--;
    if (stk[sp].kind) { t[stk[sp].a] = stk[sp].v; continue; }
    pc = stk[sp].a;
    for (;;) {
      u32 idx = l->sparse[pc];
      const RxInst* in;
      if (idx < l->n && l->pcs[idx] == pc) break;
      idx = l->n++;
      l->sparse[pc] = idx;
      l->pcs[idx] = pc;
      in = &code[pc];
      switch (in->op) {
      case RXO_JMP:
        pc = in->x;
        continue;
      case RXO_SPLIT:
        stk[sp].kind = 0;
        stk[sp].a = in->y;
        sp++;
        pc = in->x;
        continue;
      case RXO_SAVE:
        stk[sp].kind = 1;
        stk[sp].a = in->x;
        stk[sp].v = t[in->x];
        sp++;
        t[in->x] = pos;
        pc++;
        continue;
      case RXO_MARK: case RXO_CHKPROG:
        pc++;
        continue;
      case RXO_CHAR: case RXO_CHARI: case RXO_ANY: case RXO_ANYNL: case RXO_CLASS: case RXO_MATCH:
        memcpy(l->caps + idx * ncap, t, ncap * sizeof(epos));
        break;
      default:
        if (rx_assert(in->op, at)) { pc++; continue; }
        break;
      }
      break;
    }
  }
}

epos rxr_line_start(RxReader* rd, epos floor, epos q)
{
  u8 buf[256];
  epos at = q;
  u32 unit = rd->unit;
  rxr_reset(rd);
  while (at > floor) {
    epos n = at - floor, k;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (unit == 2) n &= ~(epos)1;
    if (n == 0) break;
    if (rd->s->read(at - n, buf, n) != E_OK) break;
    for (k = n; k >= unit; k -= unit) {
      u32 ch = unit == 1 ? buf[k - 1] : (rd->c->kind == EK_UTF16BE ? ((u32)buf[k - 2] << 8 | buf[k - 1])
                                                                  : ((u32)buf[k - 1] << 8 | buf[k - 2]));
      if (ch == 10 || ch == 13) { rxr_reset(rd); return at - n + k; }
    }
    at -= n;
  }
  rxr_reset(rd);
  return floor;
}

// After a jump to pos: the character before it and the one at it.
static int reload(RxReader* rd, epos lo, epos pos, ecp* prev, int* has_prev, ecp* ch, u32* len, int* has)
{
  int r = rxr_prev(rd, lo, pos, prev, 0);
  if (r < 0) return r;
  *has_prev = r;
  *len = rxr_decode(rd, pos, ch);
  if (*len == 0 && rd->err) return rd->err;
  *has = *len > 0;
  return E_OK;
}

// Moves *pos just past the next line break (CR, LF or CRLF) at or after *pos; 1 found with the new position <= hi,
// 0 none, < 0 error/cancel. *prev = the break character.
static int next_line(RxReader* rd, epos* pos, epos hi, epos ctx, ecp* prev, RxProgress* g)
{
  epos at = *pos;
  int r;
  while (at < ctx && at < hi) {
    ecp c;
    u32 len;
    if (rd->fast && rd->chunk && at >= rd->cbeg && at < rd->cend) {   // byte scan of the cached chunk
      const u8* base = rd->chunk;
      epos cb = rd->cbeg, lim = rd->cend;
      if (lim > ctx) lim = ctx;
      if (lim - at > 65536) lim = at + 65536;
      while (at < lim && base[at - cb] != 10 && base[at - cb] != 13) at++;
      if (at >= lim) {
        if ((r = rxp_tick(g, at)) != E_OK) return r;
        if (at >= ctx) break;
        if (rxr_decode(rd, at, &c) == 0 && rd->err) return rd->err;   // load the next chunk
        continue;
      }
    }
    len = rxr_decode(rd, at, &c);
    if (len == 0) { if (rd->err) return rd->err; break; }
    at += len;
    if (c == 10 || c == 13) {
      if (c == 13 && at < ctx) {     // CR LF: the line starts after the LF
        ecp c2;
        u32 l2 = rxr_decode(rd, at, &c2);
        if (l2 == 0 && rd->err) return rd->err;
        if (l2 && c2 == 10) { at += l2; c = 10; }
      }
      *pos = at;
      *prev = c;
      return at <= hi ? 1 : 0;
    }
    if ((at & 0xFFFF) == 0 && (r = rxp_tick(g, at)) != E_OK) return r;
  }
  return 0;
}

static int pike(RxMState* st, RxReader* rd, epos lo, epos from, epos hi, epos ctx, RxMatch* m, RxProgress* g)
{
  const RxProg* p = st->prog;
  const RxInst* code = p->code.data();
  RxList* cl = &st->l[0];
  RxList* nl = &st->l[1];
  u32 ncap = st->ncap, len = 0, nlen = 0, i;
  epos pos = from;
  ecp ch = 0, prev = 0, nch = 0, fch = 0;
  int has = 0, has_prev, nhas = 0, matched = 0, r, can, fdone;
  RxAt at;
  RxPre* pre = st->pre.ok ? &st->pre : 0;
  RxPre* req = (!pre && st->req.ok && p->line_local && !p->anchored) ? &st->req : 0;
  epos req_q = 0;
  int req_valid = 0;

  r = rxr_prev(rd, lo, pos, &prev, 0);
  if (r < 0) return r;
  has_prev = r;
  if (pos < ctx) {
    len = rxr_decode(rd, pos, &ch);
    if (len == 0 && rd->err) return rd->err;
    has = len > 0;
  }
  cl->n = 0;
  for (;;) {
    if (cl->n == 0) {
      if (matched || pos > hi) break;
      if (p->anchored && pos != lo) break;
      if (pre) {
        epos q = 0;
        if (pre->never) break;
        r = rxpre_find(pre, rd->s, pos, hi, hi, from, &q, g);
        rxr_reset(rd);
        if (r < 0) return r;
        if (r == 0) break;
        if (p->pure && pre->full) {
          epos caps[2];
          caps[0] = q;
          caps[1] = q + pre->m;
          rx_fill_match(p, caps, m);
          return 1;
        }
        if (q != pos) {
          pos = q;
          if ((r = reload(rd, lo, pos, &prev, &has_prev, &ch, &len, &has)) != E_OK) return r;
        }
      } else {
        int done = 0;
        while (!done) {                // skip to the next position where a match may start
          int need_line = 0;
          if (req && (!req_valid || pos > req_q)) {
            epos q = 0, ls;
            if (req->never) goto finish;
            r = rxpre_find(req, rd->s, pos, hi, hi, from, &q, g);
            rxr_reset(rd);
            if (r < 0) return r;
            if (r == 0) goto finish;                 // every match contains the literal
            req_q = q;
            req_valid = 1;
            ls = rxr_line_start(rd, pos, q);         // a match containing it starts on its line
            if (ls > pos) {
              pos = ls;
              if ((r = reload(rd, lo, pos, &prev, &has_prev, &ch, &len, &has)) != E_OK) return r;
            }
          }
          if (p->bol) {
            at.prev = prev; at.has_prev = (u8)has_prev; at.next = ch; at.has_next = (u8)has;
            if (!rx_assert(RXO_BOL, &at)) need_line = 1;
            else if (p->first_ok && (!has || !rx_first_has(p, ch))) need_line = 1;
            if (need_line) {
              r = next_line(rd, &pos, hi, ctx, &prev, g);
              if (r < 0) return r;
              if (r == 0) goto finish;
              has_prev = 1;
              has = 0;
              len = 0;
              if (pos < ctx) {
                len = rxr_decode(rd, pos, &ch);
                if (len == 0 && rd->err) return rd->err;
                has = len > 0;
              }
              continue;
            }
            done = 1;
          } else if (p->first_ok) {
            while (has && !rx_first_has(p, ch)) {
              if (len == 1 && ch < 0x80 && rd->fast && rd->chunk && pos >= rd->cbeg && pos < rd->cend) {
                // tight loop over ASCII bytes of the cached chunk that can't start a match
                const u8* base = rd->chunk;
                epos cb = rd->cbeg, lim = rd->cend, q = pos + 1;
                if (lim > hi) lim = hi;
                if (lim > ctx) lim = ctx;
                if (lim - pos > 65536) lim = pos + 65536;      // progress ticks in between
                while (q < lim) {
                  u8 b = base[q - cb];
                  if (b >= 0x80 || ((p->first[b >> 5] >> (b & 31)) & 1u)) break;
                  q++;
                }
                prev = base[q - 1 - cb];
                pos = q;
              } else {
                prev = ch;
                pos += len;
              }
              has_prev = 1;
              if (pos > hi) goto finish;
              has = 0;
              if (pos < ctx) {
                len = rxr_decode(rd, pos, &ch);
                if (len == 0 && rd->err) return rd->err;
                has = len > 0;
              }
              if ((r = rxp_tick(g, pos)) != E_OK) return r;
              if (req && req_valid && pos > req_q) break;   // look for the next literal first
            }
            if (!has) goto finish;
            done = !(req && req_valid && pos > req_q);
          } else {
            done = 1;
          }
        }
        if (pos > hi) break;
      }
    }
    if (!matched && pos <= hi && (!p->anchored || pos == lo)) {
      at.prev = prev; at.has_prev = (u8)has_prev; at.next = ch; at.has_next = (u8)has;
      addthread(st, cl, 0, st->none, pos, &at);
    }
    can = has && len <= hi - pos;
    nl->n = 0;
    nhas = 0;
    nlen = 0;
    if (can) {
      epos np = pos + len;
      if (np < ctx) {
        nlen = rxr_decode(rd, np, &nch);
        if (nlen == 0 && rd->err) return rd->err;
        nhas = nlen > 0;
      }
      at.prev = ch; at.has_prev = 1; at.next = nch; at.has_next = (u8)nhas;
    }
    fdone = 0;
    for (i = 0; i < cl->n; i++) {
      u32 pc = cl->pcs[i];
      const RxInst* in = &code[pc];
      int ok;
      switch (in->op) {
      case RXO_MATCH:
        memcpy(st->best, cl->caps + i * ncap, ncap * sizeof(epos));
        matched = 1;
        goto cut;
      case RXO_CHAR:  ok = can && ch == in->x; break;
      case RXO_CHARI:
        if (!fdone) { fch = rx_fold(ch); fdone = 1; }
        ok = can && fch == in->x;
        break;
      case RXO_ANY:   ok = can && ch != 10 && ch != 13; break;
      case RXO_ANYNL: ok = can; break;
      case RXO_CLASS: ok = can && rx_class_has(p, in->x, ch); break;
      default:        ok = 0; break;
      }
      if (ok) addthread(st, nl, pc + 1, cl->caps + i * ncap, pos + len, &at);
    }
  cut:
    if (!can) break;
    {
      RxList* t = cl; cl = nl; nl = t;
    }
    prev = ch;
    has_prev = 1;
    pos += len;
    ch = nch;
    has = nhas;
    len = nlen;
    if ((r = rxp_tick(g, pos)) != E_OK) return r;
  }
finish:
  if (matched) rx_fill_match(p, st->best, m);
  return matched;
}

int RxMatcher::search(ByteSource* s, const Codec* c, epos lo, epos from, epos hi, RxMatch* m, ProgressCb* cb)
{
  RxMState* st = st_;
  RxReader rd;
  RxProgress g;
  epos size, ctx;
  u32 key;
  if (!st || !prog_ || !s || !c || !m) return E_STATE;
  size = s->size();
  if (hi > size) hi = size;
  if (lo > from || from > hi) return 0;
  ctx = ctx_end_ == EPOS_NONE ? hi : ctx_end_;
  if (ctx > size) ctx = size;
  if (ctx < hi) ctx = hi;
  key = ((u32)c->kind << 24) ^ ((u32)c->unit << 16) ^ c->cp;
  if (!st->pre_built || st->pre_codec != c || st->pre_key != key) {
    rxpre_build(&st->pre, prog_->prefix.data(), prog_->prefix.size(), prog_->prefix_icase, c);
    rxpre_build(&st->req, prog_->req.data(), prog_->req.size(), prog_->req_icase, c);
    st->pre_codec = c;
    st->pre_key = key;
    st->pre_built = 1;
  }
  rxp_init(&g, cb, from, hi - from);
  rxr_init(&rd, s, c);
  if (prog_->use_bt) return rxbt_search(st, &rd, lo, from, hi, ctx, budget_, m, &g);
  return pike(st, &rd, lo, from, hi, ctx, m, &g);
}
