// pfuzz.cpp - Mac-only mutation fuzzer for the core parsers (T16): INI + config + session, .lng files, hex
// patterns and go-to expressions, regex compile + search, backup headers, detection + Document lines.
//   pfuzz [seconds] [seed] [crash-dir]      default 3 s (the `make -C tests mac` run), seed 1, crash-dir tmp
// No coverage feedback: inputs are mutated from seed corpora (the real .lng files, a generated settings file and
// session, regex and hex samples), and mutants that parse are added to the corpus now and then. Build it with
// -fsanitize=address,undefined for a real pass (see HANDOFF T16); a crash writes the input to
// <crash-dir>/pfuzz-<target>-<seed>.bin first (sanitizer death callback / signal handler). Replaying one:
//   pfuzz -replay <target> <file>
// Inputs that crashed once are kept in tests/data/fuzz/ and replayed on every run.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <csignal>
#include <string>
#include <vector>
#include "etypes.h"
#include "estr.h"
#include "ini.h"
#include "config.h"
#include "session.h"
#include "lexer.h"
#include "hexfmt.h"
#include "regex.h"
#include "search.h"
#include "codec.h"
#include "detect.h"
#include "doc.h"
#include "fileops.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#include <sanitizer/common_interface_defs.h>
#define PF_SAN 1
#endif
#endif

typedef std::vector<unsigned char> Buf;

static unsigned g_rng = 1;
static unsigned rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static unsigned below(unsigned n) { return n ? rnd() % n : 0; }

enum { T_INI, T_LNG, T_HEX, T_RX, T_BAK, T_DOC, T_COUNT };
static const char* const k_tname[T_COUNT] = { "ini", "lng", "hex", "rx", "bak", "doc" };

static Buf g_cur;                     // the input being run (written out on a crash)
static int g_cur_target = -1;
static std::string g_crash_dir = "tmp";
static unsigned g_seed = 1;

static void save_crash()
{
  char path[600];
  FILE* f;
  if (g_cur_target < 0) return;
  snprintf(path, sizeof(path), "%s/pfuzz-%s-%u.bin", g_crash_dir.c_str(), k_tname[g_cur_target], g_seed);
  f = fopen(path, "wb");
  if (f) { if (!g_cur.empty()) fwrite(&g_cur[0], 1, g_cur.size(), f); fclose(f); }
  fprintf(stderr, "pfuzz: crashing input (target %s, %u bytes) saved to %s\n", k_tname[g_cur_target], (unsigned)g_cur.size(), path);
}
static void on_signal(int sig) { save_crash(); signal(sig, SIG_DFL); raise(sig); }

static bool read_file(const std::string& p, Buf* out)
{
  FILE* f = fopen(p.c_str(), "rb");
  long n;
  if (!f) return false;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  out->resize((size_t)(n > 0 ? n : 0));
  if (n > 0 && fread(&(*out)[0], 1, (size_t)n, f) != (size_t)n) { fclose(f); return false; }
  fclose(f);
  return true;
}

static void to_ecp(const Buf& b, Vec<ecp>* out)
{
  out->clear();
  if (!b.empty()) es_utf8_to_ecp((const char*)&b[0], (u32)b.size(), out);
}

// ---------------------------------------------------------------------------------------------------------------
// Targets: each returns normally for any input (crashes and sanitizer reports are the failures); invariant
// violations print and count.

static int g_viol = 0;
static void viol(const char* what) { if (g_viol < 20) fprintf(stderr, "pfuzz: invariant: %s (target %s)\n", what, k_tname[g_cur_target]); g_viol++; }

static void run_ini(const Buf& b)
{
  Ini ini, i2;
  Config c;
  Session s;
  EStr w1, w2;
  u32 seq = 0;
  int rc;
  const char* p = b.empty() ? "" : (const char*)&b[0];
  ini.parse(p, (u32)b.size());
  config_load(&c, ini);
  if (c.font_pt < 6 || c.font_pt > 72 || !memchr(c.font_face, 0, sizeof(c.font_face))) viol("config range");
  config_store(&c, &ini);
  ini.write(&w1);
  i2.parse(w1.c_str(), w1.len());
  i2.write(&w2);
  if (strcmp(w1.c_str(), w2.c_str()) != 0) viol("ini write not stable");
  session_init(&s);
  {
    Ini i3;
    i3.parse(p, (u32)b.size());
    rc = session_from_ini(i3, &s);
    if (rc == E_OK) {
      Ini i4;
      EStr w3;
      u32 k;
      for (k = 0; k < s.tabs.size(); k++) if (s.tabs[k]->tab_width < 1 || s.tabs[k]->tab_width > 16 || s.tabs[k]->id == 0) viol("session tab");
      session_to_ini(s, &i4);
      i4.write(&w3);
    } else if (s.tabs.size()) viol("session error with tabs");
    if ((session_validate(b.empty() ? 0 : &b[0], (u32)b.size(), &seq) == E_OK) != (rc == E_OK)) viol("session_validate");
  }
  session_free(&s);
}

