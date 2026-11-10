// dlgmisc.cpp - exit prompt, convert-loss prompt, Properties, About (DESIGN 15.9) and st_dialogs. Owner: T12.
// In selftest mode the two question dialogs still open, then take their button from the ui_ask answer queue
// (Q_EXIT: IDYES = Save Selected, IDNO = Discard, IDRETRY = Keep for Next Session, IDCANCEL = Cancel;
//  Q_CONVERT_LOSS: IDYES/IDOK = Convert, IDNO/IDRETRY = Go to First, IDCANCEL = Cancel).
#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "dialogs.h"
#include "dialogs_int.h"
#include "selftest.h"
#include "views.h"
#include "fsys.h"
#include "estr.h"
#include "resource.h"
#include "ids_misc.h"
#include "ids_opts.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// exit / close-many prompt

struct ExitState { Tab** tabs; u32 n; u8* flags; int keep_ok; int modeless; };

static void exit_post_answer(HWND h, ExitState* s, const char* text)
{
  int a = ui_ask(Q_EXIT, text, MB_YESNOCANCEL);
  int id = IDCANCEL;
  if (a == IDYES || a == IDOK) id = IDC_EXIT_SAVE;
  else if (a == IDNO) id = IDC_EXIT_DISCARD;
  else if (a == IDRETRY) id = s->keep_ok ? IDC_EXIT_KEEP : IDCANCEL;
  PostMessageA(h, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}

static void exit_end(HWND h, ExitState* s, int result)
{
  if (s->modeless) { DestroyWindow(h); delete s; }
  else EndDialog(h, result);
}

static BOOL CALLBACK exit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  ExitState* s = (ExitState*)GetWindowLong(h, DWL_USER);
  switch (msg) {
  case WM_INITDIALOG: {
    HWND lv = GetDlgItem(h, IDC_EXIT_LIST);
    LVCOLUMNA col;
    RECT rc;
    u32 i;
    char text[300];
    s = (ExitState*)lp;
    SetWindowLong(h, DWL_USER, (LONG)s);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES);
    GetClientRect(lv, &rc);
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_WIDTH;
    col.cx = rc.right - GetSystemMetrics(SM_CXVSCROLL) - 2;
    ListView_InsertColumn(lv, 0, &col);
    for (i = 0; i < s->n; i++) {
      Tab* t = s->tabs[i];
      LVITEMA it;
      char name[300];
      if (t->untitled || !t->path[0]) tab_title(t, name, sizeof(name));
      else es_copy(name, sizeof(name), t->path);
      if (s->keep_ok && t->ondemand) es_cat(name, sizeof(name), "  (not kept: too large)");
      memset(&it, 0, sizeof(it));
      it.mask = LVIF_TEXT;
      it.iItem = (int)i;
      it.pszText = name;
      ListView_InsertItem(lv, &it);
      ListView_SetItemState(lv, (int)i, INDEXTOSTATEIMAGEMASK(2), LVIS_STATEIMAGEMASK);   // ticked
    }
    ListView_SetItemState(lv, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
    if (s->n == 1) SetDlgItemTextA(h, IDC_EXIT_TEXT, "This document has unsaved changes. Save it?");
    if (!s->keep_ok) {
      ShowWindow(GetDlgItem(h, IDC_EXIT_KEEP), SW_HIDE);
      ShowWindow(GetDlgItem(h, IDC_EXIT_HINT), SW_HIDE);
    }
    if (g_app.selftest && !s->modeless) {
      es_fmt(text, sizeof(text), "%u document%s with unsaved changes", s->n, s->n == 1 ? "" : "s");
      exit_post_answer(h, s, text);
    }
    return TRUE;
  }
  case WM_COMMAND:
    if (!s) return FALSE;
    switch (LOWORD(wp)) {
    case IDC_EXIT_SAVE: {
      HWND lv = GetDlgItem(h, IDC_EXIT_LIST);
      u32 i;
      for (i = 0; i < s->n; i++)
        s->flags[i] = (u8)(((ListView_GetItemState(lv, (int)i, LVIS_STATEIMAGEMASK) >> 12) == 2) ? 1 : 0);
      exit_end(h, s, EXIT_SAVE);
      return TRUE;
    }
    case IDC_EXIT_KEEP:
      if (!s->keep_ok) return TRUE;
      exit_end(h, s, EXIT_KEEP);
      return TRUE;
    case IDC_EXIT_DISCARD:
      exit_end(h, s, EXIT_DISCARD);
      return TRUE;
    case IDCANCEL:
      exit_end(h, s, EXIT_CANCEL);
      return TRUE;
    }
    break;
  }
  return FALSE;
}

