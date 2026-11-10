// t_regex.cpp - suite "regex" (T03): parser/compiler errors, match table (Pike VM and forced backtracker), context
// (lo/from/hi/ctx), Pike vs backtracker differential on random patterns, pathological patterns, OOM.
#include "tf.h"
#include "regex.h"
#include "regex_int.h"
#include "codec.h"
#include "ucd.h"
#include <string.h>
#include <time.h>

enum { I = RXF_ICASE, S = RXF_DOTALL, W = RXF_WORD };

struct RxtCase { const char* pat; u32 flags; const char* text; const char* expect; };
// expect: "-" no match, "s,e" byte span, optionally "|s,e|-..." for groups 1..n (then all groups are compared)

static const RxtCase k_cases[] = {
  // literals and escapes
  { "abc", 0, "xxabcxx", "2,5" }, { "abc", 0, "ababc", "2,5" }, { "abc", 0, "ab", "-" }, { "abc", 0, "", "-" },
  { "a.c", 0, "abc", "0,3" }, { "a.c", 0, "a\nc", "-" }, { "a.c", S, "a\nc", "0,3" }, { "(?s)a.c", 0, "a\nc", "0,3" },
  { "a.c", 0, "a\rc", "-" }, { "a.c", S, "a\rc", "0,3" }, { "\\t", 0, "a\tb", "1,2" }, { "\\n", 0, "a\nb", "1,2" },
  { "\\r\\n", 0, "a\r\nb", "1,3" }, { "\\x41", 0, "zA", "1,2" }, { "\\x{41}", 0, "zA", "1,2" },
  { "\\u00e4", 0, "\xc3\xa4", "0,2" }, { "\\x{20AC}", 0, "a\xe2\x82\xac", "1,4" },
  { "\\x{1F600}", 0, "\xf0\x9f\x98\x80", "0,4" }, { "\\uD83D\\uDE00", 0, "x\xf0\x9f\x98\x80", "1,5" },
  { "\\.", 0, "a.b", "1,2" }, { "\\\\", 0, "a\\b", "1,2" }, { "\\[", 0, "a[b", "1,2" }, { "\\*", 0, "a*b", "1,2" },
  { "\\e", 0, "x\x1b", "1,2" }, { "\\f\\v", 0, "\f\v", "0,2" }, { "\\cA", 0, "\x01", "0,1" }, { "\\a", 0, "\a", "0,1" },
  { "\\Qa.b*\\E", 0, "xa.b*", "1,5" }, { "\\Qa.b", 0, "a.b", "0,3" }, { "\\Qab\\E+", 0, "abbb", "0,4" },
  { "\\Q(a|b)\\E", 0, "(a|b)", "0,5" }, { "a\\Eb", 0, "ab", "0,2" }, { "}", 0, "a}", "1,2" }, { "]", 0, "a]", "1,2" },
  { "a{", 0, "a{", "0,2" }, { "a{x}", 0, "a{x}", "0,4" }, { "a{1", 0, "a{1", "0,3" }, { "a{,2}", 0, "a{,2}", "0,5" },
  { "\xc3\xa4", 0, "a\xc3\xa4", "1,3" }, { "\\\xc3\xa4", 0, "\xc3\xa4", "0,2" }, { "\\-", 0, "a-", "1,2" },
  { "\\<", 0, "a", "0,0" }, { "-", 0, "a-b", "1,2" }, { "a b", 0, "xa b", "1,4" }, { ",", 0, ",", "0,1" },
  // classes
  { "[abc]", 0, "xxbx", "2,3" }, { "[^abc]", 0, "abcd", "3,4" }, { "[a-z]+", 0, "12abc3", "2,5" },
  { "[^a-z]+", 0, "ab12c", "2,4" }, { "[]a]", 0, "x]", "1,2" }, { "[^]a]", 0, "]ab", "2,3" }, { "[a-]", 0, "x-", "1,2" },
  { "[-a]", 0, "x-", "1,2" }, { "[\\w-]+", 0, "!ab-c!", "1,5" }, { "[\\d.]+", 0, "v1.25x", "1,5" },
  { "[a\\-z]", 0, "b-", "1,2" }, { "[\\]]", 0, "]", "0,1" }, { "[\\\\]", 0, "\\", "0,1" }, { "[\\n]", 0, "a\nb", "1,2" },
  { "[\\b]", 0, "\b", "0,1" }, { "[\\x41-\\x43]+", 0, "xABCD", "1,4" }, { "[[:alpha:]]+", 0, "12ab3", "2,4" },
  { "[[:digit:]]+", 0, "ab123c", "2,5" }, { "[[:space:]]+", 0, "a \t b", "1,4" }, { "[[:upper:]]+", 0, "abCDe", "2,4" },
  { "[[:lower:]]+", 0, "ABcdE", "2,4" }, { "[[:alnum:]]+", 0, "!a1!", "1,3" }, { "[[:punct:]]+", 0, "ab!?.c", "2,5" },
  { "[[:xdigit:]]+", 0, "xyz0fAg", "3,6" }, { "[[:blank:]]+", 0, "a \tb", "1,3" }, { "[[:cntrl:]]", 0, "a\x01", "1,2" },
  { "[[:word:]]+", 0, "!a_1!", "1,4" }, { "[^[:digit:]]+", 0, "12ab3", "2,4" }, { "[[:print:]]+", 0, "\x01" "ab", "1,3" },
  { "[[:graph:]]+", 0, " ab ", "1,3" }, { "[[:digit:][:upper:]]+", 0, "ab1C2d", "2,5" }, { "[a[]", 0, "[", "0,1" },
  { "[[]", 0, "x[", "1,2" }, { "[a-c[:digit:]]+", 0, "x1b2y", "1,4" }, { "[\\d-z]+", 0, "x-1z", "1,4" },
  { "\\d+", 0, "ab123", "2,5" }, { "\\D+", 0, "12ab3", "2,4" }, { "\\w+", 0, "!!ab_1!", "2,6" },
  { "\\W+", 0, "ab!?c", "2,4" }, { "\\s+", 0, "a \t\nb", "1,4" }, { "\\S+", 0, "  ab ", "2,4" },
  { "\\w+", 0, "\xc3\xa4\xc3\xb6x", "0,5" }, { "[\xc3\xa4-\xc3\xb6]+", 0, "a\xc3\xa4\xc3\xb6", "1,5" },
  { "[^a]", 0, "a\xc3\xa4", "1,3" }, { "[a-c]", I, "B", "0,1" }, { "[A-C]", I, "b", "0,1" }, { "[^a-c]", I, "B", "-" },
  { "[^a-c]", I, "Bd", "1,2" }, { "[[:upper:]]", I, "a", "0,1" }, { "[\xc3\x84]", I, "\xc3\xa4", "0,2" },
  { "\\d", 0, "\xd9\xa3", "-" }, { ".", 0, "\xc3\xa4", "0,2" }, { "[\\s\\d]+", 0, "a1 2b", "1,4" },
  { "[^\\s]+", 0, "  ab ", "2,4" }, { "[\\W\\d]+", 0, "ab!1c", "2,4" }, { "[.]", 0, "a.", "1,2" },
  { "[*+?{}()|^$]+", 0, "a*+?{}()|^$b", "1,11" }, { "[\\x{20AC}]", 0, "a\xe2\x82\xac", "1,4" },
  // anchors
  { "^abc", 0, "abc", "0,3" }, { "^abc", 0, "xabc", "-" }, { "^abc", 0, "x\nabc", "2,5" },
  { "^abc", 0, "x\r\nabc", "3,6" }, { "^abc", 0, "x\rabc", "2,5" }, { "abc$", 0, "abc", "0,3" },
  { "abc$", 0, "abc\n", "0,3" }, { "abc$", 0, "abc\r\n", "0,3" }, { "abc$", 0, "abc\rx", "0,3" },
  { "abc$", 0, "abcx", "-" }, { "^$", 0, "a\n\nb", "2,2" }, { "^$", 0, "a\r\n\r\nb", "3,3" }, { "$", 0, "ab\r\n", "2,2" },
  { "^", 0, "ab", "0,0" }, { "x^", 0, "x", "-" }, { "\\r$", 0, "a\r\n", "-" }, { "^\\n", 0, "a\r\nb", "-" },
  { "\\Aabc", 0, "abc", "0,3" }, { "\\Aabc", 0, "x\nabc", "-" }, { "abc\\z", 0, "abc\n", "-" },
  { "abc\\z", 0, "xabc", "1,4" }, { "a$\\n^b", 0, "a\nb", "0,3" }, { "$^", 0, "a\n\nb", "2,2" },
  { "a$\\r\\n^b", 0, "a\r\nb", "0,4" }, { "^.*$", 0, "ab\r\ncd", "0,2" }, { "^b.*$", 0, "ab\r\nbc\r\n", "4,6" },
  { "\\A", 0, "ab", "0,0" }, { "\\z", 0, "ab", "2,2" }, { "$", 0, "ab", "2,2" }, { "^$", 0, "", "0,0" },
  { "^ab|cd$", 0, "xab cd", "4,6" }, { "(^a|b)+", 0, "ab", "0,2|1,2" },
  // word boundaries
  { "\\bfoo\\b", 0, "a foo b", "2,5" }, { "\\bfoo\\b", 0, "afoo b", "-" }, { "\\bfoo", 0, "foo", "0,3" },
  { "foo\\b", 0, "foobar foo", "7,10" }, { "\\Boo\\B", 0, "foo xoox", "5,7" }, { "\\<foo", 0, "xfoo foo", "5,8" },
  { "foo\\>", 0, "foox foo", "5,8" }, { "\\b", 0, "  ab", "2,2" }, { "\\B", 0, "ab", "1,1" },
  { "\\bk\\b", 0, "\xc3\xa4k \xc3\xa4 k", "7,8" }, { "\\b\xc3\xa4", 0, "x\xc3\xa4 \xc3\xa4", "4,6" },
  { "\\b_\\b", 0, "a _ b", "2,3" }, { "\\B", 0, "a b", "-" }, { "\\b1", 0, "a1 1", "3,4" }, { "\\>", 0, "ab c", "2,2" },
  { "\\<", 0, " ab", "1,1" }, { "\\b.", 0, "  a", "2,3" },
  // quantifiers
  { "a*", 0, "aaa", "0,3" }, { "a*", 0, "baaa", "0,0" }, { "a+", 0, "baaa", "1,4" }, { "a?", 0, "a", "0,1" },
  { "ba?", 0, "baa", "0,2" }, { "a{3}", 0, "aaaa", "0,3" }, { "a{2,}", 0, "a aaaa", "2,6" }, { "a{2,3}", 0, "aaaa", "0,3" },
  { "a{0,1}b", 0, "ab", "0,2" }, { "a{0}b", 0, "ab", "1,2" }, { "a{1}b", 0, "ab", "0,2" }, { "a{3}", 0, "aa", "-" },
  { "a*?", 0, "aaa", "0,0" }, { "a+?", 0, "aaa", "0,1" }, { "a??b", 0, "ab", "0,2" }, { "a{2,3}?", 0, "aaaa", "0,2" },
  { "a{2,}?", 0, "aaaa", "0,2" }, { "<.+?>", 0, "<a><b>", "0,3" }, { "<.+>", 0, "<a><b>", "0,6" },
  { "a.*b", 0, "axxbxxb", "0,7" }, { "a.*?b", 0, "axxbxxb", "0,4" }, { "(ab)+", 0, "ababab", "0,6|4,6" },
  { "(ab)+?", 0, "ababab", "0,2|0,2" }, { "(a|ab)(c|bcd)(d*)", 0, "abcd", "0,4|0,1|1,4|4,4" },
  { "(a*)+", 0, "b", "0,0|0,0" }, { "(a*)*", 0, "b", "0,0|-" }, { "(a|b)*c", 0, "abac", "0,4|2,3" },
  { "(?:a|b)*c", 0, "abac", "0,4" }, { "x*", 0, "", "0,0" }, { "(a){0}b", 0, "ab", "1,2|-" },
  { "(a){2}", 0, "aaa", "0,2|1,2" }, { "(a?){3}", 0, "aa", "0,2|2,2" }, { "a{1000}", 0, "aaa", "-" },
  { "(?:a{2}){2}", 0, "aaaaa", "0,4" }, { "a{2}b{2}", 0, "aabbb", "0,4" }, { "[ab]{3}", 0, "xabab", "1,4" },
  { "a*b*c*", 0, "aabbcc", "0,6" }, { "(a+)(a+)", 0, "aaaa", "0,4|0,3|3,4" }, { "(a+?)(a+)", 0, "aaaa", "0,4|0,1|1,4" },
  { "(a*)(a*)", 0, "aa", "0,2|0,2|2,2" }, { "(a*?)(a*)", 0, "aa", "0,2|0,0|0,2" }, { ".*", 0, "ab\ncd", "0,2" },
  { ".*", S, "ab\ncd", "0,5" }, { "(.)*", 0, "abc", "0,3|2,3" }, { "x{0,2}y", 0, "xxxy", "1,4" },
  { "(?:x?)*y", 0, "xxy", "0,3" }, { "(?:x*)*y", 0, "xxy", "0,3" }, { "(?:x|y?)+z", 0, "xyz", "0,3" },
  { "a{2}?", 0, "aaa", "0,2" },
  // alternation priority
  { "a|ab", 0, "ab", "0,1" }, { "ab|a", 0, "ab", "0,2" }, { "(a|ab)c", 0, "abc", "0,3|0,2" },
  { "cat|category", 0, "category", "0,3" }, { "x(a|b|c)y", 0, "xby", "0,3|1,2" }, { "|a", 0, "a", "0,0" },
  { "a|", 0, "b", "0,0" }, { "a|b|c", 0, "zzc", "2,3" }, { "(a|b)|(c)", 0, "c", "0,1|-|0,1" },
  { "abc|abd", 0, "xabd", "1,4" }, { "(?:ab|a)(?:bc|c)", 0, "abc", "0,3" }, { "ab|cd|ef", 0, "xxef", "2,4" },
  { "a(?:b|c|d)+e", 0, "abcde", "0,5" }, { "(foo|foobar)baz", 0, "foobarbaz", "0,9|0,6" },
  // groups
  { "(a)(b)(c)", 0, "abc", "0,3|0,1|1,2|2,3" }, { "(a(b(c)))", 0, "abc", "0,3|0,3|1,3|2,3" },
  { "(?:a)(b)", 0, "ab", "0,2|1,2" }, { "(a)|(b)", 0, "b", "0,1|-|0,1" }, { "(a)?b", 0, "b", "0,1|-" },
  { "((a)|b)+", 0, "ab", "0,2|1,2|0,1" }, { "()", 0, "a", "0,0|0,0" }, { "(?:)", 0, "a", "0,0" }, { "", 0, "abc", "0,0" },
  { "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)", 0, "abcdefghij", "0,10|0,1|1,2|2,3|3,4|4,5|5,6|6,7|7,8|8,9" },
  { "(x)(?:(y)|z)*", 0, "xyz", "0,3|0,1|1,2" },
  // inline flags
  { "(?i)abc", 0, "xABC", "1,4" }, { "(?i)ABC", 0, "abc", "0,3" }, { "a(?i)b", 0, "aB", "0,2" }, { "a(?i)b", 0, "AB", "-" },
  { "(?i:a)b", 0, "Ab", "0,2" }, { "(?i:a)b", 0, "AB", "-" }, { "(?-i)abc", I, "ABC", "-" }, { "(?-i)abc", I, "abc", "0,3" },
  { "(?is)a.c", 0, "A\nC", "0,3" }, { "(?m)^b", 0, "a\nb", "2,3" }, { "(?#comment)ab", 0, "ab", "0,2" },
  { "(a(?i)b)c", 0, "aBc", "0,3|0,2" }, { "(a(?i)b)c", 0, "aBC", "-" }, { "(?i)a|b", 0, "B", "0,1" },
  { "(?s-i)a.", I, "A\n", "-" }, { "(?i)(?s)a.", 0, "A\n", "0,2" }, { "(?i)[a-c]+", 0, "xAbC", "1,4" },
  // case-insensitive
  { "abc", I, "xAbC", "1,4" }, { "\xc3\xa4", I, "\xc3\x84", "0,2" }, { "\xc3\x84", I, "\xc3\xa4", "0,2" },
  { "HeLLo", I, "say hello", "4,9" }, { "a1b", I, "A1B", "0,3" }, { "[x-z]+", I, "XyZ", "0,3" },
  // \R
  { "a\\Rb", 0, "a\r\nb", "0,4" }, { "a\\Rb", 0, "a\nb", "0,3" }, { "a\\Rb", 0, "a\rb", "0,3" }, { "\\R", 0, "x\r\n", "1,3" },
  { "\\R+", 0, "\n\r\n\r", "0,4" }, { "a\\R", 0, "ab", "-" }, { "\\R", I, "\r", "0,1" },
  // backreferences
  { "(a)\\1", 0, "aa", "0,2|0,1" }, { "(a+)b\\1", 0, "aaabaa", "1,6|1,3" }, { "(\\w+) \\1", 0, "the the", "0,7|0,3" },
  { "(\\w+) \\1\\b", 0, "the theory the the", "11,18|11,14" }, { "(a)|\\1b", 0, "b", "-" }, { "(a)?\\1", 0, "b", "-" },
  { "(a*)\\1", 0, "aaaa", "0,4|0,2" }, { "(?:(a)|b)\\1", 0, "aa", "0,2|0,1" }, { "(.)\\1", 0, "abccd", "2,4|2,3" },
  { "(a)\\1", I, "aA", "0,2|0,1" }, { "(\xc3\xa4)\\1", 0, "x\xc3\xa4\xc3\xa4", "1,5|1,3" }, { "(a)\\1", 0, "ab", "-" },
  { "(a)(b)\\2\\1", 0, "abba", "0,4|0,1|1,2" }, { "(.+)\\1", 0, "xabcabc", "1,7|1,4" }, { "(a*)b\\1", 0, "b", "0,1|0,0" },
  { "\\b(\\w)\\w*\\1\\b", 0, "abc aba", "4,7|4,5" }, { "(.)(?:\\1)+", 0, "abbbc", "1,4|1,2" },
  // lookahead
  { "foo(?=bar)", 0, "foobaz foobar", "7,10" }, { "foo(?!bar)", 0, "foobar foobaz", "7,10" }, { "(?=a)a", 0, "ba", "1,2" },
  { "(?!a).", 0, "ab", "1,2" }, { "a(?=b)", 0, "ab", "0,1" }, { "(?=(a+))a*b\\1", 0, "baaabac", "3,6|3,4" },
  { "(?=.*x)abc", 0, "abc x", "0,3" }, { "(?!.*x)abc", 0, "abc x abc", "6,9" }, { "a(?=b)c", 0, "abc", "-" },
  { "(?=a)", 0, "ba", "1,1" }, { "(?!a)", 0, "aab", "2,2" }, { "\\w+(?=,)", 0, "ab cd, ef", "3,5" },
  { "(?=(\\w+))\\1:", 0, "abc:", "0,4|0,3" }, { "(?!(a))b", 0, "b", "0,1|-" }, { "x(?=y|z)", 0, "xa xz", "3,4" },
  { "(?=a(?=b))ab", 0, "aab", "1,3" }, { "(?=.*\\d)\\w+", 0, "ab c1", "0,2" }, { "^(?!.*bad).*$", 0, "a bad\ngood", "6,10" },
  { "(?=a)*", 0, "a", "E" },
  // required literal (no prefix): the match starts on the literal's line
  { "\\w+ing\\b", 0, "sing\nthe thing", "0,4" }, { "\\w+ing\\b", 0, "the\nthing", "4,9" },
  { "\\d+px", 0, "a 10 b\nw: 20px", "10,14" }, { "[a-z]+ing", I, "x\nRUNNING", "2,9" }, { "x?ab", 0, "zzab", "2,4" },
  { "[^a]?bc", 0, "xbc", "0,3" }, { "\\w+ing", 0, "ing ingx", "-" }, { "\\s+xy", 0, "a\n  xy", "1,6" },
  { ".+ab", 0, "q\r\nzzab", "3,7" }, { "\\b\\w+ab", 0, "x zab ab", "2,5" }, { "(?:a|b)cd", 0, "zcd bcd", "4,7" },
  { "[ab]*cd", I, "xBACD", "1,5" }, { "\\w*\\x{20AC}\\x{20AC}", 0, "a\xe2\x82\xac\xe2\x82\xac", "0,7" },
  { "\\w+(?:ing|ed)", 0, "walked", "0,6" }, { "\\S+ing$", 0, "sing\r\nring", "0,4" },
  // patterns that start at a line start (^ on every path) skip to line starts
  { "^\\s*#", 0, "a #\n  # x", "4,7" }, { "^b|^c", 0, "ab\ncb", "3,4" }, { "^a", 0, "x\ra", "2,3" },
  { "^a", 0, "x\r\na", "3,4" }, { "(?:^|\\A)\\d", 0, "x1\n22", "3,4" }, { "^[a-c]+$", 0, "xa\nabc\n", "3,6" },
  { "^\\w+(?=:)", 0, "a b:\nkey: v", "5,8" }, { "^(\\w)\\1", 0, "xaa\nbb", "4,6|4,5" }, { "^", 0, "", "0,0" },
  // empty and misc
  { "(|a)b", 0, "ab", "0,2|0,1" }, { "(a|)b", 0, "b", "0,1|0,0" }, { "\\d{2,3}", 0, "1 12345", "2,5" },
  { "[0-9]+\\.[0-9]+", 0, "v 1.25 x", "2,6" }, { "^\\s*#", 0, "x\n  # c", "2,5" }, { "\\s+$", 0, "ab  \ncd", "2,4" },
  { "[^,]+", 0, ",,ab,c", "2,4" }, { "\"[^\"]*\"", 0, "say \"hi\" ok", "4,8" }, { "\\w+@\\w+\\.com", 0, "x a@b.com y", "2,9" },
};

