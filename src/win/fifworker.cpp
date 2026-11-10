// fifworker.cpp - find-in-files worker thread (DESIGN 4.3, 11.6). Owner: T11.
// One search at a time. The thread runs fif_run over fs_os() and posts WM_APP_FIF batches (files, up to 50 hits,
// skipped files, the latest progress, the final result) to the frame; the UI feeds them to the results panel.
// Each run has an id: batches of an older run are dropped. The thread context is reference counted (UI + thread),
// so a run that doesn't stop in time can be abandoned safely; it sees its own cancel flag and exits later.
#include <windows.h>
#include <process.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "fifworker.h"
#include "fifworker_int.h"
#include "estr.h"

enum { BATCH_HITS = 50, FLUSH_MS = 150, MAX_OUTSTANDING = 48 };
enum { REC_FILE = 1, REC_HIT = 2, REC_SKIP = 3 };

struct FifBatch {
  u32 run;
  u8 done; u8 pad[3]; int result; u32 searched, fwh, hits;
  u32 prog_done, prog_seen; char current[260];
  u32 nrec, nhits, used, cap; u8* data;
};

struct FifCtx {
  FifQuery q;
  volatile i32 cancel;
  volatile LONG refs;
  u32 run;
  HWND notify;
};

static u32 g_run = 0;                 // current run id (incremented for every start)
static HANDLE g_thread = 0;
static FifCtx* g_ctx = 0;             // UI reference to the running context
static int g_running = 0;
static volatile LONG g_outstanding = 0;
static u32 g_skipped = 0, g_finished = 0;
static int g_last_result = 1;

static void ctx_release(FifCtx* c)
{
  if (InterlockedDecrement((LONG*)&c->refs) == 0) e_free(c);
}

static void batch_free(FifBatch* b)
{
  if (!b) return;
  if (b->data) e_free(b->data);
  e_free(b);
}

static FifBatch* batch_new(u32 run)
{
  FifBatch* b = (FifBatch*)e_alloc(sizeof(FifBatch));
  if (!b) return 0;
  memset(b, 0, sizeof(*b));
  b->run = run;
  return b;
}

static int batch_put(FifBatch* b, const void* p, u32 n)
{
  if (b->used + n > b->cap) {
    u32 cap = b->cap ? b->cap * 2 : 4096;
    u8* q;
    while (cap < b->used + n) cap *= 2;
    q = (u8*)e_realloc(b->data, cap);
    if (!q) return E_NOMEM;
    b->data = q; b->cap = cap;
  }
  memcpy(b->data + b->used, p, n);
  b->used += n;
  return E_OK;
}

// ---- worker side ----

class WorkerSink : public FifSink {
public:
  FifCtx* ctx; FifBatch* b; u32 last_post; int failed;
  WorkerSink(FifCtx* c) : ctx(c), b(0), last_post(GetTickCount()), failed(0) {}
  ~WorkerSink() { batch_free(b); }
  FifBatch* cur()
  {
    if (!b) b = batch_new(ctx->run);
    if (!b) failed = 1;
    return b;
  }
  void post()
  {
    FifBatch* x = cur();
    if (!x) return;
    while (g_outstanding > MAX_OUTSTANDING && !ctx->cancel) Sleep(20);   // let the UI catch up
    InterlockedIncrement((LONG*)&g_outstanding);
    if (!PostMessageA(ctx->notify, WM_APP_FIF, (WPARAM)ctx->run, (LPARAM)x)) {
      InterlockedDecrement((LONG*)&g_outstanding);
      batch_free(x);
    }
    b = 0;
    last_post = GetTickCount();
  }
  void maybe_post()
  {
    if (b && (b->nhits >= BATCH_HITS || GetTickCount() - last_post >= FLUSH_MS)) post();
    else if (!b && GetTickCount() - last_post >= FLUSH_MS * 2) post();   // progress only
  }
  int put_rec(u8 kind, const void* head, u32 hn, const char* s)
  {
    FifBatch* x = cur();
    u16 sl = (u16)(s ? es_len(s) : 0);
    if (!x) return 1;
    if (batch_put(x, &kind, 1) != E_OK || batch_put(x, head, hn) != E_OK || batch_put(x, &sl, 2) != E_OK ||
        (sl && batch_put(x, s, sl) != E_OK)) { failed = 1; return 1; }
    x->nrec++;
    return 0;
  }
  int on_file(const char* path, u32 index)
  {
    if (put_rec(REC_FILE, &index, 4, path)) return 1;
    return ctx->cancel ? 1 : 0;
  }
  int on_hit(const FifHit& h)
  {
    u32 head[7];
    head[0] = h.file_index; head[1] = h.line; head[2] = h.col; head[3] = h.pos; head[4] = h.len;
    head[5] = h.hl_start; head[6] = h.hl_len;
    if (put_rec(REC_HIT, head, sizeof(head), h.text)) return 1;
    b->nhits++;
    maybe_post();
    return ctx->cancel ? 1 : 0;
  }
  int on_progress(u32 done, u32 seen, const char* current)
  {
    FifBatch* x = cur();
    if (x) { x->prog_done = done; x->prog_seen = seen; es_copy(x->current, sizeof(x->current), current); }
    maybe_post();
    return ctx->cancel ? 1 : 0;
  }
  void on_skip(const char* path, int err)
  {
    i32 e = err;
    put_rec(REC_SKIP, &e, 4, path);
  }
  void on_done(int result, u32 searched, u32 fwh, u32 hits)
  {
    FifBatch* x = cur();
    if (!x) {                                  // out of memory: try a bare batch for the final result
      x = batch_new(ctx->run);
      b = x;
      if (!x) return;
    }
    x->done = 1;
    x->result = failed && result == E_OK ? E_NOMEM : result;
    x->searched = searched; x->fwh = fwh; x->hits = hits;
    post();
  }
};

