// notifybar.cpp - per-tab notification bar (DESIGN 15.2). STUB from T00. Owner after Wave 0: T07.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "notifybar.h"

HWND notifybar_create(HWND parent) { (void)parent; return 0; }
void notifybar_show(Tab* t, int kind, const char* text, const NotifyButton* b, u32 n, NotifyFn fn, void* ctx)
{ (void)t; (void)kind; (void)b; (void)n; (void)fn; (void)ctx; if (text) app_status_note("%s", text); }
void notifybar_hide(Tab* t, int kind) { (void)t; (void)kind; }
void notifybar_update() {}
int notifybar_height() { return 0; }
