// layout.h - line layout and long-line index (DESIGN 6.12). Owner after Wave 0: T06.
#ifndef PICA_LAYOUT_H
#define PICA_LAYOUT_H

#include "etypes.h"
#include "emem.h"
#include "esource.h"
#include "codec.h"

enum { LAY_LONG = 65536, LAY_CHECKPOINT = 4096, LAY_WINDOW = 65536 };
enum LayKind { LK_CHAR = 0, LK_TAB, LK_CTRL, LK_BAD, LK_WIDE, LK_ZERO, LK_SPACE, LK_NODRAW };
struct LayoutOpts { u8 tab_width; u8 show_ws; u8 pad[2]; };
struct LayChar { u32 off; u32 cell; u16 glyph; u8 width; u8 kind; u8 nglyph; u8 pad[3]; };
// off: byte offset relative to the segment start; cell: absolute display cell; glyph: index of the first
// glyph (u16, UTF-16 code unit) in glyphs(); nglyph: glyphs drawn (control picture "NUL" = 3)
class LineLayout {
public:
  LineLayout();  ~LineLayout();
  // Lay out bytes [p, p + n) of one line (EOL excluded) starting at display cell start_cell.
  int  build(const u8* p, u32 n, const Codec* c, const LayoutOpts& o, u32 start_cell, int eof);
  u32  count() const;  const LayChar* chars() const;
  const u16* glyphs() const;  u32 glyph_count() const;
  u32  end_cell() const;
  u32  char_at_offset(u32 off) const;            // char containing byte off; count() if off >= bytes laid out
  u32  cell_of_offset(u32 off) const;            // caret x
  u32  offset_of_cell(u32 cell, int nearest) const;  // hit test: nearest char boundary (or floor)
  int  wrap_rows(u32 width_cells, int word, Vec<u32>* row_starts) const;  // char indices where rows start (first 0)
private:                                         // private state: T06 may change freely
  Vec<LayChar> chars_; Vec<u16> glyphs_; u32 start_cell_; u32 end_cell_; u32 nbytes_;
  LineLayout(const LineLayout&); LineLayout& operator=(const LineLayout&);
};
class LongLineIndex {                            // checkpoints every LAY_CHECKPOINT bytes for lines > LAY_LONG
public:
  LongLineIndex();  ~LongLineIndex();
  int  build(ByteSource* s, const Codec* c, const LayoutOpts& o, epos start, epos end, ProgressCb* cb);
  int  at_offset(epos pos, epos* cp_pos, u32* cp_cell) const;   // checkpoint at or before pos
  int  at_cell(u32 cell, epos* cp_pos, u32* cp_cell) const;
  u32  total_cells() const;  epos start() const;  epos end() const;  void clear();
private:                                         // private state: T06 may change freely
  Vec<epos> cp_pos_; Vec<u32> cp_cell_; u32 total_; epos start_; epos end_;
  LongLineIndex(const LongLineIndex&); LongLineIndex& operator=(const LongLineIndex&);
};
const char* lay_ctrl_name(u8 c);                 // "NUL", "SOH", ... "US", "DEL"; "CR" for a lone CR

#endif
