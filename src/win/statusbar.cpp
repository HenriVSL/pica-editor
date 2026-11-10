// statusbar.cpp - status bar with the 9 parts of DESIGN 15.3: texts for the text and hex views, transient notes,
// status progress, click targets (Go To, Syntax/Encoding/Line endings popups, INS/OVR, TEXT/HEX). Owner: T07.
#include <windows.h>
#include <commctrl.h>
#include <stdarg.h>
#include <string.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "views.h"
#include "selftest.h"
#include "frame_int.h"
#include "codec.h"
#include "lexer.h"
#include "estr.h"
#include "resource.h"

enum { SB_TEXT_SET = 0, SB_HEX_SET = 1 };
static int g_widths[2][SB_PARTS - 1];       // part widths for the text view and the hex view
static int g_cur_set = -1, g_cur_w = -1;
static char g_note[300];
static char g_text[SB_PARTS][160];          // what each part shows now (only changed parts are sent)

static int text_w(HDC dc, const char* s)
{
  SIZE sz;
  if (!GetTextExtentPoint32A(dc, s, (int)es_len(s), &sz)) return 8 * (int)es_len(s);
  return sz.cx;
}

static void measure_widths()
{
  static const char* const k_text[SB_PARTS - 1] = {
    "Ln 99,999, Col 999", "Sel 99,999 (999 lines)", "U+10FFFF", "Plain text", "UTF-16 LE no BOM", "Mixed", "OVR",
    "TEXT"
  };
  static const char* const k_hex[SB_PARTS - 1] = {
    "Offset 0x0000FFFF (99,999,999)", "Sel 9,999,999 bytes", "Byte 0xFF 255", "Plain text", "UTF-16 LE no BOM",
    "Mixed", "OVR", "TEXT"
  };
  HDC dc = GetDC(0);
  HFONT old = (HFONT)SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
  int i, lang_w = text_w(dc, "Plain text");
  u32 k;
  for (k = 0; k < lang_count(); k++) {
    const LangInfo* l = lang_at(k);
    int w = l ? text_w(dc, l->name) : 0;
    if (w > lang_w) lang_w = w;
  }
  for (i = 0; i < SB_PARTS - 1; i++) {
    g_widths[SB_TEXT_SET][i] = text_w(dc, k_text[i]) + 10;
    g_widths[SB_HEX_SET][i] = text_w(dc, k_hex[i]) + 10;
  }
  g_widths[SB_TEXT_SET][SB_PART_LANG] = g_widths[SB_HEX_SET][SB_PART_LANG] = lang_w + 10;
  SelectObject(dc, old);
  ReleaseDC(0, dc);
}

HWND statusbar_create(HWND parent)
{
  measure_widths();
  g_app.statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "",
                                    WS_CHILD | WS_CLIPSIBLINGS | SBARS_SIZEGRIP | (g_app.cfg.win_statusbar ? WS_VISIBLE : 0),
                                    0, 0, 0, 0, parent, (HMENU)ID_STATUSBAR, g_app.hinst, 0);
  g_cur_set = -1;
  statusbar_layout();
  return g_app.statusbar;
}

void statusbar_layout()
{
  int edges[SB_PARTS];
  int i, x = 0, set;
  RECT rc;
  Tab* t = app_active();
  if (!g_app.statusbar) return;
  set = t && t->view == VIEW_HEX ? SB_HEX_SET : SB_TEXT_SET;
  GetClientRect(g_app.statusbar, &rc);
  if (set == g_cur_set && rc.right == g_cur_w) return;
  g_cur_set = set;
  g_cur_w = rc.right;
  for (i = 0; i < SB_PARTS - 1; i++) { x += g_widths[set][i]; edges[i] = x; }
  edges[SB_PARTS - 1] = -1;
  SendMessageA(g_app.statusbar, SB_SETPARTS, SB_PARTS, (LPARAM)edges);
}

void statusbar_set(int part, const char* text)
{
  if (part < 0 || part >= SB_PARTS) return;
  if (!text) text = "";
  if (strcmp(g_text[part], text) == 0) return;
  es_copy(g_text[part], sizeof(g_text[part]), text);
  if (g_app.statusbar) SendMessageA(g_app.statusbar, SB_SETTEXTA, (WPARAM)part, (LPARAM)g_text[part]);
}

void statusbar_get(int part, char* out, u32 cap)
{
  out[0] = 0;
  if (part >= 0 && part < SB_PARTS) es_copy(out, cap, g_text[part]);
}