int dlg_exit_prompt(Tab** tabs, u32 n, u8* save_flags)
{
  ExitState s;
  u32 i;
  int r;
  if (!tabs || !save_flags) return E_STATE;
  for (i = 0; i < n; i++) save_flags[i] = 0;
  if (n == 0) return EXIT_DISCARD;
  InitCommonControls();
  s.tabs = tabs; s.n = n; s.flags = save_flags; s.keep_ok = g_app.cfg.backup_enabled ? 1 : 0; s.modeless = 0;
  r = (int)DialogBoxParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_EXITPROMPT), g_app.frame, exit_proc, (LPARAM)&s);
  if (r <= 0) return E_NOMEM;
  if (r != EXIT_SAVE) for (i = 0; i < n; i++) save_flags[i] = 0;
  return r;
}

static HWND exit_modeless(Tab** tabs, u32 n, u8* flags)   // screenshots
{
  ExitState* s = new ExitState;
  HWND h;
  if (!s) return 0;
  s->tabs = tabs; s->n = n; s->flags = flags; s->keep_ok = g_app.cfg.backup_enabled ? 1 : 0; s->modeless = 1;
  h = CreateDialogParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_EXITPROMPT), g_app.frame, exit_proc, (LPARAM)s);
  if (!h) { delete s; return 0; }
  ShowWindow(h, SW_SHOW);
  return h;
}

// ---------------------------------------------------------------------------------------------------------------
// convert-loss prompt

struct ConvState { char text[400]; int modeless; };

static BOOL CALLBACK conv_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  ConvState* s = (ConvState*)GetWindowLong(h, DWL_USER);
  int r = 0;
  switch (msg) {
  case WM_INITDIALOG:
    s = (ConvState*)lp;
    SetWindowLong(h, DWL_USER, (LONG)s);
    SetDlgItemTextA(h, IDC_CONV_TEXT, s->text);
    SendDlgItemMessageA(h, IDC_CONV_ICON, STM_SETICON, (WPARAM)LoadIcon(0, IDI_EXCLAMATION), 0);
    MessageBeep(MB_ICONEXCLAMATION);
    if (g_app.selftest && !s->modeless) {
      int a = ui_ask(Q_CONVERT_LOSS, s->text, MB_YESNOCANCEL);
      int id = (a == IDYES || a == IDOK) ? IDOK : ((a == IDNO || a == IDRETRY) ? IDC_CONV_GOTO : IDCANCEL);
      PostMessageA(h, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
    }
    return TRUE;
  case WM_COMMAND:
    if (!s) return FALSE;
    switch (LOWORD(wp)) {
    case IDOK: r = CONVERT_OK; break;
    case IDC_CONV_GOTO: r = CONVERT_GOTO; break;
    case IDCANCEL: r = CONVERT_CANCEL; break;
    default: return FALSE;
    }
    if (s->modeless) { DestroyWindow(h); delete s; }
    else EndDialog(h, r);
    return TRUE;
  }
  return FALSE;
}

static void conv_text(const char* enc_label, const ConvertReport& r, u32 first_line, char* out, u32 cap)
{
  char a[24], b[24], ln[24];
  dlg_fmt_num(r.unrepresentable, a, sizeof(a));
  dlg_fmt_num(r.invalid_src, b, sizeof(b));
  dlg_fmt_num(first_line ? first_line : 1, ln, sizeof(ln));   // 1-based (doccmds passes line_of + 1)
  out[0] = 0;
  if (r.unrepresentable)
    es_fmt(out, cap, "%s character%s can't be stored in %s and will become '?'.", a, r.unrepresentable == 1 ? "" : "s",
           enc_label ? enc_label : "this encoding");
  if (r.invalid_src) {
    char more[200];
    es_fmt(more, sizeof(more), "%s%s byte%s that %s not valid in the current encoding will be replaced.",
           out[0] ? " " : "", b, r.invalid_src == 1 ? "" : "s", r.invalid_src == 1 ? "is" : "are");
    es_cat(out, cap, more);
  }
  if (out[0]) {
    char more[64];
    es_fmt(more, sizeof(more), " First at line %s.", ln);
    es_cat(out, cap, more);
  }
  es_cat(out, cap, "\r\n\r\nConvert anyway? Undo restores the original.");
}