static unsigned __stdcall fif_thread(void* p)
{
  FifCtx* c = (FifCtx*)p;
  {
    WorkerSink sink(c);
    fif_run(fs_os(), c->q, &sink, &c->cancel);
  }
  ctx_release(c);
  return 0;
}

// ---- UI side ----

static void forget_thread(int wait_ms)
{
  if (!g_thread) return;
  if (g_ctx) InterlockedExchange((LONG*)&g_ctx->cancel, 1);
  if (wait_ms) WaitForSingleObject(g_thread, (DWORD)wait_ms);
  CloseHandle(g_thread);
  g_thread = 0;
  if (g_ctx) { ctx_release(g_ctx); g_ctx = 0; }
}

int fif_start(const FifQuery& q)
{
  FifCtx* c;
  unsigned tid = 0;
  char header[400], tail[64];
  if (g_running || g_thread) forget_thread(2000);        // an abandoned thread keeps running on its own context
  g_running = 0;
  g_run++;
  if (g_run == 0) g_run = 1;
  c = (FifCtx*)e_alloc(sizeof(FifCtx));
  if (!c) { app_status_note("Find in Files: out of memory"); return E_NOMEM; }
  memcpy(&c->q, &q, sizeof(q));
  c->cancel = 0;
  c->refs = 2;
  c->run = g_run;
  c->notify = g_app.frame;
  search_header(q.pattern, q.pattern_len, header, sizeof(header));
  es_fmt(tail, sizeof(tail), " (searching...)");
  es_cat(header, sizeof(header), tail);
  results_begin(header);
  g_skipped = 0;
  g_last_result = 1;
  g_ctx = c;
  g_thread = (HANDLE)_beginthreadex(0, 0, fif_thread, c, 0, &tid);
  if (!g_thread) {
    g_ctx = 0;
    e_free(c);
    results_finish("Search failed: could not start the worker thread");
    return E_NOMEM;
  }
  SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
  g_running = 1;
  results_set_running(1);
  finddlg_fif_state(1);
  return E_OK;
}

void fif_stop()
{
  if (g_running && g_ctx) InterlockedExchange((LONG*)&g_ctx->cancel, 1);
}

int fif_running() { return g_running; }

void fif_abandon()
{
  if (!g_running && !g_thread) return;
  forget_thread(0);
  g_running = 0;
  g_run++;
  if (g_run == 0) g_run = 1;
  results_set_running(0);
  finddlg_fif_state(0);
}
u32 fif_last_skipped() { return g_skipped; }
int fif_last_result() { return g_last_result; }
u32 fif_runs_finished() { return g_finished; }

static u32 rd_u32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }

