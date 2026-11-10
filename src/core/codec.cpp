// codec.cpp - encodings and codecs (DESIGN 6.9, 7.1): strict UTF-8, UTF-16 LE/BE, single-byte code pages from the
// generated tables in cptables.cpp, DBCS through registered DbcsProvider objects; enc_* helpers, labels and tokens;
// codec_decode_buf / codec_encode_str / codec_prev; CharReader.
//
// Invalid input (DESIGN 5.1, 7.1): one ECP_BADBYTE per undecodable byte (UTF-8, SBCS undefined byte, DBCS lead byte
// of a bad pair, final odd byte of UTF-16) and one ECP_BADUNIT per lone UTF-16 surrogate unit. Every codec encodes
// ECP_BADBYTE(b) back to the byte b; the UTF-16 codecs encode ECP_BADUNIT(u) back to the unit u in their byte order.
// ECP_BADUNIT is not representable in the byte-oriented codecs (encode returns 0).
//
// Threads: codec_init (and codec_register_dbcs) run on the UI thread before any worker; after that every codec object
// is read-only and shareable.
// Owner: T02.
#include "codec.h"
#include "codec_tables.h"
#include "ptree.h"
#include "estr.h"
#include <string.h>

static u16 g_acp = 1252, g_oemcp = 437;

static int is_dbcs_cp(u16 cp);

// ---------------------------------------------------------------------------------------------------------------
// EncSpec helpers

bool enc_equal(const EncSpec& a, const EncSpec& b)
{
  return a.kind == b.kind && a.bom == b.bom && a.cp == b.cp;
}

void enc_set_system(u16 acp, u16 oemcp)
{
  g_acp = acp;
  g_oemcp = oemcp;
}

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

// Name of a code page without the ANSI/OEM aliasing: "Windows-1251", "OEM 866", "KOI8-R", "ISO-8859-5",
// "Shift-JIS", "Code page 1361". Declared for other modules in codec.h once the overseer adds it (HANDOFF T02).
const char* enc_cp_name(u16 cp, char* buf, u32 cap)
{
  if (!buf || cap == 0) return "";
  if (cp == 65001) es_copy(buf, cap, "UTF-8");
  else if (cp == 1200) es_copy(buf, cap, "UTF-16 LE");
  else if (cp == 1201) es_copy(buf, cap, "UTF-16 BE");
  else if (cp == 874 || (cp >= 1250 && cp <= 1258)) es_fmt(buf, cap, "Windows-%u", (u32)cp);
  else if (cp == 437 || cp == 720 || cp == 737 || cp == 775 || (cp >= 850 && cp <= 869))
    es_fmt(buf, cap, "OEM %u", (u32)cp);
  else if (cp == 20866) es_copy(buf, cap, "KOI8-R");
  else if (cp == 21866) es_copy(buf, cap, "KOI8-U");
  else if (cp >= 28591 && cp <= 28599) es_fmt(buf, cap, "ISO-8859-%u", (u32)(cp - 28590));
  else if (cp == 28605) es_copy(buf, cap, "ISO-8859-15");
  else if (cp == 932) es_copy(buf, cap, "Shift-JIS");
  else if (cp == 936) es_copy(buf, cap, "GBK");
  else if (cp == 949) es_copy(buf, cap, "Korean 949");
  else if (cp == 950) es_copy(buf, cap, "Big5");
  else es_fmt(buf, cap, "Code page %u", (u32)cp);
  return buf;
}

const char* enc_label(const EncSpec& e, char* buf, u32 cap)
{
  if (!buf || cap == 0) return "";
  switch (e.kind) {
  case EK_UTF8: es_copy(buf, cap, e.bom ? "UTF-8 BOM" : "UTF-8"); return buf;
  case EK_UTF16LE: es_copy(buf, cap, e.bom ? "UTF-16 LE" : "UTF-16 LE no BOM"); return buf;
  case EK_UTF16BE: es_copy(buf, cap, e.bom ? "UTF-16 BE" : "UTF-16 BE no BOM"); return buf;
  default: break;
  }
  if (e.cp == g_acp) es_copy(buf, cap, "ANSI");
  else if (e.cp == g_oemcp) es_copy(buf, cap, "OEM");
  else enc_cp_name(e.cp, buf, cap);
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
  else if (es_nicmp(token, "cp", 2) == 0 && token[2] >= '0' && token[2] <= '9' &&
           es_parse_u32(token + 2, &cp, 10) == E_OK && cp > 0 && cp < 65536)
    e = enc_cp((u16)cp);
  else return E_SYNTAX;
  *out = e;
  return E_OK;
}

