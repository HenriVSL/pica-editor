// rxpre.cpp - literal prefilter (DESIGN 11.3): Boyer-Moore-Horspool over raw ByteSource spans, exact or with a
// per-encoding case-folding key table. Candidates are verified by the matcher unless the pattern is a pure literal
// and the filter is exact. Owner: T03.
#include "regex_int.h"
#include <string.h>

void rxp_init(RxProgress* g, ProgressCb* cb, epos base, epos total)
{
  g->cb = cb;
  g->base = base;
  g->total = total;
  g->next = base + 65536;
  if (g->next < base) g->next = EPOS_NONE;
}

// Non-ASCII characters whose simple case folding may be ASCII (U+212A KELVIN SIGN -> k, U+017F LONG S -> s, and
// U+0130 / U+0131 depending on the tables). t_regex checks that no other character folds into ASCII.
static const ecp k_fold_into_ascii[] = { 0x130, 0x131, 0x17F, 0x212A, 0 };

void rxpre_unsafe_letters(const Codec* c, u32 mask[4])
{
  u32 k;
  u8 buf[8];
  mask[0] = mask[1] = mask[2] = mask[3] = 0;
  for (k = 0; k_fold_into_ascii[k]; k++) {
    ecp x = k_fold_into_ascii[k], f = rx_fold(x);
    if (f < 128 && c->encode(x, buf)) mask[f >> 5] |= 1u << (f & 31);
  }
}

int rxpre_encodable(const Codec* c, ecp x, int icase)
{
  u8 buf[8];
  u32 k;
  if (c->encode(x, buf)) return 1;
  if (!icase || ECP_IS_BAD(x)) return 0;
  if (c->encode(uc_upper(x), buf) || c->encode(uc_lower(x), buf) || c->encode(uc_fold(x), buf)) return 1;
  if (c->encode(uc_upper(uc_fold(x)), buf) || c->encode(uc_lower(uc_fold(x)), buf)) return 1;
  for (k = 0; k_fold_into_ascii[k]; k++)
    if (rx_fold(k_fold_into_ascii[k]) == rx_fold(x) && c->encode(k_fold_into_ascii[k], buf)) return 1;
  if (x == 0x3C3 && c->encode(0x3C2, buf)) return 1;     // final sigma folds to sigma
  if (x == 0xDF && c->encode(0x1E9E, buf)) return 1;     // capital sharp s folds to sharp s
  if (x == 0xE5 && c->encode(0x212B, buf)) return 1;     // angstrom sign
  if (x == 0x3C9 && c->encode(0x2126, buf)) return 1;    // ohm sign
  return 0;
}

static void make_shift(RxPre* pf)
{
  u32 j, b, m = pf->m;
  for (b = 0; b < 256; b++) pf->shift[b] = m;
  if (!pf->keyed) {
    for (j = 0; j + 1 < m; j++) pf->shift[pf->lit[j]] = m - 1 - j;
  } else {
    for (j = 0; j + 1 < m; j++)
      for (b = 0; b < 256; b++)
        if (pf->key[b] == pf->lk[j]) pf->shift[b] = m - 1 - j;
  }
}

void rxpre_bytes(RxPre* pf, const u8* bytes, u32 m)
{
  memset(pf, 0, sizeof(*pf));
  if (m == 0 || m > RXPRE_MAX) return;
  memcpy(pf->lit, bytes, m);
  pf->m = m;
  pf->ok = 1;
  pf->full = 1;
  pf->align = 1;
  make_shift(pf);
}

