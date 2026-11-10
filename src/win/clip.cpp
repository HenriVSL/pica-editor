// clip.cpp - clipboard (DESIGN 9.6, ARCH 3.5). Owner: T12.
//
// Copy puts three formats on the clipboard:
//   APP_RAWFMT ("Pica.RawBytes"): header (magic, byte count, EncSpec) + the exact document bytes (NULs, invalid
//              bytes, any encoding) - used by Pica's own paste;
//   CF_UNICODETEXT: the text (hex pane: "4D 5A 90" hex text, 16 bytes per line), line breaks as CRLF, bad
//              characters as U+FFFD;
//   CF_TEXT: the same text in the system ANSI code page ('?' for what it can't store).
// Paste reads RawBytes first, then CF_UNICODETEXT (works on Win98: the system keeps it for ANSI programs too), then
// CF_TEXT (system ANSI code page). Copies and pastes above 16 MB are refused (E_TOOBIG).
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "clip.h"
#include "winenc.h"
#include "hexfmt.h"
#include <string.h>

enum { CLIP_MAX = 16u * 1024u * 1024u, CLIP_MAGIC = 0x31425250u /* "PRB1" */ };
struct ClipRawHdr { u32 magic; u32 size; u8 kind; u8 bom; u16 cp; };

static UINT clip_rawfmt()
{
  static UINT fmt = 0;
  if (!fmt) fmt = RegisterClipboardFormatA(APP_RAWFMT);
  return fmt;
}

static int clip_open()
{
  int i;
  for (i = 0; i < 20; i++) {                     // another program may hold it for a moment
    if (OpenClipboard(g_app.frame)) return 1;
    Sleep(10);
  }
  return 0;
}

static HGLOBAL clip_alloc(const void* p, u32 n)
{
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, n ? n : 1);
  void* q;
  if (!h) return 0;
  q = GlobalLock(h);
  if (!q) { GlobalFree(h); return 0; }
  if (n) memcpy(q, p, n);
  GlobalUnlock(h);
  return h;
}

// Text builder: code points -> UTF-16 (CF_UNICODETEXT) and a code point list for the ANSI conversion.
struct ClipText {
  Vec<u16> w;          // UTF-16 with the terminating 0 added at the end
  Vec<ecp> cps;        // same text as code points (for CF_TEXT)
  int put(ecp c) {
    if (ECP_IS_BAD(c)) c = ECP_REPLACEMENT;
    if (cps.push(c) != E_OK) return E_NOMEM;
    if (c >= 0x10000) {
      u32 v = c - 0x10000;
      if (w.push((u16)(0xD800 + (v >> 10))) != E_OK) return E_NOMEM;
      return w.push((u16)(0xDC00 + (v & 0x3FF)));
    }
    return w.push((u16)c);
  }
};

// Decoded text of [lo, hi) with line breaks as CRLF.
static int clip_build_text(Document* d, epos lo, epos hi, ClipText* out)
{
  CharReader r;
  ecp c, prev = 0;
  epos at;
  int rc, cr_mode = d->eol_style() == EOL_CR;
  r.init(d->bytes(), d->codec(), lo, hi);
  while ((rc = r.next(&c, &at)) > 0) {
    if (!cr_mode && c == '\n' && prev != '\r') { if (out->put('\r') != E_OK) return E_NOMEM; }
    if (out->put(c) != E_OK) return E_NOMEM;
    if (cr_mode && c == '\r') { if (out->put('\n') != E_OK) return E_NOMEM; }
    prev = c;
  }
  return rc < 0 ? rc : E_OK;
}

static int clip_build_hex(Document* d, epos lo, epos hi, ClipText* out)
{
  u8 buf[256];
  epos p = lo;
  u32 k = 0;
  while (p < hi) {
    u32 n = hi - p > sizeof(buf) ? (u32)sizeof(buf) : hi - p, i;
    if (d->read(p, buf, n) != E_OK) return E_IO;
    for (i = 0; i < n; i++, k++) {
      char hx[2];
      if (k > 0) {
        if (k % HEX_BPR == 0) { if (out->put('\r') != E_OK || out->put('\n') != E_OK) return E_NOMEM; }
        else if (out->put(' ') != E_OK) return E_NOMEM;
      }
      hex_byte_text(buf[i], hx);
      if (out->put((ecp)(u8)hx[0]) != E_OK || out->put((ecp)(u8)hx[1]) != E_OK) return E_NOMEM;
    }
    p += n;
  }
  return E_OK;
}

