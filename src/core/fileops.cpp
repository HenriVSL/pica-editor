// fileops.cpp - load, FileSource, index, safe save, A/B files, backups (DESIGN 6.20, 13.2-13.7, 14.1).
// Owner after Wave 0: T05.
//
// Safety rules followed everywhere in this file:
// - The original file is never truncated or written before the new content is complete on disk (temp + rename).
// - Every failure path leaves the target with either its old bytes or the new bytes; files we could not delete are
//   reported by name in SaveResult (temp, old_copy, stray).
// - A rename/remove that reports failure is double-checked with stat (SMB redirectors can report errors for
//   operations that did happen), so we never act on a wrong picture of the disk.
#include "fileops.h"
#include <string.h>

const char BACKUP_TRAILER[] = "\r\n@END PICABAK1\r\n";

enum {
  FO_CHUNK = 65536,                 // I/O chunk and FileSource block size
  FO_TRAILER_LEN = 17,              // strlen(BACKUP_TRAILER)
  FO_HDR_MAX = 8192,                // a backup header is never longer than this
  FO_AB_MAX = 16 * 1024 * 1024,     // A/B files (session) larger than this are not read
  FO_DEFAULT_THRESHOLD = 16 * 1024 * 1024,
  FO_DEFAULT_CACHE = 64,
  FO_MAX_CACHE = 4096,
  FO_PATH = 260
};

#if defined(_WIN32)
#define FO_SEP '\\'
#else
#define FO_SEP '/'
#endif

// ---------------------------------------------------------------------------------------------------------------
// small helpers

static int fo_is_sep(char c) { return c == '\\' || c == '/'; }

static int fo_cancelled(volatile i32* cancel) { return cancel && *cancel != 0; }

static int fo_progress(ProgressCb* cb, u32 done, u32 total) { return cb ? cb->progress(done, total) : 0; }

// Folder part of a path: "C:\a\b.txt" -> "C:\a", "C:\b.txt" -> "C:\", "b.txt" -> ".", "/x" -> "/".
static int fo_dir_of(const char* path, char* out, u32 cap)
{
  u32 n, i, last = 0xFFFFFFFFu;
  if (!path || !path[0]) return E_NOTFOUND;
  n = es_len(path);
  if (n + 1 > cap) return E_RANGE;
  for (i = 0; i < n; i++) if (fo_is_sep(path[i])) last = i;
  if (last == 0xFFFFFFFFu) {
    if (n >= 2 && path[1] == ':') { out[0] = path[0]; out[1] = ':'; out[2] = 0; }
    else { out[0] = '.'; out[1] = 0; }
    return E_OK;
  }
  if (last == 0 || (last == 2 && path[1] == ':')) {   // root: keep the separator
    memcpy(out, path, last + 1);
    out[last + 1] = 0;
    return E_OK;
  }
  memcpy(out, path, last);
  out[last] = 0;
  return E_OK;
}

static int fo_join(const char* dir, const char* name, char* out, u32 cap)
{
  u32 n = es_len(dir);
  int sep = n > 0 && !fo_is_sep(dir[n - 1]) && dir[n - 1] != ':';
  if (n + (sep ? 1u : 0u) + es_len(name) + 1 > cap) return E_RANGE;
  es_copy(out, cap, dir);
  if (sep) { out[n] = FO_SEP; out[n + 1] = 0; }
  es_cat(out, cap, name);
  return E_OK;
}

// 1 = exists, 0 = missing, < 0 = could not tell
static int fo_exists(FileSys* fs, const char* path)
{
  FsStat st;
  int rc = fs->stat(path, &st);
  if (rc != E_OK) return rc;
  return st.exists ? 1 : 0;
}

// Removes a file; E_OK also when it is already gone (a remove that "failed" but did happen counts as done).
static int fo_remove(FileSys* fs, const char* path)
{
  int rc = fs->remove(path);
  if (rc == E_OK) return E_OK;
  if (fo_exists(fs, path) == 0) return E_OK;
  return rc;
}

// Moves 'from' to 'to'. A reported failure is double-checked: if 'from' is gone and 'to' exists, it happened.
static int fo_move(FileSys* fs, const char* from, const char* to)
{
  int rc = fs->move(from, to);
  if (rc == E_OK) return E_OK;
  if (fo_exists(fs, from) == 0 && fo_exists(fs, to) == 1) return E_OK;
  return rc;
}

// Writes all bytes of 'data' to f (sequentially) through a 64 KB staging buffer (pieces can be tiny).
// Progress reports bytes of data; base/total let callers include other bytes in the totals.
static int fo_write_data(FileSys* fs, FsFile f, ByteSource* data, ProgressCb* cb, volatile i32* cancel)
{
  epos size = data->size(), off = 0;
  u32 fill = 0;
  int rc = E_OK;
  u8* stage = 0;
  while (off < size && rc == E_OK) {
    epos avail = 0;
    const u8* p;
    if (fo_cancelled(cancel)) { rc = E_CANCEL; break; }
    p = data->get(off, &avail);
    if (!p || avail == 0) { rc = data->err() != E_OK ? data->err() : E_IO; break; }
    if (avail > size - off) avail = size - off;
    while (avail > 0) {
      u32 take;
      if (fill == 0 && avail >= FO_CHUNK) {             // big span: write straight from the source
        rc = fs->write(f, p, FO_CHUNK);
        if (rc != E_OK) break;
        p += FO_CHUNK; off += FO_CHUNK; avail -= FO_CHUNK;
        if (fo_progress(cb, off, size)) { rc = E_CANCEL; break; }
        if (fo_cancelled(cancel)) { rc = E_CANCEL; break; }
        continue;
      }
      if (!stage) {
        stage = (u8*)e_alloc(FO_CHUNK);
        if (!stage) { rc = E_NOMEM; break; }
      }
      take = FO_CHUNK - fill;
      if (take > avail) take = avail;
      memcpy(stage + fill, p, take);
      fill += take; p += take; off += take; avail -= take;
      if (fill == FO_CHUNK) {
        rc = fs->write(f, stage, fill);
        fill = 0;
        if (rc != E_OK) break;
        if (fo_progress(cb, off, size)) { rc = E_CANCEL; break; }
      }
    }
  }
  if (rc == E_OK && fill > 0) rc = fs->write(f, stage, fill);
  if (rc == E_OK && size > 0 && fo_progress(cb, size, size)) rc = E_CANCEL;
  if (stage) e_free(stage);
  return rc;
}

// Reads n bytes at off into buf; E_IO when the file is shorter.
static int fo_read_exact(FileSys* fs, FsFile f, u32 off, void* buf, u32 n)
{
  u32 got = 0;
  int rc = fs->read_at(f, off, 0, buf, n, &got);
  if (rc != E_OK) return rc;
  return got == n ? E_OK : E_IO;
}