int dlg_convert_loss(const char* enc_label, const ConvertReport& r, u32 first_line)
{
  ConvState s;
  int res;
  conv_text(enc_label, r, first_line, s.text, sizeof(s.text));
  s.modeless = 0;
  res = (int)DialogBoxParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_CONVERTLOSS), g_app.frame, conv_proc, (LPARAM)&s);
  if (res <= 0) return E_NOMEM;
  return res;
}

static HWND conv_modeless(const char* enc_label, const ConvertReport& r, u32 first_line)
{
  ConvState* s = new ConvState;
  HWND h;
  if (!s) return 0;
  conv_text(enc_label, r, first_line, s->text, sizeof(s->text));
  s->modeless = 1;
  h = CreateDialogParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_CONVERTLOSS), g_app.frame, conv_proc, (LPARAM)s);
  if (!h) { delete s; return 0; }
  ShowWindow(h, SW_SHOW);
  return h;
}

// ---------------------------------------------------------------------------------------------------------------
// Properties

static void prop_time(u32 lo, u32 hi, char* out, u32 cap)
{
  FILETIME ft, lt;
  SYSTEMTIME st;
  char d[64], t[64];
  ft.dwLowDateTime = lo; ft.dwHighDateTime = hi;
  out[0] = 0;
  if (!lo && !hi) return;
  if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return;
  if (!GetDateFormatA(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, 0, d, sizeof(d))) d[0] = 0;
  if (!GetTimeFormatA(LOCALE_USER_DEFAULT, 0, &st, 0, t, sizeof(t))) t[0] = 0;
  es_fmt(out, cap, "%s, %s", d, t);
}

static void prop_fill(HWND h, Tab* t)
{
  Document* d = t->doc;
  char buf[400], a[24], b[24], lab[40];
  const EolCounts& ec = d->eol_counts();
  FsStat st;
  static const char* const eolname[] = { "CRLF", "LF", "CR" };
  if (t->untitled || !t->path[0]) { tab_title(t, lab, sizeof(lab)); es_fmt(buf, sizeof(buf), "%s (not saved yet)", lab); }
  else es_copy(buf, sizeof(buf), t->path);
  SetDlgItemTextA(h, IDC_PROP_PATH, buf);
  memset(&st, 0, sizeof(st));
  if (!t->untitled && t->path[0]) fs_os()->stat(t->path, &st);
  dlg_fmt_num(d->length(), a, sizeof(a));
  if (st.exists && !st.size_hi && st.size_lo != d->length()) {
    dlg_fmt_num(st.size_lo, b, sizeof(b));
    es_fmt(buf, sizeof(buf), "%s bytes (on disk: %s bytes)", a, b);
  } else es_fmt(buf, sizeof(buf), "%s bytes", a);
  SetDlgItemTextA(h, IDC_PROP_SIZE, buf);
  prop_time(st.exists ? st.mtime_lo : 0, st.exists ? st.mtime_hi : 0, buf, sizeof(buf));
  SetDlgItemTextA(h, IDC_PROP_MODIFIED, buf[0] ? buf : "-");
  prop_time(st.exists ? st.ctime_lo : 0, st.exists ? st.ctime_hi : 0, buf, sizeof(buf));
  SetDlgItemTextA(h, IDC_PROP_CREATED, buf[0] ? buf : "-");
  enc_label(d->enc(), lab, sizeof(lab));
  es_fmt(buf, sizeof(buf), "%s%s", lab, t->enc_forced ? " (chosen by you)" : "");
  SetDlgItemTextA(h, IDC_PROP_ENC, buf);
  es_fmt(buf, sizeof(buf), "%u CRLF, %u LF, %u CR%s; new lines use %s", ec.crlf, ec.lf, ec.cr,
         d->eol_mixed() ? " (mixed)" : "", eolname[d->eol_style() <= 2 ? d->eol_style() : 0]);
  SetDlgItemTextA(h, IDC_PROP_EOL, buf);
  dlg_fmt_num(d->line_count(), a, sizeof(a));
  SetDlgItemTextA(h, IDC_PROP_LINES, a);
  es_fmt(buf, sizeof(buf), "%s%s", t->lang ? t->lang->name : "Plain text", t->lang_forced ? " (chosen by you)" : "");
  SetDlgItemTextA(h, IDC_PROP_LANG, buf);
  es_copy(buf, sizeof(buf), d->modified() ? "Modified" : "Unmodified");
  if (d->read_only()) es_cat(buf, sizeof(buf), ", read-only");
  if (t->ondemand) es_cat(buf, sizeof(buf), ", read from disk on demand");
  if (t->large) es_cat(buf, sizeof(buf), ", large-file mode");
  if (st.exists && (st.attrs & FILE_ATTRIBUTE_READONLY)) es_cat(buf, sizeof(buf), ", read-only on disk");
  SetDlgItemTextA(h, IDC_PROP_STATE, buf);
}

