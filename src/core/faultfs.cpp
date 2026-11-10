// faultfs.cpp - FaultFs: forwards to an inner FileSys and fails chosen calls (DESIGN 6.20). Tests only.
// Modes: one-shot (fail_on), persistent (fail_from), simulated power loss (crash_after: every later call fails),
// torn writes (an injected write failure writes half the buffer first), fake free space, a "locked" path (another
// program holds it open). Not thread-safe (counters); tests use it from one thread. Owner after Wave 0: T05.
#include "fsys.h"
#include "estr.h"
#include <string.h>

typedef char faultfs_kinds_check[(FaultFs::F_ALL_KINDS == 15) ? 1 : -1];   // FX_KINDS in fsys.h must match

FaultFs::FaultFs(FileSys* inner) : inner_(inner) { reset(); }

void FaultFs::fail_on(int kind, u32 nth, int err)
{
  if (kind < 0 || kind >= F_ALL_KINDS) return;
  fail_nth_[kind] = nth;
  fail_err_[kind] = err;
  fail_sticky_[kind] = 0;
}

void FaultFs::fail_from(int kind, u32 nth, int err)
{
  if (kind < 0 || kind >= F_ALL_KINDS) return;
  fail_nth_[kind] = nth;
  fail_err_[kind] = err;
  fail_sticky_[kind] = 1;
}

void FaultFs::reset()
{
  int i;
  for (i = 0; i < F_ALL_KINDS; i++) { calls_[i] = 0; fail_nth_[i] = 0; fail_err_[i] = E_OK; fail_sticky_[i] = 0; }
  total_ = 0;
  crash_after_ = 0xFFFFFFFFu;
  torn_ = 0;
  fake_space_on_ = 0;
  fake_space_ = 0;
  lock_deny_write_ = 0;
  short_nth_ = 0;
  late_ = 0;
  locked_[0] = 0;
}

void FaultFs::short_write_on(u32 nth) { short_nth_ = nth; }
void FaultFs::set_fail_late(int on) { late_ = on; }

u32 FaultFs::calls(int kind) const
{
  return (kind >= 0 && kind < F_ALL_KINDS) ? calls_[kind] : 0;
}

void FaultFs::crash_after(u32 n) { crash_after_ = n; }
u32 FaultFs::total_calls() const { return total_; }
void FaultFs::set_torn_writes(int on) { torn_ = on; }
void FaultFs::set_free_space(int on, u64 bytes) { fake_space_on_ = on; fake_space_ = bytes; }

void FaultFs::set_locked(const char* path, int deny_write)
{
  if (path) es_copy(locked_, sizeof(locked_), path);
  else locked_[0] = 0;
  lock_deny_write_ = deny_write;
}

int FaultFs::is_locked_(const char* path) const
{
  return locked_[0] && path && es_icmp(locked_, path) == 0;
}

int FaultFs::hit_(int kind)   // counts the call; returns the injected error or E_OK
{
  u32 nth;
  total_++;
  calls_[kind]++;
  if (crash_after_ != 0xFFFFFFFFu && total_ > crash_after_) return E_IO;
  nth = fail_nth_[kind];
  if (nth && (calls_[kind] == nth || (fail_sticky_[kind] && calls_[kind] > nth))) return fail_err_[kind];
  return E_OK;
}

int FaultFs::open(const char* path, u32 mode, FsFile* out)
{
  int e = hit_(F_OPEN);
  *out = 0;
  if (e) return e;
  if ((mode & FS_WRITE) && lock_deny_write_ && is_locked_(path)) return E_SHARING;
  return inner_->open(path, mode, out);
}

int FaultFs::read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got)
{
  int e = hit_(F_READ);
  if (e) { if (got) *got = 0; return e; }
  return inner_->read_at(f, off_lo, off_hi, buf, n, got);
}