// ---------------------------------------------------------------------------------------------------------------
// FileSource

FileSource::FileSource()
  : fs_(0), f_(0), mode_(0), size_(0), err_(E_OK), slots_(0), nslots_(0), stamp_(0), last_(0)
{
  path_[0] = 0;
}

FileSource::~FileSource()
{
  close();
  free_cache_();
}

void FileSource::free_cache_()
{
  u32 i;
  if (slots_) {
    for (i = 0; i < nslots_; i++) if (slots_[i].p) e_vfree(slots_[i].p, FO_CHUNK);
    e_free(slots_);
  }
  slots_ = 0; nslots_ = 0; stamp_ = 0; last_ = 0;
}

int FileSource::open(FileSys* fs, const char* path, u32 mode, u32 cache_blocks)
{
  FsFile f = 0;
  u32 lo = 0, hi = 0, n, i;
  int rc;
  close();
  free_cache_();
  fs_ = 0; size_ = 0; err_ = E_OK; path_[0] = 0;
  if (!fs || !path) return E_STATE;
  if (es_len(path) >= sizeof(path_)) return E_RANGE;
  mode = (mode & ~(u32)(FS_WRITE | FS_NEW)) | FS_READ;
  rc = fs->open(path, mode, &f);
  if (rc != E_OK) return rc;
  rc = fs->size(f, &lo, &hi);
  if (rc == E_OK && (hi != 0 || lo > EPOS_MAX)) rc = E_TOOBIG;
  n = cache_blocks ? cache_blocks : FO_DEFAULT_CACHE;
  if (n > FO_MAX_CACHE) n = FO_MAX_CACHE;
  if (rc == E_OK) {
    slots_ = (Slot*)e_alloc(n * (u32)sizeof(Slot));
    if (!slots_) rc = E_NOMEM;
  }
  if (rc != E_OK) { fs->close(f); return rc; }
  for (i = 0; i < n; i++) { slots_[i].p = 0; slots_[i].blk = 0xFFFFFFFFu; slots_[i].len = 0; slots_[i].stamp = 0; }
  nslots_ = n;
  fs_ = fs; f_ = f; mode_ = mode; size_ = lo;
  es_copy(path_, sizeof(path_), path);
  return E_OK;
}

int FileSource::reopen(const char* path)
{
  char p[FO_PATH];
  FsFile f = 0;
  u32 lo = 0, hi = 0, i, best = 0xFFFFFFFFu;
  int rc;
  if (!fs_) return E_STATE;
  if (!path || es_len(path) >= sizeof(p)) return E_RANGE;
  es_copy(p, sizeof(p), path);                       // path may be path_ itself
  close();
  rc = fs_->open(p, mode_, &f);
  if (rc != E_OK) return rc;
  rc = fs_->size(f, &lo, &hi);
  if (rc == E_OK && (hi != 0 || lo != size_)) rc = E_STATE;   // not the same bytes
  // Same bytes? Compare the most recently used cached block with the file (catches a wrong file of equal size).
  for (i = 0; i < nslots_; i++)
    if (slots_[i].blk != 0xFFFFFFFFu && (best == 0xFFFFFFFFu || slots_[i].stamp > slots_[best].stamp)) best = i;
  if (rc == E_OK && best != 0xFFFFFFFFu) {
    u8* tmp = (u8*)e_valloc(slots_[best].len);
    if (!tmp) rc = E_NOMEM;
    else {
      rc = fo_read_exact(fs_, f, slots_[best].blk << 16, tmp, slots_[best].len);
      if (rc == E_OK && memcmp(tmp, slots_[best].p, slots_[best].len) != 0) rc = E_STATE;
      e_vfree(tmp, slots_[best].len);
    }
  }
  if (rc != E_OK) { fs_->close(f); return rc; }
  f_ = f;
  es_copy(path_, sizeof(path_), p);
  err_ = E_OK;
  return E_OK;
}

void FileSource::close()
{
  if (fs_ && f_) fs_->close(f_);
  f_ = 0;                                            // the cache stays: same bytes, still served while closed
}

epos FileSource::size() const { return size_; }

const u8* FileSource::get(epos off, epos* avail)
{
  u32 blk, i, base, want, got = 0;
  Slot* s = 0;
  int rc;
  if (avail) *avail = 0;
  if (off >= size_ || !slots_) return 0;
  blk = off >> 16;
  if (last_ < nslots_ && slots_[last_].blk == blk) s = &slots_[last_];
  else {
    for (i = 0; i < nslots_; i++) if (slots_[i].blk == blk) { s = &slots_[i]; last_ = i; break; }
  }
  if (!s) {
    u32 victim = 0;
    if (!f_) { err_ = E_STATE; return 0; }          // closed (during the rename sequence) and not cached
    for (i = 0; i < nslots_; i++) {
      if (slots_[i].blk == 0xFFFFFFFFu) { victim = i; break; }
      if (slots_[i].stamp < slots_[victim].stamp) victim = i;
    }
    s = &slots_[victim];
    if (!s->p) {
      s->p = (u8*)e_valloc(FO_CHUNK);
      if (!s->p) { err_ = E_NOMEM; return 0; }
    }
    base = blk << 16;
    want = size_ - base;
    if (want > FO_CHUNK) want = FO_CHUNK;
    s->blk = 0xFFFFFFFFu;
    rc = fs_->read_at(f_, base, 0, s->p, want, &got);
    if (rc != E_OK || got != want) { s->len = 0; err_ = rc != E_OK ? rc : E_IO; return 0; }
    s->blk = blk; s->len = want;
    last_ = victim;
  }
  s->stamp = ++stamp_;
  err_ = E_OK;
  base = blk << 16;
  if (avail) *avail = s->len - (off - base);
  return s->p + (off - base);
}

int FileSource::err() const { return err_; }
const char* FileSource::path() const { return path_; }

// ---------------------------------------------------------------------------------------------------------------
// file_load

static void fo_apply_forced_enc(const u8* sample, u32 n, const LoadOpts& o, DetectResult* d)
{
  EncSpec e = o.enc, eb = o.enc;
  u8 bom[4];
  u32 bl, kinds;
  eb.bom = 1;
  bl = enc_bom(eb, bom);
  d->bom_len = (bl > 0 && n >= bl && memcmp(sample, bom, bl) == 0) ? bl : 0;
  e.bom = (u8)(d->bom_len ? 1 : 0);
  d->enc = e;
  detect_eol(sample + d->bom_len, n - d->bom_len, e, &d->eol);
  d->eol_style = detect_dominant_eol(d->eol, o.det.default_eol);
  kinds = (d->eol.crlf > 0 ? 1u : 0u) + (d->eol.lf > 0 ? 1u : 0u) + (d->eol.cr > 0 ? 1u : 0u);
  d->eol_mixed = (u8)(kinds > 1 ? 1 : 0);
}

