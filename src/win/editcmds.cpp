// editcmds.cpp - Edit menu, line, bookmark, go to, brace and Options commands (DESIGN 15.4). Owner: T12.
//
// The commands work on the active tab. Tab::sel is the selection both views share: commands read it, change the
// document through the core ed_* functions (one undo group each) and hand the new selection back to the active
// view (*_set_selection), which also keeps Tab::sel. In the hex view undo/redo/cut/copy/paste/delete/select all
// work; line commands, bookmarks and brace matching are text-view only (greyed).
// Line keys follow Notepad++/VC6: Ctrl+D duplicate, Ctrl+L cut line(s) (to the clipboard), Ctrl+Shift+L delete
// line(s), Ctrl+Shift+Up/Down move line(s), Ctrl+Q toggle comment.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "clip.h"
#include "dialogs.h"
#include "selftest.h"
#include "views.h"
#include "docio.h"
#include "notifybar.h"
#include "hexfmt.h"
#include "edlines_int.h"
#include "resource.h"
#include "estr.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// helpers

static Tab* ec_tab()
{
  Tab* t = app_active();
  return (t && t->doc) ? t : 0;
}

static int ec_is_text(Tab* t) { return t && t->view == VIEW_TEXT; }

static void ec_ctx(Tab* t, EdCtx* e)
{
  memset(e, 0, sizeof(*e));
  e->doc = t->doc;
  e->sel = t->sel;
  ed_lang_from(t->lang, t->tv.tab_width, t->tv.use_spaces, &e->lang);
  if (!g_app.cfg.use_lang_tabs || !t->lang) {   // the global/tab settings, not the language's
    e->lang.tab_width = t->tv.tab_width ? t->tv.tab_width : 4;
    e->lang.use_spaces = t->tv.use_spaces ? 1 : 0;
  }
  e->overwrite = t->tv.overwrite;
  e->auto_indent = g_app.cfg.auto_indent;
  e->now_ms = app_now_ms();
}

// Applies a selection to the tab and the view that shows it.
static void ec_set_sel(Tab* t, epos anchor, epos caret, int scroll)
{
  t->sel.anchor = anchor;
  t->sel.caret = caret;
  if (t != app_active()) return;
  if (t->view == VIEW_TEXT) { if (g_app.textview) textview_set_selection(g_app.textview, anchor, caret, scroll); }
  else {
    t->hv.caret.pos = caret;
    t->hv.caret.nibble = 0;
    if (g_app.hexview) hexview_set_selection(g_app.hexview, anchor, caret, scroll);
  }
}

// Status note + beep for a refused edit. Returns rc.
static int ec_report(int rc)
{
  const char* msg = 0;
  switch (rc) {
  case E_OK: return rc;
  case E_READONLY: msg = "The document is read-only (Document > Read-Only)"; break;
  case E_BUSY: msg = "The document is busy (saving or indexing); try again in a moment"; break;
  case E_NOMEM: msg = "Not enough memory: close some tabs"; break;
  case E_ALIGN: msg = "UTF-16 documents are edited in whole 2-byte units"; break;
  case E_TOOBIG: msg = "The document would become too large"; break;
  case E_RANGE: msg = "A character can't be stored in this encoding"; break;
  case E_UNSUPPORTED: msg = "This language has no comment syntax"; break;
  default: break;
  }
  MessageBeep(MB_OK);
  if (msg) app_status_note("%s", msg);
  else app_status_note("Edit failed (%s)", e_errname(rc));
  return rc;
}

// After an edit through an EdCtx: apply its selection, refresh title/status.
static int ec_done(Tab* t, const EdCtx& e, int rc, int scroll)
{
  if (rc != E_OK) return ec_report(rc);
  ec_set_sel(t, e.sel.anchor, e.sel.caret, scroll);
  app_on_doc_changed(t);
  app_update_status();
  return E_OK;
}

static int ec_line_op(int which, int arg)
{
  Tab* t = ec_tab();
  EdCtx e;
  int rc;
  if (!ec_is_text(t)) return E_STATE;
  ec_ctx(t, &e);
  switch (which) {
  case 0: rc = ed_duplicate(&e); break;
  case 1: rc = ed_delete_lines(&e); break;
  case 2: rc = ed_move_lines(&e, arg); break;
  case 3: rc = ed_toggle_comment(&e); break;
  case 4: rc = ed_change_case(&e, arg); break;
  default: rc = ed_indent(&e, arg); break;
  }
  return ec_done(t, e, rc, SCROLL_MIN);
}

// ---------------------------------------------------------------------------------------------------------------
// undo / redo

static void ec_undo(int redo)
{
  Tab* t = ec_tab();
  Selection s;
  int rc;
  if (!t) return;
  if (redo ? !t->doc->can_redo() : !t->doc->can_undo()) { MessageBeep(MB_OK); return; }
  rc = redo ? t->doc->redo(&s) : t->doc->undo(&s);
  if (rc != E_OK) { ec_report(rc); return; }
  if (t->view == VIEW_TEXT) {                    // the text view never shows the caret inside the BOM
    epos o = t->doc->text_origin();
    if (s.anchor < o) s.anchor = o;
    if (s.caret < o) s.caret = o;
  }
  if (s.anchor > t->doc->length()) s.anchor = t->doc->length();
  if (s.caret > t->doc->length()) s.caret = t->doc->length();
  ec_set_sel(t, s.anchor, s.caret, SCROLL_MIN);
  app_on_doc_changed(t);
  app_update_status();
}

