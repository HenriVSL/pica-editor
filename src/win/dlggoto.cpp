// dlggoto.cpp - Go To dialog (DESIGN 9.4, 15.9). Owner: T12.
// Text view: "line[:col]" (1-based, +N/-N relative, "end", ":col" on the current line); the column is a display
// column like the status bar's "Col". Hex view: an offset (hex by default, #123 or 123d decimal, +N/-N, end, N%).
// The result line under the field previews the target live and shows errors in red; Go is disabled while invalid.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "dialogs.h"
#include "dialogs_int.h"
#include "views.h"
#include "hexfmt.h"
#include "edlines_int.h"
#include "estr.h"
#include "ids_misc.h"
#include <string.h>

struct GotoState { Tab* t; int hex; int valid; int modeless; epos pos; };

void dlg_fmt_num(u32 v, char* out, u32 cap)
{
  char tmp[16], res[24];
  u32 n = (u32)es_fmt(tmp, sizeof(tmp), "%u", v), i, k = 0;
  for (i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0) res[k++] = ',';
    res[k++] = tmp[i];
  }
  res[k] = 0;
  es_copy(out, cap, res);
}

int dlg_goto_eval(Tab* t, int hex_mode, const char* text, epos* pos, char* msg, u32 cap)
{
  Document* d = t->doc;
  char err[160], a[24], b[24];
  const char* p = text;
  int rc;
  msg[0] = 0;
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) return 0;
  err[0] = 0;
  if (hex_mode) {
    epos off = 0;
    char hx[9];
    rc = goto_parse_offset(text, t->sel.caret, d->length(), &off, err, sizeof(err));
    if (rc != E_OK) { es_copy(msg, cap, err[0] ? err : "Not an offset"); return 0; }
    hex_offset_text(off, hx);
    dlg_fmt_num(off, a, sizeof(a));
    dlg_fmt_num(d->length(), b, sizeof(b));
    es_fmt(msg, cap, "= 0x%s (%s) of %s bytes", hx, a, b);
    *pos = off;
    return 1;
  } else {
    u32 line = 0, col = 0, cur = d->line_of(t->sel.caret), lc = d->line_count();
    u32 cells;
    epos ps;
    rc = goto_parse_line(text, cur, lc, &line, &col, err, sizeof(err));
    if (rc != E_OK) { es_copy(msg, cap, err[0] ? err : "Not a line number"); return 0; }
    ps = ed_pos_of_col(d, line, col, t->tv.tab_width);
    cells = ed_col_of(d, d->line_end(line), t->tv.tab_width);
    dlg_fmt_num(line + 1, a, sizeof(a));
    dlg_fmt_num(lc, b, sizeof(b));
    if (col == 0) es_fmt(msg, cap, "Line %s of %s", a, b);
    else if (col > cells) es_fmt(msg, cap, "Line %s of %s, end of line (column %u)", a, b, cells + 1);
    else es_fmt(msg, cap, "Line %s of %s, column %u", a, b, col + 1);
    *pos = ps;
    return 1;
  }
}

void dlg_goto_apply(Tab* t, int hex_mode, epos pos)
{
  if (pos > t->doc->length()) pos = t->doc->length();
  if (!hex_mode && pos < t->doc->text_origin()) pos = t->doc->text_origin();
  t->sel.anchor = t->sel.caret = pos;
  if (t != app_active()) return;
  if (t->view == VIEW_HEX) {
    t->hv.caret.pos = pos;
    t->hv.caret.nibble = 0;
    if (g_app.hexview) hexview_set_selection(g_app.hexview, pos, pos, SCROLL_CENTER);
  } else if (g_app.textview) textview_set_selection(g_app.textview, pos, pos, SCROLL_CENTER);
  app_update_status();
}