static int fo_load_memory(FileSys* fs, const char* path, ProgressCb* cb, LoadResult* r, u8** data, epos* len)
{
  FsFile f = 0;
  u32 lo = 0, hi = 0, done = 0, got;
  u8* buf;
  int rc;
  *data = 0; *len = 0;
  rc = fs->open(path, FS_READ | FS_SHARE_ALL, &f);   // share read+write: files other programs keep open still load
  if (rc != E_OK) return rc;
  rc = fs->size(f, &lo, &hi);
  if (rc == E_OK && (hi != 0 || lo > EPOS_MAX)) rc = E_TOOBIG;
  if (rc != E_OK) { fs->close(f); return rc; }
  buf = (u8*)e_valloc(lo);
  if (!buf) { fs->close(f); return E_NOMEM; }
  while (done < lo) {
    u32 want = lo - done;
    if (want > FO_CHUNK) want = FO_CHUNK;
    got = 0;
    rc = fs->read_at(f, done, 0, buf + done, want, &got);
    if (rc != E_OK) break;
    done += got;
    if (got < want) break;                           // the file shrank while we read: keep what is there
    if (fo_progress(cb, done, lo)) { rc = E_CANCEL; break; }
  }
  fs->close(f);
  if (rc != E_OK) { e_vfree(buf, lo); return rc; }
  if (done != lo) {
    // Record the size we really have, so change detection (size differs from the disk) reloads it later.
    r->st.size_lo = done;
  }
  *data = buf;
  *len = done;
  return E_OK;
}

int file_load(FileSys* fs, const char* path, const LoadOpts& o, ProgressCb* cb, LoadResult* r)
{
  FsStat st;
  epos thr, len = 0;
  u8* data = 0;
  u8* sample = 0;
  u32 n = 0;
  int rc, whole;
  memset(r, 0, sizeof(*r));
  if (!fs || !path || !path[0]) return E_NOTFOUND;
  rc = fs->stat(path, &st);
  if (rc != E_OK) return rc;
  if (!st.exists) return E_NOTFOUND;
  if (st.attrs & FSA_DIR) return E_ACCESS;
  if (st.size_hi != 0 || st.size_lo > EPOS_MAX) return E_TOOBIG;
  r->st = st;
  r->ro_attr = (u8)((st.attrs & FSA_READONLY) ? 1 : 0);
  thr = o.ondemand_threshold ? o.ondemand_threshold : (epos)FO_DEFAULT_THRESHOLD;

  if (st.size_lo <= thr) {
    MemSource* ms;
    rc = fo_load_memory(fs, path, cb, r, &data, &len);
    if (rc != E_OK) return rc;
    ms = new MemSource();
    if (!ms) { e_vfree(data, len); return E_NOMEM; }
    ms->adopt(data, len, 1);
    r->src = ms;
    r->mode = LM_MEMORY;
    sample = data;
    n = len < FO_CHUNK ? len : FO_CHUNK;
    whole = len <= FO_CHUNK;
  } else {
    FileSource* fsrc = new FileSource();
    if (!fsrc) return E_NOMEM;
    rc = fsrc->open(fs, path, FS_READ | FS_DENY_WRITE, o.cache_blocks);
    if (rc == E_SHARING) {                            // someone has it open for writing: read-only, shared
      rc = fsrc->open(fs, path, FS_READ | FS_SHARE_ALL, o.cache_blocks);
      if (rc == E_OK) r->shared_ro = 1;
    }
    if (rc != E_OK) { delete fsrc; return rc; }
    r->src = fsrc;
    r->mode = LM_ONDEMAND;
    len = fsrc->size();
    if (len != st.size_lo) r->st.size_lo = len;
    n = len < FO_CHUNK ? len : FO_CHUNK;
    whole = len <= FO_CHUNK;
    sample = (u8*)e_alloc(n);
    if (!sample) rc = E_NOMEM;
    else rc = fsrc->read(0, sample, n);
    if (rc != E_OK) {
      if (sample) e_free(sample);
      delete r->src; r->src = 0;
      return rc;
    }
  }
  rc = detect(sample, n, whole, o.det, &r->det);
  if (rc >= 0 && o.force_enc) fo_apply_forced_enc(sample, n, o, &r->det);
  if (r->mode == LM_ONDEMAND) e_free(sample);
  if (rc < 0) { delete r->src; r->src = 0; return rc; }
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// file_index: break units per 64 KB block + EOL counts (detect_eol semantics: CRLF pairs, bare LFs, lone CRs, in
// code units; the same in LF and CR mode)

int file_index(FileSys* fs, const char* path, const BreakSpec& bs, IndexResult* r, ProgressCb* cb, volatile i32* cancel)
{
  FsFile f = 0;
  u32 lo = 0, hi = 0, nblocks, b, crlf = 0, lf = 0, cr = 0;
  int rc, pending = 0, le;
  u8* buf = 0;
  r->block_breaks.clear();
  r->eol.crlf = r->eol.lf = r->eol.cr = 0;
  r->size = 0;
  if (!(bs.unit == 1 && (bs.b0 == 0x0A || bs.b0 == 0x0D)) &&
      !(bs.unit == 2 && ((bs.b1 == 0 && (bs.b0 == 0x0A || bs.b0 == 0x0D)) ||
                         (bs.b0 == 0 && (bs.b1 == 0x0A || bs.b1 == 0x0D)))))
    return E_RANGE;
  le = bs.b0 != 0;
  rc = fs->open(path, FS_READ | FS_SHARE_ALL, &f);   // our own handle; compatible with a deny-write FileSource
  if (rc != E_OK) return rc;
  rc = fs->size(f, &lo, &hi);
  if (rc == E_OK && (hi != 0 || lo > EPOS_MAX)) rc = E_TOOBIG;
  nblocks = (lo >> 16) + ((lo & 0xFFFFu) ? 1u : 0u);
  if (rc == E_OK) rc = r->block_breaks.resize(nblocks);
  if (rc == E_OK) {
    buf = (u8*)e_valloc(FO_CHUNK);
    if (!buf) rc = E_NOMEM;
  }
  for (b = 0; b < nblocks && rc == E_OK; b++) {
    u32 base = b << 16, want = lo - base, nb = 0, i = 0;
    if (want > FO_CHUNK) want = FO_CHUNK;
    if (fo_cancelled(cancel)) { rc = E_CANCEL; break; }
    rc = fo_read_exact(fs, f, base, buf, want);
    if (rc != E_OK) break;
    if (bs.unit == 1) {
      const u8* p = buf;
      u8 bk = bs.b0;
      if (pending) {                                  // a CR ended the previous block
        pending = 0;
        if (want > 0 && p[0] == 0x0A) { crlf++; if (bk == 0x0A) nb++; i = 1; }
        else cr++;
      }
      for (; i < want; i++) {
        u8 c = p[i];
        if (c > 0x0D) continue;
        if (c == 0x0A) { lf++; if (bk == 0x0A) nb++; }
        else if (c == 0x0D) {
          if (bk == 0x0D) nb++;
          if (i + 1 < want) {
            if (p[i + 1] == 0x0A) { crlf++; i++; if (bk == 0x0A) nb++; }
            else cr++;
          } else pending = 1;
        }
      }
    } else {
      u32 units = want / 2;                           // a final odd byte is not a unit
      const u8* p = buf;
      u16 bk = (u16)(le ? bs.b0 : bs.b1);
      if (pending) {
        u16 u0;
        pending = 0;
        u0 = units > 0 ? (u16)(le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1])) : 0;
        if (units > 0 && u0 == 0x0A) { crlf++; if (bk == 0x0A) nb++; i = 1; }
        else cr++;
      }
      for (; i < units; i++) {
        u16 u = (u16)(le ? (p[2 * i] | (p[2 * i + 1] << 8)) : ((p[2 * i] << 8) | p[2 * i + 1]));
        if (u > 0x0D) continue;
        if (u == 0x0A) { lf++; if (bk == 0x0A) nb++; }
        else if (u == 0x0D) {
          if (bk == 0x0D) nb++;
          if (i + 1 < units) {
            u16 nx = (u16)(le ? (p[2 * i + 2] | (p[2 * i + 3] << 8)) : ((p[2 * i + 2] << 8) | p[2 * i + 3]));
            if (nx == 0x0A) { crlf++; i++; if (bk == 0x0A) nb++; }
            else cr++;
          } else if (want == FO_CHUNK) pending = 1;   // more units may follow in the next block
          else cr++;
        }
      }
    }
    r->block_breaks[b] = nb;
    if (fo_progress(cb, base + want, lo)) { rc = E_CANCEL; break; }
  }
  if (rc == E_OK && pending) cr++;                  // CR at the very end
  if (buf) e_vfree(buf, FO_CHUNK);
  fs->close(f);
  if (rc != E_OK) { r->block_breaks.release(); return rc; }
  r->eol.crlf = crlf; r->eol.lf = lf; r->eol.cr = cr;
  r->size = lo;
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Safe save (13.3)