// Unsaved open documents inside the searched folder (their disk versions were searched).
static u32 unsaved_in(const FifQuery& q)
{
  int i, n = app_tab_count();
  u32 cnt = 0, fl = es_len(q.folder);
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    if (!t || !t->doc || t->untitled || !t->path[0] || !t->doc->modified()) continue;
    if (es_nicmp(t->path, q.folder, fl) != 0 || !t->path[fl]) continue;
    if (fl && q.folder[fl - 1] != '\\' && t->path[fl] != '\\') continue;
    if (!q.subfolders && strchr(t->path + fl + 1, '\\')) continue;
    if (!fif_mask_match(q.masks, ui_path_name(t->path))) continue;
    cnt++;
  }
  return cnt;
}

static void finish_run(const FifBatch* b)
{
  char header[600], num[64];
  EStr extra;
  const FifQuery* q = g_ctx ? &g_ctx->q : 0;
  search_header(q ? q->pattern : 0, q ? q->pattern_len : 0, header, sizeof(header));
  es_fmt(num, sizeof(num), " (%u hit%s in %u file%s of %u searched)", b->hits, b->hits == 1 ? "" : "s", b->fwh,
         b->fwh == 1 ? "" : "s", b->searched);
  es_cat(header, sizeof(header), num);
  if (b->result == E_CANCEL) extra.append(", cancelled");
  else if (b->result == E_TOOBIG && b->hits) extra.appendf(", stopped at %u hits", b->hits);
  else if (b->result == E_NOTFOUND) extra.appendf(", folder not found: %s", q ? q->folder : "");
  else if (b->result == E_NOMEM) extra.append(", out of memory");
  else if (b->result < 0 && b->result != E_TOOBIG) extra.appendf(", failed (%s)", e_errname(b->result));
  if (g_skipped) extra.appendf(", %u file%s could not be read", g_skipped, g_skipped == 1 ? "" : "s");
  if (q) {
    u32 u = unsaved_in(*q);
    if (u) extra.appendf(" (%u open file%s unsaved changes; disk versions searched)", u, u == 1 ? " has" : "s have");
  }
  es_cat(header, sizeof(header), extra.c_str());
  g_last_result = b->result;
  results_finish(header);
  results_set_running(0);
  finddlg_fif_state(0);
  g_finished++;
  if (b->result == E_OK || b->result == E_TOOBIG)
    app_status_note("Find in Files: %u hit%s in %u file%s", b->hits, b->hits == 1 ? "" : "s", b->fwh,
                    b->fwh == 1 ? "" : "s");
  else if (b->result == E_CANCEL) app_status_note("Find in Files cancelled");
}

void fif_on_msg(WPARAM w, LPARAM l)
{
  FifBatch* b = (FifBatch*)l;
  const u8* p;
  const u8* e;
  (void)w;
  if (!b) return;
  InterlockedDecrement((LONG*)&g_outstanding);
  if (b->run != g_run || !g_running) { batch_free(b); return; }
  p = b->data; e = b->data + b->used;
  while (p && p < e) {
    u8 kind = *p++;
    u32 hn = kind == REC_HIT ? 28 : 4;
    u16 sl;
    char text[1100];
    if (p + hn + 2 > e) break;
    memcpy(&sl, p + hn, 2);
    if (p + hn + 2 + sl > e) break;
    if (sl >= sizeof(text)) sl = (u16)(sizeof(text) - 1);
    memcpy(text, p + hn + 2, sl);
    text[sl] = 0;
    if (kind == REC_FILE) results_add_file(text, 0, 0);
    else if (kind == REC_SKIP) g_skipped++;
    else if (kind == REC_HIT) {
      FifHit* h = (FifHit*)e_alloc(sizeof(FifHit));
      if (h) {
        h->file_index = rd_u32(p); h->line = rd_u32(p + 4); h->col = rd_u32(p + 8); h->pos = rd_u32(p + 12);
        h->len = rd_u32(p + 16); h->hl_start = rd_u32(p + 20); h->hl_len = rd_u32(p + 24);
        es_copy(h->text, sizeof(h->text), text);
        h->text_len = es_len(h->text);
        results_add_hit(*h);
        e_free(h);
      }
    }
    memcpy(&sl, p + hn, 2);
    p += hn + 2 + sl;
  }
  results_flush();
  if (b->done) {
    finish_run(b);
    g_running = 0;
    if (g_thread) { CloseHandle(g_thread); g_thread = 0; }   // the thread exits right after posting
    if (g_ctx) { ctx_release(g_ctx); g_ctx = 0; }
  } else if (b->current[0]) {
    results_progress(b->prog_done, b->prog_seen, b->current);
  }
  batch_free(b);
}
