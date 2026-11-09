// layout.cpp - LineLayout, LongLineIndex (DESIGN 6.12, 8.1, 14.2). STUB from T00. Owner after Wave 0: T06.
#include "layout.h"

LineLayout::LineLayout() : start_cell_(0), end_cell_(0), nbytes_(0) {}
LineLayout::~LineLayout() {}
int LineLayout::build(const u8* p, u32 n, const Codec* c, const LayoutOpts& o, u32 start_cell, int eof)
{ (void)p; (void)n; (void)c; (void)o; (void)eof; start_cell_ = end_cell_ = start_cell; return E_NOTIMPL; }
u32 LineLayout::count() const { return chars_.size(); }
const LayChar* LineLayout::chars() const { return chars_.data(); }
const u16* LineLayout::glyphs() const { return glyphs_.data(); }
u32 LineLayout::glyph_count() const { return glyphs_.size(); }
u32 LineLayout::end_cell() const { return end_cell_; }
u32 LineLayout::char_at_offset(u32 off) const { (void)off; return chars_.size(); }
u32 LineLayout::cell_of_offset(u32 off) const { (void)off; return start_cell_; }
u32 LineLayout::offset_of_cell(u32 cell, int nearest) const { (void)cell; (void)nearest; return 0; }
int LineLayout::wrap_rows(u32 width_cells, int word, Vec<u32>* row_starts) const
{ (void)width_cells; (void)word; (void)row_starts; return E_NOTIMPL; }

LongLineIndex::LongLineIndex() : total_(0), start_(0), end_(0) {}
LongLineIndex::~LongLineIndex() {}
int LongLineIndex::build(ByteSource* s, const Codec* c, const LayoutOpts& o, epos start, epos end, ProgressCb* cb)
{ (void)s; (void)c; (void)o; (void)start; (void)end; (void)cb; return E_NOTIMPL; }
int LongLineIndex::at_offset(epos pos, epos* cp_pos, u32* cp_cell) const { (void)pos; (void)cp_pos; (void)cp_cell; return E_NOTIMPL; }
int LongLineIndex::at_cell(u32 cell, epos* cp_pos, u32* cp_cell) const { (void)cell; (void)cp_pos; (void)cp_cell; return E_NOTIMPL; }
u32 LongLineIndex::total_cells() const { return total_; }
epos LongLineIndex::start() const { return start_; }
epos LongLineIndex::end() const { return end_; }
void LongLineIndex::clear() { cp_pos_.clear(); cp_cell_.clear(); total_ = 0; start_ = end_ = 0; }

const char* lay_ctrl_name(u8 c) { (void)c; return "?"; }