int file_write_temp(FileSys* fs, const char* path, ByteSource* data, ProgressCb* cb, volatile i32* cancel, SaveResult* r)
{
  char dir[FO_PATH];
  FsStat st;
  FsFile f = 0;
  u64 freeb = 0;
  epos size;
  int rc, crc;
  memset(r, 0, sizeof(*r));
  if (!fs || !path || !data) return E_STATE;
  size = data->size();
  // 1. folder exists, enough free space (the old file stays until the end, so the full new size is needed)
  r->failed_step = 1;
  rc = fo_dir_of(path, dir, sizeof(dir));
  if (rc != E_OK) return rc;
  rc = fs->stat(dir, &st);
  if (rc != E_OK) return rc;
  if (!st.exists || !(st.attrs & FSA_DIR)) return E_NOTFOUND;
  if (fs->free_space(dir, &freeb) == E_OK && freeb < (u64)size + FO_CHUNK) return E_DISKFULL;
  // 2. unique temp file in the target folder (same volume), all bytes, flush, close
  r->failed_step = 2;
  rc = fs->make_temp(dir, "~PC", r->temp, sizeof(r->temp));
  if (rc != E_OK) { r->temp[0] = 0; return rc; }
  rc = fs->open(r->temp, FS_WRITE, &f);
  if (rc == E_OK) {
    rc = fo_write_data(fs, f, data, cb, cancel);
    if (rc == E_OK) rc = fs->flush(f);
    crc = fs->close(f);
    if (rc == E_OK) rc = crc;
  }
  // 3. verify the size on disk
  if (rc == E_OK) {
    r->failed_step = 3;
    rc = fs->stat(r->temp, &st);
    if (rc == E_OK && (!st.exists || st.size_hi != 0 || st.size_lo != size)) rc = E_IO;
  }
  if (rc != E_OK) {
    if (fo_remove(fs, r->temp) == E_OK) r->temp[0] = 0;   // else: reported (kept, possibly partial)
    return rc;
  }
  r->st = st;                                        // the temp file's state; file_replace checks it again
  r->failed_step = 0;
  return E_OK;
}

// After the target was moved away: puts the old bytes back at 'path'. Returns E_OK if the target holds the old
// bytes again (old_copy then emptied if the file is gone), else an error (target missing; old_copy kept).
static int fo_rollback(FileSys* fs, const char* path, SaveResult* r)
{
  int rc = fo_move(fs, r->old_copy, path);
  if (rc == E_OK) { r->old_copy[0] = 0; return E_OK; }
  rc = fs->copy(r->old_copy, path);                  // keeps old_copy
  if (rc == E_OK) {
    if (fo_remove(fs, r->old_copy) == E_OK) r->old_copy[0] = 0;
    return E_OK;
  }
  if (fo_exists(fs, r->old_copy) == 1) fo_remove(fs, path);   // never leave a partial copy at the target
  return rc;
}

