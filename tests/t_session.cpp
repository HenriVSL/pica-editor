// t_session.cpp - suite "session": round trip of a 3-tab session (all fields, bookmarks), validation of truncated
// and damaged texts, tolerance of hand edits. Owner: T06.
#include "tf.h"
#include "session.h"
#include <string.h>

static SessionTab* new_tab()
{
  SessionTab* t = new SessionTab;
  memset(t->path, 0, sizeof(t->path));
  memset(t->lang, 0, sizeof(t->lang));
  t->id = 0; t->untitled = 0; t->view = 0; t->hex_pane = 0; t->hex_insert = 0; t->untitled_no = 0;
  t->enc = enc_ansi(); t->enc_forced = 0; t->eol = 0; t->lang_forced = 0; t->wrap = 0;
  t->tab_width = 4; t->use_spaces = 0; t->read_only = 0; t->modified = 0; t->has_backup = 0; t->disk_missing = 0;
  t->pad[0] = t->pad[1] = 0;
  t->caret = t->anchor = 0; t->top_line = t->top_subrow = 0; t->hex_top = 0;
  t->disk_size = t->disk_mtime_lo = t->disk_mtime_hi = 0;
  return t;
}

static void make_session(Session* s)
{
  SessionTab* t;
  session_init(s);
  s->seq = 42; s->active = 1; s->next_tabid = 17; s->next_untitled = 3; s->clean_exit = 0;
  t = new_tab();
  t->id = 5; es_copy(t->path, sizeof(t->path), "M:\\dev\\editor\\src\\core\\ptree.cpp");
  t->enc = enc_utf8(0); t->eol = EOL_CRLF; es_copy(t->lang, sizeof(t->lang), "cpp");
  t->caret = 1234; t->anchor = 1200; t->top_line = 100; t->top_subrow = 2; t->tab_width = 4;
  t->modified = 1; t->has_backup = 1; t->disk_size = 12345; t->disk_mtime_hi = 0x01D9F1A2u; t->disk_mtime_lo = 0xB3C4D5E6u;
  t->bookmarks.push(10); t->bookmarks.push(20); t->bookmarks.push(30);
  s->tabs.push(t);
  t = new_tab();
  t->id = 9; t->untitled = 1; t->untitled_no = 2; t->view = 1; t->hex_pane = 1; t->hex_insert = 1;
  t->enc = enc_utf16le(1); t->enc_forced = 1; t->eol = EOL_LF; t->lang_forced = 1; t->wrap = 1;
  es_copy(t->lang, sizeof(t->lang), "python"); t->tab_width = 8; t->use_spaces = 1; t->read_only = 0;
  t->caret = 0xFFFFFFFEu; t->anchor = 0; t->hex_top = 0x1000;
  s->tabs.push(t);
  t = new_tab();
  t->id = 16; es_copy(t->path, sizeof(t->path), "C:\\My Documents\\ spaced name ;semi.txt");
  t->enc = enc_cp(1251); t->eol = EOL_CR; t->read_only = 1; t->disk_missing = 1; t->top_line = 7;
  t->disk_size = 0xFFFFFFFFu; t->disk_mtime_hi = 0xFFFFFFFFu; t->disk_mtime_lo = 1;
  {
    u32 i;
    for (i = 0; i < 500; i++) t->bookmarks.push(i * 3);
  }
  s->tabs.push(t);
}

static int tabs_equal(const SessionTab& a, const SessionTab& b)
{
  u32 i;
  if (a.id != b.id || strcmp(a.path, b.path) != 0 || a.untitled != b.untitled || a.view != b.view ||
      a.hex_pane != b.hex_pane || a.hex_insert != b.hex_insert || a.untitled_no != b.untitled_no ||
      !enc_equal(a.enc, b.enc) || a.enc_forced != b.enc_forced || a.eol != b.eol || a.lang_forced != b.lang_forced ||
      a.wrap != b.wrap || strcmp(a.lang, b.lang) != 0 || a.tab_width != b.tab_width || a.use_spaces != b.use_spaces ||
      a.read_only != b.read_only || a.modified != b.modified || a.has_backup != b.has_backup ||
      a.disk_missing != b.disk_missing || a.caret != b.caret || a.anchor != b.anchor || a.top_line != b.top_line ||
      a.top_subrow != b.top_subrow || a.hex_top != b.hex_top || a.disk_size != b.disk_size ||
      a.disk_mtime_lo != b.disk_mtime_lo || a.disk_mtime_hi != b.disk_mtime_hi ||
      a.bookmarks.size() != b.bookmarks.size()) return 0;
  for (i = 0; i < a.bookmarks.size(); i++) if (a.bookmarks[i] != b.bookmarks[i]) return 0;
  return 1;
}

static void to_text(const Session& s, EStr* out)
{
  Ini ini;
  session_to_ini(s, &ini);
  out->clear();
  ini.write(out);
}

static int from_text(const char* text, u32 n, Session* s)
{
  Ini ini;
  int rc = ini.parse(text, n);
  if (rc != E_OK) return rc;
  return session_from_ini(ini, s);
}