void enc_token(const EncSpec& e, char* buf, u32 cap)
{
  if (!buf || cap == 0) return;
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

// ---------------------------------------------------------------------------------------------------------------
// UTF-8 (strict: no overlongs, no surrogates, nothing above U+10FFFF; one bad marker per bad byte)

static u32 bad_marker_byte(ecp c, u8* out)    // ECP_BADBYTE -> its byte (1), else 0
{
  if (c >= 0x110000u && c < 0x110100u) { out[0] = (u8)(c - 0x110000u); return 1; }
  return 0;
}

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
        if (!eof) return 0;                       // need more bytes; the prefix is valid so far
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
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c < 0x800) { out[0] = (u8)(0xC0 | (c >> 6)); out[1] = (u8)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
      if (c >= 0xD800u && c <= 0xDFFFu) return 0;
      out[0] = (u8)(0xE0 | (c >> 12)); out[1] = (u8)(0x80 | ((c >> 6) & 0x3F)); out[2] = (u8)(0x80 | (c & 0x3F));
      return 3;
    }
    if (c < 0x110000u) {
      out[0] = (u8)(0xF0 | (c >> 18)); out[1] = (u8)(0x80 | ((c >> 12) & 0x3F));
      out[2] = (u8)(0x80 | ((c >> 6) & 0x3F)); out[3] = (u8)(0x80 | (c & 0x3F));
      return 4;
    }
    return bad_marker_byte(c, out);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// UTF-16 LE / BE (surrogate pairs; a lone surrogate is one ECP_BADUNIT of 2 bytes; a final odd byte is one
// ECP_BADBYTE)

class Utf16Codec : public Codec {
public:
  explicit Utf16Codec(int be) : be_(be)
  {
    kind = (u8)(be ? EK_UTF16BE : EK_UTF16LE); unit = 2; max_len = 4; ascii_compat = 0; self_sync = 1; pad = 0;
    cp = (u16)(be ? 1201 : 1200);
  }
  u16 get(const u8* p) const { return be_ ? (u16)((p[0] << 8) | p[1]) : (u16)(p[0] | (p[1] << 8)); }
  void put(u16 u, u8* out) const
  {
    if (be_) { out[0] = (u8)(u >> 8); out[1] = (u8)u; }
    else { out[0] = (u8)u; out[1] = (u8)(u >> 8); }
  }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u16 u, u2;
    if (avail < 2) {
      if (!eof) return 0;
      *out = ECP_BADBYTE(p[0]);                   // final odd byte
      return 1;
    }
    u = get(p);
    if (u < 0xD800 || u > 0xDFFF) { *out = u; return 2; }
    if (u >= 0xDC00) { *out = ECP_BADUNIT(u); return 2; }   // lone low surrogate
    if (avail < 4) {
      if (!eof) return 0;
      *out = ECP_BADUNIT(u); return 2;
    }
    u2 = get(p + 2);
    if (u2 < 0xDC00 || u2 > 0xDFFF) { *out = ECP_BADUNIT(u); return 2; }
    *out = 0x10000u + (((ecp)u - 0xD800u) << 10) + ((ecp)u2 - 0xDC00u);
    return 4;
  }
  u32 encode(ecp c, u8* out) const
  {
    if (c < 0x10000u) {
      if (c >= 0xD800u && c <= 0xDFFFu) return 0;
      put((u16)c, out);
      return 2;
    }
    if (c < 0x110000u) {
      c -= 0x10000u;
      put((u16)(0xD800u + (c >> 10)), out);
      put((u16)(0xDC00u + (c & 0x3FFu)), out + 2);
      return 4;
    }
    if (c >= 0x120000u && c < 0x130000u) { put((u16)(c - 0x120000u), out); return 2; }
    return bad_marker_byte(c, out);
  }