struct InfoState { Tab* t; int modeless; HFONT bold; };

static BOOL CALLBACK info_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  InfoState* s = (InfoState*)GetWindowLong(h, DWL_USER);
  switch (msg) {
  case WM_INITDIALOG:
    s = (InfoState*)lp;
    SetWindowLong(h, DWL_USER, (LONG)s);
    if (s->t) prop_fill(h, s->t);
    else {
      char buf[400];
      LOGFONTA lf;
      HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
      es_fmt(buf, sizeof(buf), "%s %s", APP_NAME, APP_VERSION);
      SetDlgItemTextA(h, IDC_ABOUT_NAME, buf);
      if (f && GetObjectA(f, sizeof(lf), &lf)) {
        lf.lfWeight = FW_BOLD;
        s->bold = CreateFontIndirectA(&lf);
        if (s->bold) SendDlgItemMessageA(h, IDC_ABOUT_NAME, WM_SETFONT, (WPARAM)s->bold, 0);
      }
      SetDlgItemTextA(h, IDC_ABOUT_TEXT,
                      "Text and hex editor for Windows 98.\r\nBuilt " __DATE__ ".\r\n\r\n"
                      "All our own code: the buffer, rendering, syntax colouring, search, encodings and the hex view.");
      es_fmt(buf, sizeof(buf), "Settings, session and backups:\r\n%s", g_app.cfg_dir[0] ? g_app.cfg_dir : "(none)");
      SetDlgItemTextA(h, IDC_ABOUT_CFG, buf);
    }
    return TRUE;
  case WM_COMMAND:
    if (s && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
      if (s->modeless) DestroyWindow(h);
      else EndDialog(h, 1);
      return TRUE;
    }
    break;
  case WM_DESTROY:
    if (s) {
      if (s->bold) DeleteObject(s->bold);
      s->bold = 0;
      if (s->modeless) { SetWindowLong(h, DWL_USER, 0); delete s; }
    }
    break;
  }
  return FALSE;
}

void dlg_properties(Tab* t)
{
  InfoState s;
  if (!t || !t->doc) return;
  s.t = t; s.modeless = 0; s.bold = 0;
  DialogBoxParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_PROPERTIES), g_app.frame, info_proc, (LPARAM)&s);
}

void dlg_about()
{
  InfoState s;
  s.t = 0; s.modeless = 0; s.bold = 0;
  DialogBoxParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_ABOUT), g_app.frame, info_proc, (LPARAM)&s);
}

static HWND info_modeless(Tab* t, int id)
{
  InfoState* s = new InfoState;
  HWND h;
  if (!s) return 0;
  s->t = t; s->modeless = 1; s->bold = 0;
  h = CreateDialogParamA(g_app.hinst, MAKEINTRESOURCEA(id), g_app.frame, info_proc, (LPARAM)s);
  if (!h) { delete s; return 0; }
  ShowWindow(h, SW_SHOW);
  return h;
}

HWND dlg_properties_modeless(Tab* t) { return info_modeless(t, IDD_PROPERTIES); }
HWND dlg_about_modeless() { return info_modeless(0, IDD_ABOUT); }

// ---------------------------------------------------------------------------------------------------------------
// selftest (st_dialogs)

static Tab* sd_tab(const char* utf8, const EncSpec& enc, EolStyle eol, int modified)
{
  Document* d = new Document;
  Tab* t;
  Vec<ecp> cps;
  if (!d) return 0;
  if (d->init_empty(enc, eol) != E_OK) { delete d; return 0; }
  es_utf8_to_ecp(utf8, es_len(utf8), &cps);
  if (cps.size()) d->replace_text(d->text_origin(), 0, cps.data(), cps.size(), 0);
  if (!modified) d->set_saved();
  t = tab_new(d);
  if (!t) { delete d; return 0; }
  t->untitled = 1;
  t->untitled_no = 800 + t->id;
  t->tv.tab_width = 4;
  if (app_add_tab(t, 1) != E_OK) { tab_free(t); return 0; }
  return t;
}