static void test_roundtrip(TF* tf)
{
  Session s, r;
  EStr text;
  Ini ini;
  u32 i, seq = 0;
  make_session(&s);
  TF_EQ_I(session_to_ini(s, &ini), E_OK);
  TF_EQ_S(ini.get("Session", "version"), "1");
  TF_EQ_S(ini.get("Session", "count"), "3");
  TF_EQ_S(ini.get("Tab0", "bookmarks"), "10,20,30");
  TF_EQ_S(ini.get("Tab0", "disk_mtime"), "01D9F1A2B3C4D5E6");
  TF_EQ_S(ini.get("Tab0", "enc"), "utf8");
  TF_EQ_S(ini.get("Tab1", "view"), "hex");
  TF_EQ_S(ini.get("Tab1", "eol"), "lf");
  TF_EQ_S(ini.get("Tab2", "eol"), "cr");
  TF_EQ_S(ini.get("Tab2", "enc"), "cp1251");
  TF_EQ_S(ini.get("end", "ok"), "1");
  TF_EQ_S(ini.section(ini.section_count() - 1), "end");
  ini.write(&text);
  session_init(&r);
  TF_EQ_I(from_text(text.c_str(), text.len(), &r), E_OK);
  TF_EQ_U(r.seq, 42); TF_EQ_U(r.active, 1); TF_EQ_U(r.next_tabid, 17); TF_EQ_U(r.next_untitled, 3);
  TF_EQ_U(r.clean_exit, 0);
  TF_EQ_U(r.tabs.size(), 3);
  for (i = 0; i < 3 && i < r.tabs.size(); i++) {
    if (!tabs_equal(*s.tabs[i], *r.tabs[i])) tf_fail(tf, __FILE__, __LINE__, "tab %u differs after round trip", i);
    else tf_check(tf, 1, __FILE__, __LINE__, "tab");
  }
  TF_EQ_I(session_validate((const u8*)text.c_str(), text.len(), &seq), E_OK);
  TF_EQ_U(seq, 42);
  // writing again into the same Ini replaces the old session (other sections kept)
  ini.set("Other", "keep", "1");
  session_free(&s);
  session_init(&s);
  s.seq = 43; s.clean_exit = 1;
  TF_EQ_I(session_to_ini(s, &ini), E_OK);
  TF_CHECK(ini.get("Tab0", "id") == 0);
  TF_EQ_S(ini.get("Other", "keep"), "1");
  TF_EQ_S(ini.get("Session", "seq"), "43");
  ini.write(&text);
  TF_EQ_I(from_text(text.c_str(), text.len(), &r), E_OK);   // r had 3 tabs: freed by session_from_ini
  TF_EQ_U(r.tabs.size(), 0); TF_EQ_U(r.seq, 43); TF_EQ_U(r.clean_exit, 1);
  session_free(&r);
  session_free(&s);
}

static void test_validate(TF* tf)
{
  Session s;
  EStr text;
  u32 n, seq, fails = 0, endpos;
  make_session(&s);
  to_text(s, &text);
  session_free(&s);
  // every truncation before the sentinel fails; the full text passes
  endpos = (u32)(strstr(text.c_str(), "[end]") - text.c_str());
  for (n = 0; n < text.len(); n += (n < endpos - 64 ? 97 : 1)) {
    int rc = session_validate((const u8*)text.c_str(), n, &seq);
    if (n < endpos + 11 && rc == E_OK) fails++;      // "[end]\r\nok=1" not complete yet
    if (n >= endpos + 11 && rc != E_OK) fails++;
  }
  TF_EQ_U(fails, 0);
  TF_EQ_I(session_validate((const u8*)text.c_str(), text.len() - 2, &seq), E_OK);   // only the last CRLF missing
  TF_EQ_I(session_validate((const u8*)text.c_str(), endpos, &seq), E_FORMAT);
  TF_EQ_U(seq, 0);
  TF_EQ_I(session_validate(0, 0, &seq), E_FORMAT);
  TF_EQ_I(session_validate((const u8*)"garbage\x00\xFF", 9, &seq), E_FORMAT);
  {
    static const char* const bad[] = {
      "[Session]\r\nversion=1\r\ncount=0\r\n",                                  // no sentinel
      "[Session]\r\nversion=1\r\ncount=0\r\n[end]\r\nok=0\r\n",                 // sentinel not ok
      "[Session]\r\nversion=2\r\ncount=0\r\n[end]\r\nok=1\r\n",                 // unknown version
      "[Session]\r\ncount=0\r\n[end]\r\nok=1\r\n",                              // no version
      "[Session]\r\nversion=1\r\n[end]\r\nok=1\r\n",                            // no count
      "[Session]\r\nversion=1\r\ncount=1\r\n[end]\r\nok=1\r\n",                 // Tab0 missing
      "[Session]\r\nversion=1\r\ncount=1\r\n[Tab0]\r\npath=x\r\n[end]\r\nok=1\r\n",          // no id
      "[Session]\r\nversion=1\r\ncount=1\r\n[Tab0]\r\nid=0\r\npath=x\r\n[end]\r\nok=1\r\n",  // id 0
      "[Session]\r\nversion=1\r\ncount=1\r\n[Tab0]\r\nid=3\r\n[end]\r\nok=1\r\n",            // named tab, no path
      "[Session]\r\nversion=1\r\ncount=2\r\n[Tab0]\r\nid=3\r\npath=a\r\n[Tab1]\r\nid=3\r\npath=b\r\n[end]\r\nok=1\r\n",
      "[Session]\r\nversion=1\r\ncount=99999\r\n[end]\r\nok=1\r\n",             // absurd count
    };
    u32 i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      int rc = session_validate((const u8*)bad[i], es_len(bad[i]), &seq);
      if (rc != E_FORMAT) tf_fail(tf, __FILE__, __LINE__, "bad session %u: rc %d", i, rc);
      else tf_check(tf, 1, __FILE__, __LINE__, "bad");
    }
  }
}

