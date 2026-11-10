// t_config.cpp - suite "config": Appendix C defaults, load/store round trip, clamping, colours, MRU/history.
// Owner: T06.
#include "tf.h"
#include "config.h"
#include <stddef.h>
#include <string.h>

#define RGB3(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16))

static int same_config(const Config& a, const Config& b)
{
  u32 i;
  if (memcmp(&a, &b, offsetof(Config, mru)) != 0) return 0;   // everything before the lists
  for (i = 0; i < CFG_MRU_MAX; i++) if (strcmp(a.mru[i], b.mru[i]) != 0) return 0;
  for (i = 0; i < CFG_HIST_MAX; i++) {
    if (strcmp(a.find_hist[i], b.find_hist[i]) != 0 || strcmp(a.repl_hist[i], b.repl_hist[i]) != 0 ||
        strcmp(a.fif_dirs[i], b.fif_dirs[i]) != 0 || strcmp(a.fif_masks[i], b.fif_masks[i]) != 0) return 0;
  }
  return 1;
}

static void load_text(const char* text, Config* c)
{
  Ini ini;
  ini.parse(text, es_len(text));
  config_load(c, ini);
}

static void test_defaults(TF* tf)
{
  Config c;
  config_defaults(&c);
  TF_EQ_U(c.restore_session, 1); TF_EQ_U(c.backup_enabled, 1); TF_EQ_I(c.backup_seconds, 5);
  TF_EQ_U(c.ask_untitled_on_exit, 0); TF_EQ_U(c.single_instance, 1); TF_EQ_I(c.mru_count, 10);
  TF_EQ_U(c.check_disk, 1); TF_EQ_U(c.reload_silent, 1); TF_EQ_I(c.poll_seconds, 3);
  TF_EQ_S(c.font_face, "Courier New"); TF_EQ_I(c.font_pt, 10); TF_EQ_I(c.font_px, 0);
  TF_EQ_I(c.tab_width, 4); TF_EQ_U(c.use_spaces, 0); TF_EQ_U(c.use_lang_tabs, 1);
  TF_EQ_U(c.auto_indent, 1); TF_EQ_U(c.line_numbers, 1); TF_EQ_U(c.wrap_default, 0); TF_EQ_U(c.show_ws, 0);
  TF_EQ_U(c.brace_match, 1);
  TF_CHECK(enc_equal(c.new_enc, enc_ansi())); TF_EQ_U(c.new_eol, EOL_CRLF);
  TF_EQ_U(c.detect_utf8, 1); TF_EQ_U(c.nfo_oem, 1); TF_EQ_U(c.binary_hex, 1);
  TF_EQ_I(c.ondemand_mb, 16); TF_EQ_I(c.large_mb, 8); TF_EQ_U(c.keep_bak, 0);
  TF_EQ_U(c.hex_overwrite, 1); TF_EQ_U(c.inspector_on, 0); TF_EQ_U(c.inspector_be, 0);
  TF_EQ_U(c.style[ST_DEFAULT].rgb, COLOR_SYSDEFAULT);
  TF_EQ_U(c.style[ST_COMMENT].rgb, RGB3(0, 128, 0));
  TF_EQ_U(c.style[ST_KEYWORD1].rgb, RGB3(0, 0, 255)); TF_EQ_U(c.style[ST_KEYWORD1].bold, 1);
  TF_EQ_U(c.style[ST_LABEL].bold, 1); TF_EQ_U(c.style[ST_HEADING].bold, 1); TF_EQ_U(c.style[ST_SECTION].bold, 1);
  TF_EQ_U(c.style[ST_DIFF_HUNK].bold, 1); TF_EQ_U(c.style[ST_DIFF_HEADER].bold, 1);
  TF_EQ_U(c.style[ST_STRING].bold, 0);
  TF_EQ_U(c.style[ST_INVALID].rgb, RGB3(192, 0, 0));
  TF_EQ_U(c.style[ST_CODE].rgb, RGB3(0, 112, 112));
  TF_EQ_U(c.ui[UIC_MARK_BG], RGB3(255, 240, 120));
  TF_EQ_U(c.ui[UIC_GUTTER_BG], COLOR_SYSDEFAULT);
  TF_EQ_U(c.ui[UIC_HEX_MODIFIED], RGB3(255, 0, 0));
  TF_EQ_U(c.ui[UIC_ISEARCH_FAIL], RGB3(255, 200, 200));
  TF_EQ_I(c.win_x, 40); TF_EQ_I(c.win_y, 40); TF_EQ_I(c.win_w, 760); TF_EQ_I(c.win_h, 540);
  TF_EQ_I(c.win_max, 0); TF_EQ_I(c.win_toolbar, 1); TF_EQ_I(c.win_statusbar, 1); TF_EQ_I(c.win_results_h, 150);
  TF_EQ_I(c.win_find_x, -1); TF_EQ_I(c.win_find_y, -1);
  TF_EQ_U(c.find_case, 0); TF_EQ_U(c.find_word, 0); TF_EQ_U(c.find_wrap, 1); TF_EQ_U(c.find_mode, 0);
  TF_EQ_U(c.find_dotall, 0); TF_EQ_U(c.find_dir_up, 0); TF_EQ_U(c.find_fif_sub, 1);
  TF_EQ_S(c.mru[0], ""); TF_EQ_S(c.find_hist[0], "");
}

