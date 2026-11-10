// docio.cpp - open/new/save/reload/revert/close (DESIGN 13.2-13.4). Owner after Wave 0: T09 (T14 in Wave 3).
//
// Data-safety rules this file keeps (DESIGN 1.1, 13.3, 16):
// - Saves never write the target directly: the I/O worker writes a temp file (file_write_temp), the UI thread runs
//   the rename sequence (file_replace) with the same SaveResult. The document is busy (edits refused) meanwhile and
//   is marked saved only at the undo state captured at snapshot time, and only after file_replace succeeded.
// - A failed save says why and what was kept, and the document stays modified.
// - No encoding or line-ending change happens without the user asking for it; Pica never adds a BOM by itself.
// - On-demand documents: the source file is read through the document's FileSource; a save over that same file uses
//   the hooks (close / reopen on the old copy, which the tab then owns and deletes on close); any other save target
//   uses NULL hooks (HANDOFF T05). The worker reads through a private second FileSource that is deleted before
//   file_replace.
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "docio_int.h"
#include "iothread.h"
#include "progress.h"
#include "views.h"
#include "notifybar.h"
#include "sessmgr.h"
#include "watch.h"
#include "largefile.h"
#include "dialogs.h"
#include "selftest.h"
#include "fsys.h"
#include "fileops.h"
#include "detect.h"
#include "config.h"
#include "estr.h"
#include "resource.h"

enum { DOCIO_MB = 1024u * 1024u, DOCIO_UNDOABLE_RELOAD_MAX = 4u * 1024u * 1024u };

// ---- per-tab bookkeeping private to docio (keyed by tab id; Tab is a shared struct) ----

struct DocioInfo {
  u32 tabid;
  EncSpec forced; u8 has_forced; u8 pad[3];   // encoding the user/session/.nfo rule forced (reload keeps it)
  char old_copy[260];                         // on-demand: file holding the original bytes after a save (owned)
  u32 save_job, save_start_ms; int save_sb; int last_save_rc;
  u32 offer_seq;                              // undo seq when the "convert to UTF-8" bar was offered
};
static Vec<DocioInfo> g_info;

static DocioInfo* info_find(u32 tabid)
{
  u32 i;
  for (i = 0; i < g_info.size(); i++) if (g_info[i].tabid == tabid) return &g_info[i];
  return 0;
}

static DocioInfo* info_get(Tab* t)
{
  DocioInfo* d = info_find(t->id);
  DocioInfo z;
  if (d) return d;
  memset(&z, 0, sizeof(z));
  z.tabid = t->id;
  z.forced = enc_ansi();
  if (g_info.push(z) != E_OK) return 0;
  return &g_info.back();
}

static void info_remove(u32 tabid)
{
  u32 i;
  for (i = 0; i < g_info.size(); i++) if (g_info[i].tabid == tabid) { g_info.erase(i, 1); return; }
}

Tab* docio_tab_by_id(u32 id)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) { Tab* t = app_tab(i); if (t && t->id == id) return t; }
  return 0;
}

// ---- small helpers ----

static void tab_name(Tab* t, char* out, u32 cap)
{
  tab_title(t, out, cap);
}

static epos ondemand_threshold()
{
  MEMORYSTATUS ms;
  u32 mb = (u32)(g_app.cfg.ondemand_mb > 0 ? g_app.cfg.ondemand_mb : 16), phys_mb;
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatus(&ms);
  phys_mb = (u32)(ms.dwTotalPhys / DOCIO_MB);
  if (phys_mb / 8 < mb) mb = phys_mb / 8;
  if (mb < 4) mb = 4;
  if (mb > 64) mb = 64;
  return (epos)(mb * DOCIO_MB);
}

static int is_nfo(const char* path)
{
  const char* n = ui_path_name(path);
  u32 len = es_len(n);
  return len >= 4 && es_icmp(n + len - 4, ".nfo") == 0;
}

static void make_load_opts(LoadOpts* lo)
{
  memset(lo, 0, sizeof(*lo));
  lo->ondemand_threshold = ondemand_threshold();
  lo->det.utf8_nobom = g_app.cfg.detect_utf8 ? 1 : 0;
  lo->det.utf16_nobom = 1;
  lo->det.ansi = enc_ansi();
  lo->det.default_eol = (EolStyle)(g_app.cfg.new_eol <= EOL_CR ? g_app.cfg.new_eol : EOL_CRLF);
  lo->force_enc = 0;
  lo->enc = enc_ansi();
  lo->cache_blocks = 0;
}

static int same_disk_state(const FsStat& a, const FsStat& b)
{
  return a.size_lo == b.size_lo && a.size_hi == b.size_hi && a.mtime_lo == b.mtime_lo && a.mtime_hi == b.mtime_hi;
}

static void sel_set(Tab* t, epos anchor, epos caret)
{
  t->sel.anchor = anchor;
  t->sel.caret = caret;
  if (t == app_active()) {
    if (t->view == VIEW_HEX && g_app.hexview) hexview_set_selection(g_app.hexview, anchor, caret, SCROLL_CENTER);
    else if (g_app.textview) textview_set_selection(g_app.textview, anchor, caret, SCROLL_CENTER);
  }
}

void docio_view_refresh(Tab* t)
{
  if (!t) return;
  if (t == app_active()) {
    if (t->view == VIEW_HEX && g_app.hexview) {
      hexview_set_selection(g_app.hexview, t->sel.anchor, t->sel.caret, SCROLL_MIN);
      hexview_invalidate(g_app.hexview);
    } else if (g_app.textview) {
      textview_set_selection(g_app.textview, t->sel.anchor, t->sel.caret, SCROLL_MIN);
      textview_invalidate(g_app.textview);
    }
  }
  app_update_tab(t);
  app_update_title();
  app_update_status();
}

// Character column of pos within its line (counted from the line start, capped).
static u32 char_col(Document* d, epos pos, u32* line_out)
{
  u32 line = d->line_of(pos), col = 0;
  epos p = d->line_start(line);
  if (p < d->text_origin()) p = d->text_origin();
  while (p < pos && col < 1000000) {
    epos q = d->char_next(p);
    if (q <= p) break;
    p = q; col++;
  }
  *line_out = line;
  return col;
}

static epos pos_of(Document* d, u32 line, u32 col)
{
  u32 n = d->line_count();
  epos p, e;
  if (line >= n) line = n ? n - 1 : 0;
  p = d->line_start(line);
  e = d->line_end(line);
  if (p < d->text_origin()) p = d->text_origin();
  if (e < p) e = p;
  while (col > 0 && p < e) {
    epos q = d->char_next(p);
    if (q <= p || q > e) break;
    p = q; col--;
  }
  return p;
}

void docio_place_get(Tab* t, DocPlace* p)
{
  p->col = char_col(t->doc, t->sel.caret, &p->line);
  p->acol = char_col(t->doc, t->sel.anchor, &p->aline);
}

void docio_place_set(Tab* t, const DocPlace& p)
{
  epos c = pos_of(t->doc, p.line, p.col);
  epos a = pos_of(t->doc, p.aline, p.acol);
  t->sel.anchor = a;
  t->sel.caret = c;
}

// ---- language ----

void docio_set_lang(Tab* t, const LangInfo* li, int forced)
{
  int color;
  if (!t || !t->hl) return;
  t->lang = li;
  t->lang_forced = (u8)(forced ? 1 : 0);
  t->hl->attach(&t->lines, li);
  // Large-mode gate (8.6): colouring off unless the user picked a language; never for on-demand documents.
  color = !t->ondemand && (!t->large || forced);
  t->hl->set_enabled(color ? true : false);
  if (li && g_app.cfg.use_lang_tabs) {
    if (li->tab_width >= 1 && li->tab_width <= 16) t->tv.tab_width = li->tab_width;
    if (li->use_tabs == 0) t->tv.use_spaces = 1;
    else if (li->use_tabs == 1) t->tv.use_spaces = 0;
    else t->tv.use_spaces = g_app.cfg.use_spaces;
  }
  if (t == app_active()) {
    if (g_app.textview) textview_invalidate(g_app.textview);
    app_update_status();
  }
}

static HMENU find_popup_with(HMENU m, UINT id)
{
  int i, n;
  if (!m) return 0;
  n = GetMenuItemCount(m);
  for (i = 0; i < n; i++) {
    HMENU sub = GetSubMenu(m, i);
    if (sub) {
      HMENU r = find_popup_with(sub, id);
      if (r) return r;
    } else if (GetMenuItemID(m, i) == id) {
      return m;
    }
  }
  return 0;
}

static int g_lang_menu_synced = 0;

int docio_lang_menu_ready()
{
  return g_lang_menu_synced;
}

void docio_sync_lang_menu()
{
  HMENU pop;
  u32 i, n = lang_count();
  if (g_lang_menu_synced || !g_app.frame || n == 0) return;
  pop = find_popup_with(GetMenu(g_app.frame), IDM_LANG_AUTO);
  if (!pop) return;
  g_lang_menu_synced = 1;
  for (i = IDR_LANG_COUNT; i < n && i < (u32)(IDM_LANG_LAST - IDM_LANG_FIRST + 1); i++) {
    const LangInfo* li = lang_at(i);
    char text[48];
    if (!li) continue;
    if (GetMenuState(pop, IDM_LANG_FIRST + i, MF_BYCOMMAND) != 0xFFFFFFFF) continue;
    es_copy(text, sizeof(text), li->name[0] ? li->name : li->id);
    AppendMenuA(pop, MF_STRING, IDM_LANG_FIRST + i, text);
  }
}

