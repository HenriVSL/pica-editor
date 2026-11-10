// t_ini.cpp - suite "ini": tolerant parse, order/comments kept, CRLF write, getters, set/remove, garbage, OOM.
// Owner: T06.
#include "tf.h"
#include "ini.h"
#include <string.h>

static int parse_s(Ini* ini, const char* s) { return ini->parse(s, es_len(s)); }

static void wr(const Ini& ini, EStr* out)
{
  out->clear();
  ini.write(out);
}

static const char k_sample[] =
  "; Pica settings\r\n"
  "global=1\r\n"
  "\r\n"
  "[General]\r\n"
  "restore_session = 1\r\n"
  "  backup_seconds=5   \r\n"
  "# hash comment\r\n"
  "this line has no equals sign\r\n"
  "=no key\r\n"
  "[Editor]   trailing junk\r\n"
  "font_face=Courier New\r\n"
  "path=\"  spaced  \"\r\n"
  "masks=*.cpp;*.h ; not a comment\r\n"
  "empty=\r\n"
  "dup=first\r\n"
  "dup=second\r\n"
  "[]\r\n"
  "[unterminated\r\n"
  "\r\n"
  "[general]\r\n"
  "late_key=late\r\n";

static void test_roundtrip(TF* tf)
{
  Ini ini;
  EStr out;
  TF_EQ_I(parse_s(&ini, k_sample), E_OK);
  wr(ini, &out);
  TF_EQ_S(out.c_str(), k_sample);
  // LF and CR input become CRLF; a missing final newline is added
  {
    Ini b;
    TF_EQ_I(parse_s(&b, "a=1\n[S]\nb=2\rc=3"), E_OK);
    wr(b, &out);
    TF_EQ_S(out.c_str(), "a=1\r\n[S]\r\nb=2\r\nc=3\r\n");
    TF_EQ_S(b.get("S", "c"), "3");
    TF_EQ_S(b.get("", "a"), "1");
  }
  // empty input
  {
    Ini e;
    TF_EQ_I(e.parse("", 0), E_OK);
    wr(e, &out);
    TF_EQ_S(out.c_str(), "");
    TF_EQ_I(e.parse(0, 5), E_OK);
    TF_EQ_U(e.section_count(), 0);
  }
  // UTF-8 BOM is dropped
  {
    Ini b;
    TF_EQ_I(parse_s(&b, "\xEF\xBB\xBF[A]\r\nk=v\r\n"), E_OK);
    TF_EQ_S(b.get("A", "k"), "v");
    wr(b, &out);
    TF_EQ_S(out.c_str(), "[A]\r\nk=v\r\n");
  }
  // blank lines at the end are kept
  {
    Ini b;
    TF_EQ_I(parse_s(&b, "[A]\r\n\r\n\r\n"), E_OK);
    wr(b, &out);
    TF_EQ_S(out.c_str(), "[A]\r\n\r\n\r\n");
  }
}

static void test_get(TF* tf)
{
  Ini ini;
  TF_EQ_I(parse_s(&ini, k_sample), E_OK);
  TF_EQ_S(ini.get("General", "restore_session"), "1");
  TF_EQ_S(ini.get("GENERAL", "Backup_Seconds"), "5");
  TF_EQ_S(ini.get("general", "late_key"), "late");      // second occurrence of the section
  TF_EQ_S(ini.get("Editor", "font_face"), "Courier New");
  TF_EQ_S(ini.get("Editor", "path"), "  spaced  ");     // quotes removed, blanks inside kept
  TF_EQ_S(ini.get("Editor", "masks"), "*.cpp;*.h ; not a comment");
  TF_EQ_S(ini.get("Editor", "empty"), "");
  TF_EQ_S(ini.get("Editor", "dup"), "first");
  TF_EQ_S(ini.get("", "global"), "1");
  TF_CHECK(ini.get("General", "global") == 0);
  TF_CHECK(ini.get("General", "missing") == 0);
  TF_CHECK(ini.get("Nope", "font_face") == 0);
  TF_CHECK(ini.get("General", "this line has no equals sign") == 0);
  TF_CHECK(ini.get("General", "") == 0);
  TF_CHECK(ini.get(0, 0) == 0);
  TF_EQ_S(ini.get_s("General", "missing", "dflt"), "dflt");
  TF_EQ_S(ini.get_s("Editor", "empty", "dflt"), "");
  TF_EQ_U(ini.section_count(), 3);                      // General, Editor, general ("[]" and "[unterminated" are not headers)
  TF_EQ_S(ini.section(0), "General");
  TF_EQ_S(ini.section(1), "Editor");
  TF_EQ_S(ini.section(2), "general");
  TF_CHECK(ini.section(3) == 0);
  TF_EQ_U(ini.key_count("General"), 3);                 // restore_session, backup_seconds, late_key
  TF_EQ_U(ini.key_count("Editor"), 6);
  TF_EQ_U(ini.key_count(""), 1);
  TF_EQ_U(ini.key_count("Nope"), 0);
  {
    const char* v = 0;
    TF_EQ_S(ini.key_at("General", 2, &v), "late_key");
    TF_EQ_S(v, "late");
    TF_EQ_S(ini.key_at("Editor", 0, &v), "font_face");
    TF_CHECK(ini.key_at("Editor", 6, &v) == 0);
    TF_CHECK(v == 0);
  }
}