struct RxtErrCase { const char* pat; int code; u32 pos; };
static const RxtErrCase k_errs[] = {
  { "(", E_SYNTAX, 0 }, { "(a", E_SYNTAX, 0 }, { "a)", E_SYNTAX, 1 }, { "[a", E_SYNTAX, 0 }, { "[z-a]", E_SYNTAX, 2 },
  { "*a", E_SYNTAX, 0 }, { "a**", E_SYNTAX, 2 }, { "a{2}{3}", E_SYNTAX, 4 }, { "+", E_SYNTAX, 0 }, { "a|*", E_SYNTAX, 2 },
  { "(*)", E_SYNTAX, 1 }, { "^*", E_SYNTAX, 1 }, { "\\b+", E_SYNTAX, 2 }, { "a{3,1}", E_SYNTAX, 1 },
  { "a{1001}", E_TOOBIG, 1 }, { "a{1,1001}", E_TOOBIG, 1 }, { "\\", E_SYNTAX, 0 }, { "ab\\", E_SYNTAX, 2 },
  { "\\q", E_SYNTAX, 0 }, { "\\xZZ", E_SYNTAX, 0 }, { "\\x{}", E_SYNTAX, 0 }, { "\\x{110000}", E_SYNTAX, 0 },
  { "\\uD800", E_SYNTAX, 0 }, { "\\u12", E_SYNTAX, 0 }, { "\\1", E_SYNTAX, 0 }, { "(a)\\2", E_SYNTAX, 3 },
  { "(?<=a)b", E_UNSUPPORTED, 0 }, { "(?<!a)b", E_UNSUPPORTED, 0 }, { "(?>a)", E_UNSUPPORTED, 0 },
  { "a*+", E_UNSUPPORTED, 2 }, { "a++", E_UNSUPPORTED, 2 }, { "a?+", E_UNSUPPORTED, 2 }, { "(?<n>a)", E_UNSUPPORTED, 0 },
  { "(?P<n>a)", E_UNSUPPORTED, 0 }, { "(?'n'a)", E_UNSUPPORTED, 0 }, { "(?R)", E_UNSUPPORTED, 0 },
  { "(?1)", E_UNSUPPORTED, 0 }, { "x(?&n)", E_UNSUPPORTED, 1 }, { "\\p{L}", E_UNSUPPORTED, 0 }, { "a\\P{L}", E_UNSUPPORTED, 1 },
  { "(?x)a", E_UNSUPPORTED, 2 }, { "(?z)", E_SYNTAX, 2 }, { "(?)", E_SYNTAX, 0 }, { "[[:foo:]]", E_SYNTAX, 1 },
  { "[[:^alpha:]]", E_UNSUPPORTED, 1 }, { "\\Z", E_UNSUPPORTED, 0 }, { "\\G", E_UNSUPPORTED, 0 },
  { "\\k<a>", E_UNSUPPORTED, 0 }, { "[\\q]", E_SYNTAX, 1 }, { "[\\1]", E_SYNTAX, 1 }, { "(?=a)*", E_SYNTAX, 5 },
  { "(?#abc", E_SYNTAX, 0 }, { "\\c1", E_SYNTAX, 0 }, { "(?|a)", E_UNSUPPORTED, 0 }, { "\\K", E_UNSUPPORTED, 0 },
  { "\\X", E_UNSUPPORTED, 0 }, { "ab(", E_SYNTAX, 2 }, { "a{2,1}", E_SYNTAX, 1 }, { "(a{1000}){5}", E_TOOBIG, 9 },
  { "[a-\\q]", E_SYNTAX, 3 }, { "x\\g1", E_UNSUPPORTED, 1 }, { "(?i", E_SYNTAX, 0 }, { "a|b)", E_SYNTAX, 3 },
  { "\\x4", E_SYNTAX, 0 }, { "[\\p{L}]", E_UNSUPPORTED, 1 }, { "a{2}+", E_UNSUPPORTED, 4 }, { "a{2}*", E_SYNTAX, 4 },
};

