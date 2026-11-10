// rxfuzz.cpp - Mac-only differential fuzz test (T03): our regex engine (Pike VM, forced backtracker, and the
// Searcher's match iteration) against std::regex ECMAScript | multiline on generated patterns from the shared subset.
// Also prints rough throughput numbers. Usage: rxfuzz [cases] [seed]
//
// Excluded on purpose (documented differences, see HANDOFF T03):
// - repeated subexpressions that can match empty, e.g. (a*)* (ECMAScript fails an empty iteration; our Pike VM
//   drops the revisited state);
// - captures inside a repeated group are compared only when no iteration can leave them unset (ECMAScript resets
//   inner captures at each iteration, we keep the last value like Perl/Boost);
// - backreferences only to groups that always take part (ECMAScript matches an unset group as empty, we fail).
// libc++ bugs worked around: \b never matches at the end of the input (texts get a '\n' sentinel); multiline ^ is
// unreliable (matches after non-newlines, and at `first` with match_prev_avail), so ^ is not generated here (the
// t_regex table and the Pike/backtracker differential cover it); a lookahead is matched as a sub-search starting at
// the current position, so \b \B inside it can't see the previous character: not generated inside lookaheads.
#include <regex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "regex.h"
#include "regex_int.h"
#include "search.h"
#include "codec.h"
#include "estr.h"

static unsigned g_rng = 1;
static unsigned rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static unsigned below(unsigned n) { return rnd() % n; }

enum { NUL = 1, CAP = 2, REP = 4 };   // nullable, contains a capture, repeatable

struct Gen {
  int groups;
  bool caps_unsafe;     // a capture inside a repeated group
  bool allow_caps;
  bool use_look;
  bool in_look;         // libc++ evaluates \b at a lookahead's start without the previous character: none there
};

static int gen_atom(Gen* g, std::string* s, int depth);

static int gen_seq(Gen* g, std::string* s, int depth)
{
  int n = 1 + (int)below(3), flags = NUL;
  for (int k = 0; k < n; k++) {
    int a = gen_atom(g, s, depth);
    bool anul = (a & NUL) != 0;
    if (a & REP) {
      unsigned q = below(10);
      if (anul && (q <= 1 || q == 3 || q == 4 || q == 5)) q = 2;
      bool multi = false;
      switch (q) {
      case 0: *s += "*"; anul = true; multi = true; break;
      case 1: *s += "+"; multi = true; break;
      case 2: *s += "?"; anul = true; break;
      case 3: *s += "*?"; anul = true; multi = true; break;
      case 4: *s += "+?"; multi = true; break;
      case 5: {
        unsigned mn = below(2), mx = 1 + below(3);
        char b[32];
        snprintf(b, sizeof(b), "{%u,%u}", mn, mx);
        *s += b;
        if (mn == 0) anul = true;
        if (mx > 1) multi = true;
        break;
      }
      case 6: *s += "{2}"; multi = true; break;
      default: break;
      }
      if (multi && (a & CAP)) g->caps_unsafe = true;
    }
    if (!anul) flags &= ~NUL;
    if (a & CAP) flags |= CAP;
  }
  return flags;
}

static int gen_atom(Gen* g, std::string* s, int depth)
{
  static const char* atoms[] = { "a", "b", "c", ".", "[ab]", "[^a]", "\\w", "\\s", "a", "b", "x", "[a-c]", "\\d",
                                 "\\W", "1", "\\n", "$", "\\b", "\\B" };
  unsigned r = below(depth < 3 ? 25u : 19u);
  if (g->in_look && (r == 17 || r == 18)) r = 16;
  if (r < 16) { *s += atoms[r]; return REP; }
  if (r < 19) { *s += atoms[r]; return NUL; }
  r++;
  if (r < 22 && g->allow_caps && g->groups < 9) {
    g->groups++;
    *s += "(";
    int f = gen_seq(g, s, depth + 1);
    if (below(2)) { *s += "|"; f |= gen_seq(g, s, depth + 1); }
    *s += ")";
    return REP | CAP | (f & (NUL | CAP));
  }
  if (r == 22 && g->use_look) {
    bool save = g->allow_caps, save_look = g->in_look;
    g->allow_caps = false;
    g->in_look = true;
    *s += below(2) ? "(?=" : "(?!";
    gen_seq(g, s, depth + 1);
    *s += ")";
    g->allow_caps = save;
    g->in_look = save_look;
    return NUL;
  }
  *s += "(?:";
  int f = gen_seq(g, s, depth + 1);
  *s += "|";
  f |= gen_seq(g, s, depth + 1);
  *s += ")";
  return REP | (f & (NUL | CAP));
}

