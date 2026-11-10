// t_fileops.cpp - suite "fileops" (T05): FileSys semantics, FaultFs, file_load, FileSource, file_index, safe save
// (fault injection over every call, simulated power loss, locked targets, on-demand hooks), A/B files, backups,
// timings. Runs on the Mac (fs_posix.cpp, Win98 sharing emulated) and on VC6 against C: and M: (-tmp DIR).
// All files live in <tmp>/fo and <tmp>/fo2, wiped before and after. Owner after Wave 0: T05.
#include "tf.h"
#include "fileops.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
u32 fs_posix_open_count();                     // tests/fs_posix.cpp: handles open through fs_os()
static u32 open_count() { return fs_posix_open_count(); }
#else
static u32 open_count() { return 0; }
#endif

static char g_dir[300];                        // <tmp>/fo
static char g_dir2[300];                       // <tmp>/fo2 (scratch)
static int g_remote = 0;                       // tmp folder on a network drive (M:): smaller runs

// ---------------------------------------------------------------------------------------------------------------
// helpers

static void pjoin(const char* dir, const char* name, char* out, u32 cap)
{
  u32 n;
  es_copy(out, cap, dir);
  n = es_len(out);
#if defined(_WIN32)
  if (n && out[n - 1] != '\\') es_cat(out, cap, "\\");
#else
  if (n && out[n - 1] != '/') es_cat(out, cap, "/");
#endif
  es_cat(out, cap, name);
}

static const char* base_of(const char* p)
{
  const char* b = p;
  for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}

struct Nm { char s[64]; int dir; };

static void list_dir(const char* dir, Vec<Nm>* out)
{
  FileSys* fs = fs_os();
  FsFind fd;
  int rc;
  out->clear();
  memset(&fd, 0, sizeof(fd));
  rc = fs->find_first(dir, &fd);
  while (rc == E_OK) {
    Nm n;
    es_copy(n.s, sizeof(n.s), fd.name);
    n.dir = (fd.attrs & FSA_DIR) ? 1 : 0;
    out->push(n);
    rc = fs->find_next(&fd);
  }
  fs->find_close(&fd);
}

static void wipe(const char* dir)
{
  FileSys* fs = fs_os();
  Vec<Nm> names;
  char full[300];
  u32 i;
  list_dir(dir, &names);
  for (i = 0; i < names.size(); i++) {
    pjoin(dir, names[i].s, full, sizeof(full));
    if (names[i].dir) wipe(full);
    else fs->set_attrs(full, 0);
    fs->remove(full);
  }
}

static int put(const char* path, const u8* p, u32 n) { return file_write_direct(fs_os(), path, p, n); }

static int exists(const char* path)
{
  FsStat st;
  return fs_os()->stat(path, &st) == E_OK && st.exists;
}

static int same(const char* path, const u8* p, u32 n)   // 1 equal, 0 different, -1 unreadable
{
  EBytes b;
  if (file_read_all(fs_os(), path, &b, 0xFFFFFFFEu) != E_OK) return -1;
  if (b.size() != n) return 0;
  return n == 0 || memcmp(b.data(), p, n) == 0;
}

static int any_file_equals(const char* dir, const char* except, const u8* p, u32 n)
{
  Vec<Nm> names;
  char full[300];
  u32 i;
  list_dir(dir, &names);
  for (i = 0; i < names.size(); i++) {
    if (names[i].dir || es_icmp(names[i].s, base_of(except)) == 0) continue;
    pjoin(dir, names[i].s, full, sizeof(full));
    if (same(full, p, n) == 1) return 1;
  }
  return 0;
}

// Every file in dir must be one of the allowed paths (empty entries ignored).
static void check_only(TF* tf, const char* dir, const char* const* allowed, u32 na, const char* ctx, int line)
{
  Vec<Nm> names;
  u32 i, j;
  list_dir(dir, &names);
  for (i = 0; i < names.size(); i++) {
    int ok = 0;
    for (j = 0; j < na; j++) if (allowed[j] && allowed[j][0] && es_icmp(names[i].s, base_of(allowed[j])) == 0) ok = 1;
    tf->checks++;
    if (!ok) tf_fail(tf, __FILE__, line, "%s: unexpected file %s", ctx, names[i].s);
  }
}

static void gen_text(TF* tf, u8* p, u32 n)
{
  u32 i;
  for (i = 0; i < n; i++) {
    u32 r = tf_rand_below(tf, 40);
    p[i] = (u8)(r == 0 ? 0x0A : (r == 1 ? 0x0D : 'a' + r % 26));
  }
}

class TestCb : public ProgressCb {
public:
  u32 calls, cancel_at, last_done, last_total, bad;
  TestCb() : calls(0), cancel_at(0), last_done(0), last_total(0), bad(0) {}
  int progress(u32 done, u32 total)
  {
    calls++;
    if (done > total || done < last_done) bad++;
    last_done = done; last_total = total;
    return (cancel_at && calls >= cancel_at) ? 1 : 0;
  }
};

class SrcHooks : public SaveHooks {             // what T09 does for on-demand documents
public:
  FileSource* src; u32 nbefore, nafter; int reopen_rc; char at[300];
  SrcHooks(FileSource* s) : src(s), nbefore(0), nafter(0), reopen_rc(E_OK) { at[0] = 0; }
  void before_replace() { nbefore++; src->close(); }
  void after_replace(const char* p) { nafter++; es_copy(at, sizeof(at), p ? p : ""); reopen_rc = p ? src->reopen(p) : E_STATE; }
};

