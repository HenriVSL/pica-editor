// views.h - text view (textview.cpp, T08) and hex view (hexview.cpp, T10) APIs (DESIGN 15.1).
// Owner after Wave 0: overseer.
#ifndef PICA_VIEWS_H
#define PICA_VIEWS_H

#include <windows.h>
#include "etypes.h"
#include "doc.h"

struct Tab;
enum { SCROLL_NONE = 0, SCROLL_MIN = 1, SCROLL_CENTER = 2 };   // scroll_mode for *_set_selection
struct CaretInfo {
  u32 line, col;          // text view: 0-based line, 1-based display column
  epos pos;               // caret byte offset
  epos sel_len;           // selected bytes
  u32 sel_lines;          // lines touched by the selection (0 = empty selection)
  ecp ch;                 // character (text) or byte (hex) at the caret
  u8 has_ch, overwrite, view, pad;
};
class PanelHandler {      // results panel (T11): Enter / double-click on a line, keys
public:
  virtual ~PanelHandler() {}
  virtual void panel_open(epos pos) = 0;        // activate the hit at this offset
  virtual int  panel_key(UINT vk) = 0;          // nonzero = handled
};
HWND textview_create(HWND parent, int id, int is_panel);
void textview_attach(HWND w, Tab* t);           // NULL detaches
void textview_set_selection(HWND w, epos anchor, epos caret, int scroll_mode);
void textview_ensure_visible(HWND w, epos pos, int center);
void textview_invalidate(HWND w);
void textview_config_changed(HWND w);           // fonts, colours, tab width, line numbers
void textview_caret_info(HWND w, CaretInfo* out);
void textview_set_panel_handler(HWND w, PanelHandler* h);
// Screen rectangle of the character at pos (one cell at the end of a line), word wrap included. Returns 1 when pos
// is on a visible row (the rectangle may still lie left or right of the window when scrolled sideways), else 0.
int  textview_pos_rect(HWND w, epos pos, RECT* screen);
HWND hexview_create(HWND parent, int id, int is_panel);
void hexview_attach(HWND w, Tab* t);
void hexview_set_selection(HWND w, epos anchor, epos caret, int scroll_mode);
void hexview_ensure_visible(HWND w, epos pos, int center);
void hexview_invalidate(HWND w);
void hexview_config_changed(HWND w);
void hexview_caret_info(HWND w, CaretInfo* out);
void hexview_set_panel_handler(HWND w, PanelHandler* h);
void hexview_set_inspector(HWND w, int on);

#endif
