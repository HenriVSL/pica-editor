// t_fif.cpp - suite "fif" (T11): find-in-files core. Classes carry a Fift prefix (coretest links all suites into one
// binary; same-named test classes in two files collide).
// Covers: masks and folder excludes, the walk over a fixture tree (order, hidden/system folders, binary skip, every
// encoding, CR mode), hit fields (line, col, pos, text, highlight), long-line windows, regex/extended/whole word/case,
// empty matches, multi-line matches, max hits, cancel and stop, missing folder, bad pattern, the windowed path for big
// files vs the in-memory path, random texts vs a reference over flat, chunked and UTF-16 sources, OOM.
#include "tf.h"
#include "fif.h"
#include "fif_int.h"
#include "codec.h"
#include "detect.h"
#include <string.h>

#if defined(_WIN32)
static const char k_sep[] = "\\";
#else
static const char k_sep[] = "/";
#endif

// ---------------------------------------------------------------------------------------------------------------
// fixtures

struct FiftHit { char path[300]; FifHit h; };

class FiftSink : public FifSink {
public:
  Vec<FiftHit*> hits;
  u32 nfiles, nprogress, nskip, ndone, cancel_at_progress, stop_at_hit;
  int result; u32 searched, fwh, total;
  char cur[300]; char last_skip[300]; int last_skip_err;
  FiftSink() : nfiles(0), nprogress(0), nskip(0), ndone(0), cancel_at_progress(0), stop_at_hit(0), result(1),
               searched(0), fwh(0), total(0), last_skip_err(0) { cur[0] = 0; last_skip[0] = 0; }
  ~FiftSink() { clear(); }
  void clear()
  {
    u32 i;
    for (i = 0; i < hits.size(); i++) e_free(hits[i]);
    hits.release();
    nfiles = nprogress = nskip = ndone = 0; result = 1; searched = fwh = total = 0; cur[0] = 0;
  }
  int on_file(const char* path, u32 index) { (void)index; nfiles++; es_copy(cur, sizeof(cur), path); return 0; }
  int on_hit(const FifHit& h)
  {
    FiftHit* x = (FiftHit*)e_alloc(sizeof(FiftHit));
    if (!x) return 1;
    es_copy(x->path, sizeof(x->path), cur);
    x->h = h;
    if (hits.push(x) != E_OK) { e_free(x); return 1; }
    return stop_at_hit && hits.size() >= stop_at_hit;
  }
  int on_progress(u32 done, u32 seen, const char* current)
  {
    (void)done; (void)seen; (void)current;
    nprogress++;
    return cancel_at_progress && nprogress >= cancel_at_progress;
  }
  void on_done(int r, u32 s, u32 f, u32 h) { ndone++; result = r; searched = s; fwh = f; total = h; }
  void on_skip(const char* path, int err) { nskip++; es_copy(last_skip, sizeof(last_skip), path); last_skip_err = err; }
};

// Forwards to fs_os(); entries named "_hid*" get FSA_HIDDEN and "_sys*" FSA_SYSTEM (the Mac has no such attributes).
class FiftHideFs : public FileSys {
public:
  FileSys* in;
  FiftHideFs() : in(fs_os()) {}
  int open(const char* p, u32 m, FsFile* o) { return in->open(p, m, o); }
  int read_at(FsFile f, u32 lo, u32 hi, void* b, u32 n, u32* g) { return in->read_at(f, lo, hi, b, n, g); }
  int write(FsFile f, const void* b, u32 n) { return in->write(f, b, n); }
  int flush(FsFile f) { return in->flush(f); }
  int size(FsFile f, u32* lo, u32* hi) { return in->size(f, lo, hi); }
  int close(FsFile f) { return in->close(f); }
  int stat(const char* p, FsStat* s) { return in->stat(p, s); }
  int set_attrs(const char* p, u32 a) { return in->set_attrs(p, a); }
  int set_ctime(const char* p, u32 lo, u32 hi) { return in->set_ctime(p, lo, hi); }
  int move(const char* a, const char* b) { return in->move(a, b); }
  int remove(const char* p) { return in->remove(p); }
  int copy(const char* a, const char* b) { return in->copy(a, b); }
  int make_temp(const char* d, const char* p, char* o, u32 c) { return in->make_temp(d, p, o, c); }
  int mkdir(const char* p) { return in->mkdir(p); }
  int free_space(const char* d, u64* b) { return in->free_space(d, b); }
  int find_first(const char* d, FsFind* f) { int r = in->find_first(d, f); mark(r, f); return r; }
  int find_next(FsFind* f) { int r = in->find_next(f); mark(r, f); return r; }
  void find_close(FsFind* f) { in->find_close(f); }
  int is_remote(const char* p) { return in->is_remote(p); }
private:
  static void mark(int r, FsFind* f)
  {
    if (r != E_OK) return;
    if (es_nicmp(f->name, "_hid", 4) == 0) f->attrs |= FSA_HIDDEN;
    if (es_nicmp(f->name, "_sys", 4) == 0) f->attrs |= FSA_SYSTEM;
  }
};

