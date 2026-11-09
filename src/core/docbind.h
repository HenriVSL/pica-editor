// docbind.h - feeds a Highlighter from a Document (DESIGN 6.8). Owner after Wave 0: T01.
#ifndef PICA_DOCBIND_H
#define PICA_DOCBIND_H

#include "doc.h"
#include "lexer.h"

class DocLines : public LineProvider, public DocListener {
public:
  DocLines();  ~DocLines();
  void bind(Document* d, Highlighter* h);          // registers as listener; h may be NULL
  void unbind();
  u32  lp_count();
  int  lp_get(u32 line, ecp* out, u32 cap, u32* n); // decodes up to cap chars of the line (EOL excluded)
  void doc_changed(Document* d, const EditEvent& e);// -> h->lines_changed / lines_reset
private:                                           // private state: T01 may change freely
  Document* d_; Highlighter* h_;
  DocLines(const DocLines&); DocLines& operator=(const DocLines&);
};

#endif