static int src_equals(ByteSource* s, const u8* p, u32 n)
{
  u8* b;
  int ok;
  if (s->size() != n) return 0;
  b = (u8*)e_valloc(n);
  if (!b) return 0;
  ok = s->read(0, b, n) == E_OK && (n == 0 || memcmp(b, p, n) == 0);
  e_vfree(b, n);
  return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// FileSys semantics (also the probe facts on the VM: open files can't be moved or deleted)

static void t_fsys(TF* tf)
{
  FileSys* fs = fs_os();
  char a[300], b[300], t[300], sub[300], x[300];
  FsFile f = 0, g = 0;
  FsStat st;
  FsFind fd;
  u8 buf[200], rb[100];
  u32 got = 0, lo = 0, hi = 0, i, seen = 0;
  u64 freeb = 0;
  int rc;
  wipe(g_dir);
  pjoin(g_dir, "a.txt", a, sizeof(a));
  pjoin(g_dir, "b.txt", b, sizeof(b));
  for (i = 0; i < 200; i++) buf[i] = (u8)(i * 7 + 1);
  TF_EQ_I(fs->open(a, FS_WRITE, &f), E_OK);
  TF_EQ_I(fs->write(f, buf, 100), E_OK);
  TF_EQ_I(fs->flush(f), E_OK);
  TF_EQ_I(fs->size(f, &lo, &hi), E_OK);
  TF_EQ_U(lo, 100); TF_EQ_U(hi, 0);
  TF_EQ_I(fs->close(f), E_OK);
  TF_EQ_I(fs->open(a, FS_WRITE | FS_NEW, &g), E_EXISTS);
  if (g) { fs->close(g); g = 0; }
  TF_EQ_I(fs->open(a, FS_READ, &f), E_OK);
  TF_EQ_I(fs->read_at(f, 10, 0, rb, 50, &got), E_OK); TF_EQ_U(got, 50); TF_EQ_MEM(rb, buf + 10, 50);
  TF_EQ_I(fs->read_at(f, 90, 0, rb, 50, &got), E_OK); TF_EQ_U(got, 10);
  TF_EQ_I(fs->read_at(f, 500, 0, rb, 50, &got), E_OK); TF_EQ_U(got, 0);
  // probe 9: MoveFile/DeleteFile of a file held open fail (C: ACCESS_DENIED, M: SHARING_VIOLATION)
  rc = fs->move(a, b);
  TF_CHECK(rc == E_ACCESS || rc == E_SHARING);
  tf_log(tf, "move of an open file: %s", e_errname(rc));
  TF_CHECK(exists(a) && !exists(b));
  rc = fs->remove(a);
  TF_CHECK(rc == E_ACCESS || rc == E_SHARING);
  TF_CHECK(exists(a));
  TF_EQ_I(fs->open(a, FS_WRITE, &g), E_SHARING);          // our read handle denies writing
  if (g) { fs->close(g); g = 0; }
  TF_EQ_I(fs->open(a, FS_READ | FS_SHARE_ALL, &g), E_OK);  // a second reader is fine
  if (g) { fs->close(g); g = 0; }
  TF_EQ_I(fs->open(a, FS_READ, &g), E_OK);
  if (g) { fs->close(g); g = 0; }
  fs->close(f);
  // stat
  TF_EQ_I(fs->stat(a, &st), E_OK);
  TF_CHECK(st.exists == 1); TF_EQ_U(st.size_lo, 100); TF_CHECK(!(st.attrs & FSA_DIR));
  TF_CHECK(st.mtime_lo != 0 || st.mtime_hi != 0);
  TF_EQ_I(fs->stat(b, &st), E_OK); TF_CHECK(st.exists == 0);
  pjoin(g_dir, "nodir", x, sizeof(x));
  pjoin(x, "y.txt", t, sizeof(t));
  TF_EQ_I(fs->stat(t, &st), E_OK); TF_CHECK(st.exists == 0);
  TF_EQ_I(fs->stat(g_dir, &st), E_OK); TF_CHECK(st.exists == 1 && (st.attrs & FSA_DIR));
  // move / copy
  TF_EQ_I(fs->move(a, b), E_OK);
  TF_CHECK(!exists(a) && exists(b));
  TF_EQ_I(put(a, buf, 5), E_OK);
  TF_EQ_I(fs->move(b, a), E_EXISTS);
  TF_EQ_I(fs->copy(b, a), E_OK);
  TF_EQ_I(same(a, buf, 100), 1);
  TF_EQ_I(put(a, buf, 200), E_OK);
  TF_EQ_I(fs->copy(b, a), E_OK);                           // shorter source: the target is cut to size
  TF_EQ_I(same(a, buf, 100), 1);
  TF_EQ_I(fs->move(t, a), E_EXISTS);
  rc = fs->move(t, x);
  TF_CHECK(rc != E_OK);
  // make_temp: unique empty 8.3 file in the folder
  TF_EQ_I(fs->make_temp(g_dir, "~PC", t, sizeof(t)), E_OK);
  TF_CHECK(exists(t));
  TF_CHECK(es_nicmp(base_of(t), "~PC", 3) == 0);
  TF_CHECK(es_len(base_of(t)) <= 12 && es_icmp(base_of(t) + es_len(base_of(t)) - 4, ".TMP") == 0);
  TF_EQ_I(fs->stat(t, &st), E_OK); TF_EQ_U(st.size_lo, 0);
  TF_EQ_I(fs->make_temp(g_dir, "~PC", x, sizeof(x)), E_OK);
  TF_CHECK(es_icmp(t, x) != 0);
  TF_EQ_I(fs->remove(t), E_OK);
  TF_EQ_I(fs->remove(x), E_OK);
  TF_CHECK(fs->remove(t) != E_OK);
  // read-only attribute
  TF_EQ_I(fs->set_attrs(a, FSA_READONLY), E_OK);
  TF_EQ_I(fs->stat(a, &st), E_OK); TF_CHECK((st.attrs & FSA_READONLY) != 0);
  TF_EQ_I(fs->open(a, FS_WRITE, &g), E_ACCESS);
  if (g) { fs->close(g); g = 0; }
  TF_EQ_I(fs->remove(a), E_ACCESS);
  TF_EQ_I(fs->set_attrs(a, 0), E_OK);
  TF_EQ_I(fs->stat(a, &st), E_OK); TF_CHECK((st.attrs & FSA_READONLY) == 0);
  // folders, free space, enumeration
  pjoin(g_dir, "sub", sub, sizeof(sub));
  TF_EQ_I(fs->mkdir(sub), E_OK);
  TF_CHECK(fs->mkdir(sub) != E_OK);
  memset(&fd, 0, sizeof(fd));
  TF_EQ_I(fs->find_first(sub, &fd), E_NOTFOUND);
  fs->find_close(&fd);
  TF_EQ_I(fs->find_first(x, &fd), E_NOTFOUND);              // missing folder
  fs->find_close(&fd);
  TF_EQ_I(fs->free_space(g_dir, &freeb), E_OK);
  TF_CHECK(freeb > 0);
  rc = fs->find_first(g_dir, &fd);
  TF_EQ_I(rc, E_OK);
  while (rc == E_OK) {
    if (es_icmp(fd.name, "a.txt") == 0) { seen |= 1; TF_EQ_U(fd.size_lo, 100); }
    else if (es_icmp(fd.name, "b.txt") == 0) seen |= 2;
    else if (es_icmp(fd.name, "sub") == 0) { seen |= 4; TF_CHECK((fd.attrs & FSA_DIR) != 0); }
    else seen |= 8;
    rc = fs->find_next(&fd);
  }
  fs->find_close(&fd);
  TF_EQ_U(seen, 7);
  TF_EQ_I(fs->remove(sub), E_OK);
  tf_log(tf, "is_remote(tmp) = %d", fs->is_remote(g_dir));
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------

static void t_readall(TF* tf)
{
  FileSys* fs = fs_os();
  char a[300], m[300];
  EBytes b;
  u8 data[3000];
  wipe(g_dir);
  pjoin(g_dir, "ra.bin", a, sizeof(a));
  pjoin(g_dir, "missing.bin", m, sizeof(m));
  gen_text(tf, data, sizeof(data));
  TF_EQ_I(file_write_direct(fs, a, data, sizeof(data)), E_OK);
  TF_EQ_I(file_read_all(fs, a, &b, 100000), E_OK);
  TF_EQ_U(b.size(), sizeof(data));
  TF_EQ_MEM(b.data(), data, sizeof(data));
  TF_EQ_I(file_read_all(fs, a, &b, 2999), E_TOOBIG);
  TF_EQ_U(b.size(), 0);
  TF_EQ_I(file_read_all(fs, m, &b, 100), E_NOTFOUND);
  TF_EQ_I(file_write_direct(fs, a, data, 0), E_OK);          // truncates
  TF_EQ_I(file_read_all(fs, a, &b, 100), E_OK);
  TF_EQ_U(b.size(), 0);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// FileSource: random reads vs the file with cache sizes 1, 2, 64; close keeps cached blocks; reopen

static void t_filesource(TF* tf)
{
  FileSys* fs = fs_os();
  char p[300], p2[300], p3[300], p4[300], pe[300];
  u32 n = 5 * 65536 + 1234, i, c, k, bad, badget;
  u8* data = (u8*)e_valloc(n);
  u8* other = (u8*)e_valloc(n);
  u8* tmp = (u8*)e_valloc(200000);
  static const u32 caches[3] = { 1, 2, 64 };
  if (!data || !other || !tmp) { TF_CHECK(0); goto out; }
  wipe(g_dir);
  for (i = 0; i < n; i++) data[i] = (u8)tf_rand(tf);
  memcpy(other, data, n);
  for (i = 0; i < n; i += 65536) other[i + 77] ^= 0x5A;
  pjoin(g_dir, "src.bin", p, sizeof(p));
  pjoin(g_dir, "src2.bin", p2, sizeof(p2));
  pjoin(g_dir, "src3.bin", p3, sizeof(p3));
  pjoin(g_dir, "src4.bin", p4, sizeof(p4));
  pjoin(g_dir, "empty.bin", pe, sizeof(pe));
  TF_EQ_I(put(p, data, n), E_OK);
  TF_EQ_I(put(p2, data, n), E_OK);
  TF_EQ_I(put(p3, other, n), E_OK);
  TF_EQ_I(put(p4, data, n - 1), E_OK);
  TF_EQ_I(put(pe, data, 0), E_OK);
  for (c = 0; c < 3; c++) {
    FileSource s;
    epos avail = 0, off = 0;
    const u8* q;
    TF_EQ_I(s.open(fs, p, FS_READ | FS_DENY_WRITE, caches[c]), E_OK);
    TF_EQ_U(s.size(), n);
    TF_EQ_S(s.path(), p);
    bad = 0; badget = 0;
    for (k = 0; k < (g_remote ? 300u : 2000u); k++) {
      u32 len;
      off = tf_rand_below(tf, n);
      len = tf_rand_below(tf, emin(n - off, (u32)150000)) + 1;
      if (s.read(off, tmp, len) != E_OK || memcmp(tmp, data + off, len) != 0) bad++;
      q = s.get(off, &avail);
      if (!q || avail == 0 || avail > 65536 - (off & 0xFFFF) || q[0] != data[off] || q[avail - 1] != data[off + avail - 1])
        badget++;
    }
    TF_EQ_U(bad, 0);
    TF_EQ_U(badget, 0);
    TF_CHECK(s.err() == E_OK);
    q = s.get(n, &avail);
    TF_CHECK(q == 0 && avail == 0);
    TF_EQ_I(s.read(n - 10, tmp, 11), E_RANGE);
    // closed: the last block is still served from the cache; others fail cleanly
    s.close();
    q = s.get(off, &avail);
    TF_CHECK(q != 0 && q[0] == data[off]);
    // reopen on a copy (same bytes, other name): everything readable again
    TF_EQ_I(s.reopen(p2), E_OK);
    TF_EQ_S(s.path(), p2);
    TF_EQ_I(s.read(0, tmp, 100000), E_OK);
    TF_EQ_MEM(tmp, data, 100000);
    TF_EQ_I(s.read(n - 100000, tmp, 100000), E_OK);
    TF_EQ_MEM(tmp, data + n - 100000, 100000);
    // not the same bytes: refused, the source stays closed
    TF_EQ_I(s.reopen(p4), E_STATE);
    TF_EQ_I(s.reopen(p3), E_STATE);
    TF_EQ_I(s.reopen(p), E_OK);
    TF_EQ_I(s.read(65536 * 2, tmp, 1000), E_OK);
    TF_EQ_MEM(tmp, data + 65536 * 2, 1000);
  }
  {
    FileSource s;
    epos avail = 7;
    TF_EQ_I(s.open(fs, pe, FS_READ, 4), E_OK);
    TF_EQ_U(s.size(), 0);
    TF_CHECK(s.get(0, &avail) == 0 && avail == 0);
    TF_EQ_I(s.read(0, tmp, 0), E_OK);
    TF_EQ_I(s.open(fs, p, FS_READ, 0), E_OK);              // re-open on another file; default cache
    TF_EQ_U(s.size(), n);
    TF_EQ_I(s.read(n - 5, tmp, 5), E_OK);
    TF_EQ_MEM(tmp, data + n - 5, 5);
    pjoin(g_dir, "nothere.bin", p4, sizeof(p4));
    TF_EQ_I(s.open(fs, p4, FS_READ, 4), E_NOTFOUND);
    TF_EQ_U(s.size(), 0);
  }
out:
  if (data) e_vfree(data, n);
  if (other) e_vfree(other, n);
  if (tmp) e_vfree(tmp, 200000);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// file_load

static void load_opts(LoadOpts* o, epos threshold)
{
  memset(o, 0, sizeof(*o));
  o->ondemand_threshold = threshold;
  o->det.utf8_nobom = 1;
  o->det.utf16_nobom = 1;
  o->det.ansi = enc_ansi();
  o->det.default_eol = EOL_CRLF;
  o->cache_blocks = 8;
}

static void t_load(TF* tf)
{
  FileSys* fs = fs_os();
  char p[300], m[300];
  LoadOpts o;
  LoadResult r;
  TestCb cb;
  FsFile g = 0;
  u32 n = 300000;
  u8* big = (u8*)e_valloc(n);
  static const u8 plain[] = "hello\r\nworld\r\n";
  static const u8 u8bom[] = { 0xEF, 0xBB, 0xBF, 'x', '\n', 'y' };
  static const u8 u16[] = { 0xFF, 0xFE, 'a', 0, 0x0A, 0, 'b', 0 };
  static const u8 u16nb[] = { 'a', 0, 0x0A, 0, 'b', 0, 0x0D, 0, 0x0A, 0 };
  static const u8 bin[] = { 'a', 'b', 0, 'c', 'd' };
  if (!big) { TF_CHECK(0); return; }
  wipe(g_dir);
  gen_text(tf, big, n);
  pjoin(g_dir, "load.txt", p, sizeof(p));
  pjoin(g_dir, "missing.txt", m, sizeof(m));
  load_opts(&o, 1 << 20);

  TF_EQ_I(put(p, plain, sizeof(plain) - 1), E_OK);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  TF_CHECK(r.src != 0 && r.mode == LM_MEMORY && r.shared_ro == 0 && r.ro_attr == 0);
  if (r.src) { TF_CHECK(src_equals(r.src, plain, sizeof(plain) - 1)); delete r.src; }
  TF_EQ_U(r.det.bom_len, 0);
  TF_EQ_U(r.det.eol_style, EOL_CRLF);
  TF_EQ_U(r.st.size_lo, sizeof(plain) - 1);
  TF_EQ_U(r.det.eol.crlf, 2);

  TF_EQ_I(put(p, u8bom, sizeof(u8bom)), E_OK);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.det.enc.kind, EK_UTF8); TF_EQ_U(r.det.bom_len, 3); TF_EQ_U(r.det.eol_style, EOL_LF);

  TF_EQ_I(put(p, u16, sizeof(u16)), E_OK);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.det.enc.kind, EK_UTF16LE); TF_EQ_U(r.det.bom_len, 2);
  o.force_enc = 1; o.enc = enc_utf8(0);                     // forced UTF-8 on a UTF-16 BOM: no BOM of UTF-8
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.det.enc.kind, EK_UTF8); TF_EQ_U(r.det.bom_len, 0); TF_EQ_U(r.det.enc.bom, 0);
  o.enc = enc_utf16le(0);                                   // forced UTF-16 LE, BOM present
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.det.enc.kind, EK_UTF16LE); TF_EQ_U(r.det.bom_len, 2); TF_EQ_U(r.det.enc.bom, 1);
  TF_EQ_I(put(p, u16nb, sizeof(u16nb)), E_OK);              // forced UTF-16 LE, no BOM: EOLs counted in units
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.det.enc.kind, EK_UTF16LE); TF_EQ_U(r.det.bom_len, 0); TF_EQ_U(r.det.enc.bom, 0);
  TF_EQ_U(r.det.eol.lf, 1); TF_EQ_U(r.det.eol.crlf, 1); TF_EQ_U(r.det.eol_mixed, 1);
  o.force_enc = 0;

  TF_EQ_I(put(p, bin, sizeof(bin)), E_OK);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.det.binary, 1);

  TF_EQ_I(put(p, plain, 0), E_OK);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  TF_CHECK(r.src && r.src->size() == 0);
  if (r.src) delete r.src;

  TF_EQ_I(file_load(fs, m, o, 0, &r), E_NOTFOUND);
  TF_CHECK(r.src == 0);
  TF_EQ_I(file_load(fs, g_dir, o, 0, &r), E_ACCESS);
  TF_CHECK(r.src == 0);

  // read-only attribute
  TF_EQ_I(put(p, plain, 5), E_OK);
  fs->set_attrs(p, FSA_READONLY);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  if (r.src) delete r.src;
  TF_EQ_U(r.ro_attr, 1);
  fs->set_attrs(p, 0);

  // progress, cancel, out of memory
  TF_EQ_I(put(p, big, n), E_OK);
  TF_EQ_I(file_load(fs, p, o, &cb, &r), E_OK);
  TF_CHECK(r.src && src_equals(r.src, big, n));
  if (r.src) delete r.src;
  TF_CHECK(cb.calls >= 4); TF_EQ_U(cb.last_done, n); TF_EQ_U(cb.last_total, n); TF_EQ_U(cb.bad, 0);
  cb.calls = 0; cb.last_done = 0; cb.cancel_at = 2;
  TF_EQ_I(file_load(fs, p, o, &cb, &r), E_CANCEL);
  TF_CHECK(r.src == 0);
  e_test_fail_after(1);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_NOMEM);
  e_test_fail_after(-1);
  TF_CHECK(r.src == 0);

  // other programs keep the file open for writing (share read+write): in-memory load still works
  TF_EQ_I(fs->open(p, FS_WRITE | FS_SHARE_ALL, &g), E_OK);
  if (g) { TF_EQ_I(fs->write(g, big, n), E_OK); fs->flush(g); }
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  TF_CHECK(r.src && src_equals(r.src, big, n));
  if (r.src) delete r.src;
  // ... and on-demand falls back to a shared read-only handle
  load_opts(&o, 1000);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  TF_CHECK(r.mode == LM_ONDEMAND && r.shared_ro == 1);
  TF_CHECK(r.src && src_equals(r.src, big, n));
  if (r.src) delete r.src;
  if (g) { fs->close(g); g = 0; }

  // on-demand: deny-write handle, first 64 KB for detection
  big[0] = 0xEF; big[1] = 0xBB; big[2] = 0xBF;
  TF_EQ_I(put(p, big, n), E_OK);
  TF_EQ_I(file_load(fs, p, o, 0, &r), E_OK);
  TF_CHECK(r.mode == LM_ONDEMAND && r.shared_ro == 0);
  TF_EQ_U(r.det.enc.kind, EK_UTF8); TF_EQ_U(r.det.bom_len, 3);
  TF_EQ_U(r.st.size_lo, n);
  if (r.src) {
    TF_CHECK(src_equals(r.src, big, n));
    TF_EQ_I(fs->open(p, FS_WRITE, &g), E_SHARING);          // nobody can write it while the tab is open
    if (g) { fs->close(g); g = 0; }
    TF_CHECK(fs->remove(p) != E_OK);
    delete r.src;
  }
  TF_EQ_I(fs->open(p, FS_WRITE | FS_SHARE_ALL, &g), E_OK);
  if (g) { fs->close(g); g = 0; }
  e_vfree(big, n);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// file_index vs a brute-force reference