// Spans end at offsets where a hash says so (1-7 bytes apart): exercises units and characters split between spans.
class FiftChunkSrc : public ByteSource {
public:
  FiftChunkSrc(const u8* p, epos n, u32 salt) : p_(p), n_(n), salt_(salt) {}
  epos size() const { return n_; }
  const u8* get(epos off, epos* avail)
  {
    epos end = off + 1;
    if (off >= n_) { *avail = 0; return 0; }
    while (end < n_ && ((end * 2654435761u) ^ salt_) % 5 != 0) end++;
    *avail = end - off;
    return p_ + off;
  }
private:
  const u8* p_; epos n_; u32 salt_;
};

static void fpath(TF* tf, const char* rel, char* out, u32 cap)
{
  char r2[300];
  u32 i;
  es_copy(r2, sizeof(r2), rel);
  for (i = 0; r2[i]; i++) if (r2[i] == '/') r2[i] = k_sep[0];
  tf_path(tf, r2, out, cap);
}

static void put_file(TF* tf, const char* rel, const void* p, u32 n)
{
  char path[300];
  FsFile f;
  int rc;
  fpath(tf, rel, path, sizeof(path));
  rc = fs_os()->open(path, FS_WRITE, &f);
  TF_EQ_I(rc, E_OK);
  if (rc != E_OK) return;
  if (n) TF_EQ_I(fs_os()->write(f, p, n), E_OK);
  fs_os()->close(f);
}

static void put_str(TF* tf, const char* rel, const char* s) { put_file(tf, rel, s, es_len(s)); }

static void mk(TF* tf, const char* rel)
{
  char path[300];
  fpath(tf, rel, path, sizeof(path));
  fs_os()->mkdir(path);   // exists from an earlier run: fine
}

static void query(TF* tf, FifQuery* q, const char* rel, const char* masks, const char* pat, u8 mode, u8 mcase)
{
  Vec<ecp> v;
  u32 i;
  memset(q, 0, sizeof(*q));
  fpath(tf, rel, q->folder, sizeof(q->folder));
  es_copy(q->masks, sizeof(q->masks), masks);
  q->subfolders = 1;
  q->opts.mode = mode; q->opts.match_case = mcase; q->opts.whole_word = 0; q->opts.dot_all = 0;
  es_utf8_to_ecp(pat, es_len(pat), &v);
  for (i = 0; i < v.size() && i < RX_MAX_PATTERN; i++) q->pattern[i] = v[i];
  q->pattern_len = i;
  q->det.utf8_nobom = 1; q->det.utf16_nobom = 1; q->det.ansi = enc_cp(1252); q->det.default_eol = EOL_CRLF;
  q->max_hits = 0;
}

static const char* base_name(const char* p)
{
  const char* b = p;
  for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}

static void make_tree(TF* tf)
{
  static const u8 u16le[] = { 0xFF, 0xFE, 'x', 0, ' ', 0, 'f', 0, 'o', 0, 'o', 0, '\r', 0, '\n', 0,
                              'f', 0, 'o', 0, 'o', 0, '\r', 0, '\n', 0 };
  static const u8 u16be[] = { 0xFE, 0xFF, 0, 'f', 0, 'o', 0, 'o', 0, '\n' };
  static const u8 dobj[] = { 0, 1, 'f', 'o', 'o', 0 };
  u8 ebin[5004];
  memset(ebin, 'x', 5000);
  ebin[5000] = 0; ebin[5001] = 'f'; ebin[5002] = 'o'; ebin[5003] = 'o';
  mk(tf, "fif1");
  mk(tf, "fif1/sub");
  mk(tf, "fif1/sub/deep");
  mk(tf, "fif1/_hidden");
  mk(tf, "fif1/_sysdir");
  mk(tf, "fif1/skipme");
  mk(tf, "fif1/empty");
  put_str(tf, "fif1/a.cpp", "int main()\r\n{\r\n  foo(1);\r\n  return foo(2) + foo(3);\r\n}\r\n");
  put_str(tf, "fif1/b.h", "#define FOO 1\nvoid foo(int);\n");
  put_str(tf, "fif1/c.txt", "no match here\n");
  put_str(tf, "fif1/cr.txt", "a\rb foo\rc\r");
  put_file(tf, "fif1/d.obj", dobj, sizeof(dobj));
  put_file(tf, "fif1/e.bin", ebin, sizeof(ebin));
  put_str(tf, "fif1/Makefile", "foo: bar\n");
  put_file(tf, "fif1/u16be.txt", u16be, sizeof(u16be));
  put_file(tf, "fif1/u16le.txt", u16le, sizeof(u16le));
  put_str(tf, "fif1/u8.txt", "\xEF\xBB\xBF" "caf\xC3\xA9 foo \xE2\x82\xAC\n");
  put_str(tf, "fif1/zero.txt", "");
  put_str(tf, "fif1/sub/s1.cpp", "foo\n");
  put_str(tf, "fif1/sub/w.txt", "foobar foo xfoo foo_ foo.\n");
  put_str(tf, "fif1/sub/deep/s2.h", "  foo foo\n");
  put_str(tf, "fif1/_hidden/h.cpp", "foo\n");
  put_str(tf, "fif1/_sysdir/y.cpp", "foo\n");
  put_str(tf, "fif1/skipme/k.cpp", "foo\n");
}

