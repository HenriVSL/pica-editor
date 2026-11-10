// fif.cpp - find in files core (DESIGN 6.16, 11.6). Owner: T11.
// fif_run walks a folder (sorted, depth-first, skipping hidden/system folders), filters names with the masks, reads
// each file (whole up to 16 MB, else through a small block cache), skips binary files (NUL in the first 4 KB unless
// UTF-16), detects the encoding, and reports line-based hits through FifSink. fif_search_source builds the hits over
// any ByteSource (also used for Find All in an open document).
#include "fif.h"
#include "fif_int.h"
#include "estr.h"
#include "emem.h"
#include <string.h>

static u32 g_mem_limit = 16u << 20;
static u32 g_block = 256u << 10;

void fif_test_limits(u32 mem_limit, u32 block)
{
  g_mem_limit = mem_limit ? mem_limit : (16u << 20);
  g_block = block ? block : (256u << 10);
}

// ---------------------------------------------------------------------------------------------------------------
// masks

static int mask_sep(char ch) { return ch == ';' || ch == ' ' || ch == ',' || ch == '\t'; }

static const char* mask_token(const char* p, char* tok, u32 cap)
{
  u32 n = 0;
  while (*p && mask_sep(*p)) p++;
  if (!*p) return 0;
  while (*p && !mask_sep(*p)) { if (n + 1 < cap) tok[n++] = *p; p++; }
  tok[n] = 0;
  return p;
}

static int dir_token(const char* t) { return t[0] == '!' && (t[1] == '\\' || t[1] == '/'); }

int fif_mask_match(const char* masks, const char* name)
{
  char tok[260];
  const char* p = masks ? masks : "";
  int have_inc = 0, inc = 0;
  if (!name) return 0;
  while ((p = mask_token(p, tok, sizeof(tok))) != 0) {
    if (tok[0] == '!') {
      if (dir_token(tok) || !tok[1]) continue;
      if (es_wildcard(tok + 1, name)) return 0;
      continue;
    }
    have_inc = 1;
    if (!inc && es_wildcard(tok, name)) inc = 1;
  }
  return have_inc ? inc : 1;
}