int file_replace(FileSys* fs, const char* path, const SaveOpts& o, SaveHooks* hooks, SaveResult* r)
{
  char dir[FO_PATH];
  FsStat old, tst;
  int rc, rc2, ondemand = hooks != 0;
  const char* orig_at = path;                        // where the original bytes are (for after_replace)
  r->in_place = 0;
  r->failed_step = 4;
  r->old_copy[0] = 0;
  r->stray[0] = 0;
  if (!fs || !path || !r->temp[0]) return E_STATE;
  rc = fo_dir_of(path, dir, sizeof(dir));
  if (rc != E_OK) return rc;
  // The temp file must still be there, complete (file_write_temp recorded its size in r->st).
  rc = fs->stat(r->temp, &tst);
  if (rc != E_OK) return rc;
  if (!tst.exists || tst.size_hi != r->st.size_hi || tst.size_lo != r->st.size_lo) { r->temp[0] = 0; return E_IO; }
  memset(&r->st, 0, sizeof(r->st));
  // 4. target state
  rc = fs->stat(path, &old);
  if (rc != E_OK) return rc;                         // temp kept (named in r->temp)
  if (old.exists && (old.attrs & FSA_DIR)) { rc = E_ACCESS; goto fail_drop_temp; }
  if (!old.exists) {                                 // new file: one rename
    r->failed_step = 7;
    rc = fo_move(fs, r->temp, path);
    if (rc != E_OK) return rc;                       // temp kept; target absent
    r->temp[0] = 0;
    goto done;
  }
  if (old.attrs & FSA_READONLY) { rc = E_ACCESS; goto fail_drop_temp; }
  // 5. on-demand: close our handle on the target (MoveFile fails on open files)
  if (hooks) hooks->before_replace();
  // 6. move the target aside under a unique name
  r->failed_step = 6;
  rc = fs->make_temp(dir, "~PO", r->old_copy, sizeof(r->old_copy));
  if (rc != E_OK) { r->old_copy[0] = 0; goto fail_unhook_drop; }
  rc = fo_remove(fs, r->old_copy);
  if (rc != E_OK) { es_copy(r->stray, sizeof(r->stray), r->old_copy); r->old_copy[0] = 0; goto fail_unhook_drop; }
  rc = fo_move(fs, path, r->old_copy);
  if (rc != E_OK) {
    int oex = fo_exists(fs, r->old_copy);
    if (fo_exists(fs, path) != 1) {
      // Can't confirm the target is still in place (stat failing too): touch nothing more. If old_copy exists the
      // move did happen and it holds the original (stays named); if we can't tell, it is only mentioned (stray).
      // The temp file stays as well.
      if (oex != 1) {
        if (oex < 0) es_copy(r->stray, sizeof(r->stray), r->old_copy);
        r->old_copy[0] = 0;
      }
      if (hooks) hooks->after_replace(r->old_copy[0] ? r->old_copy : path);
      return rc;
    }
    if (oex == 1 && fo_remove(fs, r->old_copy) != E_OK) es_copy(r->stray, sizeof(r->stray), r->old_copy);
    r->old_copy[0] = 0;
    if (rc != E_ACCESS && rc != E_SHARING) goto fail_unhook_drop;
    if (ondemand) {                                  // in use by another program: report, keep the temp file
      hooks->after_replace(path);
      return rc;
    }
    // In-place fallback (in-memory documents): copy the old bytes aside, then overwrite the target in place.
    r->in_place = 1;
    rc = fs->make_temp(dir, "~PO", r->old_copy, sizeof(r->old_copy));
    if (rc != E_OK) { r->old_copy[0] = 0; goto fail_drop_temp; }
    rc = fs->copy(path, r->old_copy);
    if (rc != E_OK) {
      if (fo_remove(fs, r->old_copy) == E_OK) r->old_copy[0] = 0;
      goto fail_drop_temp;
    }
    rc = fs->copy(r->temp, path);
    if (rc != E_OK) {
      rc2 = fs->copy(r->old_copy, path);             // the target may be damaged: put the old bytes back
      if (rc2 == E_OK) {
        if (fo_remove(fs, r->old_copy) == E_OK) r->old_copy[0] = 0;
        goto fail_drop_temp;
      }
      return rc;                                     // target damaged: old_copy and temp both kept and named
    }
    if (fo_remove(fs, r->temp) != E_OK) es_copy(r->stray, sizeof(r->stray), r->temp);
    r->temp[0] = 0;
    goto finish;
  }
  orig_at = r->old_copy;
  // 7. the new file takes the name
  r->failed_step = 7;
  rc = fo_move(fs, r->temp, path);
  if (rc != E_OK) {
    if (fo_rollback(fs, path, r) == E_OK) orig_at = path;
    if (hooks) hooks->after_replace(orig_at);
    return rc;                                       // temp kept and named
  }
  r->temp[0] = 0;
  // 8. attributes and creation time of the old file
  if (old.ctime_lo || old.ctime_hi) fs->set_ctime(path, old.ctime_lo, old.ctime_hi);
finish:
  if (old.attrs & (FSA_HIDDEN | FSA_SYSTEM)) fs->set_attrs(path, (old.attrs & (FSA_HIDDEN | FSA_SYSTEM)) | FSA_ARCHIVE);
  // 9. the old copy
  if (ondemand) {
    hooks->after_replace(r->old_copy);               // the source reads the old bytes from there; the tab owns it now
  } else if (r->old_copy[0]) {
    if (o.keep_bak) {
      char bak[FO_PATH + 4];
      if (es_len(path) + 5 <= sizeof(bak)) {
        es_copy(bak, sizeof(bak), path);
        es_cat(bak, sizeof(bak), ".bak");
        if (fo_remove(fs, bak) == E_OK && fo_move(fs, r->old_copy, bak) == E_OK) r->old_copy[0] = 0;
      }
    }
    if (r->old_copy[0] && fo_remove(fs, r->old_copy) == E_OK) r->old_copy[0] = 0;
  }
done:
  if (fs->stat(path, &r->st) != E_OK) memset(&r->st, 0, sizeof(r->st));
  r->failed_step = 0;
  return E_OK;

fail_unhook_drop:
  if (hooks) hooks->after_replace(path);
fail_drop_temp:
  if (fo_remove(fs, r->temp) == E_OK) r->temp[0] = 0;
  return rc;
}

int file_save(FileSys* fs, const char* path, ByteSource* data, const SaveOpts& o, SaveHooks* hooks, ProgressCb* cb, SaveResult* r)
{
  int rc = file_write_temp(fs, path, data, cb, 0, r);
  if (rc != E_OK) return rc;
  return file_replace(fs, path, o, hooks, r);
}

// ---------------------------------------------------------------------------------------------------------------
// Whole-file helpers and A/B files (13.6)

int file_read_all(FileSys* fs, const char* path, EBytes* out, u32 max)
{
  FsFile f = 0;
  u32 lo = 0, hi = 0, done = 0, got;
  int rc;
  out->clear();
  rc = fs->open(path, FS_READ | FS_SHARE_ALL, &f);
  if (rc != E_OK) return rc;
  rc = fs->size(f, &lo, &hi);
  if (rc == E_OK && (hi != 0 || lo > max)) rc = E_TOOBIG;
  if (rc == E_OK) rc = out->resize(lo);
  while (rc == E_OK && done < lo) {
    u32 want = lo - done;
    if (want > FO_CHUNK) want = FO_CHUNK;
    got = 0;
    rc = fs->read_at(f, done, 0, out->data() + done, want, &got);
    if (rc != E_OK) break;
    done += got;
    if (got < want) break;                           // shrank while reading
  }
  fs->close(f);
  if (rc != E_OK) { out->release(); return rc; }
  out->resize(done);
  return E_OK;
}

