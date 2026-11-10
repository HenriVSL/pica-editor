// ptree.cpp - piece tree, AddBuffer, SnapshotSource (DESIGN 6.5, ARCH 1). Owner: T01.
//
// The tree is a treap keyed implicitly by position: every node is one piece, nodes live in a pool (32-bit indices,
// index 0 = nil, free list through Node::l), priorities come from a per-tree xorshift32 with a fixed seed, so runs
// are reproducible. Every operation is "split at exact piece boundaries, merge": a position inside a piece is first
// turned into a boundary by ensure_boundary_ (shrinks the piece, inserts a node for its tail). Node allocations are
// reserved before any mutation, so a failed operation never leaves a half-edited tree.
//
// Unit-2 (UTF-16) alignment: every piece except the last has an even length, so every piece starts at an even
// document offset and piece-local break counts (pairs at even offsets relative to the piece start) stay valid when
// pieces move. Operations that would break this (odd positions or lengths, which only undo/redo across a
// "reload as" can produce) take a slow path: apply, then rebuild the whole piece list with realignment and recount.
#include "ptree.h"
#include <string.h>

enum { NIL = 0 };

// ---------------------------------------------------------------------------------------------------------------
// AddBuffer

AddBuffer::AddBuffer() : chunks_(0), nchunks_(0), used_last_(0), total_(0) {}

AddBuffer::~AddBuffer()
{
  clear();
}

void AddBuffer::clear()
{
  u32 i;
  if (chunks_) {
    for (i = 0; i < nchunks_; i++) e_vfree(chunks_[i], PT_ADD_CHUNK);
    e_free(chunks_);
  }
  chunks_ = 0; nchunks_ = 0; used_last_ = 0; total_ = 0;
}

void AddBuffer::swap(AddBuffer& o)
{
  u8** c = chunks_; u32 n = nchunks_, u = used_last_, t = total_;
  chunks_ = o.chunks_; nchunks_ = o.nchunks_; used_last_ = o.used_last_; total_ = o.total_;
  o.chunks_ = c; o.nchunks_ = n; o.used_last_ = u; o.total_ = t;
}

int AddBuffer::append(const u8* p, epos n, PieceVec* refs)
{
  return append_aligned(p, n, 1, refs);
}

static u32 npieces_of(u32 n)
{
  return n / PT_PIECE_MAX + ((n % PT_PIECE_MAX) ? 1 : 0);
}

int AddBuffer::append_aligned(const u8* p, epos n, u32 align, PieceVec* refs)
{
  u32 pad = 0, room = 0, need = 0, nrefs = 0, rem, take, j;
  if (n == 0) return E_OK;
  if (nchunks_) {
    if (align == 2 && (used_last_ & 1)) pad = 1;
    room = PT_ADD_CHUNK - used_last_ - pad;
  }
  // plan: pieces in the current chunk, then whole new chunks
  take = n < room ? n : room;
  nrefs = npieces_of(take);
  rem = n - take;
  need = rem / PT_ADD_CHUNK + ((rem % PT_ADD_CHUNK) ? 1 : 0);
  nrefs += (rem / PT_ADD_CHUNK) * (PT_ADD_CHUNK / PT_PIECE_MAX) + npieces_of(rem % PT_ADD_CHUNK);
  if (need > (u32)PT_ADD_MAX_CHUNKS - nchunks_) return E_TOOBIG;
  if (refs && refs->reserve(refs->size() + nrefs) != E_OK) return E_NOMEM;
  if (need && !chunks_) {
    chunks_ = (u8**)e_alloc((u32)PT_ADD_MAX_CHUNKS * (u32)sizeof(u8*));
    if (!chunks_) return E_NOMEM;
    memset(chunks_, 0, (u32)PT_ADD_MAX_CHUNKS * sizeof(u8*));
  }
  for (j = 0; j < need; j++) {
    u8* c = (u8*)e_valloc(PT_ADD_CHUNK);
    if (!c) {
      while (j > 0) { j--; e_vfree(chunks_[nchunks_ + j], PT_ADD_CHUNK); chunks_[nchunks_ + j] = 0; }
      return E_NOMEM;
    }
    chunks_[nchunks_ + j] = c;
  }
  // commit (cannot fail from here)
  used_last_ += pad;
  total_ += pad;
  while (n > 0) {
    u32 off;
    if (nchunks_ == 0 || used_last_ == PT_ADD_CHUNK) { nchunks_++; used_last_ = 0; }
    take = PT_ADD_CHUNK - used_last_;
    if (take > n) take = n;
    memcpy(chunks_[nchunks_ - 1] + used_last_, p, take);
    for (off = 0; off < take; ) {
      PieceRef r;
      u32 len = take - off;
      if (len > PT_PIECE_MAX) len = PT_PIECE_MAX;
      r.src = (u16)nchunks_; r.flags = 0; r.start = used_last_ + off; r.len = len; r.nbreaks = 0;
      if (refs) refs->push(r);
      off += len;
    }
    used_last_ += take; total_ += take; p += take; n -= take;
  }
  return E_OK;
}