static u16 unit_at(const u8* p, u32 i, int unit, int le)
{
  if (unit == 1) return p[i];
  return (u16)(le ? (p[2 * i] | (p[2 * i + 1] << 8)) : ((p[2 * i] << 8) | p[2 * i + 1]));
}

static void ref_index(const u8* p, u32 n, const BreakSpec& bs, Vec<u32>* blocks, EolCounts* e)
{
  int le = bs.b0 != 0;
  u32 units = bs.unit == 1 ? n : n / 2, i;
  u16 bk = (u16)(bs.unit == 1 ? bs.b0 : (le ? bs.b0 : bs.b1));
  blocks->clear();
  blocks->resize((n + 65535) / 65536);
  e->crlf = e->lf = e->cr = 0;
  for (i = 0; i < units; i++) if (unit_at(p, i, bs.unit, le) == bk) (*blocks)[(i * bs.unit) >> 16]++;
  for (i = 0; i < units; i++) {
    u16 u = unit_at(p, i, bs.unit, le);
    if (u == 0x0A) e->lf++;
    else if (u == 0x0D) {
      if (i + 1 < units && unit_at(p, i + 1, bs.unit, le) == 0x0A) { e->crlf++; i++; }
      else e->cr++;
    }
  }
}

static void check_index(TF* tf, const char* path, const u8* p, u32 n, const char* what)
{
  static const BreakSpec specs[6] = { { 1, 0x0A, 0, 0 }, { 1, 0x0D, 0, 0 }, { 2, 0x0A, 0, 0 }, { 2, 0x0D, 0, 0 },
                                      { 2, 0, 0x0A, 0 }, { 2, 0, 0x0D, 0 } };
  u32 s, i;
  for (s = 0; s < 6; s++) {
    IndexResult r;
    Vec<u32> blocks;
    EolCounts e;
    int rc = file_index(fs_os(), path, specs[s], &r, 0, 0);
    ref_index(p, n, specs[s], &blocks, &e);
    tf->checks++;
    if (rc != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s spec %u: rc %s", what, s, e_errname(rc)); continue; }
    TF_EQ_U(r.size, n);
    TF_EQ_U(r.block_breaks.size(), blocks.size());
    for (i = 0; i < blocks.size() && i < r.block_breaks.size(); i++)
      if (r.block_breaks[i] != blocks[i]) {
        tf_fail(tf, __FILE__, __LINE__, "%s spec %u block %u: %u vs %u", what, s, i, r.block_breaks[i], blocks[i]);
        break;
      }
    if (r.eol.crlf != e.crlf || r.eol.lf != e.lf || r.eol.cr != e.cr)
      tf_fail(tf, __FILE__, __LINE__, "%s spec %u: eol %u/%u/%u vs %u/%u/%u", what, s, r.eol.crlf, r.eol.lf, r.eol.cr,
              e.crlf, e.lf, e.cr);
  }
}

static void t_index(TF* tf)
{
  static const u32 sizes[] = { 0, 1, 2, 3, 65535, 65536, 65537, 131072, 3 * 65536 + 5, 200001 };
  static const u8 alpha[4] = { 0x0A, 0x0D, 0x00, 'a' };
  char p[300], what[64];
  u32 n = 4 * 65536, s, i;
  u8* d = (u8*)e_valloc(n + 8);
  BreakSpec lf = { 1, 0x0A, 0, 0 }, badbs = { 3, 0x0A, 0, 0 };
  IndexResult r;
  TestCb cb;
  volatile i32 cancel = 1;
  if (!d) { TF_CHECK(0); return; }
  wipe(g_dir);
  pjoin(g_dir, "index.bin", p, sizeof(p));
  for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    for (i = 0; i < sizes[s]; i++) d[i] = alpha[tf_rand_below(tf, 4)];
    TF_EQ_I(put(p, d, sizes[s]), E_OK);
    es_fmt(what, sizeof(what), "random %u", sizes[s]);
    check_index(tf, p, d, sizes[s], what);
  }
  // CR / LF pairs split by the 64 KB block boundary, CR at the very end
  memset(d, 'a', 131075);
  d[65535] = 0x0D; d[65536] = 0x0A; d[131074] = 0x0D;
  TF_EQ_I(put(p, d, 131075), E_OK);
  check_index(tf, p, d, 131075, "boundary unit 1");
  memset(d, 'a', 131076);
  d[65534] = 0x0D; d[65535] = 0; d[65536] = 0x0A; d[65537] = 0; d[131074] = 0x0D; d[131075] = 0;
  TF_EQ_I(put(p, d, 131076), E_OK);
  check_index(tf, p, d, 131076, "boundary LE");
  memset(d, 'a', 131076);
  d[65534] = 0; d[65535] = 0x0D; d[65536] = 0; d[65537] = 0x0A; d[131074] = 0; d[131075] = 0x0D;
  TF_EQ_I(put(p, d, 131076), E_OK);
  check_index(tf, p, d, 131076, "boundary BE");
  // exactly one block ending in CR (pending CR at EOF)
  memset(d, 'a', 65536);
  d[65535] = 0x0D;
  TF_EQ_I(put(p, d, 65536), E_OK);
  check_index(tf, p, d, 65536, "full block CR");
  // cancel, errors, a deny-write FileSource holding the file
  for (i = 0; i < 4 * 65536; i++) d[i] = alpha[tf_rand_below(tf, 4)];
  TF_EQ_I(put(p, d, 4 * 65536), E_OK);
  TF_EQ_I(file_index(fs_os(), p, lf, &r, 0, &cancel), E_CANCEL);
  TF_EQ_U(r.block_breaks.size(), 0);
  cb.cancel_at = 2;
  TF_EQ_I(file_index(fs_os(), p, lf, &r, &cb, 0), E_CANCEL);
  TF_EQ_U(cb.calls, 2);
  cb.cancel_at = 0; cb.calls = 0; cb.last_done = 0;
  TF_EQ_I(file_index(fs_os(), p, lf, &r, &cb, 0), E_OK);
  TF_EQ_U(cb.calls, 4); TF_EQ_U(cb.last_done, 4 * 65536); TF_EQ_U(cb.bad, 0);
  TF_EQ_I(file_index(fs_os(), p, badbs, &r, 0, 0), E_RANGE);
  {
    FileSource fsrc;
    TF_EQ_I(fsrc.open(fs_os(), p, FS_READ | FS_DENY_WRITE, 2), E_OK);
    TF_EQ_I(file_index(fs_os(), p, lf, &r, 0, 0), E_OK);
    TF_EQ_U(r.size, 4 * 65536);
  }
  pjoin(g_dir, "nothere.bin", p, sizeof(p));
  TF_EQ_I(file_index(fs_os(), p, lf, &r, 0, 0), E_NOTFOUND);
  e_vfree(d, n + 8);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// Safe save: plain cases

static void t_save_basic(TF* tf)
{
  FileSys* fs = fs_os();
  char p[300], bak[300], x[300];
  u32 nn = 150000, no = 7000;
  u8* nw = (u8*)e_valloc(nn);
  u8* od = (u8*)e_valloc(no);
  SaveOpts so;
  SaveResult r;
  TestCb cb;
  volatile i32 cancel = 1;
  const char* only[2];
  if (!nw || !od) { TF_CHECK(0); return; }
  gen_text(tf, nw, nn);
  gen_text(tf, od, no);
  memset(&so, 0, sizeof(so));
  wipe(g_dir);
  pjoin(g_dir, "save.txt", p, sizeof(p));
  es_copy(bak, sizeof(bak), p); es_cat(bak, sizeof(bak), ".bak");
  only[0] = p; only[1] = bak;
  {
    ViewSource vs(nw, nn);
    ViewSource vo(od, no);
    ViewSource v0(nw, 0);
    // new file
    TF_EQ_I(file_save(fs, p, &vs, so, 0, &cb, &r), E_OK);
    TF_EQ_I(same(p, nw, nn), 1);
    TF_CHECK(r.temp[0] == 0 && r.old_copy[0] == 0 && r.stray[0] == 0 && r.failed_step == 0 && r.in_place == 0);
    TF_EQ_U(r.st.size_lo, nn); TF_CHECK(r.st.exists == 1);
    TF_EQ_U(cb.last_done, nn); TF_EQ_U(cb.bad, 0);
    check_only(tf, g_dir, only, 1, "new", __LINE__);
    // existing file
    TF_EQ_I(put(p, od, no), E_OK);
    TF_EQ_I(file_save(fs, p, &vs, so, 0, 0, &r), E_OK);
    TF_EQ_I(same(p, nw, nn), 1);
    check_only(tf, g_dir, only, 1, "existing", __LINE__);
    // keep .bak (replaced by the next save)
    so.keep_bak = 1;
    TF_EQ_I(put(p, od, no), E_OK);
    TF_EQ_I(file_save(fs, p, &vs, so, 0, 0, &r), E_OK);
    TF_EQ_I(same(p, nw, nn), 1);
    TF_EQ_I(same(bak, od, no), 1);
    TF_EQ_I(file_save(fs, p, &vo, so, 0, 0, &r), E_OK);
    TF_EQ_I(same(p, od, no), 1);
    TF_EQ_I(same(bak, nw, nn), 1);
    check_only(tf, g_dir, only, 2, "bak", __LINE__);
    fs->remove(bak);
    so.keep_bak = 0;
    // read-only target: refused, nothing changed, no temp left
    TF_EQ_I(put(p, od, no), E_OK);
    fs->set_attrs(p, FSA_READONLY);
    TF_EQ_I(file_save(fs, p, &vs, so, 0, 0, &r), E_ACCESS);
    TF_EQ_U(r.failed_step, 4);
    TF_CHECK(r.temp[0] == 0);
    fs->set_attrs(p, 0);
    TF_EQ_I(same(p, od, no), 1);
    check_only(tf, g_dir, only, 1, "read-only", __LINE__);
    // missing folder, no space
    pjoin(g_dir, "nodir", x, sizeof(x));
    pjoin(x, "x.txt", x, sizeof(x));
    TF_EQ_I(file_save(fs, x, &vs, so, 0, 0, &r), E_NOTFOUND);
    TF_EQ_U(r.failed_step, 1);
    {
      FaultFs ff(fs);
      ff.set_free_space(1, 1000);
      TF_EQ_I(file_save(&ff, p, &vs, so, 0, 0, &r), E_DISKFULL);
      TF_EQ_U(r.failed_step, 1);
      TF_EQ_I(same(p, od, no), 1);
      check_only(tf, g_dir, only, 1, "diskfull", __LINE__);
    }
    // cancel (flag and progress callback): temp removed, target untouched
    TF_EQ_I(file_write_temp(fs, p, &vs, 0, &cancel, &r), E_CANCEL);
    TF_CHECK(r.temp[0] == 0);
    cb.calls = 0; cb.last_done = 0; cb.cancel_at = 1;
    TF_EQ_I(file_write_temp(fs, p, &vs, &cb, 0, &r), E_CANCEL);
    TF_CHECK(r.temp[0] == 0);
    TF_EQ_I(same(p, od, no), 1);
    check_only(tf, g_dir, only, 1, "cancel", __LINE__);
    // split as the app does it (worker writes the temp, UI replaces)
    TF_EQ_I(file_write_temp(fs, p, &vs, 0, 0, &r), E_OK);
    TF_CHECK(r.temp[0] != 0 && es_nicmp(base_of(r.temp), "~PC", 3) == 0);
    TF_EQ_I(same(r.temp, nw, nn), 1);
    TF_EQ_I(same(p, od, no), 1);
    TF_EQ_I(file_replace(fs, p, so, 0, &r), E_OK);
    TF_EQ_I(same(p, nw, nn), 1);
    check_only(tf, g_dir, only, 1, "split", __LINE__);
    // the temp file vanished before the replace: refused
    TF_EQ_I(put(p, od, no), E_OK);
    TF_EQ_I(file_write_temp(fs, p, &vs, 0, 0, &r), E_OK);
    fs->remove(r.temp);
    TF_EQ_I(file_replace(fs, p, so, 0, &r), E_IO);
    TF_EQ_I(same(p, od, no), 1);
    TF_EQ_I(file_replace(fs, p, so, 0, &r), E_STATE);        // r.temp cleared
    // empty document
    TF_EQ_I(file_save(fs, p, &v0, so, 0, 0, &r), E_OK);
    TF_EQ_I(same(p, nw, 0), 1);
#if defined(_WIN32)
    // Win32: hidden attribute and creation time survive the save
    {
      FsStat s0, s1;
      TF_EQ_I(put(p, od, no), E_OK);
      fs->set_attrs(p, FSA_HIDDEN);
      TF_EQ_I(fs->stat(p, &s0), E_OK);
      TF_EQ_I(file_save(fs, p, &vs, so, 0, 0, &r), E_OK);
      TF_EQ_I(fs->stat(p, &s1), E_OK);
      TF_CHECK((s1.attrs & FSA_HIDDEN) != 0);
      TF_EQ_U(s1.ctime_lo, s0.ctime_lo); TF_EQ_U(s1.ctime_hi, s0.ctime_hi);
      fs->set_attrs(p, 0);
    }
#endif
    // locked by another program (share read+write): in-place fallback
    {
      FaultFs ff(fs);
      TF_EQ_I(put(p, od, no), E_OK);
      ff.set_locked(p, 0);
      TF_EQ_I(file_save(&ff, p, &vs, so, 0, 0, &r), E_OK);
      TF_EQ_U(r.in_place, 1);
      TF_EQ_I(same(p, nw, nn), 1);
      check_only(tf, g_dir, only, 1, "locked", __LINE__);
      so.keep_bak = 1;
      TF_EQ_I(file_save(&ff, p, &vo, so, 0, 0, &r), E_OK);
      TF_EQ_I(same(p, od, no), 1);
      TF_EQ_I(same(bak, nw, nn), 1);
      so.keep_bak = 0;
      fs->remove(bak);
      // locked deny-write: nothing can be done; the target is intact, every extra file is named
      ff.set_locked(p, 1);
      TF_CHECK(file_save(&ff, p, &vs, so, 0, 0, &r) == E_SHARING);
      TF_EQ_I(same(p, od, no), 1);
      {
        const char* allowed[4];
        allowed[0] = p; allowed[1] = r.temp; allowed[2] = r.old_copy; allowed[3] = r.stray;
        check_only(tf, g_dir, allowed, 4, "locked deny-write", __LINE__);
      }
    }
  }
  e_vfree(nw, nn);
  e_vfree(od, no);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// Safe save of an on-demand document with a real FileSource (the probe's open-file rule)

static void t_save_ondemand(TF* tf)
{
  FileSys* fs = fs_os();
  char p[300];
  u32 no = 200000, nn = 70000;
  u8* od = (u8*)e_valloc(no);
  u8* nw = (u8*)e_valloc(nn);
  LoadOpts o;
  LoadResult lr;
  SaveOpts so;
  SaveResult r;
  int rc;
  if (!od || !nw) { TF_CHECK(0); return; }
  gen_text(tf, od, no);
  gen_text(tf, nw, nn);
  memset(&so, 0, sizeof(so));
  wipe(g_dir);
  pjoin(g_dir, "big.txt", p, sizeof(p));
  TF_EQ_I(put(p, od, no), E_OK);
  load_opts(&o, 1000);
  TF_EQ_I(file_load(fs, p, o, 0, &lr), E_OK);
  TF_CHECK(lr.mode == LM_ONDEMAND && lr.src != 0);
  if (lr.src) {
    FileSource* src = (FileSource*)lr.src;
    ViewSource vs(nw, nn);
    SrcHooks hk(src);
    // forgetting the hooks: the move fails (our own handle) and nothing is damaged
    rc = file_save(fs, p, &vs, so, 0, 0, &r);
    TF_CHECK(rc != E_OK);
    tf_log(tf, "save of an open on-demand file without hooks: %s (step %u)", e_errname(rc), (u32)r.failed_step);
    TF_EQ_I(same(p, od, no), 1);
    TF_CHECK(src_equals(src, od, no));
    {
      const char* allowed[4];
      allowed[0] = p; allowed[1] = r.temp; allowed[2] = r.old_copy; allowed[3] = r.stray;
      check_only(tf, g_dir, allowed, 4, "ondemand nohooks", __LINE__);
      if (r.temp[0]) fs->remove(r.temp);
      if (r.old_copy[0]) fs->remove(r.old_copy);
      if (r.stray[0]) fs->remove(r.stray);
    }
    // with the hooks: closed for the renames, reopened on the old copy
    TF_EQ_I(file_save(fs, p, &vs, so, &hk, 0, &r), E_OK);
    TF_EQ_U(hk.nbefore, 1); TF_EQ_U(hk.nafter, 1);
    TF_EQ_I(hk.reopen_rc, E_OK);
    TF_EQ_I(same(p, nw, nn), 1);
    TF_CHECK(r.old_copy[0] != 0);
    TF_EQ_S(hk.at, r.old_copy);
    TF_EQ_S(src->path(), r.old_copy);
    TF_CHECK(src_equals(src, od, no));                       // pieces over the original stay valid
    TF_CHECK(fs->remove(r.old_copy) != E_OK);                // held open (deny-write) by the source
    TF_EQ_I(same(r.old_copy, od, no), 1);
    delete lr.src;
    TF_EQ_I(fs->remove(r.old_copy), E_OK);
    {
      const char* allowed[1];
      allowed[0] = p;
      check_only(tf, g_dir, allowed, 1, "ondemand", __LINE__);
    }
  }
  e_vfree(od, no);
  e_vfree(nw, nn);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// Safe save: every FileSys call failing in turn (each error kind, one-shot and from then on), and a simulated power
// loss after every call (with torn writes)

struct SaveScenario { const char* name; u8 existing; u8 ondemand; u8 keep_bak; u8 locked; };
struct KindErrs { int kind; const char* name; int errs[4]; u32 n; };

static const KindErrs g_kinds[] = {
  { FaultFs::F_OPEN, "open", { E_IO, E_ACCESS, E_SHARING, E_NOTFOUND }, 4 },
  { FaultFs::F_READ, "read", { E_IO, 0, 0, 0 }, 1 },
  { FaultFs::F_WRITE, "write", { E_IO, E_DISKFULL, 0, 0 }, 2 },
  { FaultFs::F_FLUSH, "flush", { E_IO, E_DISKFULL, 0, 0 }, 2 },
  { FaultFs::F_CLOSE, "close", { E_IO, E_DISKFULL, 0, 0 }, 2 },
  { FaultFs::F_MOVE, "move", { E_IO, E_ACCESS, E_SHARING, E_EXISTS }, 4 },
  { FaultFs::F_REMOVE, "remove", { E_IO, E_ACCESS, E_SHARING, 0 }, 3 },
  { FaultFs::F_COPY, "copy", { E_IO, E_DISKFULL, E_SHARING, 0 }, 3 },
  { FaultFs::F_TEMP, "temp", { E_IO, E_ACCESS, 0, 0 }, 2 },
  { FaultFs::F_STAT, "stat", { E_IO, 0, 0, 0 }, 1 },
  { FaultFs::F_ATTRS, "attrs", { E_IO, 0, 0, 0 }, 1 },
  { FaultFs::F_CTIME, "ctime", { E_IO, 0, 0, 0 }, 1 },
  { FaultFs::F_SPACE, "space", { E_IO, 0, 0, 0 }, 1 },
};

struct SaveRun {                    // inputs of one faulty save
  const SaveScenario* sc; const u8* od; u32 no; const u8* nw; u32 nn; char p[300]; char bak[300];
};

// mode: 0 no fault, 1 one-shot, 2 from nth on, 3 crash after nth calls, 4 nth write lies (short, E_OK),
// 5 one-shot reported after the move/remove/copy was done;
// failing writes/copies are torn in every mode
static int save_run(TF* tf, SaveRun* sr, FaultFs* ff, int mode, int kind, u32 nth, int err, const char* ctx)
{
  const SaveScenario* sc = sr->sc;
  FileSource* src = 0;
  SrcHooks* hk = 0;
  SaveOpts so;
  SaveResult r;
  ViewSource vs(sr->nw, sr->nn);
  int rc, t;
  u32 open0 = open_count();
  memset(&so, 0, sizeof(so));
  so.keep_bak = sc->keep_bak;
  wipe(g_dir);
  if (sc->existing) put(sr->p, sr->od, sr->no);
  ff->reset();
  if (sc->ondemand) {
    epos av = 0;
    src = new FileSource();
    if (!src || src->open(ff, sr->p, FS_READ | FS_DENY_WRITE, 4) != E_OK) { TF_CHECK(0); delete src; return E_STATE; }
    src->get(0, &av);                                         // a cached block: reopen verifies it
    hk = new SrcHooks(src);
  }
  ff->reset();
  if (sc->locked) ff->set_locked(sr->p, 0);
  if (mode == 1) ff->fail_on(kind, nth, err);
  else if (mode == 2) ff->fail_from(kind, nth, err);
  else if (mode == 3) ff->crash_after(nth);
  else if (mode == 4) ff->short_write_on(nth);
  else if (mode == 5) { ff->fail_on(kind, nth, err); ff->set_fail_late(1); }
  ff->set_torn_writes(1);                                     // failing writes and copies leave partial data
  rc = file_save(ff, sr->p, &vs, so, hk, 0, &r);
  t = same(sr->p, sr->nw, sr->nn);
  if (mode == 4) {
    tf->checks++;
    if (rc != E_IO || r.failed_step != 3) tf_fail(tf, __FILE__, __LINE__, "%s: short write not caught (%s step %u)", ctx,
                                                  e_errname(rc), (u32)r.failed_step);
  }
  if (mode == 3) {
    // after a power loss: the target is old or new, or (between the two renames) missing with the old copy kept
    int ok;
    if (sc->existing) ok = t == 1 || same(sr->p, sr->od, sr->no) == 1 || any_file_equals(g_dir, sr->p, sr->od, sr->no);
    else ok = t == 1 || !exists(sr->p);
    tf->checks++;
    if (!ok) tf_fail(tf, __FILE__, __LINE__, "%s: target damaged after power loss", ctx);
  } else {
    const char* allowed[5];
    tf->checks++;
    if (rc == E_OK) {
      if (t != 1) tf_fail(tf, __FILE__, __LINE__, "%s: rc OK but target not new", ctx);
      if (r.temp[0]) tf_fail(tf, __FILE__, __LINE__, "%s: rc OK but temp named", ctx);
    } else if (sc->existing) {
      // DESIGN 13.3 step 6: only an in-place overwrite that fails and can't be undone may damage the target, and then
      // the old version is kept and named (checked below)
      int damaged_ok = mode == 2 && kind == FaultFs::F_COPY && r.in_place && r.old_copy[0] && r.temp[0];
      if (same(sr->p, sr->od, sr->no) != 1 && !damaged_ok)
        tf_fail(tf, __FILE__, __LINE__, "%s: rc %s, target not old", ctx, e_errname(rc));
    } else {
      if (exists(sr->p)) tf_fail(tf, __FILE__, __LINE__, "%s: rc %s, new target exists", ctx, e_errname(rc));
    }
    // every extra file is named, and named files are what they claim to be
    allowed[0] = sr->p; allowed[1] = sc->keep_bak ? sr->bak : ""; allowed[2] = r.temp; allowed[3] = r.old_copy;
    allowed[4] = r.stray;
    check_only(tf, g_dir, allowed, 5, ctx, __LINE__);
    tf->checks++;
    if (r.temp[0] && (!exists(r.temp) || (r.failed_step >= 4 && same(r.temp, sr->nw, sr->nn) != 1)))
      tf_fail(tf, __FILE__, __LINE__, "%s: named temp missing or incomplete", ctx);
    tf->checks++;
    if (r.old_copy[0] && same(r.old_copy, sr->od, sr->no) != 1)
      tf_fail(tf, __FILE__, __LINE__, "%s: named old copy is not the old content", ctx);
    tf->checks++;                                             // stray: may exist (left behind) or not (unknown)
    if (r.stray[0] && es_nicmp(base_of(r.stray), "~P", 2) != 0) tf_fail(tf, __FILE__, __LINE__, "%s: odd stray", ctx);
    tf->checks++;
    if (sc->keep_bak && exists(sr->bak) && same(sr->bak, sr->od, sr->no) != 1)
      tf_fail(tf, __FILE__, __LINE__, "%s: .bak is not the old content", ctx);
    if (sc->ondemand && hk) {
      // the source's original bytes are still somewhere it was told about
      tf->checks++;
      if (hk->nbefore != hk->nafter || hk->nafter > 1)
        tf_fail(tf, __FILE__, __LINE__, "%s: hooks before %u after %u", ctx, hk->nbefore, hk->nafter);
      else if (hk->nafter == 1 && same(hk->at, sr->od, sr->no) != 1)
        tf_fail(tf, __FILE__, __LINE__, "%s: after_replace(%s) does not hold the original", ctx, hk->at);
    }
  }
  ff->reset();
  delete hk;
  delete src;
  tf->checks++;
  if (open_count() != open0) tf_fail(tf, __FILE__, __LINE__, "%s: %u handles leaked", ctx, open_count() - open0);
  return rc;
}

static void t_save_faults(TF* tf)
{
  static const SaveScenario scs[] = {
    { "new", 0, 0, 0, 0 }, { "existing", 1, 0, 0, 0 }, { "bak", 1, 0, 1, 0 }, { "ondemand", 1, 1, 0, 0 },
    { "locked", 1, 0, 0, 1 }, { "ondemand-locked", 1, 1, 0, 1 }
  };
  FaultFs ff(fs_os());
  SaveRun sr;
  u32 s, k, e, nth, total, base[FaultFs::F_ALL_KINDS], runs = 0, failed0 = tf->failed;
  int mode, rc;
  char ctx[160];
  u8* od;
  u8* nw;
  sr.no = 3000;
  sr.nn = g_remote ? 3000 : 70000;                            // 70000: two write calls
  od = (u8*)e_valloc(sr.no);
  nw = (u8*)e_valloc(sr.nn);
  if (!od || !nw) { TF_CHECK(0); return; }
  gen_text(tf, od, sr.no);
  gen_text(tf, nw, sr.nn);
  sr.od = od; sr.nw = nw;
  pjoin(g_dir, "target.txt", sr.p, sizeof(sr.p));
  es_copy(sr.bak, sizeof(sr.bak), sr.p);
  es_cat(sr.bak, sizeof(sr.bak), ".bak");
  for (s = 0; s < sizeof(scs) / sizeof(scs[0]) && tf->failed - failed0 < 20; s++) {
    sr.sc = &scs[s];
    es_fmt(ctx, sizeof(ctx), "%s baseline", scs[s].name);
    // baseline: count the calls of each kind
    {
      FileSource* src = 0;
      SrcHooks* hk = 0;
      SaveOpts so;
      SaveResult r;
      ViewSource vs(nw, sr.nn);
      memset(&so, 0, sizeof(so));
      so.keep_bak = scs[s].keep_bak;
      wipe(g_dir);
      if (scs[s].existing) put(sr.p, od, sr.no);
      if (scs[s].ondemand) {
        epos av;
        src = new FileSource();
        src->open(&ff, sr.p, FS_READ | FS_DENY_WRITE, 4);
        src->get(0, &av);
        hk = new SrcHooks(src);
      }
      ff.reset();
      if (scs[s].locked) ff.set_locked(sr.p, 0);
      rc = file_save(&ff, sr.p, &vs, so, hk, 0, &r);
      for (k = 0; k < FaultFs::F_ALL_KINDS; k++) base[k] = ff.calls(k);
      total = ff.total_calls();
      ff.reset();
      delete hk;
      delete src;
      if (scs[s].ondemand && scs[s].locked) TF_EQ_I(rc, E_SHARING);
      else TF_EQ_I(rc, E_OK);
      if (r.old_copy[0]) fs_os()->remove(r.old_copy);
      if (r.temp[0]) fs_os()->remove(r.temp);
    }
    for (k = 0; k < sizeof(g_kinds) / sizeof(g_kinds[0]); k++) {
      const KindErrs& ke = g_kinds[k];
      u32 nerr = g_remote ? 1 : ke.n;
      for (e = 0; e < nerr; e++) {
        for (mode = 1; mode <= 2; mode++) {
          for (nth = 1; nth <= base[ke.kind]; nth++) {
            es_fmt(ctx, sizeof(ctx), "%s: %s #%u %s %s", scs[s].name, ke.name, nth, e_errname(ke.errs[e]),
                   mode == 1 ? "once" : "from then on");
            save_run(tf, &sr, &ff, mode, ke.kind, nth, ke.errs[e], ctx);
            runs++;
          }
        }
      }
    }
    for (nth = 0; nth <= total; nth++) {
      es_fmt(ctx, sizeof(ctx), "%s: power loss after %u calls", scs[s].name, nth);
      save_run(tf, &sr, &ff, 3, 0, nth, 0, ctx);
      runs++;
    }
    for (nth = 1; nth <= base[FaultFs::F_WRITE]; nth++) {
      es_fmt(ctx, sizeof(ctx), "%s: write #%u short", scs[s].name, nth);
      save_run(tf, &sr, &ff, 4, 0, nth, 0, ctx);
      runs++;
    }
    {
      static const int late_kinds[3] = { FaultFs::F_MOVE, FaultFs::F_REMOVE, FaultFs::F_COPY };
      static const char* const late_names[3] = { "move", "remove", "copy" };
      for (k = 0; k < 3; k++) {
        for (nth = 1; nth <= base[late_kinds[k]]; nth++) {
          es_fmt(ctx, sizeof(ctx), "%s: %s #%u done but reported E_IO", scs[s].name, late_names[k], nth);
          save_run(tf, &sr, &ff, 5, late_kinds[k], nth, E_IO, ctx);
          runs++;
        }
      }
    }
  }
  tf_log(tf, "save fault runs: %u", runs);
  e_vfree(od, sr.no);
  e_vfree(nw, sr.nn);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// A/B files

static int ab_val(const u8* p, u32 n, u32* seq)          // "AB<8 hex>" ... "\nEND\n"
{
  u32 i, v = 0;
  if (n < 15 || p[0] != 'A' || p[1] != 'B' || memcmp(p + n - 5, "\nEND\n", 5) != 0) return E_FORMAT;
  for (i = 2; i < 10; i++) {
    u8 c = p[i];
    int d = (c >= '0' && c <= '9') ? c - '0' : ((c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1);
    if (d < 0) return E_FORMAT;
    v = (v << 4) | (u32)d;
  }
  *seq = v;
  return E_OK;
}

static void make_ab(u32 seq, EBytes* out)
{
  char h[16];
  u32 i;
  out->clear();
  es_fmt(h, sizeof(h), "AB%08X", seq);
  out->append(h, 10);
  for (i = 0; i < 30 + seq % 7; i++) { u8 c = (u8)('a' + (seq + i) % 26); out->append(&c, 1); }
  out->append("\nEND\n", 5);
}

static void t_ab(TF* tf)
{
  FileSys* fs = fs_os();
  char p1[300], p2[300];
  EBytes b, cur, prev, got;
  int last = 0, which = 0, rc;
  u32 s, L, n, total;
  wipe(g_dir);
  pjoin(g_dir, "session1.ini", p1, sizeof(p1));
  pjoin(g_dir, "session2.ini", p2, sizeof(p2));
  TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_NOTFOUND);
  TF_EQ_I(which, 0);
  for (s = 1; s <= 5; s++) {
    make_ab(s, &b);
    TF_EQ_I(ab_write(fs, p1, p2, last, b.data(), b.size(), &which), E_OK);
    TF_EQ_I(which, last == 1 ? 2 : 1);
    last = which;
    TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_OK);
    TF_EQ_I(which, last);
    TF_CHECK(got.size() == b.size() && memcmp(got.data(), b.data(), b.size()) == 0);
  }
  // damage the newer one at every length: the older one is returned
  make_ab(5, &cur);
  make_ab(4, &prev);
  for (L = 0; L < cur.size(); L++) {
    put(last == 1 ? p1 : p2, cur.data(), L);
    rc = ab_read(fs, p1, p2, ab_val, &got, &which);
    tf->checks++;
    if (rc != E_OK || which == last || got.size() != prev.size() || memcmp(got.data(), prev.data(), prev.size()) != 0) {
      tf_fail(tf, __FILE__, __LINE__, "ab torn at %u: rc %s which %d", L, e_errname(rc), which);
      break;
    }
  }
  cur[cur.size() - 2] = 'X';                                  // same length, bad end marker
  put(last == 1 ? p1 : p2, cur.data(), cur.size());
  TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_OK);
  TF_EQ_I(which, last == 1 ? 2 : 1);
  // one missing / both bad
  fs->remove(last == 1 ? p1 : p2);
  TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_OK);
  TF_EQ_I(which, last == 1 ? 2 : 1);
  put(p1, (const u8*)"zz", 2);
  put(p2, (const u8*)"", 0);
  TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_FORMAT);
  TF_EQ_I(which, 0);
  // a failed write leaves the newest copy alone
  make_ab(1, &b); put(p1, b.data(), b.size());
  make_ab(2, &b); put(p2, b.data(), b.size());
  {
    FaultFs ff(fs);
    make_ab(3, &b);
    ff.set_torn_writes(1);
    ff.fail_on(FaultFs::F_WRITE, 1, E_DISKFULL);
    which = 7;
    TF_EQ_I(ab_write(&ff, p1, p2, 2, b.data(), b.size(), &which), E_DISKFULL);
    TF_EQ_I(which, 2);
    TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_OK);
    TF_EQ_I(which, 2);
    ff.reset();
    ff.short_write_on(1);
    TF_EQ_I(ab_write(&ff, p1, p2, 2, b.data(), b.size(), &which), E_IO);
    TF_EQ_I(which, 2);
    TF_EQ_I(ab_read(fs, p1, p2, ab_val, &got, &which), E_OK);
    TF_EQ_I(which, 2);
    // power loss after every call of a write: the result is always the old newest or the new one
    ff.reset();
    TF_EQ_I(ab_write(&ff, p1, p2, 2, b.data(), b.size(), &which), E_OK);
    total = ff.total_calls();
    for (n = 0; n <= total; n++) {
      u32 seq = 0;
      make_ab(1, &b); put(p1, b.data(), b.size());
      make_ab(2, &b); put(p2, b.data(), b.size());
      make_ab(3, &b);
      ff.reset();
      ff.crash_after(n);
      ff.set_torn_writes(1);
      ab_write(&ff, p1, p2, 2, b.data(), b.size(), &which);
      ff.reset();
      rc = ab_read(fs, p1, p2, ab_val, &got, &which);
      if (rc == E_OK) ab_val(got.data(), got.size(), &seq);
      TF_CHECK(rc == E_OK && (seq == 2 || seq == 3));
    }
  }
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------
// Backups