static void sd_free(StCtx* c, Tab* t)
{
  if (!t) return;
  app_remove_tab(t);
  tab_free(t);
  st_pump(c, 0);
}

static int sd_shots(StCtx* c)                   // <cfg>\st_shots.txt: keep each dialog up for screenshots
{
  char p[300];
  ui_path_join(c->cfg_dir, "st_shots.txt", p, sizeof(p));
  return ui_file_exists(p);
}

static void sd_hold(StCtx* c, HWND h, const char* what)
{
  if (!h || !sd_shots(c)) return;
  selftest_log("shot: %s (t=%u)", what, GetTickCount());
  SetForegroundWindow(h);
  st_pump(c, 7000);
}

static void sd_text(HWND h, int id, char* out, u32 cap)
{
  out[0] = 0;
  GetDlgItemTextA(h, id, out, (int)cap);
}

static void sd_goto(StCtx* c)
{
  Tab* t = sd_tab("line 1\nline 2\n\tx = 3;\nline 4\nlast", enc_utf8(0), EOL_LF, 0);
  epos pos;
  char msg[200];
  HWND h;
  if (!t) { ST_CHECK(c, 0, "tab"); return; }
  t->sel.anchor = t->sel.caret = 0;
  ST_CHECK(c, dlg_goto_eval(t, 0, "3", &pos, msg, sizeof(msg)) && pos == t->doc->line_start(2), "go to line 3");
  ST_CHECK(c, strstr(msg, "Line 3 of 5") != 0, "preview names the line");
  ST_CHECK(c, dlg_goto_eval(t, 0, "3:5", &pos, msg, sizeof(msg)) && pos == t->doc->line_start(2) + 1,
           "column 5 on a line starting with a tab = after the tab (display columns)");
  ST_CHECK(c, dlg_goto_eval(t, 0, "end", &pos, msg, sizeof(msg)) && pos == t->doc->line_start(4), "end");
  ST_CHECK(c, dlg_goto_eval(t, 0, "+2", &pos, msg, sizeof(msg)) && pos == t->doc->line_start(2), "+2 from line 1");
  ST_CHECK(c, dlg_goto_eval(t, 0, "2:99", &pos, msg, sizeof(msg)) && pos == t->doc->line_end(1), "column past the end");
  ST_CHECK(c, !dlg_goto_eval(t, 0, "9", &pos, msg, sizeof(msg)) && strstr(msg, "5 lines") != 0, "line beyond the end");
  ST_CHECK(c, !dlg_goto_eval(t, 0, "0", &pos, msg, sizeof(msg)) && msg[0], "line 0 is an error");
  ST_CHECK(c, !dlg_goto_eval(t, 0, "abc", &pos, msg, sizeof(msg)) && msg[0], "junk is an error");
  ST_CHECK(c, !dlg_goto_eval(t, 0, "  ", &pos, msg, sizeof(msg)) && !msg[0], "empty: no message");
  ST_CHECK(c, dlg_goto_eval(t, 1, "10", &pos, msg, sizeof(msg)) && pos == 16 && strstr(msg, "0x00000010") != 0, "hex 10");
  ST_CHECK(c, dlg_goto_eval(t, 1, "#16", &pos, msg, sizeof(msg)) && pos == 16, "decimal #16");
  ST_CHECK(c, dlg_goto_eval(t, 1, "end", &pos, msg, sizeof(msg)) && pos == t->doc->length(), "hex end");
  ST_CHECK(c, !dlg_goto_eval(t, 1, "FFFF", &pos, msg, sizeof(msg)) && msg[0], "offset beyond the end");
  // the dialog itself (modeless twin): type, check the preview, press Go
  h = dlg_goto_modeless(t, 0);
  ST_CHECK(c, h != 0, "Go To dialog opens");
  if (h) {
    char res[200];
    SetDlgItemTextA(h, IDC_GOTO_EDIT, "4:3");
    st_pump(c, 50);
    sd_text(h, IDC_GOTO_RESULT, res, sizeof(res));
    ST_CHECK(c, strstr(res, "Line 4 of 5, column 3") != 0, "live preview text");
    sd_hold(c, h, "goto-text");
    SetDlgItemTextA(h, IDC_GOTO_EDIT, "77");
    st_pump(c, 50);
    ST_CHECK(c, !IsWindowEnabled(GetDlgItem(h, IDOK)), "Go disabled for an invalid line");
    sd_hold(c, h, "goto-text-error");
    SetDlgItemTextA(h, IDC_GOTO_EDIT, "4:3");
    SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    st_pump(c, 50);
    ST_CHECK(c, !IsWindow(h), "Go closes the dialog");
    ST_CHECK(c, t->sel.caret == t->doc->line_start(3) + 2 && t->sel.anchor == t->sel.caret, "caret moved to 4:3");
  }
  h = dlg_goto_modeless(t, 1);
  if (h) {
    char res[200];
    SetDlgItemTextA(h, IDC_GOTO_EDIT, "1A");
    st_pump(c, 50);
    sd_text(h, IDC_GOTO_RESULT, res, sizeof(res));
    ST_CHECK(c, strstr(res, "0x0000001A (26)") != 0, "hex preview");
    sd_hold(c, h, "goto-hex");
    SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    st_pump(c, 20);
    ST_CHECK(c, !IsWindow(h), "Cancel closes");
  }
  sd_free(c, t);
}

