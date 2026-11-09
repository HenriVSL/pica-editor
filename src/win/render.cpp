// render.cpp - fonts and run drawing (DESIGN 10). STUB from T00. Owner after Wave 0: T08.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "render.h"
#include "config.h"

static RenderFonts g_fonts;
int render_init(const char* face, int height_px, int pt, int charset) { (void)face; (void)height_px; (void)pt; (void)charset; return E_NOTIMPL; }
void render_shutdown() {}
const RenderFonts* render_fonts() { return &g_fonts; }
void render_run(HDC dc, int x, int y, const RECT* cell_rect, const WCHAR* s, u32 n, const RunAttr& a, const int* dx)
{ (void)dc; (void)x; (void)y; (void)cell_rect; (void)s; (void)n; (void)a; (void)dx; }
void render_fill(HDC dc, const RECT* r, COLORREF bg) { (void)dc; (void)r; (void)bg; }
COLORREF render_style_fg(u8 style) { (void)style; return GetSysColor(COLOR_WINDOWTEXT); }
COLORREF render_style_bg(u8 style) { (void)style; return GetSysColor(COLOR_WINDOW); }
int render_style_bold(u8 style) { (void)style; return 0; }
COLORREF render_ui(int uic) { (void)uic; return GetSysColor(COLOR_WINDOWTEXT); }