static void to_ecp(const char* s, Vec<ecp>* out)
{
  out->clear();
  es_utf8_to_ecp(s, es_len(s), out);
}

static void fmt_result(int r, const RxMatch& m, u32 ng, char* out, u32 cap)
{
  u32 k;
  if (r < 0) { es_fmt(out, cap, "E%d", r); return; }
  if (r == 0) { es_copy(out, cap, "-"); return; }
  es_fmt(out, cap, "%u,%u", m.pos, m.pos + m.len);
  for (k = 1; k <= ng && k < RX_GROUPS; k++) {
    char b[32];
    if (m.gpos[k] == EPOS_NONE) es_copy(b, sizeof(b), "|-");
    else es_fmt(b, sizeof(b), "|%u,%u", m.gpos[k], m.gpos[k] + m.glen[k]);
    es_cat(out, cap, b);
  }
}

static int match_str(const char* pat, u32 flags, const u8* text, u32 tlen, epos lo, epos from, epos hi, epos ctx,
                     char* out, u32 cap)
{
  Vec<ecp> p;
  RxProg* prog = 0;
  RxError err;
  RxMatcher mt;
  RxMatch m;
  ViewSource src(text, tlen);
  int r;
  to_ecp(pat, &p);
  r = rx_compile(p.data(), p.size(), flags, &prog, &err);
  if (r != E_OK) { es_fmt(out, cap, "E%d@%u", r, err.pos); return r; }
  r = mt.init(prog);
  if (r == E_OK) {
    mt.set_context_end(ctx);
    r = mt.search(&src, codec_get(enc_utf8(0)), lo, from, hi, &m, 0);
  }
  fmt_result(r, m, rx_group_count(prog), out, cap);
  rx_free(prog);
  return r;
}