// ---------------------------------------------------------------------------------------------------------------
// tests

static void test_masks(TF* tf)
{
  struct M { const char* masks; const char* name; int want; };
  static const M k[] = {
    { "*.cpp;*.h", "a.cpp", 1 }, { "*.cpp;*.h", "A.CPP", 1 }, { "*.cpp;*.h", "b.h", 1 }, { "*.cpp;*.h", "b.hpp", 0 },
    { "*.cpp *.h", "x.h", 1 }, { "*.cpp, *.rc", "r.rc", 1 }, { "", "anything", 1 }, { "  ", "x", 1 },
    { "*.*", "Makefile", 1 }, { "*", "Makefile", 1 }, { "!*.obj", "a.obj", 0 }, { "!*.obj", "a.cpp", 1 },
    { "*.* !*.obj", "x.obj", 0 }, { "*.cpp !test*.cpp", "test1.cpp", 0 }, { "*.cpp !test*.cpp", "main.cpp", 1 },
    { "Makefile", "makefile", 1 }, { "Makefile", "Makefile.vc", 0 }, { "makefile.*", "Makefile", 1 },
    { "?.c", "a.c", 1 }, { "?.c", "ab.c", 0 }, { "!\\sub", "x.cpp", 1 }, { "*.cpp !\\sub", "sub", 0 },
    { "!", "x", 1 }, { ";;*.h;;", "q.h", 1 }, { "*.h\t*.c", "q.c", 1 }
  };
  u32 i;
  for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
    int got = fif_mask_match(k[i].masks, k[i].name);
    if (got != k[i].want) tf_fail(tf, __FILE__, __LINE__, "mask '%s' name '%s': got %d", k[i].masks, k[i].name, got);
    else TF_CHECK(1);
  }
  TF_EQ_I(fif_dir_excluded("*.cpp !\\sub", "SUB"), 1);
  TF_EQ_I(fif_dir_excluded("*.cpp !/b*", "build"), 1);
  TF_EQ_I(fif_dir_excluded("*.cpp !*.obj", "obj"), 0);
  TF_EQ_I(fif_dir_excluded("", "x"), 0);
  TF_EQ_I(fif_mask_match(0, "x"), 1);
  TF_EQ_I(fif_mask_match("*", 0), 0);
}

struct FiftWant { const char* file; u32 line, col; epos pos; const char* text; u32 hl_start, hl_len; };

static void check_hits(TF* tf, FiftSink& s, const FiftWant* w, u32 n, int line)
{
  u32 i;
  if (s.hits.size() != n) tf_fail(tf, __FILE__, line, "hit count %u, want %u", s.hits.size(), n);
  for (i = 0; i < n && i < s.hits.size(); i++) {
    const FifHit& h = s.hits[i]->h;
    const char* bn = base_name(s.hits[i]->path);
    int ok = es_icmp(bn, w[i].file) == 0 && h.line == w[i].line && h.col == w[i].col && h.pos == w[i].pos &&
             strcmp(h.text, w[i].text) == 0 && h.text_len == es_len(w[i].text) && h.hl_start == w[i].hl_start &&
             h.hl_len == w[i].hl_len;
    if (!ok)
      tf_fail(tf, __FILE__, line, "hit %u: got %s %u:%u pos %u '%s' hl %u+%u; want %s %u:%u pos %u '%s' hl %u+%u", i,
              bn, h.line, h.col, h.pos, h.text, h.hl_start, h.hl_len, w[i].file, w[i].line, w[i].col, w[i].pos,
              w[i].text, w[i].hl_start, w[i].hl_len);
    else TF_CHECK(1);
  }
}

