// render.cpp - fonts, colours and run drawing (DESIGN 10). Owner: T08.
//
// One font set for the whole program (text views and hex view): normal, bold (used only when its cell metrics equal
// the normal font's), an optional CJK font found at startup (EnumFontFamiliesEx over the SHIFTJIS / GB2312 /
// HANGEUL / CHINESEBIG5 charsets), and for raster fonts (Fixedsys, Terminal, Courier) a Courier New fallback at the
// same cell height for characters outside the font's code page.
//
// render_run caches the selected font and the text/background colours per DC (4 DCs; views use CS_OWNDC). If a caller
// changes those on a DC directly (SelectObject, SetTextColor, SetBkColor), it must call render_fill(dc, NULL, 0)
// first: a NULL rectangle drops the cached state of that DC.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "render.h"
#include "render_int.h"
#include "config.h"
#include "lexer.h"
#include "estr.h"
#include <string.h>

static RenderFonts g_fonts;
static HFONT g_fallback;
static int g_asc[3];                 // ascent per RF_* font (baseline alignment)
static int g_raster, g_font_cp;
static char g_face[32];
static int g_pt, g_px, g_charset, g_inited;
static u32 g_gen = 1;                // bumped when fonts are recreated (drops DC caches)

static COLORREF g_sfg[ST_COUNT], g_sbg[ST_COUNT];
static u8 g_sbold[ST_COUNT];
static COLORREF g_ui[UIC_COUNT];

// per-glyph font choice for raster fonts: 2 bits per BMP code unit (0 unknown, 1 normal, 2 fallback)
static u8* g_gmap;

struct DcCache { HDC dc; HFONT font; COLORREF fg, bg; u32 gen, use; };
static DcCache g_dcs[4];
static u32 g_dctick;

static DcCache* dc_state(HDC dc)
{
  u32 i, best = 0;
  for (i = 0; i < 4; i++) {
    if (g_dcs[i].dc == dc) {
      if (g_dcs[i].gen != g_gen) { g_dcs[i].font = 0; g_dcs[i].fg = g_dcs[i].bg = CLR_INVALID; g_dcs[i].gen = g_gen; }
      g_dcs[i].use = ++g_dctick;
      return &g_dcs[i];
    }
    if (g_dcs[i].use < g_dcs[best].use) best = i;
  }
  g_dcs[best].dc = dc; g_dcs[best].font = 0; g_dcs[best].fg = g_dcs[best].bg = CLR_INVALID;
  g_dcs[best].gen = g_gen; g_dcs[best].use = ++g_dctick;
  return &g_dcs[best];
}

static int g_gdi_live;

HGDIOBJ gdi_track(HGDIOBJ o)
{
  if (o) g_gdi_live++;
  return o;
}

BOOL gdi_delete(HGDIOBJ o)
{
  BOOL ok;
  if (!o) return FALSE;
  ok = DeleteObject(o);
  if (ok) g_gdi_live--;
  return ok;
}

int gdi_live()
{
  return g_gdi_live;
}

static HFONT mk_font(const char* face, int h, int weight, int charset)
{
  LOGFONTA lf;
  memset(&lf, 0, sizeof(lf));
  lf.lfHeight = h;
  lf.lfWeight = weight;
  lf.lfCharSet = (BYTE)charset;
  lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
  lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
  lf.lfQuality = DEFAULT_QUALITY;
  lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
  es_copy(lf.lfFaceName, sizeof(lf.lfFaceName), face);
  return (HFONT)gdi_track(CreateFontIndirectA(&lf));
}

struct CjkFind { char face[LF_FACESIZE]; int charset; int fixed; };

static int CALLBACK cjk_enum(const LOGFONTA* lf, const TEXTMETRICA* tm, DWORD type, LPARAM lp)
{
  CjkFind* f = (CjkFind*)lp;
  int fixed = (lf->lfPitchAndFamily & 3) == FIXED_PITCH;
  (void)tm; (void)type;
  if (lf->lfFaceName[0] == '@') return 1;              // vertical variants
  if (!f->face[0] || (fixed && !f->fixed)) {
    es_copy(f->face, sizeof(f->face), lf->lfFaceName);
    f->charset = lf->lfCharSet;
    f->fixed = fixed;
  }
  return f->fixed ? 0 : 1;
}