static int expect_eq(const char* got, const char* exp)
{
  u32 n;
  if (strchr(exp, '|')) return strcmp(got, exp) == 0;
  n = es_len(exp);
  return strncmp(got, exp, n) == 0 && (got[n] == 0 || got[n] == '|');
}

static void test_table(TF* tf)
{
  u32 i, n = sizeof(k_cases) / sizeof(k_cases[0]);
  for (i = 0; i < n; i++) {
    const RxtCase& k = k_cases[i];
    u32 tl = es_len(k.text), pass;
    char got[256];
    for (pass = 0; pass < 2; pass++) {
      u32 fl = k.flags | (pass ? (u32)RXF_FORCEBT_ : 0u);
      match_str(k.pat, fl, (const u8*)k.text, tl, 0, 0, tl, EPOS_NONE, got, sizeof(got));
      if (k.expect[0] == 'E') {
        if (got[0] != 'E') tf_fail(tf, __FILE__, __LINE__, "case %u /%s/: expected an error, got %s", i, k.pat, got);
        else tf_check(tf, 1, __FILE__, __LINE__, "error");
        break;
      }
      if (!expect_eq(got, k.expect))
        tf_fail(tf, __FILE__, __LINE__, "case %u /%s/ %s on \"%s\": got %s, expected %s", i, k.pat,
                pass ? "(bt)" : "(vm)", k.text, got, k.expect);
      else tf_check(tf, 1, __FILE__, __LINE__, "case");
    }
  }
  tf_log(tf, "regex table: %u cases", n);
}

