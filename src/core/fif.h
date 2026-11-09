// fif.h - find in files (DESIGN 6.16). Owner after Wave 0: T11.
#ifndef PICA_FIF_H
#define PICA_FIF_H

#include "etypes.h"
#include "fsys.h"
#include "detect.h"
#include "regex.h"
#include "search.h"

struct FifQuery {
  char folder[260]; char masks[260]; u8 subfolders; u8 pad[3];
  SearchOpts opts; ecp pattern[RX_MAX_PATTERN]; u32 pattern_len;
  DetectOpts det; u32 max_hits;        // default 50,000
};
struct FifHit { u32 file_index; u32 line; u32 col; char text[1025]; u32 text_len; u32 hl_start; u32 hl_len; };
// text: the line in UTF-8, at most 256 chars (tabs kept); hl_*: match position in chars within text
class FifSink {
public:
  virtual ~FifSink() {}
  virtual int  on_file(const char* path, u32 index) = 0;      // a file with hits begins
  virtual int  on_hit(const FifHit& h) = 0;                   // nonzero = stop
  virtual int  on_progress(u32 done, u32 seen, const char* current) = 0;   // nonzero = cancel
  virtual void on_done(int result, u32 searched, u32 files_with_hits, u32 hits) = 0;
};
int fif_mask_match(const char* masks, const char* name);       // "*.cpp;*.h *.rc !*.obj" (; or space; ! excludes)
int fif_run(FileSys* fs, const FifQuery& q, FifSink* sink, volatile i32* cancel);
int fif_search_buffer(const u8* p, u32 n, const DetectResult& det, Searcher& s, u32 file_index, FifSink* sink, u32* hits);

#endif