static void test_tree(TF* tf)
{
  static char ebin_text[64];
  static FiftWant want[] = {
    { "a.cpp", 2, 2, 17, "  foo(1);", 2, 3 },
    { "a.cpp", 3, 9, 35, "  return foo(2) + foo(3);", 9, 3 },
    { "a.cpp", 3, 18, 44, "  return foo(2) + foo(3);", 18, 3 },
    { "b.h", 1, 5, 19, "void foo(int);", 5, 3 },
    { "cr.txt", 1, 2, 4, "b foo", 2, 3 },
    { "e.bin", 0, 5001, 5001, 0, 43, 3 },
    { "Makefile", 0, 0, 0, "foo: bar", 0, 3 },
    { "u16be.txt", 0, 0, 2, "foo", 0, 3 },
    { "u16le.txt", 0, 2, 6, "x foo", 2, 3 },
    { "u16le.txt", 1, 0, 16, "foo", 0, 3 },
    { "u8.txt", 0, 5, 9, "caf\xC3\xA9 foo \xE2\x82\xAC", 5, 3 },
    { "k.cpp", 0, 0, 0, "foo", 0, 3 },
    { "s1.cpp", 0, 0, 0, "foo", 0, 3 },
    { "w.txt", 0, 0, 0, "foobar foo xfoo foo_ foo.", 0, 3 },
    { "w.txt", 0, 7, 7, "foobar foo xfoo foo_ foo.", 7, 3 },
    { "w.txt", 0, 12, 12, "foobar foo xfoo foo_ foo.", 12, 3 },
    { "w.txt", 0, 16, 16, "foobar foo xfoo foo_ foo.", 16, 3 },
    { "w.txt", 0, 21, 21, "foobar foo xfoo foo_ foo.", 21, 3 },
    { "s2.h", 0, 2, 2, "  foo foo", 2, 3 },
    { "s2.h", 0, 6, 6, "  foo foo", 6, 3 }
  };
  FiftHideFs hfs;
  FiftSink s;
  FifQuery* q = (FifQuery*)e_alloc(sizeof(FifQuery));
  int rc;
  u32 i;
  if (!q) { TF_CHECK(0); return; }
  es_copy(ebin_text, sizeof(ebin_text), "...");
  for (i = 0; i < 39; i++) es_cat(ebin_text, sizeof(ebin_text), "x");   // chars 4961..4999
  es_cat(ebin_text, sizeof(ebin_text), "\xEF\xBF\xBD" "foo");           // the NUL shown as U+FFFD
  want[5].text = ebin_text;
  query(tf, q, "fif1", "*.*", "foo", SM_NORMAL, 1);
  rc = fif_run(&hfs, *q, &s, 0);
  TF_EQ_I(rc, E_OK);
  TF_EQ_I(s.result, E_OK);
  TF_EQ_U(s.ndone, 1);
  check_hits(tf, s, want, sizeof(want) / sizeof(want[0]), __LINE__);
  TF_EQ_U(s.total, 20);
  TF_EQ_U(s.fwh, 12);
  TF_EQ_U(s.nfiles, 12);
  TF_EQ_U(s.searched, 15);            // includes d.obj (opened, binary) and zero.txt; not the hidden/system folders
  TF_EQ_U(s.nprogress, 15);
  TF_EQ_U(s.nskip, 0);
  // file_index: increasing per file with hits
  {
    u32 expect = 0;
    for (i = 0; i < s.hits.size(); i++) {
      if (i && strcmp(s.hits[i]->path, s.hits[i - 1]->path) != 0) expect++;
      if (s.hits[i]->h.file_index != expect) { tf_fail(tf, __FILE__, __LINE__, "file_index %u at hit %u", s.hits[i]->h.file_index, i); break; }
    }
  }
  // no subfolders
  s.clear();
  q->subfolders = 0;
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 11);
  // masks
  s.clear();
  q->subfolders = 1;
  es_copy(q->masks, sizeof(q->masks), "*.cpp;*.h !\\skipme");
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 7);                // a.cpp 3, b.h 1, s1 1, s2 2
  TF_EQ_U(s.fwh, 4);
  s.clear();
  es_copy(q->masks, sizeof(q->masks), "!*.txt !*.bin");
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 9);                // a 3, b 1, Makefile 1, k 1, s1 1, s2 2
  // plain fs_os: the hidden and system folders are searched too (the Mac has no attributes)
  s.clear();
  es_copy(q->masks, sizeof(q->masks), "*.cpp");
  TF_EQ_I(fif_run(fs_os(), *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 7);                // a 3, h 1, y 1, k 1, s1 1 (no real attributes set on the fixtures)
  // case-insensitive: b.h "#define FOO" also matches
  s.clear();
  es_copy(q->masks, sizeof(q->masks), "b.h");
  q->opts.match_case = 0;
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 2);
  if (s.hits.size() == 2) { TF_EQ_U(s.hits[0]->h.line, 0); TF_EQ_U(s.hits[0]->h.col, 8); TF_EQ_U(s.hits[1]->h.line, 1); }
  // whole word
  s.clear();
  es_copy(q->masks, sizeof(q->masks), "w.txt");
  q->opts.whole_word = 1;
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 2);
  if (s.hits.size() == 2) { TF_EQ_U(s.hits[0]->h.col, 7); TF_EQ_U(s.hits[1]->h.col, 21); }
  q->opts.whole_word = 0;
  // regex: ^\s*foo per line; anchors see each file's text
  s.clear();
  query(tf, q, "fif1", "*.cpp;*.h", "^\\s*foo\\(", SM_REGEX, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 1);                // a.cpp line 2
  if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.line, 2); TF_EQ_U(s.hits[0]->h.col, 0); TF_EQ_U(s.hits[0]->h.hl_len, 6); }
  // empty matches: ^ on b.h = 3 lines (incl. the empty last one)
  s.clear();
  query(tf, q, "fif1", "b.h", "^", SM_REGEX, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 3);
  if (s.hits.size() == 3) {
    TF_EQ_U(s.hits[2]->h.line, 2); TF_EQ_U(s.hits[2]->h.hl_len, 0); TF_EQ_U(s.hits[2]->h.text_len, 0);
    TF_EQ_U(s.hits[1]->h.pos, 14);
  }
  // extended \r\n: one hit per CRLF of a.cpp, at the line end
  s.clear();
  query(tf, q, "fif1", "a.cpp", "\\r\\n", SM_EXTENDED, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 5);
  if (s.hits.size() == 5) {
    TF_EQ_U(s.hits[0]->h.line, 0); TF_EQ_U(s.hits[0]->h.col, 10); TF_EQ_U(s.hits[0]->h.hl_start, 10);
    TF_EQ_U(s.hits[0]->h.hl_len, 0); TF_EQ_S(s.hits[0]->h.text, "int main()"); TF_EQ_U(s.hits[0]->h.len, 2);
  }
  // a match spanning lines: highlighted to the end of its first line
  s.clear();
  query(tf, q, "fif1", "a.cpp", "\\(1\\);\\R\\s*return", SM_REGEX, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 1);
  if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.line, 2); TF_EQ_U(s.hits[0]->h.hl_start, 5); TF_EQ_U(s.hits[0]->h.hl_len, 4); }
  // UTF-8 and UTF-16 patterns with non-ASCII characters (encoded per file)
  s.clear();
  query(tf, q, "fif1", "*.txt", "\xC3\xA9 foo \xE2\x82\xAC", SM_NORMAL, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.total, 1);
  if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.col, 3); TF_EQ_U(s.hits[0]->h.hl_len, 7); TF_EQ_U(s.hits[0]->h.len, 10); }
  e_free(q);
}