static void test_errors(TF* tf)
{
  u32 i, n = sizeof(k_errs) / sizeof(k_errs[0]);
  for (i = 0; i < n; i++) {
    Vec<ecp> p;
    RxProg* prog = 0;
    RxError err;
    int r;
    to_ecp(k_errs[i].pat, &p);
    r = rx_compile(p.data(), p.size(), 0, &prog, &err);
    if (r != k_errs[i].code || err.code != r || err.pos != k_errs[i].pos || !err.msg[0] || prog)
      tf_fail(tf, __FILE__, __LINE__, "error case %u /%s/: got %s @%u \"%s\", expected %s @%u", i, k_errs[i].pat,
              e_errname(r), err.pos, err.msg, e_errname(k_errs[i].code), k_errs[i].pos);
    else tf_check(tf, 1, __FILE__, __LINE__, "err");
    rx_free(prog);
  }
  // limits
  {
    Vec<ecp> p;
    RxProg* prog = 0;
    RxError err;
    u32 k;
    for (k = 0; k < 1001; k++) p.push('a');
    TF_EQ_I(rx_compile(p.data(), 1001, 0, &prog, &err), E_TOOBIG);
    TF_EQ_U(err.pos, 1000);
    TF_EQ_I(rx_compile(p.data(), 1000, 0, &prog, &err), E_OK);
    rx_free(prog);
    prog = 0;
    p.clear();
    for (k = 0; k < 101; k++) p.push('(');
    for (k = 0; k < 101; k++) p.push(')');
    TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_TOOBIG);
    p.clear();
    for (k = 0; k < 100; k++) p.push('(');
    for (k = 0; k < 100; k++) p.push(')');
    TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
    TF_EQ_U(rx_group_count(prog), 9);
    rx_free(prog);
    prog = 0;
    to_ecp("a{1000}", &p);
    TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
    rx_free(prog);
    prog = 0;
    to_ecp("[ab]{1000}{1000}", &p);
    TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_SYNTAX);
    to_ecp("(?:(?:a{100}){100}){100}", &p);
    TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_TOOBIG);
  }
}

