// bench.cpp - pica.exe -bench (DESIGN 3.2, 17.5). Owner: T17.
//
// Run on VM A with the window visible and no VNC viewer connected:
//   pica.exe -bench -cfg C:\PICATEST
// Fixtures are generated once into <cfg>\bench (the 100 MB file takes about a minute to write and is kept for later
// runs). Every measurement goes to <cfg>\bench.txt as one line "name  value  budget  OK/OVER"; the exit code is the
// number of OVER lines. Timings use QueryPerformanceCounter; a GetPixel on the view's DC after each paint makes sure
// the display driver has finished drawing.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "bench.h"
#include "views.h"
#include "docio.h"
#include "docio_int.h"
#include "watch_int.h"
#include "frame_int.h"
#include "fifworker.h"
#include "fifworker_int.h"
#include "iothread.h"
#include "render.h"
#include "render_int.h"
#include "esource.h"
#include "fsys.h"
#include "estr.h"
#include "search.h"
#include "fileops.h"
#include "doc.h"
#include "resource.h"

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif

enum { BENCH_MB = 1024u * 1024u, REPAINTS = 60 };

static FILE* g_out;
static char g_dir[260];
static double g_freq;
static u32 g_ok, g_over, g_info;

// ---------------------------------------------------------------------------------------------------------------
// timing and message helpers

static double now_ms()
{
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / g_freq;
}

static void sync_dc(HWND w)                      // waits until the driver has drawn everything queued for w
{
  HDC dc;
  if (!w) return;
  dc = GetDC(w);
  if (dc) { GetPixel(dc, 1, 1); ReleaseDC(w, dc); }
}

static void pump()
{
  MSG m;
  while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) {
    if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); return; }
    app_dispatch(&m);
  }
}

static int any_busy()
{
  int i;
  for (i = 0; i < app_tab_count(); i++) {
    Tab* t = app_tab(i);
    if (t->indexing || t->saving || (t->doc && t->doc->indexing())) return 1;
  }
  return fif_running();
}

// Runs messages and idle work (highlighter, deferred UI refresh) until nothing is pending or max_ms passed.
static void settle(u32 max_ms)
{
  u32 start = GetTickCount();
  for (;;) {
    int more;
    pump();
    more = app_idle_work();
    if (!more && !any_busy()) { pump(); if (!app_idle_work()) break; }
    if (GetTickCount() - start > max_ms) break;
    if (!more) Sleep(5);
  }
}

static void redraw_all()
{
  RedrawWindow(g_app.frame, 0, 0, RDW_UPDATENOW | RDW_ALLCHILDREN);
  sync_dc(g_app.textview);
}

static void sort_d(double* x, u32 n)
{
  u32 i, j;
  for (i = 1; i < n; i++)
    for (j = i; j > 0 && x[j - 1] > x[j]; j--) { double t = x[j]; x[j] = x[j - 1]; x[j - 1] = t; }
}

static double median_d(double* x, u32 n) { sort_d(x, n); return n ? x[n / 2] : 0.0; }
static double worst_d(double* x, u32 n) { u32 i; double w = 0; for (i = 0; i < n; i++) if (x[i] > w) w = x[i]; return w; }

static u32 avail_virtual()                       // free address space of the process (memory use deltas)
{
  MEMORYSTATUS ms;
  memset(&ms, 0, sizeof(ms));
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatus(&ms);
  return (u32)ms.dwAvailVirtual;
}

// ---------------------------------------------------------------------------------------------------------------
// report

static void out_line(const char* name, const char* value, const char* budget, int verdict)   // 1 OK, 0 OVER, -1 info
{
  const char* v = verdict > 0 ? "OK" : verdict == 0 ? "OVER" : "info";
  if (verdict > 0) g_ok++; else if (verdict == 0) g_over++; else g_info++;
  if (g_out) { fprintf(g_out, "%-44s %-34s %-18s %s\r\n", name, value, budget, v); fflush(g_out); }
}