static HWND sd_page(HWND sheet, int i)
{
  PropSheet_SetCurSel(sheet, 0, i);
  return PropSheet_GetCurrentPageHwnd(sheet);
}

static void sd_options(StCtx* c)
{
  Config saved = g_app.cfg;
  HWND sheet, pg;
  int i;
  sheet = dlg_options_modeless(1);
  ST_CHECK(c, sheet != 0, "Options sheet opens");
  if (!sheet) return;
  dlg_options_applied();
  st_pump(c, 50);
  pg = PropSheet_GetCurrentPageHwnd(sheet);
  ST_CHECK(c, pg && GetDlgItem(pg, IDC_OE_TABW), "starts on the Editor page");
  if (sd_shots(c)) {
    static const char* const names[] = { "opt-general", "opt-editor", "opt-files", "opt-colours", "opt-hex" };
    for (i = 0; i < 5; i++) { sd_page(sheet, i); st_pump(c, 50); sd_hold(c, sheet, names[i]); }
    pg = sd_page(sheet, 1);
  }
  // every page explains its options: count the hint lines
  for (i = 0; i < 5; i++) {
    HWND p = sd_page(sheet, i), ch;
    int hints = 0, opts = 0;
    for (ch = GetWindow(p, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT)) {
      char cls[16];
      LONG st = GetWindowLong(ch, GWL_STYLE);
      int id = GetDlgCtrlID(ch);
      GetClassNameA(ch, cls, sizeof(cls));
      if (id >= IDC_OPT_HINT_FIRST && id <= IDC_OPT_HINT_LAST) hints++;
      else if (!es_icmp(cls, "Button") && ((st & 0x0F) == BS_AUTOCHECKBOX)) opts++;
    }
    st_checkf(c, hints >= 1 && hints >= opts - 1, "page %d: %d hints for %d check boxes", i, hints, opts);
  }
  // change two Editor options and Apply
  pg = sd_page(sheet, 1);
  CheckDlgButton(pg, IDC_OE_LINENUM, saved.line_numbers ? BST_UNCHECKED : BST_CHECKED);
  SendMessageA(pg, WM_COMMAND, MAKEWPARAM(IDC_OE_LINENUM, BN_CLICKED), (LPARAM)GetDlgItem(pg, IDC_OE_LINENUM));
  SetDlgItemTextA(pg, IDC_OE_TABW, "8");
  PropSheet_PressButton(sheet, PSBTN_APPLYNOW);
  st_pump(c, 50);
  ST_CHECK(c, g_app.cfg.tab_width == 8, "Apply: tab width 8");
  ST_CHECK(c, g_app.cfg.line_numbers == (saved.line_numbers ? 0 : 1), "Apply: line numbers toggled");
  ST_CHECK(c, dlg_options_applied() == 1, "Apply committed once (not once per page)");
  // an invalid number keeps the sheet open and changes nothing
  SetDlgItemTextA(pg, IDC_OE_TABW, "99");
  PropSheet_PressButton(sheet, PSBTN_OK);
  st_pump(c, 50);
  ST_CHECK(c, PropSheet_GetCurrentPageHwnd(sheet) != 0, "invalid tab width keeps the sheet open");
  ST_CHECK(c, g_app.cfg.tab_width == 8, "invalid tab width not applied");
  // Colours: make Comment bold, Hex: big-endian inspector, then OK
  SetDlgItemTextA(pg, IDC_OE_TABW, "4");
  pg = sd_page(sheet, 3);
  if (pg) {
    SendDlgItemMessageA(pg, IDC_OC_LIST, LB_SETCURSEL, 1, 0);
    SendMessageA(pg, WM_COMMAND, MAKEWPARAM(IDC_OC_LIST, LBN_SELCHANGE), (LPARAM)GetDlgItem(pg, IDC_OC_LIST));
    CheckDlgButton(pg, IDC_OC_BOLD, saved.style[ST_COMMENT].bold ? BST_UNCHECKED : BST_CHECKED);
    SendMessageA(pg, WM_COMMAND, MAKEWPARAM(IDC_OC_BOLD, BN_CLICKED), (LPARAM)GetDlgItem(pg, IDC_OC_BOLD));
  }
  pg = sd_page(sheet, 4);
  if (pg) {
    CheckRadioButton(pg, IDC_OH_LE, IDC_OH_BE, saved.inspector_be ? IDC_OH_LE : IDC_OH_BE);
    SendMessageA(pg, WM_COMMAND, MAKEWPARAM(IDC_OH_BE, BN_CLICKED), (LPARAM)GetDlgItem(pg, IDC_OH_BE));
  }
  PropSheet_PressButton(sheet, PSBTN_OK);
  st_pump(c, 50);
  ST_CHECK(c, PropSheet_GetCurrentPageHwnd(sheet) == 0, "OK closes the sheet");
  ST_CHECK(c, g_app.cfg.tab_width == 4, "OK: tab width 4");
  ST_CHECK(c, g_app.cfg.style[ST_COMMENT].bold == (saved.style[ST_COMMENT].bold ? 0 : 1), "OK: Comment bold toggled");
  ST_CHECK(c, g_app.cfg.inspector_be == (saved.inspector_be ? 0 : 1), "OK: inspector byte order");
  ST_CHECK(c, g_app.cfg.win_w == saved.win_w && !strcmp(g_app.cfg.mru[0], saved.mru[0]), "window/MRU fields untouched");
  DestroyWindow(sheet);
  // Cancel discards
  sheet = dlg_options_modeless(0);
  if (sheet) {
    pg = PropSheet_GetCurrentPageHwnd(sheet);
    CheckDlgButton(pg, IDC_OG_RESTORE, g_app.cfg.restore_session ? BST_UNCHECKED : BST_CHECKED);
    SendMessageA(pg, WM_COMMAND, MAKEWPARAM(IDC_OG_RESTORE, BN_CLICKED), (LPARAM)GetDlgItem(pg, IDC_OG_RESTORE));
    i = g_app.cfg.restore_session;
    PropSheet_PressButton(sheet, PSBTN_CANCEL);
    st_pump(c, 50);
    ST_CHECK(c, g_app.cfg.restore_session == i, "Cancel discards changes");
    DestroyWindow(sheet);
  }
  memcpy(&g_app.cfg, &saved, sizeof(saved));
  app_apply_config();
  st_pump(c, 20);
}