static void test_api(TF* tf)
{
  Vec<ecp> p, lit;
  RxProg* prog = 0;
  RxError err;
  int ic = 0;
  to_ecp("(foo)bar\\d", &p);
  TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
  TF_EQ_U(rx_group_count(prog), 1);
  TF_EQ_I(rx_uses_backtracker(prog), 0);
  TF_EQ_I(rx_literal_prefix(prog, &lit, &ic), E_OK);
  TF_EQ_U(lit.size(), 6);
  TF_EQ_I(ic, 0);
  TF_CHECK(lit.size() == 6 && lit[0] == 'f' && lit[5] == 'r');
  rx_free(prog);
  to_ecp("(?i)Ab{2}c|x", &p);
  TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
  rx_literal_prefix(prog, &lit, &ic);
  TF_EQ_U(lit.size(), 0);
  rx_free(prog);
  to_ecp("\\bAb{2}c+", &p);
  TF_EQ_I(rx_compile(p.data(), p.size(), I, &prog, &err), E_OK);
  rx_literal_prefix(prog, &lit, &ic);
  TF_EQ_U(lit.size(), 4);
  TF_EQ_I(ic, 1);
  TF_CHECK(lit.size() == 4 && lit[0] == 'a' && lit[3] == 'c');
  rx_free(prog);
  to_ecp("(a)\\1", &p);
  TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
  TF_EQ_I(rx_uses_backtracker(prog), 1);
  rx_free(prog);
  to_ecp("a(?=b)", &p);
  TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
  TF_EQ_I(rx_uses_backtracker(prog), 1);
  rx_free(prog);
  prog = 0;
  TF_EQ_I(rx_literal_prefix(0, &lit, &ic), E_STATE);
  {
    RxMatcher mt;
    RxMatch m;
    ViewSource src((const u8*)"x", 1);
    TF_EQ_I(mt.search(&src, codec_get(enc_utf8(0)), 0, 0, 1, &m, 0), E_STATE);
    TF_EQ_I(mt.init(0), E_STATE);
  }
  // NUL characters
  {
    static const u8 t[] = { 'a', 0, 'b', 0 };
    char got[64];
    match_str("\\0b", 0, t, 4, 0, 0, 4, EPOS_NONE, got, sizeof(got));
    TF_EQ_S(got, "1,3");
    match_str("a.b", 0, t, 4, 0, 0, 4, EPOS_NONE, got, sizeof(got));
    TF_EQ_S(got, "0,3");
    match_str("\\x00$", 0, t, 4, 0, 0, 4, EPOS_NONE, got, sizeof(got));
    TF_EQ_S(got, "3,4");
  }
}

// lo / from / hi / context end
static void test_context(TF* tf)
{
  char got[64];
  const u8* t = (const u8*)"ba";
  match_str("^a", 0, t, 2, 1, 1, 2, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "1,2");
  match_str("\\ba", 0, t, 2, 1, 1, 2, EPOS_NONE, got, sizeof(got));  TF_EQ_S(got, "1,2");
  match_str("^a", 0, t, 2, 0, 1, 2, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "-");
  match_str("^a|^b", 0, t, 2, 1, 1, 2, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "1,2");
  match_str("^(a)\\1?", 0, t, 2, 1, 1, 2, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "1,2|1,2");
  match_str("\\Aa", 0, t, 2, 1, 1, 2, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "1,2");
  t = (const u8*)"ab";
  match_str("a$", 0, t, 2, 0, 0, 1, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "0,1");
  match_str("a$", 0, t, 2, 0, 0, 1, 2, got, sizeof(got));           TF_EQ_S(got, "-");
  match_str("a\\b", 0, t, 2, 0, 0, 1, 2, got, sizeof(got));         TF_EQ_S(got, "-");
  match_str("a\\b", 0, t, 2, 0, 0, 1, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "0,1");
  match_str("a\\z", 0, t, 2, 0, 0, 1, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "0,1");
  match_str("ab", 0, t, 2, 0, 0, 1, 2, got, sizeof(got));           TF_EQ_S(got, "-");
  t = (const u8*)"foobar";
  match_str("foo(?=bar)", 0, t, 6, 0, 0, 3, 6, got, sizeof(got));   TF_EQ_S(got, "0,3");
  match_str("foo(?=bar)", 0, t, 6, 0, 0, 3, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "-");
  match_str("foo(?!bar)", 0, t, 6, 0, 0, 3, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "0,3");
  t = (const u8*)"aXa";
  match_str("a", 0, t, 3, 0, 1, 3, EPOS_NONE, got, sizeof(got));    TF_EQ_S(got, "2,3");
  match_str("a", 0, t, 3, 0, 3, 3, EPOS_NONE, got, sizeof(got));    TF_EQ_S(got, "-");
  match_str("a*", 0, t, 3, 0, 3, 3, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "3,3");
  match_str("(?=X)", 0, t, 3, 0, 0, 3, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "1,1");
  t = (const u8*)"aaa";
  match_str("a+", 0, t, 3, 0, 0, 2, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "0,2");
  match_str("a+", 0, t, 3, 0, 1, 2, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "1,2");
  match_str("(a)\\1", 0, t, 3, 0, 1, 3, EPOS_NONE, got, sizeof(got)); TF_EQ_S(got, "1,3|1,2");
  match_str("(a)\\1", 0, t, 3, 0, 0, 1, 3, got, sizeof(got));       TF_EQ_S(got, "-");
  // a multi-byte character straddling hi is not consumed
  t = (const u8*)"a\xc3\xa4";
  match_str("a.", 0, t, 3, 0, 0, 2, 3, got, sizeof(got));           TF_EQ_S(got, "-");
  match_str("a.", 0, t, 3, 0, 0, 3, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "0,3");
  match_str("a\\b", 0, t, 3, 0, 0, 1, 3, got, sizeof(got));         TF_EQ_S(got, "-");
  match_str("a\\B", 0, t, 3, 0, 0, 1, 3, got, sizeof(got));         TF_EQ_S(got, "0,1");
  // CRLF context at the floor: ^ at lo even between CR and LF
  t = (const u8*)"\r\nx";
  match_str("^", 0, t, 3, 1, 1, 3, EPOS_NONE, got, sizeof(got));    TF_EQ_S(got, "1,1");
  match_str("^x", 0, t, 3, 0, 1, 3, EPOS_NONE, got, sizeof(got));   TF_EQ_S(got, "2,3");
  match_str("$", 0, t, 3, 0, 1, 3, EPOS_NONE, got, sizeof(got));    TF_EQ_S(got, "3,3");
}

