// doccmds.cpp - Document menu commands and doccmd_offer_utf8 (DESIGN 12, 15.4). Owner after Wave 0: T09.
// Syntax (auto / plain / language, forced flag), Encoding (reload as, convert with the loss prompt), Line endings,
// Tabs, Read-only, Properties, and the "Convert to UTF-8" notification bar of 12.4.
#include <windows.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "docio_int.h"
#include "notifybar.h"
#include "progress.h"
#include "views.h"
#include "dialogs.h"
#include "codec.h"
#include "estr.h"
#include "resource.h"

static const u16 k_reload_cps[ENC_RELOAD_CP_COUNT] = { 1250, 1251, 1253, 1254, 1257, 437, 850, 852, 866, 20866 };

static Tab* active_doc()
{
  Tab* t = app_active();
  return t && t->doc ? t : 0;
}

static int busy_note(Tab* t)
{
  if (t->saving) { app_status_note("Saving... please wait"); return 1; }
  if (t->indexing || t->doc->indexing()) { app_status_note("Indexing... please wait"); return 1; }
  return 0;
}

static void enc_name(const EncSpec& e, char* out, u32 cap)
{
  char a[64], b[64];
  enc_label(e, a, sizeof(a));
  if ((e.kind == EK_SBCS || e.kind == EK_DBCS) && (es_icmp(a, "ANSI") == 0 || es_icmp(a, "OEM") == 0)) {
    enc_cp_name(e.cp, b, sizeof(b));
    es_fmt(out, cap, "%s (%s)", a, b);
  } else {
    es_copy(out, cap, a);
  }
}

// ---- conversions (shared with docio_save for Save As) ----

int doccmd_convert(Tab* t, const EncSpec& to, int report)
{
  Document* d;
  ConvertReport rep;
  DocPlace pl;
  char label[96], text[600];
  int rc;
  if (!t || !t->doc) return E_STATE;
  d = t->doc;
  if (enc_equal(to, d->enc())) return E_OK;
  enc_name(to, label, sizeof(label));
  if (t->ondemand || t->fsrc) {
    if (report) ui_error("Converting the encoding is not available for files opened on demand (large files). The "
                         "document is unchanged.");
    return E_TOOBIG;
  }
  if (busy_note(t)) return E_BUSY;
  if (d->read_only()) {
    if (report) ui_error("The document is read-only; it can't be converted to %s.", label);
    return E_READONLY;
  }
  if (!codec_get(to)) {
    if (report) ui_error("%s is not supported on this system.", label);
    return E_UNSUPPORTED;
  }
  {
    UiProgress prog("Converting", "Checking the characters...", 1);
    rc = d->check_convert(to, &rep, &prog);
  }
  if (rc == E_CANCEL) return E_CANCEL;
  if (rc != E_OK) {
    if (report) ui_error("The document could not be checked for conversion (%s).", e_errname(rc));
    return rc;
  }
  if (rep.unrepresentable || rep.invalid_src) {
    u32 first_line = rep.first_bad != EPOS_NONE ? d->line_of(rep.first_bad) + 1 : 1;
    int r = dlg_convert_loss(label, rep, first_line);
    if (r == E_NOTIMPL || r < 0) {                   // T12's dialog not there: plain question
      EStr s;
      if (rep.unrepresentable)
        s.appendf("%u character%s can't be stored in %s and will become '?'.", rep.unrepresentable,
                  rep.unrepresentable == 1 ? "" : "s", label);
      if (rep.invalid_src) {
        if (s.len()) s.append(" ");
        s.appendf("%u byte%s are not valid in the current encoding and will become %s.", rep.invalid_src,
                  rep.invalid_src == 1 ? "" : "s",
                  (to.kind == EK_UTF8 || to.kind == EK_UTF16LE || to.kind == EK_UTF16BE) ? "U+FFFD" : "'?'");
      }
      s.appendf(" First at line %u.\n\nConvert anyway? (Undo restores the original.)", first_line);
      es_copy(text, sizeof(text), s.c_str());
      r = ui_ask(Q_CONVERT_LOSS, text, MB_OKCANCEL | MB_ICONWARNING) == IDOK ? CONVERT_OK : CONVERT_CANCEL;
    }
    if (r == CONVERT_GOTO) {
      epos p = rep.first_bad != EPOS_NONE ? d->snap_char(rep.first_bad) : d->text_origin();
      t->sel.anchor = t->sel.caret = p;
      docio_view_refresh(t);
      if (t == app_active() && t->view == VIEW_TEXT && g_app.textview) textview_ensure_visible(g_app.textview, p, 1);
      return E_CANCEL;
    }
    if (r != CONVERT_OK) return E_CANCEL;
  }
  docio_place_get(t, &pl);
  {
    UiProgress prog("Converting", "Converting...", 0);
    rc = d->convert(to, &rep, &prog);
  }
  if (rc != E_OK) {
    if (report) ui_error("The document could not be converted to %s (%s). It is unchanged.", label, e_errname(rc));
    return rc;
  }
  docio_place_set(t, pl);
  docio_view_refresh(t);
  app_on_doc_changed(t);
  notifybar_hide(t, NB_CONVERT);
  app_status_note("Converted to %s (Undo restores the previous encoding)", label);
  return E_OK;
}

