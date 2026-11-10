// filecmds.cpp - File menu commands (DESIGN 15.4): New, Open (multi-select, filters from the language registry),
// Save, Save As (template with the Encoding and Line endings combos, 12.2 / 15.9), Save All, Close / Close All /
// Close Others, Revert, recent files, Exit. Owner after Wave 0: T09.
#include <windows.h>
#include <commdlg.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "docio.h"
#include "docio_int.h"
#include "selftest.h"
#include "config.h"
#include "codec.h"
#include "estr.h"
#include "resource.h"
#include "ids_file.h"

static char g_last_dir[260];
static DWORD g_open_filter = 1;

static void remember_dir(const char* path)
{
  ui_path_dir(path, g_last_dir, sizeof(g_last_dir));
}

static void initial_dir(Tab* t, char* out, u32 cap)
{
  Tab* a = app_active();
  out[0] = 0;
  if (t && !t->untitled && t->path[0]) ui_path_dir(t->path, out, cap);
  else if (g_last_dir[0]) es_copy(out, cap, g_last_dir);
  else if (a && !a->untitled && a->path[0]) ui_path_dir(a->path, out, cap);
}

// ---- Open ----

// "All files" + "Text files" + one entry per language ("C/C++ (*.c;*.cpp;...)"), double-NUL terminated.
static void build_open_filter(char* buf, u32 cap)
{
  u32 n = 0, i;
  const char* fixed[4] = { "All files (*.*)", "*.*", "Text files (*.txt;*.log;*.nfo)", "*.txt;*.log;*.nfo" };
  for (i = 0; i < 4; i++) {
    u32 len = es_len(fixed[i]);
    if (n + len + 2 >= cap) break;
    memcpy(buf + n, fixed[i], len + 1);
    n += len + 1;
  }
  for (i = 0; i < lang_count(); i++) {
    const LangInfo* li = lang_at(i);
    char pat[400], label[480];
    const char* p;
    u32 k = 0, len1, len2;
    if (!li || !li->exts[0]) continue;
    pat[0] = 0;
    for (p = li->exts; *p && k + 8 < sizeof(pat); ) {
      if (*p == ';' || *p == ' ') { p++; continue; }
      if (k) pat[k++] = ';';
      pat[k++] = '*'; pat[k++] = '.';
      while (*p && *p != ';' && *p != ' ' && k + 2 < sizeof(pat)) pat[k++] = *p++;
      pat[k] = 0;
    }
    if (!k) continue;
    es_fmt(label, sizeof(label), "%s (%s)", li->name[0] ? li->name : li->id, pat);
    len1 = es_len(label); len2 = es_len(pat);
    if (n + len1 + len2 + 3 >= cap) break;
    memcpy(buf + n, label, len1 + 1); n += len1 + 1;
    memcpy(buf + n, pat, len2 + 1); n += len2 + 1;
  }
  buf[n] = 0;
  if (n + 1 < cap) buf[n + 1] = 0;
}

static void open_one(const char* path)
{
  Tab* t = 0;
  docio_open(path, 0, &t);                           // reports its own errors
}

void cmd_file_open(void)
{
  enum { BUF = 32768 };
  OPENFILENAMEA ofn;
  char* files;
  char filter[6000], dir[260];
  if (g_app.selftest) { selftest_log("cmd_file_open: dialog suppressed in selftest mode"); return; }
  files = (char*)e_alloc(BUF);
  if (!files) { ui_error("Not enough memory."); return; }
  files[0] = 0;
  build_open_filter(filter, sizeof(filter));
  initial_dir(app_active(), dir, sizeof(dir));
  memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_app.frame;
  ofn.lpstrFilter = filter;
  ofn.nFilterIndex = g_open_filter;
  ofn.lpstrFile = files;
  ofn.nMaxFile = BUF;
  ofn.lpstrInitialDir = dir[0] ? dir : 0;
  ofn.lpstrTitle = "Open";
  ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
              OFN_NOCHANGEDIR;
  if (GetOpenFileNameA(&ofn)) {
    const char* first = files;
    const char* p = first + es_len(first) + 1;
    g_open_filter = ofn.nFilterIndex;
    if (!*p) {                                       // one file: full path
      remember_dir(first);
      open_one(first);
    } else {                                         // folder, then names
      char path[300];
      es_copy(g_last_dir, sizeof(g_last_dir), first);
      while (*p) {
        ui_path_join(first, p, path, sizeof(path));
        open_one(path);
        p += es_len(p) + 1;
      }
    }
  } else if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) {
    ui_error("Too many files were selected at once. Please open fewer files at a time.");
  }
  e_free(files);
}