void cmd_edit_undo(void) { ec_undo(0); }
void cmd_edit_redo(void) { ec_undo(1); }

// ---------------------------------------------------------------------------------------------------------------
// clipboard commands

static int ec_copy(Tab* t, epos lo, epos hi)
{
  Selection s;
  int rc;
  s.anchor = lo; s.caret = hi;
  rc = clip_copy(t, s, t->view == VIEW_HEX && t->hv.caret.pane == 0);
  if (rc == E_TOOBIG) ui_error("Selection too large to copy (16 MB max).");
  else if (rc == E_BUSY) ui_error("The clipboard is in use by another program. Try again.");
  else if (rc == E_NOMEM) ui_error("Not enough memory to copy the selection.");
  else if (rc < 0 && rc != E_NOTFOUND) ui_error("Copy failed (%s).", e_errname(rc));
  return rc;
}

static int ec_delete_sel(Tab* t)                // selection -> nothing (one undo group)
{
  Document* d = t->doc;
  epos lo = sel_lo(t->sel), hi = sel_hi(t->sel);
  int rc;
  if (lo >= hi) return E_OK;
  if (t->view == VIEW_HEX) {
    HexCaret c = t->hv.caret;
    Selection s = t->sel;
    c.pos = t->sel.caret;
    rc = hx_delete(d, &c, &s, 0);
    if (rc == E_OK) { t->hv.caret = c; ec_set_sel(t, s.anchor, s.caret, SCROLL_MIN); }
  } else {
    Selection after;
    after.anchor = after.caret = lo;
    d->begin_edit(UK_OTHER, t->sel, app_now_ms());
    rc = d->replace(lo, hi - lo, 0, 0);
    d->end_edit(rc == E_OK ? after : t->sel);
    if (rc == E_OK) ec_set_sel(t, lo, lo, SCROLL_MIN);
  }
  if (rc != E_OK) return ec_report(rc);
  app_on_doc_changed(t);
  app_update_status();
  return E_OK;
}

void cmd_edit_copy(void)
{
  Tab* t = ec_tab();
  if (!t || sel_empty(t->sel)) return;
  ec_copy(t, sel_lo(t->sel), sel_hi(t->sel));
}

void cmd_edit_cut(void)
{
  Tab* t = ec_tab();
  if (!t || sel_empty(t->sel)) return;
  if (!t->doc->editable()) { ec_report(t->doc->read_only() ? E_READONLY : E_BUSY); return; }
  if (ec_copy(t, sel_lo(t->sel), sel_hi(t->sel)) != E_OK) return;
  ec_delete_sel(t);
}

// Text view paste: raw bytes when they come from Pica in the same encoding, else the text (EOLs normalised,
// unrepresentable characters become '?' and the notification bar offers UTF-8).
static int ec_paste_text(Tab* t, ClipData* cd)
{
  EdCtx e;
  u32 replaced = 0;
  int rc = E_ALIGN;
  ec_ctx(t, &e);
  if (cd->has_raw && enc_equal(cd->enc, t->doc->enc())) rc = ed_insert_bytes(&e, cd->raw.data(), cd->raw.size());
  if (rc == E_ALIGN) {                            // text: bad bytes of the source become U+FFFD
    u32 i;
    for (i = 0; i < cd->text.size(); i++) if (ECP_IS_BAD(cd->text[i])) cd->text[i] = ECP_REPLACEMENT;
    ec_ctx(t, &e);
    rc = ed_insert_text(&e, cd->text.data(), cd->text.size(), &replaced);
  }
  rc = ec_done(t, e, rc, SCROLL_MIN);
  if (rc == E_OK && replaced) {
    char lab[40], msg[200];
    enc_label(t->doc->enc(), lab, sizeof(lab));
    es_fmt(msg, sizeof(msg), "%u pasted character%s became '?' (not in %s).", replaced, replaced == 1 ? "" : "s", lab);
    doccmd_offer_utf8(t, msg);
  }
  return rc;
}

// Hex view paste: raw bytes; else hex text in the hex pane; else the text encoded in the document encoding.
static int ec_paste_hex(Tab* t, ClipData* cd)
{
  EBytes bytes;
  HexCaret c = t->hv.caret;
  Selection s = t->sel;
  const u8* p = 0;
  u32 n = 0, lost = 0;
  int rc;
  if (cd->has_raw) { p = cd->raw.data(); n = cd->raw.size(); }
  else {
    int parsed = 0;
    if (c.pane == 0 && cd->text.size()) {
      Vec<char> a;
      u32 i;
      int ascii = 1;
      for (i = 0; i < cd->text.size() && ascii; i++) {
        if (cd->text[i] >= 0x80) ascii = 0;
        else if (a.push((char)cd->text[i]) != E_OK) return ec_report(E_NOMEM);
      }
      if (ascii && hex_parse_bytes(a.data(), a.size(), &bytes) == E_OK) parsed = 1;
    }
    if (!parsed) {
      bytes.clear();
      rc = codec_encode_str(t->doc->codec(), cd->text.data(), cd->text.size(), &bytes, &lost, '?');
      if (rc != E_OK) return ec_report(rc);
    }
    p = bytes.data(); n = bytes.size();
  }
  if (!n) return E_OK;
  c.pos = t->sel.caret;
  rc = hx_write(t->doc, &c, &s, p, n);
  if (rc != E_OK) return ec_report(rc);
  t->hv.caret = c;
  ec_set_sel(t, s.anchor, s.caret, SCROLL_MIN);
  app_on_doc_changed(t);
  app_update_status();
  if (lost) app_status_note("%u pasted character%s became '?'", lost, lost == 1 ? "" : "s");
  return E_OK;
}