static void out_ms(const char* name, double* x, u32 n, double budget)   // median (and worst) of n samples
{
  char value[80], b[40];
  double med = median_d(x, n), wst = worst_d(x, n);
  if (n > 1) es_fmt(value, sizeof(value), "median %d.%d ms, worst %d.%d ms", (int)med, (int)(med * 10) % 10, (int)wst,
                    (int)(wst * 10) % 10);
  else es_fmt(value, sizeof(value), "%d.%d ms", (int)med, (int)(med * 10) % 10);
  if (budget > 0) es_fmt(b, sizeof(b), "<= %d ms", (int)budget); else es_copy(b, sizeof(b), "-");
  out_line(name, value, b, budget > 0 ? med <= budget : -1);
}

static void out_note(const char* fmt, ...)
{
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_out) { fprintf(g_out, "  %s\r\n", buf); fflush(g_out); }
}

// ---------------------------------------------------------------------------------------------------------------
// fixtures

static void fix_path(const char* name, char* out, u32 cap) { ui_path_join(g_dir, name, out, cap); }

static int file_size_is(const char* path, u32 size)
{
  FsStat st;
  return fs_os()->stat(path, &st) == E_OK && st.exists && !st.size_hi && st.size_lo == size;
}

static int write_bytes(const char* path, const void* p, u32 n)
{
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  DWORD w = 0;
  BOOL ok;
  if (h == INVALID_HANDLE_VALUE) return E_IO;
  ok = n ? WriteFile(h, p, n, &w, 0) : TRUE;
  CloseHandle(h);
  return ok && w == n ? E_OK : E_IO;
}

// SynthSource text of size bytes (kept between runs when the size matches).
static int synth_file(const char* name, u32 size, u32 seed, int kind, char* path, u32 cap)
{
  SynthSource src(size, seed, kind);
  HANDLE h;
  epos off = 0;
  int rc = E_OK;
  fix_path(name, path, cap);
  if (file_size_is(path, size)) return E_OK;
  h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (h == INVALID_HANDLE_VALUE) return E_IO;
  while (off < size && rc == E_OK) {
    epos avail = 0;
    DWORD w = 0;
    const u8* p = src.get(off, &avail);
    if (!p || !avail) { rc = E_IO; break; }
    if (avail > size - off) avail = size - off;
    if (!WriteFile(h, p, avail, &w, 0) || w != avail) rc = E_IO;
    off += avail;
  }
  CloseHandle(h);
  if (rc != E_OK) DeleteFileA(path);
  return rc;
}

// A C source file with comments, strings, numbers and keywords (about 11 lines per function).
static int gen_c(EBytes* out, u32 lines)
{
  char buf[200];
  u32 i = 0, n = 0;
  out->clear();
  {
    static const char head[] = "#include <stdio.h>\r\n#include <string.h>\r\n\r\n";
    if (out->append(head, sizeof(head) - 1) != E_OK) return E_NOMEM;
  }
  n = 3;
  while (n < lines) {
    const char* tpl[11] = {
      "/* Function %u: adds up the bytes of a buffer for the editor benchmark. */\r\n",
      "static int func_%u(const char* s, int n)\r\n",
      "{\r\n",
      "    int i, total = 0;   // running total\r\n",
      "    for (i = 0; i < n; i++) {\r\n",
      "        if (s[i] == '\\n') total += 0x%X;\r\n",
      "        else total ^= (s[i] << 3) + %u;\r\n",
      "    }\r\n",
      "    printf(\"func_%u: %%d\\n\", total);\r\n",
      "    return total;\r\n",
      "}\r\n"
    };
    u32 k;
    for (k = 0; k < 11 && n < lines; k++, n++) {
      es_fmt(buf, sizeof(buf), tpl[k], i, i * 7 + 3);
      if (out->append(buf, es_len(buf)) != E_OK) return E_NOMEM;
    }
    i++;
  }
  return E_OK;
}

