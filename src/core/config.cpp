// config.cpp - Config defaults/load/store, MRU and history lists (DESIGN 6.19, Appendix B/C). Owner: T06.
// config_load starts from the defaults and takes every valid key; numbers out of range are clamped, unparsable values
// keep the default. config_store writes every key (so the file documents all options) and keeps unknown keys.
#include "config.h"
#include "estr.h"
#include <stddef.h>
#include <string.h>

#define RGBC(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16))

static void set_style(Config* c, int st, u32 rgb, int bold)
{
  c->style[st].rgb = rgb;
  c->style[st].bold = (u8)bold;
}

void config_defaults(Config* c)
{
  memset(c, 0, sizeof(*c));
  // [General]
  c->restore_session = 1; c->backup_enabled = 1; c->backup_seconds = 5; c->ask_untitled_on_exit = 0;
  c->single_instance = 1; c->mru_count = 10; c->check_disk = 1; c->reload_silent = 1; c->poll_seconds = 3;
  // [Editor]
  es_copy(c->font_face, sizeof(c->font_face), "Courier New");
  c->font_pt = 10; c->font_px = 0; c->tab_width = 4; c->use_spaces = 0; c->use_lang_tabs = 1;
  c->auto_indent = 1; c->line_numbers = 1; c->wrap_default = 0; c->show_ws = 0; c->brace_match = 1;
  // [Files]
  c->new_enc = enc_ansi(); c->new_eol = EOL_CRLF; c->detect_utf8 = 1; c->nfo_oem = 1; c->binary_hex = 1;
  c->ondemand_mb = 16; c->large_mb = 8; c->keep_bak = 0;
  // [Hex]
  c->hex_overwrite = 1; c->inspector_on = 0; c->inspector_be = 0;
  // [Colors] (Appendix B)
  set_style(c, ST_DEFAULT, COLOR_SYSDEFAULT, 0);
  set_style(c, ST_COMMENT, RGBC(0, 128, 0), 0);
  set_style(c, ST_STRING, RGBC(128, 0, 0), 0);
  set_style(c, ST_CHAR, RGBC(128, 0, 0), 0);
  set_style(c, ST_NUMBER, RGBC(0, 128, 128), 0);
  set_style(c, ST_KEYWORD1, RGBC(0, 0, 255), 1);
  set_style(c, ST_KEYWORD2, RGBC(128, 0, 128), 0);
  set_style(c, ST_KEYWORD3, RGBC(0, 96, 160), 0);
  set_style(c, ST_KEYWORD4, RGBC(160, 80, 0), 0);
  set_style(c, ST_PREPROC, RGBC(160, 80, 0), 0);
  set_style(c, ST_OPERATOR, RGBC(0, 0, 128), 0);
  set_style(c, ST_LABEL, RGBC(128, 0, 128), 1);
  set_style(c, ST_VARIABLE, RGBC(160, 80, 0), 0);
  set_style(c, ST_SECTION, RGBC(0, 0, 160), 1);
  set_style(c, ST_KEY, RGBC(0, 96, 160), 0);
  set_style(c, ST_TAG, RGBC(0, 0, 160), 0);
  set_style(c, ST_ATTR, RGBC(160, 0, 0), 0);
  set_style(c, ST_ENTITY, RGBC(128, 0, 128), 0);
  set_style(c, ST_HEADING, RGBC(0, 0, 160), 1);
  set_style(c, ST_EMPH, RGBC(128, 0, 0), 0);
  set_style(c, ST_CODE, RGBC(0, 112, 112), 0);
  set_style(c, ST_DIFF_ADD, RGBC(0, 128, 0), 0);
  set_style(c, ST_DIFF_DEL, RGBC(192, 0, 0), 0);
  set_style(c, ST_DIFF_HUNK, RGBC(128, 0, 128), 1);
  set_style(c, ST_DIFF_HEADER, RGBC(0, 0, 160), 1);
  set_style(c, ST_INVALID, RGBC(192, 0, 0), 0);    // background 255,224,224 is fixed in render (T08)
  c->ui[UIC_MARK_BG] = RGBC(255, 240, 120);
  c->ui[UIC_BRACE_BG] = RGBC(200, 230, 255);
  c->ui[UIC_BRACE_BAD_FG] = RGBC(255, 0, 0);
  c->ui[UIC_WS_FG] = RGBC(176, 176, 176);
  c->ui[UIC_CTRL_FG] = RGBC(96, 96, 96);
  c->ui[UIC_CTRL_BG] = RGBC(224, 224, 224);
  c->ui[UIC_BAD_FG] = RGBC(160, 0, 0);
  c->ui[UIC_BAD_BG] = RGBC(255, 208, 208);
  c->ui[UIC_GUTTER_BG] = COLOR_SYSDEFAULT;
  c->ui[UIC_GUTTER_FG] = RGBC(128, 128, 128);
  c->ui[UIC_BOOKMARK] = RGBC(0, 0, 192);
  c->ui[UIC_HEX_OFFSET] = RGBC(0, 0, 128);
  c->ui[UIC_HEX_MODIFIED] = RGBC(255, 0, 0);
  c->ui[UIC_HEX_MIRROR_BG] = RGBC(210, 210, 255);
  c->ui[UIC_HEX_BOM] = RGBC(160, 160, 160);
  c->ui[UIC_ISEARCH_FAIL] = RGBC(255, 200, 200);
  // [Window]
  c->win_x = 40; c->win_y = 40; c->win_w = 760; c->win_h = 540; c->win_max = 0;
  c->win_toolbar = 1; c->win_statusbar = 1; c->win_results_h = 150; c->win_find_x = -1; c->win_find_y = -1;
  // [Find]
  c->find_case = 0; c->find_word = 0; c->find_wrap = 1; c->find_mode = 0; c->find_dotall = 0; c->find_dir_up = 0;
  c->find_fif_sub = 1;
}