void cmd_edit_paste(void)
{
  Tab* t = ec_tab();
  ClipData cd;
  int rc;
  if (!t) return;
  if (!t->doc->editable()) { ec_report(t->doc->read_only() ? E_READONLY : E_BUSY); return; }
  rc = clip_get(&cd);
  if (rc == E_TOOBIG) ui_error("The clipboard holds more than 16 MB; Pica can't paste that much at once.");
  else if (rc == E_BUSY) ui_error("The clipboard is in use by another program. Try again.");
  else if (rc == E_NOMEM) ui_error("Not enough memory to paste.");
  else if (rc == E_OK) {
    if (t->view == VIEW_HEX) ec_paste_hex(t, &cd);
    else ec_paste_text(t, &cd);
  } else MessageBeep(MB_OK);
  clip_free(&cd);
}

void cmd_edit_delete(void)
{
  Tab* t = ec_tab();
  int rc;
  if (!t) return;
  if (!sel_empty(t->sel)) { ec_delete_sel(t); return; }
  if (t->view == VIEW_HEX) {
    HexCaret c = t->hv.caret;
    Selection s = t->sel;
    c.pos = t->sel.caret;
    rc = hx_delete(t->doc, &c, &s, 0);
    if (rc != E_OK) { ec_report(rc); return; }
    t->hv.caret = c;
    ec_set_sel(t, s.anchor, s.caret, SCROLL_MIN);
    app_on_doc_changed(t);
    app_update_status();
  } else {
    EdCtx e;
    ec_ctx(t, &e);
    rc = ed_delete(&e);
    ec_done(t, e, rc, SCROLL_MIN);
  }
}

void cmd_edit_selectall(void)
{
  Tab* t = ec_tab();
  if (!t) return;
  if (t->view == VIEW_HEX) ec_set_sel(t, 0, t->doc->length(), SCROLL_NONE);
  else ec_set_sel(t, t->doc->text_origin(), t->doc->length(), SCROLL_NONE);
  app_update_status();
}

// ---------------------------------------------------------------------------------------------------------------
// line commands

void cmd_line_dup(void) { ec_line_op(0, 0); }

// Ctrl+Shift+L (Scintilla LINEDELETE).
static void ec_line_delete() { ec_line_op(1, 0); }

// Ctrl+L (Scintilla LINECUT, also VC6): the touched lines go to the clipboard, then they are deleted.
void cmd_line_cut(void)
{
  Tab* t = ec_tab();
  epos a, b;
  if (!ec_is_text(t)) return;
  if (!t->doc->editable()) { ec_report(t->doc->read_only() ? E_READONLY : E_BUSY); return; }
  ed_lines_range(t->doc, t->sel, &a, &b);
  if (a < b && ec_copy(t, a, b) != E_OK) return;
  ec_line_delete();
}

// IDM_LINE_DEL (Ctrl+Shift+L and the menu): delete the touched lines without touching the clipboard. Ctrl+L sends
// IDM_LINE_CUT (cmd_line_cut).
void cmd_line_del(void) { ec_line_delete(); }

void cmd_line_up(void) { ec_line_op(2, 0); }
void cmd_line_down(void) { ec_line_op(2, 1); }
void cmd_edit_comment(void) { ec_line_op(3, 0); }
void cmd_edit_upper(void) { ec_line_op(4, 1); }
void cmd_edit_lower(void) { ec_line_op(4, 0); }
void cmd_edit_indent(void) { ec_line_op(5, 0); }
void cmd_edit_unindent(void) { ec_line_op(5, 1); }

void cmd_edit_overwrite(void)
{
  Tab* t = ec_tab();
  if (!t) return;
  if (t->view == VIEW_HEX) t->hv.caret.insert = (u8)(t->hv.caret.insert ? 0 : 1);
  else t->tv.overwrite = (u8)(t->tv.overwrite ? 0 : 1);
  if (t->view == VIEW_HEX) { if (g_app.hexview) { hexview_invalidate(g_app.hexview); hexview_set_selection(g_app.hexview, t->sel.anchor, t->sel.caret, SCROLL_NONE); } }
  else if (g_app.textview) { textview_invalidate(g_app.textview); textview_set_selection(g_app.textview, t->sel.anchor, t->sel.caret, SCROLL_NONE); }
  app_update_status();
}

// ---------------------------------------------------------------------------------------------------------------
// go to, brace, bookmarks, options

void cmd_search_goto(void)
{
  Tab* t = ec_tab();
  if (!t) return;
  dlg_goto(t, t->view == VIEW_HEX);
}