static void test_typed(TF* tf)
{
  Ini ini;
  TF_EQ_I(parse_s(&ini,
    "[N]\r\na=42\r\nb=-7\r\nc=abc\r\nd=99999999999\r\ne=-99999999999\r\nf= 12 \r\ng=0x1F\r\nh=1Fh\r\ni=31\r\n"
    "y1=yes\r\ny2=TRUE\r\ny3=on\r\ny4=1\r\nn1=no\r\nn2=False\r\nn3=off\r\nn4=0\r\nbad=maybe\r\nbig=0x100000000\r\n"), E_OK);
  TF_EQ_I(ini.get_i("N", "a", 0, -100, 100), 42);
  TF_EQ_I(ini.get_i("N", "a", 0, 0, 10), 10);           // clamped high
  TF_EQ_I(ini.get_i("N", "b", 0, 0, 10), 0);            // clamped low
  TF_EQ_I(ini.get_i("N", "b", 0, -10, 10), -7);
  TF_EQ_I(ini.get_i("N", "c", 5, 0, 10), 5);            // bad -> default
  TF_EQ_I(ini.get_i("N", "d", 5, 0, 10), 10);           // too large -> clamped
  TF_EQ_I(ini.get_i("N", "e", 5, 0, 10), 0);
  TF_EQ_I(ini.get_i("N", "f", 0, 0, 100), 12);
  TF_EQ_I(ini.get_i("N", "zz", 3, 0, 10), 3);
  TF_EQ_I(ini.get_i("N", "g", 3, 0, 100), 3);           // get_i is decimal only
  TF_EQ_U(ini.get_x("N", "g", 0), 0x1F);
  TF_EQ_U(ini.get_x("N", "h", 0), 0x1F);
  TF_EQ_U(ini.get_x("N", "i", 0), 31);
  TF_EQ_U(ini.get_x("N", "c", 77), 77);
  TF_EQ_U(ini.get_x("N", "big", 77), 77);
  TF_EQ_I(ini.get_b("N", "y1", 0), 1); TF_EQ_I(ini.get_b("N", "y2", 0), 1);
  TF_EQ_I(ini.get_b("N", "y3", 0), 1); TF_EQ_I(ini.get_b("N", "y4", 0), 1);
  TF_EQ_I(ini.get_b("N", "n1", 1), 0); TF_EQ_I(ini.get_b("N", "n2", 1), 0);
  TF_EQ_I(ini.get_b("N", "n3", 1), 0); TF_EQ_I(ini.get_b("N", "n4", 1), 0);
  TF_EQ_I(ini.get_b("N", "bad", 1), 1); TF_EQ_I(ini.get_b("N", "bad", 0), 0);
  TF_EQ_I(ini.get_b("N", "missing", 1), 1);
  TF_EQ_I(ini.set_i("N", "a", -123), E_OK);
  TF_EQ_S(ini.get("N", "a"), "-123");
  TF_EQ_I(ini.set_x("N", "x", 0xBEEF), E_OK);
  TF_EQ_S(ini.get("N", "x"), "0xBEEF");
  TF_EQ_U(ini.get_x("N", "x", 0), 0xBEEF);
}