static void run_lng(const Buf& b)
{
  LangInfo li;
  EStr err;
  int rc = lang_parse(b.empty() ? "" : (const char*)&b[0], (u32)b.size(), &li, &err);
  if (rc == E_OK && li.lexer) {
    static const char sample[] = "int x = 0x1F; /* c */ // l\n\"s\\\"\" 'c' <a href=\"x\"> # pre ;;; --> ``` [x](y)";
    Vec<ecp> s;
    std::vector<u8> st;
    u16 state = 0;
    u32 k;
    es_utf8_to_ecp(sample, (u32)strlen(sample), &s);
    st.resize(s.size() + 1);
    for (k = 0; k < 4; k++) {
      state = li.lexer->lex_line(s.data(), s.size(), state, &st[0]);
      if (state >= 0x7FFF) { viol("lexer state"); break; }
    }
  } else if (rc != E_OK && err.len() == 0) viol("lng error without message");
}

static void run_hex(const Buf& b)
{
  std::string t(b.begin(), b.end());
  EBytes out, mask;
  Vec<ecp> e;
  epos off = 0;
  u32 line = 0, col = 0, size = (u32)(t.size() * 977 + 13);
  char err[128];
  int rc;
  for (size_t i = 0; i < t.size(); i++) e.push((u8)t[i]);
  rc = hex_parse_bytes(t.c_str(), (u32)t.size(), &out);
  if (rc == E_OK && out.size() == 0) viol("hex bytes empty");
  rc = hex_parse_pattern(e.data(), e.size(), &out, &mask);
  if (rc == E_OK && out.size() != mask.size()) viol("hex mask size");
  rc = goto_parse_offset(t.c_str(), size / 2, size, &off, err, sizeof(err));
  if (rc == E_OK && off > size) viol("goto offset range");
  rc = goto_parse_line(t.c_str(), 3, 500, &line, &col, err, sizeof(err));
  if (rc == E_OK && line >= 500) viol("goto line range");
}

static void run_rx(const Buf& b)
{
  // first byte: flags; up to the first 0x01: pattern (UTF-8); the rest: subject
  size_t cut = b.empty() ? 0 : 1;
  while (cut < b.size() && b[cut] != 0x01) cut++;
  Buf pb(b.begin() + (b.empty() ? 0 : 1), b.begin() + cut), sb;
  if (cut < b.size()) sb.assign(b.begin() + cut + 1, b.end());
  Vec<ecp> pat;
  to_ecp(pb, &pat);
  unsigned fl = b.empty() ? 0 : b[0];
  Searcher s;
  SearchOpts o;
  SearchError err;
  o.mode = (u8)(fl & 3); o.match_case = (u8)((fl >> 2) & 1); o.whole_word = (u8)((fl >> 3) & 1); o.dot_all = (u8)((fl >> 4) & 1);
  EncSpec enc = (fl & 0x20) ? enc_utf16le(0) : enc_utf8(0);
  if (s.compile(pat.data(), pat.size(), o, enc, &err) != E_OK) {
    if (!memchr(err.msg, 0, sizeof(err.msg))) viol("rx message");
    return;
  }
  MemSource src;
  if (enc.kind == EK_UTF16LE && (sb.size() & 1)) sb.push_back(0);
  src.copy_from(sb.empty() ? (const u8*)"" : &sb[0], (u32)sb.size());
  const Codec* c = codec_get(enc);
  u32 cnt = 0;
  Vec<MarkRange> all;
  RxMatch m;
  int rc = s.count(&src, c, 0, src.size(), &cnt, 0);
  if (rc == E_BUDGET || rc < 0) return;
  rc = s.find_all(&src, c, 0, src.size(), &all, 100000, 0);
  if (rc >= 0 && all.size() != cnt) viol("find_all vs count");
  rc = s.find(&src, c, 0, src.size(), src.size(), 1, &m, 0);
  if (rc == 1 && m.pos + m.len > src.size()) viol("backward range");
  if (cnt) {
    static const char rep[] = "<$0|$1|\\U$2\\E|${3}|$$|\\x{1F600}>";
    Vec<ecp> r;
    EBytes out;
    u32 unrep = 0;
    es_utf8_to_ecp(rep, (u32)strlen(rep), &r);
    if (s.find(&src, c, 0, src.size(), 0, 0, &m, 0) == 1) s.expand(r.data(), r.size(), &src, c, m, &out, &unrep);
  }
}