// ---- key tables ----

static const char* const k_style_names[ST_COUNT] = {
  "default", "comment", "string", "char", "number", "keyword1", "keyword2", "keyword3", "keyword4", "preproc",
  "operator", "label", "variable", "section", "key", "tag", "attr", "entity", "heading", "emph", "code",
  "diff_add", "diff_del", "diff_hunk", "diff_header", "invalid"
};
static const char* const k_ui_names[UIC_COUNT] = {
  "mark_bg", "brace_bg", "brace_bad_fg", "ws_fg", "ctrl_fg", "ctrl_bg", "bad_fg", "bad_bg", "gutter_bg",
  "gutter_fg", "bookmark", "hex_offset", "hex_modified", "hex_mirror_bg", "hex_bom", "isearch_fail"
};

enum { CK_B8 = 0, CK_BI32 = 1, CK_I32 = 2, CK_U8 = 3 };   // u8 bool, i32 bool, i32 number, u8 number
struct CfgKey { const char* sec; const char* key; u8 type; u8 pad[3]; u32 off; i32 lo, hi; };

#define CB(sec, key, f)          { sec, key, CK_B8, { 0, 0, 0 }, (u32)offsetof(Config, f), 0, 1 }
#define CBI(sec, key, f)         { sec, key, CK_BI32, { 0, 0, 0 }, (u32)offsetof(Config, f), 0, 1 }
#define CI(sec, key, f, lo, hi)  { sec, key, CK_I32, { 0, 0, 0 }, (u32)offsetof(Config, f), lo, hi }
#define CU(sec, key, f, lo, hi)  { sec, key, CK_U8, { 0, 0, 0 }, (u32)offsetof(Config, f), lo, hi }

static const CfgKey k_keys[] = {
  CB("General", "restore_session", restore_session),
  CB("General", "backup_enabled", backup_enabled),
  CI("General", "backup_seconds", backup_seconds, 2, 300),
  CB("General", "ask_untitled_on_exit", ask_untitled_on_exit),
  CB("General", "single_instance", single_instance),
  CI("General", "mru_count", mru_count, 0, CFG_MRU_MAX),
  CB("General", "check_disk", check_disk),
  CB("General", "reload_silent", reload_silent),
  CI("General", "poll_seconds", poll_seconds, 1, 60),
  CI("Editor", "font_pt", font_pt, 6, 72),
  CI("Editor", "font_px", font_px, 0, 64),
  CI("Editor", "tab_width", tab_width, 1, 16),
  CB("Editor", "use_spaces", use_spaces),
  CB("Editor", "use_lang_tabs", use_lang_tabs),
  CB("Editor", "auto_indent", auto_indent),
  CB("Editor", "line_numbers", line_numbers),
  CB("Editor", "wrap_default", wrap_default),
  CB("Editor", "show_ws", show_ws),
  CB("Editor", "brace_match", brace_match),
  CB("Files", "detect_utf8", detect_utf8),
  CB("Files", "nfo_oem", nfo_oem),
  CB("Files", "binary_hex", binary_hex),
  CI("Files", "ondemand_mb", ondemand_mb, 4, 64),
  CI("Files", "large_mb", large_mb, 1, 64),
  CB("Files", "keep_bak", keep_bak),
  CB("Hex", "hex_overwrite", hex_overwrite),
  CB("Hex", "inspector_on", inspector_on),
  CB("Hex", "inspector_be", inspector_be),
  CI("Window", "x", win_x, -32000, 32000),
  CI("Window", "y", win_y, -32000, 32000),
  CI("Window", "w", win_w, 200, 16000),
  CI("Window", "h", win_h, 150, 16000),
  CBI("Window", "max", win_max),
  CBI("Window", "toolbar", win_toolbar),
  CBI("Window", "statusbar", win_statusbar),
  CI("Window", "results_h", win_results_h, 40, 4000),
  CI("Window", "find_x", win_find_x, -32000, 32000),
  CI("Window", "find_y", win_find_y, -32000, 32000),
  CB("Find", "case", find_case),
  CB("Find", "word", find_word),
  CB("Find", "wrap", find_wrap),
  CU("Find", "mode", find_mode, 0, 3),
  CB("Find", "dotall", find_dotall),
  CB("Find", "dir_up", find_dir_up),
  CB("Find", "fif_sub", find_fif_sub)
};
enum { CFG_NKEYS = sizeof(k_keys) / sizeof(k_keys[0]) };

