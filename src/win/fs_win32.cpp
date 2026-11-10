// fs_win32.cpp - FileSys over the ANSI Win32 API (DESIGN 6.20, probe/RESULTS.md 7 and 9). Owner after Wave 0: T05.
// - CreateFileA with share READ (default / FS_DENY_WRITE) or READ|WRITE (FS_SHARE_ALL); never FILE_SHARE_DELETE
//   (invalid parameter on 9x).
// - 64-bit offsets with SetFilePointer(lo, &hi); SetLastError(0) first so a low part of 0xFFFFFFFF is not an error.
// - copy() is our own loop (not CopyFileA): both ends opened shared read+write so the in-place save fallback can
//   overwrite a file another program keeps open; the target is overwritten in place (OPEN_ALWAYS + SetEndOfFile, so
//   its attributes and creation time stay) and flushed.
// - GetFileAttributesExA and GetDiskFreeSpaceExA through GetProcAddress (looked up once, at static init), with
//   FindFirstFileA / GetDiskFreeSpaceA fallbacks.
// Thread-safe: no mutable state besides OS handles.
#include <windows.h>
#include "fsys.h"
#include "estr.h"

static int map_err(DWORD e)
{
  switch (e) {
  case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH:
  case ERROR_BAD_NET_NAME: case ERROR_NO_MORE_FILES: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME:
  case ERROR_FILENAME_EXCED_RANGE: case ERROR_DIRECTORY:
    return E_NOTFOUND;
  case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: case ERROR_CURRENT_DIRECTORY: case ERROR_CANNOT_MAKE:
  case ERROR_NETWORK_ACCESS_DENIED: case ERROR_DIR_NOT_EMPTY: case ERROR_INVALID_ACCESS:
    return E_ACCESS;
  case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_DRIVE_LOCKED: case ERROR_SHARING_BUFFER_EXCEEDED:
    return E_SHARING;
  case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS:
    return E_EXISTS;
  case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL:
    return E_DISKFULL;
  case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY:
    return E_NOMEM;
  default:
    break;
  }
  return E_IO;
}

static int last_err()
{
  return map_err(GetLastError());
}

static int not_found_code(DWORD e)
{
  return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME ||
         e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME || e == ERROR_INVALID_DRIVE || e == ERROR_BAD_PATHNAME ||
         e == ERROR_FILENAME_EXCED_RANGE || e == ERROR_NO_MORE_FILES;
}

typedef BOOL (WINAPI* AttrExFn)(LPCSTR, int, LPVOID);
typedef BOOL (WINAPI* FreeExFn)(LPCSTR, ULARGE_INTEGER*, ULARGE_INTEGER*, ULARGE_INTEGER*);

struct AttrData {               // WIN32_FILE_ATTRIBUTE_DATA (declared here: not in every VC6 header configuration)
  DWORD attrs; FILETIME ctime, atime, mtime; DWORD size_hi, size_lo;
};

static void root_of(const char* path, char* out, u32 cap)   // "C:\" or "\\server\share\" (or "" if unknown)
{
  u32 n, seps = 0;
  out[0] = 0;
  if (cap < 4) return;
  if (path[0] && path[1] == ':') { out[0] = path[0]; out[1] = ':'; out[2] = '\\'; out[3] = 0; return; }
  if (path[0] != '\\' || path[1] != '\\') return;
  for (n = 2; path[n] && n + 2 < cap; n++) {
    if (path[n] == '\\' && ++seps == 2) break;
  }
  es_copy(out, n + 1, path);
  es_cat(out, cap, "\\");
}

// Copies path without a trailing separator ("C:\dir\" -> "C:\dir"; roots "C:\" and "\" keep theirs).
static void strip_sep(const char* path, char* out, u32 cap)
{
  u32 n;
  es_copy(out, cap, path);
  n = es_len(out);
  while (n > 1 && (out[n - 1] == '\\' || out[n - 1] == '/') && !(n == 3 && out[1] == ':')) out[--n] = 0;
}

static int has_wild(const char* s)
{
  for (; *s; s++) if (*s == '*' || *s == '?') return 1;
  return 0;
}

static void join(const char* dir, const char* name, char* out, u32 cap)
{
  u32 n;
  es_copy(out, cap, dir);
  n = es_len(out);
  if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/' && out[n - 1] != ':') es_cat(out, cap, "\\");
  es_cat(out, cap, name);
}

static int is_dot(const char* s)
{
  return s[0] == '.' && (s[1] == 0 || (s[1] == '.' && s[2] == 0));
}

