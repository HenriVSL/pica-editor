// fs_posix.cpp - FileSys over POSIX for the Mac tests (DESIGN 6.20), emulating the Win98 semantics the save code
// depends on (probe/RESULTS.md 9):
// - move() fails with E_EXISTS if the target exists;
// - move()/remove() of a file that any of our handles holds open fails with E_ACCESS (as on C:);
// - remove() of a read-only file fails with E_ACCESS;
// - share modes: a handle grants FS read sharing always and write sharing only with FS_SHARE_ALL; an open whose
//   access conflicts with another open handle's sharing (or whose sharing doesn't allow that handle's access) fails
//   with E_SHARING;
// - make_temp() creates "<prefix3>XXXX.TMP" (8.3); times are Win32 FILETIME values (100 ns units since 1601).
// The open-handle table is a fixed array without locking: the core tests use the file system from one thread.
// Not compiled on VC6 (fs_win32.cpp is used there). Owner after Wave 0: T05.
#include "fsys.h"
#include "estr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static int map_errno(int e)
{
  switch (e) {
  case ENOENT: case ENOTDIR: case ENAMETOOLONG: return E_NOTFOUND;
  case EACCES: case EPERM: case EROFS: case EISDIR: case ENOTEMPTY: return E_ACCESS;
  case EEXIST: return E_EXISTS;
  case ENOSPC: case EDQUOT: return E_DISKFULL;
  case EBUSY: case ETXTBSY: return E_SHARING;
  case ENOMEM: return E_NOMEM;
  default: break;
  }
  return E_IO;
}

static FsFile fd_to_file(int fd) { return (FsFile)(size_t)(fd + 1); }
static int file_to_fd(FsFile f) { return (int)(size_t)f - 1; }

static void time_to_filetime(time_t t, u32* lo, u32* hi)
{
  u64 ft = ((u64)(i64)t + (u64)11644473600u) * (u64)10000000u;
  *lo = (u32)ft;
  *hi = (u32)(ft >> 32);
}

// ---- open-handle table (Win98 sharing emulation) ----
enum { SH_READ = 1, SH_WRITE = 2, OPEN_MAX = 256 };
struct OpenEnt { int fd; dev_t dev; ino_t ino; u32 access; u32 share; };
static OpenEnt g_open[OPEN_MAX];
static u32 g_nopen = 0;

static int find_open(dev_t dev, ino_t ino, u32 start)
{
  u32 i;
  for (i = start; i < g_nopen; i++) if (g_open[i].dev == dev && g_open[i].ino == ino) return (int)i;
  return -1;
}

// Would a new handle (access, share) on this file conflict with the open ones?
static int share_conflict(dev_t dev, ino_t ino, u32 access, u32 share)
{
  int i = find_open(dev, ino, 0);
  while (i >= 0) {
    const OpenEnt& e = g_open[i];
    if ((access & ~e.share) != 0 || (e.access & ~share) != 0) return 1;
    i = find_open(dev, ino, (u32)i + 1);
  }
  return 0;
}

static void track_open(int fd, u32 access, u32 share)
{
  struct stat s;
  if (g_nopen >= OPEN_MAX || fstat(fd, &s) != 0) return;
  g_open[g_nopen].fd = fd; g_open[g_nopen].dev = s.st_dev; g_open[g_nopen].ino = s.st_ino;
  g_open[g_nopen].access = access; g_open[g_nopen].share = share;
  g_nopen++;
}

static void untrack(int fd)
{
  u32 i;
  for (i = 0; i < g_nopen; i++) {
    if (g_open[i].fd == fd) { g_open[i] = g_open[g_nopen - 1]; g_nopen--; return; }
  }
}

struct PosixFind { DIR* d; char dir[260]; };