// ---- notification bars ----

static void bar_readonly_fn(Tab* t, int kind, int button_id, void* ctx)
{
  (void)ctx;
  if (button_id == 1 && t && t->doc) {               // Edit anyway: saving will ask to clear the attribute
    t->doc->set_read_only(false);
    app_update_title();
    app_update_status();
  }
  notifybar_hide(t, kind);
}

static void bar_binary_fn(Tab* t, int kind, int button_id, void* ctx)
{
  (void)ctx;
  if (button_id == 1 && t == app_active()) app_command(IDM_VIEW_HEX);   // toggles the view (T10)
  notifybar_hide(t, kind);
}

static void bar_close_fn(Tab* t, int kind, int button_id, void* ctx)
{
  (void)button_id; (void)ctx;
  notifybar_hide(t, kind);
}

static void show_open_bars(Tab* t, const LoadResult& r)
{
  char name[280], text[300];
  NotifyButton b[2];
  tab_name(t, name, sizeof(name));
  memset(b, 0, sizeof(b));
  if (r.shared_ro) {
    es_fmt(text, sizeof(text), "Another program has %s open for writing; it was opened read-only.", name);
    notifybar_show(t, NB_SHARED_RO, text, 0, 0, bar_close_fn, 0);
  } else if (r.ro_attr) {
    b[0].id = 1; es_copy(b[0].text, sizeof(b[0].text), "Edit anyway");
    es_fmt(text, sizeof(text), "%s is a read-only file.", name);
    notifybar_show(t, NB_READONLY, text, b, 1, bar_readonly_fn, 0);
  } else if (r.det.binary) {
    b[0].id = 1;
    es_copy(b[0].text, sizeof(b[0].text), t->view == VIEW_HEX ? "Show as text" : "Show in hex");
    if (r.det.utf32)
      es_fmt(text, sizeof(text), "%s looks like UTF-32 text, which Pica can't decode; it is shown as bytes.", name);
    else
      es_fmt(text, sizeof(text), "%s looks like a binary file (it contains NUL bytes).", name);
    notifybar_show(t, NB_BINARY, text, b, 1, bar_binary_fn, 0);
  }
}

// ---- open ----

void docio_default_opts(OpenOpts* o)
{
  o->force_enc = 0; o->hex = 0; o->read_only = 0; o->no_activate = 0;
  o->enc = enc_ansi(); o->line = 0; o->from_session = 0; o->pad[0] = o->pad[1] = o->pad[2] = 0;
}

static void report_open_error(const char* path, int rc)
{
  const char* n = ui_path_name(path);
  switch (rc) {
  case E_NOTFOUND: ui_error("%s was not found.\n\n%s", n, path); break;
  case E_ACCESS:   ui_error("%s can't be opened: access is denied (or it is a folder).\n\n%s", n, path); break;
  case E_TOOBIG:   ui_ask(Q_TOO_BIG, "Files of 4 GB or more can't be opened.", MB_OK | MB_ICONSTOP); break;
  case E_NOMEM:    ui_error("There is not enough memory to open %s. Close some tabs and try again.", n); break;
  case E_IO:       ui_error("A read error occurred while opening %s.\n\n%s", n, path); break;
  default:         ui_error("%s can't be opened (%s).\n\n%s", n, e_errname(rc), path); break;
  }
}

// Synchronous indexing of an on-demand document (until T14's indexer thread): modal progress, Cancel closes.
static int index_sync(Tab* t, ProgressCb* cb)
{
  BreakSpec bs;
  IndexResult ir;
  int rc;
  codec_break_spec(t->doc->enc(), t->doc->eol_style() == EOL_CR ? 1 : 0, &bs);
  rc = file_index(fs_os(), t->fsrc ? t->fsrc->path() : t->path, bs, &ir, cb, 0);
  if (rc != E_OK) return rc;
  if (ir.size != t->doc->length()) return E_STATE;     // the file changed while we indexed it
  return t->doc->finish_indexing(ir.block_breaks.data(), ir.block_breaks.size(), ir.eol);
}

