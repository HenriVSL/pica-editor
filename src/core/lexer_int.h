// lexer_int.h - private declarations shared by lexer.cpp, lexgen.cpp, lexhtml.cpp and lexmd.cpp. Owner: T04.
// Nothing outside the lexer module includes this header.
//
// State words: every lexer returns states below LEX_STATE_LIMIT (15 bits); the Highlighter uses bit 15 as a flag and
// 0x7FFF as "unknown". Generic (lexgen) states use only the low 10 bits (GEN_STATE_BITS) so the HTML lexer can keep
// an embedded JS/CSS state in the low bits and its own mode in bits 10-14.
#ifndef PICA_LEXER_INT_H
#define PICA_LEXER_INT_H

#include "lexer.h"

enum {
  LEX_STATE_LIMIT = 0x7FFF,          // lexers return states < this
  GEN_STATE_BITS = 10,
  GEN_STATE_MASK = 0x3FF,
  GEN_TOK_MAX = 8,                   // max chars of a comment/string/line-rule token
  GEN_MAX_LC = 4,                    // line comments
  GEN_MAX_BC = 2,                    // block comment pairs
  GEN_MAX_STR = 4,                   // string kinds
  GEN_MAX_LINE = 16,                 // column-0 whole-line rules (diff)
  GEN_MAX_CWORD = 4,                 // comment words (BAT rem)
  KW_MAX_LEN = 47                    // longer identifiers are never keywords
};

enum LexKind { LK_GENERIC = 0, LK_HTML = 1, LK_XML = 2, LK_MD = 3, LK_RESULTS = 4 };
enum GenNumber { GN_NONE = 0, GN_C = 1, GN_BASIC = 2, GN_PASCAL = 3 };
enum GenVars { GV_NONE = 0, GV_MAKE = 1, GV_BAT = 2, GV_PERCENT = 3 };
enum GenLabel { GL_NONE = 0, GL_COLON_PREFIX = 1, GL_COLON_SUFFIX = 2, GL_ASM = 3 };

// keyword table: ASCII words -> class 1..4 (ST_KEYWORD1..4); open addressing over a byte pool
class KwTable {
public:
  KwTable();  ~KwTable();
  void set_fold(bool fold) { fold_ = fold; }
  int  add(const char* w, u32 n, u8 cls);          // E_OK / E_NOMEM / E_RANGE (too long); a repeated word keeps the first class
  u8   find(const ecp* s, u32 n) const;             // 0 = not a keyword, else the first class the word was added with
  u8   find_mask(const ecp* s, u32 n) const;        // bit (cls - 1) set for every class the word was added with
  u8   find_ascii(const char* s, u32 n, u8* mask) const;
  u32  count() const { return count_; }
  void clear();
private:
  Vec<u8> pool_;  Vec<u32> slots_;  u32 count_;  bool fold_;
  int  rehash_(u32 nslots);
  KwTable(const KwTable&); KwTable& operator=(const KwTable&);
};

struct GenTok { ecp t[GEN_TOK_MAX]; u8 len; u8 start_only; u8 pad[2]; };
struct GenStr { GenTok open; GenTok close; ecp esc; u8 style; u8 multiline; u8 pad[2]; };
struct GenLine { GenTok prefix; u8 style; u8 pad[3]; };

// character class bits for ASCII
enum { CC_IDS = 1, CC_IDP = 2, CC_OP = 4, CC_SPECIAL = 8 };

class GenLexer : public Lexer {
public:
  GenLexer();  ~GenLexer();
  u16 lex_line(const ecp* s, u32 n, u16 state, u8* styles) const;
  // line_start = 0 when s is the middle of a line (HTML <script> bodies): no line-start rules
  u16 lex_range(const ecp* s, u32 n, u16 state, u8* styles, int line_start) const;
  void finish();                       // after parsing: char classes, sort strings

  // definition (filled by the parser)
  u8 cls[128];
  GenTok lc[GEN_MAX_LC];  u32 nlc;
  GenTok bo[GEN_MAX_BC];  GenTok bc[GEN_MAX_BC];  u32 nbc;  u8 nested;
  GenStr str[GEN_MAX_STR];  u32 nstr;
  GenLine line[GEN_MAX_LINE];  u32 nline;
  char cword[GEN_MAX_CWORD][16];  u32 ncword;
  ecp  pp_char, deco_char;
  u8   number, vars, label, regex, ini_section, ini_key, make_rules, lc_cont, case_sens;
  KwTable kw;                          // keywords1..4
  KwTable prefixes;                    // string prefixes (r, b, L, u8 ...)
private:
  int  match_tok(const ecp* s, u32 n, u32 i, const GenTok& t) const;
  u32  scan_block(const ecp* s, u32 n, u32 i, u32 idx, u32* depth, u8* st) const;   // returns end index; depth 0 = closed
  u32  scan_string(const ecp* s, u32 n, u32 i, u32 idx, u8* st, int* open_state) const;
  u32  scan_number(const ecp* s, u32 n, u32 i) const;
  u32  scan_regex(const ecp* s, u32 n, u32 i) const;      // 0 = not a regex literal
  u32  line_rules(const ecp* s, u32 n, u8* st, int* pp, int* done) const;
  u32  make_rule(const ecp* s, u32 n, u8* st) const;
  GenLexer(const GenLexer&); GenLexer& operator=(const GenLexer&);
};

class HtmlLexer : public Lexer {
public:
  HtmlLexer(int xml, GenLexer* meta);  ~HtmlLexer();   // takes ownership of meta (keyword lists: 1 tags, 2 attributes)
  u16 lex_line(const ecp* s, u32 n, u16 state, u8* styles) const;
  void set_sub(const GenLexer* js, const GenLexer* css) { js_ = js; css_ = css; }
private:
  int xml_;  GenLexer* meta_;  const GenLexer* js_;  const GenLexer* css_;
  u32 tag_name(const ecp* s, u32 n, u32 i) const;
  u8  tag_style(const ecp* s, u32 n) const;
  u8  attr_style(const ecp* s, u32 n) const;
  HtmlLexer(const HtmlLexer&); HtmlLexer& operator=(const HtmlLexer&);
};

class MdLexer : public Lexer {
public:
  MdLexer() {}
  u16 lex_line(const ecp* s, u32 n, u16 state, u8* styles) const;
private:
  void inl(const ecp* s, u32 n, u32 i, u8 base, u8* st, u16* state) const;
};

// parser (lexgen.cpp). kind/sub ids are filled from the "lexer", "script", "style" keys.
struct LngExtra { u32 kind; char script[16]; char style[16]; };
int  lng_parse(const char* text, u32 len, LangInfo* out, LngExtra* ex, Lexer** lex, EStr* err);
const char* lng_style_name(u32 style);           // "comment" etc.
int  lng_style_from_name(const char* s);          // -1 if unknown

// helpers shared by the lexers
inline void lex_fill(u8* st, u32 from, u32 to, u8 style) { if (st) { u32 k; for (k = from; k < to; k++) st[k] = style; } }
inline ecp lex_lower(ecp c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
inline int lex_is_space(ecp c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r'; }
inline int lex_is_alpha(ecp c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline int lex_is_digit(ecp c) { return c >= '0' && c <= '9'; }
inline int lex_is_alnum(ecp c) { return lex_is_alpha(c) || lex_is_digit(c); }
int lex_word_char(ecp c);                          // ASCII alnum/_ or a Unicode word character

#endif