int file_write_direct(FileSys* fs, const char* path, const u8* p, u32 n)
{
  FsFile f = 0;
  u32 done = 0;
  int rc, crc;
  rc = fs->open(path, FS_WRITE, &f);
  if (rc != E_OK) return rc;
  while (rc == E_OK && done < n) {
    u32 k = n - done;
    if (k > FO_CHUNK) k = FO_CHUNK;
    rc = fs->write(f, p + done, k);
    done += k;
  }
  if (rc == E_OK) rc = fs->flush(f);
  crc = fs->close(f);
  return rc != E_OK ? rc : crc;
}

static int fo_ab_valid(AbValidator v, const EBytes& b, u32* seq)
{
  static const u8 empty[1] = { 0 };
  *seq = 0;
  return v(b.size() ? b.data() : empty, b.size(), seq) >= 0;
}

int ab_read(FileSys* fs, const char* p1, const char* p2, AbValidator v, EBytes* out, int* which)
{
  EBytes b1, b2;
  u32 s1 = 0, s2 = 0;
  int r1, r2, ok1, ok2, pick;
  if (which) *which = 0;
  out->clear();
  r1 = file_read_all(fs, p1, &b1, FO_AB_MAX);
  r2 = file_read_all(fs, p2, &b2, FO_AB_MAX);
  ok1 = r1 == E_OK && fo_ab_valid(v, b1, &s1);
  ok2 = r2 == E_OK && fo_ab_valid(v, b2, &s2);
  if (!ok1 && !ok2) {
    if (r1 == E_NOMEM || r2 == E_NOMEM) return E_NOMEM;
    if (r1 == E_NOTFOUND && r2 == E_NOTFOUND) return E_NOTFOUND;
    return E_FORMAT;
  }
  pick = (ok1 && (!ok2 || s1 >= s2)) ? 1 : 2;
  out->swap(pick == 1 ? b1 : b2);
  if (which) *which = pick;
  return E_OK;
}

int ab_write(FileSys* fs, const char* p1, const char* p2, int last_which, const u8* p, u32 n, int* which)
{
  const char* target = last_which == 1 ? p2 : p1;   // never overwrite the newest valid copy
  int w = last_which == 1 ? 2 : 1, rc;
  FsStat st;
  if (which) *which = last_which;
  rc = file_write_direct(fs, target, p, n);
  if (rc == E_OK) {
    rc = fs->stat(target, &st);
    if (rc == E_OK && (!st.exists || st.size_hi != 0 || st.size_lo != n)) rc = E_IO;
  }
  if (rc != E_OK) return rc;
  if (which) *which = w;
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Backups (13.5)

static const char HEX[] = "0123456789ABCDEF";

void backup_file_name(u32 tabid, u32 gen, char out[13])
{
  u32 i;
  out[0] = 'T';
  for (i = 0; i < 5; i++) out[1 + i] = HEX[(tabid >> (16 - 4 * i)) & 15];
  out[6] = HEX[(gen >> 4) & 15];
  out[7] = HEX[gen & 15];
  out[8] = '.'; out[9] = 'B'; out[10] = 'A'; out[11] = 'K'; out[12] = 0;
}

static int fo_hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// "T" + 5 hex + 2 hex + ".BAK" (case-insensitive) -> 1 and the numbers
static int fo_parse_backup_name(const char* name, u32* tabid, u32* gen)
{
  u32 i, v = 0;
  if (es_len(name) != 12 || (name[0] != 'T' && name[0] != 't') || es_icmp(name + 8, ".BAK") != 0) return 0;
  for (i = 1; i < 8; i++) {
    int d = fo_hexval(name[i]);
    if (d < 0) return 0;
    v = (v << 4) | (u32)d;
  }
  *tabid = v >> 8;
  *gen = v & 0xFF;
  return 1;
}

static int fo_has_ctl(const char* s, u32 cap)
{
  u32 i;
  for (i = 0; i < cap && s[i]; i++) if ((u8)s[i] < 0x20) return 1;
  return i == cap;                                    // not terminated
}

static void fo_hex64(u32 hi, u32 lo, char* out)
{
  u32 i;
  for (i = 0; i < 8; i++) out[i] = HEX[(hi >> (28 - 4 * i)) & 15];
  for (i = 0; i < 8; i++) out[8 + i] = HEX[(lo >> (28 - 4 * i)) & 15];
  out[16] = 0;
}

static int fo_parse_hex64(const char* s, u32* hi, u32* lo)
{
  u32 i, h = 0, l = 0;
  if (es_len(s) != 16) return E_FORMAT;
  for (i = 0; i < 16; i++) {
    int d = fo_hexval(s[i]);
    if (d < 0) return E_FORMAT;
    if (i < 8) h = (h << 4) | (u32)d; else l = (l << 4) | (u32)d;
  }
  *hi = h; *lo = l;
  return E_OK;
}

int backup_header_write(const BackupMeta& m, epos data_len, EStr* out)
{
  char enc[32], t1[17], t2[17];
  const char* eol;
  int rc = E_OK;
  out->clear();
  if (fo_has_ctl(m.path, sizeof(m.path)) || fo_has_ctl(m.lang, sizeof(m.lang))) return E_FORMAT;
  enc_token(m.enc, enc, sizeof(enc));
  eol = m.eol == EOL_LF ? "lf" : (m.eol == EOL_CR ? "cr" : "crlf");
  fo_hex64(m.disk_mtime_hi, m.disk_mtime_lo, t1);
  fo_hex64(m.written_hi, m.written_lo, t2);
  if (rc == E_OK) rc = out->appendf("PICABAK1\r\ntabid=%u\r\ngen=%u\r\nseq=%u\r\npath=", m.tabid, m.gen, m.seq);
  if (rc == E_OK) rc = out->append(m.path);
  if (rc == E_OK) rc = out->appendf("\r\nuntitled=%u\r\nuntitled_no=%u\r\nenc=%s\r\neol=%s\r\nlang=",
                                    (u32)m.untitled, m.untitled_no, enc, eol);
  if (rc == E_OK) rc = out->append(m.lang);
  if (rc == E_OK) rc = out->appendf("\r\ncaret=%u\r\nanchor=%u\r\ndisk_size=%u\r\ndisk_mtime=%s\r\nwritten=%s\r\n@DATA %u\r\n",
                                    m.caret, m.anchor, m.disk_size, t1, t2, data_len);
  if (rc != E_OK) { out->clear(); return E_NOMEM; }
  return E_OK;
}

static int fo_parse_dec(const char* s, u32* out)
{
  u32 v = 0;
  if (!*s) return E_FORMAT;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return E_FORMAT;
    if (v > 429496729u || (v == 429496729u && *s > '5')) return E_FORMAT;
    v = v * 10 + (u32)(*s - '0');
  }
  *out = v;
  return E_OK;
}