struct CfgList { const char* sec; char prefix; u8 max; u16 width; u32 off; };
static const CfgList k_lists[] = {
  { "Recent", 'f', CFG_MRU_MAX, 260, (u32)offsetof(Config, mru) },
  { "FindHistory", 'h', CFG_HIST_MAX, 256, (u32)offsetof(Config, find_hist) },
  { "ReplaceHistory", 'h', CFG_HIST_MAX, 256, (u32)offsetof(Config, repl_hist) },
  { "FifFolders", 'h', CFG_HIST_MAX, 256, (u32)offsetof(Config, fif_dirs) },
  { "FifMasks", 'h', CFG_HIST_MAX, 256, (u32)offsetof(Config, fif_masks) }
};
enum { CFG_NLISTS = sizeof(k_lists) / sizeof(k_lists[0]) };

static const char* const k_eol_names[3] = { "crlf", "lf", "cr" };

// ---- colours: "RRGGBB[,bold]" or "sys[,bold]" ----

static int hexv(char ch)
{
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static int parse_color(const char* v, u32* rgb, int* bold)
{
  char buf[32];
  char* comma;
  u32 i, x = 0;
  es_copy(buf, sizeof(buf), v);
  comma = strchr(buf, ',');
  if (comma) {
    *comma = 0;
    es_trim(comma + 1);
    if (es_icmp(comma + 1, "1") == 0) *bold = 1;
    else if (es_icmp(comma + 1, "0") == 0) *bold = 0;
    else return E_SYNTAX;
  }
  es_trim(buf);
  if (es_icmp(buf, "sys") == 0) { *rgb = COLOR_SYSDEFAULT; return E_OK; }
  if (buf[0] == '#') memmove(buf, buf + 1, es_len(buf));
  if (es_len(buf) != 6) return E_SYNTAX;
  for (i = 0; i < 6; i++) {
    int d = hexv(buf[i]);
    if (d < 0) return E_SYNTAX;
    x = (x << 4) | (u32)d;
  }
  *rgb = RGBC((x >> 16) & 0xFF, (x >> 8) & 0xFF, x & 0xFF);   // text RRGGBB -> COLORREF 0x00BBGGRR
  return E_OK;
}

static void format_color(u32 rgb, int with_bold, int bold, char* out, u32 cap)
{
  char col[8];
  if (rgb == COLOR_SYSDEFAULT) es_copy(col, sizeof(col), "sys");
  else es_fmt(col, sizeof(col), "%02X%02X%02X", rgb & 0xFF, (rgb >> 8) & 0xFF, (rgb >> 16) & 0xFF);
  if (with_bold) es_fmt(out, cap, "%s,%d", col, bold ? 1 : 0);
  else es_copy(out, cap, col);
}

// ---- load / store ----

void config_load(Config* c, const Ini& ini)
{
  u32 i, j;
  const char* v;
  char key[32];
  config_defaults(c);
  for (i = 0; i < CFG_NKEYS; i++) {
    const CfgKey& k = k_keys[i];
    char* f = (char*)c + k.off;
    switch (k.type) {
    case CK_B8: *(u8*)f = (u8)ini.get_b(k.sec, k.key, *(u8*)f); break;
    case CK_BI32: *(i32*)f = ini.get_b(k.sec, k.key, *(i32*)f ? 1 : 0); break;
    case CK_I32: *(i32*)f = ini.get_i(k.sec, k.key, *(i32*)f, k.lo, k.hi); break;
    default: *(u8*)f = (u8)ini.get_i(k.sec, k.key, *(u8*)f, k.lo, k.hi); break;
    }
  }
  v = ini.get("Editor", "font_face");
  if (v) {
    char face[64];
    es_copy(face, sizeof(face), v);
    es_trim(face);
    if (face[0] && es_len(face) < sizeof(c->font_face)) es_copy(c->font_face, sizeof(c->font_face), face);
  }
  v = ini.get("Files", "new_enc");
  if (v) {
    EncSpec e;
    if (enc_parse(v, &e) == E_OK) c->new_enc = e;
  }
  v = ini.get("Files", "new_eol");
  if (v) for (i = 0; i < 3; i++) if (es_icmp(v, k_eol_names[i]) == 0) c->new_eol = (u8)i;
  for (i = 0; i < ST_COUNT; i++) {
    es_fmt(key, sizeof(key), "s_%s", k_style_names[i]);
    v = ini.get("Colors", key);
    if (v) {
      u32 rgb;
      int bold = c->style[i].bold;
      if (parse_color(v, &rgb, &bold) == E_OK) { c->style[i].rgb = rgb; c->style[i].bold = (u8)bold; }
    }
  }
  for (i = 0; i < UIC_COUNT; i++) {
    es_fmt(key, sizeof(key), "u_%s", k_ui_names[i]);
    v = ini.get("Colors", key);
    if (v) {
      u32 rgb;
      int bold = 0;
      if (parse_color(v, &rgb, &bold) == E_OK) c->ui[i] = rgb;
    }
  }
  for (i = 0; i < CFG_NLISTS; i++) {
    const CfgList& l = k_lists[i];
    char* base = (char*)c + l.off;
    u32 n = 0;
    for (j = 0; j < l.max; j++) {
      es_fmt(key, sizeof(key), "%c%u", l.prefix, j + 1);
      v = ini.get(l.sec, key);
      if (!v || !v[0]) continue;
      es_copy(base + n * l.width, l.width, v);
      n++;
    }
  }
}

void config_store(const Config* c, Ini* ini)
{
  u32 i, j;
  char key[32], val[300];
  if (!ini) return;
  for (i = 0; i < CFG_NKEYS; i++) {
    const CfgKey& k = k_keys[i];
    const char* f = (const char*)c + k.off;
    switch (k.type) {
    case CK_B8: ini->set_i(k.sec, k.key, *(const u8*)f ? 1 : 0); break;
    case CK_BI32: ini->set_i(k.sec, k.key, *(const i32*)f ? 1 : 0); break;
    case CK_I32: ini->set_i(k.sec, k.key, *(const i32*)f); break;
    default: ini->set_i(k.sec, k.key, *(const u8*)f); break;
    }
  }
  ini->set("Editor", "font_face", c->font_face);
  enc_token(c->new_enc, val, sizeof(val));
  ini->set("Files", "new_enc", val);
  ini->set("Files", "new_eol", k_eol_names[c->new_eol < 3 ? c->new_eol : 0]);
  for (i = 0; i < ST_COUNT; i++) {
    es_fmt(key, sizeof(key), "s_%s", k_style_names[i]);
    format_color(c->style[i].rgb, 1, c->style[i].bold, val, sizeof(val));
    ini->set("Colors", key, val);
  }
  for (i = 0; i < UIC_COUNT; i++) {
    es_fmt(key, sizeof(key), "u_%s", k_ui_names[i]);
    format_color(c->ui[i], 0, 0, val, sizeof(val));
    ini->set("Colors", key, val);
  }
  for (i = 0; i < CFG_NLISTS; i++) {
    const CfgList& l = k_lists[i];
    const char* base = (const char*)c + l.off;
    u32 n = 0;
    for (j = 0; j < l.max; j++) {
      const char* s = base + j * l.width;
      if (!s[0]) continue;
      es_fmt(key, sizeof(key), "%c%u", l.prefix, n + 1);
      ini->set(l.sec, key, s);
      n++;
    }
    for (j = n; j < l.max; j++) {                    // drop entries the list no longer has
      es_fmt(key, sizeof(key), "%c%u", l.prefix, j + 1);
      ini->remove(l.sec, key);
    }
  }
}

// ---- MRU and history lists ----

static void list_add(char* base, u32 width, u32 max, const char* s, int icase)
{
  u32 i, found;
  char item[300];
  if (!s || !s[0] || max == 0) return;
  es_copy(item, width < sizeof(item) ? width : sizeof(item), s);
  found = max - 1;                                   // not found: the last entry drops out
  for (i = 0; i < max; i++) {
    const char* e = base + i * width;
    if (!e[0]) { found = i; break; }                 // first empty slot: shift only up to here
    if (icase ? es_icmp(e, item) == 0 : strcmp(e, item) == 0) { found = i; break; }
  }
  if (found > 0) memmove(base + width, base, found * width);
  es_copy(base, width, item);
}

void config_mru_add(Config* c, const char* path)
{
  if (!c) return;
  list_add(&c->mru[0][0], sizeof(c->mru[0]), CFG_MRU_MAX, path, 1);
}

void config_hist_add(char (*list)[256], u32 max, const char* s)
{
  if (!list) return;
  list_add(&list[0][0], 256, max, s, 0);
}