// Characters needing the full Unicode tables (T02's ucd). Checked only when the tables are present.
static void test_unicode_fold(TF* tf)
{
  static const RxtCase k_uni[] = {
    { "k", I, "\xe2\x84\xaa", "0,3" }, { "K", I, "\xe2\x84\xaa", "0,3" }, { "[k]", I, "\xe2\x84\xaa", "0,3" },
    { "[a-z]", I, "\xe2\x84\xaa", "0,3" }, { "\xe2\x84\xaa", I, "k", "0,1" }, { "s", I, "\xc5\xbf", "0,2" },
    { "\xce\xa3", I, "\xcf\x83", "0,2" }, { "\xcf\x83", I, "\xcf\x82", "0,2" }, { "\xd0\x96", I, "\xd0\xb6", "0,2" },
    { "\xc3\x9f", I, "\xe1\xba\x9e", "0,3" }, { "\\w+", 0, "\xd0\x96\xd0\xb6", "0,4" },
    { "[[:upper:]]", 0, "a\xd0\x96", "1,3" }, { "[[:lower:]]", 0, "A\xd0\xb6", "1,3" }, { "\\b\xd0\xb6", 0, "\xd0\x96\xd0\xb6 \xd0\xb6", "5,7" },
    { "\xc3\xbf", I, "\xc5\xb8", "0,2" }, { "(\xd0\x96)\\1", I, "\xd0\x96\xd0\xb6", "0,4|0,2" }, { "\xf0\x90\x90\x80", I, "\xf0\x90\x90\xa8", "0,4" },
  };
  u32 i, n = sizeof(k_uni) / sizeof(k_uni[0]);
  if (uc_fold(0x212A) != 'k' || uc_fold(0x416) != 0x436) {
    tf_log(tf, "regex: %u Unicode case-folding cases skipped (minimal ucd; they need T02's tables)", n);
    return;
  }
  for (i = 0; i < n; i++) {
    char got[128];
    u32 pass;
    for (pass = 0; pass < 2; pass++) {
      u32 tl = es_len(k_uni[i].text);
      match_str(k_uni[i].pat, k_uni[i].flags | (pass ? (u32)RXF_FORCEBT_ : 0u), (const u8*)k_uni[i].text, tl, 0, 0, tl,
                EPOS_NONE, got, sizeof(got));
      if (!expect_eq(got, k_uni[i].expect)) {
        tf_fail(tf, __FILE__, __LINE__, "unicode case %u /%s/: got %s, expected %s", i, k_uni[i].pat, got, k_uni[i].expect);
      } else {
        tf_check(tf, 1, __FILE__, __LINE__, "uni");
      }
    }
  }
  // only the characters rxpre.cpp knows about may fold into ASCII
  {
    ecp c;
    u32 found = 0;
    for (c = 0x80; c < 0x110000; c++) {
      ecp f = uc_fold(c);
      if (f < 0x80 && c != 0x130 && c != 0x131 && c != 0x17F && c != 0x212A) {
        tf_fail(tf, __FILE__, __LINE__, "U+%X folds to ASCII %X: add it to k_fold_into_ascii (rxpre.cpp)", c, f);
        found++;
        if (found > 5) break;
      }
    }
    TF_EQ_U(found, 0);
  }
}

// ---- random patterns: Pike VM vs backtracker ----
// Repeated subexpressions that can match empty (like (a*)*) are left out: there the backtracker follows ECMAScript
// (an empty iteration fails) and the Pike VM drops the revisited state, so captures/lengths may differ (HANDOFF).

enum { GA_REPEATABLE = 1, GA_NULLABLE = 2 };

static int gen_atom(TF* tf, EStr* s, u32 depth, u32* groups);

static int gen_seq(TF* tf, EStr* s, u32 depth, u32* groups)    // returns 1 if the sequence can match empty
{
  u32 n = 1 + tf_rand_below(tf, 3), k;
  int nul = 1;
  for (k = 0; k < n; k++) {
    int a = gen_atom(tf, s, depth, groups), anul = (a & GA_NULLABLE) ? 1 : 0;
    if (a & GA_REPEATABLE) {
      u32 q = tf_rand_below(tf, 10);
      if (anul && (q <= 1 || q == 3 || q == 4 || q == 5)) q = 2;   // only ? on a nullable atom
      switch (q) {
      case 0: s->append("*"); anul = 1; break;
      case 1: s->append("+"); break;
      case 2: s->append("?"); anul = 1; break;
      case 3: s->append("*?"); anul = 1; break;
      case 4: s->append("+?"); break;
      case 5: {
        u32 mn = tf_rand_below(tf, 2);
        s->appendf("{%u,%u}", mn, 1 + tf_rand_below(tf, 3));
        if (mn == 0) anul = 1;
        break;
      }
      default: break;
      }
    }
    if (!anul) nul = 0;
  }
  return nul;
}

static int gen_atom(TF* tf, EStr* s, u32 depth, u32* groups)
{
  static const char* atoms[] = { "a", "b", "c", ".", "[ab]", "[^a]", "\\w", "\\s", "a", "b", "^", "$", "\\b", "\\B",
                                 "\\n", "[a-c]", "x" };
  u32 r = tf_rand_below(tf, depth < 3 ? 22 : 17);
  int nul;
  if (r < 17) {
    s->append(atoms[r]);
    return (r >= 10 && r <= 13) ? GA_NULLABLE : GA_REPEATABLE;
  }
  if (r < 19 && *groups < 9) {
    (*groups)++;
    s->append("(");
    nul = gen_seq(tf, s, depth + 1, groups);
    if (tf_rand_below(tf, 2)) { s->append("|"); nul |= gen_seq(tf, s, depth + 1, groups); }
    s->append(")");
    return GA_REPEATABLE | (nul ? GA_NULLABLE : 0);
  }
  s->append("(?:");
  nul = gen_seq(tf, s, depth + 1, groups);
  s->append("|");
  nul |= gen_seq(tf, s, depth + 1, groups);
  s->append(")");
  return GA_REPEATABLE | (nul ? GA_NULLABLE : 0);
}