int doccmd_convert_eol(Tab* t, int eol, int report)
{
  Document* d;
  DocPlace pl;
  int rc;
  static const char* names[3] = { "Windows (CRLF)", "Unix (LF)", "Mac (CR)" };
  if (!t || !t->doc || eol < EOL_CRLF || eol > EOL_CR) return E_STATE;
  d = t->doc;
  if (!d->eol_mixed() && d->eol_style() == (EolStyle)eol) return E_OK;
  if (t->ondemand || t->fsrc) {
    if (report) ui_error("Converting line endings is not available for files opened on demand (large files).");
    return E_TOOBIG;
  }
  if (busy_note(t)) return E_BUSY;
  if (d->read_only()) {
    if (report) ui_error("The document is read-only; its line endings can't be converted.");
    return E_READONLY;
  }
  docio_place_get(t, &pl);
  {
    UiProgress prog("Converting", "Converting line endings...", 0);
    rc = d->convert_eol((EolStyle)eol, &prog);
  }
  if (rc != E_OK) {
    if (report) ui_error("The line endings could not be converted (%s). The document is unchanged.", e_errname(rc));
    return rc;
  }
  d->set_eol_style((EolStyle)eol);
  docio_place_set(t, pl);
  docio_view_refresh(t);
  app_on_doc_changed(t);
  app_status_note("Line endings converted to %s", names[eol]);
  return E_OK;
}

