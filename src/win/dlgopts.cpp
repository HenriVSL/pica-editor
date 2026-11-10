// dlgopts.cpp - Options property sheet (DESIGN 15.8). Owner: T12.
// Five pages (General, Editor, Files, Colours, Hex) edit a working copy of the option part of Config (every field
// before win_x). OK/Apply validate the page, copy the options into g_app.cfg, write pica.ini (app_save_config:
// file_save temp + rename; selftests never write) and call app_apply_config. Tabs that still use the old global
// tab width / insert-spaces setting follow the new one.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stddef.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "dialogs.h"
#include "dialogs_int.h"
#include "estr.h"
#include "ids_opts.h"
#include <string.h>

enum { OPT_PAGES = 5 };

static Config g_o;                 // working copy while the sheet is open
static int g_o_dirty;              // controls changed since the last commit
static int g_o_applied;            // commits (dlg_options_applied)
static int g_o_session_applied;    // commits while the current sheet was open
static int g_o_loading;            // ignore EN_CHANGE while filling controls
static int g_o_sel = 0;            // selected item on the Colours page
static COLORREF g_custom[16];

struct OptCheck { int id; u32 off; };
static const OptCheck k_checks[] = {
  { IDC_OG_RESTORE, offsetof(Config, restore_session) }, { IDC_OG_BACKUP, offsetof(Config, backup_enabled) },
  { IDC_OG_ASKUNTITLED, offsetof(Config, ask_untitled_on_exit) }, { IDC_OG_SINGLE, offsetof(Config, single_instance) },
  { IDC_OG_CHECKDISK, offsetof(Config, check_disk) }, { IDC_OG_RELOAD, offsetof(Config, reload_silent) },
  { IDC_OE_SPACES, offsetof(Config, use_spaces) }, { IDC_OE_LANGTABS, offsetof(Config, use_lang_tabs) },
  { IDC_OE_AUTOINDENT, offsetof(Config, auto_indent) }, { IDC_OE_LINENUM, offsetof(Config, line_numbers) },
  { IDC_OE_WRAP, offsetof(Config, wrap_default) }, { IDC_OE_SHOWWS, offsetof(Config, show_ws) },
  { IDC_OE_BRACE, offsetof(Config, brace_match) },
  { IDC_OF_DETECT, offsetof(Config, detect_utf8) }, { IDC_OF_NFO, offsetof(Config, nfo_oem) },
  { IDC_OF_BINHEX, offsetof(Config, binary_hex) }, { IDC_OF_BAK, offsetof(Config, keep_bak) },
  { IDC_OH_OVERWRITE, offsetof(Config, hex_overwrite) }, { IDC_OH_INSPECTOR, offsetof(Config, inspector_on) }
};
struct OptNum { int id; u32 off; i32 lo, hi; const char* what; };
static const OptNum k_nums[] = {
  { IDC_OG_BACKUPSEC, offsetof(Config, backup_seconds), 2, 300, "the backup delay (seconds)" },
  { IDC_OG_MRU, offsetof(Config, mru_count), 0, 10, "the number of recent files" },
  { IDC_OE_TABW, offsetof(Config, tab_width), 1, 16, "the tab width" },
  { IDC_OF_ONDEMAND, offsetof(Config, ondemand_mb), 4, 64, "the read-on-demand size (MB)" },
  { IDC_OF_LARGE, offsetof(Config, large_mb), 1, 64, "the large-file size (MB)" }
};

static u8* opt_u8(u32 off) { return (u8*)&g_o + off; }
static i32* opt_i32(u32 off) { return (i32*)((u8*)&g_o + off); }

// ---- colours ----

