// t_search.cpp - suite "search" (T03). Classes here carry a Srch prefix: coretest links every suite into one binary,
// and same-named classes in two test files (inline methods, vtables) collide silently.
// Covers: Searcher modes, extended/hex parsing, whole word, impossible patterns,
// multi-span sources and encodings (UTF-8, cp1252, UTF-16 LE/BE fixtures + T02's codecs when present, a fake DBCS),
// backward search vs the forward iteration, Replace All vs a naive reference, empty matches, cancel, OOM, throughput.
#include "tf.h"
#include "search.h"
#include "regex_int.h"
#include "codec.h"
#include "ucd.h"
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------------------------------------------
// Fixtures

// Pieces of random sizes; every get() returns a fresh copy of the rest of the piece and poisons the previous one.
class SrchChunkSource : public ByteSource {
public:
  SrchChunkSource(const u8* data, epos n, TF* tf, u32 maxpiece) : d_(data), n_(n), buf_(0), bufn_(0)
  {
    epos at = 0;
    while (at < n) {
      u32 len = tf_rand_below(tf, 4) == 0 ? 1 : 1 + tf_rand_below(tf, maxpiece);
      cuts_.push(at);
      at += len;
    }
    cuts_.push(n);
  }
  ~SrchChunkSource() { if (buf_) e_free(buf_); }
  epos size() const { return n_; }
  const u8* get(epos off, epos* avail)
  {
    u32 a = 0, b = cuts_.size() - 1;
    epos end, len;
    if (off >= n_) { *avail = 0; return 0; }
    while (b - a > 1) {               // cuts_[a] <= off < cuts_[b]
      u32 mid = (a + b) / 2;
      if (cuts_[mid] <= off) a = mid; else b = mid;
    }
    end = cuts_[b];
    len = end - off;
    if (buf_) { memset(buf_, 0xEE, bufn_); e_free(buf_); }
    buf_ = (u8*)e_alloc(len);
    bufn_ = len;
    if (!buf_) { *avail = 0; return 0; }
    memcpy(buf_, d_ + off, len);
    *avail = len;
    return buf_;
  }
private:
  const u8* d_; epos n_; Vec<epos> cuts_; u8* buf_; epos bufn_;
};

class SrchU16Codec : public Codec {        // UTF-16 fixture (T02 owns the real one)
public:
  SrchU16Codec(int be) { kind = (u8)(be ? EK_UTF16BE : EK_UTF16LE); unit = 2; max_len = 4; ascii_compat = 0; self_sync = 1;
                    pad = 0; cp = (u16)(be ? 1201 : 1200); }
  u32 unit_at(const u8* p) const { return kind == EK_UTF16BE ? ((u32)p[0] << 8 | p[1]) : ((u32)p[1] << 8 | p[0]); }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u32 u;
    if (avail < 2) { if (!eof) return 0; *out = ECP_BADBYTE(p[0]); return 1; }
    u = unit_at(p);
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (avail < 4) { if (!eof) return 0; *out = ECP_BADUNIT(u); return 2; }
      {
        u32 lo = unit_at(p + 2);
        if (lo >= 0xDC00 && lo <= 0xDFFF) { *out = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); return 4; }
      }
      *out = ECP_BADUNIT(u);
      return 2;
    }
    if (u >= 0xDC00 && u <= 0xDFFF) { *out = ECP_BADUNIT(u); return 2; }
    *out = u;
    return 2;
  }
  void put(u32 u, u8* o) const
  {
    if (kind == EK_UTF16BE) { o[0] = (u8)(u >> 8); o[1] = (u8)u; } else { o[0] = (u8)u; o[1] = (u8)(u >> 8); }
  }
  u32 encode(ecp c, u8* out) const
  {
    if (c >= 0x120000 && c <= 0x12FFFF) { put(c - 0x120000, out); return 2; }
    if (c >= 0x110000 && c <= 0x1100FF) { out[0] = (u8)(c - 0x110000); return 1; }
    if (c >= 0xD800 && c <= 0xDFFF) return 0;
    if (c < 0x10000) { put(c, out); return 2; }
    if (c <= 0x10FFFF) { c -= 0x10000; put(0xD800 + (c >> 10), out); put(0xDC00 + (c & 0x3FF), out + 2); return 4; }
    return 0;
  }
};

// Fake DBCS: lead bytes 0x81-0x9F, trail 0x40-0xFC (not 0x7F) -> U+4E00 + (lead - 0x81) * 189 + (trail - 0x40).
class SrchDbcsCodec : public Codec {
public:
  SrchDbcsCodec() { kind = EK_DBCS; unit = 1; max_len = 2; ascii_compat = 1; self_sync = 0; pad = 0; cp = 932; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u8 b = p[0];
    if (b < 0x80) { *out = b; return 1; }
    if (b >= 0x81 && b <= 0x9F) {
      if (avail < 2) { if (!eof) return 0; *out = ECP_BADBYTE(b); return 1; }
      if (p[1] >= 0x40 && p[1] <= 0xFC && p[1] != 0x7F) { *out = 0x4E00 + (b - 0x81) * 189 + (p[1] - 0x40); return 2; }
      *out = ECP_BADBYTE(b);
      return 1;
    }
    *out = ECP_BADBYTE(b);
    return 1;
  }
  u32 encode(ecp c, u8* out) const
  {
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c >= 0x110000 && c <= 0x1100FF) { out[0] = (u8)(c - 0x110000); return 1; }
    if (c >= 0x4E00 && c < 0x4E00 + 31 * 189) {
      u32 k = c - 0x4E00, tr = 0x40 + k % 189;
      if (tr == 0x7F) return 0;
      out[0] = (u8)(0x81 + k / 189);
      out[1] = (u8)tr;
      return 2;
    }
    return 0;
  }
};

static void to_ecp(const char* s, Vec<ecp>* out)
{
  out->clear();
  es_utf8_to_ecp(s, es_len(s), out);
}

static SearchOpts mk_opts(u8 mode, u8 match_case, u8 whole_word)
{
  SearchOpts o;
  o.mode = mode; o.match_case = match_case; o.whole_word = whole_word; o.dot_all = 0;
  return o;
}

static int compile_s(Searcher* s, const char* pat, u8 mode, u8 mc, u8 ww, EncSpec e)
{
  Vec<ecp> p;
  SearchError err;
  to_ecp(pat, &p);
  return s->compile(p.data(), p.size(), mk_opts(mode, mc, ww), e, &err);
}

// all matches as "s,e;" (byte offsets)
static void all_str(Searcher* s, ByteSource* src, const Codec* c, epos lo, epos hi, EStr* out)
{
  Vec<MarkRange> v;
  u32 k;
  int r = s->find_all(src, c, lo, hi, &v, 100000, 0);
  out->clear();
  if (r < 0) { out->appendf("E%d", r); return; }
  for (k = 0; k < v.size(); k++) out->appendf("%u,%u;", v[k].pos, v[k].pos + v[k].len);
}

static void flat_all(TF* tf, const char* pat, u8 mode, u8 mc, u8 ww, const char* text, const char* expect)
{
  Searcher s;
  EStr got;
  ViewSource src((const u8*)text, es_len(text));
  int r = compile_s(&s, pat, mode, mc, ww, enc_utf8(0));
  if (r != E_OK) { tf_fail(tf, __FILE__, __LINE__, "compile /%s/ failed: %s", pat, e_errname(r)); return; }
  all_str(&s, &src, codec_get(enc_utf8(0)), 0, es_len(text), &got);
  if (strcmp(got.c_str(), expect) != 0)
    tf_fail(tf, __FILE__, __LINE__, "/%s/ mode %u case %u word %u on \"%s\": got %s, expected %s", pat, mode, mc, ww, text,
            got.c_str(), expect);
  else tf_check(tf, 1, __FILE__, __LINE__, "all");
}

// ---------------------------------------------------------------------------------------------------------------
// Unit tests