static void fill_meta(BackupMeta* m, u32 tabid, u32 gen, u32 seq)
{
  memset(m, 0, sizeof(*m));
  m->tabid = tabid; m->gen = gen; m->seq = seq;
  es_copy(m->path, sizeof(m->path), "C:\\My Documents\\x y \xE9.txt");
  m->untitled = 0; m->eol = EOL_LF; m->untitled_no = 3;
  m->enc = enc_utf8(1);
  es_copy(m->lang, sizeof(m->lang), "cpp");
  m->caret = 1234; m->anchor = 99;
  m->disk_size = 12345; m->disk_mtime_hi = 0x01D9F1A2u; m->disk_mtime_lo = 0xB3C4D5E6u;
  m->written_hi = 0xFFFFFFFFu; m->written_lo = 1;
}

static int meta_eq(const BackupMeta& a, const BackupMeta& b)
{
  return a.tabid == b.tabid && a.gen == b.gen && a.seq == b.seq && strcmp(a.path, b.path) == 0 &&
         a.untitled == b.untitled && a.eol == b.eol && a.untitled_no == b.untitled_no && enc_equal(a.enc, b.enc) &&
         strcmp(a.lang, b.lang) == 0 && a.caret == b.caret && a.anchor == b.anchor && a.disk_size == b.disk_size &&
         a.disk_mtime_lo == b.disk_mtime_lo && a.disk_mtime_hi == b.disk_mtime_hi &&
         a.written_lo == b.written_lo && a.written_hi == b.written_hi;
}