static void test_tolerant(TF* tf)
{
  Session s;
  const char* text =
    "; hand edited\r\n[Session]\r\nversion=1\r\nseq=7\r\nactive=5\r\nnext_tabid=2\r\nnext_untitled=0\r\ncount=2\r\n"
    "[Tab1]\r\nid=8\r\nuntitled=1\r\nuntitled_no=6\r\n"
    "[Tab0]\r\nid=4\r\npath=\"C:\\x y.txt\"\r\nview=weird\r\nenc=nonsense\r\neol=??\r\ntab_width=99\r\n"
    "caret=abc\r\nanchor=17\r\nbookmarks=30, 10,x,20,10,,99999999999,5\r\ndisk_mtime=zz\r\nmodified=yes\r\n"
    "unknown_key=1\r\n"
    "[end]\r\nok=1\r\n";
  session_init(&s);
  TF_EQ_I(from_text(text, es_len(text), &s), E_OK);
  TF_EQ_U(s.tabs.size(), 2);
  TF_EQ_U(s.seq, 7);
  TF_EQ_U(s.active, 0);                            // out of range -> first tab
  TF_EQ_U(s.next_tabid, 9);                        // bumped past the highest id
  TF_EQ_U(s.next_untitled, 7);                     // bumped past untitled_no 6
  if (s.tabs.size() == 2) {
    const SessionTab& t = *s.tabs[0];
    TF_EQ_U(t.id, 4);
    TF_EQ_S(t.path, "C:\\x y.txt");
    TF_EQ_U(t.view, 0);
    TF_CHECK(enc_equal(t.enc, enc_ansi()));
    TF_EQ_U(t.eol, EOL_CRLF);
    TF_EQ_U(t.tab_width, 16);
    TF_EQ_U(t.caret, 0);
    TF_EQ_U(t.anchor, 17);
    TF_EQ_U(t.bookmarks.size(), 4);
    if (t.bookmarks.size() == 4) {
      TF_EQ_U(t.bookmarks[0], 5); TF_EQ_U(t.bookmarks[1], 10); TF_EQ_U(t.bookmarks[2], 20); TF_EQ_U(t.bookmarks[3], 30);
    }
    TF_EQ_U(t.disk_mtime_hi, 0); TF_EQ_U(t.disk_mtime_lo, 0);
    TF_EQ_U(t.modified, 1);
    TF_EQ_U(s.tabs[1]->id, 8); TF_EQ_U(s.tabs[1]->untitled, 1); TF_EQ_S(s.tabs[1]->path, "");
  }
  session_free(&s);
}

static void test_oom(TF* tf)
{
  Session s;
  EStr text;
  i32 n;
  int ok = 0;
  make_session(&s);
  to_text(s, &text);
  session_free(&s);
  for (n = 1; n < 400 && !ok; n++) {
    Ini ini;
    int rc;
    TF_EQ_I(ini.parse(text.c_str(), text.len()), E_OK);
    session_init(&s);
    e_test_fail_after(n);
    rc = session_from_ini(ini, &s);
    e_test_fail_after(-1);
    if (rc == E_OK) { ok = 1; TF_EQ_U(s.tabs.size(), 3); }
    else { TF_EQ_I(rc, E_NOMEM); TF_EQ_U(s.tabs.size(), 0); }
    session_free(&s);
  }
  TF_CHECK(ok);
  ok = 0;
  make_session(&s);
  for (n = 1; n < 400 && !ok; n++) {
    Ini ini;
    int rc;
    e_test_fail_after(n);
    rc = session_to_ini(s, &ini);
    e_test_fail_after(-1);
    if (rc == E_OK) ok = 1;
    else TF_EQ_I(rc, E_NOMEM);
  }
  TF_CHECK(ok);
  session_free(&s);
}

void suite_session(TF* tf)
{
  test_roundtrip(tf);
  test_validate(tf);
  test_tolerant(tf);
  test_oom(tf);
}
