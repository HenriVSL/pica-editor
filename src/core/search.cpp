// search.cpp - Searcher and replace-all (DESIGN 6.15, 11). STUB from T00. Owner after Wave 0: T03.
#include "search.h"
#include "estr.h"

static void notimpl_err(SearchError* err)
{
  if (err) { err->code = E_NOTIMPL; err->pos = 0; es_copy(err->msg, sizeof(err->msg), "Search is not implemented yet"); }
}

int search_parse_extended(const ecp* in, u32 n, Vec<ecp>* out, SearchError* err)
{ (void)in; (void)n; (void)out; notimpl_err(err); return E_NOTIMPL; }

Searcher::Searcher() : prog_(0), impossible_(0)
{
  opts_.mode = SM_NORMAL; opts_.match_case = 0; opts_.whole_word = 0; opts_.dot_all = 0;
  enc_ = enc_ansi();
}
Searcher::~Searcher() {}
int Searcher::compile(const ecp* pat, u32 n, const SearchOpts& o, const EncSpec& doc_enc, SearchError* err)
{ (void)pat; (void)n; opts_ = o; enc_ = doc_enc; notimpl_err(err); return E_NOTIMPL; }
int Searcher::find(ByteSource* s, const Codec* c, epos lo, epos hi, epos from, int backward, RxMatch* m, ProgressCb* cb)
{ (void)s; (void)c; (void)lo; (void)hi; (void)from; (void)backward; (void)m; (void)cb; return E_NOTIMPL; }
int Searcher::count(ByteSource* s, const Codec* c, epos lo, epos hi, u32* n, ProgressCb* cb)
{ (void)s; (void)c; (void)lo; (void)hi; (void)cb; if (n) *n = 0; return E_NOTIMPL; }
int Searcher::find_all(ByteSource* s, const Codec* c, epos lo, epos hi, Vec<MarkRange>* out, u32 max, ProgressCb* cb)
{ (void)s; (void)c; (void)lo; (void)hi; (void)out; (void)max; (void)cb; return E_NOTIMPL; }
int Searcher::expand(const ecp* rep, u32 n, ByteSource* s, const Codec* c, const RxMatch& m, EBytes* out, u32* unrepresentable)
{ (void)rep; (void)n; (void)s; (void)c; (void)m; (void)out; if (unrepresentable) *unrepresentable = 0; return E_NOTIMPL; }
int Searcher::impossible() const { return impossible_; }
const SearchOpts& Searcher::opts() const { return opts_; }

int search_replace_all(Searcher& s, ByteSource* src, const Codec* c, epos lo, epos hi, const ecp* rep, u32 n,
                       EditSink* sink, ProgressCb* cb, u32* replaced, u32* unrepresentable)
{
  (void)s; (void)src; (void)c; (void)lo; (void)hi; (void)rep; (void)n; (void)sink; (void)cb;
  if (replaced) *replaced = 0;
  if (unrepresentable) *unrepresentable = 0;
  return E_NOTIMPL;
}