int backup_header_parse(const u8* p, u32 n, BackupMeta* m, u32* header_len, epos* data_len)
{
  char line[320];
  u32 pos, lim = n < FO_HDR_MAX ? n : FO_HDR_MAX, have = 0;
  memset(m, 0, sizeof(*m));
  m->enc = enc_ansi();
  m->eol = EOL_CRLF;
  if (header_len) *header_len = 0;
  if (data_len) *data_len = 0;
  if (lim < 10 || memcmp(p, "PICABAK1\r\n", 10) != 0) return E_FORMAT;
  pos = 10;
  for (;;) {
    u32 e = pos, len;
    char* val;
    u32 v = 0;
    while (e + 1 < lim && !(p[e] == '\r' && p[e + 1] == '\n')) e++;
    if (e + 1 >= lim) return E_FORMAT;                // no complete line: truncated or not ours
    len = e - pos;
    if (len >= sizeof(line)) return E_FORMAT;
    memcpy(line, p + pos, len);
    line[len] = 0;
    pos = e + 2;
    if (memchr(line, 0, len)) return E_FORMAT;
    if (len > 6 && memcmp(line, "@DATA ", 6) == 0) {
      if (fo_parse_dec(line + 6, &v) != E_OK || v > EPOS_MAX) return E_FORMAT;
      if ((have & 3) != 3) return E_FORMAT;           // tabid and gen are required
      if (header_len) *header_len = pos;
      if (data_len) *data_len = v;
      return E_OK;
    }
    val = strchr(line, '=');
    if (!val) return E_FORMAT;
    *val++ = 0;
    if (strcmp(line, "path") == 0) {
      if (es_len(val) >= sizeof(m->path)) return E_FORMAT;
      es_copy(m->path, sizeof(m->path), val);
    } else if (strcmp(line, "lang") == 0) {
      es_copy(m->lang, sizeof(m->lang), val);
    } else if (strcmp(line, "enc") == 0) {
      EncSpec es;
      if (enc_parse(val, &es) == E_OK) m->enc = es;    // unknown token: keep ANSI (the bytes are raw anyway)
    } else if (strcmp(line, "eol") == 0) {
      m->eol = (u8)(strcmp(val, "lf") == 0 ? EOL_LF : (strcmp(val, "cr") == 0 ? EOL_CR : EOL_CRLF));
    } else if (strcmp(line, "disk_mtime") == 0) {
      if (fo_parse_hex64(val, &m->disk_mtime_hi, &m->disk_mtime_lo) != E_OK) return E_FORMAT;
    } else if (strcmp(line, "written") == 0) {
      if (fo_parse_hex64(val, &m->written_hi, &m->written_lo) != E_OK) return E_FORMAT;
    } else if (strcmp(line, "tabid") == 0 || strcmp(line, "gen") == 0 || strcmp(line, "seq") == 0 ||
               strcmp(line, "untitled") == 0 || strcmp(line, "untitled_no") == 0 || strcmp(line, "caret") == 0 ||
               strcmp(line, "anchor") == 0 || strcmp(line, "disk_size") == 0) {
      if (fo_parse_dec(val, &v) != E_OK) return E_FORMAT;
      if (strcmp(line, "tabid") == 0) { m->tabid = v; have |= 1; }
      else if (strcmp(line, "gen") == 0) { m->gen = v; have |= 2; }
      else if (strcmp(line, "seq") == 0) m->seq = v;
      else if (strcmp(line, "untitled") == 0) m->untitled = (u8)(v ? 1 : 0);
      else if (strcmp(line, "untitled_no") == 0) m->untitled_no = v;
      else if (strcmp(line, "caret") == 0) m->caret = v;
      else if (strcmp(line, "anchor") == 0) m->anchor = v;
      else m->disk_size = v;
    }
    // other keys: ignored (written by a newer version)
  }
}

// Checks a backup file completely except for reading the data: header, total size, trailer, name = header ids.
// Returns E_OK with the meta and lengths, or E_FORMAT / an I/O error.
static int fo_backup_check(FileSys* fs, FsFile f, u32 name_tabid, u32 name_gen, BackupMeta* m, u32* hlen, epos* dlen)
{
  u8* hdr;
  u8 tr[FO_TRAILER_LEN];
  u32 lo = 0, hi = 0, n, got = 0;
  int rc;
  rc = fs->size(f, &lo, &hi);
  if (rc != E_OK) return rc;
  if (hi != 0) return E_FORMAT;
  n = lo < FO_HDR_MAX ? lo : FO_HDR_MAX;
  hdr = (u8*)e_alloc(n);
  if (!hdr) return E_NOMEM;
  rc = fs->read_at(f, 0, 0, hdr, n, &got);
  if (rc == E_OK && got != n) rc = E_FORMAT;
  if (rc == E_OK) rc = backup_header_parse(hdr, n, m, hlen, dlen);
  e_free(hdr);
  if (rc != E_OK) return rc;
  if (m->tabid != name_tabid || m->gen != name_gen) return E_FORMAT;
  if (*dlen > lo || *hlen > lo - *dlen || lo - *dlen - *hlen != FO_TRAILER_LEN) return E_FORMAT;
  rc = fs->read_at(f, lo - FO_TRAILER_LEN, 0, tr, FO_TRAILER_LEN, &got);
  if (rc != E_OK) return rc;
  if (got != FO_TRAILER_LEN || memcmp(tr, BACKUP_TRAILER, FO_TRAILER_LEN) != 0) return E_FORMAT;
  return E_OK;
}

static int fo_backup_check_path(FileSys* fs, const char* full, u32 tabid, u32 gen, BackupMeta* m)
{
  FsFile f = 0;
  u32 hlen = 0;
  epos dlen = 0;
  int rc = fs->open(full, FS_READ | FS_SHARE_ALL, &f);
  if (rc != E_OK) return rc;
  rc = fo_backup_check(fs, f, tabid, gen, m, &hlen, &dlen);
  fs->close(f);
  return rc;
}

struct FoName { char s[16]; u32 tabid; u32 gen; };

// Backup files in dir (all tabs, or only 'tabid' when one_tab). A missing folder is an empty list.
static int fo_scan_backups(FileSys* fs, const char* dir, int one_tab, u32 tabid, Vec<FoName>* out)
{
  FsFind fd;
  int rc;
  out->clear();
  memset(&fd, 0, sizeof(fd));
  rc = fs->find_first(dir, &fd);
  if (rc == E_NOTFOUND) return E_OK;
  if (rc != E_OK) return rc;
  for (;;) {
    FoName nm;
    if (!(fd.attrs & FSA_DIR) && fo_parse_backup_name(fd.name, &nm.tabid, &nm.gen) &&
        (!one_tab || nm.tabid == tabid)) {
      es_copy(nm.s, sizeof(nm.s), fd.name);
      if (out->push(nm) != E_OK) { fs->find_close(&fd); return E_NOMEM; }
    }
    rc = fs->find_next(&fd);
    if (rc != E_OK) break;
  }
  fs->find_close(&fd);
  return rc == E_NOTFOUND ? E_OK : rc;
}