static void test_set(TF* tf)
{
  Ini ini;
  EStr out;
  TF_EQ_I(parse_s(&ini, "; top\r\n[A]\r\nk1 = old  \r\n; inside A\r\n\r\n[B]\r\nx=1\r\n[a]\r\nlate=1\r\n; tail\r\n"), E_OK);
  TF_EQ_I(ini.set("a", "K1", "new"), E_OK);           // existing key: replaced in place, key spelling kept
  TF_EQ_I(ini.set("A", "k2", "two"), E_OK);           // new key: after the last key of the LAST [A] occurrence
  TF_EQ_I(ini.set("C", "c", "see"), E_OK);            // new section at the end, after a blank line
  TF_EQ_I(ini.set("B", "y", "2"), E_OK);
  wr(ini, &out);
  TF_EQ_S(out.c_str(),
    "; top\r\n[A]\r\nk1=new\r\n; inside A\r\n\r\n[B]\r\nx=1\r\ny=2\r\n[a]\r\nlate=1\r\nk2=two\r\n; tail\r\n"
    "\r\n[C]\r\nc=see\r\n");
  // quoting keeps every string exact
  {
    static const char* const vals[] = { "  lead", "trail  ", " both ", "\"quoted\"", "\"", "\"\"", "a\"b", "", "x",
                                        "C:\\Program Files\\x;y", "\ttab" };
    u32 i;
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
      Ini r;
      TF_EQ_I(ini.set("Q", "v", vals[i]), E_OK);
      TF_EQ_S(ini.get("Q", "v"), vals[i]);
      wr(ini, &out);
      TF_EQ_I(r.parse(out.c_str(), out.len()), E_OK);
      TF_EQ_S(r.get("Q", "v"), vals[i]);
    }
  }
  // CR/LF in values become spaces (one line per key)
  TF_EQ_I(ini.set("Q", "nl", "a\r\nb\nc"), E_OK);
  TF_EQ_S(ini.get("Q", "nl"), "a  b c");
  TF_EQ_I(ini.set("Q", "nullv", 0), E_OK);
  TF_EQ_S(ini.get("Q", "nullv"), "");
  // invalid names
  TF_EQ_I(ini.set("Q", "", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q", 0, "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q", "a=b", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q", " lead", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q", "[x", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q", ";c", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q", "a\nb", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q]", "k", "v"), E_SYNTAX);
  TF_EQ_I(ini.set("Q\r", "k", "v"), E_SYNTAX);
  // global keys go before the first section
  {
    Ini g;
    TF_EQ_I(parse_s(&g, "; c\r\n[S]\r\nk=v\r\n"), E_OK);
    TF_EQ_I(g.set("", "top", "1"), E_OK);
    TF_EQ_I(g.set(0, "top2", "2"), E_OK);
    wr(g, &out);
    TF_EQ_S(out.c_str(), "top=1\r\ntop2=2\r\n; c\r\n[S]\r\nk=v\r\n");
    TF_EQ_S(g.get(0, "top"), "1");
  }
  // set on an empty Ini
  {
    Ini e;
    TF_EQ_I(e.set("S", "k", "v"), E_OK);
    wr(e, &out);
    TF_EQ_S(out.c_str(), "[S]\r\nk=v\r\n");
  }
}

static void test_remove(TF* tf)
{
  Ini ini;
  EStr out;
  TF_EQ_I(parse_s(&ini, "g=0\r\n[A]\r\nk=1\r\n; c\r\nk=2\r\nj=3\r\n[B]\r\nx=1\r\n[a]\r\nk=4\r\n[C]\r\n"), E_OK);
  TF_EQ_I(ini.remove("A", "k"), E_OK);                // all occurrences in all [A] sections
  TF_CHECK(ini.get("A", "k") == 0);
  TF_EQ_I(ini.remove("A", "k"), E_NOTFOUND);
  TF_EQ_I(ini.remove("Nope", "k"), E_NOTFOUND);
  wr(ini, &out);
  TF_EQ_S(out.c_str(), "g=0\r\n[A]\r\n; c\r\nj=3\r\n[B]\r\nx=1\r\n[a]\r\n[C]\r\n");
  TF_EQ_I(ini.remove_section("a"), E_OK);             // both occurrences with their lines
  wr(ini, &out);
  TF_EQ_S(out.c_str(), "g=0\r\n[B]\r\nx=1\r\n[C]\r\n");
  TF_EQ_I(ini.remove_section("A"), E_NOTFOUND);
  TF_EQ_I(ini.remove_section(""), E_OK);              // the global part
  wr(ini, &out);
  TF_EQ_S(out.c_str(), "[B]\r\nx=1\r\n[C]\r\n");
  TF_EQ_I(ini.remove_section(""), E_NOTFOUND);
  TF_EQ_I(ini.remove_section("C"), E_OK);
  TF_EQ_I(ini.remove_section("B"), E_OK);
  wr(ini, &out);
  TF_EQ_S(out.c_str(), "");
  TF_EQ_U(ini.section_count(), 0);
}