static void run_bak(const Buf& b)
{
  BackupMeta m;
  u32 hl = 0;
  epos dl = 0;
  int rc = backup_header_parse(b.empty() ? (const u8*)"" : &b[0], (u32)b.size(), &m, &hl, &dl);
  if (rc == E_OK) {
    EStr h;
    if (hl > b.size() || !memchr(m.path, 0, sizeof(m.path)) || !memchr(m.lang, 0, sizeof(m.lang))) viol("backup header");
    backup_header_write(m, dl, &h);
  }
}

static void run_doc(const Buf& b)
{
  DetectOpts o;
  DetectResult r;
  Document d;
  MemSource* src = new MemSource();
  o.utf8_nobom = 1; o.utf16_nobom = 1; o.pad[0] = o.pad[1] = 0; o.ansi = enc_ansi(); o.default_eol = EOL_CRLF;
  detect(b.empty() ? (const u8*)"" : &b[0], (u32)b.size(), 1, o, &r);
  src->copy_from(b.empty() ? (const u8*)"" : &b[0], (u32)b.size());
  if (d.init_source(src, r.enc, r.bom_len, r.eol_style, 0) != E_OK) return;
  u32 lines = d.line_count(), l;
  epos prev = 0;
  for (l = 0; l < lines && l < 200; l++) {
    epos a = d.line_start(l), e = d.line_end(l), n = d.line_next(l);
    if (a != prev || a > e || e > n) { viol("doc lines"); break; }
    prev = n;
  }
  epos pos = d.text_origin();
  u32 steps = 0;
  while (pos < d.length() && steps++ <= d.length()) pos = d.char_next(pos);
  if (pos != d.length()) viol("char walk");
}

static void run_target(int t, const Buf& b)
{
  g_cur = b;
  g_cur_target = t;
  switch (t) {
    case T_INI: run_ini(b); break;
    case T_LNG: run_lng(b); break;
    case T_HEX: run_hex(b); break;
    case T_RX: run_rx(b); break;
    case T_BAK: run_bak(b); break;
    default: run_doc(b); break;
  }
  g_cur_target = -1;
}

// ---------------------------------------------------------------------------------------------------------------
// Corpus and mutations

static const char* const k_dict[] = {
  "[", "]", "=", "\r\n", "\n", "\r", "\0", ";", "#", "\"", "[Session]", "[end]", "ok=1", "count=", "version=1",
  "[Tab0]", "id=", "path=", "untitled=1", "bookmarks=", "keywords1=", "comment=", "block_comment=", "string=",
  "number=", "\\s", "\\t", "\\", "(", ")", "*", "+", "?", "{", "}", "{2,", "|", "^", "$", "\\b", "\\1", "(?=", "(?i)",
  "[^", "-", "0x", "??", "?", "h", "end", "#", "%", "+", ":", "@DATA ", "PICABAK1\r\n", "tabid=", "gen=",
  "\xEF\xBB\xBF", "\xFF\xFE", "\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "\xED\xA0\x80", "\xC0\x80",
  "4294967295", "4294967296", "99999999999", "-1", "0", "\t", "        " };

