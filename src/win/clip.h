// clip.h - clipboard (DESIGN 9.6, ARCH 3.5). Implemented in clip.cpp (T12). Owner after Wave 0: overseer.
#ifndef PICA_CLIP_H
#define PICA_CLIP_H

#include <windows.h>
#include "etypes.h"
#include "emem.h"
#include "codec.h"
#include "doc.h"

struct Tab;
struct ClipData {          // what clip_get found
  u8 has_raw; u8 pad[3];   // 1: raw bytes from Pica (APP_RAWFMT) in encoding enc
  EncSpec enc;
  EBytes raw;
  Vec<ecp> text;           // decoded text (CF_UNICODETEXT preferred, else CF_TEXT); also filled when has_raw
};
int  clip_copy(Tab* t, const Selection& s, int hex_pane);   // RawBytes + CF_UNICODETEXT + CF_TEXT; E_TOOBIG > 16 MB
int  clip_get(ClipData* out);            // E_NOTFOUND if nothing usable
void clip_free(ClipData* d);
int  clip_has_data();

#endif