const u8* AddBuffer::chunk(u16 src) const
{
  if (src == 0 || src > nchunks_) return 0;
  return chunks_[src - 1];
}

u32 AddBuffer::bytes_used() const { return total_; }
u32 AddBuffer::chunk_count() const { return nchunks_; }

u32 AddBuffer::chunk_fill(u16 src) const
{
  if (src == 0 || src > nchunks_) return 0;
  return src == nchunks_ ? used_last_ : (u32)PT_ADD_CHUNK;
}

// ---------------------------------------------------------------------------------------------------------------
// PieceTree: pool, treap primitives

PieceTree::PieceTree()
  : root_(NIL), free_head_(NIL), free_n_(0), rng_(0x9E3779B9u), npieces_(0), stamp_(0), orig_(0)
{
  bs_.unit = 1; bs_.b0 = 0x0A; bs_.b1 = 0; bs_.pad = 0;
}

PieceTree::~PieceTree() {}

u32 PieceTree::slen_(u32 t) const { return t ? pool_[t].slen : 0; }
u32 PieceTree::snb_(u32 t) const { return t ? pool_[t].snb : 0; }

void PieceTree::upd_(u32 t)
{
  Node& n = pool_[t];
  n.slen = n.len + slen_(n.l) + slen_(n.r);
  n.snb = n.nb + snb_(n.l) + snb_(n.r);
}

int PieceTree::reserve_nodes(u32 k)
{
  u32 old, add, i;
  if (free_n_ >= k) return E_OK;
  old = pool_.size();
  add = k - free_n_;
  if (old == 0) add++;                       // index 0 is the nil slot
  if (add > 0x7FFFFFFFu - old) return E_NOMEM;
  if (pool_.resize(old + add) != E_OK) return E_NOMEM;
  for (i = old + add; i > old; i--) {
    u32 t = i - 1;
    if (t == NIL) continue;
    pool_[t].l = free_head_;
    free_head_ = t;
    free_n_++;
  }
  return E_OK;
}

u32 PieceTree::new_node_(const PieceRef& r)
{
  u32 t = free_head_;                        // reserve_nodes() guarantees one
  Node& n = pool_[t];
  free_head_ = n.l;
  free_n_--;
  rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
  n.l = n.r = NIL;
  n.prio = rng_;
  n.src = r.src; n.flags = 0; n.start = r.start; n.len = r.len; n.nb = r.nbreaks;
  n.slen = n.len; n.snb = n.nb;
  return t;
}

void PieceTree::free_node_(u32 t)
{
  pool_[t].l = free_head_;
  pool_[t].r = NIL;
  free_head_ = t;
  free_n_++;
}

u32 PieceTree::free_tree_(u32 t)
{
  u32 l, r, k;
  if (!t) return 0;
  l = pool_[t].l; r = pool_[t].r;
  k = free_tree_(l) + free_tree_(r) + 1;
  free_node_(t);
  return k;
}

// Split by position; pos must be a piece boundary inside t (0 <= pos <= slen(t)).
void PieceTree::split_(u32 t, epos pos, u32* a, u32* b)
{
  epos ll;
  if (!t) { *a = *b = NIL; return; }
  if (pos == 0) { *a = NIL; *b = t; return; }
  if (pos >= pool_[t].slen) { *a = t; *b = NIL; return; }
  ll = slen_(pool_[t].l);
  if (pos <= ll) {
    u32 x, y;
    split_(pool_[t].l, pos, &x, &y);
    pool_[t].l = y;
    upd_(t);
    *a = x; *b = t;
  } else {
    u32 x, y;
    split_(pool_[t].r, pos - ll - pool_[t].len, &x, &y);
    pool_[t].r = x;
    upd_(t);
    *a = t; *b = y;
  }
}

u32 PieceTree::merge_(u32 a, u32 b)
{
  if (!a) return b;
  if (!b) return a;
  if (pool_[a].prio >= pool_[b].prio) {
    pool_[a].r = merge_(pool_[a].r, b);
    upd_(a);
    return a;
  }
  pool_[b].l = merge_(a, pool_[b].l);
  upd_(b);
  return b;
}

u16 PieceTree::tag_() const
{
  u32 t = 0x8000u;
  if (bs_.unit == 2) t |= 0x2000u;
  if (bs_.b1) t |= 0x1000u;
  t |= (u32)(bs_.b0 ^ bs_.b1);
  return (u16)t;
}