static void sd_exit(StCtx* c)
{
  Tab* t[3];
  u8 flags[3];
  int r, keep = g_app.cfg.backup_enabled;
  t[0] = sd_tab("one", enc_ansi(), EOL_CRLF, 1);
  t[1] = sd_tab("two", enc_ansi(), EOL_CRLF, 1);
  t[2] = sd_tab("three", enc_utf8(0), EOL_LF, 1);
  if (!t[0] || !t[1] || !t[2]) { ST_CHECK(c, 0, "tabs"); sd_free(c, t[0]); sd_free(c, t[1]); sd_free(c, t[2]); return; }
  es_copy(t[0]->path, sizeof(t[0]->path), "C:\\PICATEST\\fixtures\\one.txt");
  t[0]->untitled = 0;
  g_app.cfg.backup_enabled = 1;
  st_answer(c, Q_EXIT, IDYES);
  r = dlg_exit_prompt(t, 3, flags);
  ST_CHECK(c, r == EXIT_SAVE && flags[0] && flags[1] && flags[2], "Save Selected: all ticked by default");
  st_answer(c, Q_EXIT, IDNO);
  r = dlg_exit_prompt(t, 3, flags);
  ST_CHECK(c, r == EXIT_DISCARD && !flags[0] && !flags[2], "Discard");
  st_answer(c, Q_EXIT, IDRETRY);
  r = dlg_exit_prompt(t, 3, flags);
  ST_CHECK(c, r == EXIT_KEEP, "Keep for Next Session (backups on)");
  st_answer(c, Q_EXIT, IDCANCEL);
  r = dlg_exit_prompt(t, 3, flags);
  ST_CHECK(c, r == EXIT_CANCEL, "Cancel");
  g_app.cfg.backup_enabled = 0;
  st_answer(c, Q_EXIT, IDRETRY);
  r = dlg_exit_prompt(t, 3, flags);
  ST_CHECK(c, r == EXIT_CANCEL, "no Keep button when backups are off");
  g_app.cfg.backup_enabled = 1;
  if (sd_shots(c)) {
    HWND h = exit_modeless(t, 3, flags);
    sd_hold(c, h, "exit-prompt");
    if (h && IsWindow(h)) SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
  }
  g_app.cfg.backup_enabled = keep;
  // convert-loss prompt
  {
    ConvertReport rep;
    rep.unrepresentable = 14; rep.invalid_src = 0; rep.first_bad = 100;
    st_answer(c, Q_CONVERT_LOSS, IDYES);
    ST_CHECK(c, dlg_convert_loss("ANSI", rep, 11) == CONVERT_OK, "convert loss: Convert");
    st_answer(c, Q_CONVERT_LOSS, IDNO);
    ST_CHECK(c, dlg_convert_loss("ANSI", rep, 11) == CONVERT_GOTO, "convert loss: Go to First");
    st_answer(c, Q_CONVERT_LOSS, IDCANCEL);
    ST_CHECK(c, dlg_convert_loss("ANSI", rep, 11) == CONVERT_CANCEL, "convert loss: Cancel");
    if (sd_shots(c)) {
      HWND h;
      rep.invalid_src = 2;
      h = conv_modeless("ANSI", rep, 11);
      sd_hold(c, h, "convert-loss");
      if (h && IsWindow(h)) SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    }
  }
  sd_free(c, t[0]); sd_free(c, t[1]); sd_free(c, t[2]);
}