struct ColItem { const char* name; int style; int idx; int bg; };   // style: index into style[], else ui[]
static const ColItem k_cols[] = {
  { "Default text", 1, ST_DEFAULT, 0 }, { "Comment", 1, ST_COMMENT, 0 }, { "String", 1, ST_STRING, 0 },
  { "Character", 1, ST_CHAR, 0 }, { "Number", 1, ST_NUMBER, 0 }, { "Keyword (statements)", 1, ST_KEYWORD1, 0 },
  { "Keyword (types)", 1, ST_KEYWORD2, 0 }, { "Keyword (library)", 1, ST_KEYWORD3, 0 },
  { "Keyword (user)", 1, ST_KEYWORD4, 0 }, { "Preprocessor", 1, ST_PREPROC, 0 }, { "Operator", 1, ST_OPERATOR, 0 },
  { "Label", 1, ST_LABEL, 0 }, { "Variable", 1, ST_VARIABLE, 0 }, { "Section (INI)", 1, ST_SECTION, 0 },
  { "Key (INI)", 1, ST_KEY, 0 }, { "Tag (HTML, XML)", 1, ST_TAG, 0 }, { "Attribute", 1, ST_ATTR, 0 },
  { "Entity", 1, ST_ENTITY, 0 }, { "Heading", 1, ST_HEADING, 0 }, { "Emphasis", 1, ST_EMPH, 0 },
  { "Code (Markdown)", 1, ST_CODE, 0 }, { "Diff: added", 1, ST_DIFF_ADD, 0 }, { "Diff: removed", 1, ST_DIFF_DEL, 0 },
  { "Diff: hunk", 1, ST_DIFF_HUNK, 0 }, { "Diff: header", 1, ST_DIFF_HEADER, 0 }, { "Invalid", 1, ST_INVALID, 0 },
  { "Mark All background", 0, UIC_MARK_BG, 1 }, { "Matching brace background", 0, UIC_BRACE_BG, 1 },
  { "Unmatched brace", 0, UIC_BRACE_BAD_FG, 0 }, { "Whitespace marks", 0, UIC_WS_FG, 0 },
  { "Control picture text", 0, UIC_CTRL_FG, 0 }, { "Control picture background", 0, UIC_CTRL_BG, 1 },
  { "Invalid byte text", 0, UIC_BAD_FG, 0 }, { "Invalid byte background", 0, UIC_BAD_BG, 1 },
  { "Margin background", 0, UIC_GUTTER_BG, 1 }, { "Line numbers", 0, UIC_GUTTER_FG, 0 },
  { "Bookmark", 0, UIC_BOOKMARK, 0 }, { "Hex offsets", 0, UIC_HEX_OFFSET, 0 },
  { "Hex modified bytes", 0, UIC_HEX_MODIFIED, 0 }, { "Hex caret mirror background", 0, UIC_HEX_MIRROR_BG, 1 },
  { "Hex BOM bytes", 0, UIC_HEX_BOM, 0 }, { "Search not found background", 0, UIC_ISEARCH_FAIL, 1 }
};
enum { OPT_NCOLS = sizeof(k_cols) / sizeof(k_cols[0]) };

static u32* col_value(int i) { return k_cols[i].style ? &g_o.style[k_cols[i].idx].rgb : &g_o.ui[k_cols[i].idx]; }

static COLORREF col_resolve(int i)
{
  u32 v = *col_value(i);
  if (v != COLOR_SYSDEFAULT) return (COLORREF)(v & 0xFFFFFF);
  if (!k_cols[i].style && k_cols[i].idx == UIC_GUTTER_BG) return GetSysColor(COLOR_3DFACE);
  return GetSysColor(k_cols[i].bg ? COLOR_WINDOW : COLOR_WINDOWTEXT);
}

static void col_default(int i, Config* defs)
{
  if (k_cols[i].style) g_o.style[k_cols[i].idx] = defs->style[k_cols[i].idx];
  else g_o.ui[k_cols[i].idx] = defs->ui[k_cols[i].idx];
}

