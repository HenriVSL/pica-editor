// fsys.h - abstract file system (DESIGN 6.20). Owner after Wave 0: T05.
#ifndef PICA_FSYS_H
#define PICA_FSYS_H

#include "etypes.h"

typedef void* FsFile;
enum FsMode { FS_READ = 1, FS_WRITE = 2, FS_DENY_WRITE = 0x10, FS_SHARE_ALL = 0x20, FS_NEW = 0x40 };
// FS_READ: open existing for reading. FS_WRITE: create/truncate for writing (FS_NEW: fail if exists).
// Sharing: default = share read; FS_DENY_WRITE = share read only (same as default, explicit); FS_SHARE_ALL = share
// read + write. FILE_SHARE_DELETE is never used.
enum FsAttr { FSA_READONLY = 1, FSA_HIDDEN = 2, FSA_SYSTEM = 4, FSA_DIR = 0x10, FSA_ARCHIVE = 0x20 };
struct FsStat { u32 exists; u32 attrs; u32 size_lo, size_hi; u32 mtime_lo, mtime_hi; u32 ctime_lo, ctime_hi; };
struct FsFind { char name[260]; u32 attrs; u32 size_lo, size_hi; void* h; };
class FileSys {                         // thread-safe: no shared mutable state besides OS handles
public:
  virtual ~FileSys() {}
  virtual int  open(const char* path, u32 mode, FsFile* out) = 0;   // E_NOTFOUND E_ACCESS E_SHARING E_EXISTS E_IO
  virtual int  read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got) = 0;
  virtual int  write(FsFile f, const void* buf, u32 n) = 0;         // sequential; E_DISKFULL E_IO
  virtual int  flush(FsFile f) = 0;
  virtual int  size(FsFile f, u32* lo, u32* hi) = 0;
  virtual int  close(FsFile f) = 0;
  virtual int  stat(const char* path, FsStat* st) = 0;              // missing file: E_OK with exists = 0
  virtual int  set_attrs(const char* path, u32 attrs) = 0;
  virtual int  set_ctime(const char* path, u32 lo, u32 hi) = 0;
  virtual int  move(const char* from, const char* to) = 0;          // E_EXISTS if 'to' exists (Win98 semantics)
  virtual int  remove(const char* path) = 0;
  virtual int  copy(const char* from, const char* to) = 0;          // overwrites 'to'
  virtual int  make_temp(const char* dir, const char* prefix3, char* out, u32 cap) = 0;  // creates an empty unique 8.3 file
  virtual int  mkdir(const char* path) = 0;
  virtual int  free_space(const char* dir, u64* bytes) = 0;
  virtual int  find_first(const char* dir, FsFind* f) = 0;          // all entries except . and ..; E_NOTFOUND if none
  virtual int  find_next(FsFind* f) = 0;                            // E_NOTFOUND at end
  virtual void find_close(FsFind* f) = 0;
  virtual int  is_remote(const char* path) = 0;                     // network drive (polling only, longer timeouts)
};
FileSys* fs_os();                       // fs_win32.cpp (VC6) / tests/fs_posix.cpp (Mac)
class FaultFs : public FileSys {        // tests: wraps another FileSys and fails the Nth call of a kind
public:
  enum { F_OPEN = 0, F_READ, F_WRITE, F_FLUSH, F_CLOSE, F_MOVE, F_REMOVE, F_COPY, F_TEMP, F_KINDS };
  FaultFs(FileSys* inner);
  void fail_on(int kind, u32 nth, int err);   // nth = 1-based call count of that kind
  void reset();  u32 calls(int kind) const;
  // all FileSys methods forward
  int  open(const char* path, u32 mode, FsFile* out);
  int  read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got);
  int  write(FsFile f, const void* buf, u32 n);
  int  flush(FsFile f);
  int  size(FsFile f, u32* lo, u32* hi);
  int  close(FsFile f);
  int  stat(const char* path, FsStat* st);
  int  set_attrs(const char* path, u32 attrs);
  int  set_ctime(const char* path, u32 lo, u32 hi);
  int  move(const char* from, const char* to);
  int  remove(const char* path);
  int  copy(const char* from, const char* to);
  int  make_temp(const char* dir, const char* prefix3, char* out, u32 cap);
  int  mkdir(const char* path);
  int  free_space(const char* dir, u64* bytes);
  int  find_first(const char* dir, FsFind* f);
  int  find_next(FsFind* f);
  void find_close(FsFind* f);
  int  is_remote(const char* path);
private:                                // private state: T05 may change freely
  enum { FX_KINDS = 15 };
  FileSys* inner_; u32 calls_[FX_KINDS]; u32 fail_nth_[FX_KINDS]; int fail_err_[FX_KINDS]; u8 fail_sticky_[FX_KINDS];
  u32 total_; u32 crash_after_; int torn_; int fake_space_on_; u64 fake_space_; int lock_deny_write_; u32 short_nth_; int late_;
  char locked_[260];
  int hit_(int kind);
  int is_locked_(const char* path) const;
public:                                 // T05 additions (tests only)
  // Extra fault kinds for fail_on/fail_from/calls (the F_* kinds above keep their values).
  enum { F_STAT = F_KINDS, F_ATTRS, F_CTIME, F_SPACE, F_FIND, F_MKDIR, F_ALL_KINDS };
  void fail_from(int kind, u32 nth, int err);   // the nth and every later call of that kind fail with err
  void crash_after(u32 n);                      // simulated power loss: every call after the first n (all kinds,
                                                // counted from reset) fails with E_IO; 0xFFFFFFFF = off (default)
  u32  total_calls() const;                     // calls of any kind since reset (to size crash_after loops)
  void set_torn_writes(int on);                 // an injected write failure first writes the first half of the buffer;
                                                // an injected copy failure first writes half the source onto 'to'
  void short_write_on(u32 nth);                 // the nth write writes only half its bytes and reports E_OK (a lying
                                                // redirector); 0 = off
  void set_fail_late(int on);                   // injected move/remove/copy failures happen AFTER the operation was
                                                // done (an SMB redirector reporting an error for a completed call)
  void set_free_space(int on, u64 bytes);       // free_space() reports 'bytes' instead of asking the inner FileSys
  void set_locked(const char* path, int deny_write);   // another program holds 'path' open: move/remove of it ->
                                                // E_SHARING; deny_write also fails open-for-write/copy onto it; NULL = off
};

#endif
