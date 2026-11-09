// clip.cpp - clipboard (DESIGN 9.6). STUB from T00. Owner after Wave 0: T12.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "clip.h"

int clip_copy(Tab* t, const Selection& s, int hex_pane) { (void)t; (void)s; (void)hex_pane; return E_NOTIMPL; }
int clip_get(ClipData* out) { out->has_raw = 0; out->raw.clear(); out->text.clear(); return E_NOTIMPL; }
void clip_free(ClipData* d) { d->raw.release(); d->text.release(); d->has_raw = 0; }
int clip_has_data() { return 0; }