static void t_backup_header(TF* tf)
{
  BackupMeta m, q;
  EStr h;
  u32 hl = 0, L;
  epos dl = 0;
  char name[13];
  static const char head[] = "PICABAK1\r\ntabid=5\r\ngen=7\r\nseq=42\r\npath=";
  static const char tail[] = "\r\n@DATA 15000\r\n";
  static const char mini[] = "PICABAK1\r\ngen=1\r\ntabid=2\r\nfuture=x\r\nenc=klingon\r\n@DATA 0\r\n";
  static const char notab[] = "PICABAK1\r\ngen=1\r\n@DATA 0\r\n";
  static const char badnum[] = "PICABAK1\r\ntabid=x\r\ngen=1\r\n@DATA 0\r\n";
  static const char badver[] = "PICABAK2\r\ntabid=1\r\ngen=1\r\n@DATA 0\r\n";
  static const char badtime[] = "PICABAK1\r\ntabid=1\r\ngen=1\r\nwritten=12\r\n@DATA 0\r\n";
  fill_meta(&m, 5, 7, 42);
  TF_EQ_I(backup_header_write(m, 15000, &h), E_OK);
  TF_CHECK(h.len() > sizeof(head) && memcmp(h.c_str(), head, sizeof(head) - 1) == 0);
  TF_CHECK(memcmp(h.c_str() + h.len() - (sizeof(tail) - 1), tail, sizeof(tail) - 1) == 0);
  TF_CHECK(strstr(h.c_str(), "\r\nenc=utf8bom\r\neol=lf\r\nlang=cpp\r\n") != 0);
  TF_CHECK(strstr(h.c_str(), "\r\ndisk_mtime=01D9F1A2B3C4D5E6\r\nwritten=FFFFFFFF00000001\r\n") != 0);
  TF_EQ_I(backup_header_parse((const u8*)h.c_str(), h.len(), &q, &hl, &dl), E_OK);
  TF_EQ_U(hl, h.len()); TF_EQ_U(dl, 15000);
  TF_CHECK(meta_eq(m, q));
  // truncated anywhere: refused
  for (L = 0; L < h.len(); L++) {
    if (backup_header_parse((const u8*)h.c_str(), L, &q, &hl, &dl) == E_OK) {
      tf_fail(tf, __FILE__, __LINE__, "header truncated at %u parsed", L);
      break;
    }
  }
  TF_CHECK(L == h.len());
  // untitled, empty path, other eol
  fill_meta(&m, 0xFFFFF, 0xFF, 0);
  m.untitled = 1; m.path[0] = 0; m.eol = EOL_CR; m.enc = enc_utf16be(1); m.lang[0] = 0;
  TF_EQ_I(backup_header_write(m, 0, &h), E_OK);
  TF_EQ_I(backup_header_parse((const u8*)h.c_str(), h.len(), &q, &hl, &dl), E_OK);
  TF_CHECK(meta_eq(m, q)); TF_EQ_U(dl, 0);
  m.eol = EOL_CRLF;
  TF_EQ_I(backup_header_write(m, 0, &h), E_OK);
  TF_EQ_I(backup_header_parse((const u8*)h.c_str(), h.len(), &q, &hl, &dl), E_OK);
  TF_CHECK(meta_eq(m, q));
  es_copy(m.path, sizeof(m.path), "a\nb");
  TF_EQ_I(backup_header_write(m, 0, &h), E_FORMAT);
  // tolerant of unknown keys and encodings; strict about structure
  TF_EQ_I(backup_header_parse((const u8*)mini, sizeof(mini) - 1, &q, &hl, &dl), E_OK);
  TF_EQ_U(q.tabid, 2); TF_EQ_U(q.gen, 1); TF_CHECK(enc_equal(q.enc, enc_ansi())); TF_EQ_U(hl, sizeof(mini) - 1);
  TF_EQ_I(backup_header_parse((const u8*)notab, sizeof(notab) - 1, &q, &hl, &dl), E_FORMAT);
  TF_EQ_I(backup_header_parse((const u8*)badnum, sizeof(badnum) - 1, &q, &hl, &dl), E_FORMAT);
  TF_EQ_I(backup_header_parse((const u8*)badver, sizeof(badver) - 1, &q, &hl, &dl), E_FORMAT);
  TF_EQ_I(backup_header_parse((const u8*)badtime, sizeof(badtime) - 1, &q, &hl, &dl), E_FORMAT);
  {
    char longl[600];
    u32 i;
    es_copy(longl, sizeof(longl), "PICABAK1\r\ntabid=1\r\ngen=1\r\npath=");
    for (i = es_len(longl); i < 500; i++) longl[i] = 'x';
    longl[i] = 0;
    es_cat(longl, sizeof(longl), "\r\n@DATA 0\r\n");
    TF_EQ_I(backup_header_parse((const u8*)longl, es_len(longl), &q, &hl, &dl), E_FORMAT);
  }
  backup_file_name(0x2A, 7, name);
  TF_EQ_S(name, "T0002A07.BAK");
  backup_file_name(0xFFFFF, 0xFF, name);
  TF_EQ_S(name, "TFFFFFFF.BAK");
  backup_file_name(1, 0x10, name);
  TF_EQ_S(name, "T0000110.BAK");
  TF_EQ_U(es_len(BACKUP_TRAILER), 17);
}