static void note_v(u32 ms, const char* fmt, va_list ap)
{
  es_vfmt(g_note, sizeof(g_note), fmt, ap);
  statusbar_set(SB_PART_FLAGS, g_note);
  selftest_log("note: %s", g_note);
  if (g_app.frame) SetTimer(g_app.frame, TIMER_NOTE, ms, 0);
}

void app_status_note(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  note_v(5000, fmt, ap);
  va_end(ap);
}

void statusbar_note_ms(u32 ms, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  note_v(ms, fmt, ap);
  va_end(ap);
}

void statusbar_note_expired()
{
  if (g_app.frame) KillTimer(g_app.frame, TIMER_NOTE);
  g_note[0] = 0;
  app_update_status();
}

// ---- texts (15.3) ----

static void fmt_num(u32 v, char* out, u32 cap)
{
  char digits[16], tmp[24];
  u32 n = 0, i, k = 0;
  do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < sizeof(digits));
  for (i = n; i > 0; i--) {
    tmp[k++] = digits[i - 1];
    if (i > 1 && (i - 1) % 3 == 0) tmp[k++] = ',';
  }
  tmp[k] = 0;
  es_copy(out, cap, tmp);
}

static void flag_add(char* out, u32 cap, const char* s)
{
  if (!s[0]) return;
  if (out[0]) es_cat(out, cap, "  ");
  es_cat(out, cap, s);
}

void app_update_status()
{
  Tab* t = app_active();
  CaretInfo ci;
  char buf[160], a[32], b[32], flags[300];
  int hex;
  if (!g_app.statusbar) return;
  if (!t || !t->doc) {
    int i;
    for (i = 0; i < SB_PARTS - 1; i++) statusbar_set(i, "");
    flags[0] = 0;
    progress_status_text(flags, sizeof(flags));
    statusbar_set(SB_PART_FLAGS, g_note[0] ? g_note : flags);
    return;
  }
  hex = t->view == VIEW_HEX;
  memset(&ci, 0, sizeof(ci));
  if (hex) { if (g_app.hexview) hexview_caret_info(g_app.hexview, &ci); }
  else if (g_app.textview) textview_caret_info(g_app.textview, &ci);
  if (!g_app.hexview && !g_app.textview) { ci.pos = t->sel.caret; }

  // 1 position, 2 selection, 3 character
  if (hex) {
    fmt_num(ci.pos, a, sizeof(a));
    es_fmt(buf, sizeof(buf), "Offset 0x%08X (%s)", ci.pos, a);
    statusbar_set(SB_PART_POS, buf);
    fmt_num(ci.sel_len, a, sizeof(a));
    if (ci.sel_len) es_fmt(buf, sizeof(buf), "Sel %s byte%s", a, ci.sel_len == 1 ? "" : "s");
    else es_copy(buf, sizeof(buf), "Sel 0");
    statusbar_set(SB_PART_SEL, buf);
    if (ci.has_ch) es_fmt(buf, sizeof(buf), "Byte 0x%02X %u", ci.ch & 0xFF, ci.ch & 0xFF);
    else es_copy(buf, sizeof(buf), "End of file");
    statusbar_set(SB_PART_CHAR, buf);
  } else {
    fmt_num(ci.line + 1, a, sizeof(a));
    es_fmt(buf, sizeof(buf), "Ln %s, Col %u", a, ci.col ? ci.col : 1);
    statusbar_set(SB_PART_POS, buf);
    fmt_num(ci.sel_len, a, sizeof(a));
    if (ci.sel_lines > 1) { fmt_num(ci.sel_lines, b, sizeof(b)); es_fmt(buf, sizeof(buf), "Sel %s (%s lines)", a, b); }
    else es_fmt(buf, sizeof(buf), "Sel %s", a);
    statusbar_set(SB_PART_SEL, buf);
    if (!ci.has_ch) es_copy(buf, sizeof(buf), "");
    else if (ci.ch >= ECP_BADUNIT(0)) es_fmt(buf, sizeof(buf), "x%04X bad", ci.ch - ECP_BADUNIT(0));
    else if (ci.ch >= ECP_BADBYTE(0)) es_fmt(buf, sizeof(buf), "x%02X bad", ci.ch - ECP_BADBYTE(0));
    else es_fmt(buf, sizeof(buf), "U+%04X", ci.ch);
    statusbar_set(SB_PART_CHAR, buf);
  }

  // 4 language, 5 encoding, 6 line endings, 7 mode, 8 view
  statusbar_set(SB_PART_LANG, t->lang ? t->lang->name : "Plain text");
  enc_label(t->doc->enc(), buf, sizeof(buf));
  statusbar_set(SB_PART_ENC, buf);
  if (t->doc->eol_mixed()) es_copy(buf, sizeof(buf), "Mixed");
  else {
    EolStyle e = t->doc->eol_style();
    es_copy(buf, sizeof(buf), e == EOL_LF ? "LF" : e == EOL_CR ? "CR" : "CRLF");
  }
  statusbar_set(SB_PART_EOL, buf);
  {
    int ovr = ci.overwrite || (hex ? !t->hv.caret.insert : t->tv.overwrite);
    statusbar_set(SB_PART_MODE, ovr ? "OVR" : "INS");
  }
  statusbar_set(SB_PART_VIEW, hex ? "HEX" : "TEXT");

  // 9 flags (a note replaces them for 5 s)
  flags[0] = 0;
  if (t->doc->read_only()) flag_add(flags, sizeof(flags), "RO");
  if (t->large) flag_add(flags, sizeof(flags), "Large");
  if (t->indexing) { es_fmt(buf, sizeof(buf), "Indexing %u%%", t->index_pct); flag_add(flags, sizeof(flags), buf); }
  if (t->saving) { es_fmt(buf, sizeof(buf), "Saving %u%%", t->save_pct); flag_add(flags, sizeof(flags), buf); }
  buf[0] = 0;
  progress_status_text(buf, sizeof(buf));
  flag_add(flags, sizeof(flags), buf);
  if (hex) {
    epos len = t->doc->length();
    fmt_num(len, a, sizeof(a));
    if (t->disk.exists && !t->disk.size_hi && t->disk.size_lo != len) {
      u32 d = len > t->disk.size_lo ? len - t->disk.size_lo : t->disk.size_lo - len;
      fmt_num(d, b, sizeof(b));
      es_fmt(buf, sizeof(buf), "Size %s (%c%s)", a, len > t->disk.size_lo ? '+' : '-', b);
    } else {
      es_fmt(buf, sizeof(buf), "Size %s", a);
    }
    flag_add(flags, sizeof(flags), buf);
  }
  statusbar_set(SB_PART_FLAGS, g_note[0] ? g_note : flags);
}