int FaultFs::write(FsFile f, const void* buf, u32 n)
{
  int e = hit_(F_WRITE);
  if (e) {
    if (torn_ && n > 1) inner_->write(f, buf, n / 2);    // the device took part of the data before failing
    return e;
  }
  if (short_nth_ && calls_[F_WRITE] == short_nth_ && n > 1) return inner_->write(f, buf, n / 2);   // lies: E_OK
  return inner_->write(f, buf, n);
}

int FaultFs::flush(FsFile f) { int e = hit_(F_FLUSH); return e ? e : inner_->flush(f); }

int FaultFs::size(FsFile f, u32* lo, u32* hi)
{
  total_++;
  if (crash_after_ != 0xFFFFFFFFu && total_ > crash_after_) return E_IO;
  return inner_->size(f, lo, hi);
}

int FaultFs::close(FsFile f)   // always closes the inner handle (no leaks), then reports the injected error
{
  int e = hit_(F_CLOSE);
  int r = inner_->close(f);
  return e ? e : r;
}

int FaultFs::stat(const char* path, FsStat* st)
{
  int e = hit_(F_STAT);
  if (e) { memset(st, 0, sizeof(*st)); return e; }
  return inner_->stat(path, st);
}

int FaultFs::set_attrs(const char* path, u32 attrs) { int e = hit_(F_ATTRS); return e ? e : inner_->set_attrs(path, attrs); }
int FaultFs::set_ctime(const char* path, u32 lo, u32 hi) { int e = hit_(F_CTIME); return e ? e : inner_->set_ctime(path, lo, hi); }

int FaultFs::move(const char* from, const char* to)
{
  int e = hit_(F_MOVE);
  if (is_locked_(from) || is_locked_(to)) return E_SHARING;
  if (e) { if (late_) inner_->move(from, to); return e; }
  return inner_->move(from, to);
}

int FaultFs::remove(const char* path)
{
  int e = hit_(F_REMOVE);
  if (is_locked_(path)) return E_SHARING;
  if (e) { if (late_) inner_->remove(path); return e; }
  return inner_->remove(path);
}

int FaultFs::copy(const char* from, const char* to)
{
  int e = hit_(F_COPY);
  if (lock_deny_write_ && is_locked_(to)) return E_SHARING;
  if (e && late_) { inner_->copy(from, to); return e; }
  if (e) {
    if (torn_) {                                         // part of the data reached 'to' before the failure
      FsFile in = 0, out = 0;
      u8 buf[4096];
      u32 got = 0;
      if (inner_->open(from, FS_READ | FS_SHARE_ALL, &in) == E_OK) {
        inner_->read_at(in, 0, 0, buf, sizeof(buf), &got);
        inner_->close(in);
        if (inner_->open(to, FS_WRITE | FS_SHARE_ALL, &out) == E_OK) {
          inner_->write(out, buf, got / 2);
          inner_->close(out);
        }
      }
    }
    return e;
  }
  return inner_->copy(from, to);
}

int FaultFs::make_temp(const char* dir, const char* prefix3, char* out, u32 cap)
{
  int e = hit_(F_TEMP);
  if (e) { if (cap) out[0] = 0; return e; }
  return inner_->make_temp(dir, prefix3, out, cap);
}

int FaultFs::mkdir(const char* path) { int e = hit_(F_MKDIR); return e ? e : inner_->mkdir(path); }

int FaultFs::free_space(const char* dir, u64* bytes)
{
  int e = hit_(F_SPACE);
  if (e) return e;
  if (fake_space_on_) { *bytes = fake_space_; return E_OK; }
  return inner_->free_space(dir, bytes);
}

int FaultFs::find_first(const char* dir, FsFind* f)
{
  int e = hit_(F_FIND);
  if (e) { f->h = 0; return e; }
  return inner_->find_first(dir, f);
}

int FaultFs::find_next(FsFind* f) { int e = hit_(F_FIND); return e ? e : inner_->find_next(f); }
void FaultFs::find_close(FsFind* f) { inner_->find_close(f); }
int FaultFs::is_remote(const char* path) { return inner_->is_remote(path); }