static int list_has(const Vec<BackupMeta>& v, u32 tabid, u32* gen)
{
  u32 i;
  for (i = 0; i < v.size(); i++) if (v[i].tabid == tabid) { *gen = v[i].gen; return 1; }
  return 0;
}

static int backup_data_is(const char* dir, const BackupMeta& m, const u8* p, u32 n)
{
  ByteSource* s = 0;
  int ok;
  if (backup_open(fs_os(), dir, m, &s) != E_OK || !s) return 0;
  ok = src_equals(s, p, n);
  delete s;
  return ok;
}

static void t_backup(TF* tf)
{
  FileSys* fs = fs_os();
  char bdir[300], f[300], name[13];
  BackupMeta m, m2;
  Vec<BackupMeta> list;
  Vec<Nm> names;
  EBytes raw;
  u8 d1[40], d2[57];
  u32 g = 0, L, total, nth, k, e;
  int rc, mode;
  volatile i32 cancel = 1;
  gen_text(tf, d1, sizeof(d1));
  gen_text(tf, d2, sizeof(d2));
  wipe(g_dir);
  wipe(g_dir2);
  pjoin(g_dir, "backup", bdir, sizeof(bdir));
  {
    ViewSource v1(d1, sizeof(d1)), v2(d2, sizeof(d2)), v0(d1, 0);
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);              // missing folder: empty
    TF_EQ_U(list.size(), 0);
    fill_meta(&m, 5, 1, 10);
    TF_EQ_I(backup_write(fs, bdir, m, &v1, 0, 0), E_OK);      // creates the folder
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_EQ_U(list.size(), 1);
    if (list.size() == 1) { TF_CHECK(meta_eq(list[0], m)); TF_CHECK(backup_data_is(bdir, list[0], d1, sizeof(d1))); }
    fill_meta(&m, 5, 2, 11);
    TF_EQ_I(backup_write(fs, bdir, m, &v2, 0, 0), E_OK);
    list_dir(bdir, &names);
    TF_EQ_U(names.size(), 1);                                 // generation 1 deleted
    if (names.size() == 1) TF_EQ_S(names[0].s, "T0000502.BAK");
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && list[0].gen == 2);
    TF_EQ_I(backup_write(fs, bdir, m, &v1, 0, 0), E_EXISTS); // never overwrite a valid backup
    TF_CHECK(backup_data_is(bdir, m, d2, sizeof(d2)));
    fill_meta(&m2, 7, 1, 3);
    TF_EQ_I(backup_write(fs, bdir, m2, &v0, 0, 0), E_OK);    // empty data is fine
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_EQ_U(list.size(), 2);
    if (list.size() == 2) { TF_EQ_U(list[0].tabid, 5); TF_EQ_U(list[1].tabid, 7); }
    TF_CHECK(backup_data_is(bdir, m2, d1, 0));
    fill_meta(&m2, 7, 2, 4);
    TF_EQ_I(backup_write(fs, bdir, m2, &v1, 0, &cancel), E_CANCEL);
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list_has(list, 7, &g) && g == 1);
    list_dir(bdir, &names);
    TF_EQ_U(names.size(), 2);
    TF_EQ_I(backup_delete_tab(fs, bdir, 5), E_OK);
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && list[0].tabid == 7);
    TF_EQ_I(backup_delete_tab(fs, bdir, 7), E_OK);
    TF_EQ_I(backup_delete_tab(fs, bdir, 7), E_OK);
    list_dir(bdir, &names);
    TF_EQ_U(names.size(), 0);
    fill_meta(&m, 0x100000, 1, 1);
    TF_EQ_I(backup_write(fs, bdir, m, &v1, 0, 0), E_RANGE);
    {
      ByteSource* none = (ByteSource*)1;
      TF_EQ_I(backup_open(fs, bdir, m2, &none), E_NOTFOUND);
      TF_CHECK(none == 0);
    }

    // torn newest generation: the previous one is returned
    fill_meta(&m, 9, 5, 20);
    TF_EQ_I(backup_write(fs, bdir, m, &v1, 0, 0), E_OK);
    fill_meta(&m2, 9, 6, 21);
    TF_EQ_I(backup_write(fs, g_dir2, m2, &v2, 0, 0), E_OK);
    backup_file_name(9, 6, name);
    pjoin(g_dir2, name, f, sizeof(f));
    TF_EQ_I(file_read_all(fs, f, &raw, 100000), E_OK);
    pjoin(bdir, name, f, sizeof(f));
    for (L = 0; L < raw.size(); L++) {
      put(f, raw.data(), L);
      rc = backup_list(fs, bdir, &list);
      tf->checks++;
      if (rc != E_OK || list.size() != 1 || list[0].gen != 5) {
        tf_fail(tf, __FILE__, __LINE__, "backup torn at %u: rc %s n %u", L, e_errname(rc), list.size());
        break;
      }
    }
    TF_CHECK(backup_data_is(bdir, m, d1, sizeof(d1)));
    put(f, raw.data(), raw.size());                          // complete: the newer one wins
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && list[0].gen == 6);
    raw[raw.size() - 3] ^= 1;                                // trailer damaged
    put(f, raw.data(), raw.size());
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && list[0].gen == 5);
    raw[raw.size() - 3] ^= 1;
    raw[30] ^= 0x20;                                         // header damaged (gen=6 -> other text) or a data byte
    put(f, raw.data(), raw.size());
    raw[30] ^= 0x20;
    backup_file_name(9, 7, name);                            // the gen-6 file under a gen-7 name: foreign, ignored
    pjoin(bdir, name, f, sizeof(f));
    put(f, raw.data(), raw.size());
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && (list[0].gen == 5 || list[0].gen == 6));
    // generation wrap and seq order
    wipe(bdir);
    wipe(g_dir2);
    fill_meta(&m, 4, 0xFF, 1);
    TF_EQ_I(backup_write(fs, bdir, m, &v1, 0, 0), E_OK);
    fill_meta(&m, 4, 0x00, 1);
    TF_EQ_I(backup_write(fs, g_dir2, m, &v2, 0, 0), E_OK);
    backup_file_name(4, 0, name);
    pjoin(g_dir2, name, f, sizeof(f));
    {
      char to[300];
      pjoin(bdir, name, to, sizeof(to));
      TF_EQ_I(fs->copy(f, to), E_OK);
    }
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && list[0].gen == 0);          // 0x00 follows 0xFF
    fill_meta(&m, 4, 0x80, 5);
    TF_EQ_I(backup_write(fs, g_dir2, m, &v1, 0, 0), E_OK);
    backup_file_name(4, 0x80, name);
    pjoin(g_dir2, name, f, sizeof(f));
    {
      char to[300];
      pjoin(bdir, name, to, sizeof(to));
      TF_EQ_I(fs->copy(f, to), E_OK);
    }
    TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
    TF_CHECK(list.size() == 1 && list[0].gen == 0x80);       // higher seq wins

    // fault injection and power loss while writing generation 2 over a valid generation 1
    {
      FaultFs ff(fs);
      for (nth = 1; nth <= 3; nth++) {                        // a write that silently loses data is caught
        wipe(bdir);
        fill_meta(&m, 3, 1, 1);
        backup_write(fs, bdir, m, &v1, 0, 0);
        fill_meta(&m2, 3, 2, 2);
        ff.reset();
        ff.short_write_on(nth);
        TF_EQ_I(backup_write(&ff, bdir, m2, &v2, 0, 0), E_IO);
        ff.reset();
        TF_EQ_I(backup_list(fs, bdir, &list), E_OK);
        TF_CHECK(list_has(list, 3, &g) && g == 1);
      }
      for (mode = 0; mode <= 3; mode++) {
        u32 nk = mode == 3 ? 1 : (u32)(sizeof(g_kinds) / sizeof(g_kinds[0]));
        for (k = 0; k < nk; k++) {
          u32 nerr = (mode == 0 || mode == 3) ? 1 : (g_remote ? 1 : g_kinds[k].n);
          for (e = 0; e < nerr; e++) {
            wipe(bdir);
            fill_meta(&m, 3, 1, 1);
            backup_write(fs, bdir, m, &v1, 0, 0);
            fill_meta(&m2, 3, 2, 2);
            ff.reset();
            backup_write(&ff, bdir, m2, &v2, 0, 0);
            total = ff.total_calls();
            if (mode == 0) { ff.reset(); continue; }
            for (nth = mode == 3 ? 0 : 1; nth <= (mode == 3 ? total : ff.calls(g_kinds[k].kind)); nth++) {
              wipe(bdir);
              backup_write(fs, bdir, m, &v1, 0, 0);
              ff.reset();
              ff.set_torn_writes(1);
              if (mode == 1) ff.fail_on(g_kinds[k].kind, nth, g_kinds[k].errs[e]);
              else if (mode == 2) ff.fail_from(g_kinds[k].kind, nth, g_kinds[k].errs[e]);
              else ff.crash_after(nth);
              rc = backup_write(&ff, bdir, m2, &v2, 0, 0);
              ff.reset();
              list.clear();
              tf->checks++;
              if (backup_list(fs, bdir, &list) != E_OK || !list_has(list, 3, &g)) {
                tf_fail(tf, __FILE__, __LINE__, "backup fault mode %d %s #%u: no valid backup left", mode,
                        g_kinds[k].name, nth);
                continue;
              }
              tf->checks++;
              if ((rc == E_OK && g != 2) || (g == 1 && !backup_data_is(bdir, m, d1, sizeof(d1))) ||
                  (g == 2 && !backup_data_is(bdir, m2, d2, sizeof(d2))) || (g != 1 && g != 2))
                tf_fail(tf, __FILE__, __LINE__, "backup fault mode %d %s #%u: rc %s gen %u", mode, g_kinds[k].name,
                        nth, e_errname(rc), g);
            }
          }
        }
      }
    }
  }
  wipe(g_dir);
  wipe(g_dir2);
}