void cmd_search_brace(void)
{
  Tab* t = ec_tab();
  epos brace, match;
  int rc;
  if (!ec_is_text(t)) return;
  rc = ed_match_brace(t->doc, t->sel.caret, t->hl, t->lang ? t->lang->braces : "()[]{}", &brace, &match);
  if (rc == 1) ec_set_sel(t, match, match, SCROLL_MIN);
  else {
    MessageBeep(MB_OK);
    app_status_note(rc == 0 ? "No matching brace" : "No brace at the caret");
  }
}

static void ec_bm_refresh(Tab* t)
{
  if (t == app_active() && g_app.textview) textview_invalidate(g_app.textview);
  app_on_doc_changed(t);                         // the session remembers bookmarks
}

void cmd_bm_toggle(void)
{
  Tab* t = ec_tab();
  if (!ec_is_text(t)) return;
  t->bookmarks.toggle(t->doc->line_of(t->sel.caret));
  ec_bm_refresh(t);
}

static void ec_bm_go(int next)
{
  Tab* t = ec_tab();
  u32 line, to;
  epos p, o;
  if (!ec_is_text(t)) return;
  if (t->bookmarks.count() == 0) { MessageBeep(MB_OK); app_status_note("No bookmarks (Ctrl+F2 sets one)"); return; }
  line = t->doc->line_of(t->sel.caret);
  to = next ? t->bookmarks.next(line) : t->bookmarks.prev(line);
  if (to == EPOS_NONE) return;
  p = t->doc->line_start(to);
  o = t->doc->text_origin();
  if (p < o) p = o;
  ec_set_sel(t, p, p, SCROLL_CENTER);
  app_update_status();
}

void cmd_bm_next(void) { ec_bm_go(1); }
void cmd_bm_prev(void) { ec_bm_go(0); }

void cmd_bm_clear(void)
{
  Tab* t = ec_tab();
  if (!t) return;
  t->bookmarks.clear();
  ec_bm_refresh(t);
}

void cmd_tools_options(void) { dlg_options(0); }

// ---------------------------------------------------------------------------------------------------------------
// menu state

UINT cmdstate_edit_undo(void) { Tab* t = ec_tab(); return (t && t->doc->can_undo()) ? 0 : MF_GRAYED; }
UINT cmdstate_edit_redo(void) { Tab* t = ec_tab(); return (t && t->doc->can_redo()) ? 0 : MF_GRAYED; }

static int ec_writable(Tab* t) { return t && !t->doc->read_only(); }

UINT cmdstate_edit_cut(void)
{
  Tab* t = ec_tab();
  return (ec_writable(t) && !sel_empty(t->sel)) ? 0 : MF_GRAYED;
}
UINT cmdstate_edit_copy(void) { Tab* t = ec_tab(); return (t && !sel_empty(t->sel)) ? 0 : MF_GRAYED; }
UINT cmdstate_edit_paste(void) { Tab* t = ec_tab(); return (ec_writable(t) && clip_has_data()) ? 0 : MF_GRAYED; }
UINT cmdstate_edit_delete(void) { return ec_writable(ec_tab()) ? 0 : MF_GRAYED; }
UINT cmdstate_edit_lineops(void) { Tab* t = ec_tab(); return (ec_writable(t) && ec_is_text(t)) ? 0 : MF_GRAYED; }
UINT cmdstate_edit_overwrite(void)
{
  Tab* t = ec_tab();
  if (!t) return MF_GRAYED;
  if (t->view == VIEW_HEX) return t->hv.caret.insert ? 0 : MF_CHECKED;
  return t->tv.overwrite ? MF_CHECKED : 0;
}
UINT cmdstate_search_brace(void) { return ec_is_text(ec_tab()) ? 0 : MF_GRAYED; }
UINT cmdstate_bm(void) { return ec_is_text(ec_tab()) ? 0 : MF_GRAYED; }

// ---------------------------------------------------------------------------------------------------------------
// selftest (st_edit): documents are built in memory (no files), commands run through app_command.

struct StEnc { const char* name; int enc; EolStyle eol; };
static const StEnc g_st_encs[] = {
  { "ansi-crlf", 0, EOL_CRLF }, { "utf8-lf", 1, EOL_LF }, { "utf16le-crlf", 2, EOL_CRLF }, { "utf16le-lf", 2, EOL_LF }
};

static EncSpec st_enc_of(int k) { return k == 0 ? enc_ansi() : (k == 1 ? enc_utf8(0) : enc_utf16le(1)); }

// Spec: UTF-8 text, '\n' = document EOL, '|' caret, '^' anchor.
static int st_build(const char* spec, const EncSpec& enc, EolStyle eol, epos origin, EBytes* out, Selection* sel)
{
  Vec<ecp> cps;
  const Codec* c = codec_get(enc);
  epos caret = EPOS_NONE, anchor = EPOS_NONE;
  u32 i;
  out->clear();
  if (!c || es_utf8_to_ecp(spec, es_len(spec), &cps) != E_OK) return E_FORMAT;
  for (i = 0; i < cps.size(); i++) {
    u8 b[8];
    u32 n;
    if (cps[i] == '|') { caret = origin + out->size(); continue; }
    if (cps[i] == '^') { anchor = origin + out->size(); continue; }
    if (cps[i] == '\n') {
      if (eol == EOL_CRLF) { n = c->encode('\r', b); out->append(b, n); }
      n = c->encode('\n', b); out->append(b, n);
      continue;
    }
    n = c->encode(cps[i], b);
    if (!n) return E_RANGE;
    out->append(b, n);
  }
  if (sel) {
    sel->caret = caret == EPOS_NONE ? origin : caret;
    sel->anchor = anchor == EPOS_NONE ? sel->caret : anchor;
  }
  return E_OK;
}