static void test_load_store(TF* tf)
{
  Config d, c, r;
  Ini ini, ini2;
  EStr text;
  config_defaults(&d);
  // empty and garbage files give the defaults
  load_text("", &c);
  TF_CHECK(same_config(c, d));
  load_text("\x01\x02garbage\r\n[General\r\n==\r\n[Editor]\r\nfont_face=\r\n", &c);
  TF_CHECK(same_config(c, d));
  // defaults round trip
  config_store(&d, &ini);
  config_load(&r, ini);
  TF_CHECK(same_config(r, d));
  TF_EQ_S(ini.get("General", "backup_seconds"), "5");
  TF_EQ_S(ini.get("Editor", "font_face"), "Courier New");
  TF_EQ_S(ini.get("Files", "new_enc"), "ansi");
  TF_EQ_S(ini.get("Files", "new_eol"), "crlf");
  TF_EQ_S(ini.get("Colors", "s_comment"), "008000,0");
  TF_EQ_S(ini.get("Colors", "s_keyword1"), "0000FF,1");
  TF_EQ_S(ini.get("Colors", "s_default"), "sys,0");
  TF_EQ_S(ini.get("Colors", "u_gutter_bg"), "sys");
  TF_EQ_S(ini.get("Colors", "u_mark_bg"), "FFF078");
  TF_EQ_S(ini.get("Window", "find_x"), "-1");
  TF_EQ_S(ini.get("Find", "fif_sub"), "1");
  // every field changed
  config_defaults(&c);
  c.restore_session = 0; c.backup_enabled = 0; c.backup_seconds = 300; c.ask_untitled_on_exit = 1;
  c.single_instance = 0; c.mru_count = 0; c.check_disk = 0; c.reload_silent = 0; c.poll_seconds = 60;
  es_copy(c.font_face, sizeof(c.font_face), "Fixedsys"); c.font_pt = 12; c.font_px = 15; c.tab_width = 8;
  c.use_spaces = 1; c.use_lang_tabs = 0; c.auto_indent = 0; c.line_numbers = 0; c.wrap_default = 1; c.show_ws = 1;
  c.brace_match = 0; c.new_enc = enc_utf8(1); c.new_eol = EOL_LF; c.detect_utf8 = 0; c.nfo_oem = 0;
  c.binary_hex = 0; c.ondemand_mb = 64; c.large_mb = 1; c.keep_bak = 1; c.hex_overwrite = 0; c.inspector_on = 1;
  c.inspector_be = 1; c.style[ST_COMMENT].rgb = RGB3(1, 2, 3); c.style[ST_COMMENT].bold = 1;
  c.style[ST_DEFAULT].rgb = RGB3(255, 255, 255); c.ui[UIC_GUTTER_BG] = RGB3(10, 20, 30);
  c.ui[UIC_BOOKMARK] = COLOR_SYSDEFAULT;
  c.win_x = -5; c.win_y = 1000; c.win_w = 1024; c.win_h = 700; c.win_max = 1; c.win_toolbar = 0;
  c.win_statusbar = 0; c.win_results_h = 300; c.win_find_x = 100; c.win_find_y = 200;
  c.find_case = 1; c.find_word = 1; c.find_wrap = 0; c.find_mode = 3; c.find_dotall = 1; c.find_dir_up = 1;
  c.find_fif_sub = 0;
  config_mru_add(&c, "C:\\a.txt");
  config_mru_add(&c, "M:\\dev\\editor\\Program Files\\b c.cpp");
  config_hist_add(c.find_hist, CFG_HIST_MAX, "  leading and trailing  ");
  config_hist_add(c.find_hist, CFG_HIST_MAX, "\"quoted\"");
  config_hist_add(c.repl_hist, CFG_HIST_MAX, "$1;x");
  config_hist_add(c.fif_dirs, CFG_HIST_MAX, "M:\\dev");
  config_hist_add(c.fif_masks, CFG_HIST_MAX, "*.cpp;*.h !*.obj");
  config_store(&c, &ini);
  TF_EQ_I(ini.write(&text), E_OK);
  TF_EQ_I(ini2.parse(text.c_str(), text.len()), E_OK);
  config_load(&r, ini2);
  TF_CHECK(same_config(r, c));
  {
    const char* quoted = "\"quoted\"";             // not inside a macro: VC6 cannot stringize \" (C2017)
    TF_EQ_S(r.find_hist[0], quoted);
  }
  TF_EQ_S(r.find_hist[1], "  leading and trailing  ");
  TF_EQ_S(r.mru[0], "M:\\dev\\editor\\Program Files\\b c.cpp");
  TF_EQ_S(ini2.get("Colors", "s_comment"), "010203,1");
  TF_EQ_S(ini2.get("Files", "new_enc"), "utf8bom");
  TF_EQ_S(ini2.get("Recent", "f2"), "C:\\a.txt");
  // shrinking lists removes stale keys
  memset(c.mru, 0, sizeof(c.mru));
  config_mru_add(&c, "only.txt");
  config_store(&c, &ini2);
  TF_EQ_S(ini2.get("Recent", "f1"), "only.txt");
  TF_CHECK(ini2.get("Recent", "f2") == 0);
  config_load(&r, ini2);
  TF_EQ_S(r.mru[0], "only.txt"); TF_EQ_S(r.mru[1], "");
}

