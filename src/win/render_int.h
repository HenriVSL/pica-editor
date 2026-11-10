// render_int.h - private render helpers shared by render.cpp and textview.cpp (T08). Not a public interface.
#ifndef PICA_RENDER_INT_H
#define PICA_RENDER_INT_H

#include <windows.h>
#include "etypes.h"

enum { RF_NORMAL = 0, RF_CJK = 1, RF_FALLBACK = 2 };   // RunAttr.font values (2 = Courier New for raster fonts)
int  render_init_from_config();          // render_init from g_app.cfg (face, pt, px); no-op if the font is unchanged
int  render_glyph_font(u16 g);           // RF_NORMAL, or RF_FALLBACK when a raster font can't draw g
int  render_is_raster();
void render_refresh_colors();            // re-read g_app.cfg colours and system colours
void render_font_desc(char* out, u32 cap);   // "Courier New 10 pt (8x16)" for logs

#endif