// ---- Save As dialog (OFN_ENABLETEMPLATE | OFN_ENABLEHOOK, template IDD_SAVEAS_EXTRA) ----

enum { SA_KEEP = 100 };
static const char* k_sa_enc_names[5] = { "ANSI", "UTF-8", "UTF-8 with BOM", "UTF-16 LE (BOM)", "UTF-16 BE (BOM)" };
static const char* k_sa_eol_names[3] = { "Windows (CRLF)", "Unix (LF)", "Mac (CR)" };

static EncSpec sa_enc(int i)
{
  switch (i) {
  case 1: return enc_utf8(0);
  case 2: return enc_utf8(1);
  case 3: return enc_utf16le(1);
  case 4: return enc_utf16be(1);
  default: return enc_ansi();
  }
}

struct SaveAsData {
  EncSpec doc_enc; int doc_eol; int doc_mixed;
  int enc_choice; int eol_choice;                  // 0..4 / 0..2, or SA_KEEP
};

static void sa_fill(HWND dlg, SaveAsData* d)
{
  HWND ce = GetDlgItem(dlg, IDC_SAVEAS_ENC), cl = GetDlgItem(dlg, IDC_SAVEAS_EOL);
  int i, sel = -1, idx;
  char label[64], text[100];
  if (ce) {
    for (i = 0; i < 5; i++) if (enc_equal(sa_enc(i), d->doc_enc)) sel = i;
    if (sel < 0) {                                   // OEM, a code page, UTF-16 without BOM...: offer to keep it
      enc_label(d->doc_enc, label, sizeof(label));
      es_fmt(text, sizeof(text), "%s (keep)", label);
      idx = (int)SendMessageA(ce, CB_ADDSTRING, 0, (LPARAM)text);
      SendMessageA(ce, CB_SETITEMDATA, (WPARAM)idx, SA_KEEP);
      SendMessageA(ce, CB_SETCURSEL, (WPARAM)idx, 0);
    }
    for (i = 0; i < 5; i++) {
      idx = (int)SendMessageA(ce, CB_ADDSTRING, 0, (LPARAM)k_sa_enc_names[i]);
      SendMessageA(ce, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
      if (i == sel) SendMessageA(ce, CB_SETCURSEL, (WPARAM)idx, 0);
    }
  }
  if (cl) {
    sel = d->doc_mixed ? -1 : d->doc_eol;
    if (d->doc_mixed) {
      idx = (int)SendMessageA(cl, CB_ADDSTRING, 0, (LPARAM)"Mixed (keep)");
      SendMessageA(cl, CB_SETITEMDATA, (WPARAM)idx, SA_KEEP);
      SendMessageA(cl, CB_SETCURSEL, (WPARAM)idx, 0);
    }
    for (i = 0; i < 3; i++) {
      idx = (int)SendMessageA(cl, CB_ADDSTRING, 0, (LPARAM)k_sa_eol_names[i]);
      SendMessageA(cl, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
      if (i == sel) SendMessageA(cl, CB_SETCURSEL, (WPARAM)idx, 0);
    }
  }
}

static int combo_choice(HWND dlg, int id)
{
  HWND c = GetDlgItem(dlg, id);
  int idx;
  if (!c) return SA_KEEP;
  idx = (int)SendMessageA(c, CB_GETCURSEL, 0, 0);
  if (idx == CB_ERR) return SA_KEEP;
  return (int)SendMessageA(c, CB_GETITEMDATA, (WPARAM)idx, 0);
}

static UINT CALLBACK saveas_hook(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)wp;
  if (msg == WM_INITDIALOG) {
    OPENFILENAMEA* ofn = (OPENFILENAMEA*)lp;
    SaveAsData* d = (SaveAsData*)ofn->lCustData;
    SetWindowLongA(dlg, GWL_USERDATA, (LONG)d);
    sa_fill(dlg, d);
    return TRUE;
  }
  if (msg == WM_NOTIFY) {
    OFNOTIFYA* n = (OFNOTIFYA*)lp;
    SaveAsData* d = (SaveAsData*)GetWindowLongA(dlg, GWL_USERDATA);
    if (d && n->hdr.code == CDN_FILEOK) {
      d->enc_choice = combo_choice(dlg, IDC_SAVEAS_ENC);
      d->eol_choice = combo_choice(dlg, IDC_SAVEAS_EOL);
    }
  }
  return 0;
}

static char g_st_saveas_path[260];                 // selftest: next Save As answer (empty = cancel)

void filecmd_selftest_saveas(const char* path)
{
  es_copy(g_st_saveas_path, sizeof(g_st_saveas_path), path ? path : "");
}

int filecmd_saveas_dialog(Tab* t, char* path, u32 cap, EncSpec* enc_out, int* eol_out, int* enc_set, int* eol_set)
{
  OPENFILENAMEA ofn;
  SaveAsData d;
  char file[MAX_PATH], dir[260], name[280];
  *enc_set = 0; *eol_set = 0;
  if (!t || !t->doc) return 0;
  d.doc_enc = t->doc->enc();
  d.doc_eol = (int)t->doc->eol_style();
  d.doc_mixed = t->doc->eol_mixed() ? 1 : 0;
  d.enc_choice = d.eol_choice = SA_KEEP;
  if (g_app.selftest) {                              // common dialogs can't be answered by a script
    if (!g_st_saveas_path[0]) { selftest_log("Save As dialog: cancelled (selftest)"); return 0; }
    es_copy(path, cap, g_st_saveas_path);
    g_st_saveas_path[0] = 0;
    selftest_log("Save As dialog: %s (selftest)", path);
    return 1;
  }
  if (!t->untitled && t->path[0]) es_copy(file, sizeof(file), t->path);
  else { tab_title(t, name, sizeof(name)); es_fmt(file, sizeof(file), "%s.txt", name); }
  initial_dir(t, dir, sizeof(dir));
  memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_app.frame;
  ofn.hInstance = g_app.hinst;
  ofn.lpstrFilter = "All files (*.*)\0*.*\0Text files (*.txt)\0*.txt\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrFile = file;
  ofn.nMaxFile = sizeof(file);
  ofn.lpstrInitialDir = dir[0] ? dir : 0;
  ofn.lpstrTitle = "Save As";
  ofn.Flags = OFN_EXPLORER | OFN_ENABLETEMPLATE | OFN_ENABLEHOOK | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
              OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  ofn.lpTemplateName = MAKEINTRESOURCEA(IDD_SAVEAS_EXTRA);
  ofn.lpfnHook = saveas_hook;
  ofn.lCustData = (LPARAM)&d;
  if (!GetSaveFileNameA(&ofn)) return 0;
  es_copy(path, cap, file);
  remember_dir(file);
  if (d.enc_choice != SA_KEEP && d.enc_choice >= 0 && d.enc_choice < 5) {
    EncSpec e = sa_enc(d.enc_choice);
    if (!enc_equal(e, d.doc_enc)) { *enc_out = e; *enc_set = 1; }
  }
  if (d.eol_choice != SA_KEEP && d.eol_choice >= EOL_CRLF && d.eol_choice <= EOL_CR) {
    if (d.doc_mixed || d.eol_choice != d.doc_eol) { *eol_out = d.eol_choice; *eol_set = 1; }
  }
  return 1;
}

int filecmd_save_as(Tab* t, int wait)
{
  char path[260];
  EncSpec enc;
  int eol = EOL_CRLF, enc_set = 0, eol_set = 0, rc = E_CANCEL, tries;
  if (!t || !t->doc) return E_STATE;
  if (t->saving) { app_status_note("Saving... please wait"); return E_BUSY; }
  enc = t->doc->enc();
  for (tries = 0; tries < 8; tries++) {
    if (!filecmd_saveas_dialog(t, path, sizeof(path), &enc, &eol, &enc_set, &eol_set)) return E_CANCEL;
    rc = wait ? docio_save_wait(t, path, enc_set ? &enc : 0, eol_set ? &eol : 0)
              : docio_save(t, path, enc_set ? &enc : 0, eol_set ? &eol : 0, 0, 0);
    if (rc != E_READONLY && rc != E_EXISTS) break;   // read-only target and "No", or open in another tab: ask again
  }
  return rc;
}

int filecmd_save(Tab* t, int wait)
{
  int rc;
  if (!t || !t->doc) return E_STATE;
  if (t->untitled || !t->path[0]) return filecmd_save_as(t, wait);
  rc = wait ? docio_save_wait(t, 0, 0, 0) : docio_save(t, 0, 0, 0, 0, 0);
  if (rc == E_READONLY) return filecmd_save_as(t, wait);   // "No" to clearing the attribute = Save As
  if (rc == E_STATE && (t->untitled || !t->path[0])) return filecmd_save_as(t, wait);
  return rc;
}

// ---- recent files ----

static void mru_remove(int i)
{
  int k;
  if (i < 0 || i >= CFG_MRU_MAX) return;
  for (k = i; k + 1 < CFG_MRU_MAX; k++) es_copy(g_app.cfg.mru[k], sizeof(g_app.cfg.mru[k]), g_app.cfg.mru[k + 1]);
  g_app.cfg.mru[CFG_MRU_MAX - 1][0] = 0;
}

void cmd_mru_open(int i)
{
  char path[260];
  Tab* t = 0;
  if (i < 0 || i >= CFG_MRU_MAX || !g_app.cfg.mru[i][0]) return;
  es_copy(path, sizeof(path), g_app.cfg.mru[i]);
  if (!ui_file_exists(path)) {
    mru_remove(i);
    app_status_note("%s no longer exists (removed from the recent files)", path);
    return;
  }
  docio_open(path, 0, &t);
}

void cmd_mru_clear(void)
{
  int i;
  for (i = 0; i < CFG_MRU_MAX; i++) g_app.cfg.mru[i][0] = 0;
  app_status_note("Recent files cleared");
}

UINT cmdstate_mru_clear(void)
{
  return g_app.cfg.mru[0][0] ? 0 : MF_GRAYED;
}

// ---- command handlers ----

void cmd_file_new(void)
{
  Tab* t = 0;
  int rc = docio_new(&t);
  if (rc != E_OK) ui_error("A new document could not be created (%s).", e_errname(rc));
}

void cmd_file_revert(void)
{
  Tab* t = app_active();
  if (t) docio_revert(t);
}

void cmd_file_save(void)
{
  Tab* t = app_active();
  if (t) filecmd_save(t, 0);
}

void cmd_file_saveas(void)
{
  Tab* t = app_active();
  if (t) filecmd_save_as(t, 0);
}

void cmd_file_saveall(void)
{
  int i, n = app_tab_count(), saved = 0;
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    if (!t || !t->doc || t->saving || !t->doc->modified()) continue;
    if (t->untitled || !t->path[0]) {
      app_activate(i);                               // the dialog should show which document it is for
      if (filecmd_save_as(t, 0) == E_OK) saved++;
      n = app_tab_count();
    } else if (filecmd_save(t, 0) == E_OK) {
      saved++;
    }
  }
  if (!saved) app_status_note("Nothing to save");
}

