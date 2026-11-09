// rxvm.cpp - RxMatcher: Pike VM over ByteSource + Codec (DESIGN 6.14, 11.3). STUB from T00. Owner after Wave 0: T03.
#include "regex.h"

RxMatcher::RxMatcher() : prog_(0), budget_(10000000) {}
RxMatcher::~RxMatcher() {}
int RxMatcher::init(const RxProg* p) { prog_ = p; return E_NOTIMPL; }
void RxMatcher::set_budget(u32 steps) { budget_ = steps; }
int RxMatcher::search(ByteSource* s, const Codec* c, epos lo, epos from, epos hi, RxMatch* m, ProgressCb* cb)
{ (void)s; (void)c; (void)lo; (void)from; (void)hi; (void)m; (void)cb; return E_NOTIMPL; }