// Deletes every generation of tabid except keep_gen (0xFFFFFFFF = all). Returns the first error, E_OK otherwise.
static int fo_delete_gens(FileSys* fs, const char* dir, u32 tabid, u32 keep_gen)
{
  Vec<FoName> names;
  char full[FO_PATH];
  u32 i;
  int rc, first = E_OK;
  rc = fo_scan_backups(fs, dir, 1, tabid, &names);   // collect first: no deleting while the folder is enumerated
  if (rc != E_OK) return rc;
  for (i = 0; i < names.size(); i++) {
    if (names[i].gen == keep_gen) continue;
    if (fo_join(dir, names[i].s, full, sizeof(full)) != E_OK) continue;
    rc = fo_remove(fs, full);
    if (rc != E_OK && first == E_OK) first = rc;
  }
  return first;
}

int backup_write(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource* data, ProgressCb* cb, volatile i32* cancel)
{
  char name[13], full[FO_PATH];
  BackupMeta old;
  EStr hdr;
  FsStat st;
  FsFile f = 0;
  epos dlen;
  u32 expect;
  int rc, crc;
  if (!fs || !dir || !data) return E_STATE;
  if (m.tabid > 0xFFFFFu || m.gen > 0xFFu) return E_RANGE;
  dlen = data->size();
  backup_file_name(m.tabid, m.gen, name);
  rc = fo_join(dir, name, full, sizeof(full));
  if (rc != E_OK) return rc;
  rc = fs->stat(dir, &st);
  if (rc != E_OK) return rc;
  if (!st.exists) fs->mkdir(dir);                    // failure shows up at open
  rc = fs->stat(full, &st);
  if (rc != E_OK) return rc;
  if (st.exists) {
    // Never overwrite a valid backup (it may be the only one): the caller must use a new generation.
    if (fo_backup_check_path(fs, full, m.tabid, m.gen, &old) == E_OK) return E_EXISTS;
    rc = fo_remove(fs, full);                        // a torn leftover
    if (rc != E_OK) return rc;
  }
  rc = backup_header_write(m, dlen, &hdr);
  if (rc != E_OK) return rc;
  if (hdr.len() > FO_HDR_MAX || dlen > EPOS_MAX - hdr.len() - FO_TRAILER_LEN) return E_TOOBIG;
  expect = hdr.len() + dlen + FO_TRAILER_LEN;
  rc = fs->open(full, FS_WRITE | FS_NEW, &f);
  if (rc != E_OK) return rc;
  rc = fs->write(f, hdr.c_str(), hdr.len());
  if (rc == E_OK) rc = fo_write_data(fs, f, data, cb, cancel);
  if (rc == E_OK) rc = fs->write(f, BACKUP_TRAILER, FO_TRAILER_LEN);
  if (rc == E_OK) rc = fs->flush(f);
  crc = fs->close(f);
  if (rc == E_OK) rc = crc;
  if (rc == E_OK) {
    rc = fs->stat(full, &st);
    if (rc == E_OK && (!st.exists || st.size_hi != 0 || st.size_lo != expect)) rc = E_IO;
  }
  if (rc != E_OK) {
    fo_remove(fs, full);                             // an invalid file is ignored anyway; remove if we can
    return rc;
  }
  fo_delete_gens(fs, dir, m.tabid, m.gen);           // older generations (and torn leftovers) of this tab
  return E_OK;
}

// a newer than b (same tab): higher seq, else the later generation (8-bit, wrapping)
static int fo_backup_newer(const BackupMeta& a, const BackupMeta& b)
{
  u32 d;
  if (a.seq != b.seq) return a.seq > b.seq;
  d = (a.gen - b.gen) & 0xFFu;
  return d != 0 && d < 128;
}

int backup_list(FileSys* fs, const char* dir, Vec<BackupMeta>* out)
{
  Vec<FoName> names;
  char full[FO_PATH];
  u32 i, j;
  int rc;
  out->clear();
  rc = fo_scan_backups(fs, dir, 0, 0, &names);
  if (rc != E_OK) return rc;
  for (i = 0; i < names.size(); i++) {
    BackupMeta m;
    if (fo_join(dir, names[i].s, full, sizeof(full)) != E_OK) continue;
    rc = fo_backup_check_path(fs, full, names[i].tabid, names[i].gen, &m);
    if (rc == E_NOMEM) { out->clear(); return rc; }
    if (rc != E_OK) continue;                         // torn, foreign or unreadable: ignored
    for (j = 0; j < out->size() && (*out)[j].tabid < m.tabid; j++) {}
    if (j < out->size() && (*out)[j].tabid == m.tabid) {
      if (fo_backup_newer(m, (*out)[j])) (*out)[j] = m;
    } else if (out->insert(j, &m, 1) != E_OK) {
      out->clear();
      return E_NOMEM;
    }
  }
  return E_OK;
}

int backup_open(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource** data)
{
  char name[13], full[FO_PATH];
  BackupMeta hm;
  FsFile f = 0;
  u32 hlen = 0;
  epos dlen = 0, done = 0;
  u8* buf = 0;
  MemSource* ms;
  int rc;
  *data = 0;
  if (m.tabid > 0xFFFFFu || m.gen > 0xFFu) return E_RANGE;
  backup_file_name(m.tabid, m.gen, name);
  rc = fo_join(dir, name, full, sizeof(full));
  if (rc != E_OK) return rc;
  rc = fs->open(full, FS_READ | FS_SHARE_ALL, &f);
  if (rc != E_OK) return rc;
  rc = fo_backup_check(fs, f, m.tabid, m.gen, &hm, &hlen, &dlen);
  if (rc == E_OK) {
    buf = (u8*)e_valloc(dlen);
    if (!buf) rc = E_NOMEM;
  }
  while (rc == E_OK && done < dlen) {
    u32 k = dlen - done;
    if (k > FO_CHUNK) k = FO_CHUNK;
    rc = fo_read_exact(fs, f, hlen + done, buf + done, k);
    done += k;
  }
  fs->close(f);
  if (rc == E_OK) {
    ms = new MemSource();
    if (!ms) rc = E_NOMEM;
    else { ms->adopt(buf, dlen, 1); *data = ms; return E_OK; }
  }
  if (buf) e_vfree(buf, dlen);
  return rc == E_IO ? E_FORMAT : rc;
}

int backup_delete_tab(FileSys* fs, const char* dir, u32 tabid)
{
  return fo_delete_gens(fs, dir, tabid, 0xFFFFFFFFu);
}