int doccmd_reload_as(Tab* t, const EncSpec& enc)
{
  char text[400], name[280];
  if (!t || !t->doc) return E_STATE;
  if (busy_note(t)) return E_BUSY;
  if (!t->untitled && t->path[0] && t->doc->modified()) {
    tab_title(t, name, sizeof(name));
    es_fmt(text, sizeof(text), "Reloading discards your changes to %s. Continue?", name);
    if (ui_ask(Q_RELOAD_DISCARD, text, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return E_CANCEL;
  }
  return docio_reload(t, &enc, 0);
}

// ---- 12.4: "can't be stored" bar ----

static void offer_fn(Tab* t, int kind, int button_id, void* ctx)
{
  (void)ctx;
  if (!t || !t->doc) return;
  if (button_id == 1) {                              // Convert to UTF-8
    doccmd_convert(t, enc_utf8(0), 1);
  } else if (button_id == 2) {                       // Undo (the paste) and convert to UTF-8
    u32 want = (u32)(size_t)ctx;
    if (t->doc->undo_seq() == want && t->doc->can_undo()) {
      Selection s;
      if (t->doc->undo(&s) == E_OK) { t->sel = s; docio_view_refresh(t); }
    }
    if (doccmd_convert(t, enc_utf8(0), 1) == E_OK) app_status_note("Converted to UTF-8: paste again to insert the text");
  }
  notifybar_hide(t, kind);
}

void doccmd_offer_utf8(Tab* t, const char* msg)
{
  NotifyButton b[2];
  if (!t || !t->doc) return;
  memset(b, 0, sizeof(b));
  if (t->ondemand || t->fsrc) {                      // no conversion for on-demand documents: just say it
    app_status_note("%s", msg ? msg : "");
    return;
  }
  if (msg && strstr(msg, "pasted")) {
    b[0].id = 2; es_copy(b[0].text, sizeof(b[0].text), "Undo and convert to UTF-8");
  } else {
    b[0].id = 1; es_copy(b[0].text, sizeof(b[0].text), "Convert to UTF-8");
  }
  b[1].id = 3; es_copy(b[1].text, sizeof(b[1].text), "Dismiss");
  notifybar_show(t, NB_CONVERT, msg ? msg : "", b, 2, offer_fn, (void*)(size_t)t->doc->undo_seq());
}

// ---- command handlers: Syntax ----

void cmd_lang_auto(void)
{
  Tab* t = active_doc();
  if (!t) return;
  docio_set_lang(t, (t->untitled || !t->path[0]) ? 0 : lang_for_file(ui_path_name(t->path)), 0);
}

void cmd_lang_plain(void)
{
  Tab* t = active_doc();
  if (t) docio_set_lang(t, 0, 1);
}

void cmd_lang_set(int i)
{
  Tab* t = active_doc();
  if (!t || i < 0 || (u32)i >= lang_count()) return;
  if (t->ondemand) app_status_note("Colouring stays off for files opened on demand");
  docio_set_lang(t, lang_at((u32)i), 1);
}

// ---- Encoding ----

static void reload_cmd(const EncSpec& e)
{
  Tab* t = active_doc();
  if (t) doccmd_reload_as(t, e);
}

void cmd_enc_reload_ansi(void) { reload_cmd(enc_ansi()); }
void cmd_enc_reload_oem(void) { reload_cmd(enc_oem()); }
void cmd_enc_reload_utf8(void) { reload_cmd(enc_utf8(0)); }
void cmd_enc_reload_utf16le(void) { reload_cmd(enc_utf16le(1)); }
void cmd_enc_reload_utf16be(void) { reload_cmd(enc_utf16be(1)); }

void cmd_enc_reload_cp(int i)
{
  if (i < 0 || i >= ENC_RELOAD_CP_COUNT) return;
  reload_cmd(enc_cp(k_reload_cps[i]));
}

static void conv_cmd(const EncSpec& e)
{
  Tab* t = active_doc();
  if (!t) return;
  if (enc_equal(e, t->doc->enc())) { app_status_note("The document already uses this encoding"); return; }
  doccmd_convert(t, e, 1);
}

void cmd_enc_conv_ansi(void) { conv_cmd(enc_ansi()); }
void cmd_enc_conv_utf8(void) { conv_cmd(enc_utf8(0)); }
void cmd_enc_conv_utf8bom(void) { conv_cmd(enc_utf8(1)); }
void cmd_enc_conv_utf16le(void) { conv_cmd(enc_utf16le(1)); }
void cmd_enc_conv_utf16be(void) { conv_cmd(enc_utf16be(1)); }

// ---- Line endings ----

static void eol_cmd(int eol)
{
  Tab* t = active_doc();
  if (t) doccmd_convert_eol(t, eol, 1);
}

void cmd_eol_crlf(void) { eol_cmd(EOL_CRLF); }
void cmd_eol_lf(void) { eol_cmd(EOL_LF); }
void cmd_eol_cr(void) { eol_cmd(EOL_CR); }

// ---- Tabs ----

static void tabs_changed(Tab* t)
{
  if (t == app_active() && g_app.textview) textview_config_changed(g_app.textview);
  app_update_status();
}

static void tabw(int w)
{
  Tab* t = active_doc();
  if (!t) return;
  t->tv.tab_width = (u8)w;
  tabs_changed(t);
}

void cmd_doc_tabw2(void) { tabw(2); }
void cmd_doc_tabw4(void) { tabw(4); }
void cmd_doc_tabw8(void) { tabw(8); }

void cmd_doc_tabspaces(void)
{
  Tab* t = active_doc();
  if (!t) return;
  t->tv.use_spaces = (u8)(t->tv.use_spaces ? 0 : 1);
  tabs_changed(t);
}

// ---- Read-only, Properties ----

void cmd_doc_readonly(void)
{
  Tab* t = active_doc();
  if (!t) return;
  if (t->doc->read_only()) {
    if (t->shared_ro && t->ondemand) {
      app_status_note("Another program has this file open for writing; it stays read-only");
      return;
    }
    t->doc->set_read_only(false);
    notifybar_hide(t, NB_READONLY);
    if (t->ro_attr) app_status_note("Editing allowed; saving will ask to clear the file's read-only attribute");
  } else {
    t->doc->set_read_only(true);
  }
  app_update_title();
  app_update_status();
}

void cmd_doc_properties(void)
{
  Tab* t = active_doc();
  if (t) dlg_properties(t);
}

// ---- menu state ----

static UINT st_base(Tab* t)
{
  return t ? 0 : MF_GRAYED;
}

static UINT st_edit_enc(Tab* t)                    // conversions: not for on-demand, busy or read-only documents
{
  if (!t) return MF_GRAYED;
  if (t->ondemand || t->fsrc || t->saving || t->indexing || t->doc->indexing() || t->doc->read_only()) return MF_GRAYED;
  return 0;
}

UINT cmdstate_lang_auto(void)
{
  Tab* t = active_doc();
  HMENU m = g_app.frame ? GetMenu(g_app.frame) : 0;
  docio_sync_lang_menu();
  if (m) {                                          // 8.6: say when colouring is off for a large file
    const char* want = (t && t->large && t->hl && !t->hl->enabled()) ? "(&Auto) - colouring off: large file" : "(&Auto)";
    char cur[64];
    cur[0] = 0;
    GetMenuStringA(m, IDM_LANG_AUTO, cur, sizeof(cur), MF_BYCOMMAND);
    if (strcmp(cur, want) != 0) ModifyMenuA(m, IDM_LANG_AUTO, MF_BYCOMMAND | MF_STRING, IDM_LANG_AUTO, want);
  }
  if (!t) return MF_GRAYED;
  return t->lang_forced ? 0 : MF_CHECKED;
}

UINT cmdstate_lang_plain(void)
{
  Tab* t = active_doc();
  if (!t) return MF_GRAYED;
  return t->lang == 0 ? MF_CHECKED : 0;
}

UINT cmdstate_lang(int i)
{
  Tab* t = active_doc();
  if (!t || i < 0 || (u32)i >= lang_count()) return MF_GRAYED;
  return t->lang && t->lang == lang_at((u32)i) ? MF_CHECKED : 0;
}

UINT cmdstate_enc_reload(void)
{
  Tab* t = active_doc();
  if (!t || t->saving || t->indexing || t->doc->indexing()) return MF_GRAYED;
  return 0;
}

UINT cmdstate_enc_reload_cp(int i)
{
  Tab* t = active_doc();
  UINT s;
  if (i < 0 || i >= ENC_RELOAD_CP_COUNT) return MF_GRAYED;
  s = cmdstate_enc_reload();
  if (t && (t->doc->enc().kind == EK_SBCS || t->doc->enc().kind == EK_DBCS) && t->doc->enc().cp == k_reload_cps[i])
    s |= MF_CHECKED;
  return s;
}

static UINT conv_state(const EncSpec& e)
{
  Tab* t = active_doc();
  UINT s = st_edit_enc(t);
  if (t && enc_equal(t->doc->enc(), e)) s |= MF_CHECKED;
  return s;
}

UINT cmdstate_enc_conv_ansi(void) { return conv_state(enc_ansi()); }
UINT cmdstate_enc_conv_utf8(void) { return conv_state(enc_utf8(0)); }
UINT cmdstate_enc_conv_utf8bom(void) { return conv_state(enc_utf8(1)); }
UINT cmdstate_enc_conv_utf16le(void) { return conv_state(enc_utf16le(1)); }
UINT cmdstate_enc_conv_utf16be(void) { return conv_state(enc_utf16be(1)); }

static UINT eol_state(int eol)
{
  Tab* t = active_doc();
  UINT s = st_edit_enc(t);
  if (t && !t->doc->eol_mixed() && (int)t->doc->eol_style() == eol) s |= MF_CHECKED;
  return s;
}

UINT cmdstate_eol_crlf(void) { return eol_state(EOL_CRLF); }
UINT cmdstate_eol_lf(void) { return eol_state(EOL_LF); }
UINT cmdstate_eol_cr(void) { return eol_state(EOL_CR); }

static UINT tabw_state(int w)
{
  Tab* t = active_doc();
  if (!t) return MF_GRAYED;
  return t->tv.tab_width == w ? MF_CHECKED : 0;
}

UINT cmdstate_doc_tabw2(void) { return tabw_state(2); }
UINT cmdstate_doc_tabw4(void) { return tabw_state(4); }
UINT cmdstate_doc_tabw8(void) { return tabw_state(8); }

UINT cmdstate_doc_tabspaces(void)
{
  Tab* t = active_doc();
  if (!t) return MF_GRAYED;
  return t->tv.use_spaces ? MF_CHECKED : 0;
}

UINT cmdstate_doc_readonly(void)
{
  Tab* t = active_doc();
  if (!t) return MF_GRAYED;
  return st_base(t) | (t->doc->read_only() ? MF_CHECKED : 0);
}