static std::string gen_pattern(Gen* g, bool* backref)
{
  std::string s;
  g->groups = 0;
  g->caps_unsafe = false;
  g->allow_caps = true;
  g->use_look = below(4) == 0;
  g->in_look = false;
  *backref = below(5) == 0;
  if (*backref) {
    // A (G) B \1 C with no capture in A, so G is group 1 and always set when \1 runs
    g->allow_caps = false;
    if (below(2)) gen_seq(g, &s, 1);
    g->allow_caps = true;
    s += "(";
    g->groups = 1;
    gen_seq(g, &s, 1);
    s += ")";
    if (below(2)) gen_seq(g, &s, 1);
    s += "(?:\\1)";
    if (below(2)) gen_seq(g, &s, 1);
    return s;
  }
  gen_seq(g, &s, 0);
  return s;
}

static void to_ecp(const std::string& s, Vec<ecp>* out)
{
  out->clear();
  for (size_t i = 0; i < s.size(); i++) out->push((ecp)(unsigned char)s[i]);
}

struct Res { int r; int s, e; int gs[10], ge[10]; };

static std::string fmt(const Res& x, int ng, bool groups)
{
  char b[64];
  if (x.r < 0) { snprintf(b, sizeof(b), "E%d", x.r); return b; }
  if (x.r == 0) return "-";
  snprintf(b, sizeof(b), "%d,%d", x.s, x.e);
  std::string out = b;
  if (groups) {
    for (int k = 1; k <= ng; k++) {
      if (x.gs[k] < 0) out += "|-";
      else { snprintf(b, sizeof(b), "|%d,%d", x.gs[k], x.ge[k]); out += b; }
    }
  }
  return out;
}

static Res ours(RxProg* p, const std::string& text, unsigned from)
{
  RxMatcher mt;
  RxMatch m;
  Res x;
  ViewSource src((const u8*)text.data(), (epos)text.size());
  memset(&x, 0, sizeof(x));
  mt.init(p);
  x.r = mt.search(&src, codec_get(enc_utf8(0)), 0, from, (epos)text.size(), &m, 0);
  if (x.r == 1) {
    x.s = (int)m.pos;
    x.e = (int)(m.pos + m.len);
    for (int k = 1; k < 10; k++) {
      if (m.gpos[k] == EPOS_NONE) x.gs[k] = x.ge[k] = -1;
      else { x.gs[k] = (int)m.gpos[k]; x.ge[k] = (int)(m.gpos[k] + m.glen[k]); }
    }
  }
  return x;
}

static Res theirs(const std::regex& re, const std::string& text, unsigned from)
{
  std::smatch m;
  Res x;
  memset(&x, 0, sizeof(x));
  std::regex_constants::match_flag_type fl = std::regex_constants::match_default;
  if (from > 0) {                     // libc++ treats `first` as a line start even with match_prev_avail
    fl |= std::regex_constants::match_prev_avail;
    if (text[from - 1] != '\n') fl |= std::regex_constants::match_not_bol;
  }
  bool f;
  try {
    f = std::regex_search(text.begin() + from, text.end(), m, re, fl);
  } catch (...) {
    x.r = -999;                       // std::regex gave up (complexity limit)
    return x;
  }
  x.r = f ? 1 : 0;
  if (f) {
    x.s = (int)(m[0].first - text.begin());
    x.e = (int)(m[0].second - text.begin());
    for (int k = 1; k < 10; k++) {
      if (k >= (int)m.size() || !m[k].matched) x.gs[k] = x.ge[k] = -1;
      else { x.gs[k] = (int)(m[k].first - text.begin()); x.ge[k] = (int)(m[k].second - text.begin()); }
    }
  }
  return x;
}

