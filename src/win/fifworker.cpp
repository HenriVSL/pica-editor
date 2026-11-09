// fifworker.cpp - find-in-files worker thread (DESIGN 11.6). STUB from T00. Owner after Wave 0: T11.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "fifworker.h"

int fif_start(const FifQuery& q) { (void)q; return E_NOTIMPL; }
void fif_stop() {}
int fif_running() { return 0; }
void fif_on_msg(WPARAM w, LPARAM l) { (void)w; (void)l; }