static void test_limits_and_errors(TF* tf)
{
  FiftHideFs hfs;
  FiftSink s;
  FifQuery* q = (FifQuery*)e_alloc(sizeof(FifQuery));
  volatile i32 cancel = 0;
  if (!q) { TF_CHECK(0); return; }
  query(tf, q, "fif1", "*.*", "foo", SM_NORMAL, 1);
  q->max_hits = 3;
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_TOOBIG);
  TF_EQ_I(s.result, E_TOOBIG);
  TF_EQ_U(s.hits.size(), 3);
  TF_EQ_U(s.total, 3);
  q->max_hits = 4;                    // the limit falls inside a file (a.cpp 3, b.h 1 -> exactly at b.h)
  s.clear();
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_TOOBIG);
  TF_EQ_U(s.total, 4);
  q->max_hits = 0;
  s.clear();
  s.cancel_at_progress = 3;
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_CANCEL);
  TF_EQ_I(s.result, E_CANCEL);
  TF_EQ_U(s.ndone, 1);
  TF_EQ_U(s.hits.size(), 4);          // a.cpp and b.h searched before the 3rd file
  s.clear();
  s.stop_at_hit = 2;
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_CANCEL);
  TF_EQ_U(s.hits.size(), 2);
  s.clear();
  cancel = 1;
  TF_EQ_I(fif_run(&hfs, *q, &s, &cancel), E_CANCEL);
  TF_EQ_U(s.hits.size(), 0);
  TF_EQ_U(s.ndone, 1);
  cancel = 0;
  s.clear();
  fpath(tf, "fif1/nonexistent", q->folder, sizeof(q->folder));
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_NOTFOUND);
  TF_EQ_I(s.result, E_NOTFOUND);
  TF_EQ_U(s.ndone, 1);
  s.clear();
  fpath(tf, "fif1/a.cpp", q->folder, sizeof(q->folder));   // a file, not a folder
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_NOTFOUND);
  s.clear();
  query(tf, q, "fif1", "*.*", "(foo", SM_REGEX, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_SYNTAX);
  TF_EQ_U(s.ndone, 1);
  TF_EQ_U(s.nprogress, 0);
  s.clear();
  query(tf, q, "fif1", "*.*", "(?<=a)b", SM_REGEX, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_UNSUPPORTED);
  s.clear();
  query(tf, q, "fif1", "*.*", "", SM_NORMAL, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_SYNTAX);
  s.clear();
  query(tf, q, "fif1/empty", "*.*", "foo", SM_NORMAL, 1);
  TF_EQ_I(fif_run(&hfs, *q, &s, 0), E_OK);
  TF_EQ_U(s.searched, 0);
  TF_EQ_I(fif_run(0, *q, &s, 0), E_RANGE);
  TF_EQ_I(fif_run(&hfs, *q, 0, 0), E_RANGE);
  e_free(q);
}

