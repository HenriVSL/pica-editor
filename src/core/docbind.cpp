// docbind.cpp - DocLines: Document -> Highlighter adapter (DESIGN 6.8). STUB from T00. Owner after Wave 0: T01.
#include "docbind.h"

DocLines::DocLines() : d_(0), h_(0) {}
DocLines::~DocLines() { unbind(); }
void DocLines::bind(Document* d, Highlighter* h)
{
  unbind();
  d_ = d; h_ = h;
  if (d_) d_->add_listener(this);
}
void DocLines::unbind()
{
  if (d_) d_->remove_listener(this);
  d_ = 0; h_ = 0;
}
u32 DocLines::lp_count() { return d_ ? d_->line_count() : 0; }
int DocLines::lp_get(u32 line, ecp* out, u32 cap, u32* n) { (void)line; (void)out; (void)cap; if (n) *n = 0; return E_NOTIMPL; }
void DocLines::doc_changed(Document* d, const EditEvent& e) { (void)d; (void)e; }
