// t_estr.cpp - suite "estr": string helpers (estr.h) and error names (etypes.h). Owner after Wave 0: overseer.
#include "tf.h"
#include <string.h>

static void test_copy_cat(TF* tf)
{
  char b[8];
  es_copy(b, sizeof(b), "hello");
  TF_EQ_S(b, "hello");
  es_copy(b, sizeof(b), "0123456789");
  TF_EQ_S(b, "0123456");
  es_copy(b, 1, "abc");
  TF_EQ_S(b, "");
  es_copy(b, sizeof(b), 0);
  TF_EQ_S(b, "");
  es_copy(b, sizeof(b), "ab");
  es_cat(b, sizeof(b), "cd");
  TF_EQ_S(b, "abcd");
  es_cat(b, sizeof(b), "efghij");
  TF_EQ_S(b, "abcdefg");
  TF_EQ_U(es_len("abc"), 3);
  TF_EQ_U(es_len(""), 0);
  TF_EQ_U(es_len(0), 0);
}

static void test_cmp(TF* tf)
{
  TF_CHECK(es_icmp("Hello", "hELLO") == 0);
  TF_CHECK(es_icmp("abc", "abd") < 0);
  TF_CHECK(es_icmp("abd", "ABC") > 0);
  TF_CHECK(es_icmp("ab", "abc") < 0);
  TF_CHECK(es_icmp("", "") == 0);
  TF_CHECK(es_nicmp("README.TXT", "readme.md", 7) == 0);
  TF_CHECK(es_nicmp("README.TXT", "readme.md", 8) != 0);
  TF_CHECK(es_nicmp("abc", "abc", 100) == 0);
  TF_CHECK(es_icmp("\xC4", "\xE4") != 0);           // ASCII only: Latin-1 letters are not folded
}

static void test_fmt(TF* tf)
{
  char b[64];
  int n;
  n = es_fmt(b, sizeof(b), "%d|%u|%x|%X|%s|%c|%%", -42, 42u, 255u, 255u, "str", 'Z');
  TF_EQ_S(b, "-42|42|ff|FF|str|Z|%");
  TF_EQ_U(n, es_len(b));
  es_fmt(b, sizeof(b), "[%5d][%-5d][%05d][%05d]", 42, 42, 42, -42);
  TF_EQ_S(b, "[   42][42   ][00042][-0042]");
  es_fmt(b, sizeof(b), "[%8s][%-8s][%.3s]", "abc", "abc", "abcdef");
  TF_EQ_S(b, "[     abc][abc     ][abc]");
  es_fmt(b, sizeof(b), "%08X", 0x1F40u);
  TF_EQ_S(b, "00001F40");
  es_fmt(b, sizeof(b), "%d %d", (int)0x7FFFFFFF, (int)(-2147483647 - 1));
  TF_EQ_S(b, "2147483647 -2147483648");
  es_fmt(b, sizeof(b), "%u", 0xFFFFFFFFu);
  TF_EQ_S(b, "4294967295");
  es_fmt(b, sizeof(b), "%ld %lu", (long)-7, (unsigned long)7);
  TF_EQ_S(b, "-7 7");
  es_fmt(b, sizeof(b), "%s", (const char*)0);
  TF_EQ_S(b, "(null)");
  es_fmt(b, sizeof(b), "%*d|%-*d|", 4, 7, 3, 7);
  TF_EQ_S(b, "   7|7  |");
  // truncation: always terminated, returns the length written
  n = es_fmt(b, 6, "%s", "abcdefgh");
  TF_EQ_S(b, "abcde");
  TF_EQ_U(n, 5);
  n = es_fmt(b, 1, "%d", 12345);
  TF_EQ_S(b, "");
  TF_EQ_U(n, 0);
  es_fmt(b, sizeof(b), "100%");
  TF_EQ_S(b, "100");
  es_fmt(b, sizeof(b), "%q");
  TF_EQ_S(b, "%q");
}

static void test_u64(TF* tf)
{
  char b[24];
  u64 big;
  TF_EQ_U(es_u64_to_dec(0, b), 1);
  TF_EQ_S(b, "0");
  big = 0xFFFFFFFFu;
  big = (big << 32) | 0xFFFFFFFFu;
  TF_EQ_U(es_u64_to_dec(big, b), 20);
  TF_EQ_S(b, "18446744073709551615");
  big = 1;
  big <<= 63;
  es_u64_to_dec(big, b);
  TF_EQ_S(b, "9223372036854775808");
  es_u64_to_dec(4294967296u + (u64)0, b);
  TF_EQ_S(b, "4294967296");
}