static void col_item_text(int i, char* out, u32 cap)
{
  es_copy(out, cap, k_cols[i].name);
  if (*col_value(i) == COLOR_SYSDEFAULT) es_cat(out, cap, " (system)");
  if (k_cols[i].style && g_o.style[k_cols[i].idx].bold) es_cat(out, cap, ", bold");
}

static void col_fill_list(HWND h)
{
  HWND lb = GetDlgItem(h, IDC_OC_LIST);
  char buf[80];
  int i;
  SendMessageA(lb, WM_SETREDRAW, FALSE, 0);
  SendMessageA(lb, LB_RESETCONTENT, 0, 0);
  for (i = 0; i < OPT_NCOLS; i++) { col_item_text(i, buf, sizeof(buf)); SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)buf); }
  SendMessageA(lb, LB_SETCURSEL, (WPARAM)g_o_sel, 0);
  SendMessageA(lb, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(lb, 0, TRUE);
}

static void col_update_controls(HWND h)
{
  int st = k_cols[g_o_sel].style;
  EnableWindow(GetDlgItem(h, IDC_OC_BOLD), st);
  CheckDlgButton(h, IDC_OC_BOLD, st && g_o.style[k_cols[g_o_sel].idx].bold ? BST_CHECKED : BST_UNCHECKED);
  InvalidateRect(GetDlgItem(h, IDC_OC_SAMPLE), 0, TRUE);
}

static void col_refresh_item(HWND h, int i)
{
  HWND lb = GetDlgItem(h, IDC_OC_LIST);
  char buf[80];
  col_item_text(i, buf, sizeof(buf));
  SendMessageA(lb, LB_DELETESTRING, (WPARAM)i, 0);
  SendMessageA(lb, LB_INSERTSTRING, (WPARAM)i, (LPARAM)buf);
  SendMessageA(lb, LB_SETCURSEL, (WPARAM)g_o_sel, 0);
}

static int col_choose(HWND owner, u32* value, COLORREF current)
{
  CHOOSECOLORA cc;
  memset(&cc, 0, sizeof(cc));
  cc.lStructSize = sizeof(cc);
  cc.hwndOwner = owner;
  cc.rgbResult = current;
  cc.lpCustColors = g_custom;
  cc.Flags = CC_RGBINIT;
  if (!ChooseColorA(&cc)) return 0;
  *value = (u32)(cc.rgbResult & 0xFFFFFF);
  return 1;
}