static void mutate(Buf* b, const std::vector<Buf>& corpus)
{
  unsigned n = 1 + below(6), k;
  for (k = 0; k < n; k++) {
    unsigned op = below(8);
    size_t at = below((unsigned)b->size() + 1);
    switch (op) {
      case 0: if (!b->empty()) (*b)[below((unsigned)b->size())] ^= (unsigned char)(1u << below(8)); break;
      case 1: if (!b->empty()) (*b)[below((unsigned)b->size())] = (unsigned char)rnd(); break;
      case 2: { const char* d = k_dict[below(sizeof(k_dict) / sizeof(k_dict[0]))]; size_t l = d[0] ? strlen(d) : 1; b->insert(b->begin() + at, d, d + l); break; }
      case 3: if (!b->empty()) { size_t l = 1 + below(at < b->size() ? 64 : 1); if (at + l > b->size()) l = b->size() - at; b->erase(b->begin() + at, b->begin() + at + l); } break;
      case 4: if (!b->empty()) { size_t s = below((unsigned)b->size()), l = 1 + below(128); if (s + l > b->size()) l = b->size() - s; Buf cp(b->begin() + s, b->begin() + s + l); b->insert(b->begin() + at, cp.begin(), cp.end()); } break;
      case 5: if (!corpus.empty()) { const Buf& o = corpus[below((unsigned)corpus.size())]; if (!o.empty()) { size_t s = below((unsigned)o.size()), l = 1 + below(256); if (s + l > o.size()) l = o.size() - s; b->insert(b->begin() + at, o.begin() + s, o.begin() + s + l); } } break;
      case 6: if (b->size() > 1) b->resize(below((unsigned)b->size())); break;     // truncation
      default: { unsigned char c = (unsigned char)"0123456789\r\n=[]"[below(16)]; b->insert(b->begin() + at, c); break; }
    }
    if (b->size() > 200000) b->resize(200000);
  }
}

static void add_str(std::vector<Buf>* c, const char* s, size_t n) { c->push_back(Buf(s, s + n)); }

static void seeds(std::vector<Buf>* c, const std::string& root)
{
  static const char* const lng[] = { "cpp", "rc", "make", "bat", "ini", "asm", "python", "html", "js", "css", "xml", "md", "diff" };
  for (int t = 0; t < T_COUNT; t++) c[t].clear();
  for (unsigned i = 0; i < 13; i++) {
    Buf b;
    if (read_file(root + "/../src/langs/" + lng[i] + ".lng", &b)) c[T_LNG].push_back(b);
  }
  {
    Config cf;
    Ini ini;
    EStr t;
    config_defaults(&cf);
    config_mru_add(&cf, "C:\\a b\\x.txt");
    config_hist_add(cf.find_hist, CFG_HIST_MAX, "find me");
    config_store(&cf, &ini);
    ini.write(&t);
    add_str(&c[T_INI], t.c_str(), t.len());
  }
  {
    Session s;
    Ini ini;
    EStr t;
    session_init(&s);
    for (unsigned i = 0; i < 3; i++) {
      SessionTab* tab = new SessionTab;
      memset((void*)tab, 0, (size_t)((char*)&tab->bookmarks - (char*)tab));
      tab->id = i + 1; tab->tab_width = 4; tab->enc = enc_utf8(0); tab->untitled = (u8)(i == 2); tab->untitled_no = 1;
      if (i < 2) snprintf(tab->path, sizeof(tab->path), "C:\\f%u.txt", i);
      tab->bookmarks.push(i + 3);
      s.tabs.push(tab);
    }
    s.next_tabid = 4; s.next_untitled = 2;
    session_to_ini(s, &ini);
    ini.write(&t);
    add_str(&c[T_INI], t.c_str(), t.len());
    session_free(&s);
  }
  {
    static const char* const hx[] = { "4D 5A 90 00", "0x4D,0x5A", "DE AD ?? EF", "4?", "end-10", "#123", "50%", "+0x10",
                                      "120:5", "end", ":7", "-5", "FFh 0Ah" };
    for (unsigned i = 0; i < 13; i++) add_str(&c[T_HEX], hx[i], strlen(hx[i]));
  }
  {
    static const char* const rx[] = { "\x02(a|b)*c\x01" "xxabcab\nc", "\x02^\\s*#\\w+\x01#include\n  #define x",
                                      "\x06(?i)caf\xC3\xA9\x01" "CAF\xC3\x89 cafe", "\x02(\\w)\\1\x01" "aabbcd", "\x00" "foo\x01" "a foo b",
                                      "\x01" "a\\tb\x01" "a\tb", "\x03" "DE ?? 0A\x01\xDE\xAD\x0A", "\x22" "a.c\x01" "a\0b\0c\0",
                                      "\x12(?=a)a*?$\x01" "aaa\r\naa", "\x0A\\bword\\b\x01" "a word, words" };
    for (unsigned i = 0; i < 10; i++) add_str(&c[T_RX], rx[i], strlen(rx[i] + 1) + 1);
  }
  {
    BackupMeta m;
    EStr h;
    memset(&m, 0, sizeof(m));
    m.tabid = 5; m.gen = 2; m.seq = 9; strcpy(m.path, "C:\\x.txt"); m.enc = enc_utf16le(1); strcpy(m.lang, "cpp");
    backup_header_write(m, 1234, &h);
    add_str(&c[T_BAK], h.c_str(), h.len());
  }
  {
    static const char d1[] = "line one\r\nline two\nthree\rfour\r\n\r\n";
    static const char d2[] = "\xFF\xFEh\0i\0\r\0\n\0\x3D\xD8\0";
    static const char d3[] = "\xEF\xBB\xBF\xC3\xA9t\xC3\xA9\n\xE2\x82\n\xF0\x9F\x98\x80";
    add_str(&c[T_DOC], d1, sizeof(d1) - 1); add_str(&c[T_DOC], d2, sizeof(d2) - 1); add_str(&c[T_DOC], d3, sizeof(d3) - 1);
  }
}

