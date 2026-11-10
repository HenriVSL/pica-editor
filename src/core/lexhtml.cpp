// lexhtml.cpp - hand-written HTML/XML lexer (DESIGN 6.13, ARCH 7.2). Owner: T04.
//
// State word: bits 10-14 = mode (below), bits 0-9 = the embedded JS/CSS generic state in H_SCRIPT / H_STYLE.
// Tag modes carry the tag kind (0 other or end tag, 1 <script>, 2 <style>) so the closing '>' knows what follows.
// Styles: <tag ...> TAG (unknown HTML element names KEYWORD4 when html.lng lists the known ones), attributes ATTR
// (unknown ones KEYWORD4), '=' OPERATOR, values STRING, &entities; ENTITY, comments COMMENT, <!DOCTYPE>/<? ?> PREPROC,
// CDATA sections CODE.
#include "lexer_int.h"

enum {
  H_TEXT = 0, H_COMMENT = 1, H_CDATA = 2, H_DECL = 3, H_PI = 4, H_TAG = 5, H_DQ = 8, H_SQ = 11, H_SCRIPT = 14,
  H_STYLE = 15
};

static u16 hpack(u32 mode, u32 low) { return (u16)((mode << GEN_STATE_BITS) | (low & GEN_STATE_MASK)); }

static int name_start(ecp c)
{
  if (c < 128) return lex_is_alpha(c) || c == '_' || c == ':';
  return lex_word_char(c);
}
static int name_char(ecp c)
{
  if (c < 128) return lex_is_alnum(c) || c == '_' || c == ':' || c == '-' || c == '.';
  return lex_word_char(c);
}

static int match_ascii(const ecp* s, u32 n, u32 i, const char* t, int fold)
{
  u32 k;
  for (k = 0; t[k]; k++) {
    ecp c;
    if (i + k >= n) return 0;
    c = s[i + k];
    if (fold) c = lex_lower(c);
    if (c != (u8)t[k]) return 0;
  }
  return 1;
}

static u32 find_ascii(const ecp* s, u32 n, u32 i, const char* t, int fold)   // n if not found
{
  ecp first = (u8)t[0];
  for (; i < n; i++) {
    ecp c = fold ? lex_lower(s[i]) : s[i];
    if (c == first && match_ascii(s, n, i, t, fold)) return i;
  }
  return n;
}

static u32 html_entity(const ecp* s, u32 n, u32 i)          // s[i] == '&'; returns the end after ';' or 0
{
  u32 j = i + 1, lim = emin(n, i + 34), d;
  if (j < lim && s[j] == '#') {
    j++;
    if (j < lim && (s[j] == 'x' || s[j] == 'X')) {
      j++;
      d = j;
      while (j < lim && (lex_is_digit(s[j]) || (lex_lower(s[j]) >= 'a' && lex_lower(s[j]) <= 'f'))) j++;
    } else {
      d = j;
      while (j < lim && lex_is_digit(s[j])) j++;
    }
    if (j == d) return 0;
  } else {
    if (j >= lim || !lex_is_alpha(s[j])) return 0;
    while (j < lim && lex_is_alnum(s[j])) j++;
  }
  return (j < lim && s[j] == ';') ? j + 1 : 0;
}

HtmlLexer::HtmlLexer(int xml, GenLexer* meta) : xml_(xml), meta_(meta), js_(0), css_(0) {}
HtmlLexer::~HtmlLexer() { delete meta_; }

u32 HtmlLexer::tag_name(const ecp* s, u32 n, u32 i) const
{
  while (i < n && name_char(s[i])) i++;
  return i;
}

u8 HtmlLexer::tag_style(const ecp* s, u32 n) const
{
  if (xml_ || !meta_ || !meta_->kw.count()) return ST_TAG;
  return (meta_->kw.find_mask(s, n) & 1) ? (u8)ST_TAG : (u8)ST_KEYWORD4;
}