// Dense text as the probe defined it (probe/RESULTS.md): 116 characters and about 14 colour runs per line.
static int gen_dense(EBytes* out, u32 lines)
{
  static const char row[] =
    "    result = compute_value(first_argument, second_arg) + 0x1F; /* trailing comment text to fill the row";
  char buf[160];
  u32 n, k;
  out->clear();
  for (n = 0; n < lines; n++) {
    es_copy(buf, sizeof(buf), row);
    for (k = es_len(buf); k < 114; k++) buf[k] = '.';
    buf[114] = '*'; buf[115] = '/'; buf[116] = 0;
    if (out->append(buf, 116) != E_OK || out->append("\r\n", 2) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

// Extreme colouring: a token change at almost every character (about 70 runs per visible row). Not a budget case.
static int gen_extreme(EBytes* out, u32 lines)
{
  static const char row[] = "a=b+c*(d-e)/f; g[h]=i<<j|k&l^m; if(n>0x1F)o=\"p\"; q+=r%s-t; u=v?w:x; y=z[0]+1; ";
  u32 n;
  out->clear();
  for (n = 0; n < lines; n++) {
    if (out->append(row, sizeof(row) - 1) != E_OK) return E_NOMEM;
    if (out->append(row, 40) != E_OK || out->append("\r\n", 2) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

// 1 MB of text with exactly 10,000 "needle" (Replace All budget).
static int gen_replace(EBytes* out)
{
  static const char line[] = "the quick brown fox jumps over the lazy dog with a needle in the hay of words and more\r\n";
  u32 n, len = sizeof(line) - 1;
  out->clear();
  for (n = 0; n < 10000; n++) if (out->append(line, len) != E_OK) return E_NOMEM;
  while (out->size() + len <= BENCH_MB) {
    static const char pad[] = "the quick brown fox jumps over the lazy dog without anything sharp in the hay at all\r\n";
    if (out->append(pad, sizeof(pad) - 1) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

static int bytes_file(const char* name, const EBytes& b, char* path, u32 cap)
{
  fix_path(name, path, cap);
  if (file_size_is(path, b.size())) return E_OK;
  return write_bytes(path, b.data(), b.size());
}

// ---------------------------------------------------------------------------------------------------------------
// tabs

static void close_all()
{
  int i;
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  settle(200);
}

static Tab* open_file(const char* path)
{
  Tab* t = 0;
  int rc = docio_open(path, 0, &t);
  if (rc < 0 || !t) { out_note("could not open %s (%s)", path, e_errname(rc)); return 0; }
  return t;
}

static int view_rows()
{
  RECT r;
  const RenderFonts* f = render_fonts();
  if (!g_app.textview || !f || f->ch <= 0) return 0;
  GetClientRect(g_app.textview, &r);
  return (r.bottom - r.top) / f->ch;
}

// ---------------------------------------------------------------------------------------------------------------
// measurements

static void bench_repaint(HWND w, const char* name, double budget)
{
  double x[REPAINTS];
  u32 i;
  settle(3000);
  for (i = 0; i < REPAINTS; i++) {
    double a;
    InvalidateRect(w, 0, FALSE);
    a = now_ms();
    UpdateWindow(w);
    sync_dc(w);
    x[i] = now_ms() - a;
  }
  out_ms(name, x, REPAINTS, budget);
}

// Sends one key or wheel message to the text view and measures until the view, the status bar and the frame are drawn.
static double timed_view_msg(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  double a = now_ms();
  SendMessageA(w, msg, wp, lp);
  UpdateWindow(w);
  pump();                                        // caret / status notifications
  UpdateWindow(g_app.statusbar);
  sync_dc(w);
  return now_ms() - a;
}

static void bench_text_view(const char* path, int dense)
{
  Tab* t = open_file(path);
  HWND w = g_app.textview;
  Document* d;
  double x[60];
  u32 i, mid;
  int rows;
  char name[80];
  if (!t || !w) return;
  d = t->doc;
  mid = d->line_count() / 2;
  textview_set_selection(w, d->line_start(mid), d->line_start(mid), SCROLL_CENTER);
  settle(5000);
  rows = view_rows();
  if (dense) {
    if (dense == 1) es_fmt(name, sizeof(name), "full repaint, dense text 14 runs/line (%d rows)", rows);
    else es_fmt(name, sizeof(name), "full repaint, extreme ~70 runs/row (%d rows)", rows);
    bench_repaint(w, name, dense == 1 ? 15 : 0);
    docio_close(t, 0);
    return;
  }
  es_fmt(name, sizeof(name), "full repaint, C file coloured (%d rows)", rows);
  bench_repaint(w, name, 12);

  // arrow down at the bottom row: the view scrolls one line
  textview_set_selection(w, d->line_start(t->tv.top_line + (u32)(rows > 1 ? rows - 1 : 0)),
                         d->line_start(t->tv.top_line + (u32)(rows > 1 ? rows - 1 : 0)), SCROLL_NONE);
  settle(500);
  for (i = 0; i < 40; i++) x[i] = timed_view_msg(w, WM_KEYDOWN, VK_DOWN, 0x01500001);
  out_ms("line scroll: Down at the bottom row", x, 40, 8);
  settle(500);
  {
    RECT r;
    POINT p;
    GetWindowRect(w, &r);
    p.x = (r.left + r.right) / 2; p.y = (r.top + r.bottom) / 2;
    for (i = 0; i < 40; i++)
      x[i] = timed_view_msg(w, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(i & 1 ? 120 : -120)), MAKELPARAM(p.x, p.y));
    out_ms("line scroll: mouse wheel (3 lines)", x, 40, 8);
  }
  settle(500);
  textview_set_selection(w, 0, 0, SCROLL_MIN);
  settle(500);
  for (i = 0; i < 40; i++) x[i] = timed_view_msg(w, WM_KEYDOWN, VK_NEXT, 0x01510001);
  out_ms("page down", x, 40, 12);
  settle(500);

  // keystrokes in a normal line: edit + undo record + relex + row + caret + status, until idle again
  textview_set_selection(w, d->line_start(mid) + 4, d->line_start(mid) + 4, SCROLL_CENTER);
  settle(1000);
  for (i = 0; i < 60; i++) {
    double a = now_ms();
    SendMessageA(w, WM_CHAR, 'x', 0x002D0001);
    UpdateWindow(w);
    pump();
    while (app_idle_work()) pump();
    UpdateWindow(g_app.statusbar);
    UpdateWindow(g_app.tabbar);
    sync_dc(w);
    x[i] = now_ms() - a;
  }
  out_ms("keystroke (until idle, status + title)", x, 60, 3);

  // hex view of the same document
  t->view = VIEW_HEX;
  app_activate(app_tab_index(t));
  settle(1000);
  {
    RECT r;
    const RenderFonts* f = render_fonts();
    int hrows = 0;
    GetClientRect(g_app.hexview, &r);
    if (f && f->ch > 0) hrows = (r.bottom - r.top) / f->ch;
    es_fmt(name, sizeof(name), "hex view full repaint (~%d rows)", hrows);
  }
  bench_repaint(g_app.hexview, name, 12);
  t->view = VIEW_TEXT;
  app_activate(app_tab_index(t));
  docio_close(t, 0);
  settle(200);
}

// Open to first paint, three times (the file is in the disk cache after the first).
static void bench_open(const char* path, const char* name, double budget, u32 size)
{
  double x[3];
  u32 i, v0 = 0, v1 = 0;
  for (i = 0; i < 3; i++) {
    Tab* t = 0;
    double a;
    int rc;
    close_all();
    v0 = avail_virtual();
    a = now_ms();
    rc = docio_open(path, 0, &t);
    redraw_all();
    x[i] = now_ms() - a;
    if (rc < 0 || !t) { out_note("open %s failed (%s)", path, e_errname(rc)); return; }
    v1 = avail_virtual();
    if (i == 0) out_note("%s: first open %d ms (disk cache cold or warm)", name, (int)x[0]);
    if (i < 2) docio_close(t, 0);
  }
  out_ms(name, x, 3, budget);
  {
    char value[80], b[60];
    u32 used = v0 > v1 ? v0 - v1 : 0;
    u32 limit = size + size / 33 + 512u * 1024u;  // file size + ~3% + allocator granularity
    es_fmt(value, sizeof(value), "%u KB for a %u KB file", used / 1024, size / 1024);
    es_fmt(b, sizeof(b), "<= %u KB", limit / 1024);
    out_line("memory: in-memory document", value, b, used <= limit);
  }
  close_all();
}

// Where the open time goes: raw ReadFile, file_load (read + detect), Document::init_source (line index).
static void bench_open_breakdown(const char* path)
{
  LoadOpts lo;
  LoadResult r;
  Document* d;
  double a, t_load, t_init;
  int rc;
  char value[100];
  memset(&lo, 0, sizeof(lo));
  lo.ondemand_threshold = docio_ondemand_threshold();
  lo.det.utf8_nobom = g_app.cfg.detect_utf8 ? 1 : 0;
  lo.det.utf16_nobom = 1;
  lo.det.ansi = enc_ansi();
  lo.det.default_eol = EOL_CRLF;
  lo.enc = enc_ansi();
  a = now_ms();
  rc = file_load(fs_os(), path, lo, 0, &r);
  t_load = now_ms() - a;
  if (rc != E_OK) { out_note("file_load %s: %s", path, e_errname(rc)); return; }
  d = new Document();
  if (!d) { delete r.src; return; }
  a = now_ms();
  rc = d->init_source(r.src, r.det.enc, r.det.bom_len, r.det.eol_style, 0);
  t_init = now_ms() - a;
  if (rc != E_OK) delete r.src;
  delete d;
  es_fmt(value, sizeof(value), "file_load %d ms, init_source %d ms", (int)t_load, (int)t_init);
  out_line("  open 10 MB breakdown (core)", value, "-", -1);
}

static void bench_raw_read(const char* path, u32 size)
{
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
  u8* buf = (u8*)e_valloc(65536);
  double a, ms;
  DWORD got = 0;
  u32 total = 0;
  char value[80];
  if (h == INVALID_HANDLE_VALUE || !buf) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); if (buf) e_vfree(buf, 65536); return; }
  a = now_ms();
  while (ReadFile(h, buf, 65536, &got, 0) && got) total += got;
  ms = now_ms() - a;
  CloseHandle(h);
  e_vfree(buf, 65536);
  es_fmt(value, sizeof(value), "%d MB/s (%u MB in %d ms)", ms > 0 ? (int)((double)total / BENCH_MB * 1000.0 / ms) : 0,
         total / BENCH_MB, (int)ms);
  out_line("  raw ReadFile of the 100 MB file", value, "-", -1);
  (void)size;
}

static void bench_open_big(const char* path, u32 size)
{
  Tab* t = 0;
  double a, first, hex, index_ms;
  u32 id, v0, v1;
  int rc;
  char value[80];
  close_all();
  v0 = avail_virtual();
  a = now_ms();
  rc = docio_open(path, 0, &t);
  redraw_all();
  first = now_ms() - a;
  if (rc < 0 || !t) { out_note("open %s failed (%s)", path, e_errname(rc)); return; }
  id = t->id;
  a = now_ms();
  t->view = VIEW_HEX;
  app_activate(app_tab_index(t));
  hexview_set_selection(g_app.hexview, size - 1, size - 1, SCROLL_CENTER);
  UpdateWindow(g_app.hexview);
  sync_dc(g_app.hexview);
  hex = now_ms() - a;
  es_fmt(value, sizeof(value), "first screen %d ms + hex at end %d ms", (int)first, (int)hex);
  out_line("open 100 MB on demand: first screen + hex", value, "<= 1000 ms", first + hex <= 1000.0);
  for (;;) {
    Tab* x = docio_tab_by_id(id);
    if (!x || (!x->indexing && !x->doc->indexing())) break;
    if (now_ms() - a > 600000.0) break;
    pump();
    Sleep(10);
  }
  index_ms = now_ms() - a + first + hex;
  t = docio_tab_by_id(id);
  es_fmt(value, sizeof(value), "%d ms after open (%u lines)", (int)index_ms, t ? t->doc->line_count() : 0);
  out_line("open 100 MB on demand: full index", value, "<= 3000 ms", t && index_ms <= 3000.0);
  settle(500);
  v1 = avail_virtual();
  {
    char b[40];
    u32 used = v0 > v1 ? v0 - v1 : 0;
    es_fmt(value, sizeof(value), "%u KB for a %u MB file", used / 1024, size / BENCH_MB);
    es_copy(b, sizeof(b), "<= 8192 KB");
    out_line("memory: on-demand document", value, b, used <= 8u * BENCH_MB);
  }
  if (t) { t->view = VIEW_TEXT; app_activate(app_tab_index(t)); }
  close_all();
}

static void bench_find(const char* path10)
{
  static const char marker[] = "\r\nqzxjvkwq y1234y\r\n";
  Tab* t = open_file(path10);
  epos len;
  double a, ms;
  int r;
  char value[80];
  if (!t) return;
  settle(1000);
  len = t->doc->length();
  if (t->doc->replace(len, 0, (const u8*)marker, sizeof(marker) - 1) != E_OK) { out_note("marker edit failed"); return; }
  textview_set_selection(g_app.textview, 0, 0, SCROLL_MIN);
  settle(500);
  a = now_ms();
  r = finddlg_bench(t, "qzxjvkwq", 0, SM_NORMAL, 0, 0);
  redraw_all();
  ms = now_ms() - a;
  es_fmt(value, sizeof(value), "%d MB/s (%d ms for %u MB, found %d)", ms > 0 ? (int)((double)len / BENCH_MB * 1000.0 / ms) : 0,
         (int)ms, len / BENCH_MB, r);
  out_line("find next, literal, ignore case, 10 MB", value, ">= 30 MB/s", r == 1 && ms > 0 && len / ms * 1000.0 >= 30.0 * BENCH_MB);
  a = now_ms();
  r = finddlg_bench(t, "qzxjvkwq", 0, SM_NORMAL, 1, 0);
  ms = now_ms() - a;
  es_fmt(value, sizeof(value), "%d MB/s (%d ms)", ms > 0 ? (int)((double)len / BENCH_MB * 1000.0 / ms) : 0, (int)ms);
  out_line("find next, literal, match case (wraps)", value, ">= 30 MB/s", r == 1 && ms > 0 && len / ms * 1000.0 >= 30.0 * BENCH_MB);
  textview_set_selection(g_app.textview, 0, 0, SCROLL_MIN);
  settle(500);
  a = now_ms();
  r = finddlg_bench(t, "[xy]\\d{4}[xy]", 0, SM_REGEX, 0, 0);
  redraw_all();
  ms = now_ms() - a;
  es_fmt(value, sizeof(value), "%d.%d MB/s (%d ms, found %d)",
         ms > 0 ? (int)((double)len / BENCH_MB * 1000.0 / ms) : 0,
         ms > 0 ? (int)((double)len / BENCH_MB * 10000.0 / ms) % 10 : 0, (int)ms, r);
  out_line("find next, regex [xy]\\d{4}[xy], 10 MB", value, ">= 2 MB/s", r == 1 && ms > 0 && len / ms * 1000.0 >= 2.0 * BENCH_MB);
  t->doc->set_saved();                           // no question on close
  docio_close(t, 0);
  settle(200);
}

static void bench_replace(const char* path)
{
  Tab* t = open_file(path);
  double a, ms;
  int r;
  char value[80];
  if (!t) return;
  settle(1000);
  a = now_ms();
  r = finddlg_bench(t, "needle", "pin", SM_NORMAL, 1, 1);
  redraw_all();
  ms = now_ms() - a;
  es_fmt(value, sizeof(value), "%d ms (%d replaced)", (int)ms, r);
  out_line("replace all, 10,000 matches in 1 MB", value, "<= 2000 ms", r == 10000 && ms <= 2000.0);
  a = now_ms();
  app_command(IDM_EDIT_UNDO);
  redraw_all();
  ms = now_ms() - a;
  es_fmt(value, sizeof(value), "%d ms", (int)ms);
  out_line("undo of that replace all", value, "-", -1);
  t->doc->set_saved();
  docio_close(t, 0);
  settle(200);
}

// GDI objects: open and close 50 tabs (some in the hex view, with notification bars) and compare our live count.
static void bench_gdi(const char* small_dir_path)
{
  char path[300], name[40];
  int before, after, i;
  close_all();
  before = gdi_live();
  for (i = 0; i < 50; i++) {
    Tab* t;
    es_fmt(name, sizeof(name), "g%02d.txt", i);
    ui_path_join(small_dir_path, name, path, sizeof(path));
    if (!file_size_is(path, 12)) write_bytes(path, "hello world\n", 12);
    t = open_file(path);
    if (!t) continue;
    if (i % 3 == 0) { t->view = VIEW_HEX; app_activate(app_tab_index(t)); }
    if (i % 5 == 0) notifybar_show(t, NB_INFO, "bench note", 0, 0, 0, 0);
    if (i % 10 == 0) redraw_all();
  }
  redraw_all();
  close_all();
  redraw_all();
  after = gdi_live();
  {
    char value[80];
    es_fmt(value, sizeof(value), "%d live before, %d after 50 tabs", before, after);
    out_line("GDI objects (own count)", value, "no growth", after <= before);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// child processes: startup and session restore

struct FindWin { DWORD pid; HWND w; };

static BOOL CALLBACK find_win_cb(HWND w, LPARAM lp)
{
  FindWin* f = (FindWin*)lp;
  DWORD pid = 0;
  char cls[64];
  GetWindowThreadProcessId(w, &pid);
  if (pid != f->pid) return TRUE;
  if (!GetClassNameA(w, cls, sizeof(cls)) || strcmp(cls, APP_CLASS_FRAME) != 0) return TRUE;
  f->w = w;
  return FALSE;
}

// Starts pica.exe with args; returns ms until its frame is visible and its message loop idle, or < 0 on failure.
static double run_child(const char* args, int close_after)
{
  char exe[MAX_PATH], cmd[1200];
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  FindWin f;
  double a, ms = -1.0;
  GetModuleFileNameA(0, exe, sizeof(exe));
  es_fmt(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOWNORMAL;
  memset(&pi, 0, sizeof(pi));
  a = now_ms();
  if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, g_dir, &si, &pi)) return -1.0;
  f.pid = pi.dwProcessId;
  f.w = 0;
  while (now_ms() - a < 30000.0) {
    f.w = 0;
    EnumWindows(find_win_cb, (LPARAM)&f);
    if (f.w && IsWindowVisible(f.w)) break;
    Sleep(2);
  }
  if (f.w) {
    WaitForInputIdle(pi.hProcess, 10000);
    ms = now_ms() - a;
  }
  if (close_after && f.w) {
    PostMessageA(f.w, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 9); ms = -2.0; }
  } else if (!f.w) {
    TerminateProcess(pi.hProcess, 9);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return ms;
}

static void clear_session_dir(const char* dir)
{
  char p[300], b[300];
  ui_path_join(dir, "session1.ini", p, sizeof(p)); DeleteFileA(p);
  ui_path_join(dir, "session2.ini", p, sizeof(p)); DeleteFileA(p);
  ui_path_join(dir, "backup", b, sizeof(b));
  {
    WIN32_FIND_DATAA fd;
    char mask[300];
    HANDLE h;
    ui_path_join(b, "*.*", mask, sizeof(mask));
    h = FindFirstFileA(mask, &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { ui_path_join(b, fd.cFileName, p, sizeof(p)); DeleteFileA(p); }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
  }
}

static void bench_startup()
{
  char dir[300], args[400];
  double x[3];
  u32 i, n = 0;
  ui_path_join(g_dir, "cfgstart", dir, sizeof(dir));
  CreateDirectoryA(dir, 0);
  es_fmt(args, sizeof(args), "-multi -nosession -cfg \"%s\"", dir);
  for (i = 0; i < 3; i++) {
    double ms = run_child(args, 1);
    if (ms >= 0) x[n++] = ms;
  }
  if (!n) { out_line("startup to window visible (no session)", "failed", "<= 300 ms", 0); return; }
  out_ms("startup to window visible + idle (no session)", x, n, 300);
  out_note("startup = CreateProcess until the frame is visible and WaitForInputIdle returns (process load included)");
}

static void bench_session(const EBytes& csrc)
{
  char dir[300], args[3000], path[300], name[40];
  double x[3];
  u32 i, n = 0;
  ui_path_join(g_dir, "cfgsess", dir, sizeof(dir));
  CreateDirectoryA(dir, 0);
  clear_session_dir(dir);
  es_fmt(args, sizeof(args), "-multi -cfg \"%s\"", dir);
  for (i = 0; i < 10; i++) {                     // 10 small C files of ~8 KB
    es_fmt(name, sizeof(name), "sess%u.c", i);
    fix_path(name, path, sizeof(path));
    if (!file_size_is(path, 8000)) write_bytes(path, csrc.data() + i * 100, 8000);
    es_cat(args, sizeof(args), " \"");
    es_cat(args, sizeof(args), path);
    es_cat(args, sizeof(args), "\"");
  }
  if (run_child(args, 1) < 0) { out_line("session restore, 10 small files", "first run failed", "<= 1000 ms", 0); return; }
  es_fmt(args, sizeof(args), "-multi -cfg \"%s\"", dir);
  for (i = 0; i < 3; i++) {
    double ms = run_child(args, 1);
    if (ms >= 0) x[n++] = ms;
  }
  if (!n) { out_line("session restore, 10 small files", "failed", "<= 1000 ms", 0); return; }
  out_ms("startup + session restore, 10 small files", x, n, 1000);
}

// ---------------------------------------------------------------------------------------------------------------

int bench_run()
{
  LARGE_INTEGER f;
  char path[300], p1[300], p10[300], p100[300], pc[300], pd[300], pr[300], px[300];
  EBytes csrc, dense, rep;
  u64 free_b = 0;
  SYSTEMTIME st;
  int have100 = 0;
  if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) return E_UNSUPPORTED;
  g_freq = (double)f.QuadPart;
  ShowWindow(g_app.frame, SW_SHOWMAXIMIZED);
  ShowWindow(g_app.frame, SW_SHOWMAXIMIZED);
  SetForegroundWindow(g_app.frame);
  settle(300);

  ui_path_join(g_app.cfg_dir, "bench", g_dir, sizeof(g_dir));
  CreateDirectoryA(g_dir, 0);
  ui_path_join(g_app.cfg_dir, "bench.txt", path, sizeof(path));
  g_out = fopen(path, "wb");
  if (!g_out) return E_IO;
  GetLocalTime(&st);
  {
    char fd[80];
    render_font_desc(fd, sizeof(fd));
    fprintf(g_out, "%s %s -bench %04u-%02u-%02u %02u:%02u  (DESIGN 3.2 budgets; window maximized, %s)\r\n\r\n", APP_NAME,
            APP_VERSION, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, fd);
  }
  fprintf(g_out, "%-44s %-34s %-18s %s\r\n", "measurement", "value", "budget", "result");

  // fixtures
  app_status_note("Benchmark: writing test files...");
  redraw_all();
  if (gen_c(&csrc, 3000) != E_OK || bytes_file("bench.c", csrc, pc, sizeof(pc)) != E_OK) { out_note("bench.c failed"); }
  if (gen_dense(&dense, 3000) != E_OK || bytes_file("dense.c", dense, pd, sizeof(pd)) != E_OK) { out_note("dense.c failed"); }
  if (gen_extreme(&dense, 3000) != E_OK || bytes_file("extreme.c", dense, px, sizeof(px)) != E_OK) { out_note("extreme.c failed"); }
  if (gen_replace(&rep) != E_OK || bytes_file("replace.txt", rep, pr, sizeof(pr)) != E_OK) { out_note("replace.txt failed"); }
  synth_file("f1mb.txt", BENCH_MB, 1, SYN_TEXT_CRLF, p1, sizeof(p1));
  synth_file("f10mb.txt", 10u * BENCH_MB, 2, SYN_TEXT_CRLF, p10, sizeof(p10));
  fix_path("f100mb.txt", p100, sizeof(p100));
  if (file_size_is(p100, 100u * BENCH_MB)) have100 = 1;
  else if (fs_os()->free_space(g_dir, &free_b) == E_OK && free_b > (u64)150u * BENCH_MB)
    have100 = synth_file("f100mb.txt", 100u * BENCH_MB, 3, SYN_TEXT_CRLF, p100, sizeof(p100)) == E_OK;
  else out_note("100 MB test skipped: less than 150 MB free in %s", g_dir);
  settle(500);

  bench_text_view(pc, 0);
  bench_text_view(pd, 1);
  bench_text_view(px, 2);
  bench_open(p1, "open 1 MB from C: to first paint", 150, BENCH_MB);
  bench_open(p10, "open 10 MB from C: to first paint", 600, 10u * BENCH_MB);
  bench_open_breakdown(p10);
  if (have100) { bench_open_big(p100, 100u * BENCH_MB); bench_raw_read(p100, 100u * BENCH_MB); }
  bench_find(p10);
  bench_replace(pr);
  bench_gdi(g_dir);
  close_all();
  bench_startup();
  bench_session(csrc);

  fprintf(g_out, "\r\nsummary: %u OK, %u OVER, %u info\r\n", g_ok, g_over, g_info);
  fclose(g_out);
  g_out = 0;
  return (int)g_over;
}