static Tab* st_mktab(StCtx* c, const EncSpec& enc, EolStyle eol, const EBytes& body, const char* lang_id)
{
  Document* d = new Document;
  Tab* t;
  if (!d) return 0;
  if (d->init_empty(enc, eol) != E_OK ||
      (body.size() && d->replace(d->text_origin(), 0, body.data(), body.size()) != E_OK)) {
    delete d; st_checkf(c, 0, "st_mktab: document"); return 0;
  }
  d->clear_undo();
  t = tab_new(d);
  if (!t) { delete d; return 0; }
  t->untitled = 1;
  t->untitled_no = 900 + t->id;
  t->tv.tab_width = 4; t->tv.use_spaces = 0;
  if (lang_id) {
    t->lang = lang_find(lang_id);
    if (t->lang) t->hl->attach(&t->lines, t->lang);
  }
  if (app_add_tab(t, 1) != E_OK) { tab_free(t); return 0; }
  st_pump(c, 0);
  return t;
}

static void st_rmtab(StCtx* c, Tab* t)
{
  if (!t) return;
  app_remove_tab(t);
  tab_free(t);
  st_pump(c, 0);
}

static int st_doc_is(Tab* t, const EBytes& body)
{
  Document* d = t->doc;
  EBytes got;
  epos o = d->text_origin();
  if (d->length() != o + body.size()) return 0;
  if (got.resize(body.size()) != E_OK) return 0;
  if (body.size() && d->read(o, got.data(), body.size()) != E_OK) return 0;
  return !body.size() || memcmp(got.data(), body.data(), body.size()) == 0;
}

// One command on every encoding variant: before -> after (bytes + selection), then Undo restores it.
static void st_line_case(StCtx* c, const char* name, int cmd, const char* before, const char* after, const char* lang)
{
  u32 v;
  for (v = 0; v < sizeof(g_st_encs) / sizeof(g_st_encs[0]); v++) {
    EncSpec enc = st_enc_of(g_st_encs[v].enc);
    EolStyle eol = g_st_encs[v].eol;
    epos origin = enc.bom ? 2 : 0;
    EBytes b0, b1;
    Selection s0, s1;
    Tab* t;
    if (st_build(before, enc, eol, origin, &b0, &s0) != E_OK || st_build(after, enc, eol, origin, &b1, &s1) != E_OK) {
      st_checkf(c, 0, "%s [%s]: spec", name, g_st_encs[v].name); continue;
    }
    t = st_mktab(c, enc, eol, b0, lang);
    if (!t) continue;
    if (lang && !t->lang) {
      selftest_log("%s: language '%s' not registered (lang_init not run?): case skipped", name, lang);
      st_rmtab(c, t);
      return;
    }
    ec_set_sel(t, s0.anchor, s0.caret, SCROLL_NONE);
    st_cmd(c, cmd);
    st_checkf(c, st_doc_is(t, b1), "%s [%s]: bytes after the command", name, g_st_encs[v].name);
    st_checkf(c, t->sel.anchor == s1.anchor && t->sel.caret == s1.caret, "%s [%s]: selection %u,%u want %u,%u", name,
              g_st_encs[v].name, t->sel.anchor, t->sel.caret, s1.anchor, s1.caret);
    st_cmd(c, IDM_EDIT_UNDO);
    st_checkf(c, st_doc_is(t, b0), "%s [%s]: undo restores the bytes", name, g_st_encs[v].name);
    st_checkf(c, t->sel.caret == s0.caret, "%s [%s]: undo restores the caret", name, g_st_encs[v].name);
    st_rmtab(c, t);
  }
}

static int st_clip_set_text(const char* ansi, const WCHAR* wide)
{
  HGLOBAL h;
  if (!OpenClipboard(g_app.frame)) return 0;
  EmptyClipboard();
  if (ansi) {
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, lstrlenA(ansi) + 1);
    if (h) { lstrcpyA((char*)GlobalLock(h), ansi); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
  }
  if (wide) {
    int n = lstrlenW(wide) + 1;
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (DWORD)n * 2);
    if (h) { memcpy(GlobalLock(h), wide, (u32)n * 2); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
  }
  CloseClipboard();
  return 1;
}

static int st_clip_get_ansi(char* out, u32 cap)
{
  HANDLE h;
  out[0] = 0;
  if (!OpenClipboard(g_app.frame)) return 0;
  h = GetClipboardData(CF_TEXT);
  if (h) { const char* p = (const char*)GlobalLock(h); if (p) { es_copy(out, cap, p); GlobalUnlock(h); } }
  CloseClipboard();
  return h != 0;
}