static HFONT find_cjk_font(HDC dc, int h)
{
  static const int sets[4] = { SHIFTJIS_CHARSET, GB2312_CHARSET, HANGEUL_CHARSET, CHINESEBIG5_CHARSET };
  u32 i;
  for (i = 0; i < 4; i++) {
    LOGFONTA lf;
    CjkFind f;
    memset(&lf, 0, sizeof(lf));
    memset(&f, 0, sizeof(f));
    lf.lfCharSet = (BYTE)sets[i];
    EnumFontFamiliesExA(dc, &lf, (FONTENUMPROCA)cjk_enum, (LPARAM)&f, 0);
    if (f.face[0] && f.charset == sets[i]) return mk_font(f.face, h, FW_NORMAL, sets[i]);
  }
  return 0;
}

static int charset_cp(int cs)
{
  switch (cs) {
  case ANSI_CHARSET: return 1252;
  case OEM_CHARSET: return (int)GetOEMCP();
  case EASTEUROPE_CHARSET: return 1250;
  case RUSSIAN_CHARSET: return 1251;
  case GREEK_CHARSET: return 1253;
  case TURKISH_CHARSET: return 1254;
  case BALTIC_CHARSET: return 1257;
  default: return (int)GetACP();
  }
}

static void free_fonts()
{
  u32 i;
  // A font still selected into a view's own DC (CS_OWNDC keeps it) can't be deleted: select a stock font first.
  for (i = 0; i < 4; i++) {
    if (g_dcs[i].dc && g_dcs[i].font) SelectObject(g_dcs[i].dc, GetStockObject(SYSTEM_FIXED_FONT));
    g_dcs[i].font = 0;
  }
  gdi_delete(g_fonts.normal);
  gdi_delete(g_fonts.bold);
  gdi_delete(g_fonts.cjk);
  gdi_delete(g_fallback);
  memset(&g_fonts, 0, sizeof(g_fonts));
  g_fallback = 0;
}

int render_init(const char* face, int height_px, int pt, int charset)
{
  HDC dc;
  HFONT normal, bold, old;
  TEXTMETRICA tm, tb;
  int h, wm = 0;
  if (!face || !face[0]) face = "Courier New";
  if (pt <= 0) pt = 10;
  dc = GetDC(0);
  if (!dc) return E_NOMEM;
  h = height_px > 0 ? height_px : -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
  normal = mk_font(face, h, FW_NORMAL, charset);
  if (!normal) { ReleaseDC(0, dc); return E_NOMEM; }
  bold = mk_font(face, h, FW_BOLD, charset);
  old = (HFONT)SelectObject(dc, normal);
  GetTextMetricsA(dc, &tm);
  if (!GetCharWidthA(dc, 'M', 'M', &wm)) wm = 0;
  memset(&tb, 0, sizeof(tb));
  if (bold) { SelectObject(dc, bold); GetTextMetricsA(dc, &tb); }
  SelectObject(dc, old);
  free_fonts();
  g_fonts.normal = normal;
  g_fonts.bold = bold;
  g_fonts.cw = wm > 0 ? wm : tm.tmAveCharWidth;
  if (g_fonts.cw <= 0) g_fonts.cw = 8;
  g_fonts.ch = tm.tmHeight > 0 ? tm.tmHeight : 16;
  g_fonts.ascent = tm.tmAscent;
  // bold only when it keeps the cell (Fixedsys' synthesised bold is 1 px wider); bold runs are drawn with lpDx
  g_fonts.bold_ok = bold && tb.tmAveCharWidth == tm.tmAveCharWidth && tb.tmHeight == tm.tmHeight;
  g_asc[RF_NORMAL] = tm.tmAscent;
  g_raster = (tm.tmPitchAndFamily & TMPF_TRUETYPE) ? 0 : 1;
  g_font_cp = charset_cp(tm.tmCharSet);
  if (g_raster) {
    g_fallback = mk_font("Courier New", g_fonts.ch, FW_NORMAL, DEFAULT_CHARSET);
    if (g_fallback) {
      old = (HFONT)SelectObject(dc, g_fallback);
      GetTextMetricsA(dc, &tb);
      SelectObject(dc, old);
      g_asc[RF_FALLBACK] = tb.tmAscent;
    }
  }
  g_fonts.cjk = find_cjk_font(dc, g_fonts.ch);
  if (g_fonts.cjk) {
    old = (HFONT)SelectObject(dc, g_fonts.cjk);
    GetTextMetricsA(dc, &tb);
    SelectObject(dc, old);
    g_asc[RF_CJK] = tb.tmAscent;
  }
  ReleaseDC(0, dc);
  if (g_gmap) memset(g_gmap, 0, 16384);
  es_copy(g_face, sizeof(g_face), face);
  g_pt = pt; g_px = height_px; g_charset = charset; g_inited = 1;
  g_gen++;
  render_refresh_colors();
  return E_OK;
}

