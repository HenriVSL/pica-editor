// winenc.cpp - Windows code pages (DESIGN 8.3, 12, ARCH 3.3/3.8): DBCS providers built from MultiByteToWideChar,
// the input-locale code page for WM_CHAR bytes, ANSI <-> ecp for UI strings and paths, CF_TEXT <-> ecp for clip.cpp.
//
// Providers are built eagerly at registration (thread-safe afterwards) and only for installed code pages
// (IsValidCodePage): 932/936/949/950 and the system ACP/OEMCP when the core has no table for them. A provider never
// dies (registered codecs point at it). Strings in a code page with a codec (all built-in SBCS tables, registered
// providers, UTF-8/16) are converted by the codec: undefined bytes become ECP_BADBYTE and encode back exactly; other
// code pages go through MultiByteToWideChar / WideCharToMultiByte.
// Owner: T02.
#include <windows.h>
#include <stdlib.h>
#include "winenc.h"
#include "winenc_int.h"
#include "codec.h"
#include "estr.h"

// ---------------------------------------------------------------------------------------------------------------
// Provider

static void* heap_alloc(u32 n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void heap_free(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static int cmp_u32(const void* a, const void* b)
{
  u32 x = *(const u32*)a, y = *(const u32*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

class WinProvider : public DbcsProvider {
public:
  WinProvider() : cp_(0), rows_(0), enc_(0), nenc_(0) { memset(rowidx_, 0, sizeof(rowidx_)); }
  ~WinProvider() { heap_free(rows_); heap_free(enc_); }
  int is_lead(u8 b) const { return rowidx_[b] != 0; }
  ecp decode_pair(u8 lead, u8 trail) const
  {
    u16 v;
    if (!rowidx_[lead]) return ECP_BADBYTE(lead);
    v = rows_[(u32)(rowidx_[lead] - 1) * 256 + trail];
    return v ? (ecp)v : ECP_BADBYTE(lead);
  }
  ecp decode_single(u8 b) const { return single_[b]; }
  u32 encode(ecp c, u8* out) const
  {
    u32 lo = 0, hi = nenc_;
    if (c > 0xFFFF) return 0;
    while (lo < hi) {
      u32 mid = (lo + hi) / 2;
      u32 key = enc_[mid] >> 16;
      if (key == c) {
        u32 v = enc_[mid] & 0xFFFF;
        if (v > 0xFF) { out[0] = (u8)(v >> 8); out[1] = (u8)v; return 2; }
        out[0] = (u8)v;
        return 1;
      }
      if (key < c) lo = mid + 1; else hi = mid;
    }
    return 0;
  }
  int build(u16 cp);
private:
  u16 cp_;
  u8 rowidx_[256];          // 0 = not a lead byte, else 1 + row number
  ecp single_[256];         // decode of a non-lead byte (ECP_BADBYTE if undefined)
  u16* rows_;               // 256 entries per lead byte: decoded pair, 0 = invalid
  u32* enc_;                // sorted (code point << 16) | bytes (pair = lead << 8 | trail)
  u32 nenc_;
  WinProvider(const WinProvider&); WinProvider& operator=(const WinProvider&);
};

static int is_surrogate(WCHAR w) { return w >= 0xD800 && w <= 0xDFFF; }

int WinProvider::build(u16 cp)
{
  CPINFO ci;
  DWORD flags = MB_ERR_INVALID_CHARS;
  WCHAR w[4];
  char s[2];
  u32 b, t, nleads = 0, k, i, j, maxenc;
  int dbcs;
  if (!IsValidCodePage(cp) || !GetCPInfo(cp, &ci)) return E_UNSUPPORTED;
  cp_ = cp;
  dbcs = ci.MaxCharSize > 1;
  s[0] = 'A';
  if (MultiByteToWideChar(cp, flags, s, 1, w, 4) == 0 && GetLastError() == ERROR_INVALID_FLAGS) flags = 0;
  for (b = 0; b < 256; b++) {
    if (dbcs && IsDBCSLeadByteEx(cp, (BYTE)b)) { rowidx_[b] = (u8)(++nleads); single_[b] = ECP_BADBYTE(b); continue; }
    s[0] = (char)b;
    if (MultiByteToWideChar(cp, flags, s, 1, w, 4) == 1 && !is_surrogate(w[0])) single_[b] = w[0];
    else single_[b] = ECP_BADBYTE(b);
  }
  if (nleads) {
    rows_ = (u16*)heap_alloc(nleads * 256 * 2);
    if (!rows_) return E_NOMEM;
    for (b = 0; b < 256; b++) {
      if (!rowidx_[b]) continue;
      for (t = 1; t < 256; t++) {
        s[0] = (char)b; s[1] = (char)t;
        if (MultiByteToWideChar(cp, flags, s, 2, w, 4) == 1 && w[0] != 0 && !is_surrogate(w[0]))
          rows_[(rowidx_[b] - 1) * 256 + t] = w[0];
      }
    }
  }
  maxenc = 256 + nleads * 256;
  enc_ = (u32*)heap_alloc(maxenc * 4);
  if (!enc_) return E_NOMEM;
  k = 0;
  for (b = 0; b < 256; b++) if (!rowidx_[b] && !ECP_IS_BAD(single_[b])) enc_[k++] = (single_[b] << 16) | b;
  for (b = 0; b < 256; b++) {
    if (!rowidx_[b]) continue;
    for (t = 0; t < 256; t++) {
      u16 v = rows_[(rowidx_[b] - 1) * 256 + t];
      if (v) enc_[k++] = ((u32)v << 16) | (b << 8) | t;
    }
  }
  qsort(enc_, k, sizeof(u32), cmp_u32);
  // duplicates (several byte sequences decode to one character): keep the one WideCharToMultiByte produces
  i = 0; j = 0;
  while (i < k) {
    u32 e = i + 1, keep = i, x;
    while (e < k && (enc_[e] >> 16) == (enc_[i] >> 16)) e++;
    if (e - i > 1) {
      char mb[4];
      int n;
      w[0] = (WCHAR)(enc_[i] >> 16);
      n = WideCharToMultiByte(cp, 0, w, 1, mb, 4, NULL, NULL);
      if (n == 1 || n == 2) {
        u32 want = n == 1 ? (u8)mb[0] : (((u32)(u8)mb[0] << 8) | (u8)mb[1]);
        for (x = i; x < e; x++) if ((enc_[x] & 0xFFFF) == want) { keep = x; break; }
      }
    }
    enc_[j++] = enc_[keep];
    i = e;
  }
  nenc_ = j;
  return E_OK;
}

DbcsProvider* winenc_provider_build(u16 cp)
{
  WinProvider* p = new WinProvider;
  if (!p) return 0;
  if (p->build(cp) != E_OK) { delete p; return 0; }
  return p;
}

void winenc_provider_free(DbcsProvider* p)
{
  delete p;
}

// ---------------------------------------------------------------------------------------------------------------
// Init, input locale

static int g_winenc_inited = 0;
static u16 g_input_cp = 0;

static void register_cp(u16 cp)
{
  DbcsProvider* p;
  if (cp == 0 || cp == 65001 || cp == 1200 || cp == 1201 || codec_sbcs_supported(cp)) return;
  if (codec_get(enc_cp(cp))) return;              // already registered
  p = winenc_provider_build(cp);
  if (!p) return;
  if (codec_register_dbcs(cp, p) != E_OK) winenc_provider_free(p);
}

int winenc_init()
{
  static const u16 k_dbcs[4] = { 932, 936, 949, 950 };
  u32 i;
  if (g_winenc_inited) return E_OK;
  g_winenc_inited = 1;
  register_cp((u16)GetACP());
  register_cp((u16)GetOEMCP());
  for (i = 0; i < 4; i++) register_cp(k_dbcs[i]);
  winenc_input_changed();
  return E_OK;
}

void winenc_input_changed()
{
  char buf[16];
  u32 cp = 0;
  LANGID lang = LOWORD((DWORD)GetKeyboardLayout(0));
  if (GetLocaleInfoA(MAKELCID(lang, SORT_DEFAULT), LOCALE_IDEFAULTANSICODEPAGE, buf, sizeof(buf)) > 0) {
    if (es_parse_u32(buf, &cp, 10) != E_OK) cp = 0;
  }
  if (cp == 0 || cp > 65535 || !IsValidCodePage(cp)) cp = GetACP();
  g_input_cp = (u16)cp;
}

u16 winenc_input_cp()
{
  if (!g_input_cp) winenc_input_changed();
  return g_input_cp;
}

int winenc_cp_valid(u16 cp)
{
  return IsValidCodePage(cp) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Conversions

int winenc_mb_to_ecp(u16 cp, const char* s, u32 n, Vec<ecp>* out)
{
  const Codec* c;
  WCHAR* w;
  int wn, i;
  if (!out) return E_RANGE;
  if (!s || n == 0) return E_OK;
  c = codec_get(enc_cp(cp));
  if (c) {
    u32 at = 0;
    if (out->reserve(out->size() + n) != E_OK) return E_NOMEM;
    while (at < n) {
      ecp v;
      u32 used = c->decode((const u8*)s + at, n - at, 1, &v);
      if (used == 0) { v = ECP_BADBYTE((u8)s[at]); used = 1; }
      out->push(v);
      at += used;
    }
    return E_OK;
  }
  if (n > 0x7FFFFFFFu) return E_TOOBIG;
  wn = MultiByteToWideChar(cp, 0, s, (int)n, NULL, 0);
  if (wn <= 0) return IsValidCodePage(cp) ? E_FORMAT : E_UNSUPPORTED;
  w = (WCHAR*)e_alloc((u32)wn * sizeof(WCHAR));
  if (!w) return E_NOMEM;
  wn = MultiByteToWideChar(cp, 0, s, (int)n, w, wn);
  for (i = 0; i < wn; i++) {
    ecp v = w[i];
    if (v >= 0xD800 && v <= 0xDBFF && i + 1 < wn && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
      v = 0x10000u + ((v - 0xD800u) << 10) + ((ecp)w[i + 1] - 0xDC00u);
      i++;
    } else if (v >= 0xD800 && v <= 0xDFFF) v = ECP_BADUNIT(v);
    if (out->push(v) != E_OK) { e_free(w); return E_NOMEM; }
  }
  e_free(w);
  return E_OK;
}

int winenc_ecp_to_mb(u16 cp, const ecp* s, u32 n, EBytes* out, u32* lost)
{
  const Codec* c;
  u32 i, bad = 0;
  u8 buf[8];
  if (lost) *lost = 0;
  if (!out) return E_RANGE;
  if (!s || n == 0) return E_OK;
  c = codec_get(enc_cp(cp));
  if (c) {
    for (i = 0; i < n; i++) {
      u32 len = c->encode(s[i], buf);
      if (len == 0) { bad++; len = c->encode('?', buf); }
      if (len && out->append(buf, len) != E_OK) { if (lost) *lost = bad; return E_NOMEM; }
    }
    if (lost) *lost = bad;
    return E_OK;
  }
  if (!IsValidCodePage(cp)) return E_UNSUPPORTED;
  for (i = 0; i < n; i++) {
    WCHAR w[2];
    int wn = 0, len = 0;
    BOOL used = FALSE;
    ecp v = s[i];
    if (v < 0x10000u && !(v >= 0xD800u && v <= 0xDFFFu)) { w[0] = (WCHAR)v; wn = 1; }
    else if (v >= 0x10000u && v < 0x110000u) {
      v -= 0x10000u;
      w[0] = (WCHAR)(0xD800u + (v >> 10)); w[1] = (WCHAR)(0xDC00u + (v & 0x3FFu)); wn = 2;
    } else if (v >= 0x110000u && v < 0x110100u) { buf[0] = (u8)(v - 0x110000u); len = 1; }
    if (wn) {
      len = WideCharToMultiByte(cp, 0, w, wn, (char*)buf, sizeof(buf), "?", &used);
      if (len <= 0 || used) len = 0;
    }
    if (len <= 0) { bad++; buf[0] = '?'; len = 1; }
    if (out->append(buf, (u32)len) != E_OK) { if (lost) *lost = bad; return E_NOMEM; }
  }
  if (lost) *lost = bad;
  return E_OK;
}

int winenc_ansi_to_ecp(const char* s, u32 n, Vec<ecp>* out)
{
  return winenc_mb_to_ecp((u16)GetACP(), s, n, out);
}

int winenc_ecp_to_ansi(const ecp* s, u32 n, EBytes* out, u32* lost)
{
  return winenc_ecp_to_mb((u16)GetACP(), s, n, out, lost);
}