static void goto_update(HWND h, GotoState* s)
{
  char text[128], msg[200];
  GetDlgItemTextA(h, IDC_GOTO_EDIT, text, sizeof(text));
  s->valid = dlg_goto_eval(s->t, s->hex, text, &s->pos, msg, sizeof(msg));
  SetDlgItemTextA(h, IDC_GOTO_RESULT, msg);
  EnableWindow(GetDlgItem(h, IDOK), s->valid);
  InvalidateRect(GetDlgItem(h, IDC_GOTO_RESULT), 0, TRUE);
}

static void goto_close(HWND h, GotoState* s, int result)
{
  if (s->modeless) { DestroyWindow(h); delete s; }
  else EndDialog(h, result);
}

static BOOL CALLBACK goto_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  GotoState* s = (GotoState*)GetWindowLong(h, DWL_USER);
  switch (msg) {
  case WM_INITDIALOG: {
    char init[32];
    s = (GotoState*)lp;
    SetWindowLong(h, DWL_USER, (LONG)s);
    if (s->hex) {
      SetWindowTextA(h, "Go To Offset");
      SetDlgItemTextA(h, IDC_GOTO_LABEL, "&Offset (hex; #123 or 123d decimal; +N, -N, end, 50%):");
      es_fmt(init, sizeof(init), "%X", s->t->sel.caret);
    } else {
      SetWindowTextA(h, "Go To Line");
      SetDlgItemTextA(h, IDC_GOTO_LABEL, "&Line or line:column (+N, -N, end; :col on this line):");
      es_fmt(init, sizeof(init), "%u", s->t->doc->line_of(s->t->sel.caret) + 1);
    }
    SendDlgItemMessageA(h, IDC_GOTO_EDIT, EM_LIMITTEXT, 100, 0);
    SetDlgItemTextA(h, IDC_GOTO_EDIT, init);
    goto_update(h, s);
    SetFocus(GetDlgItem(h, IDC_GOTO_EDIT));
    SendDlgItemMessageA(h, IDC_GOTO_EDIT, EM_SETSEL, 0, -1);
    return FALSE;                                // focus set
  }
  case WM_CTLCOLORSTATIC:
    if (s && (HWND)lp == GetDlgItem(h, IDC_GOTO_RESULT)) {
      SetTextColor((HDC)wp, s->valid ? GetSysColor(COLOR_BTNTEXT) : RGB(192, 0, 0));
      SetBkColor((HDC)wp, GetSysColor(COLOR_3DFACE));
      return (BOOL)GetSysColorBrush(COLOR_3DFACE);
    }
    return FALSE;
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case IDC_GOTO_EDIT:
      if (HIWORD(wp) == EN_CHANGE && s) goto_update(h, s);
      return TRUE;
    case IDOK:
      if (!s) return TRUE;
      goto_update(h, s);
      if (!s->valid) { MessageBeep(MB_OK); SetFocus(GetDlgItem(h, IDC_GOTO_EDIT)); return TRUE; }
      dlg_goto_apply(s->t, s->hex, s->pos);
      goto_close(h, s, 1);
      return TRUE;
    case IDCANCEL:
      if (s) goto_close(h, s, 0);
      return TRUE;
    }
    break;
  }
  return FALSE;
}

int dlg_goto(Tab* t, int hex_mode)
{
  GotoState s;
  int r;
  if (!t || !t->doc) return 0;
  s.t = t; s.hex = hex_mode ? 1 : 0; s.valid = 0; s.modeless = 0; s.pos = 0;
  r = (int)DialogBoxParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_GOTO), g_app.frame, goto_proc, (LPARAM)&s);
  return r == 1 ? 1 : 0;
}

HWND dlg_goto_modeless(Tab* t, int hex_mode)
{
  GotoState* s = new GotoState;
  HWND h;
  if (!s) return 0;
  s->t = t; s->hex = hex_mode ? 1 : 0; s->valid = 0; s->modeless = 1; s->pos = 0;
  h = CreateDialogParamA(g_app.hinst, MAKEINTRESOURCEA(IDD_GOTO), g_app.frame, goto_proc, (LPARAM)s);
  if (!h) { delete s; return 0; }
  ShowWindow(h, SW_SHOW);
  return h;
}