static void test_trim_kv(TF* tf)
{
  char b[64];
  char* k;
  char* v;
  es_copy(b, sizeof(b), "  \t hi there \r\n");
  TF_EQ_S(es_trim(b), "hi there");
  TF_EQ_S(b, "hi there");
  es_copy(b, sizeof(b), "   ");
  TF_EQ_S(es_trim(b), "");
  es_copy(b, sizeof(b), " key = some value ");
  TF_CHECK(es_kv(b, &k, &v) == 1);
  TF_EQ_S(k, "key");
  TF_EQ_S(v, "some value");
  es_copy(b, sizeof(b), "a=b=c");
  TF_CHECK(es_kv(b, &k, &v) == 1);
  TF_EQ_S(k, "a");
  TF_EQ_S(v, "b=c");
  es_copy(b, sizeof(b), "empty=");
  TF_CHECK(es_kv(b, &k, &v) == 1);
  TF_EQ_S(v, "");
  es_copy(b, sizeof(b), "; comment=1");
  TF_CHECK(es_kv(b, &k, &v) == 0);
  es_copy(b, sizeof(b), "[Section]");
  TF_CHECK(es_kv(b, &k, &v) == 0);
  es_copy(b, sizeof(b), "no equals sign");
  TF_CHECK(es_kv(b, &k, &v) == 0);
  es_copy(b, sizeof(b), " = value");
  TF_CHECK(es_kv(b, &k, &v) == 0);
}

static void test_parse(TF* tf)
{
  u32 u = 0;
  i32 i = 0;
  TF_EQ_I(es_parse_u32("123", &u, 0), E_OK);  TF_EQ_U(u, 123);
  TF_EQ_I(es_parse_u32("0x1F", &u, 0), E_OK); TF_EQ_U(u, 31);
  TF_EQ_I(es_parse_u32("1Fh", &u, 0), E_OK);  TF_EQ_U(u, 31);
  TF_EQ_I(es_parse_u32("ffH", &u, 0), E_OK);  TF_EQ_U(u, 255);
  TF_EQ_I(es_parse_u32(" 42 ", &u, 10), E_OK); TF_EQ_U(u, 42);
  TF_EQ_I(es_parse_u32("1F", &u, 16), E_OK);  TF_EQ_U(u, 31);
  TF_EQ_I(es_parse_u32("0x10", &u, 16), E_OK); TF_EQ_U(u, 16);
  TF_EQ_I(es_parse_u32("4294967295", &u, 0), E_OK); TF_EQ_U(u, 0xFFFFFFFFu);
  TF_EQ_I(es_parse_u32("4294967296", &u, 0), E_RANGE);
  TF_EQ_I(es_parse_u32("0x100000000", &u, 0), E_RANGE);
  TF_EQ_I(es_parse_u32("", &u, 0), E_SYNTAX);
  TF_EQ_I(es_parse_u32("   ", &u, 0), E_SYNTAX);
  TF_EQ_I(es_parse_u32("12a", &u, 10), E_SYNTAX);
  TF_EQ_I(es_parse_u32("1F", &u, 0), E_SYNTAX);     // hex needs 0x or h in base 0
  TF_EQ_I(es_parse_u32("0x", &u, 0), E_SYNTAX);
  TF_EQ_I(es_parse_u32("h", &u, 0), E_SYNTAX);
  TF_EQ_I(es_parse_u32("-1", &u, 0), E_SYNTAX);
  TF_EQ_I(es_parse_u32(0, &u, 0), E_SYNTAX);
  TF_EQ_I(es_parse_i32("-42", &i), E_OK);  TF_EQ_I(i, -42);
  TF_EQ_I(es_parse_i32("+7", &i), E_OK);   TF_EQ_I(i, 7);
  TF_EQ_I(es_parse_i32("2147483647", &i), E_OK);  TF_EQ_I(i, 2147483647);
  TF_EQ_I(es_parse_i32("-2147483648", &i), E_OK); TF_EQ_I(i, -2147483647 - 1);
  TF_EQ_I(es_parse_i32("2147483648", &i), E_RANGE);
  TF_EQ_I(es_parse_i32("-2147483649", &i), E_RANGE);
  TF_EQ_I(es_parse_i32("99999999999999", &i), E_RANGE);
  TF_EQ_I(es_parse_i32("-", &i), E_SYNTAX);
  TF_EQ_I(es_parse_i32("1 2", &i), E_SYNTAX);
}

static void test_wildcard(TF* tf)
{
  TF_CHECK(es_wildcard("*.*", "Makefile"));
  TF_CHECK(es_wildcard("*", "a.b.c"));
  TF_CHECK(es_wildcard("*.cpp", "MAIN.CPP"));
  TF_CHECK(!es_wildcard("*.cpp", "main.c"));
  TF_CHECK(!es_wildcard("*.c", "main.cpp"));
  TF_CHECK(es_wildcard("t_?.cpp", "t_a.cpp"));
  TF_CHECK(!es_wildcard("t_?.cpp", "t_ab.cpp"));
  TF_CHECK(es_wildcard("a*b*c", "aXXbYYc"));
  TF_CHECK(!es_wildcard("a*b*c", "aXXbYY"));
  TF_CHECK(es_wildcard("*e*", "readme"));
  TF_CHECK(es_wildcard("readme.*", "README"));
  TF_CHECK(es_wildcard("readme.*", "readme.txt"));
  TF_CHECK(!es_wildcard("readme.*", "readmex"));
  TF_CHECK(!es_wildcard("", "x"));
  TF_CHECK(es_wildcard("", ""));
}