int fif_dir_excluded(const char* masks, const char* name)
{
  char tok[260];
  const char* p = masks ? masks : "";
  if (!name) return 0;
  while ((p = mask_token(p, tok, sizeof(tok))) != 0)
    if (dir_token(tok) && tok[2] && es_wildcard(tok + 2, name)) return 1;
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// hit builder

class FifCancelCb : public ProgressCb {
public:
  volatile i32* flag;
  FifCancelCb(volatile i32* f) : flag(f) {}
  int progress(u32 done, u32 total) { (void)done; (void)total; return flag && *flag ? 1 : 0; }
};

struct LineScan { u32 unit; int be; u32 brk; };

// Counts the break units starting in [a, b); after each one *line++ and *line_start = the position after it.
static int count_breaks(ByteSource* s, const LineScan& ls, epos a, epos b, u32* line, epos* line_start)
{
  epos pos = a;
  if (ls.unit == 1) {
    while (pos < b) {
      epos avail = 0;
      const u8* p = s->get(pos, &avail);
      const u8* q;
      const u8* e;
      if (!p) return s->err() != E_OK ? s->err() : E_IO;
      if (avail > b - pos) avail = b - pos;
      q = p; e = p + avail;
      while (q < e && (q = (const u8*)memchr(q, (int)ls.brk, (size_t)(e - q))) != 0) {
        q++;
        (*line)++;
        *line_start = pos + (epos)(q - p);
      }
      pos += avail;
    }
    return E_OK;
  }
  if (pos & 1) pos++;                       // units sit at even offsets
  while (pos < b && b - pos >= 2) {
    epos avail = 0, n, i;
    const u8* p = s->get(pos, &avail);
    u8 tmp[2];
    if (!p) return s->err() != E_OK ? s->err() : E_IO;
    n = avail & ~(epos)1;
    if (n > b - pos) n = (b - pos) & ~(epos)1;
    if (n == 0) {                           // a unit split between two spans
      int rc = s->read(pos, tmp, 2);
      if (rc != E_OK) return rc;
      p = tmp; n = 2;
    }
    for (i = 0; i < n; i += 2) {
      u32 u = ls.be ? ((u32)p[i] << 8 | p[i + 1]) : ((u32)p[i] | (u32)p[i + 1] << 8);
      if (u == ls.brk) { (*line)++; *line_start = pos + i + 2; }
    }
    pos += n;
  }
  return E_OK;
}

enum { RING = 64, LEAD = 40, FULL_UPTO = 200 };

static u32 put_utf8(char* out, u32 at, ecp c)
{
  if (ECP_IS_BAD(c) || (c >= 0xD800u && c <= 0xDFFFu)) c = ECP_REPLACEMENT;
  return at + es_utf8_encode(c, out + at);
}

struct HitState {
  u32 line; epos line_start;                // line of scan_pos and where it starts
  epos scan_pos;                            // breaks before here are counted
  epos col_pos; u32 col;                    // character cursor on the current line
  epos ring[RING];                          // start offsets of the last RING characters of the line (index % RING)
};

// Character index of m.pos on its line (advances the cursor, records char starts in the ring).
static int advance_col(ByteSource* s, const Codec* c, HitState* h, epos to)
{
  CharReader rd;
  ecp cp;
  epos at;
  int r;
  if (h->col_pos >= to) return E_OK;
  rd.init(s, c, h->col_pos, to);
  for (;;) {
    r = rd.next(&cp, &at);
    if (r < 0) return r;
    if (r == 0) break;
    h->ring[h->col % RING] = at;
    h->col++;
  }
  h->col_pos = to;
  return E_OK;
}

// Builds the hit text for a match at m (h->col = chars before m.pos on the line).
static int build_text(ByteSource* s, const Codec* c, const HitState* h, const LineScan& ls, int cr_mode, epos hi,
                      const RxMatch& m, FifHit* out)
{
  ecp buf[FIF_TEXT_CHARS + 2];
  epos start;
  u32 kchar, prefix, budget, n = 0, kept, hl0 = 0, hl_n = 0, i, k, more = 0, have_hl0 = 0;
  int pend_cr = 0;
  CharReader rd;
  (void)ls;
  if (h->col <= FULL_UPTO) { start = h->line_start; kchar = 0; prefix = 0; }
  else { kchar = h->col - LEAD; start = h->ring[kchar % RING]; prefix = 3; }
  budget = FIF_TEXT_CHARS - prefix;
  rd.init(s, c, start, hi);
  for (;;) {
    ecp cp;
    epos at;
    int r = rd.next(&cp, &at);
    if (r < 0) return r;
    if (r == 0) break;
    if (!cr_mode) {
      if (cp == 0x0A) { pend_cr = 0; break; }
      if (pend_cr) {                        // the CR was not part of a CRLF: show it
        pend_cr = 0;
        if (n > budget) { more = 1; break; }
        buf[n++] = ECP_REPLACEMENT;
      }
      if (cp == 0x0D) {
        pend_cr = 1;
        if (!have_hl0 && at >= m.pos) { hl0 = n; have_hl0 = 1; }
        if (at >= m.pos && at < m.pos + m.len) hl_n++;
        continue;
      }
    } else {
      if (cp == 0x0D) break;
    }
    if (n > budget) { more = 1; break; }
    if (!have_hl0 && at >= m.pos) { hl0 = n; have_hl0 = 1; }
    if (at >= m.pos && at < m.pos + m.len) hl_n++;
    if (cp == 0 || cp == 0x0A || cp == 0x0D) cp = ECP_REPLACEMENT;
    buf[n++] = cp;
  }
  if (pend_cr && n <= budget) buf[n++] = ECP_REPLACEMENT;   // a lone CR at the very end
  if (!have_hl0) hl0 = n;                   // match at the line end (or an empty match there)
  if (n > budget) more = 1;
  kept = more ? budget - 3 : n;
  if (hl0 > kept) { hl0 = kept; hl_n = 0; }
  if (hl0 + hl_n > kept) hl_n = kept - hl0;
  k = 0;
  if (prefix) { out->text[k++] = '.'; out->text[k++] = '.'; out->text[k++] = '.'; }
  for (i = 0; i < kept; i++) k = put_utf8(out->text, k, buf[i]);
  if (more) { out->text[k++] = '.'; out->text[k++] = '.'; out->text[k++] = '.'; }
  out->text[k] = 0;
  out->text_len = k;
  out->hl_start = prefix + hl0;
  out->hl_len = hl_n;
  return E_OK;
}

int fif_search_source(ByteSource* s, const Codec* c, epos lo, epos hi, int cr_mode, Searcher& srch, u32 file_index,
                      const char* path, FifSink* sink, u32 max_hits, u32* hits, volatile i32* cancel)
{
  HitState h;
  LineScan ls;
  FifCancelCb cb(cancel);
  RxMatch m;
  FifHit* hit;
  epos from = lo;
  u32 nh = 0;
  int rc = E_OK, r;
  if (hits) *hits = 0;
  if (!s || !c) return E_RANGE;
  if (hi > s->size()) hi = s->size();
  if (lo > hi) return E_OK;
  ls.unit = c->unit >= 2 ? 2 : 1;
  ls.be = c->kind == EK_UTF16BE;
  ls.brk = cr_mode ? 0x0D : 0x0A;
  h.line = 0; h.line_start = lo; h.scan_pos = lo; h.col_pos = lo; h.col = 0;
  hit = (FifHit*)e_alloc(sizeof(FifHit));
  if (!hit) return E_NOMEM;
  for (;;) {
    if (cancel && *cancel) { rc = E_CANCEL; break; }
    if (from > hi) break;
    r = srch.find(s, c, lo, hi, from, 0, &m, &cb);
    if (r < 0) { rc = r; break; }
    if (r == 0) break;
    rc = count_breaks(s, ls, h.scan_pos, m.pos, &h.line, &h.line_start);
    if (rc != E_OK) break;
    h.scan_pos = m.pos;
    if (h.line_start > h.col_pos || h.col_pos > m.pos) { h.col_pos = h.line_start; h.col = 0; }
    rc = advance_col(s, c, &h, m.pos);
    if (rc != E_OK) break;
    hit->file_index = file_index;
    hit->line = h.line;
    hit->col = h.col;
    hit->pos = m.pos;
    hit->len = m.len;
    rc = build_text(s, c, &h, ls, cr_mode, hi, m, hit);
    if (rc != E_OK) break;
    if (nh == 0 && path && sink && sink->on_file(path, file_index)) { rc = E_CANCEL; break; }
    nh++;
    if (sink && sink->on_hit(*hit)) { rc = E_CANCEL; break; }
    if (max_hits && nh >= max_hits) { rc = E_TOOBIG; break; }
    if (m.len) from = m.pos + m.len;
    else {
      ecp cp;
      u8 tmp[8];
      epos avail = hi - m.pos;
      u32 used;
      if (m.pos >= hi) break;
      if (avail > c->max_len) avail = c->max_len;
      rc = s->read(m.pos, tmp, avail);
      if (rc != E_OK) break;
      used = c->decode(tmp, avail, 1, &cp);
      from = m.pos + (used ? used : 1);
    }
  }
  e_free(hit);
  if (hits) *hits = nh;
  return rc;
}

int fif_search_buffer(const u8* p, u32 n, const DetectResult& det, Searcher& s, u32 file_index, FifSink* sink, u32* hits)
{
  ViewSource src(p, n);
  const Codec* c = codec_get(det.enc);
  if (!c) c = codec_get(enc_cp(1252));
  return fif_search_source(&src, c, det.bom_len <= n ? det.bom_len : n, n, det.eol_style == EOL_CR, s, file_index, 0,
                           sink, 0, hits, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// file source for files above the in-memory limit: 4 cached blocks

class FifFileSource : public ByteSource {
public:
  FifFileSource(FileSys* fs, FsFile f, epos n) : fs_(fs), f_(f), size_(n), bs_(g_block), tick_(0), err_(E_OK)
  {
    u32 i;
    for (i = 0; i < 4; i++) { blk_[i] = 0; off_[i] = 0; len_[i] = 0; use_[i] = 0; }
  }
  ~FifFileSource()
  {
    u32 i;
    for (i = 0; i < 4; i++) if (blk_[i]) e_vfree(blk_[i], bs_);
  }
  int init()
  {
    u32 i;
    for (i = 0; i < 4; i++) { blk_[i] = (u8*)e_valloc(bs_); if (!blk_[i]) return E_NOMEM; }
    return E_OK;
  }
  epos size() const { return size_; }
  int err() const { return err_; }
  const u8* get(epos off, epos* avail)
  {
    u32 i, victim = 0, got = 0;
    epos base;
    int rc;
    *avail = 0;
    if (off >= size_) return 0;
    for (i = 0; i < 4; i++) {
      if (len_[i] && off >= off_[i] && off < off_[i] + len_[i]) {
        use_[i] = ++tick_;
        *avail = off_[i] + len_[i] - off;
        return blk_[i] + (off - off_[i]);
      }
      if (use_[i] < use_[victim]) victim = i;
    }
    base = off - off % bs_;
    rc = fs_->read_at(f_, base, 0, blk_[victim], (u32)emin<epos>(bs_, size_ - base), &got);
    if (rc != E_OK || got == 0 || base + got <= off) { err_ = rc != E_OK ? rc : E_IO; len_[victim] = 0; return 0; }
    off_[victim] = base; len_[victim] = got; use_[victim] = ++tick_;
    *avail = base + got - off;
    return blk_[victim] + (off - base);
  }
private:
  FileSys* fs_; FsFile f_; epos size_; u32 bs_; u32 tick_; int err_;
  u8* blk_[4]; epos off_[4]; epos len_[4]; u32 use_[4];
  FifFileSource(const FifFileSource&); FifFileSource& operator=(const FifFileSource&);
};

// ---------------------------------------------------------------------------------------------------------------
// walk

struct FifSrch { EncSpec enc; Searcher* s; };
struct FifEnt { u32 name; u32 attrs; u32 size_lo, size_hi; };

struct FifRun {
  FileSys* fs; const FifQuery* q; FifSink* sink; volatile i32* cancel;
  u32 searched, seen, fwh, hits, max_hits;
  FifSrch cache[6]; u32 ncache;
  u8* buf; u32 cap;
  char sep;
};

static Searcher* run_searcher(FifRun* r, const EncSpec& e, int* rc)
{
  u32 i;
  SearchError err;
  Searcher* s;
  for (i = 0; i < r->ncache; i++) if (enc_equal(r->cache[i].enc, e)) return r->cache[i].s;
  s = new Searcher;
  if (!s) { *rc = E_NOMEM; return 0; }
  *rc = s->compile(r->q->pattern, r->q->pattern_len, r->q->opts, e, &err);
  if (*rc != E_OK) { delete s; return 0; }
  if (r->ncache == sizeof(r->cache) / sizeof(r->cache[0])) {   // full: replace the newest entry
    delete r->cache[r->ncache - 1].s;
    r->ncache--;
  }
  r->cache[r->ncache].enc = e;
  r->cache[r->ncache].s = s;
  r->ncache++;
  return s;
}

static int grow_buf(FifRun* r, u32 need)
{
  u32 cap;
  u8* p;
  if (need <= r->cap && r->buf) return E_OK;
  cap = (need + 0xFFFFu) & ~0xFFFFu;
  p = (u8*)e_valloc(cap);
  if (!p) return E_NOMEM;
  if (r->buf) e_vfree(r->buf, r->cap);
  r->buf = p; r->cap = cap;
  return E_OK;
}

static int cancelled(FifRun* r) { return r->cancel && *r->cancel; }

static int read_into(FifRun* r, FsFile f, u32 off, u32 n)
{
  while (n) {
    u32 chunk = n > (1u << 20) ? (1u << 20) : n, got = 0;
    int rc;
    if (cancelled(r)) return E_CANCEL;
    rc = r->fs->read_at(f, off, 0, r->buf + off, chunk, &got);
    if (rc != E_OK) return rc;
    if (got == 0) return E_IO;             // the file shrank
    off += got; n -= got;
  }
  return E_OK;
}

// One file: returns E_OK (also for skipped files), E_CANCEL/E_TOOBIG to stop the walk, E_NOMEM.
static int search_file(FifRun* r, const char* path, u32 size_lo, u32 size_hi)
{
  FsFile f;
  DetectResult det;
  u32 n = size_lo, sample, nh = 0;
  int rc, inmem;
  const Codec* c;
  Searcher* s;
  if (r->sink->on_progress(r->searched, r->seen, path)) return E_CANCEL;
  if (size_hi) { r->sink->on_skip(path, E_TOOBIG); return E_OK; }
  rc = r->fs->open(path, FS_READ | FS_SHARE_ALL, &f);
  if (rc != E_OK) { r->sink->on_skip(path, rc); return E_OK; }
  r->searched++;
  sample = n < 65536u ? n : 65536u;
  inmem = n <= g_mem_limit;
  rc = grow_buf(r, inmem ? (n ? n : 1) : (sample ? sample : 1));
  if (rc == E_NOMEM && inmem && n > sample) { inmem = 0; rc = grow_buf(r, sample ? sample : 1); }
  if (rc == E_OK) rc = read_into(r, f, 0, sample);
  if (rc != E_OK) {
    r->fs->close(f);
    if (rc == E_CANCEL || rc == E_NOMEM) return rc;
    r->sink->on_skip(path, rc);
    return E_OK;
  }
  if (detect_is_binary(r->buf, sample < FIF_BINARY_SAMPLE ? sample : FIF_BINARY_SAMPLE)) { r->fs->close(f); return E_OK; }
  detect(r->buf, sample, n == sample, r->q->det, &det);
  if (det.utf32) { r->fs->close(f); return E_OK; }
  c = codec_get(det.enc);
  if (!c) { det.enc = enc_cp(1252); c = codec_get(det.enc); }
  s = run_searcher(r, det.enc, &rc);
  if (!s) {
    r->fs->close(f);
    if (rc == E_NOMEM) return rc;
    r->sink->on_skip(path, rc);
    return E_OK;
  }
  if (s->impossible()) { r->fs->close(f); return E_OK; }
  {
    u32 left = r->max_hits ? r->max_hits - r->hits : 0;
    if (inmem && n > sample) rc = read_into(r, f, sample, n - sample);
    if (rc == E_OK && inmem) {
      ViewSource src(r->buf, n);
      rc = fif_search_source(&src, c, det.bom_len, n, det.eol_style == EOL_CR, *s, r->fwh, path, r->sink, left, &nh,
                             r->cancel);
    } else if (rc == E_OK) {
      FifFileSource src(r->fs, f, n);
      rc = src.init();
      if (rc == E_OK)
        rc = fif_search_source(&src, c, det.bom_len, n, det.eol_style == EOL_CR, *s, r->fwh, path, r->sink, left,
                               &nh, r->cancel);
    }
  }
  r->fs->close(f);
  if (nh) { r->fwh++; r->hits += nh; }
  if (rc == E_CANCEL || rc == E_TOOBIG || rc == E_NOMEM) return rc;
  if (rc != E_OK) r->sink->on_skip(path, rc);   // E_IO, E_BUDGET: report and go on
  return E_OK;
}

static int ent_less(const char* names, const FifEnt& a, const FifEnt& b)
{
  int d = es_icmp(names + a.name, names + b.name);
  if (d == 0) d = strcmp(names + a.name, names + b.name);
  return d < 0;
}

static void sort_ents(const char* names, FifEnt* e, u32 n, FifEnt* tmp)
{
  u32 width, i;
  for (width = 1; width < n; width *= 2) {          // bottom-up merge sort (stable)
    for (i = 0; i < n; i += 2 * width) {
      u32 a = i, mid = emin(i + width, n), b = mid, end = emin(i + 2 * width, n), k = i;
      while (a < mid && b < end) tmp[k++] = ent_less(names, e[b], e[a]) ? e[b++] : e[a++];
      while (a < mid) tmp[k++] = e[a++];
      while (b < end) tmp[k++] = e[b++];
    }
    memcpy(e, tmp, n * sizeof(FifEnt));
  }
}

static void join_path(const char* dir, const char* name, char sep, char* out, u32 cap)
{
  u32 n;
  es_copy(out, cap, dir);
  n = es_len(out);
  if (n && out[n - 1] != '\\' && out[n - 1] != '/') { char s2[2]; s2[0] = sep; s2[1] = 0; es_cat(out, cap, s2); }
  es_cat(out, cap, name);
}

static int walk_dir(FifRun* r, const char* dir, Vec<char*>* stack)
{
  FsFind fd;
  EBytes names;
  Vec<FifEnt> ents, tmp;
  u32 i;
  int rc = r->fs->find_first(dir, &fd);
  if (rc == E_NOTFOUND) return E_OK;
  if (rc != E_OK) { r->sink->on_skip(dir, rc); return rc == E_NOMEM ? rc : E_OK; }
  for (;;) {
    FifEnt e;
    e.name = names.size(); e.attrs = fd.attrs; e.size_lo = fd.size_lo; e.size_hi = fd.size_hi;
    if (names.append(fd.name, es_len(fd.name) + 1) != E_OK || ents.push(e) != E_OK) { rc = E_NOMEM; break; }
    rc = r->fs->find_next(&fd);
    if (rc != E_OK) { rc = rc == E_NOTFOUND ? E_OK : rc; break; }
    if (cancelled(r)) { rc = E_CANCEL; break; }
  }
  r->fs->find_close(&fd);
  if (rc == E_CANCEL || rc == E_NOMEM) return rc;
  if (ents.empty()) return E_OK;
  if (tmp.resize(ents.size()) != E_OK) return E_NOMEM;
  sort_ents((const char*)names.data(), ents.data(), ents.size(), tmp.data());
  for (i = 0; i < ents.size(); i++) {                // files first
    const char* nm = (const char*)names.data() + ents[i].name;
    char path[520];
    if (ents[i].attrs & FSA_DIR) continue;
    if (!fif_mask_match(r->q->masks, nm)) continue;
    join_path(dir, nm, r->sep, path, sizeof(path));
    if (es_len(path) >= 260) { r->sink->on_skip(path, E_RANGE); continue; }
    r->seen++;
    rc = search_file(r, path, ents[i].size_lo, ents[i].size_hi);
    if (rc != E_OK) return rc;
  }
  if (!r->q->subfolders) return E_OK;
  i = ents.size();
  while (i-- > 0) {                                  // push in reverse so the walk visits them in order
    const char* nm = (const char*)names.data() + ents[i].name;
    char path[520];
    char* dup;
    if (!(ents[i].attrs & FSA_DIR)) continue;
    if (ents[i].attrs & (FSA_HIDDEN | FSA_SYSTEM)) continue;
    if (fif_dir_excluded(r->q->masks, nm)) continue;
    join_path(dir, nm, r->sep, path, sizeof(path));
    if (es_len(path) >= 260) { r->sink->on_skip(path, E_RANGE); continue; }
    dup = (char*)e_alloc(es_len(path) + 1);
    if (!dup) return E_NOMEM;
    es_copy(dup, es_len(path) + 1, path);
    if (stack->push(dup) != E_OK) { e_free(dup); return E_NOMEM; }
  }
  return E_OK;
}

int fif_run(FileSys* fs, const FifQuery& q, FifSink* sink, volatile i32* cancel)
{
  FifRun r;
  Vec<char*> stack;
  FsStat st;
  int rc;
  u32 i;
  if (!sink) return E_RANGE;
  memset(&r, 0, sizeof(r));
  r.fs = fs; r.q = &q; r.sink = sink; r.cancel = cancel;
  r.max_hits = q.max_hits ? q.max_hits : FIF_MAX_HITS_DEFAULT;
  r.sep = (strchr(q.folder, '/') && !strchr(q.folder, '\\')) ? '/' : '\\';
  if (!fs) rc = E_RANGE;
  else if (q.pattern_len == 0 || q.pattern_len > RX_MAX_PATTERN) rc = q.pattern_len ? E_TOOBIG : E_SYNTAX;
  else {
    rc = E_OK;
    run_searcher(&r, enc_utf8(0), &rc);          // validates the pattern once
    if (rc == E_OK) {
      rc = fs->stat(q.folder, &st);
      if (rc == E_OK && (!st.exists || !(st.attrs & FSA_DIR))) rc = E_NOTFOUND;
    }
  }
  if (rc == E_OK) {
    char* dup = (char*)e_alloc(es_len(q.folder) + 1);
    if (!dup) rc = E_NOMEM;
    else {
      es_copy(dup, es_len(q.folder) + 1, q.folder);
      if (stack.push(dup) != E_OK) { e_free(dup); rc = E_NOMEM; }
    }
  }
  while (rc == E_OK && !stack.empty()) {
    char* dir = stack.back();
    stack.pop();
    if (cancelled(&r)) rc = E_CANCEL;
    else rc = walk_dir(&r, dir, &stack);
    e_free(dir);
  }
  for (i = 0; i < stack.size(); i++) e_free(stack[i]);
  for (i = 0; i < r.ncache; i++) delete r.cache[i].s;
  if (r.buf) e_vfree(r.buf, r.cap);
  if (rc == E_OK && cancelled(&r)) rc = E_CANCEL;
  sink->on_done(rc, r.searched, r.fwh, r.hits);
  return rc;
}