// all matches with our iteration rule (continue at the end; after an empty match one character later)
static std::string iter_theirs(const std::regex& re, const std::string& text)
{
  std::string out;
  size_t pos = 0;
  char b[32];
  for (;;) {
    Res x = theirs(re, text, (unsigned)pos);
    if (x.r == -999) return "skip";
    if (x.r != 1) break;
    snprintf(b, sizeof(b), "%d,%d;", x.s, x.e);
    out += b;
    pos = x.e > x.s ? (size_t)x.e : (size_t)x.s + 1;
    if (pos > text.size()) break;
  }
  return out;
}

static std::string iter_ours(const std::string& pat, int icase, const std::string& text)
{
  Searcher s;
  SearchOpts o;
  SearchError err;
  Vec<ecp> p;
  Vec<MarkRange> all;
  std::string out;
  char b[32];
  o.mode = SM_REGEX; o.match_case = icase ? 0 : 1; o.whole_word = 0; o.dot_all = 0;
  to_ecp(pat, &p);
  if (s.compile(p.data(), p.size(), o, enc_utf8(0), &err) != E_OK) return "compile error";
  ViewSource src((const u8*)text.data(), (epos)text.size());
  int r = s.find_all(&src, codec_get(enc_utf8(0)), 0, (epos)text.size(), &all, 100000, 0);
  if (r < 0) return "error";
  for (u32 k = 0; k < all.size(); k++) { snprintf(b, sizeof(b), "%u,%u;", all[k].pos, all[k].pos + all[k].len); out += b; }
  return out;
}

static double now() { return (double)clock() / CLOCKS_PER_SEC; }

static void throughput()
{
  const unsigned n = 8 * 1024 * 1024;
  std::string text;
  text.reserve(n + 100);
  static const char* words[] = { "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "int ",
                                 "return ", "value; ", "x = 42;\r\n", "if (a < b) ", "{\r\n", "}\r\n", "while " };
  g_rng = 12345;
  while (text.size() < n) text += words[below(16)];
  struct { const char* pat; int mode; int icase; } tests[] = {
    { "zebra", SM_NORMAL, 0 }, { "ZEBRA", SM_NORMAL, 1 }, { "lazy dog", SM_NORMAL, 0 }, { "q", SM_NORMAL, 0 },
    { "zebra", SM_REGEX, 0 }, { "\\bzeb\\w+", SM_REGEX, 0 }, { "[0-9]{3}", SM_REGEX, 0 }, { "a.*z", SM_REGEX, 0 },
    { "(foo|bar)baz", SM_REGEX, 0 }, { "\\w+ing\\b", SM_REGEX, 0 }, { "^\\s*#", SM_REGEX, 0 },
    { "(\\w)\\1x", SM_REGEX, 0 }, { "Straße", SM_NORMAL, 1 },
  };
  printf("throughput on %u MB (UTF-8, no match unless noted):\n", n >> 20);
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    Searcher s;
    SearchOpts o;
    SearchError err;
    Vec<ecp> p;
    o.mode = (u8)tests[i].mode; o.match_case = tests[i].icase ? 0 : 1; o.whole_word = 0; o.dot_all = 0;
    std::string ps = tests[i].pat;
    p.clear();
    es_utf8_to_ecp(ps.c_str(), (u32)ps.size(), &p);
    if (s.compile(p.data(), p.size(), o, enc_utf8(0), &err) != E_OK) { printf("  compile error %s\n", err.msg); continue; }
    ViewSource src((const u8*)text.data(), (epos)text.size());
    u32 cnt = 0;
    double t0 = now();
    s.count(&src, codec_get(enc_utf8(0)), 0, (epos)text.size(), &cnt, 0);
    double dt = now() - t0;
    printf("  %-8s %-14s %8.1f MB/s  (%u matches)\n", tests[i].mode == SM_NORMAL ? (tests[i].icase ? "normal/i" : "normal")
           : "regex", tests[i].pat, (n / 1048576.0) / (dt > 0 ? dt : 1e-9), cnt);
  }
}

