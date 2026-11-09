// render.h - fonts and run drawing (DESIGN 10.2). Implemented in render.cpp (T08). Owner after Wave 0: overseer.
#ifndef PICA_RENDER_H
#define PICA_RENDER_H

#include <windows.h>
#include "etypes.h"

struct RenderFonts { HFONT normal, bold; HFONT cjk; int cw, ch, ascent; int bold_ok; };
int  render_init(const char* face, int height_px, int pt, int charset);   // creates fonts; returns E_*
void render_shutdown();
const RenderFonts* render_fonts();
struct RunAttr { COLORREF fg, bg; u8 bold; u8 font; /* 0 normal, 1 cjk */ u8 box; /* placeholder outline */ };
void render_run(HDC dc, int x, int y, const RECT* cell_rect, const WCHAR* s, u32 n, const RunAttr& a, const int* dx);
void render_fill(HDC dc, const RECT* r, COLORREF bg);
COLORREF render_style_fg(u8 style);  COLORREF render_style_bg(u8 style);  int render_style_bold(u8 style);
COLORREF render_ui(int uic);         // resolves COLOR_SYSDEFAULT to system colours

#endif