static void test_long_lines(TF* tf)
{
  char line[1200];
  FiftSink s;
  Searcher srch;
  SearchError err;
  ecp pat[3] = { 'f', 'o', 'o' };
  SearchOpts o;
  DetectResult det;
  u32 hits = 0, i, nchars;
  const char* t;
  o.mode = SM_NORMAL; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
  TF_EQ_I(srch.compile(pat, 3, o, enc_cp(1252), &err), E_OK);
  memset(&det, 0, sizeof(det));
  det.enc = enc_cp(1252); det.eol_style = EOL_LF;
  // match at char 500 of a 1000-char line: window with "..." both ends
  memset(line, 'x', 1000);
  memcpy(line + 500, "foo", 3);
  line[1000] = '\n';
  TF_EQ_I(fif_search_buffer((const u8*)line, 1001, det, srch, 7, &s, &hits), E_OK);
  TF_EQ_U(hits, 1);
  if (s.hits.size() == 1) {
    const FifHit& h = s.hits[0]->h;
    TF_EQ_U(h.col, 500); TF_EQ_U(h.pos, 500); TF_EQ_U(h.file_index, 7);
    TF_EQ_U(h.text_len, 256); TF_EQ_U(h.hl_start, 43); TF_EQ_U(h.hl_len, 3);
    TF_CHECK(memcmp(h.text, "...", 3) == 0);
    TF_CHECK(memcmp(h.text + 43, "foo", 3) == 0);
    TF_CHECK(memcmp(h.text + 253, "...", 3) == 0);
  }
  // match at char 100: starts at the line start, cut at the end
  s.clear();
  memset(line, 'x', 1000);
  memcpy(line + 100, "foo", 3);
  TF_EQ_I(fif_search_buffer((const u8*)line, 1001, det, srch, 0, &s, &hits), E_OK);
  if (s.hits.size() == 1) {
    const FifHit& h = s.hits[0]->h;
    TF_EQ_U(h.text_len, 256); TF_EQ_U(h.hl_start, 100); TF_CHECK(memcmp(h.text + 100, "foo", 3) == 0);
    TF_CHECK(memcmp(h.text + 253, "...", 3) == 0);
  }
  // match at char 252 (past 200): window mode
  s.clear();
  memset(line, 'x', 1000);
  memcpy(line + 252, "foo", 3);
  TF_EQ_I(fif_search_buffer((const u8*)line, 1001, det, srch, 0, &s, &hits), E_OK);
  if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.hl_start, 43); TF_EQ_U(s.hits[0]->h.hl_len, 3); }
  // a 60-char match at char 199 crossing the cut: highlight clipped to the 253 kept chars
  s.clear();
  {
    ecp lp[60];
    Searcher sl;
    for (i = 0; i < 60; i++) lp[i] = 'y';
    TF_EQ_I(sl.compile(lp, 60, o, enc_cp(1252), &err), E_OK);
    memset(line, 'x', 1000);
    memset(line + 199, 'y', 60);
    TF_EQ_I(fif_search_buffer((const u8*)line, 1001, det, sl, 0, &s, &hits), E_OK);
    if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.hl_start, 199); TF_EQ_U(s.hits[0]->h.hl_len, 54); TF_EQ_U(s.hits[0]->h.len, 60); }
  }
  // exactly 256 chars: no cut
  s.clear();
  memset(line, 'x', 256);
  memcpy(line + 10, "foo", 3);
  line[256] = '\r'; line[257] = '\n';
  TF_EQ_I(fif_search_buffer((const u8*)line, 258, det, srch, 0, &s, &hits), E_OK);
  if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.text_len, 256); TF_CHECK(s.hits[0]->h.text[255] == 'x'); }
  // 257 chars: cut
  s.clear();
  memset(line, 'x', 257);
  memcpy(line + 10, "foo", 3);
  line[257] = '\n';
  TF_EQ_I(fif_search_buffer((const u8*)line, 258, det, srch, 0, &s, &hits), E_OK);
  if (s.hits.size() == 1) { TF_EQ_U(s.hits[0]->h.text_len, 256); TF_CHECK(memcmp(s.hits[0]->h.text + 253, "...", 3) == 0); }
  // many matches on one long UTF-8 line: every hit window shows its match; text stays <= 256 chars
  s.clear();
  {
    EBytes b;
    DetectResult d8 = det;
    Searcher s8;
    d8.enc = enc_utf8(0);
    TF_EQ_I(s8.compile(pat, 3, o, d8.enc, &err), E_OK);
    for (i = 0; i < 400; i++) {
      b.append(i % 3 ? "\xC3\xA9" : "a", i % 3 ? 2 : 1);
      if (i % 37 == 0) b.append("foo", 3);
    }
    b.append("\n", 1);
    TF_EQ_I(fif_search_buffer(b.data(), b.size(), d8, s8, 0, &s, &hits), E_OK);
    TF_EQ_U(hits, 11);
    for (i = 0; i < s.hits.size(); i++) {
      const FifHit& h = s.hits[i]->h;
      Vec<ecp> v;
      es_utf8_to_ecp(h.text, h.text_len, &v);
      nchars = v.size();
      TF_CHECK(nchars <= 256);
      TF_CHECK(h.hl_start + 3 <= nchars && v[h.hl_start] == 'f' && v[h.hl_start + 2] == 'o');
      TF_CHECK(memcmp(b.data() + h.pos, "foo", 3) == 0);
    }
  }
  // controls: NUL and lone CR become U+FFFD, tabs stay
  s.clear();
  t = "\tA\0B\rfoo\r\n";
  TF_EQ_I(fif_search_buffer((const u8*)t, 10, det, srch, 0, &s, &hits), E_OK);
  if (s.hits.size() == 1) {
    TF_EQ_S(s.hits[0]->h.text, "\tA\xEF\xBF\xBD" "B\xEF\xBF\xBD" "foo");
    TF_EQ_U(s.hits[0]->h.hl_start, 5); TF_EQ_U(s.hits[0]->h.col, 5); TF_EQ_U(s.hits[0]->h.line, 0);
  }
}

