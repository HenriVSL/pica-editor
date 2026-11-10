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
struct FifHit { u32 file_index; u32 line; u32 col; char text[1025]; u32 text_len; u32 hl_start; u32 hl_len;
                epos pos; epos len; };   // T11 additions: match byte offset and length in the file (BOM included)
// text: the line in UTF-8, at most 256 chars (tabs kept); hl_*: match position in chars within text.
// line and col are 0-based (col in characters from the line start). A long line is shown as a window around the
// match with "..." at the cut ends (still at most 256 chars). CR, LF and NUL inside the text become U+FFFD.
class FifSink {
public:
  virtual ~FifSink() {}
  virtual int  on_file(const char* path, u32 index) = 0;      // a file with hits begins
  virtual int  on_hit(const FifHit& h) = 0;                   // nonzero = stop
  virtual int  on_progress(u32 done, u32 seen, const char* current) = 0;   // nonzero = cancel
  virtual void on_done(int result, u32 searched, u32 files_with_hits, u32 hits) = 0;
  // T11 addition: a file that could not be read or searched (E_SHARING, E_ACCESS, E_IO, E_BUDGET, E_TOOBIG > 4 GB)
  virtual void on_skip(const char* path, int err) { (void)path; (void)err; }
};
// on_done result: E_OK, E_CANCEL (cancel flag, on_progress or on_hit asked to stop), E_TOOBIG (stopped at max_hits),
// E_NOTFOUND (folder missing), E_SYNTAX/E_UNSUPPORTED/E_TOOBIG from the pattern, E_NOMEM.
int fif_mask_match(const char* masks, const char* name);       // "*.cpp;*.h *.rc !*.obj" (; or space; ! excludes)
int fif_run(FileSys* fs, const FifQuery& q, FifSink* sink, volatile i32* cancel);
int fif_search_buffer(const u8* p, u32 n, const DetectResult& det, Searcher& s, u32 file_index, FifSink* sink, u32* hits);
// T11 addition: the hit builder over any ByteSource (Find All in an open document uses it on Document::bytes()).
// Searches [lo, hi) (lo = text origin: line 0 starts there); cr_mode = lines end at CR only (else at LF; CRLF is one
// break). path != NULL: sink->on_file(path, file_index) before the first hit. Stops after max_hits hits (0 = no
// limit) with E_TOOBIG. *hits = hits reported. Returns E_OK, E_CANCEL, E_TOOBIG, E_BUDGET, E_IO, E_NOMEM.
int fif_search_source(ByteSource* s, const Codec* c, epos lo, epos hi, int cr_mode, Searcher& srch, u32 file_index,
                      const char* path, FifSink* sink, u32 max_hits, u32* hits, volatile i32* cancel);

#endif
