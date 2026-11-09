// marks.cpp - LineMarks (bookmarks) and RangeMarks (DESIGN 6.8). STUB from T00. Owner after Wave 0: T01.
#include "marks.h"

LineMarks::LineMarks() : d_(0) {}
LineMarks::~LineMarks() { detach(); }
void LineMarks::attach(Document* d) { detach(); d_ = d; if (d_) d_->add_listener(this); }
void LineMarks::detach() { if (d_) d_->remove_listener(this); d_ = 0; }
int LineMarks::toggle(u32 line) { (void)line; return E_NOTIMPL; }
bool LineMarks::has(u32 line) const { (void)line; return false; }
u32 LineMarks::next(u32 line) const { (void)line; return EPOS_NONE; }
u32 LineMarks::prev(u32 line) const { (void)line; return EPOS_NONE; }
void LineMarks::clear() { lines_.clear(); }
u32 LineMarks::count() const { return lines_.size(); }
u32 LineMarks::at(u32 i) const { return i < lines_.size() ? lines_[i] : EPOS_NONE; }
int LineMarks::set(const u32* lines, u32 n) { (void)lines; (void)n; return E_NOTIMPL; }
void LineMarks::doc_changed(Document* d, const EditEvent& e) { (void)d; (void)e; }

RangeMarks::RangeMarks() : d_(0) {}
RangeMarks::~RangeMarks() { detach(); }
void RangeMarks::attach(Document* d) { detach(); d_ = d; if (d_) d_->add_listener(this); }
void RangeMarks::detach() { if (d_) d_->remove_listener(this); d_ = 0; }
int RangeMarks::set(const MarkRange* r, u32 n) { (void)r; (void)n; return E_NOTIMPL; }
void RangeMarks::clear() { r_.clear(); }
u32 RangeMarks::count() const { return r_.size(); }
const MarkRange& RangeMarks::at(u32 i) const
{
  static MarkRange none = { 0, 0 };
  return i < r_.size() ? r_[i] : none;
}
u32 RangeMarks::first_ending_after(epos pos) const { (void)pos; return r_.size(); }
void RangeMarks::doc_changed(Document* d, const EditEvent& e) { (void)d; (void)e; }