int main(int argc, char** argv)
{
  int cases = argc > 1 ? atoi(argv[1]) : 10000;
  g_rng = argc > 2 ? (unsigned)atoi(argv[2]) : 7;
  int bad = 0, matched = 0, skipped_budget = 0, skipped_std = 0, iters = 0;
  codec_init();
  enc_set_system(1252, 437);
  for (int c = 0; c < cases && bad < 20; c++) {
    Gen g;
    bool backref;
    std::string pat = gen_pattern(&g, &backref);
    int icase = below(4) == 0;
    std::string text;
    unsigned tl = below(20);
    static const char alpha[] = "aabbc \nx1";
    for (unsigned k = 0; k < tl; k++) {
      char ch = alpha[below(9)];
      if (icase && ch >= 'a' && below(3) == 0) ch = (char)(ch - 32);
      text += ch;
    }
    text += '\n';                      // sentinel: libc++'s \b never matches at the end of the input
    tl++;
    std::regex re;
    try {
      re = std::regex(pat, std::regex::ECMAScript | std::regex::multiline | (icase ? std::regex::icase : std::regex::ECMAScript));
    } catch (...) {
      printf("std::regex rejected /%s/\n", pat.c_str());
      continue;
    }
    Vec<ecp> p;
    to_ecp(pat, &p);
    RxProg* p1 = 0;
    RxProg* p2 = 0;
    RxError err;
    u32 fl = icase ? RXF_ICASE : 0;
    if (rx_compile(p.data(), p.size(), fl, &p1, &err) != E_OK || rx_compile(p.data(), p.size(), fl | RXF_FORCEBT_, &p2, &err) != E_OK) {
      printf("FAIL: we rejected /%s/: %s\n", pat.c_str(), err.msg);
      bad++;
      rx_free(p1);
      continue;
    }
    unsigned from = tl ? below(tl + 1) : 0;
    if (below(2)) from = 0;
    Res a = ours(p1, text, from), b = ours(p2, text, from), t = theirs(re, text, from);
    int ng = (int)p1->ngroups;
    bool groups = !g.caps_unsafe;
    std::string sa = fmt(a, ng, groups), sb = fmt(b, ng, groups), st = fmt(t, ng, groups);
    if (t.r == -999) { skipped_std++; rx_free(p1); rx_free(p2); continue; }
    if (b.r == E_BUDGET) { skipped_budget++; sb = sa; }
    if (sa != st || sb != st) {
      printf("FAIL /%s/%s on \"", pat.c_str(), icase ? "i" : "");
      for (size_t k = 0; k < text.size(); k++) { if (text[k] == '\n') printf("\\n"); else putchar(text[k]); }
      printf("\" from %u: ours %s, bt %s, std::regex %s\n", from, sa.c_str(), sb.c_str(), st.c_str());
      bad++;
    } else if (t.r == 1) {
      matched++;
    }
    if (c % 4 == 0 && !bad) {
      std::string io = iter_ours(pat, icase, text), it = iter_theirs(re, text);
      iters++;
      if (it != "skip" && io != it) {
        printf("FAIL iteration /%s/%s: ours %s, std::regex %s\n", pat.c_str(), icase ? "i" : "", io.c_str(), it.c_str());
        bad++;
      }
    }
    rx_free(p1);
    rx_free(p2);
  }
  printf("rxfuzz: %d cases, %d matched, %d iterations compared, %d over the backtracker budget, %d std::regex gave up, "
         "%d mismatches\n", cases, matched, iters, skipped_budget, skipped_std, bad);
  if (!bad) throughput();
  return bad ? 1 : 0;
}
