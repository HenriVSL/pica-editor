// fif.cpp - find in files core (DESIGN 6.16, 11.6). STUB from T00. Owner after Wave 0: T11.
#include "fif.h"

int fif_mask_match(const char* masks, const char* name) { (void)masks; (void)name; return 0; }
int fif_run(FileSys* fs, const FifQuery& q, FifSink* sink, volatile i32* cancel)
{
  (void)fs; (void)q; (void)cancel;
  if (sink) sink->on_done(E_NOTIMPL, 0, 0, 0);
  return E_NOTIMPL;
}
int fif_search_buffer(const u8* p, u32 n, const DetectResult& det, Searcher& s, u32 file_index, FifSink* sink, u32* hits)
{ (void)p; (void)n; (void)det; (void)s; (void)file_index; (void)sink; if (hits) *hits = 0; return E_NOTIMPL; }
