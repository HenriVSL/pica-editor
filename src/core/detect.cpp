// detect.cpp - MINIMAL detection from T00: BOMs (rule 1), NUL -> binary (rule 3), pure ASCII (rule 5) and exact EOL
// counting. T02 adds BOM-less UTF-16 (rule 2) and UTF-8 without BOM (rule 4).
// Owner after Wave 0: T02.
#include "detect.h"
#include <string.h>

int detect_eol(const u8* p, u32 n, const EncSpec& e, EolCounts* c)
{
  u32 i;
  c->crlf = c->lf = c->cr = 0;
  if (e.kind == EK_UTF16LE || e.kind == EK_UTF16BE) {
    int be = e.kind == EK_UTF16BE;
    u32 units = n / 2;
    for (i = 0; i < units; i++) {
      u16 u = be ? (u16)((p[2 * i] << 8) | p[2 * i + 1]) : (u16)(p[2 * i] | (p[2 * i + 1] << 8));
      if (u == 0x0A) c->lf++;
      else if (u == 0x0D) {
        u16 nx = 0;
        if (i + 1 < units) nx = be ? (u16)((p[2 * i + 2] << 8) | p[2 * i + 3]) : (u16)(p[2 * i + 2] | (p[2 * i + 3] << 8));
        if (nx == 0x0A) { c->crlf++; i++; }
        else c->cr++;
      }
    }
    return E_OK;
  }
  for (i = 0; i < n; i++) {
    if (p[i] == 0x0A) c->lf++;
    else if (p[i] == 0x0D) {
      if (i + 1 < n && p[i + 1] == 0x0A) { c->crlf++; i++; }
      else c->cr++;
    }
  }
  return E_OK;
}

EolStyle detect_dominant_eol(const EolCounts& c, EolStyle dflt)
{
  if (c.cr > 0 && c.crlf == 0 && c.lf == 0) return EOL_CR;
  if (c.crlf == 0 && c.lf == 0) return dflt;
  return c.lf > c.crlf ? EOL_LF : EOL_CRLF;
}

int detect(const u8* p, u32 n, int whole_file, const DetectOpts& o, DetectResult* r)
{
  u32 i, kinds;
  (void)whole_file;
  memset(r, 0, sizeof(*r));
  r->enc = o.ansi;
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { r->enc = enc_utf8(1); r->bom_len = 3; }
  else if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0 && p[3] == 0) { r->utf32 = 1; r->binary = 1; }
  else if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0xFE && p[3] == 0xFF) { r->utf32 = 1; r->binary = 1; }
  else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { r->enc = enc_utf16le(1); r->bom_len = 2; }
  else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { r->enc = enc_utf16be(1); r->bom_len = 2; }
  else {
    int ascii = 1;
    for (i = 0; i < n; i++) {
      if (p[i] == 0) { r->binary = 1; }
      if (p[i] >= 0x80) ascii = 0;
    }
    r->pure_ascii = (u8)(ascii && !r->binary);
  }
  detect_eol(p + r->bom_len, n - r->bom_len, r->enc, &r->eol);
  r->eol_style = detect_dominant_eol(r->eol, o.default_eol);
  kinds = (r->eol.crlf > 0) + (r->eol.lf > 0) + (r->eol.cr > 0);
  r->eol_mixed = (u8)(kinds > 1);
  return E_OK;
}

int detect_is_binary(const u8* p, u32 n)
{
  u32 i;
  if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) return 0;
  for (i = 0; i < n; i++) if (p[i] == 0) return 1;
  return 0;
}