// ---- random texts vs a reference ----

struct FiftRef { u32 line, col; epos pos; };

// Reference over the decoded characters: literal non-overlapping occurrences of pat, lines split at LF (CRLF = one
// break) or at CR (cr_mode).
static void ref_hits(const ecp* t, u32 n, const ecp* pat, u32 pn, int cr_mode, const epos* off, Vec<FiftRef>* out)
{
  u32 i = 0, line = 0, ls = 0;
  u32 j;
  while (i + pn <= n) {
    if (memcmp(t + i, pat, pn * sizeof(ecp)) == 0) {
      FiftRef r;
      for (j = ls; j < i; j++) if (t[j] == (cr_mode ? 0x0Du : 0x0Au)) { line++; ls = j + 1; }
      r.line = line; r.col = i - ls; r.pos = off[i];
      out->push(r);
      i += pn;
    } else i++;
  }
}

static void test_random(TF* tf)
{
  static const ecp alpha[] = { 'a', 'b', ' ', '\t', 'a', 'b', 0x0A, 0x0D, 0xE9, 0x20AC, 0x1F600 };
  u32 iter, n_iter = tf->long_run ? 3000 : 600;
  for (iter = 0; iter < n_iter; iter++) {
    int cr_mode = tf_rand_below(tf, 4) == 0;
    u32 n = tf_rand_below(tf, iter % 10 == 0 ? 3000 : 200), i, k, pn = 1 + tf_rand_below(tf, 3);
    Vec<ecp> text, pat;
    Vec<FiftRef> ref;
    u32 encs;
    for (i = 0; i < n; i++) {
      ecp c = alpha[tf_rand_below(tf, sizeof(alpha) / sizeof(alpha[0]))];
      if (cr_mode && c == 0x0A) c = 'a';
      if (!cr_mode && c == 0x0D && tf_rand_below(tf, 2)) c = 'b';
      text.push(c);
    }
    for (i = 0; i < pn; i++) pat.push(tf_rand_below(tf, 3) ? (ecp)("ab"[tf_rand_below(tf, 2)]) : (ecp)0xE9);
    for (encs = 0; encs < 4; encs++) {          // UTF-8, UTF-16 LE, UTF-16 BE, UTF-8 chunked
      EncSpec e = encs == 1 ? enc_utf16le(1) : encs == 2 ? enc_utf16be(1) : enc_utf8(0);
      const Codec* c = codec_get(e);
      EBytes bytes;
      Vec<epos> off;
      Searcher srch;
      SearchError err;
      SearchOpts o;
      FiftSink s;
      u32 hits = 0, bom;
      int rc;
      if (!c) { TF_CHECK(0); continue; }
      {
        u8 b[4];
        bom = enc_bom(e, b);
        bytes.append(b, bom);
      }
      for (i = 0; i < n; i++) {
        u8 b[4];
        u32 len = c->encode(text[i], b);
        off.push(bytes.size());
        bytes.append(b, len);
      }
      off.push(bytes.size());
      ref.clear();
      ref_hits(text.data(), n, pat.data(), pn, cr_mode, off.data(), &ref);
      o.mode = SM_NORMAL; o.match_case = 1; o.whole_word = 0; o.dot_all = 0;
      TF_EQ_I(srch.compile(pat.data(), pn, o, e, &err), E_OK);
      if (encs == 3) {
        FiftChunkSrc cs(bytes.data(), bytes.size(), iter);
        rc = fif_search_source(&cs, c, bom, bytes.size(), cr_mode, srch, 0, 0, &s, 0, &hits, 0);
      } else {
        ViewSource vs(bytes.data(), bytes.size());
        rc = fif_search_source(&vs, c, bom, bytes.size(), cr_mode, srch, 0, 0, &s, 0, &hits, 0);
      }
      TF_EQ_I(rc, E_OK);
      if (s.hits.size() != ref.size()) {
        tf_fail(tf, __FILE__, __LINE__, "iter %u enc %u: %u hits, ref %u", iter, encs, s.hits.size(), ref.size());
        continue;
      }
      for (k = 0; k < ref.size(); k++) {
        const FifHit& h = s.hits[k]->h;
        if (h.line != ref[k].line || h.col != ref[k].col || h.pos != ref[k].pos || h.hl_len != pn) {
          tf_fail(tf, __FILE__, __LINE__, "iter %u enc %u hit %u: %u:%u pos %u hl %u, ref %u:%u pos %u", iter, encs, k,
                  h.line, h.col, h.pos, h.hl_len, ref[k].line, ref[k].col, ref[k].pos);
          break;
        }
        {
          Vec<ecp> v;
          es_utf8_to_ecp(h.text, h.text_len, &v);
          if (v.size() > 256 || h.hl_start + pn > v.size() || memcmp(v.data() + h.hl_start, pat.data(), pn * sizeof(ecp)) != 0) {
            tf_fail(tf, __FILE__, __LINE__, "iter %u enc %u hit %u: text window wrong", iter, encs, k);
            break;
          }
          if (h.col <= 200 && v.size() < 256) {       // whole line shown: compare with the reference line
            u32 ls = 0, le, q2, x = 0, bad = 0;
            for (q2 = 0; q2 < n && x < ref[k].line; q2++) if (text[q2] == (cr_mode ? 0x0Du : 0x0Au)) { x++; ls = q2 + 1; }
            le = ls;
            while (le < n && text[le] != (cr_mode ? 0x0Du : 0x0Au)) le++;
            if (!cr_mode && le > ls && text[le - 1] == 0x0D && le < n) le--;
            if (le - ls != v.size()) bad = 1;
            for (q2 = 0; !bad && q2 < le - ls; q2++) {
              ecp want = text[ls + q2];
              if (want == 0x0D || want == 0x0A) want = ECP_REPLACEMENT;
              if (v[q2] != want) bad = 1;
            }
            if (bad) { tf_fail(tf, __FILE__, __LINE__, "iter %u enc %u hit %u: line text differs", iter, encs, k); break; }
          }
        }
      }
      TF_CHECK(1);
    }
  }
}

