// faultfs.cpp - FaultFs: forwards to an inner FileSys and fails the Nth call of a kind (DESIGN 6.20).
// T00: forwarding and fault injection work (simple). Owner after Wave 0: T05.
#include "fsys.h"

FaultFs::FaultFs(FileSys* inner) : inner_(inner) { reset(); }

void FaultFs::fail_on(int kind, u32 nth, int err)
{
  if (kind < 0 || kind >= F_KINDS) return;
  fail_nth_[kind] = nth;
  fail_err_[kind] = err;
}

void FaultFs::reset()
{
  int i;
  for (i = 0; i < F_KINDS; i++) { calls_[i] = 0; fail_nth_[i] = 0; fail_err_[i] = E_OK; }
}

u32 FaultFs::calls(int kind) const
{
  return (kind >= 0 && kind < F_KINDS) ? calls_[kind] : 0;
}

int FaultFs::hit_(int kind)   // returns the injected error or E_OK
{
  calls_[kind]++;
  if (fail_nth_[kind] && calls_[kind] == fail_nth_[kind]) return fail_err_[kind];
  return E_OK;
}

int FaultFs::open(const char* path, u32 mode, FsFile* out) { int e = hit_(F_OPEN); return e ? e : inner_->open(path, mode, out); }
int FaultFs::read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got)
{ int e = hit_(F_READ); return e ? e : inner_->read_at(f, off_lo, off_hi, buf, n, got); }
int FaultFs::write(FsFile f, const void* buf, u32 n) { int e = hit_(F_WRITE); return e ? e : inner_->write(f, buf, n); }
int FaultFs::flush(FsFile f) { int e = hit_(F_FLUSH); return e ? e : inner_->flush(f); }
int FaultFs::size(FsFile f, u32* lo, u32* hi) { return inner_->size(f, lo, hi); }
int FaultFs::close(FsFile f) { int e = hit_(F_CLOSE); int r = inner_->close(f); return e ? e : r; }   // always closes
int FaultFs::stat(const char* path, FsStat* st) { return inner_->stat(path, st); }
int FaultFs::set_attrs(const char* path, u32 attrs) { return inner_->set_attrs(path, attrs); }
int FaultFs::set_ctime(const char* path, u32 lo, u32 hi) { return inner_->set_ctime(path, lo, hi); }
int FaultFs::move(const char* from, const char* to) { int e = hit_(F_MOVE); return e ? e : inner_->move(from, to); }
int FaultFs::remove(const char* path) { int e = hit_(F_REMOVE); return e ? e : inner_->remove(path); }
int FaultFs::copy(const char* from, const char* to) { int e = hit_(F_COPY); return e ? e : inner_->copy(from, to); }
int FaultFs::make_temp(const char* dir, const char* prefix3, char* out, u32 cap)
{ int e = hit_(F_TEMP); return e ? e : inner_->make_temp(dir, prefix3, out, cap); }
int FaultFs::mkdir(const char* path) { return inner_->mkdir(path); }
int FaultFs::free_space(const char* dir, u64* bytes) { return inner_->free_space(dir, bytes); }
int FaultFs::find_first(const char* dir, FsFind* f) { return inner_->find_first(dir, f); }
int FaultFs::find_next(FsFind* f) { return inner_->find_next(f); }
void FaultFs::find_close(FsFind* f) { inner_->find_close(f); }
int FaultFs::is_remote(const char* path) { return inner_->is_remote(path); }