static void opt_draw_sample(const DRAWITEMSTRUCT* di, int item)
{
  RECT r = di->rcItem;
  COLORREF fg, bg;
  HBRUSH br;
  HFONT f, old = 0;
  int bold = k_cols[item].style && g_o.style[k_cols[item].idx].bold;
  HDC dc = di->hDC;
  int h = g_o.font_px > 0 ? g_o.font_px : -MulDiv(g_o.font_pt > 0 ? g_o.font_pt : 10, GetDeviceCaps(dc, LOGPIXELSY), 72);
  const char* text = "AaBb 123 {}";
  if (k_cols[item].bg) { bg = col_resolve(item); fg = GetSysColor(COLOR_WINDOWTEXT); }
  else { fg = col_resolve(item); bg = GetSysColor(COLOR_WINDOW); }
  if (!k_cols[item].style && k_cols[item].idx == UIC_GUTTER_FG) bg = GetSysColor(COLOR_3DFACE);
  DrawEdge(dc, &r, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
  br = CreateSolidBrush(bg);
  FillRect(dc, &r, br);
  DeleteObject(br);
  f = CreateFontA(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN,
                  g_o.font_face[0] ? g_o.font_face : "Courier New");
  if (f) old = (HFONT)SelectObject(dc, f);
  SetTextColor(dc, fg);
  SetBkMode(dc, TRANSPARENT);
  DrawTextA(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (f) { SelectObject(dc, old); DeleteObject(f); }
}

static void opt_draw_swatch(const DRAWITEMSTRUCT* di, COLORREF c)
{
  RECT r = di->rcItem;
  HBRUSH br;
  DrawEdge(di->hDC, &r, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
  br = CreateSolidBrush(c);
  FillRect(di->hDC, &r, br);
  DeleteObject(br);
}

static int col_index_of_ui(int uic)
{
  int i;
  for (i = 0; i < OPT_NCOLS; i++) if (!k_cols[i].style && k_cols[i].idx == uic) return i;
  return 0;
}

// ---- font ----

static void font_show(HWND h)
{
  char buf[64];
  if (g_o.font_px > 0) es_fmt(buf, sizeof(buf), "%s, %d pixels", g_o.font_face, g_o.font_px);
  else es_fmt(buf, sizeof(buf), "%s, %d pt", g_o.font_face, g_o.font_pt);
  SetDlgItemTextA(h, IDC_OE_FONTNAME, buf);
}

static int font_choose(HWND h)
{
  LOGFONTA lf;
  CHOOSEFONTA cf;
  HDC dc = GetDC(h);
  HFONT f, old;
  TEXTMETRICA tm;
  memset(&lf, 0, sizeof(lf));
  lf.lfHeight = g_o.font_px > 0 ? g_o.font_px : -MulDiv(g_o.font_pt > 0 ? g_o.font_pt : 10, GetDeviceCaps(dc, LOGPIXELSY), 72);
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
  es_copy(lf.lfFaceName, sizeof(lf.lfFaceName), g_o.font_face[0] ? g_o.font_face : "Courier New");
  memset(&cf, 0, sizeof(cf));
  cf.lStructSize = sizeof(cf);
  cf.hwndOwner = h;
  cf.lpLogFont = &lf;
  cf.Flags = CF_SCREENFONTS | CF_FIXEDPITCHONLY | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST;
  if (!ChooseFontA(&cf)) { ReleaseDC(h, dc); return 0; }
  es_copy(g_o.font_face, sizeof(g_o.font_face), lf.lfFaceName);
  g_o.font_pt = cf.iPointSize > 0 ? (cf.iPointSize + 5) / 10 : 10;
  g_o.font_px = 0;
  f = CreateFontIndirectA(&lf);                    // raster fonts (Fixedsys, Terminal) keep their pixel height
  if (f) {
    old = (HFONT)SelectObject(dc, f);
    if (GetTextMetricsA(dc, &tm) && !(tm.tmPitchAndFamily & (TMPF_TRUETYPE | TMPF_VECTOR))) g_o.font_px = tm.tmHeight;
    SelectObject(dc, old);
    DeleteObject(f);
  }
  ReleaseDC(h, dc);
  return 1;
}

// ---- encoding combo ----

static const char* const k_enc_names[] = { "ANSI", "UTF-8", "UTF-8 with BOM", "UTF-16 LE", "UTF-16 BE" };

static EncSpec enc_choice(int i)
{
  switch (i) {
  case 1: return enc_utf8(0);
  case 2: return enc_utf8(1);
  case 3: return enc_utf16le(1);
  case 4: return enc_utf16be(1);
  default: return enc_ansi();
  }
}

// ---- generic page plumbing ----

static void page_load(HWND h, int page)
{
  u32 i;
  g_o_loading = 1;
  for (i = 0; i < sizeof(k_checks) / sizeof(k_checks[0]); i++)
    if (GetDlgItem(h, k_checks[i].id)) CheckDlgButton(h, k_checks[i].id, *opt_u8(k_checks[i].off) ? BST_CHECKED : BST_UNCHECKED);
  for (i = 0; i < sizeof(k_nums) / sizeof(k_nums[0]); i++)
    if (GetDlgItem(h, k_nums[i].id)) {
      SendDlgItemMessageA(h, k_nums[i].id, EM_LIMITTEXT, 3, 0);
      SetDlgItemInt(h, k_nums[i].id, (UINT)*opt_i32(k_nums[i].off), FALSE);
    }
  if (page == 1) font_show(h);
  if (page == 2) {
    HWND ce = GetDlgItem(h, IDC_OF_ENC), cl = GetDlgItem(h, IDC_OF_EOL);
    int sel = -1;
    char lab[40];
    SendMessageA(ce, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < 5; i++) {
      SendMessageA(ce, CB_ADDSTRING, 0, (LPARAM)k_enc_names[i]);
      if (enc_equal(enc_choice((int)i), g_o.new_enc)) sel = (int)i;
    }
    if (sel < 0) {                                 // an encoding set in pica.ini by hand: keep it selectable
      enc_label(g_o.new_enc, lab, sizeof(lab));
      SendMessageA(ce, CB_ADDSTRING, 0, (LPARAM)lab);
      sel = 5;
    }
    SendMessageA(ce, CB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(cl, CB_RESETCONTENT, 0, 0);
    SendMessageA(cl, CB_ADDSTRING, 0, (LPARAM)"Windows (CRLF)");
    SendMessageA(cl, CB_ADDSTRING, 0, (LPARAM)"Unix (LF)");
    SendMessageA(cl, CB_ADDSTRING, 0, (LPARAM)"Mac (CR)");
    SendMessageA(cl, CB_SETCURSEL, (WPARAM)(g_o.new_eol <= 2 ? g_o.new_eol : 0), 0);
  }
  if (page == 3) { col_fill_list(h); col_update_controls(h); }
  if (page == 4) CheckRadioButton(h, IDC_OH_LE, IDC_OH_BE, g_o.inspector_be ? IDC_OH_BE : IDC_OH_LE);
  g_o_loading = 0;
}

// Reads the page into g_o. validate: refuse bad numbers with a message (returns 0).
static int page_read(HWND h, int page, int validate)
{
  u32 i;
  for (i = 0; i < sizeof(k_checks) / sizeof(k_checks[0]); i++)
    if (GetDlgItem(h, k_checks[i].id)) *opt_u8(k_checks[i].off) = (u8)(IsDlgButtonChecked(h, k_checks[i].id) == BST_CHECKED);
  for (i = 0; i < sizeof(k_nums) / sizeof(k_nums[0]); i++) {
    BOOL ok = FALSE;
    UINT v;
    if (!GetDlgItem(h, k_nums[i].id)) continue;
    v = GetDlgItemInt(h, k_nums[i].id, &ok, FALSE);
    if (!ok || (i32)v < k_nums[i].lo || (i32)v > k_nums[i].hi) {
      if (validate) {
        ui_error("Enter a number from %d to %d for %s.", (int)k_nums[i].lo, (int)k_nums[i].hi, k_nums[i].what);
        SetFocus(GetDlgItem(h, k_nums[i].id));
        SendDlgItemMessageA(h, k_nums[i].id, EM_SETSEL, 0, -1);
        return 0;
      }
      continue;
    }
    *opt_i32(k_nums[i].off) = (i32)v;
  }
  if (page == 2) {
    int e = (int)SendDlgItemMessageA(h, IDC_OF_ENC, CB_GETCURSEL, 0, 0);
    int l = (int)SendDlgItemMessageA(h, IDC_OF_EOL, CB_GETCURSEL, 0, 0);
    if (e >= 0 && e < 5) g_o.new_enc = enc_choice(e);
    if (l >= 0 && l <= 2) g_o.new_eol = (u8)l;
  }
  if (page == 4) g_o.inspector_be = (u8)(IsDlgButtonChecked(h, IDC_OH_BE) == BST_CHECKED);
  return 1;
}

static void opt_commit()
{
  Config old = g_app.cfg;
  int i, n = app_tab_count();
  if (!g_o_dirty) return;
  memcpy(&g_app.cfg, &g_o, offsetof(Config, win_x));
  for (i = 0; i < n; i++) {                        // tabs that followed the global setting keep following it
    Tab* t = app_tab(i);
    if (t->tv.tab_width == (u8)old.tab_width) t->tv.tab_width = (u8)g_app.cfg.tab_width;
    if (t->tv.use_spaces == old.use_spaces) t->tv.use_spaces = g_app.cfg.use_spaces;
  }
  g_o_dirty = 0;
  g_o_applied++;
  g_o_session_applied++;
  if (app_save_config() < 0) app_status_note("Settings could not be written to %s", APP_INI);
  app_apply_config();
}

static void page_changed(HWND h)
{
  if (g_o_loading) return;
  g_o_dirty = 1;
  PropSheet_Changed(GetParent(h), h);
}

static BOOL CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  int page = (int)GetWindowLong(h, DWL_USER);
  switch (msg) {
  case WM_INITDIALOG:
    page = (int)((PROPSHEETPAGEA*)lp)->lParam;
    SetWindowLong(h, DWL_USER, (LONG)page);
    page_load(h, page);
    return TRUE;
  case WM_CTLCOLORSTATIC:
    if (GetDlgCtrlID((HWND)lp) >= IDC_OPT_HINT_FIRST && GetDlgCtrlID((HWND)lp) <= IDC_OPT_HINT_LAST) {   // explanations: grey
      SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT) == GetSysColor(COLOR_3DFACE) ? GetSysColor(COLOR_BTNTEXT)
                                                                                     : GetSysColor(COLOR_GRAYTEXT));
      SetBkMode((HDC)wp, TRANSPARENT);
      return (BOOL)GetSysColorBrush(COLOR_3DFACE);
    }
    return FALSE;
  case WM_DRAWITEM: {
    const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
    if (di->CtlID == IDC_OC_SAMPLE) { opt_draw_sample(di, g_o_sel); return TRUE; }
    if (di->CtlID == IDC_OH_MODSAMPLE) { opt_draw_swatch(di, col_resolve(col_index_of_ui(UIC_HEX_MODIFIED))); return TRUE; }
    return FALSE;
  }
  case WM_COMMAND: {
    int id = LOWORD(wp), code = HIWORD(wp);
    if (id == IDC_OE_FONT && code == BN_CLICKED) {
      if (font_choose(h)) { font_show(h); page_changed(h); }
      return TRUE;
    }
    if (id == IDC_OC_LIST) {
      if (code == LBN_SELCHANGE) {
        int s = (int)SendDlgItemMessageA(h, IDC_OC_LIST, LB_GETCURSEL, 0, 0);
        if (s >= 0 && s < OPT_NCOLS) { g_o_sel = s; col_update_controls(h); }
      } else if (code == LBN_DBLCLK) PostMessageA(h, WM_COMMAND, MAKEWPARAM(IDC_OC_COLOR, BN_CLICKED), 0);
      return TRUE;
    }
    if (id == IDC_OC_COLOR && code == BN_CLICKED) {
      if (col_choose(h, col_value(g_o_sel), col_resolve(g_o_sel))) {
        col_refresh_item(h, g_o_sel); col_update_controls(h); page_changed(h);
      }
      return TRUE;
    }
    if (id == IDC_OC_BOLD && code == BN_CLICKED) {
      if (k_cols[g_o_sel].style) {
        g_o.style[k_cols[g_o_sel].idx].bold = (u8)(IsDlgButtonChecked(h, IDC_OC_BOLD) == BST_CHECKED);
        col_refresh_item(h, g_o_sel); col_update_controls(h); page_changed(h);
      }
      return TRUE;
    }
    if ((id == IDC_OC_DEFAULT || id == IDC_OC_RESET) && code == BN_CLICKED) {
      Config defs;
      int i;
      config_defaults(&defs);
      if (id == IDC_OC_DEFAULT) col_default(g_o_sel, &defs);
      else for (i = 0; i < OPT_NCOLS; i++) col_default(i, &defs);
      col_fill_list(h); col_update_controls(h); page_changed(h);
      return TRUE;
    }
    if (id == IDC_OH_MODCOLOR && code == BN_CLICKED) {
      int i = col_index_of_ui(UIC_HEX_MODIFIED);
      if (col_choose(h, col_value(i), col_resolve(i))) {
        InvalidateRect(GetDlgItem(h, IDC_OH_MODSAMPLE), 0, TRUE); page_changed(h);
      }
      return TRUE;
    }
    if ((code == BN_CLICKED && id != IDC_OE_FONT) || code == EN_CHANGE || code == CBN_SELCHANGE) page_changed(h);
    return FALSE;
  }
  case WM_NOTIFY: {
    NMHDR* nm = (NMHDR*)lp;
    switch (nm->code) {
    case PSN_SETACTIVE:
      if (page == 3) { col_fill_list(h); col_update_controls(h); }   // the Hex page may have changed a colour
      if (page == 4) InvalidateRect(GetDlgItem(h, IDC_OH_MODSAMPLE), 0, TRUE);
      SetWindowLong(h, DWL_MSGRESULT, 0);
      return TRUE;
    case PSN_KILLACTIVE:
      SetWindowLong(h, DWL_MSGRESULT, page_read(h, page, 1) ? FALSE : TRUE);
      return TRUE;
    case PSN_APPLY:
      page_read(h, page, 0);
      opt_commit();
      SetWindowLong(h, DWL_MSGRESULT, PSNRET_NOERROR);
      return TRUE;
    }
    return FALSE;
  }
  }
  return FALSE;
}

static int opt_sheet(int page, int modeless, HWND* out)
{
  static const int ids[OPT_PAGES] = { IDD_OPT_GENERAL, IDD_OPT_EDITOR, IDD_OPT_FILES, IDD_OPT_COLORS, IDD_OPT_HEX };
  PROPSHEETPAGEA pg[OPT_PAGES];
  PROPSHEETHEADERA ph;
  int i, r;
  InitCommonControls();
  g_o = g_app.cfg;
  g_o_dirty = 0;
  g_o_session_applied = 0;
  if (g_o_sel < 0 || g_o_sel >= OPT_NCOLS) g_o_sel = 0;
  memset(pg, 0, sizeof(pg));
  for (i = 0; i < OPT_PAGES; i++) {
    pg[i].dwSize = sizeof(PROPSHEETPAGEA);
    pg[i].dwFlags = PSP_DEFAULT;
    pg[i].hInstance = g_app.hinst;
    pg[i].pszTemplate = MAKEINTRESOURCEA(ids[i]);
    pg[i].pfnDlgProc = page_proc;
    pg[i].lParam = i;
  }
  memset(&ph, 0, sizeof(ph));
  ph.dwSize = sizeof(PROPSHEETHEADERA);
  ph.dwFlags = PSH_PROPSHEETPAGE | (modeless ? PSH_MODELESS : 0);
  ph.hwndParent = g_app.frame;
  ph.hInstance = g_app.hinst;
  ph.pszCaption = "Options";
  ph.nPages = OPT_PAGES;
  ph.nStartPage = (page >= 0 && page < OPT_PAGES) ? (UINT)page : 0;
  ph.ppsp = pg;
  r = (int)PropertySheetA(&ph);
  if (modeless) { if (out) *out = (HWND)r; return r != 0 && r != -1; }
  return g_o_session_applied > 0 ? 1 : 0;
}

int dlg_options(int page)
{
  return opt_sheet(page, 0, 0);
}

HWND dlg_options_modeless(int page)
{
  HWND h = 0;
  if (!opt_sheet(page, 1, &h)) return 0;
  return h;
}

int dlg_options_applied()
{
  int n = g_o_applied;
  g_o_applied = 0;
  return n;
}