static void test_clamp(TF* tf)
{
  Config c, d;
  config_defaults(&d);
  load_text("[General]\r\nbackup_seconds=1\r\nmru_count=99\r\npoll_seconds=-4\r\nrestore_session=maybe\r\n"
            "[Editor]\r\ntab_width=0\r\nfont_pt=1000\r\nfont_face=  Lucida Console  \r\nshow_ws=yes\r\n"
            "[Files]\r\nondemand_mb=1\r\nlarge_mb=99999999999\r\nnew_enc=bogus\r\nnew_eol=LF\r\n"
            "[Window]\r\nw=10\r\nx=-99999\r\nmax=true\r\n[Find]\r\nmode=9\r\ncase=2\r\n", &c);
  TF_EQ_I(c.backup_seconds, 2); TF_EQ_I(c.mru_count, 10); TF_EQ_I(c.poll_seconds, 1);
  TF_EQ_U(c.restore_session, 1);                  // bad boolean keeps the default
  TF_EQ_I(c.tab_width, 1); TF_EQ_I(c.font_pt, 72); TF_EQ_S(c.font_face, "Lucida Console"); TF_EQ_U(c.show_ws, 1);
  TF_EQ_I(c.ondemand_mb, 4); TF_EQ_I(c.large_mb, 64);
  TF_CHECK(enc_equal(c.new_enc, d.new_enc)); TF_EQ_U(c.new_eol, EOL_LF);
  TF_EQ_I(c.win_w, 200); TF_EQ_I(c.win_x, -32000); TF_EQ_I(c.win_max, 1);
  TF_EQ_U(c.find_mode, 3); TF_EQ_U(c.find_case, 0);  // "2" is not a boolean: default
  load_text("[Editor]\r\nfont_face=ThisFontNameIsMuchTooLongForTheField32\r\n", &c);
  TF_EQ_S(c.font_face, "Courier New");
  load_text("[Colors]\r\ns_comment=#FF0000,1\r\ns_string=sys\r\ns_number=12345\r\ns_char=GGGGGG,0\r\n"
            "s_tag=00FF00,7\r\nu_ws_fg=0000ff\r\nu_bad_fg=sys,1\r\nu_bad_bg=nonsense\r\n", &c);
  TF_EQ_U(c.style[ST_COMMENT].rgb, RGB3(255, 0, 0)); TF_EQ_U(c.style[ST_COMMENT].bold, 1);
  TF_EQ_U(c.style[ST_STRING].rgb, COLOR_SYSDEFAULT); TF_EQ_U(c.style[ST_STRING].bold, 0);
  TF_EQ_U(c.style[ST_NUMBER].rgb, d.style[ST_NUMBER].rgb);
  TF_EQ_U(c.style[ST_CHAR].rgb, d.style[ST_CHAR].rgb);
  TF_EQ_U(c.style[ST_TAG].rgb, d.style[ST_TAG].rgb);
  TF_EQ_U(c.ui[UIC_WS_FG], RGB3(0, 0, 255));
  TF_EQ_U(c.ui[UIC_BAD_FG], COLOR_SYSDEFAULT);
  TF_EQ_U(c.ui[UIC_BAD_BG], d.ui[UIC_BAD_BG]);
}