private:
  int be_;
};

// ---------------------------------------------------------------------------------------------------------------
// Single-byte code pages (generated tables)

class SbcsCodec : public Codec {
public:
  SbcsCodec() : t(0) { kind = EK_SBCS; unit = 1; max_len = 1; ascii_compat = 1; self_sync = 1; pad = 0; cp = 0; }
  void set(const SbcsTable* tbl) { t = tbl; cp = tbl->cp; }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u8 b = p[0];
    (void)avail; (void)eof;
    if (b < 0x80) *out = b;
    else *out = t->hi[b - 0x80] ? (ecp)t->hi[b - 0x80] : ECP_BADBYTE(b);
    return 1;
  }
  u32 encode(ecp c, u8* out) const
  {
    u32 lo, hi;
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c > 0xFFFF) return bad_marker_byte(c, out);
    lo = 0; hi = t->nrev;
    while (lo < hi) {
      u32 mid = (lo + hi) / 2;
      u16 v = t->rev_cp[mid];
      if (v == c) { out[0] = t->rev_b[mid]; return 1; }
      if (v < c) lo = mid + 1; else hi = mid;
    }
    return 0;
  }
  const SbcsTable* t;
};

// ---------------------------------------------------------------------------------------------------------------
// Provider-based code pages (DBCS, or a single-byte system code page without a built-in table)

class ProvCodec : public Codec {
public:
  ProvCodec() : p_(0) { kind = EK_DBCS; unit = 1; max_len = 2; ascii_compat = 0; self_sync = 0; pad = 0; cp = 0; }
  void set(u16 c, DbcsProvider* prov)
  {
    u32 b;
    int leads = 0, ascii = 1;
    p_ = prov;
    cp = c;
    for (b = 0; b < 256; b++) {
      if (prov->is_lead((u8)b)) leads = 1;
      else if (b < 0x80 && prov->decode_single((u8)b) != b) ascii = 0;
    }
    kind = (u8)(leads ? EK_DBCS : EK_SBCS);
    max_len = (u8)(leads ? 2 : 1);
    self_sync = (u8)(leads ? 0 : 1);
    ascii_compat = (u8)(leads ? 0 : ascii);
  }
  u32 decode(const u8* p, u32 avail, int eof, ecp* out) const
  {
    u8 b = p[0];
    ecp c;
    if (!p_->is_lead(b)) {
      c = p_->decode_single(b);
      *out = ECP_IS_BAD(c) ? ECP_BADBYTE(b) : c;
      return 1;
    }
    if (avail < 2) {
      if (!eof) return 0;
      *out = ECP_BADBYTE(b);
      return 1;
    }
    c = p_->decode_pair(b, p[1]);
    if (ECP_IS_BAD(c)) { *out = ECP_BADBYTE(b); return 1; }   // bad pair: the lead byte alone is the bad char
    *out = c;
    return 2;
  }
  u32 encode(ecp c, u8* out) const
  {
    if (c >= 0x110000u) return bad_marker_byte(c, out);
    return p_->encode(c, out);
  }
  DbcsProvider* p_;
};

// ---------------------------------------------------------------------------------------------------------------
// Registry

enum { SBCS_MAX = 48, PROV_MAX = 8 };

static Utf8Codec g_utf8;
static Utf16Codec g_utf16le(0);
static Utf16Codec g_utf16be(1);
static SbcsCodec g_sbcs[SBCS_MAX];
static u32 g_sbcs_n = 0;
static ProvCodec g_prov[PROV_MAX];
static u16 g_prov_cp[PROV_MAX];               // 0 = free slot

static int is_dbcs_cp(u16 cp)
{
  u32 i;
  if (cp == 932 || cp == 936 || cp == 949 || cp == 950 || cp == 1361) return 1;
  for (i = 0; i < PROV_MAX; i++)
    if (g_prov_cp[i] == cp && g_prov[i].kind == EK_DBCS) return 1;
  return 0;
}