class PosixFs : public FileSys {
public:
  int open(const char* path, u32 mode, FsFile* out)
  {
    int fd;
    struct stat s;
    u32 access = (mode & FS_WRITE) ? SH_WRITE : SH_READ;
    u32 share = (mode & FS_SHARE_ALL) ? (SH_READ | SH_WRITE) : SH_READ;
    *out = 0;
    if (::stat(path, &s) == 0) {
      if (S_ISDIR(s.st_mode)) return E_ACCESS;
      if (share_conflict(s.st_dev, s.st_ino, access, share)) return E_SHARING;
      if ((mode & FS_WRITE) && (mode & FS_NEW)) return E_EXISTS;
    }
    if (mode & FS_WRITE) fd = ::open(path, O_WRONLY | O_CREAT | ((mode & FS_NEW) ? O_EXCL : O_TRUNC), 0666);
    else fd = ::open(path, O_RDONLY);
    if (fd < 0) return map_errno(errno);
    track_open(fd, access, share);
    *out = fd_to_file(fd);
    return E_OK;
  }
  int read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got)
  {
    u32 done = 0;
    off_t off = (off_t)(((u64)off_hi << 32) | off_lo);
    while (done < n) {
      ssize_t r = pread(file_to_fd(f), (char*)buf + done, n - done, off + (off_t)done);
      if (r < 0) { if (errno == EINTR) continue; if (got) *got = done; return map_errno(errno); }
      if (r == 0) break;
      done += (u32)r;
    }
    if (got) *got = done;
    return E_OK;
  }
  int write(FsFile f, const void* buf, u32 n)
  {
    u32 done = 0;
    while (done < n) {
      ssize_t r = ::write(file_to_fd(f), (const char*)buf + done, n - done);
      if (r < 0) { if (errno == EINTR) continue; return map_errno(errno); }
      if (r == 0) return E_DISKFULL;
      done += (u32)r;
    }
    return E_OK;
  }
  int flush(FsFile f) { return fsync(file_to_fd(f)) == 0 ? E_OK : map_errno(errno); }
  int size(FsFile f, u32* lo, u32* hi)
  {
    struct stat st;
    if (fstat(file_to_fd(f), &st) != 0) return map_errno(errno);
    *lo = (u32)((u64)st.st_size);
    *hi = (u32)((u64)st.st_size >> 32);
    return E_OK;
  }
  int close(FsFile f)
  {
    int fd = file_to_fd(f);
    untrack(fd);
    return ::close(fd) == 0 ? E_OK : map_errno(errno);
  }
  int stat(const char* path, FsStat* st)
  {
    struct stat s;
    memset(st, 0, sizeof(*st));
    if (::stat(path, &s) != 0) {
      if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) return E_OK;
      return map_errno(errno);
    }
    st->exists = 1;
    st->attrs = (S_ISDIR(s.st_mode) ? FSA_DIR : FSA_ARCHIVE) | ((s.st_mode & S_IWUSR) ? 0 : FSA_READONLY);
    st->size_lo = (u32)((u64)s.st_size);
    st->size_hi = (u32)((u64)s.st_size >> 32);
    time_to_filetime(s.st_mtime, &st->mtime_lo, &st->mtime_hi);
#if defined(__APPLE__)
    time_to_filetime(s.st_birthtime, &st->ctime_lo, &st->ctime_hi);
#else
    time_to_filetime(s.st_ctime, &st->ctime_lo, &st->ctime_hi);
