// marks.cpp - LineMarks (bookmarks) and RangeMarks (DESIGN 6.8). Owner: T01.
#include "marks.h"
#include <stdlib.h>

enum { RM_MAX = 10000 };

// ---- LineMarks ----

LineMarks::LineMarks() : d_(0) {}
LineMarks::~LineMarks() { detach(); }
void LineMarks::attach(Document* d) { detach(); d_ = d; if (d_) d_->add_listener(this); }
void LineMarks::detach() { if (d_) d_->remove_listener(this); d_ = 0; }

// index of the first mark >= line
static u32 lm_lower(const Vec<u32>& v, u32 line)
{
  u32 lo = 0, hi = v.size();
  while (lo < hi) {
    u32 mid = lo + (hi - lo) / 2;
    if (v[mid] < line) lo = mid + 1; else hi = mid;
  }
  return lo;
}

int LineMarks::toggle(u32 line)
{
  u32 i = lm_lower(lines_, line);
  if (i < lines_.size() && lines_[i] == line) { lines_.erase(i, 1); return 0; }
  if (lines_.insert(i, &line, 1) != E_OK) return E_NOMEM;
  return 1;
}

bool LineMarks::has(u32 line) const
{
  u32 i = lm_lower(lines_, line);
  return i < lines_.size() && lines_[i] == line;
}

u32 LineMarks::next(u32 line) const
{
  u32 i;
  if (lines_.empty()) return EPOS_NONE;
  i = line == 0xFFFFFFFFu ? lines_.size() : lm_lower(lines_, line + 1);
  return i < lines_.size() ? lines_[i] : lines_[0];
}

u32 LineMarks::prev(u32 line) const
{
  u32 i;
  if (lines_.empty()) return EPOS_NONE;
  i = lm_lower(lines_, line);
  return i > 0 ? lines_[i - 1] : lines_[lines_.size() - 1];
}

void LineMarks::clear() { lines_.clear(); }
u32 LineMarks::count() const { return lines_.size(); }
u32 LineMarks::at(u32 i) const { return i < lines_.size() ? lines_[i] : EPOS_NONE; }

static int cmp_u32(const void* a, const void* b)
{
  u32 x = *(const u32*)a, y = *(const u32*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

int LineMarks::set(const u32* lines, u32 n)
{
  Vec<u32> v;
  u32 i, k = 0;
  if (n && v.insert(0, lines, n) != E_OK) return E_NOMEM;
  if (n > 1) qsort(v.data(), n, sizeof(u32), cmp_u32);
  for (i = 0; i < n; i++) if (k == 0 || v[k - 1] != v[i]) v[k++] = v[i];
  v.resize(k);
  lines_.swap(v);
  return E_OK;
}

void LineMarks::doc_changed(Document* d, const EditEvent& e)
{
  u32 i, k, first, last, cnt;
  if (lines_.empty()) return;
  if (e.flags & (EV_RESET | EV_INDEXED)) {           // keep marks that still exist
    cnt = d->line_count();
    i = lm_lower(lines_, cnt);
    lines_.resize(i);
    return;
  }
  if (e.lines_removed == 0 && e.lines_added == 0) return;
  // Lines (L, L + removed] were joined into L; later lines shift. A pure insertion at the start of line L moves
  // that line's mark down with its text.
  first = e.line + 1;
  if (e.removed == 0 && e.lines_added > 0) {
    epos ls = d->line_start(e.line);
    if (ls < d->text_origin()) ls = d->text_origin();
    if (e.pos <= ls) first = e.line;
  }
  last = e.line + e.lines_removed;                   // marks in [first, last] collapse onto e.line
  for (i = 0; i < lines_.size(); i++) {
    u32 m = lines_[i];
    if (m < first) continue;
    if (m <= last && m > e.line) lines_[i] = e.line;
    else if (m > last || (m == e.line && first == e.line)) lines_[i] = m - e.lines_removed + e.lines_added;
  }
  for (i = 0, k = 0; i < lines_.size(); i++) if (k == 0 || lines_[k - 1] != lines_[i]) lines_[k++] = lines_[i];
  lines_.resize(k);
}

// ---- RangeMarks ----

RangeMarks::RangeMarks() : d_(0) {}
RangeMarks::~RangeMarks() { detach(); }
void RangeMarks::attach(Document* d) { detach(); d_ = d; if (d_) d_->add_listener(this); }
void RangeMarks::detach() { if (d_) d_->remove_listener(this); d_ = 0; }

static int cmp_range(const void* a, const void* b)
{
  const MarkRange* x = (const MarkRange*)a;
  const MarkRange* y = (const MarkRange*)b;
  if (x->pos != y->pos) return x->pos < y->pos ? -1 : 1;
  return x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
}

// Copies, sorts, drops empty and overlapping ranges, keeps at most 10,000.
int RangeMarks::set(const MarkRange* r, u32 n)
{
  Vec<MarkRange> v;
  u32 i, k = 0;
  if (n && v.insert(0, r, n) != E_OK) return E_NOMEM;
  if (n > 1) qsort(v.data(), n, sizeof(MarkRange), cmp_range);
  for (i = 0; i < n && k < RM_MAX; i++) {
    if (v[i].len == 0) continue;
    if (k > 0 && v[i].pos < v[k - 1].pos + v[k - 1].len) continue;
    v[k++] = v[i];
  }
  v.resize(k);
  r_.swap(v);
  return E_OK;
}

void RangeMarks::clear() { r_.clear(); }
u32 RangeMarks::count() const { return r_.size(); }

const MarkRange& RangeMarks::at(u32 i) const
{
  static MarkRange none = { 0, 0 };
  return i < r_.size() ? r_[i] : none;
}

u32 RangeMarks::first_ending_after(epos pos) const
{
  u32 lo = 0, hi = r_.size();
  while (lo < hi) {
    u32 mid = lo + (hi - lo) / 2;
    if (r_[mid].pos + r_[mid].len <= pos) lo = mid + 1; else hi = mid;
  }
  return lo;
}

void RangeMarks::doc_changed(Document* d, const EditEvent& e)
{
  u32 i, k;
  epos end = e.pos + e.removed;
  (void)d;
  if (r_.empty()) return;
  if (e.flags & (EV_RESET | EV_INDEXED)) {
    if (e.removed || e.inserted) r_.clear();         // new bytes: positions mean nothing
    return;
  }
  for (i = 0, k = 0; i < r_.size(); i++) {
    MarkRange m = r_[i];
    if (m.pos + m.len <= e.pos) { r_[k++] = m; continue; }
    if (m.pos >= end) { m.pos = m.pos - e.removed + e.inserted; r_[k++] = m; continue; }
    // overlaps the edit: dropped
  }
  r_.resize(k);
}