static void test_extended(TF* tf)
{
  Vec<ecp> in, out;
  SearchError err;
  to_ecp("a\\nb\\r\\t\\0\\\\\\x41\\u00e4\\d065\\u20AC\\uD83D\\uDE00", &in);
  TF_EQ_I(search_parse_extended(in.data(), in.size(), &out, &err), E_OK);
  TF_EQ_U(out.size(), 12);
  if (out.size() == 12) {
    TF_EQ_U(out[0], 'a'); TF_EQ_U(out[1], 10); TF_EQ_U(out[2], 'b'); TF_EQ_U(out[3], 13); TF_EQ_U(out[4], 9);
    TF_EQ_U(out[5], 0); TF_EQ_U(out[6], '\\'); TF_EQ_U(out[7], 'A'); TF_EQ_U(out[8], 0xE4); TF_EQ_U(out[9], 'A');
    TF_EQ_U(out[10], 0x20AC); TF_EQ_U(out[11], 0x1F600);
  }
  {
    static const struct { const char* s; u32 pos; } errs[] = {
      { "\\q", 0 }, { "ab\\", 2 }, { "\\x4", 0 }, { "x\\xZZ", 1 }, { "\\d256", 0 }, { "\\d12", 0 }, { "\\uD800", 0 },
      { "\\u12", 0 }, { "ab\\.", 2 }
    };
    u32 i;
    for (i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
      to_ecp(errs[i].s, &in);
      TF_EQ_I(search_parse_extended(in.data(), in.size(), &out, &err), E_SYNTAX);
      TF_EQ_U(err.pos, errs[i].pos);
      TF_CHECK(err.msg[0] != 0);
    }
  }
}