// The windowed path (files above the in-memory limit) gives the same hits as the in-memory path.
static void test_windowed(TF* tf)
{
  EBytes b;
  u32 i, k;
  FiftHideFs hfs;
  FiftSink s1, s2;
  FifQuery* q = (FifQuery*)e_alloc(sizeof(FifQuery));
  const char* pats[3] = { "foo", "o\\R+f", "^x.*?y$" };
  if (!q) { TF_CHECK(0); return; }
  mk(tf, "fif2");
  for (i = 0; i < 6000; i++) {
    u32 r = tf_rand_below(tf, 20);
    const char* piece = r == 0 ? "foo" : r == 1 ? "\r\n" : r == 2 ? "\n" : r == 3 ? "x" : r == 4 ? "y" : r == 5 ? "\xC3\xA9" : "ab";
    b.append(piece, es_len(piece));
  }
  b.append("\nxfooy\r\nfoo\r\nf\n", 15);   // T16: every pattern has a hit whatever the seed (-long failed ~1 in 5)
  put_file(tf, "fif2/big.txt", b.data(), b.size());
  for (k = 0; k < 3; k++) {
    query(tf, q, "fif2", "*.txt", pats[k], k ? SM_REGEX : SM_NORMAL, 1);
    s1.clear(); s2.clear();
    fif_test_limits(0, 0);
    TF_EQ_I(fif_run(&hfs, *q, &s1, 0), E_OK);
    fif_test_limits(100, 512);        // 4 x 512-byte blocks
    TF_EQ_I(fif_run(&hfs, *q, &s2, 0), E_OK);
    fif_test_limits(0, 0);
    TF_CHECK(s1.total > 0);
    TF_EQ_U(s1.total, s2.total);
    for (i = 0; i < s1.hits.size() && i < s2.hits.size(); i++) {
      const FifHit& a = s1.hits[i]->h;
      const FifHit& c = s2.hits[i]->h;
      if (a.pos != c.pos || a.len != c.len || a.line != c.line || a.col != c.col || strcmp(a.text, c.text) != 0 ||
          a.hl_start != c.hl_start || a.hl_len != c.hl_len) {
        tf_fail(tf, __FILE__, __LINE__, "pattern %u hit %u differs (pos %u vs %u)", k, i, a.pos, c.pos);
        break;
      }
    }
  }
  e_free(q);
}

static void test_oom(TF* tf)
{
  FiftHideFs hfs;
  FifQuery* q = (FifQuery*)e_alloc(sizeof(FifQuery));
  u32 k, done = 0;
  if (!q) { TF_CHECK(0); return; }
  query(tf, q, "fif1", "*.*", "fo+", SM_REGEX, 1);
  for (k = 1; k < 3000 && !done; k++) {
    FiftSink s;
    u32 before = e_alloc_count();
    int rc;
    e_test_fail_after((i32)k);
    rc = fif_run(&hfs, *q, &s, 0);
    e_test_fail_after(-1);
    if (rc == E_OK) { done = 1; TF_EQ_U(s.total, 20); }
    else if (rc != E_NOMEM && rc != E_CANCEL) { tf_fail(tf, __FILE__, __LINE__, "OOM at %u: %s", k, e_errname(rc)); break; }
    TF_EQ_U(s.ndone, 1);
    s.clear();
    if (e_alloc_count() != before) { tf_fail(tf, __FILE__, __LINE__, "leak at %u", k); break; }
  }
  TF_CHECK(done);
  e_free(q);
}

void suite_fif(TF* tf)
{
  codec_init();
  make_tree(tf);
  test_masks(tf);
  test_tree(tf);
  test_limits_and_errors(tf);
  test_long_lines(tf);
  test_random(tf);
  test_windowed(tf);
  test_oom(tf);
}