static int st_clip_get_wide(WCHAR* out, u32 cap)
{
  HANDLE h;
  u32 i = 0;
  out[0] = 0;
  if (!OpenClipboard(g_app.frame)) return 0;
  h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const WCHAR* p = (const WCHAR*)GlobalLock(h);
    if (p) { while (p[i] && i + 1 < cap) { out[i] = p[i]; i++; } GlobalUnlock(h); }
  }
  out[i] = 0;
  CloseClipboard();
  return h != 0;
}

static void st_edit_clipboard(StCtx* c)
{
  static const u8 raw_src[] = { 'a', 0x00, 'b', 0xFF, '\r', '\n', 'c' };    // NUL and an invalid UTF-8 byte
  static const WCHAR w_cyr[] = { 0x0416, 'x', 0x00E9, 0 };
  EBytes body, want;
  Selection s;
  Tab *a, *b;
  char ansi[200];
  WCHAR wide[200];
  // 1. RawBytes between two UTF-8 documents: exact bytes, NUL and the bad byte survive
  body.clear(); body.append(raw_src, sizeof(raw_src));
  a = st_mktab(c, enc_utf8(0), EOL_CRLF, body, 0);
  b = st_mktab(c, enc_utf8(0), EOL_LF, want, 0);
  if (!a || !b) { st_rmtab(c, a); st_rmtab(c, b); return; }
  app_activate(app_tab_index(a));
  ec_set_sel(a, 0, a->doc->length(), SCROLL_NONE);
  st_cmd(c, IDM_EDIT_COPY);
  ST_CHECK(c, st_clip_get_ansi(ansi, sizeof(ansi)) && ansi[0] == 'a', "CF_TEXT present after copy (stops at the NUL)");
  ST_CHECK(c, st_clip_get_wide(wide, 200) && wide[0] == 'a', "CF_UNICODETEXT present after copy");
  app_activate(app_tab_index(b));
  st_cmd(c, IDM_EDIT_PASTE);
  ST_CHECK(c, st_doc_is(b, body), "RawBytes paste is byte-exact (NUL, 0xFF, CRLF kept in an LF document)");
  ST_CHECK(c, b->sel.caret == b->doc->length(), "caret after the pasted bytes");
  // 2. RawBytes into a document in another encoding: decoded text, EOLs normalised
  st_rmtab(c, b);
  want.clear();
  b = st_mktab(c, enc_utf16le(1), EOL_CRLF, want, 0);
  if (b) {
    EBytes w16;
    static const u8 exp16[] = { 'a', 0, 0, 0, 'b', 0, 0xFD, 0xFF, '\r', 0, '\n', 0, 'c', 0 };
    st_cmd(c, IDM_EDIT_PASTE);
    w16.append(exp16, sizeof(exp16));
    ST_CHECK(c, st_doc_is(b, w16), "RawBytes into UTF-16: decoded text, the bad byte becomes U+FFFD");
    st_rmtab(c, b);
  }
  // 3. CF_TEXT from outside (no Unicode format): CRLF -> LF in an LF document
  want.clear();
  b = st_mktab(c, enc_ansi(), EOL_LF, want, 0);
  if (b) {
    EBytes exp;
    st_clip_set_text("one\r\ntwo\rthree\nfour", 0);
    st_cmd(c, IDM_EDIT_PASTE);
    st_build("one\ntwo\nthree\nfour", enc_ansi(), EOL_LF, 0, &exp, 0);
    ST_CHECK(c, st_doc_is(b, exp), "CF_TEXT paste: every line break becomes the document's LF");
    // Cut puts it back and empties the document
    ec_set_sel(b, 0, b->doc->length(), SCROLL_NONE);
    st_cmd(c, IDM_EDIT_CUT);
    ST_CHECK(c, b->doc->length() == 0, "cut removes the selection");
    ST_CHECK(c, st_clip_get_ansi(ansi, sizeof(ansi)) && es_icmp(ansi, "one\r\ntwo\r\nthree\r\nfour") == 0,
             "copied text uses CRLF for other programs");
    st_rmtab(c, b);
  }
  // 4. CF_UNICODETEXT into UTF-8 (exact) and into ANSI ('?' + the UTF-8 offer)
  want.clear();
  b = st_mktab(c, enc_utf8(0), EOL_CRLF, want, 0);
  if (b) {
    static const u8 exp8[] = { 0xD0, 0x96, 'x', 0xC3, 0xA9 };
    EBytes exp;
    exp.append(exp8, sizeof(exp8));
    st_clip_set_text(0, w_cyr);
    st_cmd(c, IDM_EDIT_PASTE);
    ST_CHECK(c, st_doc_is(b, exp), "CF_UNICODETEXT paste into UTF-8 keeps Cyrillic");
    st_rmtab(c, b);
  }
  b = st_mktab(c, enc_cp(1252), EOL_CRLF, want, 0);
  if (b) {
    static const u8 exp1252[] = { '?', 'x', 0xE9 };
    EBytes exp;
    exp.append(exp1252, sizeof(exp1252));
    st_clip_set_text(0, w_cyr);
    st_cmd(c, IDM_EDIT_PASTE);
    ST_CHECK(c, st_doc_is(b, exp), "CF_UNICODETEXT paste into Windows-1252: unstorable char becomes '?'");
    // copy back out: the ANSI and Unicode formats both carry the text
    ec_set_sel(b, 0, b->doc->length(), SCROLL_NONE);
    st_cmd(c, IDM_EDIT_COPY);
    ST_CHECK(c, st_clip_get_wide(wide, 200) && wide[0] == '?' && wide[1] == 'x' && wide[2] == 0x00E9 && !wide[3],
             "copy from ANSI: CF_UNICODETEXT decoded from Windows-1252");
    st_rmtab(c, b);
  }
  // 5. hex pane copy: hex text (16 bytes per line), RawBytes exact
  app_activate(app_tab_index(a));
  s.anchor = 0; s.caret = 4;
  ST_CHECK(c, clip_copy(a, s, 1) == E_OK, "clip_copy hex pane");
  ST_CHECK(c, st_clip_get_ansi(ansi, sizeof(ansi)) && es_icmp(ansi, "61 00 62 FF") == 0, "hex pane copies hex text");
  s.anchor = 1; s.caret = 1;
  ST_CHECK(c, clip_copy(a, s, 0) == E_NOTFOUND, "empty selection copies nothing");
  st_rmtab(c, a);
}

