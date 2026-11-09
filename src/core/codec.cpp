// codec.cpp - MINIMAL codec layer from T00: strict UTF-8 and Windows-1252 (hand-written 0x80-0x9F table), the enc_*
// helpers, codec_decode_buf/encode_str/prev and CharReader. T02 replaces this with the full set (UTF-16, generated
// SBCS tables from cptables.cpp, DBCS providers).
// Owner after Wave 0: T02.
#include "codec.h"
#include "ptree.h"
#include "estr.h"
#include <string.h>

static u16 g_acp = 1252, g_oemcp = 437;

static int is_dbcs_cp(u16 cp)
{
  return cp == 932 || cp == 936 || cp == 949 || cp == 950;
}

bool enc_equal(const EncSpec& a, const EncSpec& b)
{
  return a.kind == b.kind && a.bom == b.bom && a.cp == b.cp;
}

void enc_set_system(u16 acp, u16 oemcp)
{
  g_acp = acp;
  g_oemcp = oemcp;
}

EncSpec enc_cp(u16 cp)
{
  EncSpec e;
  if (cp == 65001) return enc_utf8(0);
  if (cp == 1200) return enc_utf16le(0);
  if (cp == 1201) return enc_utf16be(0);
  e.kind = (u8)(is_dbcs_cp(cp) ? EK_DBCS : EK_SBCS);
  e.bom = 0;
  e.cp = cp;
  return e;
}

EncSpec enc_ansi() { return enc_cp(g_acp); }
EncSpec enc_oem() { return enc_cp(g_oemcp); }

EncSpec enc_utf8(int bom)
{
  EncSpec e;
  e.kind = EK_UTF8; e.bom = (u8)(bom ? 1 : 0); e.cp = 65001;
  return e;
}

EncSpec enc_utf16le(int bom)
{
  EncSpec e;
  e.kind = EK_UTF16LE; e.bom = (u8)(bom ? 1 : 0); e.cp = 1200;
  return e;
}

EncSpec enc_utf16be(int bom)
{
  EncSpec e;
  e.kind = EK_UTF16BE; e.bom = (u8)(bom ? 1 : 0); e.cp = 1201;
  return e;
}

const char* enc_label(const EncSpec& e, char* buf, u32 cap)
{
  if (!buf || cap == 0) return "";
  switch (e.kind) {
  case EK_UTF8: es_copy(buf, cap, e.bom ? "UTF-8 BOM" : "UTF-8"); return buf;
  case EK_UTF16LE: es_copy(buf, cap, "UTF-16 LE"); return buf;
  case EK_UTF16BE: es_copy(buf, cap, "UTF-16 BE"); return buf;
  default: break;
  }
  if (e.cp == g_acp) es_copy(buf, cap, "ANSI");
  else if (e.cp == g_oemcp) es_copy(buf, cap, "OEM");
  else if (e.cp == 874 || (e.cp >= 1250 && e.cp <= 1258)) es_fmt(buf, cap, "Windows-%u", (u32)e.cp);
  else if (e.cp >= 437 && e.cp <= 869) es_fmt(buf, cap, "OEM %u", (u32)e.cp);
  else if (e.cp == 20866) es_copy(buf, cap, "KOI8-R");
  else if (e.cp == 21866) es_copy(buf, cap, "KOI8-U");
  else if (e.cp >= 28591 && e.cp <= 28599) es_fmt(buf, cap, "ISO-8859-%u", (u32)(e.cp - 28590));
  else if (e.cp == 28605) es_copy(buf, cap, "ISO-8859-15");
  else if (e.kind == EK_DBCS) es_fmt(buf, cap, "DBCS %u", (u32)e.cp);
  else es_fmt(buf, cap, "Code page %u", (u32)e.cp);
  return buf;
}

int enc_parse(const char* token, EncSpec* out)
{
  u32 cp;
  EncSpec e;
  if (!token || !out) return E_SYNTAX;
  if (es_icmp(token, "ansi") == 0) e = enc_ansi();
  else if (es_icmp(token, "oem") == 0) e = enc_oem();
  else if (es_icmp(token, "utf8") == 0) e = enc_utf8(0);
  else if (es_icmp(token, "utf8bom") == 0) e = enc_utf8(1);
  else if (es_icmp(token, "utf16le") == 0) e = enc_utf16le(1);
  else if (es_icmp(token, "utf16be") == 0) e = enc_utf16be(1);
  else if (es_icmp(token, "utf16le-nobom") == 0) e = enc_utf16le(0);
  else if (es_icmp(token, "utf16be-nobom") == 0) e = enc_utf16be(0);
  else if (es_nicmp(token, "cp", 2) == 0 && es_parse_u32(token + 2, &cp, 10) == E_OK && cp > 0 && cp < 65536)
    e = enc_cp((u16)cp);
  else return E_SYNTAX;
  *out = e;
  return E_OK;
}

