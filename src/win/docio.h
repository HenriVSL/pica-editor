// docio.h - open/new/save/reload/revert/close (DESIGN 13.2-13.4, 15.1). Implemented in docio.cpp (T09; T14 in
// Wave 3), doccmd_offer_utf8 in doccmds.cpp (T09). Owner after Wave 0: overseer.
#ifndef PICA_DOCIO_H
#define PICA_DOCIO_H

#include <windows.h>
#include "etypes.h"
#include "codec.h"

struct Tab;
struct OpenOpts {
  u8 force_enc;           // use enc instead of detection (Reload as, session, .nfo rule)
  u8 hex;                 // open in the hex view (-hex)
  u8 read_only;           // -ro
  u8 no_activate;         // open in the background (session restore)
  EncSpec enc;
  u32 line;               // -n<line>: 1-based, 0 = none
  u8 from_session;        // missing file -> status note only, MRU entry kept
  u8 pad[3];
};
void docio_default_opts(OpenOpts* o);
typedef void (*SaveDone)(Tab* t, int result, void* ctx);
int  docio_open(const char* path, const OpenOpts* o, Tab** out);   // o may be NULL; an open file activates its tab
int  docio_new(Tab** out);
int  docio_save(Tab* t, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null, SaveDone cb,
                void* ctx);                                       // asynchronous (I/O worker); cb may be NULL
int  docio_save_wait(Tab* t, const char* path_or_null, const EncSpec* enc_or_null, const int* eol_or_null);  // modal
int  docio_reload(Tab* t, const EncSpec* as_or_null, int undoable);
int  docio_revert(Tab* t);
int  docio_close(Tab* t, int ask);       // 1 closed, 0 cancelled by the user, < 0 error
void doccmd_offer_utf8(Tab* t, const char* msg);   // notification bar "[Convert to UTF-8] [Dismiss]" (12.4)

#endif