static void sd_info(StCtx* c)
{
  Tab* t = sd_tab("a\r\nb\nc", enc_utf8(1), EOL_CRLF, 1);
  HWND h;
  char buf[300];
  if (!t) return;
  h = dlg_properties_modeless(t);
  ST_CHECK(c, h != 0, "Properties opens");
  if (h) {
    sd_text(h, IDC_PROP_LINES, buf, sizeof(buf));
    ST_CHECK(c, !strcmp(buf, "3"), "Properties: 3 lines");
    sd_text(h, IDC_PROP_ENC, buf, sizeof(buf));
    ST_CHECK(c, strstr(buf, "UTF-8 BOM") != 0, "Properties: encoding");
    sd_text(h, IDC_PROP_EOL, buf, sizeof(buf));
    ST_CHECK(c, strstr(buf, "1 CRLF, 1 LF") != 0 && strstr(buf, "mixed") != 0, "Properties: EOL counts");
    sd_text(h, IDC_PROP_SIZE, buf, sizeof(buf));
    ST_CHECK(c, strstr(buf, "9 bytes") != 0, "Properties: size includes the BOM");
    sd_text(h, IDC_PROP_STATE, buf, sizeof(buf));
    ST_CHECK(c, strstr(buf, "Modified") != 0, "Properties: state");
    sd_hold(c, h, "properties");
    SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    st_pump(c, 20);
    ST_CHECK(c, !IsWindow(h), "Properties closes");
  }
  h = dlg_about_modeless();
  ST_CHECK(c, h != 0, "About opens");
  if (h) {
    sd_text(h, IDC_ABOUT_NAME, buf, sizeof(buf));
    ST_CHECK(c, strstr(buf, APP_NAME) != 0 && strstr(buf, APP_VERSION) != 0, "About: name and version");
    sd_text(h, IDC_ABOUT_CFG, buf, sizeof(buf));
    ST_CHECK(c, strstr(buf, g_app.cfg_dir) != 0, "About: settings folder");
    sd_hold(c, h, "about");
    SendMessageA(h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    st_pump(c, 20);
  }
  sd_free(c, t);
}

void st_dialogs(StCtx* c)
{
  ui_answer_clear();
  sd_goto(c);
  sd_options(c);
  sd_exit(c);
  sd_info(c);
  ST_CHECK(c, ui_unexpected_questions() == 0, "no unexpected questions");
  ST_CHECK(c, ui_answer_pending() == 0, "every queued answer was used");
}