// ---- clicks ----

int statusbar_part_at(int x)
{
  int i;
  RECT r;
  if (!g_app.statusbar) return -1;
  for (i = 0; i < SB_PARTS; i++) {
    if (!SendMessageA(g_app.statusbar, SB_GETRECT, (WPARAM)i, (LPARAM)&r)) continue;
    if (x >= r.left && x < r.right) return i;
  }
  return -1;
}

static HMENU find_submenu(HMENU m, int id)
{
  int i, n = GetMenuItemCount(m);
  for (i = 0; i < n; i++) {
    HMENU sub = GetSubMenu(m, i);
    if (sub) {
      HMENU f;
      if (GetMenuState(sub, (UINT)id, MF_BYCOMMAND) != 0xFFFFFFFF) {
        // the id is somewhere below sub: is it a direct item of sub?
        int k, c = GetMenuItemCount(sub);
        for (k = 0; k < c; k++) if (GetMenuItemID(sub, k) == (UINT)id) return sub;
        f = find_submenu(sub, id);
        if (f) return f;
      }
    }
  }
  return 0;
}

HMENU statusbar_popup_menu(int part)
{
  HMENU m = g_app.frame ? GetMenu(g_app.frame) : 0;
  if (!m) return 0;
  if (part == SB_PART_LANG) return find_submenu(m, IDM_LANG_AUTO);
  if (part == SB_PART_ENC) {                       // the Encoding menu holds "Reload as" + the conversions
    return find_submenu(m, IDM_ENC_CONV_ANSI);
  }
  if (part == SB_PART_EOL) return find_submenu(m, IDM_EOL_CRLF);
  return 0;
}

void statusbar_click(int part)
{
  if (!app_active()) return;
  switch (part) {
  case SB_PART_POS: app_command(IDM_SEARCH_GOTO); break;
  case SB_PART_MODE: app_command(IDM_EDIT_OVERWRITE); break;
  case SB_PART_VIEW: app_command(IDM_VIEW_HEX); break;
  case SB_PART_LANG: case SB_PART_ENC: case SB_PART_EOL: {
    HMENU sub = statusbar_popup_menu(part);
    RECT r;
    POINT p;
    int cmd;
    if (!sub || !SendMessageA(g_app.statusbar, SB_GETRECT, (WPARAM)part, (LPARAM)&r)) break;
    p.x = r.left; p.y = r.top;
    ClientToScreen(g_app.statusbar, &p);
    cmd = (int)TrackPopupMenu(sub, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_LEFTBUTTON, p.x, p.y, 0,
                              g_app.frame, 0);
    if (cmd) app_command(cmd);
    break;
  }
  default:
    break;
  }
}