// Fast-path alignment for unit 2: pos even and n even, or the odd part ends at the document end.
int PieceTree::aligned_(epos pos, epos n, int last_odd_ok) const
{
  if (bs_.unit != 2) return 1;
  if (pos & 1) return 0;
  if (n & 1) return last_odd_ok;
  return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Byte access and break counting

const u8* PieceTree::src_ptr_(u16 src, u32 start, u32 maxlen, epos* avail)
{
  const u8* p;
  if (src == 0) {
    stamp_++;
    if (!orig_) { *avail = 0; return 0; }
    p = orig_->get(start, avail);
    if (!p || *avail == 0) { *avail = 0; return 0; }
    if (*avail > maxlen) *avail = maxlen;
    return p;
  }
  p = add_.chunk(src);
  if (!p) { *avail = 0; return 0; }
  *avail = maxlen;
  return p + start;
}

// Break units in bytes [start, start + len) of a source; unit 2 counts complete pairs at even offsets from start.
int PieceTree::count_(u16 src, u32 start, u32 len, u32* out)
{
  u32 cnt = 0, off = 0;
  int have = 0;
  u8 lo = 0;
  if (bs_.unit == 2) len &= ~1u;
  while (off < len) {
    epos av = 0;
    const u8* p = src_ptr_(src, start + off, len - off, &av);
    if (!p) return E_IO;
    if (bs_.unit == 1) {
      const u8* q = p;
      const u8* e = p + av;
      u8 b = bs_.b0;
      while (q < e) {
        const u8* f = (const u8*)memchr(q, b, (size_t)(e - q));
        if (!f) break;
        cnt++;
        q = f + 1;
      }
    } else {
      u32 i = 0;
      u8 b0 = bs_.b0, b1 = bs_.b1;
      if (have) { if (lo == b0 && p[0] == b1) cnt++; have = 0; i = 1; }
      for (; i + 1 < av; i += 2) if (p[i] == b0 && p[i + 1] == b1) cnt++;
      if (i < av) { have = 1; lo = p[i]; }
    }
    off += av;
  }
  *out = cnt;
  return E_OK;
}

// Relative offset of break unit k (0-based) within bytes [start, start + len) of a source.
int PieceTree::find_(u16 src, u32 start, u32 len, u32 k, u32* rel)
{
  u32 off = 0;
  int have = 0;
  u8 lo = 0;
  if (bs_.unit == 2) len &= ~1u;
  while (off < len) {
    epos av = 0;
    const u8* p = src_ptr_(src, start + off, len - off, &av);
    if (!p) return E_IO;
    if (bs_.unit == 1) {
      const u8* q = p;
      const u8* e = p + av;
      u8 b = bs_.b0;
      while (q < e) {
        const u8* f = (const u8*)memchr(q, b, (size_t)(e - q));
        if (!f) break;
        if (k == 0) { *rel = off + (u32)(f - p); return E_OK; }
        k--;
        q = f + 1;
      }
    } else {
      u32 i = 0;
      u8 b0 = bs_.b0, b1 = bs_.b1;
      if (have) {
        if (lo == b0 && p[0] == b1) { if (k == 0) { *rel = off - 1; return E_OK; } k--; }
        have = 0; i = 1;
      }
      for (; i + 1 < av; i += 2) {
        if (p[i] == b0 && p[i + 1] == b1) { if (k == 0) { *rel = off + i; return E_OK; } k--; }
      }
      if (i < av) { have = 1; lo = p[i]; }
    }
    off += av;
  }
  return E_STATE;                            // counts out of date (never in a consistent tree)
}

int PieceTree::count_refs(PieceRef* refs, u32 n)
{
  u32 i;
  for (i = 0; i < n; i++) {
    u32 c = 0;
    int rc = count_(refs[i].src, refs[i].start, refs[i].len, &c);
    if (rc != E_OK) return rc;
    refs[i].nbreaks = c;
    refs[i].flags = tag_();
  }
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Lifecycle

void PieceTree::clear()
{
  pool_.release();
  root_ = NIL; free_head_ = NIL; free_n_ = 0; npieces_ = 0;
  rng_ = 0x9E3779B9u;
  add_.clear();
  orig_ = 0;
  stamp_++;
}

int PieceTree::init_blocks_(ByteSource* original, const BreakSpec& bs, ProgressCb* cb, int counted)
{
  epos size, off;
  u32 nblocks;
  int rc;
  clear();
  orig_ = original;
  bs_ = bs;
  size = original ? original->size() : 0;
  nblocks = size / PT_PIECE_MAX + ((size % PT_PIECE_MAX) ? 1 : 0);
  if (reserve_nodes(nblocks) != E_OK) { clear(); return E_NOMEM; }
  for (off = 0; off < size; ) {
    PieceRef r;
    epos len = size - off;
    if (len > PT_PIECE_MAX) len = PT_PIECE_MAX;
    r.src = 0; r.flags = 0; r.start = off; r.len = len; r.nbreaks = 0;
    if (counted) {
      rc = count_(0, off, len, &r.nbreaks);
      if (rc != E_OK) { clear(); return rc; }
      if (cb && cb->progress(off + len, size)) { clear(); return E_CANCEL; }
    }
    root_ = merge_(root_, new_node_(r));
    npieces_++;
    off += len;
  }
  orig_ = original;
  return E_OK;
}

int PieceTree::init(ByteSource* original, const BreakSpec& bs, ProgressCb* cb)
{
  return init_blocks_(original, bs, cb, 1);
}

int PieceTree::init_uncounted(ByteSource* original, const BreakSpec& bs)
{
  return init_blocks_(original, bs, 0, 0);
}

int PieceTree::set_counts(const u32* block_breaks, u32 nblocks)
{
  PieceVec list;
  u32 i;
  if (nblocks != npieces_) return E_STATE;
  if (snapshot(&list) != E_OK) return E_NOMEM;
  for (i = 0; i < nblocks; i++) {
    if (list[i].src != 0 || list[i].start != i * (u32)PT_PIECE_MAX) return E_STATE;
    if (block_breaks[i] > list[i].len) return E_RANGE;
  }
  // walk in order and assign; then recompute sums bottom-up
  {
    Vec<u32> stack;
    u32 t = root_, k = 0;
    if (stack.reserve(64) != E_OK) return E_NOMEM;
    while (t || !stack.empty()) {
      while (t) { if (stack.push(t) != E_OK) return E_NOMEM; t = pool_[t].l; }
      t = stack.back(); stack.pop();
      pool_[t].nb = block_breaks[k++];
      t = pool_[t].r;
    }
  }
  // recompute sums (post-order)
  {
    Vec<u32> order, stack;
    u32 t;
    if (order.reserve(npieces_) != E_OK || stack.reserve(64) != E_OK) return E_NOMEM;
    if (root_ && stack.push(root_) != E_OK) return E_NOMEM;
    while (!stack.empty()) {
      t = stack.back(); stack.pop();
      order.push(t);
      if (pool_[t].l && stack.push(pool_[t].l) != E_OK) return E_NOMEM;
      if (pool_[t].r && stack.push(pool_[t].r) != E_OK) return E_NOMEM;
    }
    for (i = order.size(); i > 0; i--) upd_(order[i - 1]);
  }
  stamp_++;
  return E_OK;
}

// Recount every node in place (same unit: no realignment needed, no allocation).
int PieceTree::recount_(u32 t, ProgressCb* cb, epos* done)
{
  int rc;
  if (!t) return E_OK;
  rc = recount_(pool_[t].l, cb, done);
  if (rc == E_OK) rc = count_(pool_[t].src, pool_[t].start, pool_[t].len, &pool_[t].nb);
  if (rc == E_OK) {
    *done += pool_[t].len;
    if (cb && cb->progress(*done, length())) rc = E_CANCEL;
  }
  if (rc == E_OK) rc = recount_(pool_[t].r, cb, done);
  upd_(t);
  return rc;
}

int PieceTree::set_break_spec(const BreakSpec& bs, ProgressCb* cb)
{
  BreakSpec old = bs_;
  epos done = 0;
  int rc;
  bs_ = bs;
  stamp_++;
  if (!root_) return E_OK;
  if (bs.unit == old.unit) {
    rc = recount_(root_, cb, &done);
    if (rc != E_OK) {                        // put the old counts back (reads that just worked)
      bs_ = old;
      done = 0;
      recount_(root_, 0, &done);
    }
    return rc;
  }
  rc = rebuild_(cb);
  if (rc != E_OK) bs_ = old;
  return rc;
}

// Snapshot the piece list, realign for unit 2, recount every piece with bs_, rebuild a fresh treap. Atomic: on
// failure the tree is unchanged (bytes appended to the add buffer for realignment are simply unused).
int PieceTree::rebuild_(ProgressCb* cb)
{
  PieceVec list, out;
  Vec<Node> old;
  u32 i, n, oroot, ofree, ofreen, opieces;
  epos total = length(), done = 0;
  int rc;
  if (snapshot(&list) != E_OK) return E_NOMEM;
  n = list.size();
  if (bs_.unit == 2) {
    if (out.reserve(n + n / 2 + 2) != E_OK) return E_NOMEM;
    for (i = 0; i < n; i++) {
      PieceRef pc = list[i];
      if (pc.len == 0) continue;
      if ((pc.len & 1) && i + 1 < n) {
        u8 two[2];
        PieceVec tmp;
        epos av = 0;
        const u8* p = src_ptr_(pc.src, pc.start + pc.len - 1, 1, &av);
        if (!p) return E_IO;
        two[0] = p[0];
        p = src_ptr_(list[i + 1].src, list[i + 1].start, 1, &av);
        if (!p) return E_IO;
        two[1] = p[0];
        pc.len--;
        if (pc.len && out.push(pc) != E_OK) return E_NOMEM;
        rc = add_.append_aligned(two, 2, 2, &tmp);
        if (rc != E_OK) return rc;
        if (out.push(tmp[0]) != E_OK) return E_NOMEM;
        list[i + 1].start++;
        list[i + 1].len--;
      } else {
        if (out.push(pc) != E_OK) return E_NOMEM;
      }
    }
    list.swap(out);
    n = list.size();
  }
  for (i = 0; i < n; i++) {
    rc = count_(list[i].src, list[i].start, list[i].len, &list[i].nbreaks);
    if (rc != E_OK) return rc;
    done += list[i].len;
    if (cb && cb->progress(done, total)) return E_CANCEL;
  }
  old.swap(pool_);
  oroot = root_; ofree = free_head_; ofreen = free_n_; opieces = npieces_;
  root_ = NIL; free_head_ = NIL; free_n_ = 0; npieces_ = 0;
  if (reserve_nodes(n) != E_OK) {
    pool_.swap(old);
    root_ = oroot; free_head_ = ofree; free_n_ = ofreen; npieces_ = opieces;
    return E_NOMEM;
  }
  for (i = 0; i < n; i++) root_ = merge_(root_, new_node_(list[i]));
  npieces_ = n;
  stamp_++;
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Queries

epos PieceTree::length() const { return slen_(root_); }
u32 PieceTree::break_count() const { return snb_(root_); }
const BreakSpec& PieceTree::break_spec() const { return bs_; }
ByteSource* PieceTree::original() const { return orig_; }
void PieceTree::set_original(ByteSource* s) { orig_ = s; stamp_++; }
AddBuffer* PieceTree::add_buffer() { return &add_; }
u32 PieceTree::piece_count() const { return npieces_; }
u32 PieceTree::stamp() const { return stamp_; }

int PieceTree::span(epos pos, PSpan* s)
{
  u32 t = root_;
  while (t) {
    Node& n = pool_[t];
    epos ll = slen_(n.l);
    if (pos < ll) { t = n.l; continue; }
    pos -= ll;
    if (pos < n.len) {
      epos av = 0;
      u16 src = n.src;
      const u8* p = src_ptr_(src, n.start + pos, n.len - pos, &av);
      if (!p) { s->p = 0; s->len = 0; s->src = src; return E_IO; }
      s->p = p; s->len = av; s->src = src;
      return E_OK;
    }
    pos -= n.len;
    t = n.r;
  }
  s->p = 0; s->len = 0; s->src = 0;
  return E_RANGE;
}

int PieceTree::read(epos pos, u8* out, epos n)
{
  epos len = length();
  if (pos > len || n > len - pos) return E_RANGE;
  while (n > 0) {
    PSpan s;
    int rc = span(pos, &s);
    if (rc != E_OK) return rc == E_RANGE ? E_IO : rc;
    if (s.len > n) s.len = n;
    memcpy(out, s.p, s.len);
    out += s.len; pos += s.len; n -= s.len;
  }
  return E_OK;
}

epos PieceTree::break_pos(u32 b)
{
  u32 t = root_;
  epos base = 0;
  if (b >= break_count()) return EPOS_NONE;
  while (t) {
    Node& n = pool_[t];
    u32 lb = snb_(n.l);
    if (b < lb) { t = n.l; continue; }
    b -= lb;
    base += slen_(n.l);
    if (b < n.nb) {
      u32 rel = 0;
      if (find_(n.src, n.start, n.len, b, &rel) != E_OK) return EPOS_NONE;
      return base + rel;
    }
    b -= n.nb;
    base += n.len;
    t = n.r;
  }
  return EPOS_NONE;
}

u32 PieceTree::breaks_before(epos pos)
{
  u32 t = root_, acc = 0;
  if (pos >= length()) return break_count();
  while (t) {
    Node& n = pool_[t];
    epos ll = slen_(n.l);
    if (pos < ll) { t = n.l; continue; }
    acc += snb_(n.l);
    pos -= ll;
    if (pos < n.len) {
      u32 rel = pos, c = 0;
      u16 src = n.src;
      u32 start = n.start, len = n.len, nb = n.nb;
      if (bs_.unit == 2) rel &= ~1u;
      if (rel <= len / 2) {
        if (count_(src, start, rel, &c) != E_OK) return acc;
        return acc + c;
      }
      if (count_(src, start + rel, len - rel, &c) != E_OK) return acc;
      return acc + nb - c;
    }
    acc += n.nb;
    pos -= n.len;
    t = n.r;
  }
  return acc;
}

u32 PieceTree::count_nodes_(u32 t) const
{
  u32 k = 0;
  while (t) { k += 1 + count_nodes_(pool_[t].l); t = pool_[t].r; }
  return k;
}

void PieceTree::collect_(u32 t, PieceVec* out) const
{
  while (t) {
    const Node& n = pool_[t];
    PieceRef r;
    collect_(n.l, out);
    r.src = n.src; r.flags = tag_(); r.start = n.start; r.len = n.len; r.nbreaks = n.nb;
    out->push(r);                            // capacity reserved by the caller
    t = n.r;
  }
}

int PieceTree::snapshot(PieceVec* out) const
{
  out->clear();
  if (out->reserve(npieces_) != E_OK) return E_NOMEM;
  collect_(root_, out);
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Edits

// Shrink the piece containing pos (strictly inside) to end at pos: new length k, new count c; fix sums on the path.
void PieceTree::shrink_(u32 t, epos pos, u32 k, u32 c)
{
  epos ll = slen_(pool_[t].l);
  if (pos < ll) shrink_(pool_[t].l, pos, k, c);
  else if (pos - ll < pool_[t].len) { pool_[t].len = k; pool_[t].nb = c; }
  else shrink_(pool_[t].r, pos - ll - pool_[t].len, k, c);
  upd_(t);
}

int PieceTree::ensure_boundary_(epos pos)
{
  u32 t = root_, k, c = 0, a, b, q;
  epos p = pos;
  PieceRef tail;
  int rc;
  while (t) {
    Node& n = pool_[t];
    epos ll = slen_(n.l);
    if (p < ll) { t = n.l; continue; }
    p -= ll;
    if (p == 0) return E_OK;
    if (p < n.len) break;
    p -= n.len;
    t = n.r;
  }
  if (!t) return E_OK;                       // pos == length()
  k = p;
  {
    Node& n = pool_[t];
    u32 kk = k;
    if (bs_.unit == 2) kk &= ~1u;
    if (kk <= n.len / 2) rc = count_(n.src, n.start, kk, &c);
    else {
      u32 s = 0;
      rc = count_(n.src, n.start + kk, n.len - kk, &s);
      c = n.nb - s;
    }
    if (rc != E_OK) return rc;
  }
  if (reserve_nodes(1) != E_OK) return E_NOMEM;
  {
    Node& n = pool_[t];
    tail.src = n.src; tail.flags = 0; tail.start = n.start + k; tail.len = n.len - k; tail.nbreaks = n.nb - c;
  }
  shrink_(root_, pos, k, c);
  split_(root_, pos, &a, &b);
  q = new_node_(tail);
  root_ = merge_(merge_(a, q), b);
  npieces_++;
  stamp_++;
  return E_OK;
}

// Insert prepared refs (counted, tagged) at pos. Fast path only (caller checked alignment).
int PieceTree::insert_list_(epos pos, const PieceRef* refs, u32 n, int coalesce)
{
  u32 a, b, i, m = NIL;
  int rc = ensure_boundary_(pos);
  if (rc != E_OK) return rc;
  if (reserve_nodes(n) != E_OK) return E_NOMEM;
  split_(root_, pos, &a, &b);
  if (coalesce && n == 1 && a && refs[0].src != 0) {
    u32 t = a;
    while (pool_[t].r) t = pool_[t].r;
    if (pool_[t].src == refs[0].src && pool_[t].start + pool_[t].len == refs[0].start &&
        pool_[t].len + refs[0].len <= (u32)PT_PIECE_MAX) {
      pool_[t].len += refs[0].len;
      pool_[t].nb += refs[0].nbreaks;
      for (t = a; t; t = pool_[t].r) { pool_[t].slen += refs[0].len; pool_[t].snb += refs[0].nbreaks; }
      root_ = merge_(a, b);
      stamp_++;
      return E_OK;
    }
  }
  for (i = 0; i < n; i++) m = merge_(m, new_node_(refs[i]));
  root_ = merge_(merge_(a, m), b);
  npieces_ += n;
  stamp_++;
  return E_OK;
}

int PieceTree::insert(epos pos, const u8* p, epos n, PieceVec* added)
{
  PieceVec loc;
  epos len = length();
  int rc;
  if (n == 0) return E_OK;
  if (pos > len) return E_RANGE;
  if (n > EPOS_MAX - len) return E_TOOBIG;
  rc = add_.append_aligned(p, n, bs_.unit, &loc);
  if (rc != E_OK) return rc;
  count_refs(loc.data(), loc.size());        // add-buffer bytes: cannot fail
  if (added && added->reserve(added->size() + loc.size()) != E_OK) return E_NOMEM;
  rc = insert_refs(pos, loc.data(), loc.size());
  if (rc != E_OK) return rc;
  if (added) added->insert(added->size(), loc.data(), loc.size());
  return E_OK;
}

int PieceTree::insert_refs(epos pos, const PieceRef* refs, u32 n)
{
  PieceVec loc;
  epos len = length(), total = 0;
  u32 i;
  int rc, fast, stale = 0;
  if (n == 0) return E_OK;
  if (pos > len) return E_RANGE;
  for (i = 0; i < n; i++) {
    if (refs[i].len == 0 || refs[i].len > PT_PIECE_MAX) return E_RANGE;
    if (refs[i].len > EPOS_MAX - len - total) return E_TOOBIG;
    total += refs[i].len;
    if (refs[i].flags != tag_()) stale = 1;
  }
  if (stale) {                                // counted for another break spec: recount a copy
    if (loc.insert(0, refs, n) != E_OK) return E_NOMEM;
    rc = count_refs(loc.data(), n);
    if (rc != E_OK) return rc;
    refs = loc.data();
  }
  fast = 1;
  if (bs_.unit == 2) {
    if (pos & 1) fast = 0;
    for (i = 0; fast && i + 1 < n; i++) if (refs[i].len & 1) fast = 0;
    if (fast && (refs[n - 1].len & 1) && pos != len) fast = 0;
  }
  if (fast) return insert_list_(pos, refs, n, n == 1 && !stale);
  // slow path: insert, then realign and recount everything (undo across a reinterpret)
  {
    Vec<Node> bk;
    u32 oroot = root_, ofree = free_head_, ofreen = free_n_, opieces = npieces_;
    if (bk.insert(0, pool_.data(), pool_.size()) != E_OK) return E_NOMEM;
    rc = insert_list_(pos, refs, n, 0);
    if (rc != E_OK) return rc;
    rc = rebuild_(0);
    if (rc != E_OK) {
      pool_.swap(bk);
      root_ = oroot; free_head_ = ofree; free_n_ = ofreen; npieces_ = opieces;
      stamp_++;
    }
    return rc;
  }
}

int PieceTree::remove_raw_(epos pos, epos n, PieceVec* removed)
{
  u32 a, b, m, c, k;
  int rc = ensure_boundary_(pos);
  if (rc != E_OK) return rc;
  rc = ensure_boundary_(pos + n);
  if (rc != E_OK) return rc;
  split_(root_, pos, &a, &b);
  split_(b, n, &m, &c);
  if (removed) {
    if (removed->reserve(removed->size() + count_nodes_(m)) != E_OK) {
      root_ = merge_(a, merge_(m, c));
      return E_NOMEM;
    }
    collect_(m, removed);
  }
  k = free_tree_(m);
  npieces_ -= k;
  root_ = merge_(a, c);
  stamp_++;
  return E_OK;
}

int PieceTree::remove(epos pos, epos n, PieceVec* removed)
{
  epos len = length();
  u32 old = removed ? removed->size() : 0;
  int rc;
  if (n == 0) return E_OK;
  if (pos > len || n > len - pos) return E_RANGE;
  if (aligned_(pos, n, pos + n == len)) return remove_raw_(pos, n, removed);
  {
    Vec<Node> bk;
    u32 oroot = root_, ofree = free_head_, ofreen = free_n_, opieces = npieces_;
    if (bk.insert(0, pool_.data(), pool_.size()) != E_OK) return E_NOMEM;
    rc = remove_raw_(pos, n, removed);
    if (rc != E_OK) return rc;
    rc = rebuild_(0);
    if (rc != E_OK) {
      pool_.swap(bk);
      root_ = oroot; free_head_ = ofree; free_n_ = ofreen; npieces_ = opieces;
      if (removed) removed->resize(old);
      stamp_++;
    }
    return rc;
  }
}

void PieceTree::swap(PieceTree& o)
{
  u32 t;
  ByteSource* s;
  BreakSpec b;
  pool_.swap(o.pool_);
  t = root_; root_ = o.root_; o.root_ = t;
  t = free_head_; free_head_ = o.free_head_; o.free_head_ = t;
  t = free_n_; free_n_ = o.free_n_; o.free_n_ = t;
  t = rng_; rng_ = o.rng_; o.rng_ = t;
  t = npieces_; npieces_ = o.npieces_; o.npieces_ = t;
  s = orig_; orig_ = o.orig_; o.orig_ = s;
  b = bs_; bs_ = o.bs_; o.bs_ = b;
  add_.swap(o.add_);
  stamp_++; o.stamp_++;
}

// ---------------------------------------------------------------------------------------------------------------
// Invariants and dump

enum { PT_CHECK_RECOUNT_MAX = 4 * 1024 * 1024 };   // full byte recount only for documents up to 4 MB

int PieceTree::check_(u32 t, u32 maxprio, u32* nodes, u32* len, u32* nb, epos base, char* why, u32 cap,
                      int recount)
{
  u32 ln = 0, lb = 0, rn = 0, rb = 0;
  Node n;
  if (!t) { *len = 0; *nb = 0; return 0; }
  if (t >= pool_.size()) { es_fmt(why, cap, "node index %u out of range", t); return 1; }
  if (++*nodes > npieces_) { es_fmt(why, cap, "more nodes reachable than piece_count %u (cycle?)", npieces_); return 1; }
  n = pool_[t];
  if (n.prio > maxprio) { es_fmt(why, cap, "heap order broken at node %u", t); return 1; }
  if (n.len == 0 || n.len > PT_PIECE_MAX) { es_fmt(why, cap, "piece at %u has length %u", base, n.len); return 1; }
  if (check_(n.l, n.prio, nodes, &ln, &lb, base, why, cap, recount)) return 1;
  base += ln;
  if (n.src == 0) {
    if (!orig_ || n.start > orig_->size() || n.len > orig_->size() - n.start) {
      es_fmt(why, cap, "original piece at %u out of the source (start %u len %u)", base, n.start, n.len);
      return 1;
    }
  } else if (n.src > add_.chunk_count() || n.start > add_.chunk_fill(n.src) ||
             n.len > add_.chunk_fill(n.src) - n.start) {
    es_fmt(why, cap, "add piece at %u out of chunk %u (start %u len %u)", base, n.src, n.start, n.len);
    return 1;
  }
  if (bs_.unit == 2 && (n.len & 1) && base + n.len != length()) {
    es_fmt(why, cap, "unit-2 piece at %u has odd length %u and is not last", base, n.len);
    return 1;
  }
  if (n.nb > n.len) { es_fmt(why, cap, "piece at %u: nbreaks %u > len %u", base, n.nb, n.len); return 1; }
  if (recount || n.src != 0) {
    u32 c = 0;
    if (count_(n.src, n.start, n.len, &c) != E_OK) { es_fmt(why, cap, "read error at %u", base); return 1; }
    if (c != n.nb) { es_fmt(why, cap, "piece at %u: nbreaks %u, recount %u", base, n.nb, c); return 1; }
  }
  if (check_(n.r, n.prio, nodes, &rn, &rb, base + n.len, why, cap, recount)) return 1;
  if (n.slen != ln + n.len + rn) { es_fmt(why, cap, "length sum wrong at node %u", t); return 1; }
  if (n.snb != lb + n.nb + rb) { es_fmt(why, cap, "break sum wrong at node %u", t); return 1; }
  *len = n.slen;
  *nb = n.snb;
  return 0;
}

int PieceTree::check(char* why, u32 cap)
{
  char dummy[4];
  u32 nodes = 0, len = 0, nb = 0, f, k;
  if (!why || !cap) { why = dummy; cap = sizeof(dummy); }
  why[0] = 0;
  if (root_ >= pool_.size() && root_ != NIL) { es_fmt(why, cap, "root %u out of range", root_); return 1; }
  if (check_(root_, 0xFFFFFFFFu, &nodes, &len, &nb, 0, why, cap, length() <= PT_CHECK_RECOUNT_MAX)) return 1;
  if (nodes != npieces_) { es_fmt(why, cap, "reachable %u != piece_count %u", nodes, npieces_); return 1; }
  for (f = free_head_, k = 0; f != NIL; f = pool_[f].l) {
    if (f >= pool_.size()) { es_fmt(why, cap, "free list index %u out of range", f); return 1; }
    if (++k > free_n_) { es_fmt(why, cap, "free list longer than %u", free_n_); return 1; }
  }
  if (k != free_n_) { es_fmt(why, cap, "free list has %u nodes, expected %u", k, free_n_); return 1; }
  if (pool_.size() && nodes + free_n_ + 1 != pool_.size()) {
    es_fmt(why, cap, "pool %u != used %u + free %u + nil", pool_.size(), nodes, free_n_);
    return 1;
  }
  return 0;
}

void PieceTree::dump(EStr* out)
{
  PieceVec list;
  u32 i;
  epos at = 0;
  out->appendf("PieceTree: length %u, breaks %u, pieces %u, unit %u, add %u bytes in %u chunks\n", length(),
               break_count(), npieces_, (u32)bs_.unit, add_.bytes_used(), add_.chunk_count());
  if (snapshot(&list) != E_OK) { out->append("  (out of memory)\n"); return; }
  for (i = 0; i < list.size(); i++) {
    if (i == 200) { out->appendf("  ... %u more\n", list.size() - i); break; }
    out->appendf("  %u: at %u src %u start %u len %u nb %u\n", i, at, (u32)list[i].src, list[i].start, list[i].len,
                 list[i].nbreaks);
    at += list[i].len;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// SnapshotSource

SnapshotSource::SnapshotSource() : add_(0), orig_(0), size_(0), err_(E_OK), last_(0) {}
SnapshotSource::~SnapshotSource() {}

int SnapshotSource::init(const PieceVec& pieces, const AddBuffer* add, ByteSource* original_private)
{
  u32 i, n = pieces.size();
  epos at = 0;
  pieces_.clear(); offs_.clear();
  size_ = 0; err_ = E_OK; last_ = 0;
  if (pieces_.reserve(n) != E_OK || offs_.reserve(n) != E_OK) { pieces_.release(); offs_.release(); return E_NOMEM; }
  for (i = 0; i < n; i++) {
    pieces_.push(pieces[i]);
    offs_.push(at);
    at += pieces[i].len;
  }
  add_ = add; orig_ = original_private; size_ = at;
  return E_OK;
}

epos SnapshotSource::size() const { return size_; }
int SnapshotSource::err() const { return err_; }

const u8* SnapshotSource::get(epos off, epos* avail_out)
{
  epos dummy;
  epos* avail = avail_out ? avail_out : &dummy;
  u32 lo, hi, i;
  epos rel;
  const PieceRef* r;
  if (off >= size_) { *avail = 0; return 0; }
  i = last_;                                 // sequential reads hit the same or the next piece
  if (!(i < pieces_.size() && offs_[i] <= off && off - offs_[i] < pieces_[i].len)) {
    if (i + 1 < pieces_.size() && offs_[i + 1] <= off && off - offs_[i + 1] < pieces_[i + 1].len) i++;
    else {
      lo = 0; hi = pieces_.size();
      while (hi - lo > 1) {
        u32 mid = lo + (hi - lo) / 2;
        if (offs_[mid] <= off) lo = mid; else hi = mid;
      }
      i = lo;
    }
  }
  last_ = i;
  r = &pieces_[i];
  rel = off - offs_[i];
  if (r->src == 0) {
    const u8* p;
    epos av = 0;
    if (!orig_) { err_ = E_IO; *avail = 0; return 0; }
    p = orig_->get(r->start + rel, &av);
    if (!p || av == 0) { err_ = orig_->err() != E_OK ? orig_->err() : E_IO; *avail = 0; return 0; }
    if (av > r->len - rel) av = r->len - rel;
    *avail = av;
    return p;
  }
  {
    const u8* c = add_ ? add_->chunk(r->src) : 0;
    if (!c) { err_ = E_IO; *avail = 0; return 0; }
    *avail = r->len - rel;
    return c + r->start + rel;
  }
}