void enc_token(const EncSpec& e, char* buf, u32 cap)
{
  switch (e.kind) {
  case EK_UTF8: es_copy(buf, cap, e.bom ? "utf8bom" : "utf8"); return;
  case EK_UTF16LE: es_copy(buf, cap, e.bom ? "utf16le" : "utf16le-nobom"); return;
  case EK_UTF16BE: es_copy(buf, cap, e.bom ? "utf16be" : "utf16be-nobom"); return;
  default: break;
  }
  if (e.cp == g_acp) es_copy(buf, cap, "ansi");
  else if (e.cp == g_oemcp) es_copy(buf, cap, "oem");
  else es_fmt(buf, cap, "cp%u", (u32)e.cp);
}

u32 enc_bom(const EncSpec& e, u8* out)
{
  if (!e.bom) return 0;
  switch (e.kind) {
  case EK_UTF8: out[0] = 0xEF; out[1] = 0xBB; out[2] = 0xBF; return 3;
  case EK_UTF16LE: out[0] = 0xFF; out[1] = 0xFE; return 2;
  case EK_UTF16BE: out[0] = 0xFE; out[1] = 0xFF; return 2;
  default: break;
  }
  return 0;
}

// ---- UTF-8 (strict) ----

class Utf8Codec : public Codec {
public:
  Utf8Codec() { kind = EK_UTF8; unit = 1; max_len = 4; ascii_compat = 1; self_sync = 1; pad = 0; cp = 65001; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u8 b0 = p[0];
    u32 len, i;
    ecp v;
    u8 lo = 0x80, hi = 0xBF;
    if (b0 < 0x80) { *out = b0; return 1; }
    if (b0 >= 0xC2 && b0 <= 0xDF) { len = 2; v = b0 & 0x1F; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) {
      len = 3; v = b0 & 0x0F;
      if (b0 == 0xE0) lo = 0xA0;
      if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      len = 4; v = b0 & 0x07;
      if (b0 == 0xF0) lo = 0x90;
      if (b0 == 0xF4) hi = 0x8F;
    } else { *out = ECP_BADBYTE(b0); return 1; }
    for (i = 1; i < len; i++) {
      u8 b;
      if (i >= avail) {
        if (!eof) return 0;                       // need more bytes; prefix valid so far
        *out = ECP_BADBYTE(b0); return 1;         // truncated at the end
      }
      b = p[i];
      if (i == 1 ? (b < lo || b > hi) : (b < 0x80 || b > 0xBF)) { *out = ECP_BADBYTE(b0); return 1; }
      v = (v << 6) | (b & 0x3F);
    }
    *out = v;
    return len;
  }
  u32 encode(ecp c, u8* out) const
  {
    if (c >= 0x110000u && c < 0x110100u) { out[0] = (u8)(c - 0x110000u); return 1; }
    if (c >= 0x110000u) return 0;                 // bad UTF-16 unit: not storable in UTF-8 (T02 decides)
    if (c >= 0xD800u && c <= 0xDFFFu) return 0;
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c < 0x800) { out[0] = (u8)(0xC0 | (c >> 6)); out[1] = (u8)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
      out[0] = (u8)(0xE0 | (c >> 12)); out[1] = (u8)(0x80 | ((c >> 6) & 0x3F)); out[2] = (u8)(0x80 | (c & 0x3F));
      return 3;
    }
    out[0] = (u8)(0xF0 | (c >> 18)); out[1] = (u8)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((c >> 6) & 0x3F)); out[3] = (u8)(0x80 | (c & 0x3F));
    return 4;
  }
};

// ---- Windows-1252 ----

static const u16 k1252_hi[32] = {   // 0x80-0x9F; 0 = undefined byte
  0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
  0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

class Cp1252Codec : public Codec {
public:
  Cp1252Codec() { kind = EK_SBCS; unit = 1; max_len = 1; ascii_compat = 1; self_sync = 1; pad = 0; cp = 1252; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u8 b = p[0];
    (void)avail; (void)eof;
    if (b >= 0x80 && b <= 0x9F) *out = k1252_hi[b - 0x80] ? (ecp)k1252_hi[b - 0x80] : ECP_BADBYTE(b);
    else *out = b;
    return 1;
  }
  u32 encode(ecp c, u8* out) const
  {
    u32 i;
    if (c >= 0x110000u && c < 0x110100u) { out[0] = (u8)(c - 0x110000u); return 1; }
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) { out[0] = (u8)c; return 1; }
    for (i = 0; i < 32; i++) if (k1252_hi[i] && k1252_hi[i] == c) { out[0] = (u8)(0x80 + i); return 1; }
    return 0;
  }
};