static void st_edit_bookmarks(StCtx* c)
{
  EBytes body;
  Tab* t;
  st_build("l0\nl1\nl2\nl3\nl4", enc_ansi(), EOL_CRLF, 0, &body, 0);
  t = st_mktab(c, enc_ansi(), EOL_CRLF, body, 0);
  if (!t) return;
  ec_set_sel(t, t->doc->line_start(1), t->doc->line_start(1), SCROLL_NONE);
  st_cmd(c, IDM_BM_TOGGLE);
  ec_set_sel(t, t->doc->line_start(3) + 1, t->doc->line_start(3) + 1, SCROLL_NONE);
  st_cmd(c, IDM_BM_TOGGLE);
  ST_CHECK(c, t->bookmarks.count() == 2 && t->bookmarks.has(1) && t->bookmarks.has(3), "two bookmarks set");
  ec_set_sel(t, 0, 0, SCROLL_NONE);
  st_cmd(c, IDM_BM_NEXT);
  ST_CHECK(c, t->sel.caret == t->doc->line_start(1), "F2 goes to the next bookmark");
  st_cmd(c, IDM_BM_NEXT);
  ST_CHECK(c, t->sel.caret == t->doc->line_start(3), "F2 again");
  st_cmd(c, IDM_BM_NEXT);
  ST_CHECK(c, t->sel.caret == t->doc->line_start(1), "F2 wraps around");
  st_cmd(c, IDM_BM_PREV);
  ST_CHECK(c, t->sel.caret == t->doc->line_start(3), "Shift+F2 wraps backwards");
  // a line command keeps bookmarks on their text
  ec_set_sel(t, 0, 0, SCROLL_NONE);
  st_cmd(c, IDM_LINE_DOWN);
  ST_CHECK(c, t->bookmarks.has(1) || t->bookmarks.has(0), "bookmark survives a line move");
  st_cmd(c, IDM_BM_CLEAR);
  ST_CHECK(c, t->bookmarks.count() == 0, "Clear All");
  st_rmtab(c, t);
}

static void st_edit_brace(StCtx* c)
{
  EBytes body;
  Tab* t;
  // offsets: f0 (1 a2 ,3 _4 "5 )6 "7 ,8 _9 b10 )11 _12 {13 _14 /15 *16 _17 }18 _19 *20 /21 _22 }23
  st_build("f(a, \")\", b) { /* } */ }", enc_ansi(), EOL_CRLF, 0, &body, 0);
  t = st_mktab(c, enc_ansi(), EOL_CRLF, body, "cpp");
  if (!t) return;
  ec_set_sel(t, 1, 1, SCROLL_NONE);             // before '('
  st_cmd(c, IDM_SEARCH_BRACE);
  if (!t->lang) {                                // no languages registered (lang_init not run): plain matching
    selftest_log("cpp language not registered: brace styles not checked");
    ST_CHECK(c, t->sel.caret == 6, "Ctrl+B without styles");
  } else {
    ST_CHECK(c, t->sel.caret == 11, "Ctrl+B jumps over the ')' in the string to the matching ')'");
    st_cmd(c, IDM_SEARCH_BRACE);
    ST_CHECK(c, t->sel.caret == 1, "Ctrl+B back to the '('");
    ec_set_sel(t, 13, 13, SCROLL_NONE);         // before '{'
    st_cmd(c, IDM_SEARCH_BRACE);
    ST_CHECK(c, t->sel.caret == 23, "brace in a comment is skipped");
  }
  ec_set_sel(t, 3, 3, SCROLL_NONE);
  st_cmd(c, IDM_SEARCH_BRACE);
  ST_CHECK(c, t->sel.caret == 3, "no brace at the caret: caret stays");
  st_rmtab(c, t);
}