static void test_unknown_kept(TF* tf)
{
  Ini ini;
  Config c;
  EStr out;
  const char* text = "; my comment\r\n[General]\r\nfuture_option=42\r\n[Plugins]\r\nx=1\r\n";
  TF_EQ_I(ini.parse(text, es_len(text)), E_OK);
  config_defaults(&c);
  config_store(&c, &ini);
  TF_EQ_S(ini.get("General", "future_option"), "42");
  TF_EQ_S(ini.get("Plugins", "x"), "1");
  ini.write(&out);
  {
    const char* head = "; my comment\r\n[General]\r\nfuture_option=42\r\nrestore_session=1\r\n";
    TF_CHECK(strncmp(out.c_str(), head, es_len(head)) == 0);
  }
}

static void test_lists(TF* tf)
{
  Config c;
  char name[32];
  u32 i;
  config_defaults(&c);
  config_mru_add(&c, "a.txt");
  config_mru_add(&c, "b.txt");
  config_mru_add(&c, "c.txt");
  TF_EQ_S(c.mru[0], "c.txt"); TF_EQ_S(c.mru[1], "b.txt"); TF_EQ_S(c.mru[2], "a.txt"); TF_EQ_S(c.mru[3], "");
  config_mru_add(&c, "A.TXT");                   // case-insensitive dedupe, moves to the front
  TF_EQ_S(c.mru[0], "A.TXT"); TF_EQ_S(c.mru[1], "c.txt"); TF_EQ_S(c.mru[2], "b.txt"); TF_EQ_S(c.mru[3], "");
  config_mru_add(&c, "");
  config_mru_add(&c, 0);
  TF_EQ_S(c.mru[0], "A.TXT");
  for (i = 0; i < 20; i++) { es_fmt(name, sizeof(name), "f%u.txt", i); config_mru_add(&c, name); }
  TF_EQ_S(c.mru[0], "f19.txt"); TF_EQ_S(c.mru[9], "f10.txt");
  config_mru_add(&c, "f15.txt");
  TF_EQ_S(c.mru[0], "f15.txt"); TF_EQ_S(c.mru[1], "f19.txt"); TF_EQ_S(c.mru[5], "f14.txt");
  TF_EQ_S(c.mru[9], "f10.txt");
  {
    char longp[400];
    memset(longp, 'x', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = 0;
    config_mru_add(&c, longp);
    TF_EQ_U(es_len(c.mru[0]), 259);
  }
  config_hist_add(c.find_hist, CFG_HIST_MAX, "foo");
  config_hist_add(c.find_hist, CFG_HIST_MAX, "Foo");  // history is case-sensitive
  config_hist_add(c.find_hist, CFG_HIST_MAX, "foo");
  TF_EQ_S(c.find_hist[0], "foo"); TF_EQ_S(c.find_hist[1], "Foo"); TF_EQ_S(c.find_hist[2], "");
  for (i = 0; i < 30; i++) { es_fmt(name, sizeof(name), "h%u", i); config_hist_add(c.find_hist, CFG_HIST_MAX, name); }
  TF_EQ_S(c.find_hist[0], "h29"); TF_EQ_S(c.find_hist[14], "h15");
  config_hist_add(c.repl_hist, 3, "1"); config_hist_add(c.repl_hist, 3, "2");
  config_hist_add(c.repl_hist, 3, "3"); config_hist_add(c.repl_hist, 3, "4");
  TF_EQ_S(c.repl_hist[0], "4"); TF_EQ_S(c.repl_hist[2], "2"); TF_EQ_S(c.repl_hist[3], "");
  config_hist_add(0, 3, "x");
  config_hist_add(c.repl_hist, 0, "x");
  TF_EQ_S(c.repl_hist[0], "4");
}

void suite_config(TF* tf)
{
  test_defaults(tf);
  test_load_store(tf);
  test_clamp(tf);
  test_unknown_kept(tf);
  test_lists(tf);
}
