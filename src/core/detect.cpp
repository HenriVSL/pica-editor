// detect.cpp - encoding and line-end detection (DESIGN 6.10, 12.1). The caller passes the sample (the first 64 KB,
// or the whole file if smaller: whole_file = 1); everything given is examined.
// Rules in order: 1 BOM (UTF-8, UTF-32 LE/BE -> binary, UTF-16 LE/BE); 2 BOM-less UTF-16 by the NUL pattern (if
// enabled, >= 16 bytes); 3 any NUL -> binary (ANSI); 4 valid UTF-8 with a multi-byte sequence -> UTF-8 (if enabled; a
// sequence cut by the end of a partial sample is allowed); 5 pure ASCII -> ANSI + pure_ascii; 6 ANSI.
// EOL counts are in code units of the detected encoding; in a partial sample a CR in the last unit is not counted
// (its LF may follow beyond the sample).
// Owner: T02.
#include "detect.h"
#include <string.h>

static u16 unit_at(const u8* p, int be)
{
  return be ? (u16)((p[0] << 8) | p[1]) : (u16)(p[0] | (p[1] << 8));
}

static void count_eol(const u8* p, u32 n, const EncSpec& e, int partial, EolCounts* c)
{
  u32 i;
  c->crlf = c->lf = c->cr = 0;
  if (e.kind == EK_UTF16LE || e.kind == EK_UTF16BE) {
    int be = e.kind == EK_UTF16BE;
    u32 units = n / 2;
    for (i = 0; i < units; i++) {
      u16 u = unit_at(p + 2 * i, be);
      if (u == 0x0A) c->lf++;
      else if (u == 0x0D) {
        if (i + 1 < units) {
          if (unit_at(p + 2 * i + 2, be) == 0x0A) { c->crlf++; i++; }
          else c->cr++;
        } else if (!partial) c->cr++;
      }
    }
    return;
  }
  // byte code pages: 0x0A/0x0D never occur inside a multi-byte character (UTF-8, DBCS trail bytes are >= 0x40)
  for (i = 0; i < n; i++) {
    u8 b = p[i];
    if (b > 0x0D) continue;
    if (b == 0x0A) c->lf++;
    else if (b == 0x0D) {
      if (i + 1 < n) {
        if (p[i + 1] == 0x0A) { c->crlf++; i++; }
        else c->cr++;
      } else if (!partial) c->cr++;
    }
  }
}

int detect_eol(const u8* p, u32 n, const EncSpec& e, EolCounts* c)
{
  if (!c) return E_RANGE;
  if (!p) n = 0;
  count_eol(p, n, e, 0, c);
  return E_OK;
}

EolStyle detect_dominant_eol(const EolCounts& c, EolStyle dflt)
{
  if (c.cr > 0 && c.crlf == 0 && c.lf == 0) return EOL_CR;
  if (c.crlf == 0 && c.lf == 0) return dflt;
  return c.lf > c.crlf ? EOL_LF : EOL_CRLF;
}

// Rule 2: returns 1 for UTF-16 LE, 2 for UTF-16 BE, 0 otherwise.
static int utf16_nul_pattern(const u8* p, u32 n)
{
  u32 pairs = n / 2, i, even = 0, odd = 0;
  if (n < 16) return 0;
  for (i = 0; i < pairs; i++) {
    if (p[2 * i] == 0) even++;
    if (p[2 * i + 1] == 0) odd++;
  }
  // ratios: odd >= 40 % and even <= 5 % (integer arithmetic; pairs <= 2^31)
  if ((u64)odd * 100 >= (u64)pairs * 40 && (u64)even * 100 <= (u64)pairs * 5) return 1;
  if ((u64)even * 100 >= (u64)pairs * 40 && (u64)odd * 100 <= (u64)pairs * 5) return 2;
  return 0;
}

// Rule 4 helper: 1 = valid UTF-8 with at least one multi-byte sequence, 0 = not. A sequence cut by the end of a
// partial sample counts as valid (it is not counted as a multi-byte sequence).
static int utf8_check(const u8* p, u32 n, int whole_file)
{
  const Codec* c = codec_get(enc_utf8(0));
  u32 i = 0;
  int multi = 0;
  while (i < n) {
    ecp cp;
    u32 used;
    if (p[i] < 0x80) { i++; continue; }
    used = c->decode(p + i, n - i, whole_file, &cp);
    if (used == 0) break;                         // truncated at the end of a partial sample
    if (ECP_IS_BAD(cp)) return 0;
    multi = 1;
    i += used;
  }
  return multi;
}

int detect(const u8* p, u32 n, int whole_file, const DetectOpts& o, DetectResult* r)
{
  u32 i, start, kinds;
  int u16kind;
  if (!r) return E_RANGE;
  memset(r, 0, sizeof(*r));
  r->enc = o.ansi;
  if (!p) n = 0;
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { r->enc = enc_utf8(1); r->bom_len = 3; }
  else if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) { r->utf32 = 1; r->binary = 1; }
  else if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) { r->utf32 = 1; r->binary = 1; }
  else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { r->enc = enc_utf16le(1); r->bom_len = 2; }
  else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { r->enc = enc_utf16be(1); r->bom_len = 2; }
  else if (o.utf16_nobom && (u16kind = utf16_nul_pattern(p, n)) != 0) {
    r->enc = u16kind == 1 ? enc_utf16le(0) : enc_utf16be(0);
  } else {
    int ascii = 1, nul = 0;
    for (i = 0; i < n; i++) {
      if (p[i] >= 0x80) ascii = 0;
      else if (p[i] == 0) nul = 1;
    }
    if (nul) r->binary = 1;
    else if (ascii) r->pure_ascii = 1;
    else if (o.utf8_nobom && utf8_check(p, n, whole_file)) r->enc = enc_utf8(0);
  }
  start = r->bom_len;
  count_eol(p + start, n - start, r->enc, !whole_file, &r->eol);
  r->eol_style = detect_dominant_eol(r->eol, o.default_eol);
  // Mixed as the Document counts it (DESIGN 7.2): lone CRs count only in CR mode, where they are the only kind.
  kinds = (r->eol.crlf > 0) + (r->eol.lf > 0);
  r->eol_mixed = (u8)(kinds > 1);
  return E_OK;
}

int detect_is_binary(const u8* p, u32 n)
{
  if (!p || n == 0) return 0;
  if (n >= 4 && ((p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) ||
                 (p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF))) return 1;   // UTF-32
  if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) return 0;
  if (memchr(p, 0, n) == 0) return 0;
  return utf16_nul_pattern(p, n) ? 0 : 1;
}
