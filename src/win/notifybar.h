// notifybar.h - per-tab notification bar (DESIGN 15.1, 15.2). Owner after Wave 0: overseer (implementation T07).
#ifndef PICA_NOTIFYBAR_H
#define PICA_NOTIFYBAR_H

#include <windows.h>
#include "etypes.h"

struct Tab;
enum NotifyKind {
  NB_NONE = 0, NB_INFO, NB_BINARY, NB_READONLY, NB_SHARED_RO, NB_CHANGED, NB_MISSING, NB_INDEXING, NB_CONVERT,
  NB_RECOVERED, NB_KINDS
};
enum { NB_MAX_BUTTONS = 4 };
struct NotifyButton { int id; char text[40]; };              // id != 0
typedef void (*NotifyFn)(Tab* t, int kind, int button_id, void* ctx);   // button_id 0 = closed with [x]
struct NotifyState {                                         // lives in Tab (one bar per tab)
  int kind; char text[300]; NotifyButton buttons[NB_MAX_BUTTONS]; u32 nbuttons;
  NotifyFn fn; void* ctx; u32 shown_ms; u32 auto_hide_ms;   // auto_hide_ms 0 = stays
};
HWND notifybar_create(HWND parent);
void notifybar_show(Tab* t, int kind, const char* text, const NotifyButton* b, u32 n, NotifyFn fn, void* ctx);
void notifybar_hide(Tab* t, int kind);   // NB_NONE hides whatever is shown
void notifybar_update();                 // active tab changed: show its bar or hide the window
int  notifybar_height();                 // 0 when hidden

#endif