static void sbcs_setup()
{
  u32 i, n;
  if (g_sbcs_n) return;
  n = g_sbcs_table_count < (u32)SBCS_MAX ? g_sbcs_table_count : (u32)SBCS_MAX;
  for (i = 0; i < n; i++) g_sbcs[i].set(&g_sbcs_tables[i]);
  g_sbcs_n = n;
}

int codec_init()
{
  sbcs_setup();
  return E_OK;
}

static const Codec* find_sbcs(u16 cp)
{
  u32 i;
  sbcs_setup();                                // normally done by codec_init (tests may skip it)
  for (i = 0; i < g_sbcs_n; i++) if (g_sbcs[i].cp == cp) return &g_sbcs[i];
  return 0;
}

static const Codec* find_prov(u16 cp)
{
  u32 i;
  for (i = 0; i < PROV_MAX; i++) if (g_prov_cp[i] == cp) return &g_prov[i];
  return 0;
}

const Codec* codec_get(const EncSpec& e)
{
  const Codec* c;
  switch (e.kind) {
  case EK_UTF8: return &g_utf8;
  case EK_UTF16LE: return &g_utf16le;
  case EK_UTF16BE: return &g_utf16be;
  case EK_SBCS:
    c = find_sbcs(e.cp);
    return c ? c : find_prov(e.cp);
  case EK_DBCS: return find_prov(e.cp);
  default: break;
  }
  return 0;
}

int codec_sbcs_supported(u16 cp)
{
  return find_sbcs(cp) ? 1 : 0;
}

void codec_break_spec(const EncSpec& e, int cr_mode, BreakSpec* bs)
{
  u8 b = (u8)(cr_mode ? 0x0D : 0x0A);
  bs->pad = 0;
  if (e.kind == EK_UTF16LE) { bs->unit = 2; bs->b0 = b; bs->b1 = 0; }
  else if (e.kind == EK_UTF16BE) { bs->unit = 2; bs->b0 = 0; bs->b1 = b; }
  else { bs->unit = 1; bs->b0 = b; bs->b1 = 0; }
}