#endif
    return E_OK;
  }
  int set_attrs(const char* path, u32 attrs)
  {
    struct stat s;
    mode_t m;
    if (::stat(path, &s) != 0) return map_errno(errno);
    m = s.st_mode & 07777;
    if (attrs & FSA_READONLY) m &= ~(mode_t)0222;
    else m |= S_IWUSR;
    return chmod(path, m) == 0 ? E_OK : map_errno(errno);
  }
  int set_ctime(const char* path, u32 lo, u32 hi)
  {
    struct stat s;
    (void)lo; (void)hi;                              // creation time can't be set on POSIX; check the file exists
    return ::stat(path, &s) == 0 ? E_OK : map_errno(errno);
  }
  int move(const char* from, const char* to)
  {
    struct stat s;
    if (::stat(to, &s) == 0) return E_EXISTS;
    if (::stat(from, &s) != 0) return map_errno(errno);
    if (find_open(s.st_dev, s.st_ino, 0) >= 0) return E_ACCESS;   // Win98: MoveFile of an open file fails
    return rename(from, to) == 0 ? E_OK : map_errno(errno);
  }
  int remove(const char* path)
  {
    struct stat s;
    if (::stat(path, &s) != 0) return map_errno(errno);
    if (S_ISDIR(s.st_mode)) return rmdir(path) == 0 ? E_OK : map_errno(errno);
    if (find_open(s.st_dev, s.st_ino, 0) >= 0) return E_ACCESS;   // Win98: DeleteFile of an open file fails
    if (!(s.st_mode & S_IWUSR)) return E_ACCESS;                   // read-only attribute
    return unlink(path) == 0 ? E_OK : map_errno(errno);
  }
  int copy(const char* from, const char* to)   // like fs_win32: both ends shared read+write, flushed
  {
    FsFile in = 0, out = 0;
    char* buf;
    u32 off = 0, got;
    int rc = open(from, FS_READ | FS_SHARE_ALL, &in), crc;
    if (rc != E_OK) return rc;
    rc = open(to, FS_WRITE | FS_SHARE_ALL, &out);
    if (rc != E_OK) { close(in); return rc; }
    buf = (char*)malloc(65536);
    if (!buf) rc = E_NOMEM;
    while (rc == E_OK) {
      got = 0;
      rc = read_at(in, off, 0, buf, 65536, &got);
      if (rc != E_OK || got == 0) break;
      rc = write(out, buf, got);
      off += got;
    }
    free(buf);
    if (rc == E_OK) rc = flush(out);
    close(in);
    crc = close(out);
    return rc != E_OK ? rc : crc;
  }
  int make_temp(const char* dir, const char* prefix3, char* out, u32 cap)
  {
    static u32 counter = 0;
    u32 tries;
    char pre[4];
    es_copy(pre, sizeof(pre), prefix3 ? prefix3 : "TMP");
    for (tries = 0; tries < 65536; tries++) {
      char name[16];
      int fd;
      counter = (counter + 0x9E37u + (u32)getpid()) & 0xFFFF;
      es_fmt(name, sizeof(name), "%s%04X.TMP", pre, counter);
      es_copy(out, cap, dir);
      if (es_len(out) && out[es_len(out) - 1] != '/') es_cat(out, cap, "/");
      es_cat(out, cap, name);
      fd = ::open(out, O_WRONLY | O_CREAT | O_EXCL, 0666);
      if (fd >= 0) { ::close(fd); return E_OK; }
      if (errno != EEXIST) { int e = map_errno(errno); out[0] = 0; return e; }
    }
    out[0] = 0;
    return E_EXISTS;
  }
  int mkdir(const char* path) { return ::mkdir(path, 0777) == 0 ? E_OK : map_errno(errno); }
  int free_space(const char* dir, u64* bytes)
  {
    struct statvfs v;
    if (statvfs(dir, &v) != 0) return map_errno(errno);
    *bytes = (u64)v.f_bavail * (u64)v.f_frsize;
    return E_OK;
  }
  int find_first(const char* dir, FsFind* f)
  {
    PosixFind* pf = (PosixFind*)malloc(sizeof(PosixFind));
    int rc;
    f->h = 0;
    if (!pf) return E_NOMEM;
    pf->d = opendir(dir);
    if (!pf->d) { rc = map_errno(errno); free(pf); return rc; }
    es_copy(pf->dir, sizeof(pf->dir), dir);
    f->h = pf;
    rc = find_next(f);
    if (rc != E_OK) find_close(f);
    return rc;
  }
  int find_next(FsFind* f)
  {
    PosixFind* pf = (PosixFind*)f->h;
    struct dirent* e;
    if (!pf) return E_NOTFOUND;
    while ((e = readdir(pf->d)) != 0) {
      char full[600];
      FsStat st;
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      es_copy(f->name, sizeof(f->name), e->d_name);
      es_copy(full, sizeof(full), pf->dir);
      es_cat(full, sizeof(full), "/");
      es_cat(full, sizeof(full), e->d_name);
      if (stat(full, &st) == E_OK && st.exists) {
        f->attrs = st.attrs; f->size_lo = st.size_lo; f->size_hi = st.size_hi;
      } else {
        f->attrs = 0; f->size_lo = f->size_hi = 0;
      }
      return E_OK;
    }
    return E_NOTFOUND;
  }
  void find_close(FsFind* f)
  {
    PosixFind* pf = (PosixFind*)f->h;
    if (pf) { closedir(pf->d); free(pf); }
    f->h = 0;
  }
  int is_remote(const char* path) { (void)path; return 0; }
};

static PosixFs g_fs;

FileSys* fs_os()
{
  return &g_fs;
}

// Test helper (not in fsys.h): number of handles currently open through fs_os(); the fileops suite checks it is
// back to its start value after each test (no leaked handles).
u32 fs_posix_open_count() { return g_nopen; }