// ---------------------------------------------------------------------------------------------------------------
// Timings (printed; VC6 on C: 10 MB and on M: 1 MB, DESIGN 3.1: C: write 6.2 MB/s, M: 0.4 MB/s)

static long ms_since(clock_t c0) { return (long)((clock() - c0) * 1000 / CLOCKS_PER_SEC); }

static void t_timing(TF* tf)
{
  FileSys* fs = fs_os();
  char p[300];
  u32 n = g_remote ? 1024 * 1024 : 10 * 1024 * 1024, i;
  u8* d = (u8*)e_valloc(n);
  SaveOpts so;
  SaveResult r;
  LoadOpts o;
  LoadResult lr;
  IndexResult ir;
  BreakSpec lf = { 1, 0x0A, 0, 0 };
  clock_t c0;
  long t_new, t_rep, t_load, t_index;
  if (!d) { TF_CHECK(0); return; }
  for (i = 0; i < n; i++) d[i] = (u8)((i % 62) == 60 ? 0x0D : ((i % 62) == 61 ? 0x0A : 'a' + i % 23));
  memset(&so, 0, sizeof(so));
  wipe(g_dir);
  pjoin(g_dir, "timing.txt", p, sizeof(p));
  {
    ViewSource vs(d, n);
    c0 = clock();
    TF_EQ_I(file_save(fs, p, &vs, so, 0, 0, &r), E_OK);
    t_new = ms_since(c0);
    c0 = clock();
    TF_EQ_I(file_save(fs, p, &vs, so, 0, 0, &r), E_OK);
    t_rep = ms_since(c0);
  }
  load_opts(&o, 16 * 1024 * 1024);
  c0 = clock();
  TF_EQ_I(file_load(fs, p, o, 0, &lr), E_OK);
  t_load = ms_since(c0);
  TF_CHECK(lr.src && lr.src->size() == n);
  if (lr.src) delete lr.src;
  c0 = clock();
  TF_EQ_I(file_index(fs, p, lf, &ir, 0, 0), E_OK);
  t_index = ms_since(c0);
  TF_EQ_U(ir.eol.crlf, n / 62);
  printf("  fileops timing (%s, %u KB): save new %ld ms, save over existing %ld ms, load %ld ms, index %ld ms\n",
         g_remote ? "network drive" : "local disk", n / 1024, t_new, t_rep, t_load, t_index);
  e_vfree(d, n);
  wipe(g_dir);
}