void render_shutdown()
{
  free_fonts();
  if (g_gmap) { e_free(g_gmap); g_gmap = 0; }
  g_inited = 0;
  g_gen++;
}

const RenderFonts* render_fonts() { return &g_fonts; }

int render_init_from_config()
{
  const Config* c = &g_app.cfg;
  int cs = es_icmp(c->font_face, "Terminal") == 0 ? OEM_CHARSET : DEFAULT_CHARSET;
  if (g_inited && es_icmp(g_face, c->font_face) == 0 && g_pt == c->font_pt && g_px == c->font_px && g_charset == cs) {
    render_refresh_colors();
    return E_OK;
  }
  return render_init(c->font_face, c->font_px, c->font_pt, cs);
}

int render_is_raster() { return g_raster; }

int render_glyph_font(u16 g)
{
  u32 idx, sh;
  u8 v;
  if (!g_raster || g < 0x80 || !g_fallback) return RF_NORMAL;
  if (!g_gmap) {
    g_gmap = (u8*)e_alloc(16384);
    if (!g_gmap) return RF_NORMAL;
    memset(g_gmap, 0, 16384);
  }
  idx = g >> 2; sh = (g & 3) * 2;
  v = (u8)((g_gmap[idx] >> sh) & 3);
  if (!v) {
    WCHAR w = (WCHAR)g;
    char out[4];
    BOOL used = FALSE;
    int n = WideCharToMultiByte((UINT)g_font_cp, 0, &w, 1, out, sizeof(out), 0, &used);
    v = (u8)((n == 1 && !used) ? 1 : 2);
    g_gmap[idx] = (u8)(g_gmap[idx] | (v << sh));
  }
  return v == 2 ? RF_FALLBACK : RF_NORMAL;
}

void render_font_desc(char* out, u32 cap)
{
  if (g_px > 0) es_fmt(out, cap, "%s %dpx (%dx%d%s)", g_face, g_px, g_fonts.cw, g_fonts.ch, g_fonts.bold_ok ? ", bold" : "");
  else es_fmt(out, cap, "%s %d pt (%dx%d%s)", g_face, g_pt, g_fonts.cw, g_fonts.ch, g_fonts.bold_ok ? ", bold" : "");
}

static COLORREF sys_or(u32 rgb, int sys)
{
  return rgb == COLOR_SYSDEFAULT ? GetSysColor(sys) : (COLORREF)(rgb & 0xFFFFFF);
}

void render_refresh_colors()
{
  const Config* c = &g_app.cfg;
  u32 i;
  for (i = 0; i < ST_COUNT; i++) {
    g_sfg[i] = sys_or(c->style[i].rgb, COLOR_WINDOWTEXT);
    g_sbg[i] = GetSysColor(COLOR_WINDOW);
    g_sbold[i] = c->style[i].bold;
  }
  g_sbg[ST_INVALID] = RGB(255, 224, 224);             // Appendix B: INVALID has its own background
  for (i = 0; i < UIC_COUNT; i++) {
    int sys;
    switch (i) {
    case UIC_GUTTER_BG: sys = COLOR_BTNFACE; break;
    case UIC_MARK_BG: case UIC_BRACE_BG: case UIC_CTRL_BG: case UIC_BAD_BG: case UIC_HEX_MIRROR_BG:
    case UIC_ISEARCH_FAIL: sys = COLOR_WINDOW; break;
    case UIC_GUTTER_FG: case UIC_WS_FG: case UIC_HEX_BOM: sys = COLOR_GRAYTEXT; break;
    default: sys = COLOR_WINDOWTEXT; break;
    }
    g_ui[i] = sys_or(c->ui[i], sys);
  }
}