static void test_modes(TF* tf)
{
  // Normal
  flat_all(tf, "abc", SM_NORMAL, 1, 0, "xxabcabc", "2,5;5,8;");
  flat_all(tf, "ABC", SM_NORMAL, 0, 0, "xAbCabc", "1,4;4,7;");
  flat_all(tf, "ABC", SM_NORMAL, 1, 0, "xAbCabc", "");
  flat_all(tf, "a.b", SM_NORMAL, 1, 0, "a.b axb", "0,3;");
  flat_all(tf, "(a", SM_NORMAL, 1, 0, "(a (a", "0,2;3,5;");
  flat_all(tf, "aa", SM_NORMAL, 1, 0, "aaaaa", "0,2;2,4;");
  flat_all(tf, "\xc3\xa4", SM_NORMAL, 0, 0, "\xc3\x84x\xc3\xa4", "0,2;3,5;");
  flat_all(tf, "$1\\n", SM_NORMAL, 1, 0, "x$1\\n", "1,5;");
  // whole word (not preceded/followed by a word character)
  flat_all(tf, "ab", SM_NORMAL, 1, 1, "ab cab ab_ ab", "0,2;11,13;");
  flat_all(tf, "(ab", SM_NORMAL, 1, 1, "x(ab y (ab", "7,10;");
  flat_all(tf, "ab", SM_NORMAL, 0, 1, "AB\xc3\xa4" "ab Ab", "7,9;");
  flat_all(tf, "a", SM_NORMAL, 1, 1, "a a\na", "0,1;2,3;4,5;");
  // Extended
  flat_all(tf, "a\\tb", SM_EXTENDED, 1, 0, "a\tb ab", "0,3;");
  flat_all(tf, "\\r\\n", SM_EXTENDED, 1, 0, "a\r\nb\r\n", "1,3;4,6;");
  flat_all(tf, "\\x41", SM_EXTENDED, 0, 0, "a A", "0,1;2,3;");
  // Regex
  flat_all(tf, "a+", SM_REGEX, 1, 0, "baaa a", "1,4;5,6;");
  flat_all(tf, "\\w+", SM_REGEX, 1, 1, "ab cd", "0,2;3,5;");
  flat_all(tf, "b", SM_REGEX, 1, 1, "ab b", "3,4;");
  flat_all(tf, "x*", SM_REGEX, 1, 0, "abc", "0,0;1,1;2,2;3,3;");
  flat_all(tf, "a*", SM_REGEX, 1, 0, "baaac", "0,0;1,4;4,4;5,5;");
  flat_all(tf, "^", SM_REGEX, 1, 0, "a\nb\r\nc", "0,0;2,2;5,5;");
  flat_all(tf, "$", SM_REGEX, 1, 0, "a\r\nb", "1,1;4,4;");
  flat_all(tf, "^$", SM_REGEX, 1, 0, "a\n\n\nb", "2,2;3,3;");
  flat_all(tf, "\\b", SM_REGEX, 1, 0, "ab c", "0,0;2,2;3,3;4,4;");
  flat_all(tf, "(?i)A", SM_REGEX, 1, 0, "aA", "0,1;1,2;");
  flat_all(tf, ".", SM_REGEX, 1, 0, "a\xc3\xa4\n", "0,1;1,3;");
  flat_all(tf, "a", SM_REGEX, 0, 0, "A a", "0,1;2,3;");
  // Hex
  {
    static const u8 bin[] = { 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0x00, 0xEF, 0x41, 0x42 };
    Searcher s;
    EStr got;
    ViewSource src(bin, sizeof(bin));
    RxMatch m;
    TF_EQ_I(compile_s(&s, "DE AD ?? EF", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "1,5;5,9;");
    TF_EQ_I(compile_s(&s, "dead", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "1,3;5,7;");
    TF_EQ_I(compile_s(&s, "0x41,0x42", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "9,11;");
    TF_EQ_I(compile_s(&s, "?? ??", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    TF_EQ_I(s.find(&src, 0, 0, sizeof(bin), 3, 0, &m, 0), 1);
    TF_EQ_U(m.pos, 3);
    TF_EQ_I(s.find(&src, 0, 0, sizeof(bin), 10, 0, &m, 0), 0);
    TF_EQ_I(compile_s(&s, "DE AD", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    TF_EQ_I(s.find(&src, 0, 0, sizeof(bin), 11, 1, &m, 0), 1);
    TF_EQ_U(m.pos, 5);
    TF_EQ_I(s.find(&src, 0, 0, sizeof(bin), 5, 1, &m, 0), 1);
    TF_EQ_U(m.pos, 1);
    TF_EQ_I(s.find(&src, 0, 0, sizeof(bin), 1, 1, &m, 0), 0);
    TF_EQ_I(s.find(&src, 0, 0, sizeof(bin), 6, 1, &m, 0), 1);   // [5,7) ends after 6: not before the position
    TF_EQ_U(m.pos, 1);
    TF_EQ_I(compile_s(&s, "4? 42", SM_HEX, 1, 0, enc_utf8(0)), E_OK);     // nibble wildcard
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "9,11;");
    TF_EQ_I(compile_s(&s, "?D A?", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "");
    TF_EQ_I(compile_s(&s, "?E A?", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "1,3;5,7;");
    TF_EQ_I(compile_s(&s, "D? ?D", SM_HEX, 1, 0, enc_utf8(0)), E_OK);
    all_str(&s, &src, 0, 0, sizeof(bin), &got);
    TF_EQ_S(got.c_str(), "1,3;5,7;");
    TF_EQ_I(compile_s(&s, "4", SM_HEX, 1, 0, enc_utf8(0)), E_SYNTAX);
    TF_EQ_I(compile_s(&s, "XY", SM_HEX, 1, 0, enc_utf8(0)), E_SYNTAX);
    TF_EQ_I(compile_s(&s, "?", SM_HEX, 1, 0, enc_utf8(0)), E_SYNTAX);
    TF_EQ_I(compile_s(&s, " ", SM_HEX, 1, 0, enc_utf8(0)), E_SYNTAX);
  }
  // errors and state
  {
    Searcher s;
    RxMatch m;
    SearchError err;
    Vec<ecp> p;
    ViewSource src((const u8*)"abc", 3);
    TF_EQ_I(s.find(&src, codec_get(enc_utf8(0)), 0, 3, 0, 0, &m, 0), E_STATE);
    TF_EQ_I(s.compile(p.data(), 0, mk_opts(SM_NORMAL, 1, 0), enc_utf8(0), &err), E_SYNTAX);
    TF_EQ_I(s.find(&src, codec_get(enc_utf8(0)), 0, 3, 0, 0, &m, 0), E_STATE);
    to_ecp("a(b", &p);
    TF_EQ_I(s.compile(p.data(), p.size(), mk_opts(SM_REGEX, 1, 0), enc_utf8(0), &err), E_SYNTAX);
    TF_EQ_U(err.pos, 1);
    TF_CHECK(err.msg[0] != 0);
    to_ecp("a\\q", &p);
    TF_EQ_I(s.compile(p.data(), p.size(), mk_opts(SM_EXTENDED, 1, 0), enc_utf8(0), &err), E_SYNTAX);
    TF_EQ_U(err.pos, 1);
    TF_EQ_I(s.compile(p.data(), p.size(), mk_opts(SM_NORMAL, 1, 0), enc_utf8(0), &err), E_OK);
    TF_EQ_U(s.opts().mode, SM_NORMAL);
    TF_EQ_I(s.compile(p.data(), p.size(), mk_opts(7, 1, 0), enc_utf8(0), &err), E_RANGE);
  }
}

static void test_find_basic(TF* tf)
{
  Searcher s;
  RxMatch m;
  const char* t = "xxabcabc";
  ViewSource src((const u8*)t, 8);
  const Codec* c = codec_get(enc_utf8(0));
  TF_EQ_I(compile_s(&s, "abc", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
  TF_EQ_I(s.find(&src, c, 0, 8, 0, 0, &m, 0), 1);  TF_EQ_U(m.pos, 2); TF_EQ_U(m.len, 3);
  TF_EQ_U(m.gpos[0], 2); TF_EQ_U(m.glen[0], 3); TF_EQ_U(m.gpos[1], EPOS_NONE);
  TF_EQ_I(s.find(&src, c, 0, 8, 3, 0, &m, 0), 1);  TF_EQ_U(m.pos, 5);
  TF_EQ_I(s.find(&src, c, 0, 8, 6, 0, &m, 0), 0);
  TF_EQ_I(s.find(&src, c, 0, 7, 3, 0, &m, 0), 0);  // must end <= hi
  TF_EQ_I(s.find(&src, c, 0, 8, 8, 1, &m, 0), 1);  TF_EQ_U(m.pos, 5);
  TF_EQ_I(s.find(&src, c, 0, 8, 5, 1, &m, 0), 1);  TF_EQ_U(m.pos, 2);
  TF_EQ_I(s.find(&src, c, 0, 8, 7, 1, &m, 0), 1);  TF_EQ_U(m.pos, 2);
  TF_EQ_I(s.find(&src, c, 0, 8, 2, 1, &m, 0), 0);
  TF_EQ_I(s.find(&src, c, 3, 8, 8, 1, &m, 0), 1);  TF_EQ_U(m.pos, 5);
  TF_EQ_I(s.find(&src, c, 3, 8, 7, 1, &m, 0), 0);
  TF_EQ_I(s.find(&src, c, 0, 100, 0, 0, &m, 0), 1); // hi clamped
  TF_EQ_I(s.find(&src, c, 5, 3, 0, 0, &m, 0), 0);
  TF_EQ_I(s.find(&src, 0, 0, 8, 0, 0, &m, 0), E_STATE);
  {
    u32 n = 0;
    TF_EQ_I(s.count(&src, c, 0, 8, &n, 0), E_OK);
    TF_EQ_U(n, 2);
    TF_EQ_I(s.count(&src, c, 3, 8, &n, 0), E_OK);
    TF_EQ_U(n, 1);
  }
  // backward with an empty-matching regex never returns the position itself
  TF_EQ_I(compile_s(&s, "^", SM_REGEX, 1, 0, enc_utf8(0)), E_OK);
  {
    ViewSource s2((const u8*)"ab\ncd\nef", 8);
    TF_EQ_I(s.find(&s2, c, 0, 8, 6, 1, &m, 0), 1);  TF_EQ_U(m.pos, 3);
    TF_EQ_I(s.find(&s2, c, 0, 8, 3, 1, &m, 0), 1);  TF_EQ_U(m.pos, 0);
    TF_EQ_I(s.find(&s2, c, 0, 8, 0, 1, &m, 0), 0);
    TF_EQ_I(s.find(&s2, c, 0, 8, 7, 0, &m, 0), 0);
    TF_EQ_I(s.find(&s2, c, 0, 8, 6, 0, &m, 0), 1);  TF_EQ_U(m.pos, 6);  TF_EQ_U(m.len, 0);
  }
  // find_all cap
  {
    Vec<MarkRange> v;
    TF_EQ_I(compile_s(&s, "a", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
    ViewSource s3((const u8*)"aaaa", 4);
    TF_EQ_I(s.find_all(&s3, c, 0, 4, &v, 2, 0), 1);
    TF_EQ_U(v.size(), 2);
    TF_EQ_I(s.find_all(&s3, c, 0, 4, &v, 4, 0), 1);
    TF_EQ_I(s.find_all(&s3, c, 0, 4, &v, 5, 0), E_OK);
    TF_EQ_U(v.size(), 4);
  }
}

static void test_impossible(TF* tf)
{
  Searcher s;
  RxMatch m;
  const Codec* c1252 = codec_get(enc_cp(1252));
  ViewSource src((const u8*)"o\xf6", 2);
  if (!c1252) { TF_CHECK(0); return; }
  TF_EQ_I(compile_s(&s, "\xc5\x91", SM_NORMAL, 1, 0, enc_cp(1252)), E_OK);   // U+0151 not in cp1252
  TF_EQ_I(s.impossible(), 1);
  TF_EQ_I(s.find(&src, c1252, 0, 2, 0, 0, &m, 0), 0);
  TF_EQ_I(compile_s(&s, "\xc5\x90", SM_NORMAL, 0, 0, enc_cp(1252)), E_OK);   // U+0150, any case
  TF_EQ_I(s.impossible(), 1);
  TF_EQ_I(compile_s(&s, "\xc3\xb6", SM_NORMAL, 1, 0, enc_cp(1252)), E_OK);   // U+00F6 is there
  TF_EQ_I(s.impossible(), 0);
  TF_EQ_I(s.find(&src, c1252, 0, 2, 0, 0, &m, 0), 1);
  TF_EQ_U(m.pos, 1);
  TF_EQ_I(compile_s(&s, "\xc3\x96", SM_NORMAL, 0, 0, enc_cp(1252)), E_OK);   // icase finds the lower case
  TF_EQ_I(s.find(&src, c1252, 0, 2, 0, 0, &m, 0), 1);
  TF_EQ_I(compile_s(&s, "x\\x{151}", SM_REGEX, 1, 0, enc_cp(1252)), E_OK);
  TF_EQ_I(s.impossible(), 1);
  TF_EQ_I(compile_s(&s, "(\\x{151}|o)", SM_REGEX, 1, 0, enc_cp(1252)), E_OK);
  TF_EQ_I(s.impossible(), 0);
  TF_EQ_I(compile_s(&s, "\xe2\x82\xac", SM_NORMAL, 1, 0, enc_cp(1252)), E_OK); // euro is 0x80
  TF_EQ_I(s.impossible(), 0);
  TF_EQ_I(compile_s(&s, "\xc5\x91", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
  TF_EQ_I(s.impossible(), 0);
}

// ---- replacement ----

static void expand_case(TF* tf, const char* pat, u8 mode, const char* text, const char* rep, const char* expect,
                        u32 exp_bad, EncSpec e)
{
  Searcher s;
  RxMatch m;
  Vec<ecp> r;
  EBytes out;
  u32 bad = 99;
  const Codec* c = codec_get(e);
  ViewSource src((const u8*)text, es_len(text));
  char got[256];
  if (compile_s(&s, pat, mode, 1, 0, e) != E_OK || !c) { tf_fail(tf, __FILE__, __LINE__, "expand /%s/: compile", pat); return; }
  if (s.find(&src, c, 0, es_len(text), 0, 0, &m, 0) != 1) { tf_fail(tf, __FILE__, __LINE__, "expand /%s/: no match", pat); return; }
  to_ecp(rep, &r);
  if (s.expand(r.data(), r.size(), &src, c, m, &out, &bad) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "expand %s failed", rep); return; }
  memcpy(got, out.data(), emin<u32>(out.size(), 255));
  got[emin<u32>(out.size(), 255)] = 0;
  if (strcmp(got, expect) != 0 || bad != exp_bad)
    tf_fail(tf, __FILE__, __LINE__, "expand /%s/ \"%s\": got \"%s\" (%u bad), expected \"%s\" (%u)", pat, rep, got, bad,
            expect, exp_bad);
  else tf_check(tf, 1, __FILE__, __LINE__, "expand");
}

static void test_expand(TF* tf)
{
  EncSpec u8e = enc_utf8(0);
  expand_case(tf, "(\\w+)@(\\w+)", SM_REGEX, "mail bob@site x", "$2 at $1", "site at bob", 0, u8e);
  expand_case(tf, "(\\w+)@(\\w+)", SM_REGEX, "bob@site", "\\U$1\\E-$2", "BOB-site", 0, u8e);
  expand_case(tf, "(\\w+)@(\\w+)", SM_REGEX, "bob@site", "\\u$1 \\l$0", "Bob bob@site", 0, u8e);
  expand_case(tf, "(\\w+)", SM_REGEX, "HeLLo", "\\L$0\\E \\U$0", "hello HELLO", 0, u8e);
  expand_case(tf, "(\\w+)", SM_REGEX, "hELLO", "\\u\\L$1", "Hello", 0, u8e);
  expand_case(tf, "(\\w+)", SM_REGEX, "ab", "$$1 $", "$1 $", 0, u8e);
  expand_case(tf, "(\\w)(\\w)", SM_REGEX, "ab", "${2}${1}${0}", "baab", 0, u8e);
  expand_case(tf, "(\\w)(\\w)", SM_REGEX, "ab", "${x}$9|$&|\\0", "${x}|ab|ab", 0, u8e);
  expand_case(tf, "(a)|(b)", SM_REGEX, "b", "[$1][$2]", "[][b]", 0, u8e);
  expand_case(tf, "a", SM_REGEX, "a", "\\n\\r\\t\\\\\\x41\\x{20AC}\\q\\", "\n\r\t\\A\xe2\x82\xac" "q\\", 0, u8e);
  expand_case(tf, "\xc3\xa4+", SM_REGEX, "\xc3\xa4\xc3\xa4", "\\U$0", "\xc3\x84\xc3\x84", 0, u8e);
  expand_case(tf, "a", SM_NORMAL, "a", "$0\\n", "$0\\n", 0, u8e);
  expand_case(tf, "a", SM_EXTENDED, "a", "x\\ty", "x\ty", 0, u8e);
  expand_case(tf, "a", SM_REGEX, "a", "\xc5\x91\xc3\xa4", "?\xe4", 1, enc_cp(1252));   // U+0151 -> ?
  expand_case(tf, "a", SM_NORMAL, "a", "\xe2\x82\xac", "\x80", 0, enc_cp(1252));
  // raw bytes of a group are kept exactly (invalid UTF-8 stays invalid)
  expand_case(tf, "a(.)b", SM_REGEX, "a\xff" "b", "<$1>", "<\xff>", 0, u8e);
  {
    Searcher s;
    RxMatch m;
    Vec<ecp> r;
    EBytes out;
    u32 bad;
    static const u8 bin[] = { 1, 2, 3 };
    ViewSource src(bin, 3);
    TF_EQ_I(compile_s(&s, "02", SM_HEX, 1, 0, u8e), E_OK);
    TF_EQ_I(s.find(&src, 0, 0, 3, 0, 0, &m, 0), 1);
    to_ecp("FF 00", &r);
    TF_EQ_I(s.expand(r.data(), r.size(), &src, 0, m, &out, &bad), E_OK);
    TF_EQ_U(out.size(), 2);
    TF_CHECK(out.size() == 2 && out[0] == 0xFF && out[1] == 0);
    to_ecp("F?", &r);
    TF_EQ_I(s.expand(r.data(), r.size(), &src, 0, m, &out, &bad), E_SYNTAX);
    TF_EQ_I(compile_s(&s, "a", SM_EXTENDED, 1, 0, u8e), E_OK);
    to_ecp("\\q", &r);
    TF_EQ_I(s.expand(r.data(), r.size(), &src, codec_get(u8e), m, &out, &bad), E_SYNTAX);
  }
}

class SrchBufSink : public EditSink {    // applies edits to a byte buffer (the model document)
public:
  EBytes b; u32 calls; int fail_at; epos last_pos;
  SrchBufSink() : calls(0), fail_at(-1), last_pos(EPOS_NONE) {}
  int apply(epos pos, epos del, const u8* ins, epos n)
  {
    EBytes nb;
    if ((int)calls == fail_at) return E_READONLY;
    calls++;
    if (pos > last_pos) return E_STATE;          // must come back to front
    last_pos = pos;
    if (pos > b.size() || del > b.size() - pos) return E_RANGE;
    if (pos && nb.append(b.data(), pos) != E_OK) return E_NOMEM;
    if (n && nb.append(ins, n) != E_OK) return E_NOMEM;
    if (b.size() - pos - del && nb.append(b.data() + pos + del, b.size() - pos - del) != E_OK) return E_NOMEM;
    b.swap(nb);
    return E_OK;
  }
};

class SrchCountSink : public EditSink {  // counts edits and checks they come back to front
public:
  u32 calls; epos last_pos; int order_bad;
  SrchCountSink() : calls(0), last_pos(EPOS_NONE), order_bad(0) {}
  int apply(epos pos, epos del, const u8* ins, epos n)
  {
    (void)del; (void)ins; (void)n;
    if (pos >= last_pos) order_bad = 1;
    last_pos = pos;
    calls++;
    return E_OK;
  }
};

static void replace_case(TF* tf, const char* pat, u8 mode, const char* text, const char* rep, const char* expect)
{
  Searcher s;
  SrchBufSink sink;
  Vec<ecp> r;
  u32 n = 0, bad = 0;
  int rc;
  ViewSource src((const u8*)text, es_len(text));
  char got[512];
  sink.b.append(text, es_len(text));
  if (compile_s(&s, pat, mode, 1, 0, enc_utf8(0)) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "replace /%s/", pat); return; }
  to_ecp(rep, &r);
  rc = search_replace_all(s, &src, codec_get(enc_utf8(0)), 0, es_len(text), r.data(), r.size(), &sink, 0, &n, &bad);
  memcpy(got, sink.b.data(), emin<u32>(sink.b.size(), 511));
  got[emin<u32>(sink.b.size(), 511)] = 0;
  if (rc != E_OK || strcmp(got, expect) != 0)
    tf_fail(tf, __FILE__, __LINE__, "replace /%s/ -> \"%s\" in \"%s\": got \"%s\" (%s), expected \"%s\"", pat, rep, text, got,
            e_errname(rc), expect);
  else tf_check(tf, 1, __FILE__, __LINE__, "replace");
}

static void test_replace_fixed(TF* tf)
{
  replace_case(tf, "^", SM_REGEX, "a\nb\r\nc", "> ", "> a\n> b\r\n> c");
  replace_case(tf, "$", SM_REGEX, "a\r\nb", ";", "a;\r\nb;");
  replace_case(tf, "a*", SM_REGEX, "baaac", "-", "-b--c-");
  replace_case(tf, "x*", SM_REGEX, "abc", "-", "-a-b-c-");
  replace_case(tf, "\\b", SM_REGEX, "ab cd", "|", "|ab| |cd|");
  replace_case(tf, "(\\w+) (\\w+)", SM_REGEX, "one two three four", "$2 $1", "two one four three");
  replace_case(tf, "o", SM_NORMAL, "foo boo", "0", "f00 b00");
  replace_case(tf, "o", SM_NORMAL, "foo", "", "f");
  replace_case(tf, "\\n", SM_EXTENDED, "a\nb\n", "\\r\\n", "a\r\nb\r\n");
  replace_case(tf, "\\w+", SM_REGEX, "hello world", "\\u$0", "Hello World");
  replace_case(tf, "^\\s+|\\s+$", SM_REGEX, "  ab  \n  cd ", "", "ab\ncd");
  replace_case(tf, "aa", SM_NORMAL, "aaaaa", "b", "bba");
  // sink failure, cancel, counts
  {
    Searcher s;
    SrchBufSink sink;
    Vec<ecp> r;
    u32 n = 0, bad = 0;
    const char* t = "a b a b";
    ViewSource src((const u8*)t, 7);
    sink.b.append(t, 7);
    TF_EQ_I(compile_s(&s, "a", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
    to_ecp("\xc5\x91", &r);
    sink.fail_at = 1;
    TF_EQ_I(search_replace_all(s, &src, codec_get(enc_utf8(0)), 0, 7, r.data(), r.size(), &sink, 0, &n, &bad), E_READONLY);
    TF_EQ_U(n, 1);
    TF_EQ_I(search_replace_all(s, &src, codec_get(enc_utf8(0)), 0, 7, r.data(), r.size(), 0, 0, &n, &bad), E_RANGE);
  }
  {
    Searcher s;
    SrchBufSink sink;
    Vec<ecp> r;
    u32 n = 0, bad = 0;
    const char* t = "a b a b";
    ViewSource src((const u8*)t, 7);
    const Codec* c1252 = codec_get(enc_cp(1252));
    sink.b.append(t, 7);
    TF_EQ_I(compile_s(&s, "a", SM_NORMAL, 1, 0, enc_cp(1252)), E_OK);
    to_ecp("\xc5\x91!", &r);
    TF_EQ_I(search_replace_all(s, &src, c1252, 0, 7, r.data(), r.size(), &sink, 0, &n, &bad), E_OK);
    TF_EQ_U(n, 2);
    TF_EQ_U(bad, 2);
    TF_EQ_U(sink.b.size(), 9);
    TF_CHECK(sink.b.size() == 9 && memcmp(sink.b.data(), "?! b ?! b", 9) == 0);
    // in a sub-range
    sink.b.clear();
    sink.b.append(t, 7);
    sink.last_pos = EPOS_NONE;
    TF_EQ_I(search_replace_all(s, &src, c1252, 1, 7, r.data(), r.size(), &sink, 0, &n, &bad), E_OK);
    TF_EQ_U(n, 1);
    TF_CHECK(sink.b.size() == 8 && memcmp(sink.b.data(), "a b ?! b", 8) == 0);
  }
}

class SrchCancelCb : public ProgressCb {
public:
  u32 calls; u32 cancel_at; u32 last_done; u32 last_total; int bad;
  SrchCancelCb(u32 at) : calls(0), cancel_at(at), last_done(0), last_total(0), bad(0) {}
  int progress(u32 done, u32 total)
  {
    if (done > total) bad = 1;
    last_done = done;
    last_total = total;
    calls++;
    return calls >= cancel_at ? 1 : 0;
  }
};

static void test_progress(TF* tf)
{
  u32 n = 2 * 1024 * 1024, k, cnt = 0;
  u8* buf = (u8*)e_alloc(n);
  const Codec* c = codec_get(enc_utf8(0));
  if (!buf) { TF_CHECK(0); return; }
  for (k = 0; k < n; k++) buf[k] = (u8)("abcdefgh \n"[k % 10]);
  {
    ViewSource src(buf, n);
    Searcher s;
    SrchCancelCb never(0xFFFFFFFFu), stop(2);
    SrchBufSink sink;
    Vec<ecp> r;
    u32 rep = 0, bad = 0;
    static const char* pats[] = { "zzz", "z\\w+", "(z)\\1", "[xyz]+" };
    static const u8 modes[] = { SM_NORMAL, SM_REGEX, SM_REGEX, SM_REGEX };
    u32 i;
    for (i = 0; i < 4; i++) {
      SrchCancelCb a(0xFFFFFFFFu), b(2);
      TF_EQ_I(compile_s(&s, pats[i], modes[i], 1, 0, enc_utf8(0)), E_OK);
      TF_EQ_I(s.count(&src, c, 0, n, &cnt, &a), E_OK);
      TF_EQ_U(cnt, 0);
      TF_CHECK(a.calls >= 2 && !a.bad && a.last_total == n);
      TF_EQ_I(s.count(&src, c, 0, n, &cnt, &b), E_CANCEL);
      TF_EQ_U(b.calls, 2);
    }
    // replace all: cancel = nothing applied
    TF_EQ_I(compile_s(&s, "a", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
    to_ecp("b", &r);
    sink.b.append(buf, n);
    TF_EQ_I(search_replace_all(s, &src, c, 0, n, r.data(), r.size(), &sink, &stop, &rep, &bad), E_CANCEL);
    TF_EQ_U(sink.calls, 0);
    TF_EQ_U(rep, 0);
    // backward search with progress
    {
      RxMatch m;
      SrchCancelCb a(0xFFFFFFFFu), b(1);
      TF_EQ_I(compile_s(&s, "zz", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
      TF_EQ_I(s.find(&src, c, 0, n, n, 1, &m, &a), 0);
      TF_CHECK(!a.bad);
      TF_EQ_I(s.find(&src, c, 0, n, n, 1, &m, &b), E_CANCEL);
    }
    (void)never;
  }
  // more than 1,000,000 replacements: E_TOOBIG, nothing applied
  {
    u32 big = 1000001;
    u8* ab = (u8*)e_alloc(big);
    if (ab) {
      Searcher s;
      SrchCountSink sink;
      Vec<ecp> r;
      u32 rep = 0, bad = 0;
      ViewSource src(ab, big);
      memset(ab, 'a', big);
      TF_EQ_I(compile_s(&s, "a", SM_NORMAL, 1, 0, enc_utf8(0)), E_OK);
      to_ecp("b", &r);
      TF_EQ_I(search_replace_all(s, &src, c, 0, big, r.data(), r.size(), &sink, 0, &rep, &bad), E_TOOBIG);
      TF_EQ_U(sink.calls, 0);
      TF_EQ_I(search_replace_all(s, &src, c, 1, big, r.data(), r.size(), &sink, 0, &rep, &bad), E_OK);
      TF_EQ_U(rep, 1000000);
      TF_EQ_U(sink.calls, 1000000);
      TF_EQ_I(sink.order_bad, 0);
      e_free(ab);
    }
  }
  e_free(buf);
}

// ---------------------------------------------------------------------------------------------------------------
// Encodings and multi-span sources

struct SrchEncText { EBytes bytes; Vec<epos> off; };   // off[i] = byte offset of char i, off[n] = end

static int encode_text(const Codec* c, const ecp* t, u32 n, SrchEncText* out)
{
  u32 i;
  out->bytes.clear();
  out->off.clear();
  for (i = 0; i < n; i++) {
    u8 b[8];
    u32 len = c->encode(t[i], b);
    if (!len) return 0;
    out->off.push(out->bytes.size());
    out->bytes.append(b, len);
  }
  out->off.push(out->bytes.size());
  return 1;
}

static u32 char_index(const SrchEncText& e, epos byte)
{
  u32 a = 0, b = e.off.size() - 1;
  while (a < b) {
    u32 mid = (a + b) / 2;
    if (e.off[mid] < byte) a = mid + 1; else b = mid;
  }
  return a;
}

struct SrchPat { const char* pat; u8 mode; u8 mc; u8 ww; };
static const SrchPat k_pats[] = {
  { "ab", SM_NORMAL, 1, 0 }, { "a", SM_NORMAL, 1, 0 }, { "AB", SM_NORMAL, 0, 0 }, { "b a", SM_NORMAL, 1, 0 },
  { "ab", SM_NORMAL, 1, 1 }, { "\\r\\n", SM_EXTENDED, 1, 0 }, { "a+", SM_REGEX, 1, 0 }, { "[a-c]+", SM_REGEX, 0, 0 },
  { "\\w+", SM_REGEX, 1, 0 }, { "\\bab\\b", SM_REGEX, 1, 0 }, { "^a", SM_REGEX, 1, 0 }, { "a$", SM_REGEX, 1, 0 },
  { ".", SM_REGEX, 1, 0 }, { "\\s+", SM_REGEX, 1, 0 }, { "(a|b)c", SM_REGEX, 1, 0 }, { "a.*?b", SM_REGEX, 1, 0 },
  { "\\R", SM_REGEX, 1, 0 }, { "[^a ]+", SM_REGEX, 1, 0 }, { "(a)\\1", SM_REGEX, 1, 0 }, { "a(?=b)", SM_REGEX, 1, 0 },
  { "x*", SM_REGEX, 1, 0 }, { "$", SM_REGEX, 1, 0 }, { "^", SM_REGEX, 1, 0 }, { "\\W", SM_REGEX, 1, 0 },
  { "b\\\\", SM_NORMAL, 1, 0 }, { "\\\\", SM_REGEX, 1, 0 }, { "(?i)b[^b]", SM_REGEX, 1, 0 }, { "\\B", SM_REGEX, 1, 0 },
  { "abc", SM_NORMAL, 0, 0 }, { "Ab\\b", SM_REGEX, 0, 0 }, { "\\w*ab\\b", SM_REGEX, 1, 0 },
  { "[^ ]ba", SM_REGEX, 1, 0 }, { "\\s?AB", SM_REGEX, 0, 0 }, { "[ab]+ca", SM_REGEX, 0, 0 },
};

// Text alphabets: shared ASCII part + extras per encoding group.
static const ecp k_ascii[] = { 'a', 'b', 'c', 'A', 'B', ' ', '\n', '\r', 'x', '1', '\\', 'a', 'b' };
static const ecp k_extra_latin[] = { 0xE4, 0xC4, 0x20AC };
static const ecp k_extra_uni[] = { 0xE4, 0x20AC, 0x1F600, 0x416, 0x436 };
static const ecp k_extra_cjk[] = { 0x4E21, 0x4E1C, 0x4E22, 0x4E00 + 189 + 0x21 };   // trail bytes 'a', '\', 'b'

static void gen_text(TF* tf, const ecp* extra, u32 nextra, u32 len, Vec<ecp>* out)
{
  u32 i;
  out->clear();
  for (i = 0; i < len; i++) {
    u32 r = tf_rand_below(tf, 13 + nextra);
    out->push(r < 13 ? k_ascii[r] : extra[r - 13]);
  }
}

// find_all over a flat and a chunked source; returns the matches as char-index pairs
static int matches_in(TF* tf, Searcher* s, const Codec* c, const SrchEncText& et, Vec<u32>* chars, const char* label)
{
  Vec<MarkRange> a, b;
  u32 k;
  ViewSource flat(et.bytes.data(), et.bytes.size());
  SrchChunkSource chunks(et.bytes.data(), et.bytes.size(), tf, 7);
  int r1 = s->find_all(&flat, c, 0, et.bytes.size(), &a, 100000, 0);
  int r2 = s->find_all(&chunks, c, 0, et.bytes.size(), &b, 100000, 0);
  chars->clear();
  if (r1 != r2 || a.size() != b.size()) {
    tf_fail(tf, __FILE__, __LINE__, "%s: flat %d/%u vs chunked %d/%u matches", label, r1, a.size(), r2, b.size());
    return 0;
  }
  for (k = 0; k < a.size(); k++) {
    if (a[k].pos != b[k].pos || a[k].len != b[k].len) {
      tf_fail(tf, __FILE__, __LINE__, "%s: match %u flat %u+%u chunked %u+%u", label, k, a[k].pos, a[k].len, b[k].pos, b[k].len);
      return 0;
    }
    chars->push(char_index(et, a[k].pos));
    chars->push(char_index(et, a[k].pos + a[k].len));
  }
  return 1;
}

static void test_encodings(TF* tf)
{
  SrchU16Codec le(0), be(1);
  SrchDbcsCodec dbcs;
  const Codec* u8c = codec_get(enc_utf8(0));
  const Codec* c1252 = codec_get(enc_cp(1252));
  const Codec* t2le = codec_get(enc_utf16le(0));
  const Codec* t2be = codec_get(enc_utf16be(0));
  const Codec* groups[3][6];
  EncSpec gspec[3][6];
  u32 ng[3], g, iter, rounds = tf->long_run ? 400 : 60, compared = 0;
  groups[0][0] = u8c; groups[0][1] = &le; groups[0][2] = &be; groups[0][3] = c1252; ng[0] = 4;
  gspec[0][0] = enc_utf8(0); gspec[0][1] = enc_utf16le(0); gspec[0][2] = enc_utf16be(0); gspec[0][3] = enc_cp(1252);
  groups[1][0] = u8c; groups[1][1] = &le; groups[1][2] = &be; ng[1] = 3;
  gspec[1][0] = enc_utf8(0); gspec[1][1] = enc_utf16le(0); gspec[1][2] = enc_utf16be(0);
  groups[2][0] = u8c; groups[2][1] = &dbcs; ng[2] = 2;
  gspec[2][0] = enc_utf8(0); gspec[2][1] = enc_cp(932);
  if (t2le && t2be) {                  // T02's codecs as well, when present
    groups[1][3] = t2le; gspec[1][3] = enc_utf16le(0);
    groups[1][4] = t2be; gspec[1][4] = enc_utf16be(0);
    ng[1] = 5;
  }
  if (!u8c || !c1252) { TF_CHECK(0); return; }
  for (iter = 0; iter < rounds; iter++) {
    for (g = 0; g < 3; g++) {
      Vec<ecp> text;
      const ecp* ex = g == 0 ? k_extra_latin : g == 1 ? k_extra_uni : k_extra_cjk;
      u32 nex = g == 0 ? 3 : g == 1 ? 5 : 4, p;
      gen_text(tf, ex, nex, tf_rand_below(tf, 60), &text);
      for (p = 0; p < sizeof(k_pats) / sizeof(k_pats[0]); p++) {
        Vec<u32> ref;
        u32 e;
        for (e = 0; e < ng[g]; e++) {
          SrchEncText et;
          Searcher s;
          Vec<u32> got;
          char label[128];
          if (!encode_text(groups[g][e], text.data(), text.size(), &et)) { TF_CHECK(0); continue; }
          if (compile_s(&s, k_pats[p].pat, k_pats[p].mode, k_pats[p].mc, k_pats[p].ww, gspec[g][e]) != E_OK) {
            TF_CHECK(0);
            continue;
          }
          es_fmt(label, sizeof(label), "/%s/ group %u enc %u iter %u", k_pats[p].pat, g, e, iter);
          if (!matches_in(tf, &s, groups[g][e], et, &got, label)) continue;
          if (e == 0) { ref.clear(); ref.insert(0, got.data(), got.size()); continue; }
          if (got.size() != ref.size() || (got.size() && memcmp(got.data(), ref.data(), got.size() * 4) != 0))
            tf_fail(tf, __FILE__, __LINE__, "%s: %u matches vs %u in UTF-8", label, got.size() / 2, ref.size() / 2);
          else { tf_check(tf, 1, __FILE__, __LINE__, "enc"); compared++; }
        }
      }
    }
  }
  tf_log(tf, "encodings: %u cross-encoding comparisons (T02 UTF-16 codecs %s)", compared, t2le ? "included" : "not present");
  // DBCS: an ASCII pattern never matches a trail byte
  {
    static const ecp t[] = { 0x4E21, 'a', 0x4E1C, '\\' };
    SrchEncText et;
    Searcher s;
    EStr got;
    encode_text(&dbcs, t, 4, &et);
    ViewSource src(et.bytes.data(), et.bytes.size());
    TF_EQ_I(compile_s(&s, "a", SM_NORMAL, 1, 0, enc_cp(932)), E_OK);
    all_str(&s, &src, &dbcs, 0, et.bytes.size(), &got);
    TF_EQ_S(got.c_str(), "2,3;");
    TF_EQ_I(compile_s(&s, "\\", SM_NORMAL, 1, 0, enc_cp(932)), E_OK);
    all_str(&s, &src, &dbcs, 0, et.bytes.size(), &got);
    TF_EQ_S(got.c_str(), "5,6;");
    {
      RxMatch m;
      TF_EQ_I(s.find(&src, &dbcs, 0, et.bytes.size(), et.bytes.size(), 1, &m, 0), 1);
      TF_EQ_U(m.pos, 5);
      TF_EQ_I(s.find(&src, &dbcs, 0, et.bytes.size(), 5, 1, &m, 0), 0);
    }
  }
  // UTF-16: an odd byte offset never matches; a pattern made of a low surrogate isn't prefiltered wrongly
  {
    static const u8 t[] = { 0x41, 0x41, 0x41, 0x00 };   // LE units 0x4141, 0x0041
    Searcher s;
    EStr got;
    ViewSource src(t, 4);
    TF_EQ_I(compile_s(&s, "A", SM_NORMAL, 1, 0, enc_utf16le(0)), E_OK);
    all_str(&s, &src, &le, 0, 4, &got);
    TF_EQ_S(got.c_str(), "2,4;");
    TF_EQ_I(compile_s(&s, "\\x{1F600}", SM_REGEX, 1, 0, enc_utf16le(0)), E_OK);
    {
      static const u8 e[] = { 0x3D, 0xD8, 0x00, 0xDE, 0x00, 0xDE };   // pair + lone low surrogate
      ViewSource s2(e, 6);
      all_str(&s, &s2, &le, 0, 6, &got);
      TF_EQ_S(got.c_str(), "0,4;");
    }
  }
}

// ---- backward == the last forward match before the position ----

static int ref_back(const SrchPat& ps, ByteSource* src, const Codec* c, epos lo, epos hi, epos from, RxMatch* out)
{
  Vec<ecp> p, lit;
  RxProg* prog = 0;
  RxError err;
  RxMatcher mt;
  RxMatch m;
  SearchError se;
  u32 flags = 0;
  epos pos = lo;
  int have = 0;
  to_ecp(ps.pat, &p);
  if (!ps.mc) flags |= RXF_ICASE;
  if (ps.ww) flags |= RXF_WORD;
  if (ps.mode == SM_EXTENDED) { search_parse_extended(p.data(), p.size(), &lit, &se); p.swap(lit); }
  if (ps.mode != SM_REGEX) flags |= RXF_LITERAL_;
  if (rx_compile(p.data(), p.size(), flags, &prog, &err) != E_OK) return -100;
  mt.init(prog);
  mt.set_context_end(hi);
  for (;;) {
    int r = mt.search(src, c, lo, pos, from, &m, 0);
    if (r != 1 || m.pos >= from) break;
    *out = m;
    have = 1;
    if (m.len) pos = m.pos + m.len;
    else {
      RxReader rd;
      ecp ch;
      u32 len;
      rxr_init(&rd, src, c);
      len = rxr_decode(&rd, m.pos, &ch);
      pos = m.pos + (len ? len : 1);
    }
    if (pos > from) break;
  }
  rx_free(prog);
  return have;
}

static void test_backward(TF* tf)
{
  const Codec* u8c = codec_get(enc_utf8(0));
  SrchU16Codec le(0);
  u32 iter, rounds = tf->long_run ? 300 : 40, checked = 0;
  for (iter = 0; iter < rounds; iter++) {
    Vec<ecp> text;
    SrchEncText et;
    u32 big = iter % 4 == 3, which;
    const Codec* c = (iter & 1) ? (const Codec*)&le : u8c;
    EncSpec es = (iter & 1) ? enc_utf16le(0) : enc_utf8(0);
    gen_text(tf, k_extra_uni, 5, big ? 3000 + tf_rand_below(tf, 6000) : tf_rand_below(tf, 80), &text);
    encode_text(c, text.data(), text.size(), &et);
    for (which = 0; which < sizeof(k_pats) / sizeof(k_pats[0]); which++) {
      const SrchPat& ps = k_pats[which];
      Searcher s;
      SrchChunkSource src(et.bytes.data(), et.bytes.size(), tf, 300);
      u32 k;
      if (big && (strstr(ps.pat, "\\s") || strstr(ps.pat, "\\R") || strstr(ps.pat, "\\W") || strstr(ps.pat, "\\r") ||
                  strstr(ps.pat, "[^") || strstr(ps.pat, "\\B") || strchr(ps.pat, ' ')))
        continue;                      // patterns that cross lines may legitimately differ over 2 KB blocks
      if (compile_s(&s, ps.pat, ps.mode, ps.mc, ps.ww, es) != E_OK) { TF_CHECK(0); continue; }
      for (k = 0; k < 6; k++) {
        u32 ci = text.size() ? tf_rand_below(tf, text.size() + 1) : 0;
        epos from = et.off[ci], lo = 0, hi = et.bytes.size();
        RxMatch a, b;
        int r1, r2;
        if (k == 0) from = hi;
        if (k == 5 && ci > 0) lo = et.off[tf_rand_below(tf, ci)];
        r1 = s.find(&src, c, lo, hi, from, 1, &a, 0);
        r2 = ref_back(ps, &src, c, lo, hi, from, &b);
        if (r1 != r2 || (r1 == 1 && (a.pos != b.pos || a.len != b.len))) {
          tf_fail(tf, __FILE__, __LINE__, "backward /%s/ iter %u from %u lo %u: got %d %u+%u, expected %d %u+%u", ps.pat,
                  iter, from, lo, r1, r1 == 1 ? a.pos : 0, r1 == 1 ? a.len : 0, r2, r2 == 1 ? b.pos : 0, r2 == 1 ? b.len : 0);
          break;
        }
        tf_check(tf, 1, __FILE__, __LINE__, "back");
        checked++;
      }
    }
  }
  tf_log(tf, "backward: %u positions checked", checked);
}

// ---- Replace All vs a naive reference ----

static void ref_expand(const char* rep, const u8* text, const RxMatch& m, EBytes* out)
{
  u32 i = 0, n = es_len(rep);
  int mode = 0, one = 0;
  while (i < n) {
    char ch = rep[i];
    int g = -1;
    if (ch == '$' && i + 1 < n && rep[i + 1] >= '0' && rep[i + 1] <= '9') { g = rep[i + 1] - '0'; i += 2; }
    else if (ch == '\\' && i + 1 < n) {
      char d = rep[i + 1];
      i += 2;
      if (d == 'U') { mode = 1; continue; }
      if (d == 'L') { mode = 2; continue; }
      if (d == 'E') { mode = 0; one = 0; continue; }
      if (d == 'u') { one = 1; continue; }
      if (d == 'n') { out->push('\n'); continue; }
      out->push((u8)d);
      continue;
    } else { i++; }
    if (g >= 0) {
      epos gp = g == 0 ? m.pos : m.gpos[g], gl = g == 0 ? m.len : m.glen[g], k;
      if (gp == EPOS_NONE) continue;
      for (k = 0; k < gl; k++) {
        u8 b = text[gp + k];
        if (one) { if (b >= 'a' && b <= 'z') b -= 32; one = 0; }
        else if (mode == 1 && b >= 'a' && b <= 'z') b -= 32;
        else if (mode == 2 && b >= 'A' && b <= 'Z') b += 32;
        out->push(b);
      }
      continue;
    }
    {
      u8 b = (u8)ch;
      if (one) { if (b >= 'a' && b <= 'z') b -= 32; one = 0; }
      else if (mode == 1 && b >= 'a' && b <= 'z') b -= 32;
      else if (mode == 2 && b >= 'A' && b <= 'Z') b += 32;
      out->push(b);
    }
  }
}

static void test_replace_random(TF* tf)
{
  static const char* pats[] = { "a", "ab", "a+", "x*", "^", "$", "\\b", "(a)(b)?", "\\w+", "(\\w)\\1", "b(?=a)", "\\s+",
                                "[^ab\\n]", "^\\w", "c$", "(a|b)+?" };
  static const char* reps[] = { "", "X", "$0$0", "[$1]", "\\U$0\\E!", "\\u$0", "<$2|$1>", "\\n", "$0\\\\" };
  const Codec* u8c = codec_get(enc_utf8(0));
  u32 iter, rounds = tf->long_run ? 2000 : 300;
  for (iter = 0; iter < rounds; iter++) {
    char text[64];
    u32 tl = tf_rand_below(tf, 40), k;
    const char* pat = pats[tf_rand_below(tf, sizeof(pats) / sizeof(pats[0]))];
    const char* rep = reps[tf_rand_below(tf, sizeof(reps) / sizeof(reps[0]))];
    Searcher s;
    SrchBufSink sink;
    EBytes ref;
    Vec<ecp> r;
    RxMatch m;
    epos pos = 0, last = 0;
    u32 n = 0, bad = 0, count = 0;
    int rc;
    for (k = 0; k < tl; k++) text[k] = "abcab A\nx"[tf_rand_below(tf, 9)];
    text[tl] = 0;
    {
      SrchChunkSource src((const u8*)text, tl, tf, 5);
      if (compile_s(&s, pat, SM_REGEX, 1, 0, enc_utf8(0)) != E_OK) { TF_CHECK(0); continue; }
      // naive: forward iteration over a flat copy, building the output front to back
      {
        ViewSource flat((const u8*)text, tl);
        for (;;) {
          if (s.find(&flat, u8c, 0, tl, pos, 0, &m, 0) != 1) break;
          ref.append(text + last, m.pos - last);
          ref_expand(rep, (const u8*)text, m, &ref);
          last = m.pos + m.len;
          count++;
          pos = m.len ? m.pos + m.len : m.pos + 1;
          if (pos > tl) break;
        }
        ref.append(text + last, tl - last);
      }
      sink.b.append(text, tl);
      to_ecp(rep, &r);
      rc = search_replace_all(s, &src, u8c, 0, tl, r.data(), r.size(), &sink, 0, &n, &bad);
    }
    if (rc != E_OK || n != count || sink.b.size() != ref.size() ||
        (ref.size() && memcmp(sink.b.data(), ref.data(), ref.size()) != 0)) {
      char a[200], b[200];
      u32 la = emin<u32>(sink.b.size(), 199), lb = emin<u32>(ref.size(), 199);
      memcpy(a, sink.b.data(), la); a[la] = 0;
      memcpy(b, ref.data(), lb); b[lb] = 0;
      tf_fail(tf, __FILE__, __LINE__, "replace /%s/ -> \"%s\" in \"%s\": got \"%s\" (%u), expected \"%s\" (%u)", pat, rep,
              text, a, n, b, count);
    } else {
      tf_check(tf, 1, __FILE__, __LINE__, "repl");
    }
  }
}

// ---- out of memory ----

static void test_oom(TF* tf)
{
  static const SrchPat ps[] = { { "abc", SM_NORMAL, 0, 1 }, { "(\\w+)\\s(\\w+)", SM_REGEX, 1, 0 },
                                { "\\r\\n", SM_EXTENDED, 1, 0 }, { "(a)\\1", SM_REGEX, 1, 0 }, { "DE ?? AD", SM_HEX, 1, 0 } };
  const char* text = "xx abc ABC aa\r\nfoo bar \xde\x01\xad";
  const Codec* u8c = codec_get(enc_utf8(0));
  u32 i;
  i32 k;
  for (i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
    for (k = 1; k < 300; k++) {
      Searcher s;
      SrchBufSink sink;
      Vec<ecp> pat, rep;
      Vec<MarkRange> all;
      SearchError err;
      u32 n = 0, bad = 0;
      int r1, r2 = E_OK, r3 = E_OK;
      ViewSource src((const u8*)text, es_len(text));
      to_ecp(ps[i].pat, &pat);
      to_ecp(ps[i].mode == SM_HEX ? "00" : ps[i].mode == SM_REGEX ? "\\U$1-$0" : "\\t", &rep);
      sink.b.append(text, es_len(text));
      e_test_fail_after(k);
      r1 = s.compile(pat.data(), pat.size(), mk_opts(ps[i].mode, ps[i].mc, ps[i].ww), enc_utf8(0), &err);
      if (r1 == E_OK) {
        r2 = s.find_all(&src, u8c, 0, es_len(text), &all, 1000, 0);
        r3 = search_replace_all(s, &src, u8c, 0, es_len(text), rep.data(), rep.size(), &sink, 0, &n, &bad);
      }
      e_test_fail_after(-1);
      if (!((r1 == E_OK || r1 == E_NOMEM) && (r2 >= 0 || r2 == E_NOMEM) && (r3 == E_OK || r3 == E_NOMEM)))
        tf_fail(tf, __FILE__, __LINE__, "oom /%s/ at %d: %d %d %d", ps[i].pat, k, r1, r2, r3);
      else tf_check(tf, 1, __FILE__, __LINE__, "oom");
      if (r1 == E_OK && r2 >= 0 && r3 == E_OK) break;
    }
  }
}

// ---- throughput (logged; run without -q to see it) ----

static void test_throughput(TF* tf)
{
  static const char* words[] = { "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "int ",
                                 "return ", "value; ", "x = 42;\r\n", "if (a < b) ", "{\r\n", "}\r\n", "while " };
  static const SrchPat ps[] = { { "zebra", SM_NORMAL, 1, 0 }, { "ZEBRA", SM_NORMAL, 0, 0 }, { "zebra", SM_NORMAL, 1, 1 },
                                { "\\bzeb\\w+", SM_REGEX, 1, 0 }, { "[0-9]{3}", SM_REGEX, 1, 0 }, { "(foo|bar)baz", SM_REGEX, 1, 0 },
                                { "\\w+ing\\b", SM_REGEX, 1, 0 }, { "^\\s*#", SM_REGEX, 1, 0 }, { "(\\w)\\1x", SM_REGEX, 1, 0 },
                                { "lazy", SM_NORMAL, 1, 0 }, { "a.*z", SM_REGEX, 1, 0 }, { "[a-z]+\\(", SM_REGEX, 1, 0 },
                                { "^\\s*[{}]", SM_REGEX, 1, 0 }, { "\\d+", SM_REGEX, 1, 0 } };
  u32 n = tf->long_run ? 8u << 20 : 1u << 20, len = 0, i;
  u8* buf = (u8*)e_alloc(n + 32);
  const Codec* u8c = codec_get(enc_utf8(0));
  if (!buf) return;
  while (len < n) {
    const char* w = words[tf_rand_below(tf, 16)];
    u32 wl = es_len(w);
    memcpy(buf + len, w, wl);
    len += wl;
  }
  for (i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
    Searcher s;
    u32 cnt = 0, reps = 0;
    clock_t t0;
    double dt;
    ViewSource src(buf, len);
    if (compile_s(&s, ps[i].pat, ps[i].mode, ps[i].mc, ps[i].ww, enc_utf8(0)) != E_OK) continue;
    t0 = clock();
    do {                               // Win98's clock() ticks every 55 ms: repeat for at least 0.3 s
      s.count(&src, u8c, 0, len, &cnt, 0);
      reps++;
      dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    } while (dt < 0.3 && reps < 200);
    {
      u32 rate100 = (u32)((reps * (len / 1048576.0)) * 100.0 / (dt > 0.0005 ? dt : 0.0005));
      tf_log(tf, "throughput %-14s mode %u: %u.%02u MB/s (%u matches, %u KB x %u, %u ms)", ps[i].pat, (u32)ps[i].mode,
             rate100 / 100, rate100 % 100, cnt, len >> 10, reps, (u32)(dt * 1000));
    }
  }
  e_free(buf);
}

void suite_search(TF* tf)
{
  test_extended(tf);
  test_modes(tf);
  test_find_basic(tf);
  test_impossible(tf);
  test_expand(tf);
  test_replace_fixed(tf);
  test_progress(tf);
  test_encodings(tf);
  test_backward(tf);
  test_replace_random(tf);
  test_oom(tf);
  test_throughput(tf);
}