static Utf8Codec g_utf8;
static Cp1252Codec g_cp1252;

int codec_init()
{
  return E_OK;
}

const Codec* codec_get(const EncSpec& e)
{
  if (e.kind == EK_UTF8) return &g_utf8;
  if (e.kind == EK_SBCS && e.cp == 1252) return &g_cp1252;
  return 0;
}

int codec_sbcs_supported(u16 cp)
{
  return cp == 1252;
}

void codec_break_spec(const EncSpec& e, int cr_mode, BreakSpec* bs)
{
  u8 b = (u8)(cr_mode ? 0x0D : 0x0A);
  bs->pad = 0;
  if (e.kind == EK_UTF16LE) { bs->unit = 2; bs->b0 = b; bs->b1 = 0; }
  else if (e.kind == EK_UTF16BE) { bs->unit = 2; bs->b0 = 0; bs->b1 = b; }
  else { bs->unit = 1; bs->b0 = b; bs->b1 = 0; }
}

int codec_register_dbcs(u16 cp, DbcsProvider* p)
{
  (void)cp; (void)p;
  return E_NOTIMPL;
}

u32 codec_decode_buf(const Codec* c, const u8* p, u32 n, int eof, ecp* out, u32 cap, u32* bytes_used)
{
  u32 i = 0, k = 0;
  while (i < n && k < cap) {
    u32 used = c->decode(p + i, n - i, eof, out + k);
    if (used == 0) break;
    i += used;
    k++;
  }
  if (bytes_used) *bytes_used = i;
  return k;
}

int codec_encode_str(const Codec* c, const ecp* s, u32 n, EBytes* out, u32* unrepresentable, ecp subst)
{
  u32 i, bad = 0;
  u8 buf[8];
  for (i = 0; i < n; i++) {
    u32 len = c->encode(s[i], buf);
    if (len == 0) {
      bad++;
      if (!subst) { if (unrepresentable) *unrepresentable = bad; return E_RANGE; }
      len = c->encode(subst, buf);
      if (len == 0) len = c->encode('?', buf);
    }
    if (len && out->append(buf, len) != E_OK) { if (unrepresentable) *unrepresentable = bad; return E_NOMEM; }
  }
  if (unrepresentable) *unrepresentable = bad;
  return E_OK;
}

epos codec_prev(ByteSource* s, const Codec* c, epos pos, epos floor)
{
  u8 buf[8];
  epos sz = s->size();
  u32 k;
  if (pos <= floor) return floor;
  if (c->self_sync) {
    for (k = c->max_len; k >= c->unit; k -= c->unit) {
      epos start, n;
      ecp cp;
      if (k > pos - floor) continue;
      start = pos - k;
      n = sz - start;
      if (n > c->max_len) n = c->max_len;
      if (s->read(start, buf, n) != E_OK) return pos - c->unit;
      if (c->decode(buf, n, start + n >= sz, &cp) == k) return start;
    }
    return pos - emin<epos>((epos)c->unit, pos - floor);
  }
  // non-self-synchronising (DBCS): decode forward from the floor
  {
    epos at = floor, last = floor;
    while (at < pos) {
      epos n = sz - at;
      ecp cp;
      u32 used;
      if (n > c->max_len) n = c->max_len;
      if (s->read(at, buf, n) != E_OK) return pos - 1;
      used = c->decode(buf, n, at + n >= sz, &cp);
      if (used == 0) used = 1;
      last = at;
      at += used;
    }
    return last;
  }
}

void CharReader::init(ByteSource* s, const Codec* c, epos pos, epos end)
{
  s_ = s; c_ = c; pos_ = pos;
  end_ = end > s->size() ? s->size() : end;
}

int CharReader::next(ecp* cp, epos* at)
{
  epos avail = 0;
  const u8* p;
  u32 used;
  if (pos_ >= end_) return 0;
  p = s_->get(pos_, &avail);
  if (!p) return E_IO;
  if (avail > end_ - pos_) avail = end_ - pos_;
  used = c_->decode(p, avail, pos_ + avail >= end_, cp);
  if (used == 0) {                     // character spans a chunk boundary: copy it
    u8 buf[8];
    epos n = end_ - pos_;
    if (n > c_->max_len) n = c_->max_len;
    if (s_->read(pos_, buf, n) != E_OK) return E_IO;
    used = c_->decode(buf, n, pos_ + n >= end_, cp);
    if (used == 0) { *cp = ECP_BADBYTE(buf[0]); used = 1; }
  }
  if (at) *at = pos_;
  pos_ += used;
  return 1;
}

epos CharReader::pos() const
{
  return pos_;
}
