// lexer.h - styles, lexers, language registry, highlighter (DESIGN 6.13). Owner after Wave 0: T04.
#ifndef PICA_LEXER_H
#define PICA_LEXER_H

#include "etypes.h"
#include "emem.h"
#include "estr.h"

enum StyleId {
  ST_DEFAULT = 0, ST_COMMENT, ST_STRING, ST_CHAR, ST_NUMBER, ST_KEYWORD1, ST_KEYWORD2, ST_KEYWORD3, ST_KEYWORD4,
  ST_PREPROC, ST_OPERATOR, ST_LABEL, ST_VARIABLE, ST_SECTION, ST_KEY, ST_TAG, ST_ATTR, ST_ENTITY, ST_HEADING,
  ST_EMPH, ST_CODE, ST_DIFF_ADD, ST_DIFF_DEL, ST_DIFF_HUNK, ST_DIFF_HEADER, ST_INVALID, ST_COUNT
};
enum { LEX_LINE_MAX = 16384 };          // lexers see at most this many chars of a line; the rest is ST_DEFAULT
class Lexer {
public:
  virtual ~Lexer() {}
  virtual u16 lex_line(const ecp* s, u32 n, u16 state, u8* styles) const = 0;   // styles may be NULL; returns end state
};
struct LangInfo {
  char id[16]; char name[32];
  char exts[160];              // "c;cpp;cc;cxx;h;hpp;inl"
  char names[96];              // whole basenames: "makefile;makefile.vc"
  ecp  line_comment[8];  u8 line_comment_len;
  ecp  block_open[4];    u8 block_open_len;
  ecp  block_close[4];   u8 block_close_len;
  u8   tab_width;              // 0 = use the global option
  u8   use_tabs;               // 0 spaces, 1 tabs, 2 global option
  u8   case_sensitive;
  char braces[8];              // "()[]{}"
  const Lexer* lexer;          // owned by the registry
};
class LangSource { public: virtual ~LangSource() {} virtual u32 count() = 0;
                   virtual int get(u32 i, const char** name, const char** text, u32* len) = 0; };
int  lang_init(LangSource* builtin, LangSource* overrides, EStr* errors);  // overrides may be NULL; same file name replaces
void lang_shutdown();
u32  lang_count();  const LangInfo* lang_at(u32 i);  const LangInfo* lang_find(const char* id);
const LangInfo* lang_for_file(const char* basename);   // NULL = plain text
const LangInfo* lang_results();                         // find-results panel lexer (not in menus)
int  lang_parse(const char* text, u32 len, LangInfo* out, EStr* err);   // test entry point (generic languages)

class LineProvider {
public:
  virtual ~LineProvider() {}
  virtual u32 lp_count() = 0;
  virtual int lp_get(u32 line, ecp* out, u32 cap, u32* n) = 0;
};
class Highlighter {
public:
  Highlighter();  ~Highlighter();
  int  attach(LineProvider* lines, const LangInfo* lang);  void detach();
  void set_enabled(bool on);  bool enabled() const;
  void lines_changed(u32 first, u32 removed, u32 added);   // after every edit
  void lines_reset();
  void set_target(u32 line);           // idle work lexes at least to here
  int  work(u32 budget_lines);         // returns 1 while more work remains
  bool state_known(u32 line) const;
  int  style_line(u32 line, const ecp* s, u32 n, u8* styles);   // 1 styled; 0 start state unknown (styles = default)
  int  take_dirty(u32* first, u32* last);   // lines whose styling changed (to repaint); 0 if none
  u32  valid_upto() const;
private:                               // private state: T04 may change freely
  LineProvider* lines_; const LangInfo* lang_; bool enabled_; u32 valid_upto_;
  Highlighter(const Highlighter&); Highlighter& operator=(const Highlighter&);
};

#endif
