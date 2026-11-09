// config.cpp - Config defaults/load/store, MRU and history lists (DESIGN 6.19, Appendix C).
// T00: config_defaults works (Appendix B/C values); load/store/mru/hist are stubs. Owner after Wave 0: T06.
#include "config.h"
#include "estr.h"
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

void config_load(Config* c, const Ini& ini) { (void)ini; config_defaults(c); }
void config_store(const Config* c, Ini* ini) { (void)c; (void)ini; }
void config_mru_add(Config* c, const char* path) { (void)c; (void)path; }
void config_hist_add(char (*list)[256], u32 max, const char* s) { (void)list; (void)max; (void)s; }