static void st_edit_misc(StCtx* c)
{
  EBytes body;
  Tab* t;
  st_build("abc\ndef", enc_utf8(0), EOL_LF, 0, &body, 0);
  t = st_mktab(c, enc_utf8(0), EOL_LF, body, 0);
  if (!t) return;
  st_cmd(c, IDM_EDIT_SELECTALL);
  ST_CHECK(c, t->sel.anchor == 0 && t->sel.caret == 7, "Select All");
  ST_CHECK(c, cmdstate_edit_copy() == 0 && cmdstate_edit_lineops() == 0, "copy and line commands enabled");
  st_cmd(c, IDM_EDIT_DELETE);
  ST_CHECK(c, t->doc->length() == 0, "Delete removes the selection");
  st_cmd(c, IDM_EDIT_UNDO);
  ST_CHECK(c, t->doc->length() == 7, "Undo");
  st_cmd(c, IDM_EDIT_REDO);
  ST_CHECK(c, t->doc->length() == 0, "Redo");
  st_cmd(c, IDM_EDIT_UNDO);
  t->doc->set_read_only(true);
  ST_CHECK(c, cmdstate_edit_lineops() == MF_GRAYED && cmdstate_edit_paste() == MF_GRAYED, "read-only greys edits");
  ec_set_sel(t, 1, 1, SCROLL_NONE);
  st_cmd(c, IDM_LINE_DUP);                       // refused by the core too
  ST_CHECK(c, t->doc->length() == 7, "read-only document unchanged");
  t->doc->set_read_only(false);
  t->view = VIEW_HEX;
  ST_CHECK(c, cmdstate_edit_lineops() == MF_GRAYED && cmdstate_bm() == MF_GRAYED, "hex view greys line commands");
  ST_CHECK(c, cmdstate_edit_redo() == 0, "hex view keeps Undo/Redo (redo pending here)");
  t->view = VIEW_TEXT;
  st_cmd(c, IDM_EDIT_OVERWRITE);
  ST_CHECK(c, t->tv.overwrite == 1 && (cmdstate_edit_overwrite() & MF_CHECKED), "Insert/Overwrite toggles");
  st_cmd(c, IDM_EDIT_OVERWRITE);
  ST_CHECK(c, t->tv.overwrite == 0, "and back");
  // Ctrl+L cuts the line to the clipboard; Ctrl+Shift+L does not touch it
  {
    char ansi[64];
    st_clip_set_text("keep", 0);
    ec_set_sel(t, 1, 1, SCROLL_NONE);
    ec_line_delete();
    ST_CHECK(c, st_clip_get_ansi(ansi, sizeof(ansi)) && es_icmp(ansi, "keep") == 0, "Ctrl+Shift+L leaves the clipboard");
    ST_CHECK(c, t->doc->length() == 3, "Ctrl+Shift+L deleted line 1");
    st_cmd(c, IDM_EDIT_UNDO);
    ec_set_sel(t, 1, 1, SCROLL_NONE);
    cmd_line_cut();
    ST_CHECK(c, st_clip_get_ansi(ansi, sizeof(ansi)) && es_icmp(ansi, "abc\r\n") == 0, "Ctrl+L puts the line on the clipboard");
    ST_CHECK(c, t->doc->length() == 3, "Ctrl+L deleted line 1");
  }
  st_rmtab(c, t);
}

void st_edit(StCtx* c)
{
  ui_answer_clear();
  st_line_case(c, "Ctrl+D line", IDM_LINE_DUP, "ab|c\ndef", "ab|c\nabc\ndef", 0);
  st_line_case(c, "Ctrl+D selection", IDM_LINE_DUP, "^ab|c", "^ab|abc", 0);
  st_line_case(c, "Ctrl+Shift+L", IDM_LINE_DEL, "abc\nd|ef\nghi", "abc\n|ghi", 0);
  st_line_case(c, "Ctrl+Shift+Up", IDM_LINE_UP, "abc\nd|ef\nghi", "|def\n^abc\nghi", 0);
  st_line_case(c, "Ctrl+Shift+Down", IDM_LINE_DOWN, "abc\nd|ef\nghi", "abc\nghi\n|def^", 0);
  st_line_case(c, "Ctrl+Shift+Down block", IDM_LINE_DOWN, "^abc\nd|ef\nghi\njk", "ghi\n|abc\ndef\n^jk", 0);
  st_line_case(c, "Ctrl+Q add", IDM_EDIT_COMMENT, "^a\n  b|", "// ^a\n  // b|", "cpp");
  st_line_case(c, "Ctrl+Q remove", IDM_EDIT_COMMENT, "// a|", "a|", "cpp");
  st_line_case(c, "Ctrl+Q css", IDM_EDIT_COMMENT, "a|", "/* a| */", "css");
  st_line_case(c, "Ctrl+Shift+U", IDM_EDIT_UPPER, "^ab\xC3\xA9|", "^AB\xC3\x89|", 0);
  st_line_case(c, "Ctrl+U", IDM_EDIT_LOWER, "|ABC^", "|abc^", 0);
  st_line_case(c, "Indent", IDM_EDIT_INDENT, "^a\nb|", "^\ta\n\tb|", 0);
  st_line_case(c, "Unindent", IDM_EDIT_UNINDENT, "^\ta\n\tb|", "^a\nb|", 0);
  st_edit_misc(c);
  st_edit_bookmarks(c);
  st_edit_brace(c);
  st_edit_clipboard(c);
  ST_CHECK(c, ui_unexpected_questions() == 0, "no unexpected questions");
}