static void test_garbage(TF* tf)
{
  u32 round;
  const char alphabet[] = "[]=;# \t\r\nabcAB\"01\xEF\xBB\xBF\x00\xFF";
  for (round = 0; round < 400; round++) {
    char buf[600];
    u32 n = tf_rand_below(tf, sizeof(buf)), i;
    Ini a, b;
    EStr o1, o2;
    for (i = 0; i < n; i++) {
      if (tf_rand_below(tf, 4) == 0) buf[i] = (char)tf_rand_below(tf, 256);
      else buf[i] = alphabet[tf_rand_below(tf, sizeof(alphabet) - 1)];
    }
    TF_EQ_I(a.parse(buf, n), E_OK);
    TF_EQ_I(a.write(&o1), E_OK);
    TF_EQ_I(b.parse(o1.c_str(), o1.len()), E_OK);
    TF_EQ_I(b.write(&o2), E_OK);
    TF_EQ_S(o1.c_str(), o2.c_str());                 // write is a fixed point after one round
    TF_EQ_U(a.section_count(), b.section_count());
    for (i = 0; i < a.section_count(); i++) {
      const char* s = a.section(i);
      TF_EQ_U(a.key_count(s), b.key_count(s));
    }
    // random edits keep the text parseable and the values readable
    for (i = 0; i < 5; i++) {
      char key[8], val[8];
      es_fmt(key, sizeof(key), "k%u", tf_rand_below(tf, 4));
      es_fmt(val, sizeof(val), "v%u", tf_rand_below(tf, 100));
      if (a.set(a.section_count() ? a.section(tf_rand_below(tf, a.section_count())) : "S", key, val) == E_OK) {
        Ini c;
        a.write(&o1);
        TF_EQ_I(c.parse(o1.c_str(), o1.len()), E_OK);
      }
    }
  }
}

static void test_oom(TF* tf)
{
  i32 n;
  int ok = 0;
  EStr before, after;
  for (n = 1; n < 200 && !ok; n++) {             // parse
    Ini ini;
    int rc;
    e_test_fail_after(n);
    rc = parse_s(&ini, k_sample);
    e_test_fail_after(-1);
    if (rc == E_OK) {
      ok = 1;
      TF_EQ_S(ini.get("Editor", "font_face"), "Courier New");
      TF_EQ_S(ini.get("general", "late_key"), "late");
    } else {
      TF_EQ_I(rc, E_NOMEM);
      TF_EQ_U(ini.section_count(), 0);
      TF_CHECK(ini.get("Editor", "font_face") == 0);
    }
  }
  TF_CHECK(ok);
  // set: new section, new key, existing key; a failure leaves the text unchanged
  {
    static const char* const secs[3] = { "New", "Editor", "Editor" };
    static const char* const keys[3] = { "key", "newkey", "font_face" };
    u32 k;
    for (k = 0; k < 3; k++) {
      ok = 0;
      for (n = 1; n < 100 && !ok; n++) {
        Ini ini;
        int rc;
        TF_EQ_I(parse_s(&ini, k_sample), E_OK);
        ini.write(&before);
        e_test_fail_after(n);
        rc = ini.set(secs[k], keys[k], "  value  ");
        e_test_fail_after(-1);
        if (rc == E_OK) {
          ok = 1;
          TF_EQ_S(ini.get(secs[k], keys[k]), "  value  ");
        } else {
          TF_EQ_I(rc, E_NOMEM);
          ini.write(&after);
          TF_EQ_S(after.c_str(), before.c_str());
          TF_EQ_S(ini.get("general", "late_key"), "late");
        }
      }
      TF_CHECK(ok);
    }
  }
}

void suite_ini(TF* tf)
{
  test_roundtrip(tf);
  test_get(tf);
  test_typed(tf);
  test_set(tf);
  test_remove(tf);
  test_garbage(tf);
  test_oom(tf);
}