// ---------------------------------------------------------------------------------------------------------------

void suite_fileops(TF* tf)
{
  FileSys* fs = fs_os();
  u32 open0 = open_count();
  enc_set_system(1252, 437);
  pjoin(tf->tmp_dir, "fo", g_dir, sizeof(g_dir));
  pjoin(tf->tmp_dir, "fo2", g_dir2, sizeof(g_dir2));
  fs->mkdir(g_dir);
  fs->mkdir(g_dir2);
  g_remote = fs->is_remote(g_dir);
  wipe(g_dir);
  wipe(g_dir2);
  t_fsys(tf);            TF_EQ_U(open_count(), open0);
  t_readall(tf);         TF_EQ_U(open_count(), open0);
  t_filesource(tf);      TF_EQ_U(open_count(), open0);
  t_load(tf);            TF_EQ_U(open_count(), open0);
  t_index(tf);           TF_EQ_U(open_count(), open0);
  t_save_basic(tf);      TF_EQ_U(open_count(), open0);
  t_save_ondemand(tf);   TF_EQ_U(open_count(), open0);
  t_save_faults(tf);     TF_EQ_U(open_count(), open0);
  t_ab(tf);              TF_EQ_U(open_count(), open0);
  t_backup_header(tf);
  t_backup(tf);          TF_EQ_U(open_count(), open0);
  t_timing(tf);          TF_EQ_U(open_count(), open0);
  wipe(g_dir);
  wipe(g_dir2);
  fs->remove(g_dir);
  fs->remove(g_dir2);
}