int docio_open(const char* path_in, const OpenOpts* o_in, Tab** out)
{
  OpenOpts o;
  LoadOpts lo;
  LoadResult r;
  char path[260], text[400];
  Tab* t;
  Tab* reuse = 0;
  Document* d;
  DocioInfo* info;
  int rc, ondemand;
  if (out) *out = 0;
  if (!path_in || !path_in[0]) return E_NOTFOUND;
  if (o_in) o = *o_in; else docio_default_opts(&o);
  if (ui_path_full(path_in, path, sizeof(path)) != E_OK) es_copy(path, sizeof(path), path_in);
  docio_sync_lang_menu();

  // 1. a file is never opened twice
  t = tab_find_path(path);
  if (t) {
    if (!o.no_activate) app_activate(app_tab_index(t));
    if (o.line) { epos p = t->doc->line_start(o.line - 1); if (p < t->doc->text_origin()) p = t->doc->text_origin(); sel_set(t, p, p); }
    if (out) *out = t;
    return E_OK;
  }
  // tab reuse: an untouched "new N" tab is replaced by the file (closed once the file is open)
  if (!o.no_activate && !o.from_session) {
    Tab* a = app_active();
    if (a && a->untitled && a->doc && !a->saving && !a->doc->modified() && !a->doc->can_undo() &&
        a->doc->length() <= a->doc->text_origin())
      reuse = a;
  }

  // 2. load
  make_load_opts(&lo);
  if (o.force_enc) { lo.force_enc = 1; lo.enc = o.enc; }
  else if (g_app.cfg.nfo_oem && is_nfo(path)) { lo.force_enc = 1; lo.enc = enc_oem(); }
  es_fmt(text, sizeof(text), "Reading %s...", ui_path_name(path));
  for (;;) {
    UiProgress prog("Opening", text, 1);
    rc = file_load(fs_os(), path, lo, &prog, &r);
    if (rc == E_SHARING) {
      es_fmt(text, sizeof(text), "%s is locked by another program.", ui_path_name(path));
      if (ui_ask(Q_LOCKED_RETRY, text, MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY) {
        es_fmt(text, sizeof(text), "Reading %s...", ui_path_name(path));
        continue;
      }
      return E_CANCEL;
    }
    if (rc == E_NOMEM && lo.ondemand_threshold > 1) {
      FsStat st;
      if (fs_os()->stat(path, &st) == E_OK && st.exists && (st.size_hi || st.size_lo > 4 * DOCIO_MB)) {
        es_fmt(text, sizeof(text), "There is not enough memory to load %s.\n\nOpen it on demand instead (slower, "
               "uses little memory)?", ui_path_name(path));
        if (ui_ask(Q_OPEN_ONDEMAND, text, MB_YESNO | MB_ICONQUESTION) == IDYES) {
          lo.ondemand_threshold = 1;                  // anything larger than 1 byte is read on demand
          es_fmt(text, sizeof(text), "Reading %s...", ui_path_name(path));
          continue;
        }
        return E_CANCEL;
      }
    }
    break;
  }
  if (rc == E_CANCEL) return rc;
  if (rc != E_OK) {
    if (!o.from_session) report_open_error(path, rc);
    else app_status_note("Could not open %s (%s)", path, e_errname(rc));
    return rc;
  }

  // 3. document
  ondemand = r.mode == LM_ONDEMAND;
  d = new Document();
  if (!d) { delete r.src; ui_error("There is not enough memory to open %s.", ui_path_name(path)); return E_NOMEM; }
  if (ondemand) rc = d->init_source_indexing(r.src, r.det.enc, r.det.bom_len, r.det.eol_style);
  else {
    UiProgress prog("Opening", "Counting lines...", 0);
    rc = d->init_source(r.src, r.det.enc, r.det.bom_len, r.det.eol_style, &prog);
  }
  if (rc != E_OK) {                                  // the document took (and freed) the source
    delete d;
    if (!o.from_session) report_open_error(path, rc);
    return rc;
  }
  t = tab_new(d);
  if (!t) { delete d; ui_error("There is not enough memory to open %s.", ui_path_name(path)); return E_NOMEM; }
  es_copy(t->path, sizeof(t->path), path);
  t->untitled = 0;
  t->disk = r.st;
  t->disk_missing = 0;
  t->ro_attr = r.ro_attr;
  t->shared_ro = r.shared_ro;
  t->ondemand = (u8)(ondemand ? 1 : 0);
  t->fsrc = ondemand ? (FileSource*)d->source() : 0;
  t->large = (u8)(ondemand || r.st.size_lo >= (u32)g_app.cfg.large_mb * DOCIO_MB ? 1 : 0);
  if (t->large) t->tv.wrap = 0;
  t->enc_forced = (u8)(o.force_enc ? 1 : 0);
  t->view = (u8)((o.hex || (r.det.binary && g_app.cfg.binary_hex)) ? VIEW_HEX : VIEW_TEXT);
  info = info_get(t);
  if (info && lo.force_enc) { info->forced = lo.enc; info->has_forced = 1; }
  if (o.read_only || r.ro_attr || r.shared_ro) d->set_read_only(true);
  docio_set_lang(t, lang_for_file(ui_path_name(path)), 0);
  {
    epos p = d->text_origin();
    if (o.line > 1) { p = d->line_start(o.line - 1); if (p < d->text_origin()) p = d->text_origin(); }
    t->sel.anchor = t->sel.caret = p;
  }
  if (app_add_tab(t, o.no_activate ? 0 : 1) != E_OK) {
    info_remove(t->id);
    tab_free(t);
    ui_error("There is not enough memory to open %s.", ui_path_name(path));
    return E_NOMEM;
  }
  if (ondemand) {
    rc = largefile_begin(t);
    if (rc == E_NOTIMPL) {                           // T14 not there yet: index now, with a modal progress
      UiProgress prog("Opening", "Indexing lines...", 1);
      t->indexing = 1;
      rc = index_sync(t, &prog);
      t->indexing = 0;
    }
    if (rc != E_OK) {
      if (rc != E_CANCEL) report_open_error(path, rc);
      docio_close(t, 0);
      return rc;
    }
  }
  show_open_bars(t, r);
  watch_tab_added(t);
  if (!o.from_session) config_mru_add(&g_app.cfg, path);
  if (t == app_active()) docio_view_refresh(t);
  if (reuse && reuse != t) docio_close(reuse, 0);
  if (out) *out = t;
  return E_OK;
}

int docio_new(Tab** out)
{
  Document* d;
  Tab* t;
  EncSpec e = g_app.cfg.new_enc;
  int rc;
  if (out) *out = 0;
  docio_sync_lang_menu();
  if (!codec_get(e)) e = enc_ansi();
  d = new Document();
  if (!d) return E_NOMEM;
  rc = d->init_empty(e, (EolStyle)(g_app.cfg.new_eol <= EOL_CR ? g_app.cfg.new_eol : EOL_CRLF));
  if (rc != E_OK) { delete d; return rc; }
  t = tab_new(d);
  if (!t) { delete d; return E_NOMEM; }
  t->untitled = 1;
  t->untitled_no = g_app.next_untitled++;
  t->path[0] = 0;
  t->sel.anchor = t->sel.caret = d->text_origin();
  info_get(t);
  docio_set_lang(t, 0, 0);
  if (app_add_tab(t, 1) != E_OK) { info_remove(t->id); tab_free(t); return E_NOMEM; }
  docio_view_refresh(t);
  if (out) *out = t;
  return E_OK;
}

// ---- save ----

struct SaveCtx {
  SaveDone cb; void* cb_ctx;
  FileSource* priv;           // on-demand: private reader for the worker (deleted before file_replace)
  u8 path_change, existed, pad[2];
};

class DocioHooks : public SaveHooks {        // on-demand save over the file the document reads (13.3 steps 5, 9)
public:
  DocioHooks(Tab* t) : t_(t), reopen_rc(E_OK), called(0) { kept[0] = 0; }
  void before_replace() { if (t_->fsrc) t_->fsrc->close(); }
  void after_replace(const char* p)
  {
    called = 1;
    es_copy(kept, sizeof(kept), p ? p : "");
    reopen_rc = t_->fsrc ? t_->fsrc->reopen(kept) : E_STATE;
  }
  Tab* t_; char kept[260]; int reopen_rc; int called;
};

// 1 if both files exist and hold the same bytes (read in 64 KB chunks), 0 if not or on any error.
static int files_equal(const char* a, const char* b)
{
  FileSys* fs = fs_os();
  FsFile fa = 0, fb = 0;
  u32 la = 0, ha = 0, lb = 0, hb = 0, off = 0;
  u8* ba;
  u8* bb;
  int same = 0;
  if (fs->open(a, FS_READ | FS_SHARE_ALL, &fa) != E_OK) return 0;
  if (fs->open(b, FS_READ | FS_SHARE_ALL, &fb) != E_OK) { fs->close(fa); return 0; }
  ba = (u8*)e_valloc(65536);
  bb = (u8*)e_valloc(65536);
  if (ba && bb && fs->size(fa, &la, &ha) == E_OK && fs->size(fb, &lb, &hb) == E_OK && la == lb && ha == 0 && hb == 0) {
    same = 1;
    while (off < la && same) {
      u32 want = la - off, ga = 0, gb = 0;
      if (want > 65536) want = 65536;
      if (fs->read_at(fa, off, 0, ba, want, &ga) != E_OK || fs->read_at(fb, off, 0, bb, want, &gb) != E_OK ||
          ga != want || gb != want || memcmp(ba, bb, want) != 0)
        same = 0;
      off += want;
    }
  }
  if (ba) e_vfree(ba, 65536);
  if (bb) e_vfree(bb, 65536);
  fs->close(fa);
  fs->close(fb);
  return same;
}

static void report_save_error(Tab* t, const char* target, int rc, const SaveResult& sr, int existed)
{
  char msg[1400], name[280];
  EStr s;
  const char* n = ui_path_name(target);
  tab_name(t, name, sizeof(name));
  switch (rc) {
  case E_DISKFULL: s.appendf("There is not enough free space on the disk to save %s.", n); break;
  case E_ACCESS:
    if (sr.failed_step == 4) s.appendf("%s can't be replaced: it is read-only or access is denied.", n);
    else s.appendf("Access is denied: %s can't be written.", n);
    break;
  case E_SHARING:  s.appendf("%s is in use by another program and can't be replaced.", n); break;
  case E_NOTFOUND: s.appendf("The folder for %s does not exist.", n); break;
  case E_EXISTS:   s.appendf("%s can't be saved: a file of the same name appeared.", n); break;
  case E_IO:       s.appendf("A disk error occurred while saving %s.", n); break;
  case E_NOMEM:    s.appendf("There is not enough memory to save %s.", n); break;
  default:         s.appendf("%s could not be saved (%s).", n, e_errname(rc)); break;
  }
  s.append("\n\n");
  if (sr.old_copy[0]) {
    s.appendf("%s may be damaged or missing. Your previous version is kept as:\n%s\n", n, sr.old_copy);
    if (sr.temp[0]) s.appendf("The new version is kept as:\n%s\n", sr.temp);
  } else {
    s.append(existed ? "Your file on disk is unchanged.\n" : "Nothing was written.\n");
    if (sr.temp[0]) s.appendf("A complete copy of the new version is kept as:\n%s\n", sr.temp);
  }
  if (sr.stray[0]) s.appendf("A temporary file may have been left behind:\n%s\n", sr.stray);
  s.appendf("\nYour changes are still in the editor (%s is not saved).", name);
  es_copy(msg, sizeof(msg), s.c_str());
  ui_ask(Q_SAVE_FAILED, msg, MB_OK | MB_ICONERROR);
}

static void save_finish_ui(Tab* t, DocioInfo* info)
{
  if (info) {
    if (info->save_sb) progress_status_end(info->save_sb);
    info->save_sb = 0;
    info->save_job = 0;
  }
  t->saving = 0;
  t->save_pct = 0;
  if (t->doc) t->doc->set_busy(false);
  app_update_tab(t);
  app_update_title();
  app_update_status();
}

void docio_io_done(IoJob* job)
{
  SaveCtx* sc = (SaveCtx*)job->ctx;
  Tab* t = docio_tab_by_id(job->tabid);
  DocioInfo* info = t ? info_get(t) : 0;
  SaveDone cb = sc ? sc->cb : 0;
  void* cb_ctx = sc ? sc->cb_ctx : 0;
  int rc = job->result;
  char name[280];
  // the worker is done with the snapshot: free it, then the private reader (Win98 can't rename open files)
  if (job->data) { delete job->data; job->data = 0; }
  if (sc && sc->priv) { delete sc->priv; sc->priv = 0; }
  job->ctx = 0;
  if (!t) {                                          // tab gone (should not happen: close waits): never rename
    if (rc == E_OK && job->save.temp[0]) fs_os()->remove(job->save.temp);
    delete sc;
    return;
  }
  if (rc == E_OK) {
    SaveOpts so;
    DocioHooks hooks(t);
    int use_hooks = t->fsrc && t->fsrc->path() && ui_path_equal(t->fsrc->path(), job->path);
    so.keep_bak = g_app.cfg.keep_bak ? 1 : 0;
    so.pad[0] = so.pad[1] = so.pad[2] = 0;
    rc = file_replace(fs_os(), job->path, so, use_hooks ? &hooks : 0, &job->save);
    // A failed in-place fallback keeps an old copy because the target *may* be damaged. If the target still holds
    // exactly the old bytes (typical: another program denies writing, so nothing was written), the old copy and the
    // temp file are redundant (the editor still has the new text): remove them and say the file is unchanged.
    if (rc != E_OK && !use_hooks && job->save.old_copy[0] && files_equal(job->path, job->save.old_copy)) {
      if (fs_os()->remove(job->save.old_copy) == E_OK) {
        job->save.old_copy[0] = 0;
        if (job->save.temp[0] && fs_os()->remove(job->save.temp) == E_OK) job->save.temp[0] = 0;
      }
    }
    if (use_hooks && hooks.called && info) {
      if (hooks.kept[0] && !ui_path_equal(hooks.kept, job->path)) {
        if (info->old_copy[0] && !ui_path_equal(info->old_copy, hooks.kept)) fs_os()->remove(info->old_copy);
        es_copy(info->old_copy, sizeof(info->old_copy), hooks.kept);
      }
      if (hooks.reopen_rc != E_OK)
        ui_error("Pica could not reopen the original bytes of %s (%s). The file on disk is %s. Close the tab and "
                 "open the file again before editing further.", ui_path_name(job->path), e_errname(hooks.reopen_rc),
                 rc == E_OK ? "saved completely" : "unchanged");
    }
  }
  save_finish_ui(t, info);
  if (info) info->last_save_rc = rc;
  if (rc == E_OK) {
    t->doc->set_saved_seq(job->seq);
    t->disk = job->save.st;
    t->disk_missing = 0;
    t->ro_attr = 0;
    t->change_pending = 0;
    if (sc && sc->path_change) {
      watch_tab_removed(t);
      es_copy(t->path, sizeof(t->path), job->path);
      t->untitled = 0;
      if (!t->lang_forced) docio_set_lang(t, lang_for_file(ui_path_name(t->path)), 0);
      config_mru_add(&g_app.cfg, t->path);
      watch_tab_added(t);
    }
    watch_tab_saved(t);
    sessmgr_tab_saved(t);
    tab_name(t, name, sizeof(name));
    if (job->save.stray[0]) app_status_note("Saved %s (a temporary file was left: %s)", name, job->save.stray);
    else if (job->save.in_place) app_status_note("Saved %s (written in place: another program holds it)", name);
    else app_status_note("Saved %s", name);
    selftest_log("saved %s (%u bytes)", job->path, job->save.st.size_lo);
    app_update_tab(t);
    app_update_title();
    app_update_status();
  } else if (rc != E_CANCEL) {
    report_save_error(t, job->path, rc, job->save, sc ? sc->existed : 1);
  } else {
    if (job->save.temp[0]) fs_os()->remove(job->save.temp);
    app_status_note("Save cancelled; the file on disk is unchanged");
  }
  delete sc;
  if (cb) cb(t, rc, cb_ctx);
}

void docio_io_abandon(IoJob* job)
{
  SaveCtx* sc = (SaveCtx*)job->ctx;
  if (job->data) { delete job->data; job->data = 0; }
  if (sc && sc->priv) { delete sc->priv; sc->priv = 0; }
  if (job->result == E_OK && job->save.temp[0]) fs_os()->remove(job->save.temp);
  delete sc;
  job->ctx = 0;
}

void docio_io_progress(u32 tabid, u32 done, u32 total)
{
  Tab* t = docio_tab_by_id(tabid);
  DocioInfo* info;
  u32 pct;
  if (!t || !t->saving) return;
  info = info_get(t);
  pct = total ? (u32)(((u64)done * 100u) / total) : 0;
  if (pct > 100) pct = 100;
  t->save_pct = pct;
  if (info && !info->save_sb && app_now_ms() - info->save_start_ms >= 500) {
    char label[300], name[280];
    tab_name(t, name, sizeof(name));
    es_fmt(label, sizeof(label), "Saving %s", name);
    info->save_sb = progress_status_begin(label);
  }
  if (info && info->save_sb) progress_status_step(info->save_sb, pct);
  app_update_tab(t);
}

int docio_save(Tab* t, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null, SaveDone cb,
               void* ctx)
{
  char target[260], name[280], text[700];
  FsStat st;
  SaveCtx* sc;
  SnapshotSource* snap;
  FileSource* priv = 0;
  IoJob* job;
  DocioInfo* info;
  FileSys* fs = fs_os();
  int rc, path_change, clear_ro = 0;
  u32 id;
  if (!t || !t->doc) return E_STATE;
  tab_name(t, name, sizeof(name));
  if (t->saving) { app_status_note("%s is already being saved", name); return E_BUSY; }
  if (t->indexing || t->doc->indexing()) {
    app_status_note("%s is still being indexed; save it when that has finished", name);
    return E_BUSY;
  }
  if (path_or_null && path_or_null[0]) {
    if (ui_path_full(path_or_null, target, sizeof(target)) != E_OK) es_copy(target, sizeof(target), path_or_null);
  } else {
    if (t->untitled || !t->path[0]) return E_STATE;   // caller runs Save As
    es_copy(target, sizeof(target), t->path);
  }
  path_change = t->untitled || !t->path[0] || !ui_path_equal(target, t->path);
  if (path_change) {
    Tab* other = tab_find_path(target);
    if (other && other != t) {
      ui_error("%s is open in another tab. Close that tab first, or choose another name.", ui_path_name(target));
      return E_EXISTS;
    }
  }
  // target state (13.3 step 4 is checked again by file_replace). Questions first, side effects after them, so a
  // Cancel anywhere leaves both the disk and the document as they were.
  memset(&st, 0, sizeof(st));
  rc = fs->stat(target, &st);
  if (rc != E_OK) memset(&st, 0, sizeof(st));
  if (st.exists && (st.attrs & FSA_DIR)) {
    ui_error("%s is a folder.", target);
    return E_ACCESS;
  }
  if (st.exists && (st.attrs & FSA_READONLY)) {
    int a;
    es_fmt(text, sizeof(text), "%s is a read-only file.\n\nClear the read-only attribute and save? (No = choose "
           "another name with Save As.)", ui_path_name(target));
    a = ui_ask(Q_CLEAR_READONLY, text, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (a == IDNO) return E_READONLY;
    if (a != IDYES) return E_CANCEL;
    clear_ro = 1;
  }
  if (!path_change && st.exists && t->disk.exists && !t->disk_missing && !same_disk_state(st, t->disk)) {
    es_fmt(text, sizeof(text), "%s was changed by another program since Pica read it.\n\nSave your version over "
           "it? (Cancel keeps the file on disk as it is.)", ui_path_name(target));
    if (ui_ask(Q_DOCIO_CHANGED_ON_DISK, text, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return E_CANCEL;
  }
  // conversions chosen in the Save As dialog (12.2): one undo step each, with the loss prompt
  if (enc_or_null && !enc_equal(*enc_or_null, t->doc->enc())) {
    rc = doccmd_convert(t, *enc_or_null, 1);
    if (rc != E_OK) return rc;
  }
  if (eol_or_null && *eol_or_null >= EOL_CRLF && *eol_or_null <= EOL_CR &&
      (t->doc->eol_mixed() || t->doc->eol_style() != (EolStyle)*eol_or_null)) {
    rc = doccmd_convert_eol(t, *eol_or_null, 1);
    if (rc != E_OK) return rc;
  }
  if (clear_ro) {
    rc = fs->set_attrs(target, st.attrs & ~(u32)(FSA_READONLY | FSA_DIR));
    if (rc != E_OK) {
      ui_error("The read-only attribute of %s could not be cleared (%s). Your file is unchanged.",
               ui_path_name(target), e_errname(rc));
      return rc;
    }
  }
  // snapshot of the bytes (4.3 exception 1); on-demand documents read through a private second handle
  if (t->fsrc) {
    if (t->fsrc->err() != E_OK || !t->fsrc->path() || !t->fsrc->path()[0]) {
      ui_error("The original bytes of %s can't be read any more (%s). Nothing was saved.", name,
               e_errname(t->fsrc->err()));
      return E_STATE;
    }
    priv = new FileSource();
    if (!priv) return E_NOMEM;
    rc = priv->open(fs, t->fsrc->path(), FS_READ | (t->shared_ro ? FS_SHARE_ALL : FS_DENY_WRITE), 4);
    if (rc != E_OK) {
      delete priv;
      ui_error("%s can't be read for saving (%s). Nothing was saved.", t->fsrc->path(), e_errname(rc));
      return rc;
    }
  }
  snap = new SnapshotSource();
  sc = new SaveCtx;
  job = new IoJob;
  if (!snap || !sc || !job) {
    delete snap; delete sc; delete job; delete priv;
    ui_error("There is not enough memory to save %s.", name);
    return E_NOMEM;
  }
  rc = t->doc->snapshot(snap, priv);
  if (rc != E_OK) {
    delete snap; delete sc; delete job; delete priv;
    ui_error("There is not enough memory to save %s (%s).", name, e_errname(rc));
    return rc;
  }
  memset(sc, 0, sizeof(*sc));
  sc->cb = cb; sc->cb_ctx = ctx; sc->priv = priv;
  sc->path_change = (u8)(path_change ? 1 : 0);
  sc->existed = (u8)(st.exists ? 1 : 0);
  memset(job, 0, sizeof(*job));
  job->kind = IOJ_SAVE_TEMP;
  job->tabid = t->id;
  es_copy(job->path, sizeof(job->path), target);
  job->data = snap;
  job->seq = t->doc->undo_seq();
  job->ctx = sc;
  t->doc->set_busy(true);
  t->saving = 1;
  t->save_pct = 0;
  info = info_get(t);
  id = iothread_submit(job);                         // owns job (and snap) from here, also on failure
  if (!id) {
    t->doc->set_busy(false);
    t->saving = 0;
    delete priv;
    delete sc;
    ui_error("The save of %s could not be started (the I/O thread is not running). Nothing was saved.", name);
    return E_STATE;
  }
  if (info) { info->save_job = id; info->save_start_ms = app_now_ms(); info->save_sb = 0; info->last_save_rc = E_BUSY; }
  app_update_tab(t);
  app_update_status();
  return E_OK;
}

int docio_wait_save(Tab* t)
{
  DocioInfo* info;
  u32 id;
  char text[300], name[280];
  if (!t) return E_STATE;
  info = info_get(t);
  if (!info) return E_NOMEM;
  id = info->save_job;
  if (!t->saving || !id) return info->last_save_rc;
  tab_name(t, name, sizeof(name));
  es_fmt(text, sizeof(text), "Saving %s...", name);
  {
    UiProgress prog("Saving", text, 1);
    int cancelled = 0;
    while (iothread_job_pending(id)) {
      u32 done = 0, total = 0;
      if (iothread_process_done(id)) break;          // finished: completion runs now (rename sequence)
      if (!iothread_job_pending(id)) break;
      done = t->save_pct; total = 100;
      if (prog.progress(done, total) && !cancelled) { cancelled = 1; iothread_cancel_tab(t->id); }
      Sleep(15);
    }
  }
  info = info_find(t->id);
  return info ? info->last_save_rc : E_STATE;
}

int docio_save_wait(Tab* t, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null)
{
  int rc = docio_save(t, path_or_null, enc_or_null, eol_or_null, 0, 0);
  if (rc != E_OK) return rc;
  return docio_wait_save(t);
}

// ---- reload, revert ----

static int bom_len_for(Document* d, const EncSpec& e)
{
  u8 bom[4], head[4];
  EncSpec eb = e;
  u32 n;
  eb.bom = 1;
  n = enc_bom(eb, bom);
  if (!n || d->length() < n) return 0;
  if (d->read(0, head, n) != E_OK) return 0;
  return memcmp(head, bom, n) == 0 ? (int)n : 0;
}

int docio_reload(Tab* t, const EncSpec* as_or_null, int undoable)
{
  LoadOpts lo;
  LoadResult r;
  DocPlace pl;
  DocioInfo* info;
  Document* d;
  char text[300], old_copy[260];
  int rc, was_ro_attr;
  if (!t || !t->doc) return E_STATE;
  d = t->doc;
  if (t->saving || t->indexing || d->indexing()) {
    app_status_note("Please wait until the save or indexing has finished");
    return E_BUSY;
  }
  info = info_get(t);
  docio_place_get(t, &pl);
  if (t->untitled || !t->path[0]) {                  // untitled: reinterpret the current bytes
    EncSpec e;
    if (!as_or_null) return E_STATE;
    e = *as_or_null;
    {
      UiProgress prog("Reloading", "Decoding...", 0);
      int bl = bom_len_for(d, e);
      e.bom = (u8)(bl ? 1 : 0);
      rc = d->reinterpret(e, (epos)bl, &prog);
    }
    if (rc != E_OK) { ui_error("The document could not be reinterpreted (%s).", e_errname(rc)); return rc; }
    if (info) { info->forced = e; info->has_forced = 1; }
    t->enc_forced = 1;
    docio_place_set(t, pl);
    docio_view_refresh(t);
    return E_OK;
  }
  make_load_opts(&lo);
  if (as_or_null) { lo.force_enc = 1; lo.enc = *as_or_null; }
  else if (info && info->has_forced) { lo.force_enc = 1; lo.enc = info->forced; }
  else if (g_app.cfg.nfo_oem && is_nfo(t->path)) { lo.force_enc = 1; lo.enc = enc_oem(); }
  es_fmt(text, sizeof(text), "Reading %s...", ui_path_name(t->path));
  {
    UiProgress prog("Reloading", text, 1);
    rc = file_load(fs_os(), t->path, lo, &prog, &r);
  }
  if (rc == E_CANCEL) return rc;
  if (rc != E_OK) {
    if (rc == E_NOTFOUND) ui_error("%s no longer exists on disk. Your document is unchanged.", ui_path_name(t->path));
    else if (rc == E_SHARING) ui_error("%s is locked by another program. Your document is unchanged.", ui_path_name(t->path));
    else ui_error("%s could not be read (%s). Your document is unchanged.", ui_path_name(t->path), e_errname(rc));
    return rc;
  }
  info = info_find(t->id);                           // the table may have moved while the progress window pumped
  was_ro_attr = t->ro_attr;
  old_copy[0] = 0;
  if (info && info->old_copy[0]) es_copy(old_copy, sizeof(old_copy), info->old_copy);
  if (undoable && !as_or_null && r.mode == LM_MEMORY && !t->ondemand && r.src->size() <= DOCIO_UNDOABLE_RELOAD_MAX &&
      enc_equal(r.det.enc, d->enc()) && (r.det.eol_style == EOL_CR) == (d->eol_style() == EOL_CR) && !d->read_only()) {
    rc = d->reload_as_edit(r.src);                   // one undo step (13.4); does not take the source
    delete r.src;
    if (rc == E_OK) d->set_saved();
  } else {
    UiProgress prog("Reloading", "Counting lines...", 1);
    rc = d->reset_source(r.src, r.det.enc, r.det.bom_len, r.det.eol_style, &prog);   // takes the source
  }
  if (rc != E_OK) {
    ui_error("%s could not be reloaded (%s). Your document is unchanged.", ui_path_name(t->path), e_errname(rc));
    return rc;
  }
  // T14 note: the Document keeps its in-memory / on-demand mode across reset_source; a file that crossed the
  // on-demand threshold keeps the old mode's rules (convert allowed / refused) until it is reopened.
  info = info_find(t->id);
  t->ondemand = (u8)(r.mode == LM_ONDEMAND ? 1 : 0);
  t->fsrc = t->ondemand ? (FileSource*)d->source() : 0;
  t->large = (u8)(t->ondemand || r.st.size_lo >= (u32)g_app.cfg.large_mb * DOCIO_MB ? 1 : 0);
  t->disk = r.st;
  t->disk_missing = 0;
  t->change_pending = 0;
  t->ro_attr = r.ro_attr;
  t->shared_ro = r.shared_ro;
  if (as_or_null) {
    if (info) { info->forced = *as_or_null; info->has_forced = 1; }
    t->enc_forced = 1;
  }
  if (r.shared_ro || (r.ro_attr && !was_ro_attr)) d->set_read_only(true);
  if (r.ro_attr && !was_ro_attr) show_open_bars(t, r);
  if (old_copy[0] && (!t->fsrc || !ui_path_equal(t->fsrc->path(), old_copy))) {   // the old source is closed now
    fs_os()->remove(old_copy);
    if (info) info->old_copy[0] = 0;
  }
  docio_set_lang(t, t->lang, t->lang_forced);        // large-mode gate may have changed
  docio_place_set(t, pl);
  watch_tab_saved(t);                                // the disk state we now hold
  docio_view_refresh(t);
  app_on_doc_changed(t);
  return E_OK;
}

int docio_revert(Tab* t)
{
  char text[400], name[280];
  if (!t || !t->doc) return E_STATE;
  if (t->untitled || !t->path[0]) { app_status_note("An untitled document has no saved version"); return E_STATE; }
  tab_name(t, name, sizeof(name));
  if (t->doc->modified()) {
    es_fmt(text, sizeof(text), "Discard all changes to %s and reload it from disk?\n\nThis can't be undone.", name);
    if (ui_ask(Q_REVERT, text, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return E_CANCEL;
  }
  return docio_reload(t, 0, 0);
}

// ---- close ----

int docio_close(Tab* t, int ask)
{
  char text[400], name[280], old_copy[260];
  DocioInfo* info;
  if (!t) return E_STATE;
  if (app_tab_index(t) < 0) return E_STATE;
  tab_name(t, name, sizeof(name));
  if (t->saving) docio_wait_save(t);                 // a running save always completes (or fails) first
  if (ask && t->doc && t->doc->modified()) {
    int a;
    es_fmt(text, sizeof(text), "Save changes to %s?", name);
    a = ui_ask(Q_SAVE_CHANGES, text, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (a == IDCANCEL) return 0;
    if (a == IDYES) {
      int rc = filecmd_save(t, 1);
      if (rc != E_OK || (t->doc && t->doc->modified())) return 0;   // not saved: keep the tab
    } else if (a != IDNO) {
      return 0;
    }
  }
  if (t->indexing) largefile_cancel(t);
  iothread_cancel_tab(t->id);
  if (!iothread_wait_tab(t->id, 30000)) {
    ui_error("%s can't be closed yet: a file operation is still running. Try again in a moment.", name);
    return E_BUSY;
  }
  sessmgr_tab_closed(t);
  watch_tab_removed(t);
  app_remove_tab(t);
  old_copy[0] = 0;
  info = info_find(t->id);
  if (info) es_copy(old_copy, sizeof(old_copy), info->old_copy);
  info_remove(t->id);
  tab_free(t);                                       // closes an on-demand FileSource
  if (old_copy[0]) {
    int rc = fs_os()->remove(old_copy);
    if (rc != E_OK && rc != E_NOTFOUND) app_status_note("Could not delete the old copy %s (%s)", old_copy, e_errname(rc));
  }
  app_update_title();
  app_update_status();
  return 1;
}

// =================================================================================================================
// selftest (st_docio): every fixture kind on the fixture folder (C: or -fixdir M:), byte-exact save round trips,
// Save As with conversion and the loss prompt, reload-as, revert, read-only target, failures, close prompts, an
// asynchronous save, and an on-demand document (local disks only).

static int doc_bytes(Document* d, EBytes* out)
{
  epos n = d->length();
  out->clear();
  if (out->resize(n) != E_OK) return E_NOMEM;
  return n ? d->read(0, out->data(), n) : E_OK;
}

static int bytes_eq(const EBytes& a, const u8* p, u32 n)
{
  return a.size() == n && (n == 0 || memcmp(a.data(), p, n) == 0);
}

static int file_eq(StCtx* c, const char* path, const u8* p, u32 n)
{
  EBytes b;
  if (st_file_bytes(c, path, &b) != E_OK) return 0;
  return bytes_eq(b, p, n);
}

static int file_eq_doc(StCtx* c, const char* path, Document* d)
{
  EBytes a, b;
  if (doc_bytes(d, &a) != E_OK) return 0;
  if (st_file_bytes(c, path, &b) != E_OK) return 0;
  return a.size() == b.size() && (a.size() == 0 || memcmp(a.data(), b.data(), a.size()) == 0);
}

struct StFix { const char* name; const char* bytes; u32 n; u8 kind; u16 cp; u8 bom; epos origin; u8 eol; u8 mixed;
               u8 hex; };

static void st_remove(const char* path)
{
  FsStat st;
  if (fs_os()->stat(path, &st) == E_OK && st.exists) {
    if (st.attrs & FSA_READONLY) fs_os()->set_attrs(path, st.attrs & ~(u32)FSA_READONLY);
    fs_os()->remove(path);
  }
}

// Removes ~PC*.TMP / ~PO*.TMP leftovers in dir; returns how many there were.
static u32 st_clean_temps(const char* dir)
{
  FsFind f;
  u32 n = 0;
  Vec<char*> names;
  u32 i;
  if (fs_os()->find_first(dir, &f) == E_OK) {
    do {
      if ((es_nicmp(f.name, "~PC", 3) == 0 || es_nicmp(f.name, "~PO", 3) == 0) && !(f.attrs & FSA_DIR)) {
        char* p = (char*)e_alloc(300);
        if (p) { ui_path_join(dir, f.name, p, 300); names.push(p); }
      }
    } while (fs_os()->find_next(&f) == E_OK);
    fs_os()->find_close(&f);
  }
  for (i = 0; i < names.size(); i++) { st_remove(names[i]); e_free(names[i]); n++; }
  return n;
}

static void st_close_all(StCtx* c)
{
  int i;
  for (i = app_tab_count() - 1; i >= 0; i--) docio_close(app_tab(i), 0);
  st_pump(c, 10);
}

static void append_unit(Document* d, char ch, u8* tmp, u32* n)
{
  const EncSpec& e = d->enc();
  if (e.kind == EK_UTF16LE) { tmp[0] = (u8)ch; tmp[1] = 0; *n = 2; }
  else if (e.kind == EK_UTF16BE) { tmp[0] = 0; tmp[1] = (u8)ch; *n = 2; }
  else { tmp[0] = (u8)ch; *n = 1; }
}

static void st_fixture_kinds(StCtx* c)
{
  static const StFix fx[] = {
    { "ansi.txt",    "caf\xE9 line1\r\nline2\r\n", 19, EK_SBCS, 1252, 0, 0, EOL_CRLF, 0, 0 },
    { "utf8.txt",    "h\xC3\xA9llo\nw\xD0\x96rld\n", 14, EK_UTF8, 65001, 0, 0, EOL_LF, 0, 0 },
    { "utf8bom.txt", "\xEF\xBB\xBF" "abc\r\ndef", 11, EK_UTF8, 65001, 1, 3, EOL_CRLF, 0, 0 },
    { "u16le.txt",   "\xFF\xFE" "a\0\r\0\n\0b\0", 10, EK_UTF16LE, 1200, 1, 2, EOL_CRLF, 0, 0 },
    { "u16be.txt",   "\xFE\xFF" "\0a\0\n\0b", 8, EK_UTF16BE, 1201, 1, 2, EOL_LF, 0, 0 },
    { "cr.txt",      "one\rtwo\rthree", 13, EK_SBCS, 1252, 0, 0, EOL_CR, 0, 0 },
    { "mixed.txt",   "a\r\nb\nc\r\n", 8, EK_SBCS, 1252, 0, 0, EOL_CRLF, 1, 0 },
    { "binary.bin",  "MZ\x90\0\x03\0\0\0\x04\0\xFF\xFF", 12, EK_SBCS, 1252, 0, 0, EOL_CRLF, 0, 1 },
    { "bad8.txt",    "ok \xC3\x28 \xFF\n", 8, EK_SBCS, 1252, 0, 0, EOL_LF, 0, 0 },
    { "empty.txt",   "", 0, EK_SBCS, 1252, 0, 0, EOL_CRLF, 0, 0 },
    { "dos.nfo",     "\xC9\xCD\xBB\r\n\xBA \xBA\r\n", 10, EK_SBCS, 437, 0, 0, EOL_CRLF, 0, 0 }
  };
  u32 i;
  for (i = 0; i < sizeof(fx) / sizeof(fx[0]); i++) {
    const StFix& f = fx[i];
    char path[300];
    Tab* t;
    EBytes b;
    u8 tmp[4], expect[64];
    u32 n = 0;
    int rc;
    if (st_fixture(c, f.name, f.bytes, f.n, path, sizeof(path)) != E_OK) continue;
    t = st_open(c, path);
    st_checkf(c, t != 0, "%s: opened", f.name);
    if (!t) continue;
    st_checkf(c, t->doc->enc().kind == f.kind && t->doc->enc().cp == f.cp && t->doc->enc().bom == f.bom,
              "%s: encoding kind %u cp %u bom %u (want %u %u %u)", f.name, t->doc->enc().kind, t->doc->enc().cp,
              t->doc->enc().bom, f.kind, f.cp, f.bom);
    st_checkf(c, t->doc->text_origin() == f.origin, "%s: origin %u (want %u)", f.name, t->doc->text_origin(), f.origin);
    st_checkf(c, (int)t->doc->eol_style() == (int)f.eol, "%s: eol %d (want %d)", f.name, (int)t->doc->eol_style(), f.eol);
    st_checkf(c, (t->doc->eol_mixed() ? 1 : 0) == f.mixed, "%s: mixed %d", f.name, t->doc->eol_mixed() ? 1 : 0);
    st_checkf(c, (t->view == VIEW_HEX) == (f.hex != 0), "%s: view %d", f.name, t->view);
    st_checkf(c, !t->doc->modified() && !t->doc->read_only(), "%s: clean, editable", f.name);
    st_checkf(c, doc_bytes(t->doc, &b) == E_OK && bytes_eq(b, (const u8*)f.bytes, f.n), "%s: document bytes == file",
              f.name);
    // unmodified save rewrites the same bytes
    rc = docio_save_wait(t, 0, 0, 0);
    st_checkf(c, rc == E_OK && file_eq(c, path, (const u8*)f.bytes, f.n), "%s: unmodified save byte-exact (%s)",
              f.name, e_errname(rc));
    // edit + save
    append_unit(t->doc, 'Z', tmp, &n);
    if (f.hex) rc = t->doc->replace(2, 1, (const u8*)"\x91", 1);
    else rc = t->doc->replace(t->doc->length(), 0, tmp, n);
    st_checkf(c, rc == E_OK && t->doc->modified(), "%s: edit (%s)", f.name, e_errname(rc));
    rc = docio_save_wait(t, 0, 0, 0);
    st_checkf(c, rc == E_OK && !t->doc->modified() && file_eq_doc(c, path, t->doc), "%s: saved bytes == document (%s)",
              f.name, e_errname(rc));
    if (f.n + n <= sizeof(expect)) {
      memcpy(expect, f.bytes, f.n);
      if (f.hex) expect[2] = 0x91;
      else { memcpy(expect + f.n, tmp, n); }
      st_checkf(c, file_eq(c, path, expect, f.hex ? f.n : f.n + n), "%s: exact expected bytes on disk", f.name);
    }
    st_checkf(c, t->disk.exists && t->disk.size_lo == t->doc->length(), "%s: disk state recorded (%u)", f.name,
              t->disk.size_lo);
    // undo past the save point -> modified; close asks, No keeps the file
    if (t->doc->can_undo()) {
      Selection s;
      t->doc->undo(&s);
      st_checkf(c, t->doc->modified(), "%s: modified after undo", f.name);
      st_answer(c, Q_SAVE_CHANGES, IDNO);
      st_checkf(c, docio_close(t, 1) == 1, "%s: close (No)", f.name);
      if (f.n + n <= sizeof(expect))
        st_checkf(c, file_eq(c, path, expect, f.hex ? f.n : f.n + n), "%s: No left the file alone", f.name);
    } else {
      docio_close(t, 0);
    }
    // reopen: the saved file is detected the same way and loads byte-exact
    t = st_open(c, path);
    if (t) {
      st_checkf(c, t->doc->enc().kind == f.kind && t->doc->enc().cp == f.cp && t->doc->enc().bom == f.bom &&
                (f.n == 0 || (int)t->doc->eol_style() == (int)f.eol) && file_eq_doc(c, path, t->doc),
                "%s: reopened with the same encoding/EOL, bytes == file", f.name);
      docio_close(t, 0);
    }
    st_remove(path);
  }
}

static void st_saveas_convert(StCtx* c)
{
  char src[300], dst[300], dst2[300];
  Tab* t;
  EncSpec u8e = enc_utf8(0), ansi = enc_ansi();
  int lf = EOL_LF, rc;
  static const char ansi_bytes[] = "caf\xE9 line1\r\nline2\r\n";
  static const char want_utf8[] = "caf\xC3\xA9 line1\nline2\n";
  static const char utf8_bytes[] = "h\xC3\xA9llo\nw\xD0\x96rld\n";
  static const char want_ansi[] = "h\xE9llo\nw?rld\n";
  // ANSI CRLF -> UTF-8 LF via Save As
  st_fixture(c, "sa_src.txt", ansi_bytes, 19, src, sizeof(src));
  ui_path_join(c->fixdir, "sa_dst.txt", dst, sizeof(dst));
  st_remove(dst);
  t = st_open(c, src);
  if (t) {
    rc = docio_save_wait(t, dst, &u8e, &lf);
    st_checkf(c, rc == E_OK, "save as utf8/lf: %s", e_errname(rc));
    st_checkf(c, file_eq(c, dst, (const u8*)want_utf8, 18), "save as: converted bytes exact");
    st_checkf(c, file_eq(c, src, (const u8*)ansi_bytes, 19), "save as: source file untouched");
    st_checkf(c, ui_path_equal(t->path, dst) && !t->doc->modified() && t->doc->enc().kind == EK_UTF8 &&
              !t->doc->enc().bom && t->doc->eol_style() == EOL_LF, "save as: tab now on the new file, utf8, clean");
    st_checkf(c, g_app.cfg.mru[0][0] && ui_path_equal(g_app.cfg.mru[0], dst), "save as: MRU updated");
    // undo goes back past the conversion: the document is modified again (the disk keeps the new file)
    docio_close(t, 0);
  }
  // UTF-8 with Cyrillic -> ANSI: loss prompt; Cancel writes nothing and changes nothing
  st_fixture(c, "sa_u8.txt", utf8_bytes, 14, src, sizeof(src));
  ui_path_join(c->fixdir, "sa_ansi.txt", dst2, sizeof(dst2));
  st_remove(dst2);
  t = st_open(c, src);
  if (t) {
    st_answer(c, Q_CONVERT_LOSS, IDCANCEL);
    rc = docio_save_wait(t, dst2, &ansi, 0);
    st_checkf(c, rc == E_CANCEL, "loss prompt cancel -> E_CANCEL (%s)", e_errname(rc));
    st_checkf(c, !ui_file_exists(dst2), "loss prompt cancel: nothing written");
    st_checkf(c, t->doc->enc().kind == EK_UTF8 && !t->doc->modified(), "loss prompt cancel: document unchanged");
    st_answer(c, Q_CONVERT_LOSS, IDOK);
    rc = docio_save_wait(t, dst2, &ansi, 0);
    st_checkf(c, rc == E_OK && file_eq(c, dst2, (const u8*)want_ansi, 12), "loss prompt OK: '?' written (%s)",
              e_errname(rc));
    docio_close(t, 0);
  }
  st_remove(src); st_remove(dst); st_remove(dst2);
  ui_path_join(c->fixdir, "sa_src.txt", src, sizeof(src));
  st_remove(src);
}

static void st_reload_revert(StCtx* c)
{
  char path[300];
  Tab* t;
  Tab* u;
  EBytes b;
  int rc;
  static const char utf8_bytes[] = "h\xC3\xA9llo\nw\xD0\x96rld\n";
  st_fixture(c, "rl.txt", utf8_bytes, 14, path, sizeof(path));
  t = st_open(c, path);
  if (t) {
    rc = doccmd_reload_as(t, enc_ansi());
    st_checkf(c, rc == E_OK && t->doc->enc().kind == EK_SBCS && !t->doc->modified() && t->doc->length() == 14,
              "reload as ANSI (unmodified): no question, bytes kept (%s)", e_errname(rc));
    st_checkf(c, doc_bytes(t->doc, &b) == E_OK && bytes_eq(b, (const u8*)utf8_bytes, 14), "reload as: bytes unchanged");
    // modified: asks; Cancel keeps the edit
    t->doc->replace(0, 0, (const u8*)"X", 1);
    st_answer(c, Q_RELOAD_DISCARD, IDCANCEL);
    rc = doccmd_reload_as(t, enc_utf8(0));
    st_checkf(c, rc == E_CANCEL && t->doc->modified() && t->doc->enc().kind == EK_SBCS, "reload as modified: cancel");
    st_answer(c, Q_RELOAD_DISCARD, IDOK);
    rc = doccmd_reload_as(t, enc_utf8(0));
    st_checkf(c, rc == E_OK && !t->doc->modified() && t->doc->enc().kind == EK_UTF8 && t->doc->length() == 14,
              "reload as modified: OK rereads as UTF-8 (%s)", e_errname(rc));
    // the forced encoding sticks for later reloads (revert)
    t->doc->replace(0, 0, (const u8*)"Y", 1);
    st_answer(c, Q_REVERT, IDOK);
    rc = docio_revert(t);
    st_checkf(c, rc == E_OK && !t->doc->modified() && file_eq_doc(c, path, t->doc) && t->doc->enc().kind == EK_UTF8,
              "revert: disk bytes, forced encoding kept (%s)", e_errname(rc));
    t->doc->replace(0, 0, (const u8*)"Y", 1);
    st_answer(c, Q_REVERT, IDCANCEL);
    rc = docio_revert(t);
    st_checkf(c, rc == E_CANCEL && t->doc->modified(), "revert cancel keeps the edit");
    t->doc->undo(&t->sel);
    docio_close(t, 0);
  }
  // untitled: reload-as reinterprets the bytes (BOM recognised)
  if (docio_new(&u) == E_OK && u) {
    rc = u->doc->replace(0, 0, (const u8*)"\xFF\xFE" "a\0b\0", 6);
    rc = doccmd_reload_as(u, enc_utf16le(1));
    st_checkf(c, rc == E_OK && u->doc->enc().kind == EK_UTF16LE && u->doc->enc().bom && u->doc->text_origin() == 2,
              "untitled reload as UTF-16 LE: reinterpreted, BOM origin 2 (%s)", e_errname(rc));
    st_checkf(c, u->doc->length() == 6, "untitled reinterpret keeps bytes");
    docio_close(u, 0);
  }
  st_remove(path);
}

static void st_convert_cmds(StCtx* c)
{
  char path[300];
  Tab* t;
  EBytes b;
  Selection s;
  int rc;
  static const char ansi_bytes[] = "caf\xE9\r\nx";
  static const char want_bom[] = "\xEF\xBB\xBF" "caf\xC3\xA9\r\nx";
  static const char mixed[] = "a\r\nb\nc\r\n";
  st_fixture(c, "cv.txt", ansi_bytes, 7, path, sizeof(path));
  t = st_open(c, path);
  if (t) {
    rc = doccmd_convert(t, enc_utf8(1), 1);
    st_checkf(c, rc == E_OK && t->doc->modified() && t->doc->enc().bom && t->doc->text_origin() == 3,
              "convert to UTF-8 BOM (%s)", e_errname(rc));
    st_checkf(c, doc_bytes(t->doc, &b) == E_OK && bytes_eq(b, (const u8*)want_bom, 11), "convert: exact bytes");
    rc = t->doc->undo(&s);
    st_checkf(c, rc == E_OK && !t->doc->modified() && t->doc->enc().kind == EK_SBCS && doc_bytes(t->doc, &b) == E_OK &&
              bytes_eq(b, (const u8*)ansi_bytes, 7), "convert undo restores bytes and encoding");
    docio_close(t, 0);
  }
  st_remove(path);
  st_fixture(c, "eol.txt", mixed, 8, path, sizeof(path));
  t = st_open(c, path);
  if (t) {
    rc = doccmd_convert_eol(t, EOL_LF, 1);
    st_checkf(c, rc == E_OK && !t->doc->eol_mixed() && t->doc->eol_style() == EOL_LF && doc_bytes(t->doc, &b) == E_OK &&
              bytes_eq(b, (const u8*)"a\nb\nc\n", 6), "line endings -> LF (%s)", e_errname(rc));
    rc = t->doc->undo(&s);
    st_checkf(c, rc == E_OK && t->doc->eol_mixed() && doc_bytes(t->doc, &b) == E_OK && bytes_eq(b, (const u8*)mixed, 8),
              "line endings undo restores mixed bytes");
    docio_close(t, 0);
  }
  st_remove(path);
}

static void st_readonly_and_failures(StCtx* c)
{
  char path[300], bad[300], lock[300];
  Tab* t;
  FsStat st;
  FsFile h;
  int rc;
  static const char orig[] = "read only\r\n";
  st_fixture(c, "ro.txt", orig, 11, path, sizeof(path));
  fs_os()->stat(path, &st);
  fs_os()->set_attrs(path, st.attrs | FSA_READONLY);
  t = st_open(c, path);
  if (t) {
    st_checkf(c, t->doc->read_only() && t->ro_attr, "read-only file opens read-only");
    st_checkf(c, t->doc->replace(0, 0, (const u8*)"x", 1) == E_READONLY, "edits refused");
    t->doc->set_read_only(false);                      // "Edit anyway"
    t->doc->replace(0, 0, (const u8*)"x", 1);
    st_answer(c, Q_CLEAR_READONLY, IDCANCEL);
    rc = docio_save_wait(t, 0, 0, 0);
    st_checkf(c, rc == E_CANCEL && file_eq(c, path, (const u8*)orig, 11) && t->doc->modified(), "RO save: cancel");
    st_answer(c, Q_CLEAR_READONLY, IDNO);
    rc = docio_save_wait(t, 0, 0, 0);
    st_checkf(c, rc == E_READONLY && file_eq(c, path, (const u8*)orig, 11), "RO save: No -> E_READONLY (Save As)");
    st_answer(c, Q_CLEAR_READONLY, IDYES);
    rc = docio_save_wait(t, 0, 0, 0);
    fs_os()->stat(path, &st);
    st_checkf(c, rc == E_OK && !(st.attrs & FSA_READONLY) && file_eq_doc(c, path, t->doc) && !t->doc->modified(),
              "RO save: Yes clears the attribute and saves (%s)", e_errname(rc));
    // save into a folder that does not exist: message, still modified
    t->doc->replace(0, 0, (const u8*)"y", 1);
    ui_path_join(c->fixdir, "no_such_dir\\x.txt", bad, sizeof(bad));
    st_answer(c, Q_SAVE_FAILED, IDOK);
    rc = docio_save_wait(t, bad, 0, 0);
    st_checkf(c, rc == E_NOTFOUND && t->doc->modified() && ui_path_equal(t->path, path) && !t->saving &&
              t->doc->editable(), "save to a missing folder fails cleanly (%s)", e_errname(rc));
    docio_close(t, 0);
  }
  st_remove(path);
  // a target another program holds open (deny write): the save fails or goes in place, never a partial file
  st_fixture(c, "lock.txt", orig, 11, lock, sizeof(lock));
  t = st_open(c, lock);
  if (t && fs_os()->open(lock, FS_READ, &h) == E_OK) {
    t->doc->replace(0, 0, (const u8*)"z", 1);
    st_answer(c, Q_SAVE_FAILED, IDOK);
    rc = docio_save_wait(t, 0, 0, 0);
    fs_os()->close(h);
    if (rc == E_OK) st_checkf(c, file_eq_doc(c, lock, t->doc), "locked target: in-place save complete");
    else st_checkf(c, file_eq(c, lock, (const u8*)orig, 11) && t->doc->modified(),
                   "locked target: save failed, file unchanged, still modified (%s)", e_errname(rc));
    st_checkf(c, st_clean_temps(c->fixdir) == 0, "locked target: no redundant temp/old copies left");
    ui_answer_clear();
    docio_close(t, 0);
  } else if (t) docio_close(t, 0);
  st_remove(lock);
  st_clean_temps(c->fixdir);
}

static void st_close_and_new(StCtx* c)
{
  char path[300];
  Tab* t;
  Tab* u;
  Tab* v;
  int n0, rc;
  static const char orig[] = "close me\n";
  st_fixture(c, "close.txt", orig, 9, path, sizeof(path));
  t = st_open(c, path);
  if (t) {
    st_checkf(c, st_open(c, path) == t, "opening an open file activates its tab");
    t->doc->replace(0, 0, (const u8*)"q", 1);
    st_answer(c, Q_SAVE_CHANGES, IDCANCEL);
    st_checkf(c, docio_close(t, 1) == 0 && app_tab_index(t) >= 0, "close: Cancel keeps the tab");
    st_answer(c, Q_SAVE_CHANGES, IDYES);
    st_checkf(c, docio_close(t, 1) == 1, "close: Yes saves and closes");
    st_checkf(c, file_eq(c, path, (const u8*)"qclose me\n", 10), "close: Yes wrote the file");
  }
  // new documents: numbering, settings defaults, no BOM
  st_close_all(c);
  rc = docio_new(&u);
  rc = rc == E_OK ? docio_new(&v) : rc;
  st_checkf(c, rc == E_OK && u && v && v->untitled_no == u->untitled_no + 1, "new: untitled numbering");
  if (u) st_checkf(c, enc_equal(u->doc->enc(), g_app.cfg.new_enc) || !codec_get(g_app.cfg.new_enc), "new: Config encoding");
  if (u) st_checkf(c, u->doc->length() == u->doc->text_origin() && (int)u->doc->eol_style() == (int)g_app.cfg.new_eol,
                   "new: empty, Config line ending");
  // tab reuse: an untouched new tab is replaced by an opened file
  n0 = app_tab_count();
  t = st_open(c, path);
  st_checkf(c, t && app_tab_count() == n0 && app_tab_index(v) < 0, "open replaces the untouched new tab");
  // closing a modified untitled document with Yes runs Save As
  if (docio_new(&u) == E_OK && u) {
    char upath[300];
    ui_path_join(c->fixdir, "untitled_saved.txt", upath, sizeof(upath));
    st_remove(upath);
    u->doc->replace(u->doc->length(), 0, (const u8*)"hello", 5);
    st_answer(c, Q_SAVE_CHANGES, IDYES);
    filecmd_selftest_saveas(upath);
    st_checkf(c, docio_close(u, 1) == 1 && file_eq(c, upath, (const u8*)"hello", 5), "untitled close: Yes -> Save As");
    st_remove(upath);
  }
  st_close_all(c);
  st_remove(path);
}

static void st_async_save(StCtx* c)
{
  char path[300];
  Tab* t;
  u8* big;
  u32 i, n = 2u * 1024u * 1024u;
  int rc;
  big = (u8*)e_valloc(n);
  if (!big) { ST_CHECK(c, 0, "2 MB buffer"); return; }
  for (i = 0; i < n; i++) big[i] = (u8)((i % 79) == 78 ? '\n' : 'a' + (i * 7 % 26));
  rc = st_fixture(c, "big2mb.txt", big, n, path, sizeof(path));
  e_vfree(big, n);
  if (rc != E_OK) return;
  t = st_open(c, path);
  if (!t) return;
  t->doc->replace(5, 0, (const u8*)"ASYNC", 5);
  rc = docio_save(t, 0, 0, 0, 0, 0);
  st_checkf(c, rc == E_OK && t->saving && !t->doc->editable(), "async save started, document busy (%s)", e_errname(rc));
  st_checkf(c, t->doc->replace(0, 0, (const u8*)"!", 1) == E_BUSY, "edits refused while saving");
  st_checkf(c, docio_save(t, 0, 0, 0, 0, 0) == E_BUSY, "second save refused while saving");
  st_pump(c, 30);
  rc = docio_wait_save(t);
  st_checkf(c, rc == E_OK && !t->saving && !t->doc->modified() && t->doc->editable(), "async save done (%s)",
            e_errname(rc));
  st_checkf(c, file_eq_doc(c, path, t->doc), "2 MB saved bytes == document");
  docio_close(t, 0);
  st_remove(path);
}

static void st_ondemand(StCtx* c)
{
  char path[300];
  Tab* t;
  u8* big;
  u32 i, n = 5u * 1024u * 1024u + 77u, lines = 0;
  i32 saved_mb = g_app.cfg.ondemand_mb;
  int rc;
  if (fs_os()->is_remote(c->fixdir)) { selftest_log("on-demand part skipped on a network folder"); return; }
  big = (u8*)e_valloc(n);
  if (!big) { ST_CHECK(c, 0, "5 MB buffer"); return; }
  for (i = 0; i < n; i++) { big[i] = (u8)((i % 61) == 60 ? '\n' : '0' + (i % 10)); if (big[i] == '\n') lines++; }
  rc = st_fixture(c, "od5mb.txt", big, n, path, sizeof(path));
  e_vfree(big, n);
  if (rc != E_OK) return;
  g_app.cfg.ondemand_mb = 4;
  t = st_open(c, path);
  g_app.cfg.ondemand_mb = saved_mb;
  if (!t) { st_remove(path); return; }
  st_checkf(c, t->ondemand && t->fsrc && t->large && !t->doc->indexing(), "5 MB opens on demand and is indexed");
  st_checkf(c, t->doc->line_count() == lines + 1, "on-demand line count %u (want %u)", t->doc->line_count(), lines + 1);
  rc = t->doc->replace(100, 0, (const u8*)"OD1", 3);
  rc = docio_save_wait(t, 0, 0, 0);
  st_checkf(c, rc == E_OK && file_eq_doc(c, path, t->doc), "on-demand save 1 byte-exact (%s)", e_errname(rc));
  t->doc->replace(4u * 1024u * 1024u, 0, (const u8*)"OD2", 3);
  rc = docio_save_wait(t, 0, 0, 0);
  st_checkf(c, rc == E_OK && file_eq_doc(c, path, t->doc), "on-demand save 2 byte-exact (%s)", e_errname(rc));
  st_checkf(c, doccmd_convert(t, enc_utf8(1), 0) == E_TOOBIG, "on-demand convert refused");
  docio_close(t, 0);
  st_checkf(c, st_clean_temps(c->fixdir) == 0, "on-demand close deleted the old copy (no ~P*.TMP left)");
  st_remove(path);
}

void st_docio(StCtx* c)
{
  st_close_all(c);
  selftest_log("fixtures in %s", c->fixdir);
  st_fixture_kinds(c);
  st_saveas_convert(c);
  st_reload_revert(c);
  st_convert_cmds(c);
  st_readonly_and_failures(c);
  st_close_and_new(c);
  st_async_save(c);
  st_ondemand(c);
  st_close_all(c);
  st_checkf(c, st_clean_temps(c->fixdir) == 0, "no temp files left in %s", c->fixdir);
}