class Win32Fs : public FileSys {
public:
  Win32Fs()
  {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    attr_ex_ = k ? (AttrExFn)GetProcAddress(k, "GetFileAttributesExA") : 0;
    free_ex_ = k ? (FreeExFn)GetProcAddress(k, "GetDiskFreeSpaceExA") : 0;
  }
  int open(const char* path, u32 mode, FsFile* out)
  {
    DWORD share = FILE_SHARE_READ, access, disp;
    HANDLE h;
    *out = 0;
    if (mode & FS_SHARE_ALL) share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    if (mode & FS_WRITE) { access = GENERIC_WRITE; disp = (mode & FS_NEW) ? CREATE_NEW : CREATE_ALWAYS; }
    else { access = GENERIC_READ; disp = OPEN_EXISTING; }
    h = CreateFileA(path, access, share, 0, disp, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return last_err();
    *out = (FsFile)h;
    return E_OK;
  }
  int read_at(FsFile f, u32 off_lo, u32 off_hi, void* buf, u32 n, u32* got)
  {
    LONG hi = (LONG)off_hi;
    DWORD r, done = 0;
    if (got) *got = 0;
    SetLastError(NO_ERROR);
    if (SetFilePointer((HANDLE)f, (LONG)off_lo, &hi, FILE_BEGIN) == 0xFFFFFFFF && GetLastError() != NO_ERROR)
      return last_err();
    while (done < n) {
      if (!ReadFile((HANDLE)f, (char*)buf + done, n - done, &r, 0)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) break;
        if (got) *got = done;
        return map_err(e);
      }
      if (r == 0) break;
      done += r;
    }
    if (got) *got = done;
    return E_OK;
  }
  int write(FsFile f, const void* buf, u32 n)
  {
    DWORD w, done = 0;
    while (done < n) {
      if (!WriteFile((HANDLE)f, (const char*)buf + done, n - done, &w, 0)) return last_err();
      if (w == 0) return E_DISKFULL;
      done += w;
    }
    return E_OK;
  }
  int flush(FsFile f) { return FlushFileBuffers((HANDLE)f) ? E_OK : last_err(); }
  int size(FsFile f, u32* lo, u32* hi)
  {
    DWORD h = 0, l;
    SetLastError(NO_ERROR);
    l = GetFileSize((HANDLE)f, &h);
    if (l == 0xFFFFFFFF && GetLastError() != NO_ERROR) return last_err();
    *lo = l; *hi = h;
    return E_OK;
  }
  int close(FsFile f) { return CloseHandle((HANDLE)f) ? E_OK : last_err(); }
  int stat(const char* path, FsStat* st)
  {
    char p[MAX_PATH + 4];
    AttrData d;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD a;
    memset(st, 0, sizeof(*st));
    if (!path || !path[0] || has_wild(path)) return E_OK;
    if (es_len(path) >= sizeof(p)) return E_OK;
    strip_sep(path, p, sizeof(p));
    if (attr_ex_) {
      if (attr_ex_(p, 0 /* GetFileExInfoStandard */, &d)) {
        st->exists = 1; st->attrs = d.attrs; st->size_lo = d.size_lo; st->size_hi = d.size_hi;
        st->mtime_lo = d.mtime.dwLowDateTime; st->mtime_hi = d.mtime.dwHighDateTime;
        st->ctime_lo = d.ctime.dwLowDateTime; st->ctime_hi = d.ctime.dwHighDateTime;
        return E_OK;
      }
      if (not_found_code(GetLastError())) return E_OK;
      // other errors (roots on some redirectors): fall through to the old APIs
    }
    h = FindFirstFileA(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
      FindClose(h);
      st->exists = 1; st->attrs = fd.dwFileAttributes; st->size_lo = fd.nFileSizeLow; st->size_hi = fd.nFileSizeHigh;
      st->mtime_lo = fd.ftLastWriteTime.dwLowDateTime; st->mtime_hi = fd.ftLastWriteTime.dwHighDateTime;
      st->ctime_lo = fd.ftCreationTime.dwLowDateTime; st->ctime_hi = fd.ftCreationTime.dwHighDateTime;
      return E_OK;
    }
    a = GetFileAttributesA(p);                     // roots ("C:\") have no find data
    if (a != 0xFFFFFFFF) { st->exists = 1; st->attrs = a; return E_OK; }
    {
      DWORD e = GetLastError();
      if (not_found_code(e)) return E_OK;
      return map_err(e);
    }
  }
  int set_attrs(const char* path, u32 attrs)
  {
    DWORD a = attrs & (FSA_READONLY | FSA_HIDDEN | FSA_SYSTEM | FSA_ARCHIVE);
    return SetFileAttributesA(path, a ? a : FILE_ATTRIBUTE_NORMAL) ? E_OK : last_err();
  }
  int set_ctime(const char* path, u32 lo, u32 hi)
  {
    FILETIME ft;
    int rc = E_OK;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return last_err();
    ft.dwLowDateTime = lo; ft.dwHighDateTime = hi;
    if (!SetFileTime(h, &ft, 0, 0)) rc = last_err();
    CloseHandle(h);
    return rc;
  }
  int move(const char* from, const char* to)
  {
    if (GetFileAttributesA(to) != 0xFFFFFFFF) return E_EXISTS;
    return MoveFileA(from, to) ? E_OK : last_err();
  }
  int remove(const char* path)
  {
    DWORD a = GetFileAttributesA(path);
    if (a == 0xFFFFFFFF) return last_err();
    if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? E_OK : last_err();
    return DeleteFileA(path) ? E_OK : last_err();
  }
  int copy(const char* from, const char* to)
  {
    HANDLE in, out;
    char* buf;
    DWORD r, w;
    int rc = E_OK;
    in = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (in == INVALID_HANDLE_VALUE) return last_err();
    out = CreateFileA(to, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (out == INVALID_HANDLE_VALUE) { rc = last_err(); CloseHandle(in); return rc; }
    buf = (char*)VirtualAlloc(0, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) rc = E_NOMEM;
    while (rc == E_OK) {
      if (!ReadFile(in, buf, 65536, &r, 0)) { rc = last_err(); break; }
      if (r == 0) break;
      if (!WriteFile(out, buf, r, &w, 0)) { rc = last_err(); break; }
      if (w != r) { rc = E_DISKFULL; break; }
    }
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    if (rc == E_OK && !SetEndOfFile(out)) rc = last_err();   // the old content may have been longer
    if (rc == E_OK && !FlushFileBuffers(out)) rc = last_err();
    CloseHandle(in);
    if (!CloseHandle(out) && rc == E_OK) rc = last_err();
    return rc;
  }
  int make_temp(const char* dir, const char* prefix3, char* out, u32 cap)
  {
    char buf[MAX_PATH + 16];
    if (cap) out[0] = 0;
    if (!GetTempFileNameA(dir, prefix3, 0, buf)) return last_err();
    if (es_len(buf) + 1 > cap) { DeleteFileA(buf); return E_RANGE; }
    es_copy(out, cap, buf);
    return E_OK;
  }
  int mkdir(const char* path) { return CreateDirectoryA(path, 0) ? E_OK : last_err(); }
  int free_space(const char* dir, u64* bytes)
  {
    char root[MAX_PATH];
    DWORD spc, bps, nfree, ntotal;
    if (free_ex_) {
      ULARGE_INTEGER avail, total, tfree;
      char d[MAX_PATH + 4];
      es_copy(d, sizeof(d), dir);
      if (es_len(d) && d[es_len(d) - 1] != '\\') es_cat(d, sizeof(d), "\\");
      if (free_ex_(d, &avail, &total, &tfree)) {
        *bytes = ((u64)avail.HighPart << 32) | avail.LowPart;
        return E_OK;
      }
    }
    root_of(dir, root, sizeof(root));
    if (!GetDiskFreeSpaceA(root[0] ? root : 0, &spc, &bps, &nfree, &ntotal)) return last_err();
    *bytes = (u64)spc * bps * nfree;
    return E_OK;
  }
  int find_first(const char* dir, FsFind* f)
  {
    char pat[MAX_PATH + 8];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    join(dir, "*.*", pat, sizeof(pat));
    f->h = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return last_err();
    f->h = (void*)h;
    for (;;) {
      if (!is_dot(fd.cFileName)) { fill_(f, &fd); return E_OK; }
      if (!FindNextFileA(h, &fd)) { FindClose(h); f->h = 0; return E_NOTFOUND; }
    }
  }
  int find_next(FsFind* f)
  {
    WIN32_FIND_DATAA fd;
    if (!f->h) return E_NOTFOUND;
    for (;;) {
      if (!FindNextFileA((HANDLE)f->h, &fd)) {
        DWORD e = GetLastError();
        return (e == ERROR_NO_MORE_FILES || e == NO_ERROR) ? E_NOTFOUND : map_err(e);
      }
      if (is_dot(fd.cFileName)) continue;
      fill_(f, &fd);
      return E_OK;
    }
  }
  void find_close(FsFind* f)
  {
    if (f->h) FindClose((HANDLE)f->h);
    f->h = 0;
  }
  int is_remote(const char* path)
  {
    char root[MAX_PATH];
    if (path[0] == '\\' && path[1] == '\\') return 1;
    root_of(path, root, sizeof(root));
    return root[0] && GetDriveTypeA(root) == DRIVE_REMOTE;
  }
private:
  AttrExFn attr_ex_;
  FreeExFn free_ex_;
  static void fill_(FsFind* f, const WIN32_FIND_DATAA* fd)
  {
    es_copy(f->name, sizeof(f->name), fd->cFileName);
    f->attrs = fd->dwFileAttributes;
    f->size_lo = fd->nFileSizeLow;
    f->size_hi = fd->nFileSizeHigh;
  }
};

static Win32Fs g_fs;            // constructed at CRT startup, before any thread exists

FileSys* fs_os()
{
  return &g_fs;
}