static int replay_dir(const std::string& root)
{
  // tests/data/fuzz/<target>-*.bin: every file is run once (regressions of crashes found earlier)
  int n = 0;
  for (int t = 0; t < T_COUNT; t++) {
    for (int i = 0; i < 100; i++) {
      char p[600];
      Buf b;
      snprintf(p, sizeof(p), "%s/data/fuzz/%s-%d.bin", root.c_str(), k_tname[t], i);
      if (!read_file(p, &b)) continue;
      run_target(t, b);
      n++;
    }
  }
  return n;
}

int main(int argc, char** argv)
{
  double secs = 3;
  std::vector<Buf> corpus[T_COUNT];
  unsigned long iters = 0, per[T_COUNT] = { 0 };
  clock_t c0;
  std::string root = ".";                   // run from tests/ (make -C tests mac)
  EStr lerr;
  enc_set_system(1252, 437);
  codec_init();
  signal(SIGSEGV, on_signal); signal(SIGBUS, on_signal); signal(SIGABRT, on_signal); signal(SIGFPE, on_signal);
#ifdef PF_SAN
  __sanitizer_set_death_callback(save_crash);
#endif
  if (argc > 3 && strcmp(argv[1], "-replay") == 0) {
    Buf b;
    int t;
    for (t = 0; t < T_COUNT; t++) if (strcmp(argv[2], k_tname[t]) == 0) break;
    if (t == T_COUNT || !read_file(argv[3], &b)) { fprintf(stderr, "pfuzz: bad replay arguments\n"); return 2; }
    run_target(t, b);
    printf("pfuzz: replay %s %s: ok (%d invariant violations)\n", argv[2], argv[3], g_viol);
    return g_viol ? 1 : 0;
  }
  if (argc > 1) secs = atof(argv[1]);
  if (argc > 2) g_seed = (unsigned)strtoul(argv[2], 0, 10);
  if (argc > 3) g_crash_dir = argv[3];
  g_rng = g_seed * 2654435761u + 1;
  seeds(corpus, root);
  if (corpus[T_LNG].size() != 13) { fprintf(stderr, "pfuzz: run from tests/ (needs ../src/langs)\n"); return 2; }
  printf("pfuzz: replayed %d saved inputs\n", replay_dir(root));
  fflush(stdout);
  c0 = clock();
  while ((double)(clock() - c0) < secs * CLOCKS_PER_SEC) {
    int t = (int)below(T_COUNT);
    std::vector<Buf>& c = corpus[t];
    for (int k = 0; k < 64; k++) {
      Buf b = c[below((unsigned)c.size())];
      mutate(&b, c);
      run_target(t, b);
      per[t]++;
      iters++;
      if (below(200) == 0 && c.size() < 400) c.push_back(b);   // keep some mutants for diversity
    }
  }
  printf("pfuzz: %lu inputs in %.0f s (ini %lu, lng %lu, hex %lu, rx %lu, bak %lu, doc %lu), %d invariant violations\n",
         iters, secs, per[0], per[1], per[2], per[3], per[4], per[5], g_viol);
  return g_viol ? 1 : 0;
}
