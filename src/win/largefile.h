// largefile.h - on-demand documents and the indexer thread (DESIGN 14.1). Implemented in largefile.cpp (T14).
// Owner after Wave 0: overseer.
#ifndef PICA_LARGEFILE_H
#define PICA_LARGEFILE_H

#include <windows.h>
#include "etypes.h"

struct Tab;
int  largefile_begin(Tab* t);            // start indexing an on-demand document (E_NOTIMPL until T14)
void largefile_cancel(Tab* t);
void largefile_on_msg(WPARAM w, LPARAM l);   // WM_APP_INDEX

#endif
