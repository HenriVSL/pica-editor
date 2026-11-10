// search.h - search engine (DESIGN 6.15). Owner after Wave 0: T03.
#ifndef PICA_SEARCH_H
#define PICA_SEARCH_H

#include "etypes.h"
#include "emem.h"
#include "esource.h"
#include "codec.h"
#include "regex.h"

enum SearchMode { SM_NORMAL = 0, SM_EXTENDED = 1, SM_REGEX = 2, SM_HEX = 3 };
struct SearchOpts  { u8 mode; u8 match_case; u8 whole_word; u8 dot_all; };
struct SearchError { int code; u32 pos; char msg[96]; };
int search_parse_extended(const ecp* in, u32 n, Vec<ecp>* out, SearchError* err);
class Searcher {
public:
  Searcher();  ~Searcher();
  int  compile(const ecp* pat, u32 n, const SearchOpts& o, const EncSpec& doc_enc, SearchError* err);
  int  find(ByteSource* s, const Codec* c, epos lo, epos hi, epos from, int backward, RxMatch* m, ProgressCb* cb);
  int  count(ByteSource* s, const Codec* c, epos lo, epos hi, u32* n, ProgressCb* cb);
  int  find_all(ByteSource* s, const Codec* c, epos lo, epos hi, Vec<MarkRange>* out, u32 max, ProgressCb* cb);
  int  expand(const ecp* rep, u32 n, ByteSource* s, const Codec* c, const RxMatch& m, EBytes* out, u32* unrepresentable);
  int  impossible() const;            // pattern can't occur in this encoding (find returns 0)
  const SearchOpts& opts() const;
private:                               // private state: T03 may change freely
  SearchOpts opts_; EncSpec enc_; RxProg* prog_; int impossible_; int compiled_;
  RxMatcher mt_;
  EBytes hex_; EBytes hexmask_;            // SM_HEX pattern bytes and bit masks (0xFF = whole byte must match)
  int find_fwd_(ByteSource* s, const Codec* c, epos lo, epos from, epos hi, epos ctx, RxMatch* m, ProgressCb* cb);
  int find_back_(ByteSource* s, const Codec* c, epos lo, epos hi, epos from, RxMatch* m, ProgressCb* cb);
  int find_hex_(ByteSource* s, epos from, epos hi, RxMatch* m, ProgressCb* cb);
  Searcher(const Searcher&); Searcher& operator=(const Searcher&);
};
class EditSink { public: virtual ~EditSink() {} virtual int apply(epos pos, epos del, const u8* ins, epos n) = 0; };
int search_replace_all(Searcher& s, ByteSource* src, const Codec* c, epos lo, epos hi, const ecp* rep, u32 n,
                       EditSink* sink, ProgressCb* cb, u32* replaced, u32* unrepresentable);  // edits back to front

#endif