int clip_copy(Tab* t, const Selection& s, int hex_pane)
{
  Document* d;
  epos lo, hi;
  ClipText txt;
  EBytes raw, ansi;
  ClipRawHdr hdr;
  HGLOBAL hraw = 0, hw = 0, ha = 0;
  u32 lost = 0;
  int rc;
  if (!t || !t->doc) return E_STATE;
  d = t->doc;
  lo = sel_lo(s); hi = sel_hi(s);
  if (hi > d->length()) hi = d->length();
  if (lo >= hi) return E_NOTFOUND;
  if (hi - lo > CLIP_MAX) return E_TOOBIG;
  hdr.magic = CLIP_MAGIC; hdr.size = hi - lo;
  hdr.kind = d->enc().kind; hdr.bom = d->enc().bom; hdr.cp = d->enc().cp;
  if (raw.resize(sizeof(hdr) + (hi - lo)) != E_OK) return E_NOMEM;
  memcpy(raw.data(), &hdr, sizeof(hdr));
  if (d->read(lo, raw.data() + sizeof(hdr), hi - lo) != E_OK) return E_IO;
  rc = hex_pane ? clip_build_hex(d, lo, hi, &txt) : clip_build_text(d, lo, hi, &txt);
  if (rc != E_OK) return rc;
  if (txt.w.push(0) != E_OK) return E_NOMEM;
  rc = winenc_ecp_to_ansi(txt.cps.data(), txt.cps.size(), &ansi, &lost);
  if (rc != E_OK) return rc;
  if (ansi.push(0) != E_OK) return E_NOMEM;
  txt.cps.release();
  hraw = clip_alloc(raw.data(), raw.size());
  hw = clip_alloc(txt.w.data(), txt.w.size() * 2);
  ha = clip_alloc(ansi.data(), ansi.size());
  if (!hraw || !hw || !ha) rc = E_NOMEM;
  else if (!clip_open()) rc = E_BUSY;
  if (rc != E_OK) {
    if (hraw) GlobalFree(hraw);
    if (hw) GlobalFree(hw);
    if (ha) GlobalFree(ha);
    return rc;
  }
  EmptyClipboard();
  if (!SetClipboardData(clip_rawfmt(), hraw)) GlobalFree(hraw);
  if (!SetClipboardData(CF_UNICODETEXT, hw)) GlobalFree(hw);
  if (!SetClipboardData(CF_TEXT, ha)) GlobalFree(ha);
  CloseClipboard();
  return E_OK;
}

static int clip_get_raw(HANDLE h, ClipData* out)
{
  const u8* p = (const u8*)GlobalLock(h);
  DWORD n = GlobalSize(h);
  ClipRawHdr hdr;
  int rc = E_FORMAT;
  if (!p) return E_FORMAT;
  if (n >= sizeof(hdr)) {
    memcpy(&hdr, p, sizeof(hdr));
    if (hdr.magic == CLIP_MAGIC && hdr.size <= n - sizeof(hdr)) {
      if (hdr.size > CLIP_MAX) rc = E_TOOBIG;
      else if (out->raw.append(p + sizeof(hdr), hdr.size) != E_OK) rc = E_NOMEM;
      else {
        out->enc.kind = hdr.kind; out->enc.bom = hdr.bom; out->enc.cp = hdr.cp;
        out->has_raw = 1;
        rc = E_OK;
      }
    }
  }
  GlobalUnlock(h);
  return rc;
}

static int clip_get_unicode(HANDLE h, ClipData* out)
{
  const u16* p = (const u16*)GlobalLock(h);
  DWORD n, i;
  int rc = E_OK;
  if (!p) return E_FORMAT;
  n = GlobalSize(h) / 2;
  if (n > CLIP_MAX) n = CLIP_MAX + 1;
  for (i = 0; i < n && p[i]; i++) {
    ecp c = p[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (p[i + 1] - 0xDC00);
      i++;
    } else if (c >= 0xD800 && c <= 0xDFFF) c = ECP_REPLACEMENT;   // lone surrogate
    if (out->text.size() >= CLIP_MAX) { rc = E_TOOBIG; break; }
    if (out->text.push(c) != E_OK) { rc = E_NOMEM; break; }
  }
  GlobalUnlock(h);
  return rc;
}

static int clip_get_ansi(HANDLE h, ClipData* out)
{
  const char* p = (const char*)GlobalLock(h);
  DWORD n, len = 0;
  int rc;
  if (!p) return E_FORMAT;
  n = GlobalSize(h);
  while (len < n && p[len]) len++;
  if (len > CLIP_MAX) rc = E_TOOBIG;
  else rc = winenc_ansi_to_ecp(p, len, &out->text);
  GlobalUnlock(h);
  return rc;
}

int clip_get(ClipData* out)
{
  HANDLE h;
  int rc = E_NOTFOUND;
  out->has_raw = 0;
  out->pad[0] = out->pad[1] = out->pad[2] = 0;
  out->enc = enc_ansi();
  out->raw.clear();
  out->text.clear();
  if (!clip_open()) return E_BUSY;
  h = GetClipboardData(clip_rawfmt());
  if (h) {
    rc = clip_get_raw(h, out);
    if (rc == E_OK) {                            // the text as well (callers in another encoding use it)
      const Codec* c = codec_get(out->enc);
      u32 i = 0, n = out->raw.size();
      while (i < n && rc == E_OK) {
        ecp ch;
        u32 used = c ? c->decode(out->raw.data() + i, n - i, 1, &ch) : 0;
        if (!used) { ch = ECP_BADBYTE(out->raw[i]); used = 1; }
        if (out->text.push(ch) != E_OK) rc = E_NOMEM;
        i += used;
      }
    }
  }
  if (rc != E_OK && rc != E_TOOBIG && rc != E_NOMEM) {
    out->has_raw = 0; out->raw.clear(); out->text.clear();
    rc = E_NOTFOUND;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) rc = clip_get_unicode(h, out);
    else {
      h = GetClipboardData(CF_TEXT);
      if (h) rc = clip_get_ansi(h, out);
    }
  }
  CloseClipboard();
  if (rc != E_OK) { out->has_raw = 0; out->raw.clear(); out->text.clear(); }
  return rc;
}

void clip_free(ClipData* d)
{
  d->raw.release();
  d->text.release();
  d->has_raw = 0;
}

int clip_has_data()
{
  return IsClipboardFormatAvailable(clip_rawfmt()) || IsClipboardFormatAvailable(CF_UNICODETEXT) ||
         IsClipboardFormatAvailable(CF_TEXT);
}
