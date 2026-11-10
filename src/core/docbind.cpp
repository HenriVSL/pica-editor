// docbind.cpp - DocLines: Document -> Highlighter adapter (DESIGN 6.8). Owner: T01.
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

// Decodes up to cap characters of the line, EOL excluded; line 0 starts at the text origin (no BOM).
int DocLines::lp_get(u32 line, ecp* out, u32 cap, u32* n)
{
  CharReader r;
  epos s, e, at;
  u32 k = 0;
  int rc = 1;
  if (n) *n = 0;
  if (!d_) return E_STATE;
  if (line >= d_->line_count()) return E_RANGE;
  s = d_->line_start(line);
  if (s < d_->text_origin()) s = d_->text_origin();
  e = d_->line_end(line);
  if (e < s) e = s;
  r.init(d_->bytes(), d_->codec(), s, e);
  while (k < cap && (rc = r.next(&out[k], &at)) == 1) k++;
  if (n) *n = k;
  return rc < 0 ? rc : E_OK;
}

void DocLines::doc_changed(Document* d, const EditEvent& e)
{
  (void)d;
  if (!h_) return;
  if (e.flags & (EV_RESET | EV_INDEXED)) h_->lines_reset();
  else h_->lines_changed(e.line, e.lines_removed, e.lines_added);
}