void rxpre_build(RxPre* pf, const ecp* lit, u32 plen, int icase, const Codec* c)
{
  u32 j, b;
  memset(pf, 0, sizeof(*pf));
  pf->align = 1;
  if (!c || plen == 0) return;
  if (!icase) {
    u32 m = 0;
    int truncated = 0;
    for (j = 0; j < plen; j++) {
      u8 buf[8];
      u32 n = c->encode(lit[j], buf);
      if (n == 0) { pf->ok = 1; pf->never = 1; return; }    // not representable: the pattern can't occur
      if (m + n > RXPRE_MAX) { truncated = 1; break; }
      memcpy(pf->lit + m, buf, n);
      m += n;
    }
    if (c->kind == EK_DBCS || (!c->self_sync && c->max_len > 1)) return;  // trail bytes look like ASCII
    if (c->kind == EK_UTF8 && pf->lit[0] >= 0x80 && pf->lit[0] <= 0xBF) return;
    if (c->unit == 2) {
      u32 u0 = c->kind == EK_UTF16BE ? ((u32)pf->lit[0] << 8 | pf->lit[1]) : ((u32)pf->lit[1] << 8 | pf->lit[0]);
      if (u0 >= 0xDC00 && u0 <= 0xDFFF) return;
      pf->align = 2;
    }
    pf->m = m;
    pf->keyed = 0;
    pf->full = (u8)(truncated ? 0 : 1);
    pf->ok = 1;
    make_shift(pf);
    return;
  }
  // case-insensitive
  if (c->kind == EK_SBCS && c->unit == 1 && c->max_len == 1) {
    for (b = 0; b < 256; b++) {
      u8 by = (u8)b;
      ecp cp = 0;
      if (c->decode(&by, 1, 1, &cp) != 1) cp = ECP_BADBYTE(b);
      pf->key[b] = rx_fold(cp);
    }
    for (j = 0; j < plen && j < RXPRE_MAX; j++) {
      ecp x = lit[j];
      for (b = 0; b < 256; b++) if (pf->key[b] == x) break;
      if (b == 256) { pf->ok = 1; pf->never = 1; return; }
      pf->lk[j] = x;
    }
    pf->m = j;
    pf->full = (u8)(j == plen ? 1 : 0);
  } else if (c->ascii_compat && c->unit == 1 && c->self_sync) {   // UTF-8: ASCII-only part of the prefix
    u32 unsafe[4];
    rxpre_unsafe_letters(c, unsafe);
    for (b = 0; b < 256; b++) pf->key[b] = b < 128 ? (u32)rx_fold(b) : 0x80000000u | b;
    for (j = 0; j < plen && j < RXPRE_MAX; j++) {
      ecp x = lit[j];
      if (x >= 128 || (unsafe[x >> 5] >> (x & 31)) & 1u) break;
      pf->lk[j] = x;
    }
    if (j == 0) return;
    pf->m = j;
    pf->full = (u8)(j == plen ? 1 : 0);
  } else {
    return;
  }
  pf->keyed = 1;
  pf->ok = 1;
  make_shift(pf);
}

static int aligned(const RxPre* pf, epos at, epos base)
{
  return pf->align == 1 || ((at - base) & 1) == 0;
}

static int cmp_at(const RxPre* pf, const u8* p)
{
  u32 j;
  if (!pf->keyed) return memcmp(p, pf->lit, pf->m) == 0;
  for (j = 0; j < pf->m; j++) if (pf->key[p[j]] != pf->lk[j]) return 0;
  return 1;
}

// Candidates at offsets 0..lim of p (p has lim + m bytes). Returns the offset or -1.
static i32 scan(const RxPre* pf, const u8* p, u32 lim, epos at, epos base)
{
  u32 m = pf->m, k = 0;
  if (!pf->keyed) {
    u8 last = pf->lit[m - 1];
    if (m == 1) {
      while (k <= lim) {
        const u8* f = (const u8*)memchr(p + k, last, lim - k + 1);
        if (!f) return -1;
        k = (u32)(f - p);
        if (aligned(pf, at + k, base)) return (i32)k;
        k++;
      }
      return -1;
    }
    while (k <= lim) {
      u8 b = p[k + m - 1];
      if (b == last && memcmp(p + k, pf->lit, m - 1) == 0 && aligned(pf, at + k, base)) return (i32)k;
      k += pf->shift[b];
    }
    return -1;
  }
  {
    u32 lastk = pf->lk[m - 1];
    while (k <= lim) {
      u8 b = p[k + m - 1];
      if (pf->key[b] == lastk) {
        i32 j = (i32)m - 2;
        while (j >= 0 && pf->key[p[k + (u32)j]] == pf->lk[j]) j--;
        if (j < 0 && aligned(pf, at + k, base)) return (i32)k;
      }
      k += pf->shift[b];
    }
    return -1;
  }
}

int rxpre_find(RxPre* pf, ByteSource* s, epos from, epos start_max, epos end, epos base, epos* q, RxProgress* g)
{
  u32 m = pf->m;
  epos last, pos = from;
  if (!pf->ok || pf->never || m == 0) return 0;
  if (end > s->size()) end = s->size();
  if (end < m) return 0;
  last = end - m;
  if (start_max < last) last = start_max;
  while (pos <= last) {
    epos avail = 0;
    const u8* p = s->get(pos, &avail);
    int r;
    if (!p || avail == 0) return s->err() != E_OK ? s->err() : E_IO;
    if (avail >= m) {
      u32 lim = avail - m;
      i32 k;
      if (lim > last - pos) lim = last - pos;
      if (lim > 262143) lim = 262143;
      k = scan(pf, p, lim, pos, base);
      if (k >= 0) { *q = pos + (u32)k; return 1; }
      pos += lim + 1;
    } else {
      u8 tmp[RXPRE_MAX];
      if (s->read(pos, tmp, m) != E_OK) return s->err() != E_OK ? s->err() : E_IO;
      if (aligned(pf, pos, base) && cmp_at(pf, tmp)) { *q = pos; return 1; }
      pos++;
    }
    if (g && (r = rxp_tick(g, pos)) != E_OK) return r;
    if (pos == 0) break;             // wrapped (pos was EPOS_MAX)
  }
  return 0;
}