static void test_utf8(TF* tf)
{
  char b[8];
  Vec<ecp> v;
  TF_EQ_U(es_utf8_encode('A', b), 1);
  TF_EQ_U(es_utf8_encode(0xE4, b), 2);
  TF_EQ_MEM(b, "\xC3\xA4", 2);
  TF_EQ_U(es_utf8_encode(0x20AC, b), 3);
  TF_EQ_MEM(b, "\xE2\x82\xAC", 3);
  TF_EQ_U(es_utf8_encode(0x1F600, b), 4);
  TF_EQ_MEM(b, "\xF0\x9F\x98\x80", 4);
  TF_EQ_U(es_utf8_encode(ECP_BADBYTE(0xFF), b), 1);   // bad byte: the raw byte
  TF_EQ_U((u8)b[0], 0xFF);
  TF_EQ_U(es_utf8_encode(0xD800, b), 3);              // surrogate: U+FFFD
  TF_EQ_MEM(b, "\xEF\xBF\xBD", 3);
  TF_EQ_I(es_utf8_to_ecp("a\xC3\xA4\xE2\x82\xAC", 6, &v), E_OK);
  TF_EQ_U(v.size(), 3);
  if (v.size() == 3) { TF_EQ_U(v[0], 'a'); TF_EQ_U(v[1], 0xE4); TF_EQ_U(v[2], 0x20AC); }
  v.clear();
  // overlong, surrogate, truncated, lone continuation: one bad marker per byte
  TF_EQ_I(es_utf8_to_ecp("\xC0\xAF" "\xED\xA0\x80" "\x80" "\xE2\x82", 8, &v), E_OK);
  TF_EQ_U(v.size(), 8);
  if (v.size() == 8) {
    TF_EQ_U(v[0], ECP_BADBYTE(0xC0)); TF_EQ_U(v[1], ECP_BADBYTE(0xAF));
    TF_EQ_U(v[2], ECP_BADBYTE(0xED)); TF_EQ_U(v[3], ECP_BADBYTE(0xA0)); TF_EQ_U(v[4], ECP_BADBYTE(0x80));
    TF_EQ_U(v[5], ECP_BADBYTE(0x80)); TF_EQ_U(v[6], ECP_BADBYTE(0xE2)); TF_EQ_U(v[7], ECP_BADBYTE(0x82));
  }
  v.clear();
  TF_EQ_I(es_utf8_to_ecp("\xF4\x90\x80\x80", 4, &v), E_OK);   // > U+10FFFF
  TF_EQ_U(v.size(), 4);
  v.release();
}

static void test_estr(TF* tf)
{
  EStr s;
  char* d;
  u32 i;
  TF_EQ_S(s.c_str(), "");
  TF_EQ_U(s.len(), 0);
  TF_EQ_I(s.append("abc"), E_OK);
  TF_EQ_I(s.appendn("defgh", 2), E_OK);
  TF_EQ_I(s.appendf("-%d-%s", 12, "x"), E_OK);
  TF_EQ_S(s.c_str(), "abcde-12-x");
  TF_EQ_U(s.len(), 10);
  for (i = 0; i < 1000; i++) s.append("0123456789");
  TF_EQ_U(s.len(), 10010);
  TF_CHECK(memcmp(s.c_str() + 10000, "0123456789", 10) == 0);
  s.clear();
  TF_EQ_S(s.c_str(), "");
  s.append("keep");
  d = s.detach();
  TF_EQ_S(d, "keep");
  e_free(d);
  TF_EQ_S(s.c_str(), "");
  d = s.detach();
  TF_EQ_S(d, "");
  e_free(d);
  // OOM: append fails cleanly
  s.append("x");
  e_test_fail_after(1);
  for (i = 0; i < 20; i++) s.append("0123456789012345678901234567890123456789");
  e_test_fail_after(-1);
  TF_CHECK(s.c_str()[0] == 'x');
}

static void test_errname(TF* tf)
{
  TF_EQ_S(e_errname(E_OK), "E_OK");
  TF_EQ_S(e_errname(E_NOMEM), "E_NOMEM");
  TF_EQ_S(e_errname(E_UNSUPPORTED), "E_UNSUPPORTED");
  TF_EQ_S(e_errname(-999), "E_UNKNOWN");
  TF_EQ_U(emin<u32>(3, 5), 3);
  TF_EQ_U(emax<u32>(3, 5), 5);
}

void suite_estr(TF* tf)
{
  test_copy_cat(tf);
  test_cmp(tf);
  test_fmt(tf);
  test_u64(tf);
  test_trim_kv(tf);
  test_parse(tf);
  test_wildcard(tf);
  test_utf8(tf);
  test_estr(tf);
  test_errname(tf);
}
