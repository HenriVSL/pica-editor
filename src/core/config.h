// config.h - settings (DESIGN 6.19, Appendix C). Owner after Wave 0: T06.
#ifndef PICA_CONFIG_H
#define PICA_CONFIG_H

#include "etypes.h"
#include "codec.h"
#include "lexer.h"
#include "ini.h"

enum UiColor { UIC_MARK_BG = 0, UIC_BRACE_BG, UIC_BRACE_BAD_FG, UIC_WS_FG, UIC_CTRL_FG, UIC_CTRL_BG, UIC_BAD_FG,
  UIC_BAD_BG, UIC_GUTTER_BG, UIC_GUTTER_FG, UIC_BOOKMARK, UIC_HEX_OFFSET, UIC_HEX_MODIFIED, UIC_HEX_MIRROR_BG,
  UIC_HEX_BOM, UIC_ISEARCH_FAIL, UIC_COUNT };
#define COLOR_SYSDEFAULT 0xFF000000u
enum { CFG_MRU_MAX = 10, CFG_HIST_MAX = 15 };
struct StyleColor { u32 rgb; u8 bold; u8 pad[3]; };   // rgb = 0x00BBGGRR (COLORREF layout) or COLOR_SYSDEFAULT
struct Config {                       // fields exactly as Appendix C (booleans u8, numbers i32)
  // [General]
  u8  restore_session, backup_enabled, ask_untitled_on_exit, single_instance;
  i32 backup_seconds, mru_count;
  u8  check_disk, reload_silent, pad_g[2];
  i32 poll_seconds;
  // [Editor]
  char font_face[32];
  i32 font_pt, font_px, tab_width;
  u8  use_spaces, use_lang_tabs, auto_indent, line_numbers, wrap_default, show_ws, brace_match, pad_e;
  // [Files]
  EncSpec new_enc;
  u8  new_eol, detect_utf8, nfo_oem, binary_hex;
  i32 ondemand_mb, large_mb;
  u8  keep_bak, pad_f[3];
  // [Hex]
  u8  hex_overwrite, inspector_on, inspector_be, pad_h;
  // [Colors]
  StyleColor style[ST_COUNT];
  u32 ui[UIC_COUNT];
  // [Window]
  i32 win_x, win_y, win_w, win_h, win_max, win_toolbar, win_statusbar, win_results_h, win_find_x, win_find_y;
  // [Find]
  u8  find_case, find_word, find_wrap, find_mode, find_dotall, find_dir_up, find_fif_sub, pad_fi;
  // [Recent], [FindHistory], [ReplaceHistory], [FifFolders], [FifMasks]
  char mru[CFG_MRU_MAX][260];
  char find_hist[CFG_HIST_MAX][256];
  char repl_hist[CFG_HIST_MAX][256];
  char fif_dirs[CFG_HIST_MAX][256];
  char fif_masks[CFG_HIST_MAX][256];
};
void config_defaults(Config* c);
void config_load(Config* c, const Ini& ini);          // starts from defaults; bad values ignored
void config_store(const Config* c, Ini* ini);         // keeps unknown keys already in ini
void config_mru_add(Config* c, const char* path);     // move to front, dedupe (case-insensitive)
void config_hist_add(char (*list)[256], u32 max, const char* s);

#endif