void cmd_file_close(void)
{
  Tab* t = app_active();
  if (t) docio_close(t, 1);
}

void cmd_file_closeall(void)
{
  int i;
  for (i = app_tab_count() - 1; i >= 0; i--) {
    Tab* t = app_tab(i);
    if (t && docio_close(t, 1) != 1) break;          // Cancel (or an error) stops the whole command
  }
}

void cmd_file_closeothers(void)
{
  Tab* keep = app_active();
  int i;
  if (!keep) return;
  for (i = app_tab_count() - 1; i >= 0; i--) {
    Tab* t = app_tab(i);
    if (!t || t == keep) continue;
    if (docio_close(t, 1) != 1) break;
  }
  if (app_tab_index(keep) >= 0) app_activate(app_tab_index(keep));
}

void cmd_file_exit(void) { if (g_app.frame) PostMessageA(g_app.frame, WM_CLOSE, 0, 0); }   // frame_close() runs the exit flow

UINT cmdstate_file_revert(void)
{
  Tab* t = app_active();
  if (!t || !t->doc || t->untitled || !t->path[0] || t->saving || t->indexing) return MF_GRAYED;
  return t->doc->modified() ? 0 : MF_GRAYED;
}

UINT cmdstate_file_save(void)
{
  Tab* t = app_active();
  if (!t || !t->doc || t->saving || t->indexing) return MF_GRAYED;
  if (!t->untitled && t->path[0] && !t->doc->modified() && !t->disk_missing) return MF_GRAYED;
  return 0;
}

UINT cmdstate_file_saveall(void)
{
  int i, n = app_tab_count();
  for (i = 0; i < n; i++) {
    Tab* t = app_tab(i);
    if (t && t->doc && !t->saving && t->doc->modified()) return 0;
  }
  return MF_GRAYED;
}

UINT cmdstate_file_closeothers(void)
{
  return app_tab_count() >= 2 ? 0 : MF_GRAYED;
}