u8 HtmlLexer::attr_style(const ecp* s, u32 n) const
{
  u32 k;
  if (xml_ || !meta_ || !meta_->kw.count()) return ST_ATTR;
  if (meta_->kw.find_mask(s, n) & 2) return ST_ATTR;
  if (n > 5 && (match_ascii(s, n, 0, "data-", 1) || match_ascii(s, n, 0, "aria-", 1))) return ST_ATTR;
  for (k = 0; k < n; k++) if (s[k] == ':') return ST_ATTR;       // xmlns:x, xml:lang, v:shape
  return ST_KEYWORD4;
}

// a quoted attribute value from i (just after the opening quote); returns the end and whether it closed
static u32 quoted(const ecp* s, u32 n, u32 i, ecp q, u8* st, int* closed)
{
  *closed = 0;
  while (i < n) {
    if (s[i] == q) { if (st) st[i] = ST_STRING; *closed = 1; return i + 1; }
    if (s[i] == '&') {
      u32 e = html_entity(s, n, i);
      if (e) { lex_fill(st, i, e, ST_ENTITY); i = e; continue; }
    }
    if (st) st[i] = ST_STRING;
    i++;
  }
  return n;
}

u16 HtmlLexer::lex_line(const ecp* s, u32 n, u16 state, u8* st) const
{
  u32 mode = (u32)state >> GEN_STATE_BITS, low = state & GEN_STATE_MASK;
  u32 i = 0, e, kind;
  int closed;

  if (n > LEX_LINE_MAX) { lex_fill(st, LEX_LINE_MAX, n, ST_DEFAULT); n = LEX_LINE_MAX; }
  if (mode > H_STYLE) mode = H_TEXT;
  while (i < n) {
    switch (mode) {
    case H_TEXT: {
      ecp c = s[i];
      if (c == '<') {
        if (match_ascii(s, n, i, "<!--", 0)) { lex_fill(st, i, i + 4, ST_COMMENT); i += 4; mode = H_COMMENT; break; }
        if (match_ascii(s, n, i, "<![CDATA[", 0)) { lex_fill(st, i, i + 9, ST_CODE); i += 9; mode = H_CDATA; break; }
        if (i + 1 < n && s[i + 1] == '!') { lex_fill(st, i, i + 2, ST_PREPROC); i += 2; mode = H_DECL; break; }
        if (i + 1 < n && s[i + 1] == '?') { lex_fill(st, i, i + 2, ST_PREPROC); i += 2; mode = H_PI; break; }
        if (i + 2 < n && s[i + 1] == '/' && name_start(s[i + 2])) {
          e = tag_name(s, n, i + 2);
          lex_fill(st, i, i + 2, ST_TAG);
          lex_fill(st, i + 2, e, tag_style(s + i + 2, e - i - 2));
          i = e; mode = H_TAG;
          break;
        }
        if (i + 1 < n && name_start(s[i + 1])) {
          e = tag_name(s, n, i + 1);
          kind = 0;
          if (!xml_ && e - i - 1 == 6 && match_ascii(s, e, i + 1, "script", 1)) kind = 1;
          else if (!xml_ && e - i - 1 == 5 && match_ascii(s, e, i + 1, "style", 1)) kind = 2;
          if (st) st[i] = ST_TAG;
          lex_fill(st, i + 1, e, tag_style(s + i + 1, e - i - 1));
          i = e; mode = H_TAG + kind;
          break;
        }
        if (st) st[i] = ST_DEFAULT;
        i++;
        break;
      }
      if (c == '&') {
        e = html_entity(s, n, i);
        if (e) { lex_fill(st, i, e, ST_ENTITY); i = e; break; }
      }
      e = i + 1;
      while (e < n && s[e] != '<' && s[e] != '&') e++;
      lex_fill(st, i, e, ST_DEFAULT);
      i = e;
      break;
    }
    case H_COMMENT:
      e = find_ascii(s, n, i, "-->", 0);
      if (e < n) { lex_fill(st, i, e + 3, ST_COMMENT); i = e + 3; mode = H_TEXT; }
      else { lex_fill(st, i, n, ST_COMMENT); i = n; }
      break;
    case H_CDATA:
      e = find_ascii(s, n, i, "]]>", 0);
      if (e < n) { lex_fill(st, i, e + 3, ST_CODE); i = e + 3; mode = H_TEXT; }
      else { lex_fill(st, i, n, ST_CODE); i = n; }
      break;
    case H_DECL:
      e = find_ascii(s, n, i, ">", 0);
      if (e < n) { lex_fill(st, i, e + 1, ST_PREPROC); i = e + 1; mode = H_TEXT; }
      else { lex_fill(st, i, n, ST_PREPROC); i = n; }
      break;
    case H_PI:
      e = find_ascii(s, n, i, "?>", 0);
      if (e < n) { lex_fill(st, i, e + 2, ST_PREPROC); i = e + 2; mode = H_TEXT; }
      else { lex_fill(st, i, n, ST_PREPROC); i = n; }
      break;
    case H_TAG: case H_TAG + 1: case H_TAG + 2: {
      ecp c = s[i];
      kind = mode - H_TAG;
      if (lex_is_space(c)) { if (st) st[i] = ST_DEFAULT; i++; break; }
      if (c == '>') {
        if (st) st[i] = ST_TAG;
        i++;
        mode = kind == 1 ? (u32)H_SCRIPT : kind == 2 ? (u32)H_STYLE : (u32)H_TEXT;
        low = 0;
        break;
      }
      if (c == '/' && i + 1 < n && s[i + 1] == '>') { lex_fill(st, i, i + 2, ST_TAG); i += 2; mode = H_TEXT; break; }
      if (c == '?' && xml_ && i + 1 < n && s[i + 1] == '>') { lex_fill(st, i, i + 2, ST_TAG); i += 2; mode = H_TEXT; break; }
      if (c == '<') { mode = H_TEXT; break; }                 // broken tag: recover at the next '<'
      if (c == '=') {
        if (st) st[i] = ST_OPERATOR;
        i++;
        while (i < n && lex_is_space(s[i])) { if (st) st[i] = ST_DEFAULT; i++; }
        if (i < n && s[i] != '"' && s[i] != '\'' && s[i] != '>' && s[i] != '<') {
          e = i;
          while (e < n && !lex_is_space(s[e]) && s[e] != '>' && !(s[e] == '/' && e + 1 < n && s[e + 1] == '>')) e++;
          if (e == i) e = i + 1;
          lex_fill(st, i, e, ST_STRING);
          i = e;
        }
        break;
      }
      if (c == '"' || c == '\'') {
        if (st) st[i] = ST_STRING;
        e = quoted(s, n, i + 1, c, st, &closed);
        if (!closed) mode = (c == '"' ? H_DQ : H_SQ) + kind;
        i = e;
        break;
      }
      e = i;
      while (e < n && !lex_is_space(s[e]) && s[e] != '=' && s[e] != '>' && s[e] != '"' && s[e] != '\'' &&
             s[e] != '<' && !(s[e] == '/' && e + 1 < n && s[e + 1] == '>')) e++;
      if (e == i) e = i + 1;                                  // a lone '/' or '?'
      lex_fill(st, i, e, attr_style(s + i, e - i));
      i = e;
      break;
    }
    case H_DQ: case H_DQ + 1: case H_DQ + 2:
    case H_SQ: case H_SQ + 1: case H_SQ + 2: {
      int dq = mode < H_SQ;
      kind = mode - (dq ? H_DQ : H_SQ);
      e = quoted(s, n, i, dq ? '"' : '\'', st, &closed);
      if (closed) mode = H_TAG + kind;
      i = e;
      break;
    }
    case H_SCRIPT: case H_STYLE: {
      const GenLexer* sub = mode == H_SCRIPT ? js_ : css_;
      e = find_ascii(s, n, i, mode == H_SCRIPT ? "</script" : "</style", 1);
      if (sub) low = sub->lex_range(s + i, e - i, (u16)low, st ? st + i : 0, i == 0) & GEN_STATE_MASK;
      else lex_fill(st, i, e, ST_DEFAULT);
      if (e < n) { mode = H_TEXT; low = 0; }
      i = e;
      break;
    }
    default:
      mode = H_TEXT;
      break;
    }
  }
  return hpack(mode, (mode == H_SCRIPT || mode == H_STYLE) ? low : 0);
}