COLORREF render_style_fg(u8 style) { return style < ST_COUNT ? g_sfg[style] : g_sfg[0]; }
COLORREF render_style_bg(u8 style) { return style < ST_COUNT ? g_sbg[style] : g_sbg[0]; }
int render_style_bold(u8 style) { return style < ST_COUNT && g_sbold[style] && g_fonts.bold_ok ? 1 : 0; }
COLORREF render_ui(int uic) { return uic >= 0 && uic < UIC_COUNT ? g_ui[uic] : GetSysColor(COLOR_WINDOWTEXT); }

static void set_bk(HDC dc, DcCache* st, COLORREF bg)
{
  if (st->bg != bg) { SetBkColor(dc, bg); st->bg = bg; }
}

void render_fill(HDC dc, const RECT* r, COLORREF bg)
{
  DcCache* st = dc_state(dc);
  if (!r) { st->font = 0; st->fg = st->bg = CLR_INVALID; return; }
  if (r->right <= r->left || r->bottom <= r->top) return;
  set_bk(dc, st, bg);
  ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, 0, 0, 0);
}

void render_run(HDC dc, int x, int y, const RECT* cell_rect, const WCHAR* s, u32 n, const RunAttr& a, const int* dx)
{
  DcCache* st = dc_state(dc);
  HFONT f;
  int fi = a.font;
  int yy;
  u32 done = 0;
  if (cell_rect->right <= cell_rect->left) return;
  if (fi == RF_CJK && !g_fonts.cjk) fi = RF_NORMAL;
  if (fi == RF_FALLBACK && !g_fallback) fi = RF_NORMAL;
  if (fi == RF_CJK) f = g_fonts.cjk;
  else if (fi == RF_FALLBACK) f = g_fallback;
  else f = a.bold && g_fonts.bold_ok ? g_fonts.bold : g_fonts.normal;
  if (st->font != f) { SelectObject(dc, f); st->font = f; }
  if (st->fg != a.fg) { SetTextColor(dc, a.fg); st->fg = a.fg; }
  set_bk(dc, st, a.bg);
  yy = y + (fi != RF_NORMAL ? g_asc[RF_NORMAL] - g_asc[fi] : 0);
  if (n == 0) {
    ExtTextOutW(dc, x, yy, ETO_OPAQUE, cell_rect, 0, 0, 0);
  } else {
    int xx = x;
    while (done < n) {                                // 9x draws at most 8192 glyphs per call; we use 1024
      u32 k = n - done, i;
      if (k > 1024) k = 1024;
      ExtTextOutW(dc, xx, yy, (done == 0 ? ETO_OPAQUE : 0) | ETO_CLIPPED, cell_rect, s + done, k,
                  dx ? (int*)(dx + done) : 0);
      if (dx) { for (i = 0; i < k; i++) xx += dx[done + i]; }
      else xx += (int)k * g_fonts.cw;
      done += k;
    }
  }
  if (a.box) {                                        // placeholder outline, 1 px inside the cells
    RECT r = *cell_rect, e;
    COLORREF save = st->bg;
    r.left += 1; r.right -= 1; r.top += 1; r.bottom -= 1;
    if (r.right > r.left + 1 && r.bottom > r.top + 1) {
      set_bk(dc, st, a.fg);
      e = r; e.bottom = e.top + 1; ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &e, 0, 0, 0);
      e = r; e.top = e.bottom - 1; ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &e, 0, 0, 0);
      e = r; e.right = e.left + 1; ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &e, 0, 0, 0);
      e = r; e.left = e.right - 1; ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &e, 0, 0, 0);
      set_bk(dc, st, save);
    }
  }
}
