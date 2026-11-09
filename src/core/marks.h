// marks.h - bookmarks and range marks (DESIGN 6.8). Owner after Wave 0: T01.
// T00 addition: constructors/destructors (the classes hold a Document pointer and a Vec).
#ifndef PICA_MARKS_H
#define PICA_MARKS_H

#include "doc.h"

class LineMarks : public DocListener {             // bookmarks: sorted unique line numbers
public:
  LineMarks();  ~LineMarks();
  void attach(Document* d);  void detach();
  int  toggle(u32 line);  bool has(u32 line) const;
  u32  next(u32 line) const;  u32 prev(u32 line) const;   // wrap around; EPOS_NONE if empty
  void clear();  u32 count() const;  u32 at(u32 i) const;  int set(const u32* lines, u32 n);
  void doc_changed(Document* d, const EditEvent& e);      // lines inside a removed range move to its first line (deduped)
private:                                           // private state: T01 may change freely
  Document* d_; Vec<u32> lines_;
  LineMarks(const LineMarks&); LineMarks& operator=(const LineMarks&);
};
class RangeMarks : public DocListener {            // Mark All / result highlights: sorted, non-overlapping, max 10,000
public:
  RangeMarks();  ~RangeMarks();
  void attach(Document* d);  void detach();
  int  set(const MarkRange* r, u32 n);  void clear();  u32 count() const;
  const MarkRange& at(u32 i) const;
  u32  first_ending_after(epos pos) const;          // binary search for painting
  void doc_changed(Document* d, const EditEvent& e);// shift after edit; drop ranges overlapping the edit
private:                                           // private state: T01 may change freely
  Document* d_; Vec<MarkRange> r_;
  RangeMarks(const RangeMarks&); RangeMarks& operator=(const RangeMarks&);
};

#endif