static void test_vm_vs_bt(TF* tf)
{
  u32 iter, n = tf->long_run ? 50000 : 3000, bad = 0, matched = 0, budget = 0;
  const Codec* u8c = codec_get(enc_utf8(0));
  for (iter = 0; iter < n && bad < 10; iter++) {
    EStr ps;
    Vec<ecp> pat;
    RxProg* p1 = 0;
    RxProg* p2 = 0;
    RxError err;
    u32 groups = 0, tl, k, flags = 0;
    char text[40];
    static const char alpha[] = "aabbc \nx";
    gen_seq(tf, &ps, 0, &groups);
    to_ecp(ps.c_str(), &pat);
    if (tf_rand_below(tf, 4) == 0) flags |= RXF_ICASE;
    if (tf_rand_below(tf, 4) == 0) flags |= RXF_DOTALL;
    tl = tf_rand_below(tf, 16);
    for (k = 0; k < tl; k++) text[k] = alpha[tf_rand_below(tf, 8)];
    if (flags & RXF_ICASE) for (k = 0; k < tl; k++) if (tf_rand_below(tf, 3) == 0 && text[k] >= 'a') text[k] -= 32;
    text[tl] = 0;
    if (rx_compile(pat.data(), pat.size(), flags, &p1, &err) != E_OK ||
        rx_compile(pat.data(), pat.size(), flags | RXF_FORCEBT_, &p2, &err) != E_OK) {
      tf_fail(tf, __FILE__, __LINE__, "random pattern /%s/ didn't compile: %s", ps.c_str(), err.msg);
      bad++;
    } else {
      RxMatcher m1, m2;
      ViewSource src((const u8*)text, tl);
      epos from = tl ? tf_rand_below(tf, tl + 1) : 0;
      RxMatch a, b;
      int r1, r2;
      char s1[128], s2[128];
      m1.init(p1);
      m2.init(p2);
      r1 = m1.search(&src, u8c, 0, from, tl, &a, 0);
      r2 = m2.search(&src, u8c, 0, from, tl, &b, 0);
      fmt_result(r1, a, p1->ngroups, s1, sizeof(s1));
      fmt_result(r2, b, p2->ngroups, s2, sizeof(s2));
      if (r2 == E_BUDGET) {
        budget++;
      } else if (strcmp(s1, s2) != 0) {
        tf_fail(tf, __FILE__, __LINE__, "vm/bt differ: /%s/ flags %u on \"%s\" from %u: vm %s bt %s", ps.c_str(), flags,
                text, from, s1, s2);
        bad++;
      } else {
        tf_check(tf, 1, __FILE__, __LINE__, "vmbt");
        if (r1 == 1) matched++;
      }
    }
    rx_free(p1);
    rx_free(p2);
  }
  tf_log(tf, "vm vs bt: %u random patterns, %u matched, %u over the backtracker budget", iter, matched, budget);
}

// ---- pathological patterns ----

static double secs_since(clock_t t0) { return (double)(clock() - t0) / CLOCKS_PER_SEC; }

static void test_pathological(TF* tf)
{
  static const char* pats[] = { "(a*)*b", "(a|a)*b", "(a|aa)*c", "(x+x+)+y", "(a?){30}a{30}", "(?:a*)*?b", ".*.*.*=",
                                "(\\w+\\s?)*$x", "(a|b|ab)*c", "(?:a+)+b",
                                // no literal to look for: the VM runs over all 100 KB
                                "(a*)*[bc]", "(a|a)*[bc]", "(a|aa)*[^a]", "(?:a+)+[bc]", "(.*)*[=]", ".*.*.*[=]",
                                "(a|b|ab)*[cd]", "(?:a*)*?[bc]", "\\w*\\s",
                                // linear but many live threads (counted repeats): timed, not bounded (PII: ~2 s)
                                "(a{1,5}){1,5}[bc]" };
  u32 n = 100000, i;
  u8* buf = (u8*)e_alloc(n);
  const Codec* u8c = codec_get(enc_utf8(0));
  if (!buf) { TF_CHECK(0); return; }
  memset(buf, 'a', n);
  for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
    Vec<ecp> p;
    RxProg* prog = 0;
    RxError err;
    RxMatcher mt;
    RxMatch m;
    ViewSource src(buf, n);
    clock_t t0;
    double dt;
    int r;
    to_ecp(pats[i], &p);
    TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
    mt.init(prog);
    t0 = clock();
    r = mt.search(&src, u8c, 0, 0, n, &m, 0);
    dt = secs_since(t0);
    TF_CHECK(r == 0 || r == 1);
    if (dt > 1.0 && strcmp(pats[i], "(a{1,5}){1,5}[bc]") != 0)
      tf_fail(tf, __FILE__, __LINE__, "/%s/ took %d ms on 100 KB", pats[i], (int)(dt * 1000));
    else tf_check(tf, 1, __FILE__, __LINE__, "patho");
    tf_log(tf, "pathological /%s/ on 100 KB: %d ms (result %d)", pats[i], (int)(dt * 1000), r);
    rx_free(prog);
  }
  // the backtracker stops at its budget
  {
    static const char* bts[] = { "(a*)*b\\1", "(a|a)*(?=b)", "(x+x+)+y\\1", "(.*)(.*)(.*)\\3\\2\\1b",
                                 "(a*)*[bc]\\1", "(a|a)*(?=[bc])", "(.*)(.*)(.*)\\3\\2\\1[bc]" };
    for (i = 0; i < sizeof(bts) / sizeof(bts[0]); i++) {
      Vec<ecp> p;
      RxProg* prog = 0;
      RxError err;
      RxMatcher mt;
      RxMatch m;
      ViewSource src(buf, 20000);
      clock_t t0;
      double dt;
      int r;
      to_ecp(bts[i], &p);
      TF_EQ_I(rx_compile(p.data(), p.size(), 0, &prog, &err), E_OK);
      mt.init(prog);
      mt.set_budget(2000000);
      t0 = clock();
      r = mt.search(&src, u8c, 0, 0, 20000, &m, 0);
      dt = secs_since(t0);
      TF_CHECK(r == E_BUDGET || r == 0);
      tf_log(tf, "backtracker /%s/ on 20 KB: %s in %d ms", bts[i], e_errname(r), (int)(dt * 1000));
      rx_free(prog);
    }
  }
  e_free(buf);
}

// ---- out of memory ----

static void test_oom(TF* tf)
{
  static const char* pats[] = { "(a|b)+c", "[[:alpha:]\\d]+x", "(?i)hello\\s+world", "(a)\\1(?=b)", "a{50}" };
  u32 i;
  i32 k;
  const Codec* u8c = codec_get(enc_utf8(0));
  const char* text = "xx hello  WORLD aab aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
    Vec<ecp> p;
    to_ecp(pats[i], &p);
    for (k = 1; k < 200; k++) {
      RxProg* prog = 0;
      RxError err;
      int r;
      e_test_fail_after(k);
      r = rx_compile(p.data(), p.size(), 0, &prog, &err);
      if (r == E_OK) {
        RxMatcher mt;
        RxMatch m;
        ViewSource src((const u8*)text, es_len(text));
        r = mt.init(prog);
        if (r == E_OK) r = mt.search(&src, u8c, 0, 0, es_len(text), &m, 0);
        TF_CHECK(r == 1 || r == 0 || r == E_NOMEM);
      } else {
        TF_CHECK(r == E_NOMEM && prog == 0);
      }
      e_test_fail_after(-1);
      rx_free(prog);
      if (r != E_NOMEM) break;
    }
  }
}

void suite_regex(TF* tf)
{
  test_table(tf);
  test_errors(tf);
  test_api(tf);
  test_context(tf);
  test_unicode_fold(tf);
  test_vm_vs_bt(tf);
  test_pathological(tf);
  test_oom(tf);
}