// Registers (or replaces) the provider for cp; p == NULL removes the registration (tests). The kind reported by
// enc_cp/codec_get follows the provider: no lead bytes = a single-byte code page.
int codec_register_dbcs(u16 cp, DbcsProvider* p)
{
  u32 i, free_slot = PROV_MAX;
  if (cp == 0 || cp == 65001 || cp == 1200 || cp == 1201) return E_RANGE;
  for (i = 0; i < PROV_MAX; i++) {
    if (g_prov_cp[i] == cp) {
      if (!p) { g_prov_cp[i] = 0; g_prov[i].p_ = 0; return E_OK; }
      g_prov[i].set(cp, p);
      return E_OK;
    }
    if (g_prov_cp[i] == 0 && free_slot == PROV_MAX) free_slot = i;
  }
  if (!p) return E_OK;
  if (free_slot == PROV_MAX) return E_NOMEM;
  g_prov[free_slot].set(cp, p);
  g_prov_cp[free_slot] = cp;
  return E_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Helpers

u32 codec_decode_buf(const Codec* c, const u8* p, u32 n, int eof, ecp* out, u32 cap, u32* bytes_used)
{
  u32 i = 0, k = 0;
  if (c->ascii_compat) {
    while (i < n && k < cap) {
      u32 used;
      if (p[i] < 0x80) { out[k++] = p[i++]; continue; }
      used = c->decode(p + i, n - i, eof, out + k);
      if (used == 0) break;
      i += used;
      k++;
    }
  } else {
    while (i < n && k < cap) {
      u32 used = c->decode(p + i, n - i, eof, out + k);
      if (used == 0) break;
      i += used;
      k++;
    }
  }
  if (bytes_used) *bytes_used = i;
  return k;
}

// Encodes s[0..n) and appends to out. Unrepresentable characters: with subst != 0 each becomes subst (or '?' if
// subst itself is not representable) and is counted; with subst == 0 the function stops at the first one and returns
// E_RANGE (out holds the bytes before it). *unrepresentable = count (may be NULL). E_NOMEM leaves out partly filled.
int codec_encode_str(const Codec* c, const ecp* s, u32 n, EBytes* out, u32* unrepresentable, ecp subst)
{
  u32 i, bad = 0;
  u8 buf[8];
  if (unrepresentable) *unrepresentable = 0;
  if (out->reserve(out->size() + (n < 0x10000000u ? n : 0)) != E_OK) return E_NOMEM;
  for (i = 0; i < n; i++) {
    ecp ch = s[i];
    u32 len;
    if (ch < 0x80 && c->ascii_compat) { buf[0] = (u8)ch; len = 1; }
    else len = c->encode(ch, buf);
    if (len == 0) {
      bad++;
      if (unrepresentable) *unrepresentable = bad;
      if (!subst) return E_RANGE;
      len = c->encode(subst, buf);
      if (len == 0) len = c->encode('?', buf);
    }
    if (len && out->append(buf, len) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

// Start of the character before pos (DESIGN 7.1). floor is a known character boundary <= pos. Self-synchronising
// codecs: the largest k <= max_len (in whole units from floor) such that decoding at pos - k consumes exactly k bytes;
// if none, pos - unit (pos is inside a character). Others (DBCS): decode forward from floor.
epos codec_prev(ByteSource* s, const Codec* c, epos pos, epos floor)
{
  u8 buf[16];
  epos sz = s->size();
  if (pos > sz) pos = sz;
  if (pos <= floor) return floor;
  if (c->self_sync) {
    epos back, wstart, wend;
    u32 wn, k;
    if (c->unit > 1 && ((pos - floor) % c->unit) != 0) return pos - (pos - floor) % c->unit;   // odd trailing byte
    if (c->max_len == 1) return pos - 1;
    back = emin<epos>((epos)c->max_len, pos - floor);
    wstart = pos - back;
    wend = sz - pos > (epos)c->max_len ? pos + c->max_len : sz;
    wn = wend - wstart;
    if (s->read(wstart, buf, wn) != E_OK) return pos - emin<epos>((epos)c->unit, pos - floor);
    if (c->ascii_compat && buf[back - 1] < 0x80) return pos - 1;
    for (k = back; k >= c->unit; k -= c->unit) {
      ecp cp;
      u32 off = back - k;
      if (c->decode(buf + off, wn - off, wend >= sz, &cp) == k) return pos - k;
    }
    return pos - c->unit;
  }
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

// ---------------------------------------------------------------------------------------------------------------
// CharReader: decodes forward through an internal buffer refilled with ByteSource::read, so the source's get()
// pointer is never held between calls (other code may use the same source in between).

void CharReader::init(ByteSource* s, const Codec* c, epos pos, epos end)
{
  epos sz = s->size();
  s_ = s; c_ = c;
  end_ = end > sz ? sz : end;
  pos_ = pos > end_ ? end_ : pos;
  buf_pos_ = pos_;
  buf_len_ = 0;
}

int CharReader::next(ecp* cp, epos* at)
{
  u32 off, avail, used, need;
  if (pos_ >= end_) return 0;
  need = c_->max_len;
  if (need > end_ - pos_) need = end_ - pos_;
  if (pos_ < buf_pos_ || pos_ - buf_pos_ + need > buf_len_) {
    epos n = end_ - pos_;
    if (n > (epos)sizeof(buf_)) n = (epos)sizeof(buf_);
    if (s_->read(pos_, buf_, n) != E_OK) { buf_len_ = 0; return E_IO; }
    buf_pos_ = pos_;
    buf_len_ = n;
  }
  off = pos_ - buf_pos_;
  avail = buf_len_ - off;
  if (c_->ascii_compat && buf_[off] < 0x80) { *cp = buf_[off]; used = 1; }
  else {
    used = c_->decode(buf_ + off, avail, buf_pos_ + buf_len_ >= end_, cp);
    if (used == 0) { *cp = ECP_BADBYTE(buf_[off]); used = 1; }
  }
  if (at) *at = pos_;
  pos_ += used;
  return 1;
}

epos CharReader::pos() const
{
  return pos_;
}
